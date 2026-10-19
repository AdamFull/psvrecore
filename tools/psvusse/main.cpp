// psvusse — disassembles the programs of GXP files (compiled Vita shaders) in the reference disassembler's syntax:
// "Secondary program:" and "Primary program:" sections of "N: text" lines (scripts/usse_check.py compares them).
// With --params it prints the program's properties and parameters instead, one "key=value" line each
// (scripts/gxp_check.py compares them with libgxm's). With --run it runs them in the reference interpreter; with
// --spirv it writes the primary program's translation (the compute form, gxm/translate.hpp); with --run and --gpu
// the translation runs on a Vulkan device in place of the interpreter's primary program.

#include <bit>
#include <cmath>
#include <cstdio>

#include "gpu.hpp"
#include "psvrecore/cli.hpp"
#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/usse/format.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "psvrecore/usse/interp.hpp"
#include "psvrecore/text.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

nx::string params(const usse::Gxp& g) {
    nx::string out = nx::format(
        "program type={} size={} discard={} depth_replace={} sprite_coord={} native_color={} frag_color={} "
        "default_uniform_buffer={} vertex_outputs={} fragment_inputs={}",
        static_cast<u32>(g.type), g.size, g.discard ? 1 : 0, g.depth_replace ? 1 : 0, g.sprite_coord ? 1 : 0,
        g.native_color ? 1 : 0, g.frag_color ? 1 : 0, g.default_uniform_buffer_size, g.vertex_outputs,
        g.fragment_inputs);
    if (g.type == usse::ShaderType::Fragment)
        out += nx::format(" output={},{}", static_cast<u32>(g.output_type), static_cast<u32>(g.output_components));
    out += '\n';
    for (usize i = 0; i < g.inputs.size(); ++i) {
        const usse::FragmentInput& in = g.inputs[i];
        out += nx::format(
            "input {} interpolated={} sampled={} format={} components={} offset={} sampler={} sample_format={} "
            "sample_offset={} projected={} raw={:08x}\n",
            i, in.interpolated, in.sampled, static_cast<u32>(in.format), static_cast<u32>(in.components), in.offset,
            static_cast<u32>(in.sampler), static_cast<u32>(in.sample_format), in.sample_offset, in.projected ? 1 : 0,
            in.raw[0]);
    }
    for (usize i = 0; i < g.parameters.size(); ++i) {
        const usse::Parameter& p = g.parameters[i];
        out += nx::format(
            "param {} offset={} category={} name={} semantic={} semantic_index={} type={} components={} array={} "
            "resource={} container={} cube={} reg_format={}\n",
            i, p.offset, static_cast<u32>(p.category), p.named ? p.name : nx::string("-"),
            static_cast<u32>(p.semantic), static_cast<u32>(p.semantic_index), static_cast<u32>(p.type),
            static_cast<u32>(p.components), p.array_size, p.resource, static_cast<u32>(p.container),
            p.sampler_cube ? 1 : 0, p.reg_format ? 1 : 0);
    }
    return out;
}

// Guest memory for --run: the words the input file gives, at their addresses.
class MapMemory : public usse::ShaderMemory {
public:
    nx::hash_map<u32, u8> bytes;
    bool read(u32 address, void* out, u32 size) override {
        for (u32 i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            static_cast<u8*>(out)[i] = it->second;
        }
        return true;
    }
    bool write(u32 address, const void* in, u32 size) override {
        for (u32 i = 0; i < size; ++i) bytes[address + i] = static_cast<const u8*>(in)[i];
        return true;
    }
};

// Textures for --run: RGBA8 images by unit, sampled at the nearest texel with wrapping; a sampler's state words
// are (unit, 0, 0, 0). Cube maps pick the face of the largest coordinate (+x, -x, +y, -y, +z, -z: faces 0-5,
// stacked vertically) and sample it at the coordinates divided by that one.
class Textures : public usse::ShaderTextures {
public:
    struct Image {
        u32 width = 0, height = 0;
        nx::vector<u32> texels;
    };
    nx::hash_map<u32, Image> images;

