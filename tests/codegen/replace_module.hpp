#pragma once

// A synthetic module for host implementations of guest functions (psvrecomp --replace, recomp::Options::replacements):
// six functions replaced (four with a guard), and their callers by each way of reaching them. Hand-written Thumb
// (assembled with clang --target=thumbv7a; the listing below is the source), at kReplaceBase:
//
//   00 c_table     cmp r3, #2; bhs 1f; tbb [pc, r3]; .byte 1, 3 (case 0: movs r0, #0, case 1: rep_store); 1: bx lr
//   0e rep_store   str r1, [r0]; adds r0, r0, #4; movs r2, #0x77; bx lr
//                  (4 instructions, no table: an inline candidate; r2 is the AAPCS's to change: its host implementation
//                  leaves it as it was)
//   16 c_direct    push {r4, lr}; mov r4, r1; bl rep_store; adds r0, r0, r4; pop {r4, pc}
//   22 c_tail      adds r1, r1, #1; b.w rep_store
//   28 c_indirect  push {r4, lr}; blx r2; adds r0, r0, #1; pop {r4, pc}   (r2: a function's address; psv_call)
//   30 c_relies    push {r4, lr}; bl rep_store; adds r0, r0, r2; pop {r4, pc}
//                  (reads r2 after the call: what the guest's code left there; the generator counts and lists it)
//   3a rep_load    ldr r0, [r0]; bx lr   (replaced with a guard)
//   3e c_guarded   push {r4, lr}; mov r4, r0; bl rep_load; str r0, [r4, #4]; pop {r4, pc}
//   4a add3        adds r0, r0, #3; bx lr   (an internal entry inlined into its callers: rep_add3's guest code)
//   4e rep_add3    push {r4, lr}; bl add3; pop {r4, pc}   (replaced with a guard: fo_ calls add3's inlined copy)
//   56 c_add3      push {r4, lr}; bl rep_add3; adds r0, r0, #1; pop {r4, pc}
//   60 c_into      adds r0, #10   (runs into rep_inc)
//   62 rep_inc     adds r0, r0, #1; bx lr   (replaced)
//   66 c_ldrpc     ldr.w pc, 1f; nop; 1: .word rep_inc   (a jump to a constant: through the runtime, f_<address>)
//   70 rep_dbl     cbz r0, 2f; subs r0, r0, #1; adds r1, r1, #2; b rep_dbl; 2: mov r0, r1; bx lr
//                  (replaced with a guard: its guest code's branch to its own entry is a loop)
//   7c c_dbl       push {r4, lr}; bl rep_dbl; adds r0, r0, #5; pop {r4, pc}
//   86 rep_tri     push {r4, lr}; movs r4, r0; beq 3f; subs r0, r0, #1; bl rep_tri; adds r0, r0, r4; pop {r4, pc};
//                  3: movs r0, #0; pop {r4, pc}   (replaced with a guard: its guest code calls itself, through the guard)
//   9a c_tri       push {r4, lr}; mov r4, r1; bl rep_tri; adds r0, r0, r4; pop {r4, pc}
//   a6 f_flag      ite cs; movcs r0, #1; movcc r0, #0; bx lr   (reads C at entry: not a C function, never replaced)

#include "psvrecore/analysis/program.hpp"
#include "psvrecore/formats/elf.hpp"

