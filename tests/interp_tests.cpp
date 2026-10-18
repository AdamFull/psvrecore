// The reference interpreter against hand-computed results: each expected value is worked out from the ARM ARM's
// pseudocode for the instruction (the reasoning is in the comments). These anchor the interpreter; the code
// generator is then checked against the interpreter (generator_tests.cpp).
//
// Encodings were produced by clang's integrated assembler (armv7a / thumbv7a, cortex-a9).

#include <cstring>

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/interp/interpreter.hpp"

using namespace psvrecore;

namespace {

struct Machine {
    PsvContext ctx;
    nx::vector<u8> memory = nx::vector<u8>(0x10000 + 16, 0);
    interp::Interpreter it{ctx};

    Machine() {
        std::memset(&ctx, 0, sizeof ctx);
        psv_membase = memory.data();
        it.address_mask = 0xFFFF;
    }
    ~Machine() { psv_membase = nullptr; }

    interp::Result arm(u32 word, u32 address = 0x1000) {
        it.thumb = false;
        it.pc = address;
        return it.execute(arm::decode_arm(word, address));
    }
    void put16(u32 address, u16 v) { std::memcpy(&memory[address], &v, 2); }
    void put32(u32 address, u32 v) { std::memcpy(&memory[address], &v, 4); }
    u32 get32(u32 address) const {
        u32 v;
        std::memcpy(&v, &memory[address], 4);
        return v;
    }
    void flags(u32 n, u32 z, u32 c, u32 v) { ctx.n = n, ctx.z = z, ctx.c = c, ctx.v = v; }
    bool has_flags(u32 n, u32 z, u32 c, u32 v) const { return ctx.n == n && ctx.z == z && ctx.c == c && ctx.v == v; }
};

}  // namespace

TEST_CASE("interp: add and subtract set NZCV as AddWithCarry defines") {
    Machine m;
    // adds r0, r1, r2: 0x7FFFFFFF + 1 overflows into the sign bit, no unsigned carry.
    m.ctx.r[1] = 0x7FFFFFFF, m.ctx.r[2] = 1;
    CHECK(m.arm(0xE0910002) == interp::Result::Next);
    CHECK(m.ctx.r[0] == 0x80000000u);
    CHECK(m.has_flags(1, 0, 0, 1));
    // 0xFFFFFFFF + 1: zero with a carry out, no overflow (-1 + 1).
    m.ctx.r[1] = 0xFFFFFFFF;
    m.arm(0xE0910002);
    CHECK(m.ctx.r[0] == 0);
    CHECK(m.has_flags(0, 1, 1, 0));
    // adcs r0, r1, r2 with C set: 0x7FFFFFFF + 0 + 1 overflows.
    m.ctx.r[1] = 0x7FFFFFFF, m.ctx.r[2] = 0, m.ctx.c = 1;
    m.arm(0xE0B10002);
    CHECK(m.ctx.r[0] == 0x80000000u);
    CHECK(m.has_flags(1, 0, 0, 1));
    // subs r0, r1, r2: 0 - 1 borrows (C clear: C is NOT borrow).
    m.ctx.r[1] = 0, m.ctx.r[2] = 1;
    m.arm(0xE0510002);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.has_flags(1, 0, 0, 0));
    // 0x80000000 - 1: overflows (most negative minus one), no borrow.
    m.ctx.r[1] = 0x80000000;
    m.arm(0xE0510002);
    CHECK(m.ctx.r[0] == 0x7FFFFFFFu);
    CHECK(m.has_flags(0, 0, 1, 1));
    // 5 - 5: zero, no borrow.
    m.ctx.r[1] = 5, m.ctx.r[2] = 5;
    m.arm(0xE0510002);
    CHECK(m.has_flags(0, 1, 1, 0));
    // sbcs r0, r1, r2 with C clear: 5 - 5 - 1 = -1, with a borrow.
    m.ctx.c = 0;
    m.arm(0xE0D10002);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.has_flags(1, 0, 0, 0));
    // rsbs r0, r1, #0: 0 - 0x80000000 overflows; 0 - 1 borrows; 0 - 0 sets Z and C.
    m.ctx.r[1] = 0x80000000;
    m.arm(0xE2710000);
    CHECK(m.ctx.r[0] == 0x80000000u);
    CHECK(m.has_flags(1, 0, 0, 1));
    m.ctx.r[1] = 1;
    m.arm(0xE2710000);
    CHECK(m.has_flags(1, 0, 0, 0));
    m.ctx.r[1] = 0;
    m.arm(0xE2710000);
    CHECK(m.has_flags(0, 1, 1, 0));
}