    bool sample(const usse::SamplerState& state, const usse::TextureRequest& r, f32 rgba[4]) override {
        const auto it = images.find(state.words[0]);
        if (it == images.end()) return false;
        const Image& im = it->second;
        f32 u = r.coord[0], v = r.dim == usse::TexDim::D1 ? 0.5f : r.coord[1];
        u32 face = 0;
        if (r.dim == usse::TexDim::Cube) {
            const f32 ax = std::fabs(r.coord[0]), ay = std::fabs(r.coord[1]), az = std::fabs(r.coord[2]);
            f32 m;
            if (ax >= ay && ax >= az) face = r.coord[0] >= 0 ? 0 : 1, m = ax, u = r.coord[2], v = r.coord[1];
            else if (ay >= az) face = r.coord[1] >= 0 ? 2 : 3, m = ay, u = r.coord[0], v = r.coord[2];
            else face = r.coord[2] >= 0 ? 4 : 5, m = az, u = r.coord[0], v = r.coord[1];
            u = (u / m + 1) * 0.5f, v = (v / m + 1) * 0.5f;
        }
        const u32 h = r.dim == usse::TexDim::Cube ? im.height / 6 : im.height;
        auto wrap = [](f32 c, u32 n) {
            const i64 i = static_cast<i64>(std::floor(c * static_cast<f32>(n)));
            return static_cast<u32>(((i % n) + n) % n);
        };
        const u32 texel = im.texels[(face * h + wrap(v, h)) * im.width + wrap(u, im.width)];
        for (u32 k = 0; k < 4; ++k) rgba[k] = static_cast<f32>((texel >> (8 * k)) & 0xFF) / 255.0f;
        return true;
    }
};

struct TraceState {
    usse::ProgramKind kind;
};

// --trace: each executed instruction and the four registers from its destination, as words and as f32.
void trace(void* user, const usse::Decoded& d, const usse::Machine& m) {
    const auto kind = static_cast<TraceState*>(user)->kind;
    nx::string line = nx::format("{:4}: {}", d.word, usse::format(d.in, kind));
    line += nx::format("   [banks {}", static_cast<u32>(d.in.dest.bank));
    for (u8 i = 0; i < d.in.src_count; ++i) line += nx::format(" {}", static_cast<u32>(d.in.src[i].bank));
    line += "]";
    const usse::Operand& o = d.in.dest;
    const u32* regs = nullptr;
    switch (o.bank) {
        case usse::Bank::Temp: regs = m.temp; break;
        case usse::Bank::Output: regs = m.output; break;
        case usse::Bank::Primary: regs = kind == usse::ProgramKind::Secondary ? m.secondary : m.primary; break;
        case usse::Bank::Secondary: regs = m.secondary; break;
        case usse::Bank::Internal: regs = o.number >= 0 && o.number < 4 ? m.internal[o.number] : nullptr; break;
        default: break;
    }
    if (regs) {
        const i32 first = o.bank == usse::Bank::Internal ? 0 : o.number;
        line += "   =";
        for (i32 i = first; i < first + 4 && i >= 0 && i < static_cast<i32>(usse::Machine::kBankSize); ++i)
            line += nx::format(" {:08x}({})", regs[i], std::bit_cast<f32>(regs[i]));
    } else if (o.bank == usse::Bank::Predicate) {
        line += nx::format("   = p{} {}", o.number, m.pred[o.number & 3] ? 1 : 0);
    }
    line += '\n';
    std::fputs(line.c_str(), stderr);
}