namespace psvrecore::replace_test {

inline constexpr u32 kReplaceBase = 0x81000000;
inline constexpr u16 kReplaceCode[] = {
    0x2b02, 0xd203, 0xe8df, 0xf003, 0x0301, 0x2000, 0x4770,  // c_table
    0x6001, 0x1d00, 0x2277, 0x4770,                          // rep_store
    0xb510, 0x460c, 0xf7ff, 0xfff8, 0x1900, 0xbd10,          // c_direct
    0x1c49, 0xf7ff, 0xbff3,                                  // c_tail
    0xb510, 0x4790, 0x1c40, 0xbd10,                          // c_indirect
    0xb510, 0xf7ff, 0xffec, 0x1880, 0xbd10,                  // c_relies
    0x6800, 0x4770,                                          // rep_load
    0xb510, 0x4604, 0xf7ff, 0xfffa, 0x6060, 0xbd10,          // c_guarded
    0x1cc0, 0x4770,                                          // add3
    0xb510, 0xf7ff, 0xfffb, 0xbd10,                          // rep_add3
    0xb510, 0xf7ff, 0xfff9, 0x1c40, 0xbd10,                  // c_add3
    0x300a,                                                  // c_into
    0x1c40, 0x4770,                                          // rep_inc
    0xf8df, 0xf004, 0xbf00, 0x0063, 0x8100,                  // c_ldrpc
    0xb110, 0x1e40, 0x1c89, 0xe7fb, 0x4608, 0x4770,          // rep_dbl
    0xb510, 0xf7ff, 0xfff7, 0x1d40, 0xbd10,                  // c_dbl
    0xb510, 0x0004, 0xd004, 0x1e40, 0xf7ff, 0xfffa, 0x1900, 0xbd10, 0x2000, 0xbd10,  // rep_tri
    0xb510, 0x460c, 0xf7ff, 0xfff2, 0x1900, 0xbd10,          // c_tri
    0xbf2c, 0x2001, 0x2000, 0x4770,                          // f_flag
};
// Code addresses (Thumb).
inline constexpr u32 kTable = kReplaceBase + 0x00 + 1, kRepStore = kReplaceBase + 0x0E + 1, kDirect = kReplaceBase + 0x16 + 1,
                     kTail = kReplaceBase + 0x22 + 1, kIndirect = kReplaceBase + 0x28 + 1, kRelies = kReplaceBase + 0x30 + 1,
                     kRepLoad = kReplaceBase + 0x3A + 1, kGuarded = kReplaceBase + 0x3E + 1, kAdd3 = kReplaceBase + 0x4A + 1,
                     kRepAdd3 = kReplaceBase + 0x4E + 1, kCallAdd3 = kReplaceBase + 0x56 + 1, kInto = kReplaceBase + 0x60 + 1,
                     kRepInc = kReplaceBase + 0x62 + 1, kLdrPc = kReplaceBase + 0x66 + 1, kRepDbl = kReplaceBase + 0x70 + 1,
                     kCallDbl = kReplaceBase + 0x7C + 1, kRepTri = kReplaceBase + 0x86 + 1, kCallTri = kReplaceBase + 0x9A + 1,
                     kFlagFn = kReplaceBase + 0xA6 + 1;
inline constexpr u32 kReplaceFunctions[] = {kTable,   kRepStore, kDirect, kTail,   kIndirect, kRelies,  kRepLoad, kGuarded, kAdd3,  kRepAdd3,
                                            kCallAdd3, kInto,    kRepInc, kLdrPc,  kRepDbl,   kCallDbl, kRepTri,  kCallTri, kFlagFn};
// The host functions the execution test defines (replace_tests in psvrecore_host_functions_tests).
inline constexpr const char* kStoreHost = "test_host_store";
inline constexpr const char* kLoadHost = "test_host_load";
inline constexpr const char* kLoadGuard = "test_host_load_guard";
inline constexpr const char* kAdd3Host = "test_host_add3";
inline constexpr const char* kIncHost = "test_host_inc";
inline constexpr const char* kDblHost = "test_host_dbl";
inline constexpr const char* kTriHost = "test_host_tri";
inline constexpr const char* kOddGuard = "test_host_odd";  // (r0 odd: rep_add3's, rep_dbl's and rep_tri's guard)

// Every replacement replacegen makes (and replace_tests' generator checks).
struct ReplaceSpec {
    u32 code;
    const char* symbol;
    const char* guard;
};
inline constexpr ReplaceSpec kReplacements[] = {{kRepStore, kStoreHost, ""}, {kRepLoad, kLoadHost, kLoadGuard}, {kRepAdd3, kAdd3Host, kOddGuard},
                                                {kRepInc, kIncHost, ""},     {kRepDbl, kDblHost, kOddGuard},    {kRepTri, kTriHost, kOddGuard}};

inline analysis::Image replace_image() {
    analysis::Segment seg;
    seg.base = seg.linked = kReplaceBase;
    seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
    for (u16 h : kReplaceCode) {
        seg.bytes.push_back(static_cast<u8>(h));
        seg.bytes.push_back(static_cast<u8>(h >> 8));
    }
    seg.file_size = seg.size();
    return analysis::Image::from_segments({seg});
}

inline analysis::Program replace_program(const analysis::Image& image) {
    analysis::Options options;
    options.use_exidx = false;
    for (u32 f : kReplaceFunctions) options.extra_entries.push_back(f);
    return analysis::analyze(image, options);
}

}  // namespace psvrecore::replace_test
