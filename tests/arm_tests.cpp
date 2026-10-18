// psvrecore::arm: decoding into the instruction model (the parts the recompiler relies on) and formatting.
// Encodings were produced by clang's integrated assembler for the instruction in each comment. The decoder's
// agreement with llvm-objdump over whole encoding spaces is checked separately (scripts/arm_check.py).

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/arm/format.hpp"

using namespace psvrecore;
using namespace psvrecore::arm;

namespace {

Instruction thumb(u32 encoding, u32 address = 0x1000) {
    ItState it;
    return decode_thumb(static_cast<u16>(encoding >> 16), static_cast<u16>(encoding), address, it);
}
Instruction thumb16(u16 hw, u32 address = 0x1000) {
    ItState it;
    return decode_thumb(hw, 0, address, it);
}

bool same_operands(const Instruction& a, const Instruction& b) {
    if (a.op != b.op || a.operand_count != b.operand_count || a.dt != b.dt || a.dt2 != b.dt2) return false;
    for (u8 i = 0; i < a.operand_count; ++i) {
        const Operand &x = a.operands[i], &y = b.operands[i];
        if (x.kind != y.kind || x.reg != y.reg || x.reg2 != y.reg2 || x.shift != y.shift || x.amount != y.amount ||
            x.flags != y.flags || x.count != y.count || x.index != y.index || x.imm != y.imm)
            return false;
    }
    return true;
}

}  // namespace

TEST_CASE("arm: ARM and Thumb encodings of one operation decode alike") {
    // add r0, r1, r2 / add.w r0, r1, r2
    const Instruction a = decode_arm(0xE0810002, 0x1000), t = thumb(0xEB010002);
    REQUIRE(a.valid());
    CHECK(a.op == Op::Add);
    CHECK(same_operands(a, t));
    CHECK(a.size == 4);
    CHECK(t.size == 4);
    // adds r3, r4, r5, lsl #3 / adds.w r3, r4, r5, lsl #3
    const Instruction b = decode_arm(0xE0943185, 0x1000), u = thumb(0xEB1403C5);
    CHECK(same_operands(b, u));
    CHECK((b.flags & kSetsFlags) != 0);
    CHECK((u.flags & kSetsFlags) != 0);
    REQUIRE(b.operand_count == 3);
    CHECK(b.operands[2].kind == OperandKind::RegShiftImm);
    CHECK(b.operands[2].shift == ShiftType::Lsl);
    CHECK(b.operands[2].amount == 3);
    // ldr r0, [r1, #-8]!
    CHECK(same_operands(decode_arm(0xE5310008, 0), thumb(0xF8510D08)));
    // vadd.f32 s0, s1, s2 (the same encoding in both instruction sets)
    const Instruction v = decode_arm(0xEE300A81, 0);
    CHECK(v.op == Op::Vadd);
    CHECK(v.dt == Dt::F32);
    CHECK(same_operands(v, thumb(0xEE300A81)));
    REQUIRE(v.operand_count == 3);
    CHECK(v.operands[1].kind == OperandKind::SReg);
    CHECK(v.operands[1].reg == 1);
    // vadd.i32 q0, q1, q2 (ARM 1111 0010 ..., Thumb 1110 1111 ...)
    const Instruction q = decode_arm(0xF2220844, 0);
    CHECK(q.op == Op::Vadd);
    CHECK(q.dt == Dt::I32);
    CHECK(q.operands[2].kind == OperandKind::QReg);
    CHECK(q.operands[2].reg == 2);
    CHECK(same_operands(q, thumb(0xEF220844)));
    // movw / movt
    CHECK(same_operands(decode_arm(0xE3011234, 0), thumb(0xF2412134)));
    const Instruction movt = decode_arm(0xE3481105, 0);
    CHECK(movt.op == Op::Movt);
    CHECK(movt.operands[1].imm == 0x8105u);
    CHECK(same_operands(movt, thumb(0xF2C81105)));
}

TEST_CASE("arm: conditions and flags") {
    const Instruction a = decode_arm(0x10810002, 0);  // addne r0, r1, r2
    CHECK(a.cond == Cond::Ne);
    CHECK(decode_arm(0xE0810002, 0).cond == Cond::Al);
    CHECK(decode_arm(0xF57FF05B, 0).op == Op::Dmb);  // dmb ish: unconditional space
    CHECK(decode_arm(0xF57FF05B, 0).cond == Cond::None);
    // 16-bit Thumb arithmetic sets flags outside an IT block
    const Instruction t = thumb16(0x1888);  // adds r0, r1, r2
    CHECK(t.op == Op::Add);
    CHECK((t.flags & kSetsFlags) != 0);
    CHECK(t.size == 2);
}

