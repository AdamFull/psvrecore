#pragma once

// A synthetic module of guest threads that wait for each other through memory alone, no kernel call (spingen
// translates it at build time; the runtime test "generated code waiting in a loop sees a store of another thread"
// runs it). Each function is a thread entry: r0 the argument block's size, r1 its address (on the thread's stack).
// Hand-written Thumb (assembled with clang --target=thumbv7a; the listing below is the source), at kSpinBase:
//
//   00 spin_flag   ldr r0, [r1]; 1: ldr r2, [r0]; cmp r2, #0; beq 1b; mov r0, r2; bx lr
//                  (waits until the word at args[0] is not 0 and ends with it)
//   0c set_flag    ldr r0, [r1]; ldr r2, [r1, #4]; str r2, [r0]; movs r0, #0; bx lr
//                  (stores args[1] at args[0])
//   16 spin_stack  push {r4, lr}; sub sp, #8; ldr r3, [r1]; movs r2, #0; str r2, [sp]; mov r2, sp; str r2, [r3];
//                  1: ldr r2, [sp]; cmp r2, #0; beq 1b; mov r0, r2; add sp, #8; pop {r4, pc}
//                  (a word of its own stack frame, its address published at args[0], waited on: a stack slot that
//                  escapes to another thread)
//   30 set_posted  ldr r3, [r1]; ldr r2, [r1, #4]; 1: ldr r0, [r3]; cmp r0, #0; beq 1b; str r2, [r0]; movs r0, #0;
//                  bx lr   (waits for an address at args[0], then stores args[1] there)
//   40 mp_write    ldr r0, [r1]; ldr r3, [r1, #4]; ldr r2, [r1, #8]; str r2, [r0]; dmb ish; movs r2, #1; str r2, [r3];
//                  movs r0, #0; bx lr   (message passing: the data args[2] at args[0], a barrier, the flag args[1])
//   54 mp_read     ldr r0, [r1]; ldr r3, [r1, #4]; 1: ldr r2, [r3]; cmp r2, #0; beq 1b; dmb ish; ldr r0, [r0]; bx lr
//                  (waits for the flag, a barrier, ends with the data)

#include "psvrecore/analysis/program.hpp"
#include "psvrecore/formats/elf.hpp"

namespace psvrecore::spin_test {

inline constexpr u32 kSpinBase = 0x81100000;
inline constexpr u16 kSpinCode[] = {
    0x6808, 0x6802, 0x2a00, 0xd0fc, 0x4610, 0x4770,                                  // spin_flag
    0x6808, 0x684a, 0x6002, 0x2000, 0x4770,                                          // set_flag
    0xb510, 0xb082, 0x680b, 0x2200, 0x9200, 0x466a, 0x601a,                          // spin_stack
    0x9a00, 0x2a00, 0xd0fc, 0x4610, 0xb002, 0xbd10,                                  //
    0x680b, 0x684a, 0x6818, 0x2800, 0xd0fc, 0x6002, 0x2000, 0x4770,                  // set_posted
    0x6808, 0x684b, 0x688a, 0x6002, 0xf3bf, 0x8f5b, 0x2201, 0x601a, 0x2000, 0x4770,  // mp_write
    0x6808, 0x684b, 0x681a, 0x2a00, 0xd0fc, 0xf3bf, 0x8f5b, 0x6800, 0x4770,          // mp_read
};
// Code addresses (Thumb).
inline constexpr u32 kSpinFlag = kSpinBase + 0x00 + 1, kSetFlag = kSpinBase + 0x0C + 1, kSpinStack = kSpinBase + 0x16 + 1,
                     kSetPosted = kSpinBase + 0x30 + 1, kMpWrite = kSpinBase + 0x40 + 1, kMpRead = kSpinBase + 0x54 + 1;
inline constexpr u32 kSpinFunctions[] = {kSpinFlag, kSetFlag, kSpinStack, kSetPosted, kMpWrite, kMpRead};

inline analysis::Image spin_image() {
    analysis::Segment seg;
    seg.base = seg.linked = kSpinBase;
    seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
    for (u16 h : kSpinCode) {
        seg.bytes.push_back(static_cast<u8>(h));
        seg.bytes.push_back(static_cast<u8>(h >> 8));
    }
    seg.file_size = seg.size();
    return analysis::Image::from_segments({seg});
}

inline analysis::Program spin_program(const analysis::Image& image) {
    analysis::Options options;
    options.use_exidx = false;
    for (u32 f : kSpinFunctions) options.extra_entries.push_back(f);
    return analysis::analyze(image, options);
}

}  // namespace psvrecore::spin_test
