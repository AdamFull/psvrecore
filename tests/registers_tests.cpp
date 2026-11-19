// psvrecore::analysis: the core registers in locals (registers.hpp) on small functions assembled by LLVM's
// assembler (the Thumb halfwords in each comment's order): what each instruction reads and writes, what a function
// reads at entry, what is dirty at its exits and calls, what it preserves for its callers, what a caller keeps across
// a call; and the load-time check of the modules' condition-flag contracts (recomp::register_module).

#include <initializer_list>

#include "nxtest.h"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/analysis/registers.hpp"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/formats/elf.hpp"
#include "psvrecore/recomp/support.hpp"

using namespace psvrecore;
using analysis::RegSet;

namespace {

constexpr u32 kBase = 0x81000000;

constexpr RegSet r(std::initializer_list<u32> regs) {
    RegSet s = 0;
    for (u32 k : regs) s |= static_cast<RegSet>(1u << k);
    return s;
}
constexpr RegSet kSp = analysis::kSpRegister, kLr = analysis::kLrRegister;

// The functions, at kBase (Thumb):
//   a_leaf      00 adds r0, r0, r1; bx lr
//   b_keep      04 push {r4, lr}; mov r4, r0; adds r0, r4, #1; pop {r4, pc}
//   c_clobber   0c adds r4, r0, #1; bx lr
//   d_caller    10 push {r4, r5, lr}; mov r4, r1; mov r5, r2; bl b_keep; bl c_clobber; add r0, r4; add r0, r5;
//                  pop {r4, r5, pc}
//   e_elsewhere 24 push {r4, lr}; str r0, [sp, #4]; pop {r4, pc}         (returns to r0, not to its caller)
//   f_indirect  2a push {r4, lr}; mov r4, r0; blx r1; add r0, r4; pop {r4, pc}
//   g_calls_f   34 push {r4, lr}; mov r4, r2; bl f_indirect; add r0, r4; pop {r4, pc}
//   h_cond      40 push {r6, lr}; cmp r0, r1; it hi; movhi r6, r0; blx r2; add r0, r6; pop {r6, pc}
//   i_recurse   4e push {r4, lr}; movs r4, r0; beq 1f; subs r0, #1; bl i_recurse; add r0, r4; 1: pop {r4, pc}
//   j_loop      5e push {r4, r5, lr}; movs r5, #3; 1: bl b_keep; add r4, r0; subs r5, #1; bne 1b; bl b_keep;
//                  add r0, r4; pop {r4, r5, pc}
constexpr u16 kCode[] = {
    0x1840, 0x4770,                                                          // a_leaf
    0xB510, 0x4604, 0x1C60, 0xBD10,                                          // b_keep
    0x1C44, 0x4770,                                                          // c_clobber
    0xB530, 0x460C, 0x4615, 0xF7FF, 0xFFF5, 0xF7FF, 0xFFF7, 0x4420, 0x4428, 0xBD30,  // d_caller
    0xB510, 0x9001, 0xBD10,                                                  // e_elsewhere
    0xB510, 0x4604, 0x4788, 0x4420, 0xBD10,                                  // f_indirect
    0xB510, 0x4614, 0xF7FF, 0xFFF7, 0x4420, 0xBD10,                          // g_calls_f
    0xB540, 0x4288, 0xBF88, 0x4606, 0x4790, 0x4430, 0xBD40,                  // h_cond
    0xB510, 0x0004, 0xD003, 0x3801, 0xF7FF, 0xFFFA, 0x4420, 0xBD10,          // i_recurse
    0xB530, 0x2503, 0xF7FF, 0xFFCF, 0x4404, 0x3D01, 0xD1FA, 0xF7FF, 0xFFCA, 0x4420, 0xBD30,  // j_loop
};
constexpr u32 kA = kBase + 0x00 + 1, kB = kBase + 0x04 + 1, kC = kBase + 0x0C + 1, kD = kBase + 0x10 + 1, kE = kBase + 0x24 + 1,
              kF = kBase + 0x2A + 1, kG = kBase + 0x34 + 1, kH = kBase + 0x40 + 1, kI = kBase + 0x4E + 1, kJ = kBase + 0x5E + 1;

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

analysis::Program discover(const analysis::Image& image) {
    analysis::Options options;
    options.extra_entries = {kA, kB, kC, kD, kE, kF, kG, kH, kI, kJ};
    options.use_exidx = false;
    return analysis::analyze(image, options);
}

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

TEST_CASE("analysis: the core registers each instruction reads and writes") {
    using analysis::register_effect;
    const auto push = register_effect(thumb(0xB530));  // push {r4, r5, lr}
    CHECK(push.uses == (r({4, 5}) | kSp | kLr));
    CHECK(push.defs == kSp);
    const auto pop = register_effect(thumb(0xBD30));  // pop {r4, r5, pc}
    CHECK(pop.uses == kSp);
    CHECK(pop.defs == (r({4, 5}) | kSp));
    CHECK(register_effect(thumb(0xF7FF, 0xFFF5)).defs == kLr);  // bl
    const auto blx = register_effect(thumb(0x4788));          // blx r1
    CHECK(blx.uses == r({1}));
    CHECK(blx.defs == kLr);
    const auto str = register_effect(thumb(0x9001));  // str r0, [sp, #4]
    CHECK(str.uses == (r({0}) | kSp));
    CHECK(str.defs == 0);
    const auto ldr_post = register_effect(arm::decode_arm(0xE4910004, kBase));  // ldr r0, [r1], #4
    CHECK(ldr_post.uses == r({1}));
    CHECK(ldr_post.defs == r({0, 1}));
    const auto umlal = register_effect(arm::decode_arm(0xE0A10392, kBase));  // umlal r0, r1, r2, r3
    CHECK(umlal.uses == r({0, 1, 2, 3}));
    CHECK(umlal.defs == r({0, 1}));
    const auto movt = register_effect(thumb(0xF2C1, 0x2034));  // movt r0, #0x1234
    CHECK(movt.uses == r({0}));
    CHECK(movt.defs == r({0}));
    const auto strd = register_effect(arm::decode_arm(0xE1E120F8, kBase));  // strd r2, r3, [r1, #8]!
    CHECK(strd.uses == r({1, 2, 3}));
    CHECK(strd.defs == r({1}));
    const auto strex = register_effect(arm::decode_arm(0xE1832F91, kBase));  // strex r2, r1, [r3]
    CHECK(strex.uses == r({1, 3}));
    CHECK(strex.defs == r({2}));
    const auto vpush = register_effect(thumb(0xED2D, 0x8B02));  // vpush {d8}
    CHECK(vpush.uses == kSp);
    CHECK(vpush.defs == kSp);
    const auto vmov = register_effect(thumb(0xEC51, 0x0B18));  // vmov r0, r1, d8
    CHECK(vmov.defs == r({0, 1}));
    CHECK(vmov.uses == 0);
    CHECK(register_effect(thumb(0xB110)).uses == r({0}));  // cbz r0
    CHECK(register_effect(thumb(0xDE00)).traps);           // udf #0
    CHECK(analysis::register_names(r({0, 1, 2, 3, 5}) | kSp) == "r0-r3 r5 sp");
}

TEST_CASE("analysis: registers read at entry, dirty at exits, kept across calls") {
    const auto img = image();
    const auto p = discover(img);
    REQUIRE(p.functions.size() == 10);
    const auto regs = analysis::register_analysis(p);
    const auto& A = *regs.at(kA);
    const auto& fa = *p.function_at(kA);
    // A leaf: reads r0, r1 (and lr, its return's target); writes r0 back.
    CHECK(A.load_at_entry == (r({0, 1}) | kLr));
    CHECK(A.exit_store[at(fa, 0x02)] == r({0}));
    CHECK(A.preserved == analysis::kPreservable);
    // push/pop: r4 and sp are what they were at the return, and need not be written (the context still holds them).
    const auto& B = *regs.at(kB);
    const auto& fb = *p.function_at(kB);
    CHECK(B.preserved == analysis::kPreservable);
    CHECK(B.exit_store[at(fb, 0x0A)] == r({0}));
    CHECK(B.load_at_entry == (r({0, 4}) | kSp | kLr));
    // A function that changes r4 without saving it does not preserve it.
    CHECK(regs.at(kC)->preserved == (analysis::kPreservable & ~r({4})));
    // The caller keeps r5 and sp across b_keep (r4 too, but it is dead until c_clobber's return reads it back);
    // after c_clobber r4 is read back, r5 and sp kept.
    const auto& D = *regs.at(kD);
    const auto& fd = *p.function_at(kD);
    const usize call_b = at(fd, 0x16), call_c = at(fd, 0x1A);
    CHECK(D.call_kept[call_b] == (r({5}) | kSp));
    CHECK((D.call_load[call_b] & (r({4, 5}) | kSp)) == 0);
    CHECK(D.call_kept[call_c] == (r({5}) | kSp));
    CHECK(D.call_store[call_b] == (r({4, 5}) | kSp | kLr));  // (r4, r5 and sp changed, lr set by bl)
    CHECK((D.call_kept[call_c] & r({4})) == 0);
    CHECK((D.call_kept[call_c] & r({5})) != 0);
    CHECK((D.call_load[call_c] & r({0, 4})) == r({0, 4}));
    CHECK(D.preserved == analysis::kPreservable);
    // A return to an address other than the LR it was given: it preserves nothing.
    CHECK(regs.at(kE)->preserved == 0);
    // An indirect call: nothing kept but sp, so the function preserves only what it saves itself (r4, and sp moved
    // back); one others rely on checks sp after the call.
    const auto& F = *regs.at(kF);
    const auto& ff = *p.function_at(kF);
    CHECK(F.preserved == (r({4}) | kSp));
    CHECK(F.checks_sp);
    CHECK(F.call_kept[at(ff, 0x2E)] == kSp);
    CHECK((F.call_load[at(ff, 0x2E)] & r({0, 4})) == r({0, 4}));
    CHECK(regs.at(kG)->call_kept[at(*p.function_at(kG), 0x38)] == (r({4}) | kSp));
    CHECK(!regs.at(kG)->checks_sp);  // (no direct caller relies on g)
    // A register written under a condition and stored at the call: read at entry for the path that does not write it.
    const auto& H = *regs.at(kH);
    CHECK((H.load_at_entry & r({6})) != 0);
    CHECK((H.call_store[at(*p.function_at(kH), 0x48)] & r({6})) != 0);
    // Recursion: it keeps r4 across its own call (the greatest fixed point).
    const auto& I = *regs.at(kI);
    CHECK(I.preserved == analysis::kPreservable);
    CHECK((I.call_kept[at(*p.function_at(kI), 0x56)] & r({4})) != 0);

    // Kept only inside loops: the loop's call keeps r4, r5 and sp; the call after the loop reads them back (the
    // registers are known the same either way: the function still preserves them).
    const auto& fj = *p.function_at(kJ);
    const usize in_loop = at(fj, 0x62), after_loop = at(fj, 0x6C);
    CHECK((regs.at(kJ)->call_kept[in_loop] & (r({4, 5}) | kSp)) == (r({4, 5}) | kSp));
    CHECK((regs.at(kJ)->call_kept[after_loop] & (r({4}) | kSp)) == (r({4}) | kSp));
    analysis::RegisterOptions loops;
    loops.keep = analysis::KeepPolicy::Loops;
    const auto in_loops = analysis::register_analysis(p, loops);
    CHECK((in_loops.at(kJ)->call_kept[in_loop] & (r({4, 5}) | kSp)) == (r({4, 5}) | kSp));
    CHECK(in_loops.at(kJ)->call_kept[after_loop] == 0);
    CHECK((in_loops.at(kJ)->call_load[after_loop] & (r({4}) | kSp)) == (r({4}) | kSp));
    CHECK(in_loops.at(kJ)->preserved == analysis::kPreservable);
    CHECK(in_loops.at(kD)->call_kept[call_b] == 0);

    // Without callee-saved knowledge: nothing kept, everything live read back.
    analysis::RegisterOptions off;
    off.callee_saved = false;
    const auto conservative = analysis::register_analysis(p, off);
    CHECK(conservative.at(kD)->call_kept[call_b] == 0);
    // (each register is read back after the last call before its use)
    CHECK((conservative.at(kD)->call_load[call_c] & (r({0, 4, 5}) | kSp)) == (r({0, 4, 5}) | kSp));
    CHECK(!conservative.at(kF)->checks_sp);
    CHECK(conservative.at(kB)->preserved == 0);
}

TEST_CASE("recomp: modules whose condition-flag contracts do not fit are refused") {
    static const PsvFunctionEntry functions[] = {{0x81000001u, nullptr}};
    PsvModuleCode reader{};
    reader.name = "reader";
    reader.functions = functions;
    reader.flag_contract = PSV_FLAG_CONTRACT;
    reader.flags_needed_at_entry = 4;    // z read at entry
    reader.flags_needed_at_return = 2;   // c read after an indirect call
    reader.flags_given_at_entry = 4;     // (a module gives what it reads itself, as the generator makes it)
    reader.flags_given_at_return = 2;
    PsvModuleCode provider{};
    provider.name = "provider";
    provider.flag_contract = PSV_FLAG_CONTRACT;
    provider.flags_given_at_entry = 0;   // gives nothing
    provider.flags_given_at_return = 2;  // c at its returns
    recomp::clear_modules();
    recomp::register_module(provider);
    CHECK(recomp::flag_contract_problems().empty());
    recomp::register_module(reader);
    const nx::string problems = recomp::flag_contract_problems();
    CHECK(problems.find("reader's functions read z at entry") != nx::string::npos);
    CHECK(problems.find("--flags-given-at-entry z") != nx::string::npos);
    CHECK(problems.find("read after its indirect calls") == nx::string::npos);  // (c is given)
    // A module generated before the contract takes every flag as read, and gives every flag.
    recomp::clear_modules();
    PsvModuleCode old{};
    old.name = "old";
    recomp::register_module(old);
    recomp::register_module(provider);
    CHECK(recomp::flag_contract_problems().find("old's functions read nzcv at entry") != nx::string::npos);
    recomp::clear_modules();
    PsvModuleCode full{};
    full.name = "full";
    full.flag_contract = PSV_FLAG_CONTRACT;
    full.flags_given_at_entry = full.flags_given_at_return = 15;
    recomp::register_module(old);
    recomp::register_module(full);
    recomp::register_module(reader);  // (old gives it all; but reader gives old nothing)
    CHECK(recomp::flag_contract_problems().find("reader's functions read") == nx::string::npos);
    CHECK(recomp::flag_contract_problems().find("reader reads") == nx::string::npos);
    CHECK(recomp::flag_contract_problems().find("old's functions read ncv at entry, but reader's") != nx::string::npos);
    recomp::clear_modules();
    recomp::register_module(old);
    recomp::register_module(full);
    CHECK(recomp::flag_contract_problems().empty());
    recomp::clear_modules();
}