TEST_CASE("interp: shifts by a register use the bottom byte, with Shift_C's carry rules") {
    Machine m;
    // lsls r0, r1, r2
    m.ctx.r[1] = 0x80000001;
    m.ctx.r[2] = 1;
    m.arm(0xE1B00211);
    CHECK(m.ctx.r[0] == 2);
    CHECK(m.ctx.c == 1);  // bit 31 shifted out
    m.ctx.r[2] = 32;
    m.arm(0xE1B00211);
    CHECK(m.ctx.r[0] == 0);
    CHECK(m.ctx.c == 1);  // bit 0 is the last shifted out
    CHECK(m.ctx.z == 1);
    m.ctx.r[2] = 33;
    m.arm(0xE1B00211);
    CHECK(m.ctx.c == 0);
    m.ctx.r[2] = 0x100, m.ctx.c = 1;  // bottom byte 0: no shift, carry unchanged
    m.arm(0xE1B00211);
    CHECK(m.ctx.r[0] == 0x80000001u);
    CHECK(m.ctx.c == 1);
    // lsrs r0, r1, r2: by 1, by 32 (carry = bit 31), by 33 (zero, no carry)
    m.ctx.r[2] = 1;
    m.arm(0xE1B00231);
    CHECK(m.ctx.r[0] == 0x40000000u);
    CHECK(m.ctx.c == 1);
    m.ctx.r[2] = 32;
    m.arm(0xE1B00231);
    CHECK(m.ctx.r[0] == 0);
    CHECK(m.ctx.c == 1);
    m.ctx.r[2] = 33;
    m.arm(0xE1B00231);
    CHECK(m.ctx.c == 0);
    // asrs r0, r1, r2: 0x80000000 by 31 (carry = bit 30 = 0), by 32 and by 200 (all sign bits, carry = sign)
    m.ctx.r[1] = 0x80000000;
    m.ctx.r[2] = 31;
    m.arm(0xE1B00251);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.ctx.c == 0);
    m.ctx.r[2] = 32;
    m.arm(0xE1B00251);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.ctx.c == 1);
    m.ctx.r[2] = 200;
    m.arm(0xE1B00251);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.ctx.c == 1);
    m.ctx.r[1] = 0x40000000, m.ctx.r[2] = 40;
    m.arm(0xE1B00251);
    CHECK(m.ctx.r[0] == 0);
    CHECK(m.ctx.c == 0);
    // rors r0, r1, r2: by 1; by 32 (value unchanged, carry = bit 31); by 33 (as by 1)
    m.ctx.r[1] = 0x80000001, m.ctx.r[2] = 1;
    m.arm(0xE1B00271);
    CHECK(m.ctx.r[0] == 0xC0000000u);
    CHECK(m.ctx.c == 1);
    m.ctx.r[2] = 32, m.ctx.c = 0;
    m.arm(0xE1B00271);
    CHECK(m.ctx.r[0] == 0x80000001u);
    CHECK(m.ctx.c == 1);
    m.ctx.r[2] = 33;
    m.arm(0xE1B00271);
    CHECK(m.ctx.r[0] == 0xC0000000u);
    // rrxs r0, r1: the carry enters at the top, bit 0 leaves into the carry.
    m.ctx.r[1] = 2, m.ctx.c = 1;
    m.arm(0xE1B00061);
    CHECK(m.ctx.r[0] == 0x80000001u);
    CHECK(m.ctx.c == 0);
}

