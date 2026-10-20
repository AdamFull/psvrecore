// psvrender — the rendering tests' runner: draws a scene of GXM programs with the reference renderer
// (gxm/reference.hpp: the USSE reference interpreter, rasterized on the CPU) and with the Vulkan backend (the
// programs' translations, drawn as the HLE renderer draws), writes both images and compares them pixel by pixel.
// scripts/render_check.py compiles the tests' shaders (tests/render) and runs it.
//
// A scene file, one directive per line ('#' comments; paths relative to the scene file):
//   target <width> <height> [<clear RGBA8 hex, red in the low byte>]
//   vertex <gxp>, fragment <gxp>            the programs of the draws that follow
//   attribute <name> <components> <f32>...  a vertex attribute (per vertex, components values)
//   indices <index>...                      a triangle list
//   uniform vertex|fragment <name> <f32>... a default uniform buffer parameter (f32 or f16, by its type)
//   buffer vertex|fragment <n> <f32>...     uniform buffer BUFFER[n]'s words
//   words vertex|fragment uniforms|<n> <hex>...  the default uniform buffer's or BUFFER[n]'s words as they are
//   texture <unit> <width> <height> nearest|linear checker|gradient|noise
//   depth, blend                            a LESS_EQUAL depth test with writes; source-alpha blending
//   tolerance <n>                           the largest difference of a channel that matches (default 1)
//   draw                                    the draw: attributes, indices, uniforms, buffers, textures and states
//                                           start over for the next (the programs stay)
// The comparison leaves out the pixels the reference marks as next to an edge; a channel may differ by the
// tolerance (rounding of interpolation and filtering).

#include <bit>
#include <cmath>
#include <cstdio>

#include "gpu.hpp"
#include "psvrecore/io/png.hpp"
#include "psvrecore/cli.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/text.hpp"
#include "psvrecore/usse/interp.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using gxm::reference::Draw;

namespace {

struct LoadedProgram {
    usse::Gxp gxp;
    nx::vector<usse::Decoded> primary, secondary;
};

struct Scene {
    u32 width = 64, height = 64, clear = 0;
    i64 tolerance = -1;  // (-1: the command line's)
    nx::vector<nx::unique_ptr<LoadedProgram>> programs;
    nx::vector<Draw> draws;
};

Result<LoadedProgram*> load_program(Scene& s, const nx::string& path) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    auto gxp = usse::parse_gxp(*data);
    if (!gxp) return nx::err(gxp.error());
    auto p = nx::make_unique<LoadedProgram>();
    p->gxp = std::move(*gxp);
    p->primary = usse::decode_program(p->gxp.primary, usse::ProgramKind::Primary);
    p->secondary = usse::decode_program(p->gxp.secondary, usse::ProgramKind::Secondary);
    s.programs.push_back(std::move(p));
    return s.programs.back().get();
}

// Writes a parameter's values into a default uniform buffer (its words from the parameter's resource index).
Result<void> set_uniform(const usse::Gxp& g, nx::string_view name, std::span<const f32> values, nx::vector<u32>& words) {
    const auto index = g.find(name);
    if (!index) return failure(nx::format("no parameter '{}'", name));
    const usse::Parameter& p = g.parameters[*index];
    if (p.category != usse::ParameterCategory::Uniform || p.container != usse::kDefaultUniformContainer)
        return failure(nx::format("'{}' is not a default uniform", name));
    const bool half = p.type == usse::ParameterType::F16;
    const usize need = p.resource + (half ? (values.size() + 1) / 2 : values.size());
    if (words.size() < need) words.resize(need, 0);
    for (usize k = 0; k < values.size(); ++k) {
        if (half) {
            u32& w = words[p.resource + k / 2];
            const u32 h = usse::to_half(values[k]);
            w = (k & 1) ? (w & 0xFFFF) | h << 16 : (w & 0xFFFF0000u) | h;
        } else {
            words[p.resource + k] = std::bit_cast<u32>(values[k]);
        }
    }
    return {};
}

gxm::reference::Texture make_texture(u32 w, u32 h, nx::string_view pattern, bool linear) {
    gxm::reference::Texture t;
    t.width = w, t.height = h, t.linear = linear;
    t.texels.resize(usize{w} * h);
    u32 seed = 0x12345678u;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 c;
            if (pattern == "checker") {
                c = ((x / 4 + y / 4) & 1) ? 0xFF2080E0u : 0xFFE0C020u;
            } else if (pattern == "gradient") {
                c = (x * 255 / nx::max(w - 1, 1u)) | (y * 255 / nx::max(h - 1, 1u)) << 8 | 0x80u << 16 | 0xFFu << 24;
            } else {
                seed = seed * 1664525u + 1013904223u;
                c = seed | 0xFF000000u;
            }
            t.texels[usize{y} * w + x] = c;
        }
    }
    return t;
}

