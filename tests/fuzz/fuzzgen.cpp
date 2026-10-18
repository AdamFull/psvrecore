// fuzzgen — writes the code generator's differential test corpus: blocks of random ARM and Thumb instructions
// (seeded, so every build has the same ones), translated by the code generator.
//
//   fuzzgen <out-dir> <seed> <blocks>
//
// Output: fuzz_functions.h, fuzz_code_0.c, fuzz_module.c (the generator's), and fuzz_blocks.c, the table of blocks:
// each block's instructions (encodings), its address and instruction set, and its generated function. The test
// (generator_tests.cpp) runs each block through the generated code and through the interpreter and compares.
//
// Blocks hold straight-line code: no instruction that writes the PC, traps, or needs the system (svc, coprocessors,
// hints that yield). Each block runs into a placeholder function that returns.

#include <cstdio>
#include <cstdlib>

#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using arm::Op;

namespace {

struct Rng {
    u64 s;
    u64 next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    u32 u32v() { return static_cast<u32>(next() >> 32); }
    u32 below(u32 n) { return static_cast<u32>((next() >> 33) % n); }
};

// The operations the corpus draws from (those both the generator and the interpreter implement).
bool allowed(Op op) {
    switch (op) {
        case Op::Undefined: case Op::B: case Op::Bl: case Op::Blx: case Op::Bx: case Op::Bxj: case Op::Cbz: case Op::Cbnz:
        case Op::Tbb: case Op::Tbh: case Op::Svc: case Op::Bkpt: case Op::Udf: case Op::Smc: case Op::Yield: case Op::Wfe:
        case Op::Wfi: case Op::Setend: case Op::Cps: case Op::Cpsie: case Op::Cpsid: case Op::Subs: case Op::Eret:
        case Op::Srsda: case Op::Srsdb: case Op::Srsia: case Op::Srsib: case Op::Rfeda: case Op::Rfedb: case Op::Rfeia:
        case Op::Rfeib: case Op::Cdp: case Op::Cdp2: case Op::Mcr: case Op::Mcr2: case Op::Mrc: case Op::Mrc2: case Op::Mcrr:
        case Op::Mcrr2: case Op::Mrrc: case Op::Mrrc2: case Op::Ldc: case Op::Ldcl: case Op::Ldc2: case Op::Ldc2l: case Op::Stc:
        case Op::Stcl: case Op::Stc2: case Op::Stc2l:
            return false;
        default:
            // (floating point and Advanced SIMD join the corpus once both sides implement them)
            return op < Op::Vadd;
    }
}

bool acceptable(const arm::Instruction& in) {
    if (!in.valid() || (in.flags & arm::kUnpredictable) || (in.flags & arm::kUserRegs)) return false;
    if (!allowed(in.op)) return false;
    if (in.op == Op::Mrs && in.operands[1].imm != 1) return false;   // (SPSR)
    if (in.op == Op::Msr && (in.operands[0].imm & 0x10)) return false;
    const auto f = analysis::flow_of(in);
    return f.kind == analysis::FlowKind::Next;
}

struct Block {
    u32 address = 0;
    bool thumb = false;
    nx::vector<arm::Instruction> code;
};

Block make_block(Rng& rng, u32 address, bool thumb) {
    Block b;
    b.address = address;
    b.thumb = thumb;
    const u32 count = 1 + rng.below(8);
    u32 at = address;
    arm::ItState it;
    while (b.code.size() < count || it.active()) {
        if (!thumb) {
            for (;;) {
                const auto in = arm::decode_arm(rng.u32v(), at);
                if (acceptable(in)) {
                    b.code.push_back(in);
                    at += 4;
                    break;
                }
            }
            continue;
        }
        // Sometimes an IT block.
        if (!it.active() && rng.below(6) == 0) {
            const u32 firstcond = rng.below(14), mask = 1 + rng.below(15);
            const u16 hw = static_cast<u16>(0xBF00 | (firstcond << 4) | mask);
            arm::ItState trial = it;
            const auto in = arm::decode_thumb(hw, 0, at, trial);
            if (in.op == Op::It && !(in.flags & arm::kUnpredictable)) {
                it = trial;
                b.code.push_back(in);
                at += 2;
                continue;
            }
        }
        for (;;) {
            arm::ItState trial = it;
            const bool wide = rng.below(2) == 0;
            const u16 hw1 = wide ? static_cast<u16>(((0x1D + rng.below(3)) << 11) | (rng.u32v() & 0x7FF)) : static_cast<u16>(rng.u32v() & 0xFFFF);
            const u16 hw2 = static_cast<u16>(rng.u32v());
            if (!wide && arm::thumb_is_32bit(hw1)) continue;
            const auto in = arm::decode_thumb(hw1, hw2, at, trial);
            if (in.op == Op::It || !acceptable(in)) continue;
            it = trial;
            b.code.push_back(in);
            at += in.size;
            break;
        }
    }
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: fuzzgen <out-dir> <seed> <blocks>\n");
        return 2;
    }
    const nx::fs::path_view dir(argv[1]);
    Rng rng{std::strtoull(argv[2], nullptr, 0) | 1};
    const u32 blocks = static_cast<u32>(std::strtoul(argv[3], nullptr, 0));

