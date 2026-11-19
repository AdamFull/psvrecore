// psvrecore::analysis: the control-flow graph (cfg.hpp) and the condition flags' liveness (flags.hpp) on small
// functions assembled by LLVM's assembler (the Thumb halfwords in each comment's order), and what each instruction
// reads and writes.

#include <initializer_list>

#include "nxtest.h"
#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/flags.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/formats/elf.hpp"

using namespace psvrecore;
using analysis::kFlagC;
using analysis::kFlagN;
using analysis::kFlagV;
using analysis::kFlagZ;

namespace {

constexpr u32 kBase = 0x81000000;

// The functions, at kBase (Thumb):
//   a_zonly    00 cmp r0, r1; beq 1f; adds r0, r0, r1; movs r0, #0; 1: bx lr
//   b_carry_in 0a adcs r0, r1; bx lr
//   c_cond     0e cmp r0, #0; it eq; cmpeq r1, r2; bne 1f; movs r0, #1; 1: bx lr
//   d_caller   1a push {r4, lr}; bl d_helper; bhi 1f; movs r0, #1; 1: pop {r4, pc}
//   d_helper   26 cmp r2, r3; bx lr
//   e_passer   2a add r0, r1; bx lr
//   e_caller   2e push {r4, lr}; cmp r0, r1; bl e_passer; bhi 1f; movs r0, #1; 1: pop {r4, pc}
//   f_indirect 3c push {r4, lr}; blx r3; bhi 1f; movs r0, #1; 1: pop {r4, pc}
//   g_tail     46 cmp r0, #2; bhi 2f; tbb [pc, r0]; (2, 3, 2, 0); 10: bx lr; 11: lsls r0, r0, #1; bx lr; 2: b.w b_carry_in
constexpr u16 kCode[] = {
    0x4288, 0xD001, 0x1840, 0x2000, 0x4770,                  // a_zonly
    0x4148, 0x4770,                                          // b_carry_in
    0x2800, 0xBF08, 0x4291, 0xD100, 0x2001, 0x4770,          // c_cond
    0xB510, 0xF000, 0xF803, 0xD800, 0x2001, 0xBD10,          // d_caller
    0x429A, 0x4770,                                          // d_helper
    0x4408, 0x4770,                                          // e_passer
    0xB510, 0x4288, 0xF7FF, 0xFFFA, 0xD800, 0x2001, 0xBD10,  // e_caller
    0xB510, 0x4798, 0xD800, 0x2001, 0xBD10,                  // f_indirect
    0x2802, 0xD806, 0xE8DF, 0xF000, 0x0302, 0x0002, 0x4770, 0x0040, 0x4770, 0xF7FF, 0xBFD7,  // g_tail
};
constexpr u32 kA = kBase + 0x00 + 1, kB = kBase + 0x0A + 1, kC = kBase + 0x0E + 1, kDCaller = kBase + 0x1A + 1,
              kDHelper = kBase + 0x26 + 1, kEPasser = kBase + 0x2A + 1, kECaller = kBase + 0x2E + 1, kFIndirect = kBase + 0x3C + 1,
              kGTail = kBase + 0x46 + 1;

analysis::Image image() {
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

analysis::Program discover(const analysis::Image& image, std::initializer_list<u32> entries) {
    analysis::Options options;
    options.extra_entries.assign(entries.begin(), entries.end());
    options.use_exidx = false;
    return analysis::analyze(image, options);
}

// The index in the function's code of the instruction at kBase + offset.
usize at(const analysis::Function& fn, u32 offset) {
    for (usize i = 0; i < fn.code.size(); ++i)
        if (fn.code[i].address == kBase + offset) return i;
    return ~usize(0);
}

arm::Instruction thumb(u16 hw1, u16 hw2 = 0) {
    arm::ItState it;
    return arm::decode_thumb(hw1, hw2, kBase, it);
}

}  // namespace

TEST_CASE("analysis: the flags each instruction reads and writes") {
    using analysis::flag_effect;
    constexpr analysis::FlagSet kNZ = kFlagN | kFlagZ, kNZC = kFlagN | kFlagZ | kFlagC, kAll = analysis::kAllFlags;
    // A logical operation writes the shifter's carry only when the shifter gives one.
    CHECK(flag_effect(thumb(0xEA11, 0x1170)).defs == kNZC);  // ands.w r1, r1, r0, ror #5
    CHECK(flag_effect(thumb(0xEA11, 0x1170)).kills == kNZC);
    CHECK(flag_effect(thumb(0x4212)).defs == kNZ);           // tst r2, r2
    CHECK(flag_effect(thumb(0x0008)).defs == kNZ);           // movs r0, r1
    CHECK(flag_effect(thumb(0xF05F, 0x4000)).defs == kNZC);  // movs.w r0, #0x80000000 (a rotated immediate)
    CHECK(flag_effect(arm::decode_arm(0xE3B00102, kBase)).defs == kNZC);  // movs r0, #0x80000000
    CHECK(flag_effect(arm::decode_arm(0xE3B00001, kBase)).defs == kNZ);   // movs r0, #1
    CHECK(flag_effect(arm::decode_arm(0xE3320000, kBase)).defs == kNZ);   // teq r2, #0
    // Shifts: by a register, a shift by 0 keeps the carry (written, not ended).
    CHECK(flag_effect(thumb(0x0043)).defs == kNZC);  // lsls r3, r0, #1
    CHECK(flag_effect(thumb(0x0043)).uses == 0);
    const auto asrs = flag_effect(thumb(0xFA51, 0xF302));  // asrs.w r3, r1, r2
    CHECK(asrs.defs == kNZC);
    CHECK(asrs.kills == kNZ);
    CHECK(asrs.uses == 0);
    // Carry in: adc, rrx as an operand or as an address offset.
    CHECK(flag_effect(thumb(0xF141, 0x0005)).uses == kFlagC);  // adc r0, r1, #5
    CHECK(flag_effect(thumb(0xF141, 0x0005)).defs == 0);
    CHECK(flag_effect(thumb(0xEB01, 0x0032)).uses == kFlagC);  // add.w r0, r1, r2, rrx
    CHECK(flag_effect(arm::decode_arm(0xE7910062, kBase)).uses == kFlagC);  // ldr r0, [r1, r2, rrx]
    // A condition reads its flags; the writes of a conditional instruction are still writes (they do not end a
    // flag's life: the liveness keeps the path where it does not execute).
    const auto addseq = flag_effect(arm::decode_arm(0x00900002, kBase));  // addseq r0, r0, r2
    CHECK(addseq.uses == kFlagZ);
    CHECK(addseq.defs == kAll);
    // The status registers; multiplies write N and Z.
    CHECK(flag_effect(thumb(0xF3EF, 0x8300)).uses == kAll);  // mrs r3, apsr
    CHECK(flag_effect(thumb(0xF381, 0x8800)).defs == kAll);  // msr APSR_nzcvq, r1
    CHECK(flag_effect(thumb(0xEEF1, 0xFA10)).defs == kAll);  // vmrs APSR_nzcv, fpscr
    CHECK(flag_effect(thumb(0x4348)).defs == kNZ);           // muls r0, r1, r0
}

TEST_CASE("analysis: a function's control-flow graph") {
    const auto img = image();
    const auto p = discover(img, {kA, kB, kDCaller, kDHelper, kGTail});
    using analysis::EdgeKind;
    {
        const auto& fn = p.functions.at(kDCaller);
        const auto g = analysis::build_cfg(p, fn);
        REQUIRE(g.nodes.size() == 5);
        CHECK(g.entry == 0);
        const auto& call = g.nodes[at(fn, 0x1C)];
        CHECK(call.call == analysis::CallKind::Direct);
        CHECK(call.call_target == kDHelper);
        const auto& bhi = g.nodes[at(fn, 0x20)];
        CHECK(bhi.conditional);
        CHECK(bhi.skip.kind == EdgeKind::Next);
        CHECK(bhi.skip.to == at(fn, 0x22));
        REQUIRE(bhi.edge_count == 1);
        CHECK(g.begin(bhi)->to == at(fn, 0x24));
        const auto& pop = g.nodes[at(fn, 0x24)];
        REQUIRE(pop.edge_count == 1);
        CHECK(g.begin(pop)->kind == EdgeKind::Return);
        // Blocks (a call ends one): the push and the call; the bhi; the movs; the pop (two ways in).
        REQUIRE(g.blocks.size() == 4);
        CHECK(g.block_of[at(fn, 0x1A)] == g.block_of[at(fn, 0x1C)]);
        CHECK(g.block_of[at(fn, 0x1C)] != g.block_of[at(fn, 0x20)]);
        CHECK(g.blocks[g.block_of[at(fn, 0x24)]].predecessors.size() == 2);
    }
    {
        const auto& fn = p.functions.at(kGTail);
        const auto g = analysis::build_cfg(p, fn);
        const auto& tbb = g.nodes[at(fn, 0x4A)];
        // The table's three entries (two of them the same place), and a trap for an index outside it.
        REQUIRE(tbb.edge_count == 4);
        CHECK(g.begin(tbb)[0].to == at(fn, 0x52));
        CHECK(g.begin(tbb)[1].to == at(fn, 0x54));
        CHECK(g.begin(tbb)[3].kind == EdgeKind::Trap);
        const auto& tail = g.nodes[at(fn, 0x58)];
        REQUIRE(tail.edge_count == 1);
        CHECK(g.begin(tail)->kind == EdgeKind::TailCall);
        CHECK(g.begin(tail)->to == kB);
        CHECK(g.blocks[g.block_of[at(fn, 0x4A)]].successors.size() == 2);
    }
}

TEST_CASE("analysis: condition flags live only where an instruction reads them") {
    const auto img = image();
    const auto p = discover(img, {kA, kB, kC, kDCaller, kDHelper, kEPasser, kECaller, kGTail});
    const auto fl = analysis::flag_liveness(p);
    const auto& a = *fl.at(kA);
    const auto& fa = p.functions.at(kA);
    // cmp + beq: Z only; the adds and movs after it compute nothing (no caller reads the flags).
    CHECK(a.after[at(fa, 0x00)] == kFlagZ);
    CHECK(a.after[at(fa, 0x04)] == 0);
    CHECK(a.after[at(fa, 0x06)] == 0);
    CHECK(a.need == 0);
    CHECK(a.pass == 0);
    CHECK(a.live_out == 0);
    // A carry in from the caller.
    CHECK(fl.at(kB)->need == kFlagC);
    CHECK(fl.at(kB)->live_in == kFlagC);
    // A compare under a condition: where it does not execute, the first compare's Z goes on to the bne.
    const auto& fc = p.functions.at(kC);
    CHECK(fl.at(kC)->after[at(fc, 0x0E)] == kFlagZ);
    CHECK(fl.at(kC)->after[at(fc, 0x12)] == kFlagZ);
    // A helper that returns a compare in the flags: they are live at its return and read back after the call;
    // nothing is written before the call (the helper writes all four first).
    const auto& fd = p.functions.at(kDCaller);
    CHECK(fl.at(kDHelper)->live_out == (kFlagC | kFlagZ));
    CHECK(fl.at(kDHelper)->need == 0);
    CHECK(fl.at(kDCaller)->after_call[at(fd, 0x1C)] == (kFlagC | kFlagZ));
    CHECK(fl.at(kDCaller)->before_call[at(fd, 0x1C)] == 0);
    // A callee that leaves the flags: the caller's compare goes through it, written before and read back after.
    const auto& fe = p.functions.at(kECaller);
    CHECK(fl.at(kEPasser)->need == 0);
    CHECK(fl.at(kEPasser)->pass == analysis::kAllFlags);
    CHECK(fl.at(kEPasser)->live_out == (kFlagC | kFlagZ));
    CHECK(fl.at(kEPasser)->live_in == (kFlagC | kFlagZ));
    CHECK(fl.at(kECaller)->after[at(fe, 0x30)] == (kFlagC | kFlagZ));
    CHECK(fl.at(kECaller)->before_call[at(fe, 0x32)] == (kFlagC | kFlagZ));
    CHECK(fl.at(kECaller)->after_call[at(fe, 0x32)] == (kFlagC | kFlagZ));
    // A tail call into a function that reads the carry: the compare's C is live there too (and Z for the bhi); and
    // N and V for the table's trap (an index outside it), which shows the flags the function computed.
    const auto& fg = p.functions.at(kGTail);
    CHECK(fl.at(kGTail)->after[at(fg, 0x46)] == analysis::kAllFlags);
    CHECK(fl.at(kGTail)->after[at(fg, 0x48)] == kFlagC);  // (taken: the tail call's carry)
    CHECK(fl.at(kGTail)->after[at(fg, 0x54)] == 0);  // (the lsls: no caller reads its flags)
    CHECK(fl.at(kGTail)->need == 0);
    // The module: b_carry_in reads the carry at entry; no indirect call reads flags after it.
    CHECK(fl.needed_at_entry == kFlagC);
    CHECK(fl.given_at_entry == kFlagC);
    CHECK(fl.needed_at_return == 0);
    CHECK(fl.given_at_return == 0);
}

TEST_CASE("analysis: flags read after an indirect call are left at every return") {
    const auto img = image();
    const auto p = discover(img, {kA, kFIndirect});
    const auto fl = analysis::flag_liveness(p);
    const auto& ff = p.functions.at(kFIndirect);
    // (any function may be the callee: each leaves C and Z when it returns)
    CHECK(fl.needed_at_return == (kFlagC | kFlagZ));
    CHECK(fl.given_at_return == (kFlagC | kFlagZ));
    CHECK(fl.at(kA)->live_out == (kFlagC | kFlagZ));
    CHECK(fl.at(kFIndirect)->after_call[at(ff, 0x3E)] == (kFlagC | kFlagZ));
    CHECK(fl.at(kFIndirect)->before_call[at(ff, 0x3E)] == (kFlagC | kFlagZ));
    // a_zonly then computes C and Z where they reach its return.
    const auto& fa = p.functions.at(kA);
    CHECK(fl.at(kA)->after[at(fa, 0x00)] == (kFlagC | kFlagZ));
    CHECK(fl.at(kA)->after[at(fa, 0x04)] == kFlagC);  // (live after the adds: its C; the movs writes Z again)
    CHECK(fl.at(kA)->after[at(fa, 0x06)] == (kFlagC | kFlagZ));
}

TEST_CASE("analysis: flag liveness options: all four at a function's returns, and none") {
    const auto img = image();
    const auto p = discover(img, {kA, kDCaller, kDHelper});
    analysis::FlagOptions full;
    full.full_exit = {kA};
    const auto fl = analysis::flag_liveness(p, full);
    const auto& fa = p.functions.at(kA);
    CHECK(fl.at(kA)->live_out == analysis::kAllFlags);
    CHECK(fl.at(kA)->after[at(fa, 0x04)] == (kFlagC | kFlagV));  // (the movs writes N and Z again)
    CHECK(fl.at(kA)->after[at(fa, 0x06)] == analysis::kAllFlags);
    CHECK(fl.at(kDHelper)->live_out == (kFlagC | kFlagZ));

    analysis::FlagOptions everywhere;
    everywhere.everywhere = true;
    const auto all = analysis::flag_liveness(p, everywhere);
    CHECK(all.at(kA)->after[at(fa, 0x00)] == analysis::kAllFlags);
    CHECK(all.at(kDCaller)->before_call[1] == analysis::kAllFlags);
    CHECK(all.given_at_entry == analysis::kAllFlags);
    CHECK(all.given_at_return == analysis::kAllFlags);
}