Result<Scene> load_scene(const nx::string& path) {
    const auto text = io::read_text_file(path);
    if (!text) return nx::err(text.error());
    nx::string dir(path);
    usize slash = dir.rfind("/");
    if (const usize back = dir.rfind("\\"); back != nx::string::npos && (slash == nx::string::npos || back > slash)) slash = back;
    if (slash != nx::string::npos) dir.resize(slash + 1);
    else dir.clear();
    Scene s;
    LoadedProgram* vertex = nullptr;
    LoadedProgram* fragment = nullptr;
    Draw current;
    nx::string error;
    u32 line_number = 0;
    auto floats = [&](std::span<const nx::string_view> fields, usize from, nx::vector<f32>& out) {
        for (usize i = from; i < fields.size(); ++i) {
            const auto v = parse_f64(fields[i]);
            if (!v) return false;
            out.push_back(static_cast<f32>(*v));
        }
        return true;
    };
    for_each_line(*text, [&](nx::string_view raw) {
        ++line_number;
        if (!error.empty()) return;
        nx::string_view line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') return;
        nx::vector<nx::string_view> f;  // (the fields; runs of spaces separate them)
        for (nx::string_view x : split(line, ' '))
            if (!x.empty()) f.push_back(x);
        const nx::string_view op = f[0];
        auto bad = [&](nx::string_view why) { error = nx::format("line {}: {}", line_number, why); };
        if (op == "target" && f.size() >= 3) {
            s.width = static_cast<u32>(parse_u64(f[1]).value_or(0));
            s.height = static_cast<u32>(parse_u64(f[2]).value_or(0));
            if (f.size() >= 4) s.clear = static_cast<u32>(parse_u64(f[3], 16).value_or(0));
            if (!s.width || !s.height || s.width > 1024 || s.height > 1024) bad("bad target size");
        } else if ((op == "vertex" || op == "fragment") && f.size() == 2) {
            auto p = load_program(s, dir + nx::string(f[1]));
            if (!p) return bad(p.error().message);
            (op == "vertex" ? vertex : fragment) = *p;
        } else if (op == "attribute" && f.size() >= 4) {
            if (!vertex) return bad("an attribute before the vertex program");
            const auto index = vertex->gxp.find(f[1]);
            if (!index) return bad(nx::format("no attribute '{}'", f[1]));
            gxm::reference::Attribute a;
            a.reg = vertex->gxp.parameters[*index].resource;
            a.components = static_cast<u32>(parse_u64(f[2]).value_or(0));
            if (!a.components || a.components > 4 || !floats(f, 3, a.values) || a.values.size() % a.components)
                return bad("bad attribute values");
            current.attributes.push_back(std::move(a));
        } else if (op == "indices") {
            for (usize i = 1; i < f.size(); ++i) current.indices.push_back(static_cast<u32>(parse_u64(f[i]).value_or(0)));
        } else if (op == "uniform" && f.size() >= 4) {
            LoadedProgram* p = f[1] == "vertex" ? vertex : fragment;
            if (!p) return bad("a uniform before its program");
            nx::vector<f32> v;
            if (!floats(f, 3, v)) return bad("bad uniform values");
            auto& words = f[1] == "vertex" ? current.vertex_uniforms : current.fragment_uniforms;
            if (auto r = set_uniform(p->gxp, f[2], v, words); !r) return bad(r.error().message);
        } else if (op == "buffer" && f.size() >= 4) {
            const u32 n = static_cast<u32>(parse_u64(f[2]).value_or(99));
            if (n >= 14) return bad("bad buffer index");
            nx::vector<f32> v;
            if (!floats(f, 3, v)) return bad("bad buffer values");
            auto& buffers = f[1] == "vertex" ? current.vertex_buffers : current.fragment_buffers;
            if (buffers.size() <= n) buffers.resize(n + 1);
            for (f32 x : v) buffers[n].push_back(std::bit_cast<u32>(x));
        } else if (op == "words" && f.size() >= 4) {
            // (words vertex|fragment uniforms|<n> <hex>...: the default uniform buffer's or BUFFER[n]'s words as they
            // are, a draw captured from a game: PSVRECORE_GXM_CAPTURE)
            const bool v = f[1] == "vertex";
            nx::vector<u32>* words = nullptr;
            if (f[2] == "uniforms") {
                words = v ? &current.vertex_uniforms : &current.fragment_uniforms;
            } else {
                const u32 n = static_cast<u32>(parse_u64(f[2]).value_or(99));
                if (n >= 14) return bad("bad buffer index");
                auto& buffers = v ? current.vertex_buffers : current.fragment_buffers;
                if (buffers.size() <= n) buffers.resize(n + 1);
                words = &buffers[n];
            }
            for (usize i = 3; i < f.size(); ++i) {
                const auto w = parse_u64(f[i], 16);
                if (!w) return bad("bad word");
                words->push_back(static_cast<u32>(*w));
            }
        } else if (op == "texture" && f.size() == 6) {
            const u32 unit = static_cast<u32>(parse_u64(f[1]).value_or(99));
            const u32 w = static_cast<u32>(parse_u64(f[2]).value_or(0)), h = static_cast<u32>(parse_u64(f[3]).value_or(0));
            if (unit >= 16 || !w || !h || w > 1024 || h > 1024) return bad("bad texture");
            if (current.textures.size() <= unit) current.textures.resize(unit + 1);
            current.textures[unit] = make_texture(w, h, f[5], f[4] == "linear");
        } else if (op == "tolerance" && f.size() == 2) {
            s.tolerance = static_cast<i64>(parse_u64(f[1]).value_or(1));
        } else if (op == "depth" && f.size() == 1) {
            current.depth_test = true;
        } else if (op == "blend" && f.size() == 1) {
            current.blend = true;
        } else if (op == "draw" && f.size() == 1) {
            if (!vertex || !fragment) return bad("a draw without programs");
            current.vertex = {&vertex->gxp, vertex->primary, vertex->secondary};
            current.fragment = {&fragment->gxp, fragment->primary, fragment->secondary};
            s.draws.push_back(std::move(current));
            current = Draw{};
        } else {
            bad(nx::format("unknown directive '{}'", line));
        }
    });
    if (!error.empty()) return failure(error);
    if (s.draws.empty()) return failure("no draws");
    return s;
}