    // A writable segment holds every block (so no load is done at generation time: the test's memory is random).
    // Blocks run into a placeholder function of their instruction set.
    constexpr u32 kBase = 0x81000000, kStride = 0x100, kEndArm = 0x80F00000, kEndThumb = 0x80F00011;
    analysis::Segment seg;
    seg.base = kBase;
    seg.flags = elf::kSegmentRead | elf::kSegmentWrite | elf::kSegmentExecute;
    seg.bytes.assign(blocks * kStride, 0);
    const auto image = analysis::Image::from_segments({seg});
    analysis::Program program;
    program.image = &image;
    nx::vector<Block> all;
    for (u32 i = 0; i < blocks; ++i) {
        Block b = make_block(rng, kBase + i * kStride, (i & 1) != 0);
        analysis::Function fn;
        fn.mode = b.thumb ? arm::Mode::Thumb : arm::Mode::Arm;
        fn.entry = analysis::code_address(b.address, fn.mode);
        fn.code = b.code;
        fn.runs_into[b.code.back().address] = b.thumb ? kEndThumb : kEndArm;
        program.functions[fn.entry] = std::move(fn);
        all.push_back(std::move(b));
    }
    for (const u32 end_code : {kEndArm, kEndThumb}) {
        analysis::Function end;
        end.entry = end_code;
        end.mode = analysis::code_mode(end_code);
        program.functions[end_code] = std::move(end);
    }

    recomp::Options options;
    options.name = "fuzz";
    options.instructions_per_file = 0xFFFFFFFFu;
    const auto output = recomp::generate(program, options);
    usize traps = 0;
    for (const auto& [what, n] : output.stats.traps) {
        std::fprintf(stderr, "fuzzgen: %zu x %s\n", static_cast<size_t>(n), what.c_str());
        traps += n;
    }
    if (traps) {
        std::fprintf(stderr, "fuzzgen: the corpus has instructions the generator translates to traps\n");
        return 1;
    }

    nx::string table = "/* Generated by fuzzgen. */\n#include \"fuzz_functions.h\"\n#include \"fuzz_blocks.h\"\n\n";
    for (usize i = 0; i < all.size(); ++i) {
        table += nx::format("static const uint32_t raw_{}[] = {{", i);
        for (const auto& in : all[i].code) table += nx::format("0x{:08X}u, ", in.raw);
        table += "};\n";
    }
    table += nx::format("\nconst FuzzBlock fuzz_blocks[{}] = {{\n", all.size());
    for (usize i = 0; i < all.size(); ++i)
        table += nx::format("    {{0x{:08X}u, {}, {}, raw_{}, {}}},\n", all[i].address, all[i].thumb ? 1 : 0, all[i].code.size(), i,
                            recomp::function_name(all[i].address));
    table += "};\n";
    table += nx::format("const uint32_t fuzz_block_count = {};\n", all.size());

    const auto write = [&](nx::string_view name, nx::string_view text) {
        const nx::string path = dir / nx::fs::path_view(name);
        if (auto ok = io::write_text_file(nx::fs::path_view(path), text); !ok) {
            std::fprintf(stderr, "fuzzgen: %s\n", ok.error().message.c_str());
            std::exit(1);
        }
    };
    if (!nx::fs::create_directories(dir)) {
        std::fprintf(stderr, "fuzzgen: cannot create the output folder\n");
        return 1;
    }
    for (const auto& f : output.files) write(f.name, f.text);
    write("fuzz_blocks.c", table);
    return 0;
}