TEST_CASE("arm: IT blocks give their instructions conditions and stop 16-bit flag setting") {
    ItState it;
    const u16 code[] = {0xBF0C, 0x2001, 0x2002, 0x3001};  // ite eq; moveq r0, #1; movne r0, #2; adds r0, #1
    const Instruction i0 = decode_thumb(code[0], 0, 0, it);
    CHECK(i0.op == Op::It);
    CHECK(i0.cond == Cond::Eq);
    CHECK(it.active());
    const Instruction i1 = decode_thumb(code[1], 0, 2, it);
    CHECK(i1.op == Op::Mov);
    CHECK(i1.cond == Cond::Eq);
    CHECK((i1.flags & kSetsFlags) == 0);
    CHECK((i1.flags & kInIt) != 0);
    const Instruction i2 = decode_thumb(code[2], 0, 4, it);
    CHECK(i2.cond == Cond::Ne);
    CHECK_FALSE(it.active());
    const Instruction i3 = decode_thumb(code[3], 0, 6, it);
    CHECK(i3.cond == Cond::Al);
    CHECK((i3.flags & kSetsFlags) != 0);
    CHECK(format(i0) == "ite\teq");
    CHECK(format(i2) == "movne\tr0, #0x2");
}

TEST_CASE("arm: branch targets") {
    // b . (ARM): imm24 = -2, target = address + 8 - 8
    const Instruction b = decode_arm(0xEAFFFFFE, 0x81000100);
    CHECK(b.op == Op::B);
    REQUIRE(b.operand_count == 1);
    CHECK(b.operands[0].kind == OperandKind::Label);
    CHECK(b.operands[0].imm == 0x81000100u);
    // bl . (Thumb): S=1, imm10 all ones, J1=J2=1, imm11 = 0x7FE: offset -4, target = address + 4 - 4
    const Instruction bl = thumb(0xF7FFFFFE, 0x81000200);
    CHECK(bl.op == Op::Bl);
    CHECK(bl.operands[0].imm == 0x81000200u);
    // b.w . (Thumb)
    CHECK(thumb(0xF7FFBFFE, 0x81000300).operands[0].imm == 0x81000300u);
    // cbz r0, address + 4 + 12 (i = 0, imm5 = 6)
    const Instruction cbz = thumb16(0xB130, 0x2000);
    CHECK(cbz.op == Op::Cbz);
    CHECK(cbz.operands[1].imm == 0x2010u);
    // blx (ARM -> Thumb): target = address + 8 + (imm24 << 2) + (H << 1)
    const Instruction blx = decode_arm(0xFB000000, 0x1000);  // H = 1, imm24 = 0
    CHECK(blx.op == Op::Blx);
    CHECK(blx.operands[0].imm == 0x100Au);
}

TEST_CASE("arm: memory operands, exclusives and table branches") {
    const Instruction ldrd = decode_arm(0xE1C020D0, 0);  // ldrd r2, r3, [r0]
    CHECK(ldrd.op == Op::Ldrd);
    REQUIRE(ldrd.operand_count == 3);
    CHECK(ldrd.operands[1].reg == 3);
    CHECK(ldrd.operands[2].kind == OperandKind::Mem);
    CHECK(ldrd.operands[2].reg == 0);
    const Instruction lit = thumb16(0x4A02);  // ldr r2, [pc, #8]
    CHECK(lit.op == Op::Ldr);
    CHECK(lit.operands[1].reg == kPc);
    CHECK(lit.operands[1].imm == 8u);
    CHECK(decode_arm(0xE1910F9F, 0).op == Op::Ldrex);  // ldrex r0, [r1]
    const Instruction strex = decode_arm(0xE1842F93, 0);  // strex r2, r3, [r4]
    CHECK(strex.op == Op::Strex);
    CHECK(strex.operands[0].reg == 2);
    CHECK(strex.operands[1].reg == 3);
    const Instruction tbb = thumb(0xE8D0F001);  // tbb [r0, r1]
    CHECK(tbb.op == Op::Tbb);
    CHECK((tbb.operands[0].flags & kMemIndexReg) != 0);
    CHECK(tbb.operands[0].reg2 == 1);
    const Instruction vldm = decode_arm(0xECB08B08, 0);  // vldmia r0!, {d8-d11}
    CHECK(vldm.op == Op::Vldmia);
    CHECK((vldm.flags & kWriteBack) != 0);
    CHECK(vldm.operands[1].kind == OperandKind::DRegList);
    CHECK(vldm.operands[1].reg == 8);
    CHECK(vldm.operands[1].count == 4);
}

TEST_CASE("arm: undefined and unpredictable encodings are reported") {
    CHECK_FALSE(decode_arm(0xE730F211, 0).valid());   // udiv: not on the Cortex-A9
    CHECK(decode_arm(0xE7F000F0, 0).op == Op::Udf);  // udf #0: permanently undefined, decoded as such
    const Instruction odd = decode_arm(0xE1C010D0, 0);  // ldrd r1, r2, [r0]: odd first register
    CHECK(odd.valid());
    CHECK((odd.flags & kUnpredictable) != 0);
    CHECK_FALSE(thumb16(0xB400).valid());  // push {} (empty list)
    CHECK(format(decode_arm(0xE0810002, 0)) == "add\tr0, r1, r2");
    CHECK(format(decode_arm(0xECB08B08, 0)) == "vldmia\tr0!, {d8, d9, d10, d11}");
}