struct Comparison {
    u32 compared = 0, edge = 0, mismatched = 0, max_difference = 0, drawn = 0;
};

Comparison compare(const gxm::reference::Image& ref, const gxm::reference::Image& gpu, u32 clear, u32 tolerance,
                   nx::vector<u32>& diff) {
    Comparison c;
    diff.assign(ref.rgba.size(), 0xFF000000u);
    for (usize i = 0; i < ref.rgba.size(); ++i) {
        if (ref.rgba[i] != clear) ++c.drawn;
        if (ref.edge[i]) {
            ++c.edge;
            diff[i] = 0xFF404040u;
            continue;
        }
        ++c.compared;
        u32 worst = 0;
        for (u32 k = 0; k < 4; ++k) {
            const i32 a = static_cast<i32>((ref.rgba[i] >> (8 * k)) & 0xFF), b = static_cast<i32>((gpu.rgba[i] >> (8 * k)) & 0xFF);
            worst = nx::max<u32>(worst, static_cast<u32>(a > b ? a - b : b - a));
        }
        c.max_difference = nx::max(c.max_difference, worst);
        if (worst > tolerance) {
            ++c.mismatched;
            diff[i] = 0xFF0000FFu;  // (red)
        } else if (worst) {
            diff[i] = 0xFF00A0A0u;  // (within the tolerance: olive)
        }
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    nx::string scene_path, out_dir;
    u32 tolerance = 1;
    psvrender::GpuOptions gpu;
    nx::cli::Parser cli("psvrender", "Draws a scene with the reference renderer and with Vulkan, and compares them.");
    cli.root()
        .positional("scene", scene_path, "the scene file (see main.cpp)")
        .option("out", out_dir, "write reference.png, gpu.png and diff.png to this folder", {.value_name = "DIR"})
        .option("tolerance", tolerance, "the largest difference of a channel that matches (default 1)", {.value_name = "N"})
        .option("gpu-index", gpu.gpu, "the Vulkan device's index (default: a discrete GPU)", {.value_name = "N"})
        .flag("validate", gpu.validation, "enable the Vulkan validation layer (its errors fail the run)");
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;

    auto scene = load_scene(scene_path);
    if (!scene) {
        nx::loge(logs::tool, "psvrender: {}: {}", scene_path, scene.error().message);
        return 1;
    }
    gxm::reference::Image ref, on_gpu;
    ref.reset(scene->width, scene->height, scene->clear);
    for (const Draw& d : scene->draws) {
        if (auto r = gxm::reference::render(d, ref); !r) {
            nx::loge(logs::tool, "psvrender: reference: {}", r.error().message);
            return 1;
        }
    }
    on_gpu.reset(scene->width, scene->height, scene->clear);
    if (auto r = psvrender::render_on_gpu(gpu, scene->draws, on_gpu); !r) {
        nx::loge(logs::tool, "psvrender: gpu: {}", r.error().message);
        return 1;
    }
    nx::vector<u32> diff;
    const Comparison c =
        compare(ref, on_gpu, scene->clear, scene->tolerance >= 0 ? static_cast<u32>(scene->tolerance) : tolerance, diff);
    if (!out_dir.empty()) {
        auto write = [&](const char* name, const nx::vector<u32>& px) {
            const nx::string p = out_dir + "/" + name;
            if (!io::write_png(p.c_str(), scene->width, scene->height, reinterpret_cast<const u8*>(px.data())))
                nx::logw(logs::tool, "psvrender: cannot write {}", p);
        };
        write("reference.png", ref.rgba);
        write("gpu.png", on_gpu.rgba);
        write("diff.png", diff);
    }
    std::printf("compared %u, edge %u, drawn %u, mismatched %u, max difference %u\n", c.compared, c.edge, c.drawn,
                c.mismatched, c.max_difference);
    if (!c.drawn) {
        nx::loge(logs::tool, "psvrender: the reference drew nothing (the test checks nothing)");
        return 1;
    }
    return c.mismatched ? 2 : 0;
}