TEST_CASE("interp: logical operations take the carry of a rotated immediate") {
    Machine m;
    // ands r0, r1, #0xFF000000: the immediate is rotated, so C becomes its bit 31 (1).
    m.ctx.r[1] = 0x12345678, m.ctx.c = 0;
    m.arm(0xE21104FF);
    CHECK(m.ctx.r[0] == 0x12000000u);
    CHECK(m.ctx.c == 1);
    CHECK(m.ctx.n == 0);
    // Thumb ands.w r0, r1, #0x00FF00FF: a replicated pattern, not rotated: C unchanged.
    interp::Interpreter& it = m.it;
    m.ctx.c = 0;
    it.thumb = true;
    it.pc = 0x2000;
    arm::ItState s;
    CHECK(it.execute(arm::decode_thumb(0xF011, 0x10FF, 0x2000, s)) == interp::Result::Next);
    CHECK(m.ctx.r[0] == 0x00340078u);
    CHECK(m.ctx.c == 0);
    // bics.w r0, r1, #0x80000000: rotated: C = 1.
    it.pc = 0x2004;
    it.execute(arm::decode_thumb(0xF031, 0x4000, 0x2004, s));
    CHECK(m.ctx.r[0] == 0x12345678u);
    CHECK(m.ctx.c == 1);
}

TEST_CASE("interp: saturation sets Q") {
    Machine m;
    // qadd r0, r1, r2 (Rd = Rm + Rn, saturated): 0x7FFFFFFF + 1.
    m.ctx.r[1] = 0x7FFFFFFF, m.ctx.r[2] = 1;
    m.arm(0xE1020051);
    CHECK(m.ctx.r[0] == 0x7FFFFFFFu);
    CHECK(m.ctx.q == 1);
    // qdsub r0, r1, r2: Rm - sat(2 * Rn): 2 * 0x40000000 saturates to 0x7FFFFFFF; 0 - that = 0x80000001.
    m.ctx.q = 0, m.ctx.r[1] = 0, m.ctx.r[2] = 0x40000000;
    m.arm(0xE1620051);
    CHECK(m.ctx.r[0] == 0x80000001u);
    CHECK(m.ctx.q == 1);
    // ssat r0, #8, r1: 200 -> 127, -200 -> -128, 100 stays (and leaves Q alone).
    m.ctx.q = 0, m.ctx.r[1] = 200;
    m.arm(0xE6A70011);
    CHECK(m.ctx.r[0] == 127);
    CHECK(m.ctx.q == 1);
    m.ctx.r[1] = static_cast<u32>(-200);
    m.arm(0xE6A70011);
    CHECK(m.ctx.r[0] == static_cast<u32>(-128));
    m.ctx.q = 0, m.ctx.r[1] = 100;
    m.arm(0xE6A70011);
    CHECK(m.ctx.r[0] == 100);
    CHECK(m.ctx.q == 0);
    // usat r0, #8, r1: 300 -> 255, -5 -> 0.
    m.ctx.r[1] = 300;
    m.arm(0xE6E80011);
    CHECK(m.ctx.r[0] == 255);
    CHECK(m.ctx.q == 1);
    m.ctx.r[1] = static_cast<u32>(-5);
    m.arm(0xE6E80011);
    CHECK(m.ctx.r[0] == 0);
}

TEST_CASE("interp: parallel arithmetic, GE and sel") {
    Machine m;
    // uadd8 r0, r1, r2: bytes 02+03, 01+02, FF+01 (carries: GE2), 80+80 (carries: GE3).
    m.ctx.r[1] = 0x80FF0102, m.ctx.r[2] = 0x80010203;
    m.arm(0xE6510F92);
    CHECK(m.ctx.r[0] == 0x00000305u);
    CHECK(m.ctx.ge == 0xC);
    // sel r0, r1, r2 with GE 0101: bytes 0 and 2 from r1, 1 and 3 from r2.
    m.ctx.ge = 0x5, m.ctx.r[1] = 0x11223344, m.ctx.r[2] = 0xAABBCCDD;
    m.arm(0xE6810FB2);
    CHECK(m.ctx.r[0] == 0xAA22CC44u);
    // usad8 r0, r1, r2: |1-4| + |2-3| + |3-2| + |4-1| = 8.
    m.ctx.r[1] = 0x01020304, m.ctx.r[2] = 0x04030201;
    m.arm(0xE780F211);
    CHECK(m.ctx.r[0] == 8);
    // sxtab16 r0, r1, r2, ror #8: r2 rotated is 0x0180FF7F; bytes 0 and 2 (0x7F, 0x80) sign-extended, added to
    // r1's halves: 0x0001 + 0x007F, 0x0001 + 0xFF80.
    m.ctx.r[1] = 0x00010001, m.ctx.r[2] = 0x80FF7F01;
    m.arm(0xE6810472);
    CHECK(m.ctx.r[0] == 0xFF810080u);
}