// --run: the input file's lines are "<what> <first> <hex word>...": pa, sa, r (registers from <first>), uniform
// (the default uniform buffer from word <first>), mem (guest memory from address <first>), buffer (<first> is the
// BUFFER index, one word: its address). The secondary attributes are set up as for a draw, the secondary program
// runs, then the primary one; the output registers o0-o63 are printed as hex words on one line ("out ..."), and
// "killed" if the instance was discarded. A fragment program's inputs are "varying <varying:: bit> <4 f32 words>"
// and its textures "texture <unit> <width> <height> <RGBA8 texels>"; it prints its primary attributes.
Result<nx::string> execute(const usse::Gxp& gxp, nx::string_view inputs, bool tracing,
                           const psvusse::GpuOptions* gpu) {
    usse::Machine m;
    MapMemory memory;
    Textures textures;
    m.memory = &memory;
    m.textures = &textures;
    nx::vector<usse::VaryingValue> varyings;
    nx::vector<u32> uniforms, buffers(15, 0);  // (BUFFER 0-13, and 14: the default uniform buffer)
    nx::string error;
    for_each_line(inputs, [&](nx::string_view line) {
        if (line.empty() || line[0] == '#' || !error.empty()) return;
        const auto f = split(line, ' ');
        if (f.size() < 2) return void(error = nx::format("bad input line '{}'", line));
        const auto first = parse_u64(f[1], 0);
        if (!first) return void(error = first.error().message);
        nx::vector<u32> words;
        for (usize i = 2; i < f.size(); ++i) {
            const auto v = parse_u64(f[i], 16);
            if (!v || *v > 0xFFFFFFFFu) return void(error = nx::format("bad word '{}'", f[i]));
            words.push_back(static_cast<u32>(*v));
        }
        u32* bank = f[0] == "pa" ? m.primary : f[0] == "sa" ? m.secondary : f[0] == "r" ? m.temp : nullptr;
        if (bank) {
            if (*first + words.size() > usse::Machine::kBankSize) return void(error = "registers outside the bank");
            for (usize i = 0; i < words.size(); ++i) bank[*first + i] = words[i];
        } else if (f[0] == "uniform") {
            if (uniforms.size() < *first + words.size()) uniforms.resize(*first + words.size(), 0);
            for (usize i = 0; i < words.size(); ++i) uniforms[*first + i] = words[i];
        } else if (f[0] == "mem") {
            for (usize i = 0; i < words.size(); ++i) memory.write(static_cast<u32>(*first + 4 * i), &words[i], 4);
        } else if (f[0] == "buffer" && *first < buffers.size() && words.size() == 1) {
            buffers[*first] = words[0];
        } else if (f[0] == "varying" && words.size() == 4) {
            usse::VaryingValue v;
            v.varying = static_cast<u32>(*first);
            for (u32 k = 0; k < 4; ++k) v.value[k] = std::bit_cast<f32>(words[k]);
            varyings.push_back(v);
        } else if (f[0] == "texture" && words.size() >= 2 && words.size() == 2 + usize{words[0]} * words[1]) {
            Textures::Image im;
            im.width = words[0], im.height = words[1];
            im.texels.assign(words.begin() + 2, words.end());
            textures.images[static_cast<u32>(*first)] = std::move(im);
        } else {
            error = nx::format("bad input line '{}'", line);
        }
    });
    if (!error.empty()) return failure(error);
    // The default uniform buffer is also in memory (programs read what does not fit the secondary attributes from
    // there), at a fixed address unless the inputs give one.
    constexpr u32 kDefaultBufferAddress = 0x00100000;
    if (!buffers[usse::kDefaultUniformContainer] && !uniforms.empty()) {
        buffers[usse::kDefaultUniformContainer] = kDefaultBufferAddress;
        for (usize i = 0; i < uniforms.size(); ++i)
            memory.write(kDefaultBufferAddress + static_cast<u32>(4 * i), &uniforms[i], 4);
    }
    nx::vector<usse::SamplerState> samplers(16);
    for (u32 u = 0; u < samplers.size(); ++u) samplers[u].words[0] = u;
    if (const auto done = usse::load_secondary(gxp, uniforms, buffers, m, samplers); !done) return nx::err(done.error());
    if (gxp.type == usse::ShaderType::Fragment)
        if (const auto done = usse::load_fragment_inputs(gxp, varyings, samplers, m); !done) return nx::err(done.error());
    TraceState state{usse::ProgramKind::Secondary};
    usse::RunLimits limits;
    if (tracing) limits.trace = trace, limits.trace_user = &state;
    const auto secondary = usse::decode_program(gxp.secondary, usse::ProgramKind::Secondary);
    if (const auto done = usse::run(secondary, usse::ProgramKind::Secondary, m, limits); !done)
        return failure(nx::format("secondary program: {}", done.error().message));
    state.kind = usse::ProgramKind::Primary;
    const auto primary = usse::decode_program(gxp.primary, usse::ProgramKind::Primary);
    if (gpu) {
        nx::vector<psvusse::GpuTexture> images;
        for (const auto& [unit, im] : textures.images) images.push_back({unit, im.width, im.height, im.texels});
        // (the mapped bytes as one block of words, the gaps zero)
        u32 lo = ~0u, hi = 0;
        for (const auto& [address, byte] : memory.bytes) lo = nx::min(lo, address), hi = nx::max(hi, address + 1);
        nx::vector<u32> words;
        if (lo < hi) {
            lo &= ~3u;
            if (hi - lo > (64u << 20)) return failure("guest memory spans more than 64 MiB");
            words.assign((hi - lo + 3) / 4, 0);
            for (const auto& [address, byte] : memory.bytes)
                words[(address - lo) / 4] |= u32{byte} << (8 * ((address - lo) & 3));
        }
        if (const auto done = psvusse::run_on_gpu(*gpu, gxp, primary, m, images, lo < hi ? lo : 0, words); !done)
            return failure(nx::format("primary program (GPU): {}", done.error().message));
    } else if (const auto done = usse::run(primary, usse::ProgramKind::Primary, m, limits); !done) {
        return failure(nx::format("primary program: {}", done.error().message));
    }
    // (a vertex program's results are its output registers; a fragment program's, its primary attributes)
    const u32* results = gxp.type == usse::ShaderType::Fragment ? m.primary : m.output;
    nx::string out = "out";
    for (u32 i = 0; i < 64; ++i) out += nx::format(" {:08x}", results[i]);
    out += m.killed ? "\nkilled\n" : "\n";
    return out;
}

