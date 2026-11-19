// psvrecore::analysis: the floating-point registers in locals (vfp.hpp) on small functions assembled by LLVM's
// assembler: what each function may read and write of them (its summary), what its callers keep across a call to it,
// how it holds a D register it accesses both ways, what it writes before an exit and a call to code outside the
// module's view. Each instruction's effect is given as the code generator's first translation would record it.

#include "nxtest.h"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/analysis/registers.hpp"
#include "psvrecore/analysis/vfp.hpp"
#include "psvrecore/formats/elf.hpp"

using namespace psvrecore;
using analysis::VfpSet;

namespace {

constexpr u32 kBase = 0x81000000;

// The functions, at kBase (Thumb):
//   int_leaf  00 adds r0, #1; bx lr
//   vfp_leaf  04 vadd.f32 s0, s0, s1; bx lr
//   caller    0a push {r4, lr}; vadd.f32 s4, s4, s5; bl int_leaf; vadd.f32 s4, s4, s5; bl vfp_leaf; vmov r0, s4;
//                pop {r4, pc}
//   indirect  22 push {r4, lr}; vadd.f32 s4, s4, s5; blx r1; vmov r0, s4; pop {r4, pc}
//   mixed     30 vadd.f32 s4, s4, s5; vmov r0, r1, d2; bx lr
constexpr u16 kCode[] = {
    0x3001, 0x4770,                                                                          // int_leaf
    0xEE30, 0x0A20, 0x4770,                                                                  // vfp_leaf
    0xB510, 0xEE32, 0x2A22, 0xF7FF, 0xFFF6, 0xEE32, 0x2A22, 0xF7FF, 0xFFF4, 0xEE12, 0x0A10, 0xBD10,  // caller
    0xB510, 0xEE32, 0x2A22, 0x4788, 0xEE12, 0x0A10, 0xBD10,                                  // indirect
    0xEE32, 0x2A22, 0xEC51, 0x0B12, 0x4770,                                                  // mixed
};
constexpr u32 kInt = kBase + 0x00 + 1, kVfp = kBase + 0x04 + 1, kCaller = kBase + 0x0A + 1, kIndirect = kBase + 0x22 + 1,
              kMixed = kBase + 0x30 + 1;

// What the translation of each floating-point instruction reads and writes, by offset.
analysis::VfpEffect effect_at(u32 offset) {
    analysis::VfpEffect e;
    switch (offset) {
        case 0x04:  // vadd.f32 s0, s0, s1
            e.uses = {0x3, analysis::kFpscrRest}, e.defs = {0x1, 0}, e.s_access = 1;
            break;
        case 0x0C: case 0x14: case 0x24: case 0x30:  // vadd.f32 s4, s4, s5
            e.uses = {0x30, analysis::kFpscrRest}, e.defs = {0x10, 0}, e.s_access = 1u << 2;
            break;
        case 0x1C: case 0x2A:  // vmov r0, s4
            e.uses = {0x10, 0}, e.s_access = 1u << 2;
            break;
        case 0x34:  // vmov r0, r1, d2
            e.uses = {0x30, 0}, e.d_access = 1u << 2;
            break;
        default: break;
    }
    return e;
}

struct Fixture {
    analysis::Image image;
    analysis::Program program;
    analysis::RegisterAnalysis registers;
    nx::map<u32, nx::vector<analysis::VfpEffect>> effects;

    Fixture() : image(make_image()), program(discover(image)), registers(analysis::register_analysis(program)) {
        for (const auto& [code, fn] : program.functions) {
            auto& v = effects[code];
            for (const auto& in : fn.code) v.push_back(effect_at(in.address - kBase));
        }
    }
    analysis::VfpAnalysis run(bool summaries) const {
        analysis::VfpOptions o;
        o.summaries = summaries;
        o.effects = &effects;
        o.registers = &registers;
        return analysis::vfp_analysis(program, o);
    }
    usize at(u32 code, u32 offset) const {
        const auto* fn = program.function_at(code);
        for (usize i = 0; fn && i < fn->code.size(); ++i)
            if (fn->code[i].address == kBase + offset) return i;
        return ~usize(0);
    }

    static analysis::Image make_image() {
        analysis::Segment seg;
        seg.base = seg.linked = kBase;
        seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
        for (u16 h : kCode) {
            seg.bytes.push_back(static_cast<u8>(h));
            seg.bytes.push_back(static_cast<u8>(h >> 8));
        }
        seg.file_size = seg.size();
        return analysis::Image::from_segments({seg});
    }
    static analysis::Program discover(const analysis::Image& image) {
        analysis::Options options;
        options.extra_entries = {kInt, kVfp, kCaller, kIndirect, kMixed};
        options.use_exidx = false;
        return analysis::analyze(image, options);
    }
};

}  // namespace

TEST_CASE("vfp analysis: summaries, and what callers keep across calls") {
    const Fixture f;
    const auto v = f.run(true);
    // A function that touches no floating-point register reads and writes nothing; one that adds s0 and s1 writes d0.
    REQUIRE(v.at(kInt) != nullptr);
    CHECK(v.at(kInt)->reads.empty());
    CHECK(v.at(kInt)->writes.empty());
    CHECK(v.at(kVfp)->writes == (VfpSet{0x3, 0}));
    CHECK((v.at(kVfp)->reads.words & 0x3) == 0x3);
    CHECK((v.at(kVfp)->reads.fpscr & analysis::kFpscrRest) != 0);
    // The caller's s4 is dirty and live across both calls: neither callee touches it, so nothing is written or read
    // back there; it is written at the return.
    const auto* c = v.at(kCaller);
    const usize call1 = f.at(kCaller, 0x10), call2 = f.at(kCaller, 0x18), ret = f.at(kCaller, 0x20);
    REQUIRE(call1 != ~usize(0));
    CHECK(c->call_store[call1].empty());
    CHECK(c->call_load[call1].empty());
    CHECK(c->call_store[call2].empty());
    CHECK(c->call_load[call2].empty());
    CHECK(c->calls_keeping == 2u);
    CHECK((c->exit_store[ret].words & 0x10) != 0);
    CHECK((c->load_at_entry.words & 0x30) == 0x30);
    // A call to code outside the module's view: s4 written before it and read back after; the function's summary is
    // everything.
    const auto* ind = v.at(kIndirect);
    const usize blx = f.at(kIndirect, 0x28);
    CHECK((ind->call_store[blx].words & 0x10) != 0);
    CHECK((ind->call_load[blx].words & 0x10) != 0);
    CHECK(ind->writes == analysis::kAllVfp);
    CHECK(v.at(kCaller)->reads.words != ~0ull);
}

TEST_CASE("vfp analysis: a D register accessed by word and whole is held as floats when words are as common") {
    const Fixture f;
    const auto v = f.run(true);
    const auto* m = v.at(kMixed);
    CHECK(m->s_held(2));
    CHECK(m->units(VfpSet{0x10, 0}) == (VfpSet{0x10, 0}));
    // (held as a double, a word of it is the whole register)
    analysis::FunctionVfp d;
    CHECK(d.units(VfpSet{0x10, 0}) == (VfpSet{0x30, 0}));
}

TEST_CASE("vfp analysis: without summaries every call reads and writes everything") {
    const Fixture f;
    const auto v = f.run(false);
    const auto* c = v.at(kCaller);
    const usize call1 = f.at(kCaller, 0x10);
    CHECK((c->call_store[call1].words & 0x10) != 0);
    CHECK((c->call_load[call1].words & 0x10) != 0);
    CHECK(c->calls_keeping == 0u);
}