TEST_CASE("interp: multiplies") {
    Machine m;
    // smlabb r0, r1, r2, r3: -32768 * -32768 = 0x40000000, plus 0x40000000 overflows 32 bits: Q.
    m.ctx.r[1] = 0x8000, m.ctx.r[2] = 0x8000, m.ctx.r[3] = 0x40000000;
    m.arm(0xE1003281);
    CHECK(m.ctx.r[0] == 0x80000000u);
    CHECK(m.ctx.q == 1);
    m.ctx.q = 0, m.ctx.r[3] = 0;
    m.arm(0xE1003281);
    CHECK(m.ctx.r[0] == 0x40000000u);
    CHECK(m.ctx.q == 0);
    // smmulr r0, r1, r2: 0x40000000 * 3 = 0xC0000000; rounding adds 0x80000000: the top word becomes 1.
    m.ctx.r[1] = 0x40000000, m.ctx.r[2] = 3;
    m.arm(0xE750F231);
    CHECK(m.ctx.r[0] == 1);
    // -1 * 1 = -1 (all ones); rounding carries into the top word: 0.
    m.ctx.r[1] = 0xFFFFFFFF, m.ctx.r[2] = 1;
    m.arm(0xE750F231);
    CHECK(m.ctx.r[0] == 0);
    // umaal r0, r1, r2, r3 (RdLo, RdHi): (2^32-1)^2 + 2 (2^32-1) = 2^64 - 1, which just fits.
    m.ctx.r[0] = m.ctx.r[1] = m.ctx.r[2] = m.ctx.r[3] = 0xFFFFFFFF;
    m.arm(0xE0410392);
    CHECK(m.ctx.r[0] == 0xFFFFFFFFu);
    CHECK(m.ctx.r[1] == 0xFFFFFFFFu);
}

TEST_CASE("interp: bit fields and byte reversal") {
    Machine m;
    // bfi r0, r1, #4, #8
    m.ctx.r[0] = 0xFFFFFFFF, m.ctx.r[1] = 0;
    m.arm(0xE7CB0211);
    CHECK(m.ctx.r[0] == 0xFFFFF00Fu);
    m.ctx.r[0] = 0, m.ctx.r[1] = 0x1AB;
    m.arm(0xE7CB0211);
    CHECK(m.ctx.r[0] == 0x00000AB0u);
    // sbfx r0, r1, #4, #8: field 0xF8, sign-extended.
    m.ctx.r[1] = 0x00000F80;
    m.arm(0xE7A70251);
    CHECK(m.ctx.r[0] == 0xFFFFFFF8u);
    // rev16 r0, r1; revsh r0, r1 (the low byte, sign-extended, becomes the high part).
    m.ctx.r[1] = 0x11223344;
    m.arm(0xE6BF0FB1);
    CHECK(m.ctx.r[0] == 0x22114433u);
    m.ctx.r[1] = 0x000080FF;
    m.arm(0xE6FF0FB1);
    CHECK(m.ctx.r[0] == 0xFFFFFF80u);
}