Result<void> run(nx::fs::path_view path, bool show_params, const nx::string& run_inputs, bool tracing,
                 const nx::string& spirv_out, const psvusse::GpuOptions* gpu) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    const auto gxp = usse::parse_gxp(*data);
    if (!gxp) return nx::err(gxp.error());
    if (!spirv_out.empty()) {
        const auto primary = usse::decode_program(gxp->primary, usse::ProgramKind::Primary);
        const auto spirv = gxm::translate_compute(*gxp, primary);
        if (!spirv) return nx::err(spirv.error());
        return io::write_file(spirv_out, std::span(reinterpret_cast<const u8*>(spirv->data()), spirv->size() * 4));
    }
    nx::string out;
    if (!run_inputs.empty()) {
        // ("@": each file's inputs are next to it, in <name>.in)
        nx::string input_path = run_inputs;
        if (run_inputs == "@") {
            input_path = nx::string(path.native());
            if (const usize dot = input_path.rfind("."); dot != nx::string::npos) input_path.resize(dot);
            input_path += ".in";
        }
        const auto inputs = io::read_text_file(input_path);
        if (!inputs) return nx::err(inputs.error());
        auto result = execute(*gxp, *inputs, tracing, gpu);
        if (!result) return nx::err(result.error());
        out = std::move(*result);
    } else if (show_params) {
        out = params(*gxp);
    } else {
        out = "Secondary program:\n";
        out += usse::format_listing(usse::decode_program(gxp->secondary, usse::ProgramKind::Secondary),
                                    usse::ProgramKind::Secondary);
        out += "Primary program:\n";
        out += usse::format_listing(usse::decode_program(gxp->primary, usse::ProgramKind::Primary),
                                    usse::ProgramKind::Primary);
    }
    std::fputs(out.c_str(), stdout);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::vector<nx::string> files;
    bool show_params = false;
    nx::string run_inputs;
    bool tracing = false;
    nx::string spirv_out;
    psvusse::GpuOptions gpu;
    bool use_gpu = false;
    nx::cli::Parser cli("psvusse", "Disassembles the shader programs of GXP files.");
    cli.root()
        .positional_many("files", files, "GXP files")
        .flag("params", show_params, "print the program's properties and parameters instead")
        .option("run", run_inputs, "run the programs with the inputs this file gives (see main.cpp)",
                {.value_name = "INPUTS"})
        .flag("trace", tracing, "with --run: print each executed instruction to stderr")
        .flag("gpu", use_gpu, "with --run: run the primary program's translation on a Vulkan device")
        .option("gpu-index", gpu.gpu, "with --gpu: the Vulkan device's index (default: a discrete GPU)",
                {.value_name = "N"})
        .flag("validate", gpu.validation, "with --gpu: enable the Vulkan validation layer (its errors fail the run)")
        .option("spirv", spirv_out, "write the primary program's SPIR-V translation (compute form) to this file",
                {.value_name = "FILE"});
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;
    int status = 0;
    for (const nx::string& file : files) {
        if (files.size() > 1) std::printf("== %s\n", file.c_str());
        if (const auto done = run(file, show_params, run_inputs, tracing, spirv_out, use_gpu ? &gpu : nullptr); !done) {
            nx::loge(logs::tool, "psvusse: {}: {}", file, done.error().message);
            status = 1;
        }
    }
    return status;
}