TEST_CASE("interp: loads, stores and the exclusive monitor") {
    Machine m;
    // ldrd r0, r1, [r2, #8]!
    m.ctx.r[2] = 0x100;
    m.put32(0x108, 0x11111111);
    m.put32(0x10C, 0x22222222);
    m.arm(0xE1E200D8);
    CHECK(m.ctx.r[0] == 0x11111111u);
    CHECK(m.ctx.r[1] == 0x22222222u);
    CHECK(m.ctx.r[2] == 0x108);
    // ldmdb r0!, {r1, r2, r3}: from r0 - 12 upwards; r0 written back to r0 - 12.
    m.ctx.r[0] = 0x200;
    m.put32(0x1F4, 1);
    m.put32(0x1F8, 2);
    m.put32(0x1FC, 3);
    m.arm(0xE930000E);
    CHECK(m.ctx.r[1] == 1);
    CHECK(m.ctx.r[2] == 2);
    CHECK(m.ctx.r[3] == 3);
    CHECK(m.ctx.r[0] == 0x1F4);
    // ldrex r0, [r1]; strex r2, r0, [r1] succeeds (0); a second strex without ldrex fails (1).
    m.ctx.r[1] = 0x300;
    m.put32(0x300, 0x55);
    m.arm(0xE1910F9F);
    CHECK(m.ctx.r[0] == 0x55);
    m.ctx.r[0] = 0x66;
    m.arm(0xE1812F90);
    CHECK(m.ctx.r[2] == 0);
    CHECK(m.get32(0x300) == 0x66);
    m.ctx.r[0] = 0x77;
    m.arm(0xE1812F90);
    CHECK(m.ctx.r[2] == 1);
    CHECK(m.get32(0x300) == 0x66);
}

TEST_CASE("interp: Thumb IT blocks, table branches and PC-relative values") {
    Machine m;
    // ite eq / moveq r0, #1 / movne r0, #2, from memory.
    m.put16(0x0, 0xBF0C);
    m.put16(0x2, 0x2001);
    m.put16(0x4, 0x2002);
    m.it.thumb = true;
    m.it.pc = 0;
    m.ctx.z = 1;
    for (int i = 0; i < 3; ++i) CHECK(m.it.step() == interp::Result::Next);
    CHECK(m.ctx.r[0] == 1);
    m.it.pc = 0;
    m.ctx.z = 0;
    for (int i = 0; i < 3; ++i) m.it.step();
    CHECK(m.ctx.r[0] == 2);
    CHECK(!m.it.it.active());
    // tbb [pc, r1] at 6: the table at 0xA holds 2, 4: index 0 goes to 0xA + 4, index 1 to 0xA + 8.
    m.put16(0x6, 0xE8DF);
    m.put16(0x8, 0xF001);
    m.memory[0xA] = 2;
    m.memory[0xB] = 4;
    m.it.pc = 6;
    m.ctx.r[1] = 0;
    CHECK(m.it.step() == interp::Result::Branched);
    CHECK(m.it.pc == 0xE);
    m.it.pc = 6;
    m.ctx.r[1] = 1;
    m.it.step();
    CHECK(m.it.pc == 0x12);
    CHECK(m.it.thumb);
    // add r0, pc at 0x14: the PC reads 0x18. adr r0 at 0x16: Align(0x1A, 4) + 4 = 0x1C.
    m.put16(0x14, 0x4478);
    m.put16(0x16, 0xA001);
    m.it.pc = 0x14;
    m.ctx.r[0] = 0x100;
    m.it.step();
    CHECK(m.ctx.r[0] == 0x118);
    m.it.step();
    CHECK(m.ctx.r[0] == 0x1C);
    // muls r0, r1, r0 (16-bit, outside an IT block: sets N and Z)
    m.put16(0x24, 0x4348);
    m.it.pc = 0x24;
    m.ctx.r[0] = 0x10000, m.ctx.r[1] = 0x10000;
    m.it.step();
    CHECK(m.ctx.r[0] == 0);
    CHECK(m.ctx.z == 1);
}

TEST_CASE("interp: branches and interworking") {
    Machine m;
    // bx r0 to a Thumb address; blx to ARM from Thumb sets LR with bit 0.
    m.ctx.r[0] = 0x2001;
    m.arm(0xE12FFF10);  // bx r0
    CHECK(m.it.thumb);
    CHECK(m.it.pc == 0x2000);
    // blx r1 (Thumb, 16-bit) at 0x2000: LR = 0x2003, target ARM 0x3000.
    m.ctx.r[1] = 0x3000;
    arm::ItState s;
    m.it.pc = 0x2000;
    CHECK(m.it.execute(arm::decode_thumb(0x4788, 0, 0x2000, s)) == interp::Result::Branched);
    CHECK(m.ctx.r[14] == 0x2003);
    CHECK(!m.it.thumb);
    CHECK(m.it.pc == 0x3000);
}
