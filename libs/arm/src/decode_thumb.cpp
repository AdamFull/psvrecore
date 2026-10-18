// The Thumb (T32) instruction set: 16- and 32-bit encodings and IT blocks, following the ARM Architecture Reference
// Manual's decode tables (ARMv7-A, A6).
//
// Two-operand forms (ands r0, r1) keep two operands: the first is both a source and the destination.

#include <bit>

#include "common.hpp"

namespace psvrecore::arm {

using namespace detail;

namespace {

u8 r3(u32 v, int lo) { return static_cast<u8>(bits(v, lo + 2, lo)); }
u8 r4(u32 v, int lo) { return static_cast<u8>(bits(v, lo + 3, lo)); }

Operand imm_offset_mem(u8 rn, u32 offset) {
    Operand o{OperandKind::Mem, rn};
    o.imm = offset;
    o.flags = kMemPre;
    return o;
}

// ThumbExpandImm: the 12-bit modified immediate of 32-bit data-processing instructions. `valid` is false for the
// encodings the architecture leaves unpredictable (a zero byte in a replicated pattern).
u32 thumb_expand_imm(u32 imm12, bool& valid) {
    valid = true;
    const u32 imm8 = imm12 & 0xFF;
    if ((imm12 >> 10) == 0) {
        switch ((imm12 >> 8) & 3) {
            case 0: return imm8;
            case 1: valid = imm8 != 0; return (imm8 << 16) | imm8;
            case 2: valid = imm8 != 0; return (imm8 << 24) | (imm8 << 8);
            default: valid = imm8 != 0; return imm8 * 0x01010101u;
        }
    }
    return ror32(0x80 | (imm12 & 0x7F), imm12 >> 7);
}

// --- 16-bit encodings (A6.2) ---

void thumb16(u16 hw, Instruction& in) {
    const bool it = in.flags & kInIt;
    const auto s = [&] {
        if (!it) in.flags |= kSetsFlags;
    };
    const u32 op = bits(hw, 15, 10);

    if ((op & 0x30) == 0) {  // shift (immediate), add, subtract, move, compare
        const u32 o = bits(hw, 13, 9);
        const u8 rd = r3(hw, 0), rm = r3(hw, 3), rn = r3(hw, 3);
        const u32 imm5 = bits(hw, 10, 6);
        if (o < 4) {  // lsl #imm (imm5 0: mov)
            s();
            if (imm5 == 0) return set(in, Op::Mov, {reg(rd), reg(rm)});
            return set(in, Op::Lsl, {reg(rd), reg(rm), imm(imm5)});
        }
        if (o < 8) {
            s();
            return set(in, Op::Lsr, {reg(rd), reg(rm), imm(imm5 ? imm5 : 32)});
        }
        if (o < 12) {
            s();
            return set(in, Op::Asr, {reg(rd), reg(rm), imm(imm5 ? imm5 : 32)});
        }
        const u8 rm3 = r3(hw, 6);
        switch (o) {
            case 12: s(); return set(in, Op::Add, {reg(rd), reg(rn), reg(rm3)});
            case 13: s(); return set(in, Op::Sub, {reg(rd), reg(rn), reg(rm3)});
            case 14: s(); return set(in, Op::Add, {reg(rd), reg(rn), imm(bits(hw, 8, 6))});
            case 15: s(); return set(in, Op::Sub, {reg(rd), reg(rn), imm(bits(hw, 8, 6))});
            default: break;
        }
        const u8 rdn = r3(hw, 8);
        const u32 imm8 = bits(hw, 7, 0);
        switch (o >> 2) {
            case 4: s(); return set(in, Op::Mov, {reg(rdn), imm(imm8)});
            case 5: return set(in, Op::Cmp, {reg(rdn), imm(imm8)});
            case 6: s(); return set(in, Op::Add, {reg(rdn), imm(imm8)});
            default: s(); return set(in, Op::Sub, {reg(rdn), imm(imm8)});
        }
    }
    if (op == 0x10) {  // data processing
        const u8 rdn = r3(hw, 0), rm = r3(hw, 3);
        switch (bits(hw, 9, 6)) {
            case 0: s(); return set(in, Op::And, {reg(rdn), reg(rm)});
            case 1: s(); return set(in, Op::Eor, {reg(rdn), reg(rm)});
            case 2: s(); return set(in, Op::Lsl, {reg(rdn), reg(rm)});
            case 3: s(); return set(in, Op::Lsr, {reg(rdn), reg(rm)});
            case 4: s(); return set(in, Op::Asr, {reg(rdn), reg(rm)});
            case 5: s(); return set(in, Op::Adc, {reg(rdn), reg(rm)});
            case 6: s(); return set(in, Op::Sbc, {reg(rdn), reg(rm)});
            case 7: s(); return set(in, Op::Ror, {reg(rdn), reg(rm)});
            case 8: return set(in, Op::Tst, {reg(rdn), reg(rm)});
            case 9: s(); return set(in, Op::Rsb, {reg(rdn), reg(rm), imm(0)});
            case 10: return set(in, Op::Cmp, {reg(rdn), reg(rm)});
            case 11: return set(in, Op::Cmn, {reg(rdn), reg(rm)});
            case 12: s(); return set(in, Op::Orr, {reg(rdn), reg(rm)});
            case 13: s(); return set(in, Op::Mul, {reg(rdn), reg(rm), reg(rdn)});
            case 14: s(); return set(in, Op::Bic, {reg(rdn), reg(rm)});
            default: s(); return set(in, Op::Mvn, {reg(rdn), reg(rm)});
        }
    }
    if (op == 0x11) {  // special data instructions, branch and exchange
        const u8 rdn = static_cast<u8>((bit(hw, 7) << 3) | bits(hw, 2, 0)), rm = r4(hw, 3);
        switch (bits(hw, 9, 6)) {
            case 0: case 1: case 2: case 3:
                if (rm == kSp) set(in, Op::Add, {reg(rdn), reg(kSp), reg(rdn)});  // add rdm, sp, rdm
                else set(in, Op::Add, {reg(rdn), reg(rm)});
                unpredictable(in, rdn == kPc && rm == kPc);
                return;
            case 4: case 5: case 6: case 7:
                set(in, Op::Cmp, {reg(rdn), reg(rm)});
                unpredictable(in, (rdn < 8 && rm < 8) || rdn == kPc || rm == kPc);
                return;
            case 8: case 9: case 10: case 11: return set(in, Op::Mov, {reg(rdn), reg(rm)});
            case 12: case 13:
                set(in, Op::Bx, {reg(rm)});
                unpredictable(in, bits(hw, 2, 0) != 0);
                return;
            default:
                if (bits(hw, 2, 0) != 0) return undefined(in);
                set(in, Op::Blx, {reg(rm)});
                unpredictable(in, rm == kPc);
                return;
        }
    }
    if ((op & 0x3E) == 0x12) {  // ldr (literal)
        Operand m = imm_offset_mem(kPc, bits(hw, 7, 0) * 4);
        m.flags |= kMemImm;
        return set(in, Op::Ldr, {reg(r3(hw, 8)), m});
    }
    if ((op & 0x3C) == 0x14) {  // load/store (register offset)
        static constexpr Op k[8] = {Op::Str, Op::Strh, Op::Strb, Op::Ldrsb, Op::Ldr, Op::Ldrh, Op::Ldrb, Op::Ldrsh};
        return set(in, k[bits(hw, 11, 9)], {reg(r3(hw, 0)), mem_reg(r3(hw, 3), r3(hw, 6), true, true, false)});
    }
    if ((op & 0x38) == 0x18 || (op & 0x3C) == 0x20) {  // load/store (immediate offset)
        const u32 opa = bits(hw, 15, 12);
        const bool load = bit(hw, 11);
        const u32 imm5 = bits(hw, 10, 6);
        const u8 rt = r3(hw, 0), rn = r3(hw, 3);
        if (opa == 6) return set(in, load ? Op::Ldr : Op::Str, {reg(rt), imm_offset_mem(rn, imm5 * 4)});
        if (opa == 7) return set(in, load ? Op::Ldrb : Op::Strb, {reg(rt), imm_offset_mem(rn, imm5)});
        return set(in, load ? Op::Ldrh : Op::Strh, {reg(rt), imm_offset_mem(rn, imm5 * 2)});
    }
    if ((op & 0x3C) == 0x24) {  // load/store SP-relative
        return set(in, bit(hw, 11) ? Op::Ldr : Op::Str, {reg(r3(hw, 8)), imm_offset_mem(kSp, bits(hw, 7, 0) * 4)});
    }
    if ((op & 0x3E) == 0x28) {  // adr
        return set(in, Op::Adr, {reg(r3(hw, 8)), imm(bits(hw, 7, 0) * 4)});
    }
    if ((op & 0x3E) == 0x2A) {  // add (SP plus immediate)
        return set(in, Op::Add, {reg(r3(hw, 8)), reg(kSp), imm(bits(hw, 7, 0) * 4)});
    }
    if ((op & 0x3C) == 0x2C) {  // miscellaneous 16-bit instructions
        const u32 o = bits(hw, 11, 5);
        if ((o & 0x7C) == 0x00) return set(in, Op::Add, {reg(kSp), imm(bits(hw, 6, 0) * 4)});
        if ((o & 0x7C) == 0x04) return set(in, Op::Sub, {reg(kSp), imm(bits(hw, 6, 0) * 4)});
        if ((o & 0x28) == 0x08) {  // cbz / cbnz
            const u32 target = in.address + 4 + ((bit(hw, 9) << 6) | (bits(hw, 7, 3) << 1));
            set(in, bit(hw, 11) ? Op::Cbnz : Op::Cbz, {reg(r3(hw, 0)), label(target)});
            unpredictable(in, it);
            return;
        }
        if ((o & 0x78) == 0x10) {
            static constexpr Op k[4] = {Op::Sxth, Op::Sxtb, Op::Uxth, Op::Uxtb};
            return set(in, k[bits(hw, 7, 6)], {reg(r3(hw, 0)), reg(r3(hw, 3))});
        }
        if ((o & 0x70) == 0x20) {
            const u32 list = bits(hw, 7, 0) | (bit(hw, 8) ? (1u << kLr) : 0u);
            if (list == 0) return undefined(in);
            return set(in, Op::Push, {reg_list(list)});
        }
        if (o == 0x32) {
            if (!bit(hw, 4) || bits(hw, 2, 0) != 0) return undefined(in);
            set(in, Op::Setend, {text(bit(hw, 3) ? kTextBe : kTextLe)});
            unpredictable(in, it);
            return;
        }
        if (o == 0x33) {
            if (bit(hw, 3)) return undefined(in);
            set(in, bit(hw, 4) ? Op::Cpsid : Op::Cpsie, {text(kTextIflagsBase | bits(hw, 2, 0))});
            unpredictable(in, it || bits(hw, 2, 0) == 0);
            return;
        }
        if ((o & 0x78) == 0x50) {
            static constexpr Op k[4] = {Op::Rev, Op::Rev16, Op::Undefined, Op::Revsh};
            const Op r = k[bits(hw, 7, 6)];
            if (r == Op::Undefined) return undefined(in);
            return set(in, r, {reg(r3(hw, 0)), reg(r3(hw, 3))});
        }
        if ((o & 0x70) == 0x60) {
            const u32 list = bits(hw, 7, 0) | (bit(hw, 8) ? (1u << kPc) : 0u);
            if (list == 0) return undefined(in);
            return set(in, Op::Pop, {reg_list(list)});
        }
        if ((o & 0x78) == 0x70) return set(in, Op::Bkpt, {imm(bits(hw, 7, 0))});
        if ((o & 0x78) == 0x78) {
            const u32 mask = bits(hw, 3, 0), hint = bits(hw, 7, 4);
            if (mask != 0) {
                set(in, Op::It, {imm(mask)});
                in.cond = static_cast<Cond>(hint);
                if (it || hint == 15 || (hint == 14 && std::popcount(mask) != 1)) unpredictable(in);
                return;
            }
            switch (hint) {
                case 0: return set(in, Op::Nop, {});
                case 1: return set(in, Op::Yield, {});
                case 2: return set(in, Op::Wfe, {});
                case 3: return set(in, Op::Wfi, {});
                case 4: return set(in, Op::Sev, {});
                default: return set(in, Op::Hint, {imm(hint)});
            }
        }
        return undefined(in);
    }
    if ((op & 0x3E) == 0x30) {  // stm (always writes back)
        const u32 list = bits(hw, 7, 0);
        const u8 rn = r3(hw, 8);
        if (list == 0) return undefined(in);
        in.flags |= kWriteBack;
        return set(in, Op::Stm, {reg(rn), reg_list(list)});
    }
    if ((op & 0x3E) == 0x32) {  // ldm (writes back unless the base is in the list)
        const u32 list = bits(hw, 7, 0);
        const u8 rn = r3(hw, 8);
        if (list == 0) return undefined(in);
        if (!(list & (1u << rn))) in.flags |= kWriteBack;
        return set(in, Op::Ldm, {reg(rn), reg_list(list)});
    }
    if ((op & 0x3C) == 0x34) {  // conditional branch, svc, udf
        const u32 cond = bits(hw, 11, 8);
        if (cond == 14) return set(in, Op::Udf, {imm(bits(hw, 7, 0))});
        if (cond == 15) return set(in, Op::Svc, {imm(bits(hw, 7, 0))});
        set(in, Op::B, {label(in.address + 4 + (sign_extend(bits(hw, 7, 0), 8) << 1))});
        in.cond = static_cast<Cond>(cond);
        unpredictable(in, it);
        return;
    }
    if ((op & 0x3E) == 0x38) {  // b (unconditional)
        set(in, Op::B, {label(in.address + 4 + (sign_extend(bits(hw, 10, 0), 11) << 1))});
        unpredictable(in, it && !(in.flags & kNarrow));
        return;
    }
    undefined(in);
}

// --- 32-bit encodings (A6.3) ---

void data_modified_immediate(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op = bits(hw1, 8, 5);
    const bool s = bit(hw1, 4);
    const u8 rn = r4(hw1, 0), rd = r4(hw2, 8);
    const u32 imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | bits(hw2, 7, 0);
    bool valid;
    const Operand value = imm(thumb_expand_imm(imm12, valid));
    if (!valid) unpredictable(in);
    if (s) in.flags |= kSetsFlags;
    if ((imm12 >> 10) != 0) in.flags |= kImmCarry;
    const auto compare = [&](Op o) {
        in.flags &= ~kSetsFlags;
        set(in, o, {reg(rn), value});
    };
    switch (op) {
        case 0: if (rd == kPc && s) return compare(Op::Tst); return set(in, Op::And, {reg(rd), reg(rn), value});
        case 1: return set(in, Op::Bic, {reg(rd), reg(rn), value});
        case 2: if (rn == kPc) return set(in, Op::Mov, {reg(rd), value}); return set(in, Op::Orr, {reg(rd), reg(rn), value});
        case 3: if (rn == kPc) return set(in, Op::Mvn, {reg(rd), value}); return set(in, Op::Orn, {reg(rd), reg(rn), value});
        case 4: if (rd == kPc && s) return compare(Op::Teq); return set(in, Op::Eor, {reg(rd), reg(rn), value});
        case 8: if (rd == kPc && s) return compare(Op::Cmn); return set(in, Op::Add, {reg(rd), reg(rn), value});
        case 10: return set(in, Op::Adc, {reg(rd), reg(rn), value});
        case 11: return set(in, Op::Sbc, {reg(rd), reg(rn), value});
        case 13: if (rd == kPc && s) return compare(Op::Cmp); return set(in, Op::Sub, {reg(rd), reg(rn), value});
        case 14: return set(in, Op::Rsb, {reg(rd), reg(rn), value});
        default: return undefined(in);
    }
}

void data_plain_immediate(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op = bits(hw1, 8, 4);
    const u8 rn = r4(hw1, 0), rd = r4(hw2, 8);
    const u32 imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | bits(hw2, 7, 0);
    const u32 imm16 = (bits(hw1, 3, 0) << 12) | imm12;
    const u32 sat_imm = bits(hw2, 4, 0), shift = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
    if (op >= 0x10 && op != 0x14 && op != 0x1C && bit(hw1, 10)) return undefined(in);
    if ((op == 0x10 || op == 0x12 || op == 0x16 || op == 0x18 || op == 0x1A) && bit(hw2, 5)) return undefined(in);
    if ((op == 0x12 || op == 0x1A) && shift == 0 && bit(hw2, 4)) return undefined(in);  // ssat16/usat16
    switch (op) {
        case 0x00:
            if (rn == kPc) return set(in, Op::Adr, {reg(rd), imm(imm12)});
            in.flags |= kImm12;
            return set(in, Op::Add, {reg(rd), reg(rn), imm(imm12)});
        case 0x04: return set(in, Op::Movw, {reg(rd), imm(imm16)});
        case 0x0A:
            if (rn == kPc) return set(in, Op::Adr, {reg(rd), imm(0u - imm12, kSigned)});
            in.flags |= kImm12;
            return set(in, Op::Sub, {reg(rd), reg(rn), imm(imm12)});
        case 0x0C: return set(in, Op::Movt, {reg(rd), imm(imm16)});
        case 0x10:
        case 0x12:
            if (op == 0x12 && shift == 0) return set(in, Op::Ssat16, {reg(rd), imm(bits(hw2, 3, 0) + 1), reg(rn)});
            return set(in, Op::Ssat, {reg(rd), imm(sat_imm + 1), shifted_reg(rn, bit(hw1, 5) ? 2 : 0, shift)});
        case 0x14:
            set(in, Op::Sbfx, {reg(rd), reg(rn), imm(shift), imm(sat_imm + 1)});
            return unpredictable(in, shift + sat_imm > 31);  // (a field past bit 31)
        case 0x16: {
            const u32 msb = sat_imm, lsb = shift;
            const u32 at = msb >= lsb ? lsb : msb, width = msb >= lsb ? msb - lsb + 1 : 1;
            if (rn == kPc) set(in, Op::Bfc, {reg(rd), imm(at), imm(width)});
            else set(in, Op::Bfi, {reg(rd), reg(rn), imm(at), imm(width)});
            unpredictable(in, msb < lsb);
            return;
        }
        case 0x18:
        case 0x1A:
            if (op == 0x1A && shift == 0) return set(in, Op::Usat16, {reg(rd), imm(bits(hw2, 3, 0)), reg(rn)});
            return set(in, Op::Usat, {reg(rd), imm(sat_imm), shifted_reg(rn, bit(hw1, 5) ? 2 : 0, shift)});
        case 0x1C:
            set(in, Op::Ubfx, {reg(rd), reg(rn), imm(shift), imm(sat_imm + 1)});
            return unpredictable(in, shift + sat_imm > 31);
        default: return undefined(in);
    }
}

void branches_misc(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw2, 14, 12), op = bits(hw1, 10, 4);
    const bool s = bit(hw1, 10), j1 = bit(hw2, 13), j2 = bit(hw2, 11);
    if ((op1 & 5) == 0) {
        if ((op & 0x38) != 0x38) {  // b<c>.w
            const u32 imm = (u32{s} << 20) | (u32{j2} << 19) | (u32{j1} << 18) | (bits(hw1, 5, 0) << 12) | (bits(hw2, 10, 0) << 1);
            set(in, Op::B, {label(in.address + 4 + sign_extend(imm, 21))});
            in.cond = static_cast<Cond>(bits(hw1, 9, 6));
            unpredictable(in, in.flags & kInIt);
            return;
        }
        const u8 rn = r4(hw1, 0);
        switch (op) {
            case 0x38:
            case 0x39: {  // msr (register)
                const u32 mask = bits(hw2, 11, 8);
                if ((mask == 0 && !bit(hw1, 4)) || bits(hw2, 7, 0) != 0 || bit(hw2, 13)) return undefined(in);
                set(in, Op::Msr, {sysreg(kSysMsrBase | (bit(hw1, 4) ? 0x10u : 0u) | mask), reg(rn)});
                return;
            }
            case 0x3A: {  // change processor state, hints
                if (bits(hw1, 3, 0) != 0xF || bit(hw2, 13) || bit(hw2, 11)) return undefined(in);
                if (bits(hw2, 10, 8) != 0) {
                    const u32 imod = bits(hw2, 10, 9), aif = bits(hw2, 7, 5), mode = bits(hw2, 4, 0);
                    const bool m = bit(hw2, 8);
                    if (imod == 1 || (imod == 0 && !m) || ((imod & 2) && aif == 0) || (!m && mode != 0) || (imod == 0 && aif != 0))
                        return undefined(in);
                    if (imod & 2) {
                        set(in, imod == 2 ? Op::Cpsie : Op::Cpsid, {text(kTextIflagsBase | aif)});
                        if (m) in.add(imm(mode));
                    } else {
                        set(in, Op::Cps, {imm(mode)});
                    }
                    in.flags |= kWide;
                    return;
                }
                in.flags |= kWide;
                switch (bits(hw2, 7, 0)) {
                    case 0: return set(in, Op::Nop, {});
                    case 1: return set(in, Op::Yield, {});
                    case 2: return set(in, Op::Wfe, {});
                    case 3: return set(in, Op::Wfi, {});
                    case 4: return set(in, Op::Sev, {});
                    default:
                        if (bits(hw2, 7, 4) == 0xF) return set(in, Op::Dbg, {imm(bits(hw2, 3, 0))});
                        return set(in, Op::Hint, {imm(bits(hw2, 7, 0))});
                }
            }
            case 0x3B: {  // miscellaneous control
                if (bits(hw1, 3, 0) != 0xF || bit(hw2, 13) || bits(hw2, 11, 8) != 0xF) return undefined(in);
                const Operand option{OperandKind::Barrier, 0, 0, ShiftType::Lsl, 0, 0, 0, 0, bits(hw2, 3, 0)};
                switch (bits(hw2, 7, 4)) {
                    case 2:
                        if (bits(hw2, 3, 0) != 0xF) return undefined(in);
                        return set(in, Op::Clrex, {});
                    case 4: return set(in, Op::Dsb, {option});
                    case 5: return set(in, Op::Dmb, {option});
                    case 6: return set(in, Op::Isb, {option});
                    default: return undefined(in);
                }
            }
            case 0x3C:
                if ((hw2 & 0x2FFF) != 0x0F00) return undefined(in);
                return set(in, Op::Bxj, {reg(rn)});
            case 0x3D:
                if (bits(hw1, 3, 0) != 0xE || bit(hw2, 13) || bits(hw2, 11, 8) != 0xF) return undefined(in);
                return set(in, Op::Subs, {reg(kPc), reg(kLr), imm(bits(hw2, 7, 0))});
            case 0x3E:
            case 0x3F:
                if (bits(hw1, 3, 0) != 0xF || bit(hw2, 13) || bits(hw2, 7, 0) != 0) return undefined(in);
                set(in, Op::Mrs, {reg(r4(hw2, 8)), sysreg(bit(hw1, 4) ? kSysSpsr : kSysApsr)});
                return;
            case 0x7F:
                if (op1 == 0) return set(in, Op::Smc, {imm(bits(hw1, 3, 0))});
                if (op1 == 2) return set(in, Op::Udf, {imm((bits(hw1, 3, 0) << 12) | bits(hw2, 11, 0))});
                return undefined(in);
            default: return undefined(in);
        }
    }
    const u32 i1 = (j1 == s) ? 1u : 0u, i2 = (j2 == s) ? 1u : 0u;
    const u32 imm = (u32{s} << 24) | (i1 << 23) | (i2 << 22) | (bits(hw1, 9, 0) << 12) | (bits(hw2, 10, 0) << 1);
    const u32 offset = sign_extend(imm, 25);
    if ((op1 & 5) == 1) {  // b.w
        set(in, Op::B, {label(in.address + 4 + offset)});
        in.flags |= kWide;
        unpredictable(in, (in.flags & kInIt) && !(in.flags & kNarrow));
        return;
    }
    if ((op1 & 5) == 4) {  // blx (immediate)
        if (bit(hw2, 0)) return undefined(in);
        set(in, Op::Blx, {label(((in.address + 4) & ~3u) + offset)});
        return;
    }
    set(in, Op::Bl, {label(in.address + 4 + offset)});
}

void load_store_multiple(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op = bits(hw1, 8, 7);
    const bool load = bit(hw1, 4), wb = bit(hw1, 5);
    const u8 rn = r4(hw1, 0);
    const u32 list = hw2 & 0xFFFF;
    if (op == 0 || op == 3) {
        if (!load && (bits(hw1, 3, 0) != 0xD || (hw2 & 0xFFE0) != 0xC000)) return undefined(in);
        if (load && hw2 != 0xC000) return undefined(in);
        if (!load) {
            static constexpr Op k[2] = {Op::Srsdb, Op::Srsia};
            set(in, k[op == 3], {reg(kSp), imm(bits(hw2, 4, 0))});
        } else {
            static constexpr Op k[2] = {Op::Rfedb, Op::Rfeia};
            set(in, k[op == 3], {reg(rn)});
        }
        if (wb) in.flags |= kWriteBack;
        return;
    }
    const unsigned count = static_cast<unsigned>(std::popcount(list));
    if (count == 0 || (!load && (list & ((1u << kSp) | (1u << kPc))))) return undefined(in);
    if (wb) in.flags |= kWriteBack;
    in.flags |= kWide;
    if (op == 1) {
        if (load && wb && rn == kSp && count >= 2) {  // (one register: printed ldm sp!, as the reference does)
            in.flags &= ~kWriteBack;
            set(in, Op::Pop, {reg_list(list)});
        } else {
            set(in, load ? Op::Ldm : Op::Stm, {reg(rn), reg_list(list)});
        }
    } else {
        if (!load && wb && rn == kSp && count >= 2) {
            in.flags &= ~kWriteBack;
            set(in, Op::Push, {reg_list(list)});
        } else {
            set(in, load ? Op::Ldmdb : Op::Stmdb, {reg(rn), reg_list(list)});
        }
    }
    unpredictable(in, count < 2 || rn == kPc || (list & (1u << kSp)) || (!load && (list & (1u << kPc))) ||
                          (load && (list & 0xC000) == 0xC000) || (wb && (list & (1u << rn))));
}

void load_store_dual_exclusive(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw1, 8, 7), op2 = bits(hw1, 5, 4), op3 = bits(hw2, 7, 4);
    const u8 rn = r4(hw1, 0), rt = r4(hw2, 12), rt2 = r4(hw2, 8), rd = r4(hw2, 0);
    const u32 imm8 = bits(hw2, 7, 0);
    if (op1 == 0 && op2 == 0) return set(in, Op::Strex, {reg(rt2), reg(rt), imm_offset_mem(rn, imm8 * 4)});
    if (op1 == 0 && op2 == 1) {
        if (rt2 != kPc) return undefined(in);
        return set(in, Op::Ldrex, {reg(rt), imm_offset_mem(rn, imm8 * 4)});
    }
    if ((op1 & 2) || (op2 & 2)) {  // ldrd / strd
        const bool p = bit(hw1, 8), u = bit(hw1, 7), wb = bit(hw1, 5), load = bit(hw1, 4);
        Operand m = mem_imm(rn, imm8 * 4, u, p, wb);
        if (!p || wb) m.flags |= kMemImm;
        set(in, load ? Op::Ldrd : Op::Strd, {reg(rt), reg(rt2), m});
        unpredictable(in, (wb && (rn == rt || rn == rt2)) || rt == kPc || rt2 == kPc || rt == kSp || rt2 == kSp || (load && rt == rt2));
        return;
    }
    const Operand m{OperandKind::Mem, rn};
    if (op1 == 1 && op2 == 0) {
        if (op3 != 7 && rt2 != kPc) return undefined(in);
        switch (op3) {
            case 4: return set(in, Op::Strexb, {reg(rd), reg(rt), m});
            case 5: return set(in, Op::Strexh, {reg(rd), reg(rt), m});
            case 7: return set(in, Op::Strexd, {reg(rd), reg(rt), reg(rt2), m});
            default: return undefined(in);
        }
    }
    if ((op3 <= 1 && (hw2 & 0xFF00) != 0xF000) || ((op3 == 4 || op3 == 5) && (rt2 != kPc || rd != kPc)) ||
        (op3 == 7 && rd != kPc))
        return undefined(in);
    switch (op3) {
        case 0: return set(in, Op::Tbb, {mem_reg(rn, rd, true, true, false)});
        case 1: return set(in, Op::Tbh, {mem_reg(rn, rd, true, true, false, ShiftType::Lsl, 1)});
        case 4: return set(in, Op::Ldrexb, {reg(rt), m});
        case 5: return set(in, Op::Ldrexh, {reg(rt), m});
        case 7: return set(in, Op::Ldrexd, {reg(rt), reg(rt2), m});
        default: return undefined(in);
    }
}

void data_shifted_register(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op = bits(hw1, 8, 5);
    const bool s = bit(hw1, 4);
    const u8 rn = r4(hw1, 0), rd = r4(hw2, 8), rm = r4(hw2, 0);
    const u32 type = bits(hw2, 5, 4), imm5 = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
    const Operand op2 = shifted_reg(rm, type, imm5);
    // (the reference checks this bit only for orr and its shift aliases, rrx excepted)
    if (bit(hw2, 15) && op == 2 && !(rn == kPc && type == 3 && imm5 == 0)) return undefined(in);
    if (s) in.flags |= kSetsFlags;
    in.flags |= kWide;
    const auto compare = [&](Op o) {
        in.flags &= ~kSetsFlags;
        set(in, o, {reg(rn), op2});
    };
    switch (op) {
        case 0: if (rd == kPc && s) return compare(Op::Tst); return set(in, Op::And, {reg(rd), reg(rn), op2});
        case 1: return set(in, Op::Bic, {reg(rd), reg(rn), op2});
        case 2:
            if (rn == kPc) {  // mov and immediate shifts
                if (op2.kind == OperandKind::Reg) return set(in, Op::Mov, {reg(rd), reg(rm)});
                if (op2.shift == ShiftType::Rrx) return set(in, Op::Rrx, {reg(rd), reg(rm)});
                static constexpr Op kShift[4] = {Op::Lsl, Op::Lsr, Op::Asr, Op::Ror};
                return set(in, kShift[static_cast<u32>(op2.shift)], {reg(rd), reg(rm), imm(op2.amount)});
            }
            return set(in, Op::Orr, {reg(rd), reg(rn), op2});
        case 3: if (rn == kPc) return set(in, Op::Mvn, {reg(rd), op2}); return set(in, Op::Orn, {reg(rd), reg(rn), op2});
        case 4: if (rd == kPc && s) return compare(Op::Teq); return set(in, Op::Eor, {reg(rd), reg(rn), op2});
        case 6: {
            if (s || bit(hw2, 4)) return undefined(in);
            const bool tb = bit(hw2, 5);
            in.flags &= ~kWide;
            return set(in, tb ? Op::Pkhtb : Op::Pkhbt, {reg(rd), reg(rn), shifted_reg(rm, tb ? 2 : 0, imm5)});
        }
        case 8: if (rd == kPc && s) return compare(Op::Cmn); return set(in, Op::Add, {reg(rd), reg(rn), op2});
        case 10: return set(in, Op::Adc, {reg(rd), reg(rn), op2});
        case 11: return set(in, Op::Sbc, {reg(rd), reg(rn), op2});
        case 13: if (rd == kPc && s) return compare(Op::Cmp); return set(in, Op::Sub, {reg(rd), reg(rn), op2});
        case 14: return set(in, Op::Rsb, {reg(rd), reg(rn), op2});
        default: return undefined(in);
    }
}

// A6.3.7 - A6.3.10: loads and stores of single bytes, halfwords and words, and the memory hints among them.
void load_store_single(u32 hw1, u32 hw2, Instruction& in) {
    const bool sign = bit(hw1, 8), imm12_form = bit(hw1, 7), load = bit(hw1, 4);
    const u32 size = bits(hw1, 6, 5);  // 0 byte, 1 halfword, 2 word
    const u8 rn = r4(hw1, 0), rt = r4(hw2, 12);
    if (size == 3) return undefined(in);
    if (!load) {
        if (sign || rn == kPc) return undefined(in);
    } else if (size == 2 && sign) {
        return undefined(in);
    }
    in.flags |= kWide;

    static constexpr Op kLoad[2][3] = {{Op::Ldrb, Op::Ldrh, Op::Ldr}, {Op::Ldrsb, Op::Ldrsh, Op::Undefined}};
    static constexpr Op kUnprivileged[2][3] = {{Op::Ldrbt, Op::Ldrht, Op::Ldrt}, {Op::Ldrsbt, Op::Ldrsht, Op::Undefined}};
    static constexpr Op kStore[3] = {Op::Strb, Op::Strh, Op::Str};
    static constexpr Op kStoreUnprivileged[3] = {Op::Strbt, Op::Strht, Op::Strt};
    // With Rt = PC, byte and halfword loads are hints: pld (byte), pldw (halfword), pli (signed byte); signed
    // halfword ones are unallocated hints.
    bool hint = load && rt == kPc && size != 2;
    const Op hint_op = size == 0 ? (sign ? Op::Pli : Op::Pld) : (sign ? Op::Nop : Op::Pldw);
    const Op op = load ? kLoad[sign][size] : kStore[size];

    Operand m;
    bool unprivileged = false;
    if (load && rn == kPc) {  // literal
        m = mem_imm(kPc, bits(hw2, 11, 0), imm12_form, true, false);
        m.flags |= kMemImm;
    } else if (imm12_form) {
        m = imm_offset_mem(rn, bits(hw2, 11, 0));
    } else {
        const u32 op2 = bits(hw2, 11, 6);
        if (op2 == 0) {
            m = mem_reg(rn, r4(hw2, 0), true, true, false, ShiftType::Lsl, static_cast<u8>(bits(hw2, 5, 4)));
        } else if ((op2 & 0x24) == 0x24) {  // 1xx1xx: imm8, post-indexed or pre-indexed with writeback
            const bool p = bit(hw2, 10), u = bit(hw2, 9);
            m = mem_imm(rn, bits(hw2, 7, 0), u, p, true);
            m.flags |= kMemImm;
            // Rt = PC here is unpredictable; the reference decodes these as loads.
            if (hint) hint = false, unpredictable(in);
        } else if ((op2 & 0x3C) == 0x30) {  // 1100xx: negative imm8
            m = mem_imm(rn, bits(hw2, 7, 0), false, true, false);
        } else if ((op2 & 0x3C) == 0x38) {  // 1110xx: unprivileged
            if (hint) hint = false, unpredictable(in);
            m = imm_offset_mem(rn, bits(hw2, 7, 0));
            unprivileged = true;
        } else {
            return undefined(in);
        }
    }
    if (hint) {
        if (rn == kPc && hint_op == Op::Pldw) {
            // There is no literal pldw. The reference reads these through the non-literal pldw forms: where
            // hw2 bits 11-6 select the register or the negative-offset form it rejects the PC base, otherwise it
            // decodes a literal pld.
            const u32 op2 = bits(hw2, 11, 6);
            if (imm12_form || op2 == 0 || (op2 & 0x3C) == 0x30) return undefined(in);
            return set(in, Op::Pld, {m});
        }
        if (hint_op == Op::Nop) return undefined(in);  // (unallocated hints: the reference does not decode them)
        in.flags &= ~kWide;
        return set(in, hint_op, {m});
    }
    const Op final_op = unprivileged ? (load ? kUnprivileged[sign][size] : kStoreUnprivileged[size]) : op;
    if (unprivileged) in.flags &= ~kWide;
    set(in, final_op, {reg(rt), m});
}

void data_register(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw1, 7, 4), op2 = bits(hw2, 7, 4);
    const u8 rn = r4(hw1, 0), rd = r4(hw2, 8), rm = r4(hw2, 0);
    if (bits(hw2, 15, 12) != 0xF) return undefined(in);
    if ((op1 & 8) == 0 && op2 == 0) {  // shifts by register
        static constexpr Op kShift[4] = {Op::Lsl, Op::Lsr, Op::Asr, Op::Ror};
        if (bit(hw1, 4)) in.flags |= kSetsFlags;
        in.flags |= kWide;
        return set(in, kShift[bits(hw1, 6, 5)], {reg(rd), reg(rn), reg(rm)});
    }
    if ((op1 & 8) == 0 && (op2 & 8)) {  // extends
        static constexpr Op kAdd[6] = {Op::Sxtah, Op::Uxtah, Op::Sxtab16, Op::Uxtab16, Op::Sxtab, Op::Uxtab};
        static constexpr Op kPlain[6] = {Op::Sxth, Op::Uxth, Op::Sxtb16, Op::Uxtb16, Op::Sxtb, Op::Uxtb};
        if (op1 > 5) return undefined(in);
        Operand o = reg(rm);
        const u32 rotation = bits(hw2, 5, 4) * 8;
        if (rotation) {
            o.kind = OperandKind::RegShiftImm;
            o.shift = ShiftType::Ror;
            o.amount = static_cast<u8>(rotation);
        }
        if (rn == kPc) {
            if (op1 != 2 && op1 != 3) in.flags |= kWide;
            return set(in, kPlain[op1], {reg(rd), o});
        }
        return set(in, kAdd[op1], {reg(rd), reg(rn), o});
    }
    if ((op1 & 8) && (op2 & 0xC) == 0) {  // signed parallel
        static constexpr u32 kIndex[8] = {4, 0, 1, 8, 7, 3, 2, 8};  // Thumb op1 order -> add16, asx, sax, sub16, add8, sub8
        const u32 index = kIndex[op1 & 7];
        const Op o = index < 8 ? parallel_op(false, (op2 & 3) + 1, index) : Op::Undefined;
        if (o == Op::Undefined) return undefined(in);
        return set(in, o, {reg(rd), reg(rn), reg(rm)});
    }
    if ((op1 & 8) && (op2 & 0xC) == 4) {  // unsigned parallel
        static constexpr u32 kIndex[8] = {4, 0, 1, 8, 7, 3, 2, 8};
        const u32 index = kIndex[op1 & 7];
        const Op o = index < 8 ? parallel_op(true, (op2 & 3) + 1, index) : Op::Undefined;
        if (o == Op::Undefined) return undefined(in);
        return set(in, o, {reg(rd), reg(rn), reg(rm)});
    }
    if ((op1 & 0xC) == 8 && (op2 & 0xC) == 8) {  // miscellaneous
        const u32 a = bits(hw1, 5, 4), b = bits(hw2, 5, 4);
        switch ((a << 2) | b) {
            case 0: return set(in, Op::Qadd, {reg(rd), reg(rm), reg(rn)});
            case 1: return set(in, Op::Qdadd, {reg(rd), reg(rm), reg(rn)});
            case 2: return set(in, Op::Qsub, {reg(rd), reg(rm), reg(rn)});
            case 3: return set(in, Op::Qdsub, {reg(rd), reg(rm), reg(rn)});
            case 4: in.flags |= kWide; set(in, Op::Rev, {reg(rd), reg(rm)}); unpredictable(in, rm != rn); return;
            case 5: in.flags |= kWide; set(in, Op::Rev16, {reg(rd), reg(rm)}); unpredictable(in, rm != rn); return;
            case 6: set(in, Op::Rbit, {reg(rd), reg(rm)}); unpredictable(in, rm != rn); return;
            case 7: in.flags |= kWide; set(in, Op::Revsh, {reg(rd), reg(rm)}); unpredictable(in, rm != rn); return;
            case 8: return set(in, Op::Sel, {reg(rd), reg(rn), reg(rm)});
            case 12: set(in, Op::Clz, {reg(rd), reg(rm)}); unpredictable(in, rm != rn); return;
            default: return undefined(in);
        }
    }
    undefined(in);
}

void multiply(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw1, 6, 4), op2 = bits(hw2, 5, 4);
    const u8 rn = r4(hw1, 0), ra = r4(hw2, 12), rd = r4(hw2, 8), rm = r4(hw2, 0);
    if (bits(hw2, 7, 6) != 0) return undefined(in);
    const bool plain = ra == kPc;
    switch (op1) {
        case 0:
            if (op2 == 0) return plain ? set(in, Op::Mul, {reg(rd), reg(rn), reg(rm)}) : set(in, Op::Mla, {reg(rd), reg(rn), reg(rm), reg(ra)});
            if (op2 == 1) return set(in, Op::Mls, {reg(rd), reg(rn), reg(rm), reg(ra)});
            return undefined(in);
        case 1: {
            static constexpr Op kMul[4] = {Op::Smulbb, Op::Smulbt, Op::Smultb, Op::Smultt};
            static constexpr Op kMla[4] = {Op::Smlabb, Op::Smlabt, Op::Smlatb, Op::Smlatt};
            return plain ? set(in, kMul[op2], {reg(rd), reg(rn), reg(rm)}) : set(in, kMla[op2], {reg(rd), reg(rn), reg(rm), reg(ra)});
        }
        case 2:
            if (op2 & 2) return undefined(in);
            return plain ? set(in, op2 ? Op::Smuadx : Op::Smuad, {reg(rd), reg(rn), reg(rm)})
                         : set(in, op2 ? Op::Smladx : Op::Smlad, {reg(rd), reg(rn), reg(rm), reg(ra)});
        case 3:
            if (op2 & 2) return undefined(in);
            return plain ? set(in, op2 ? Op::Smulwt : Op::Smulwb, {reg(rd), reg(rn), reg(rm)})
                         : set(in, op2 ? Op::Smlawt : Op::Smlawb, {reg(rd), reg(rn), reg(rm), reg(ra)});
        case 4:
            if (op2 & 2) return undefined(in);
            return plain ? set(in, op2 ? Op::Smusdx : Op::Smusd, {reg(rd), reg(rn), reg(rm)})
                         : set(in, op2 ? Op::Smlsdx : Op::Smlsd, {reg(rd), reg(rn), reg(rm), reg(ra)});
        case 5:
            if (op2 & 2) return undefined(in);
            return plain ? set(in, op2 ? Op::Smmulr : Op::Smmul, {reg(rd), reg(rn), reg(rm)})
                         : set(in, op2 ? Op::Smmlar : Op::Smmla, {reg(rd), reg(rn), reg(rm), reg(ra)});
        case 6:
            if (op2 & 2) return undefined(in);
            return set(in, op2 ? Op::Smmlsr : Op::Smmls, {reg(rd), reg(rn), reg(rm), reg(ra)});
        default:
            if (op2 != 0) return undefined(in);
            return plain ? set(in, Op::Usad8, {reg(rd), reg(rn), reg(rm)}) : set(in, Op::Usada8, {reg(rd), reg(rn), reg(rm), reg(ra)});
    }
}

void long_multiply(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw1, 6, 4), op2 = bits(hw2, 7, 4);
    const u8 rn = r4(hw1, 0), lo = r4(hw2, 12), hi = r4(hw2, 8), rm = r4(hw2, 0);
    const auto four = [&](Op o) { set(in, o, {reg(lo), reg(hi), reg(rn), reg(rm)}); };
    switch (op1) {
        case 0: if (op2 == 0) return four(Op::Smull); return undefined(in);
        case 2: if (op2 == 0) return four(Op::Umull); return undefined(in);
        case 4:
            if (op2 == 0) return four(Op::Smlal);
            if ((op2 & 0xC) == 8) {
                static constexpr Op k[4] = {Op::Smlalbb, Op::Smlalbt, Op::Smlaltb, Op::Smlaltt};
                return four(k[op2 & 3]);
            }
            if ((op2 & 0xE) == 0xC) return four(op2 & 1 ? Op::Smlaldx : Op::Smlald);
            return undefined(in);
        case 5: if ((op2 & 0xE) == 0xC) return four(op2 & 1 ? Op::Smlsldx : Op::Smlsld); return undefined(in);
        case 6:
            if (op2 == 0) return four(Op::Umlal);
            if (op2 == 6) return four(Op::Umaal);
            return undefined(in);
        default: return undefined(in);  // (sdiv, udiv: not on the Cortex-A9)
    }
}

void thumb32(u32 hw1, u32 hw2, Instruction& in) {
    const u32 op1 = bits(hw1, 12, 11), op2 = bits(hw1, 10, 4);
    const bool op = bit(hw2, 15);
    const u32 word = (hw1 << 16) | hw2;
    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) return load_store_multiple(hw1, hw2, in);
        if ((op2 & 0x64) == 0x04) return load_store_dual_exclusive(hw1, hw2, in);
        if ((op2 & 0x60) == 0x20) return data_shifted_register(hw1, hw2, in);
        // coprocessor, floating point, Advanced SIMD
    } else if (op1 == 2) {
        if (op) return branches_misc(hw1, hw2, in);
        if ((op2 & 0x20) == 0) return data_modified_immediate(hw1, hw2, in);
        return data_plain_immediate(hw1, hw2, in);
    } else {
        if ((op2 & 0x71) == 0x00 || (op2 & 0x67) == 0x01 || (op2 & 0x67) == 0x03 || (op2 & 0x67) == 0x05)
            return load_store_single(hw1, hw2, in);
        if ((op2 & 0x71) == 0x10) {  // Advanced SIMD element or structure load/store, as ARM's 1111 0100
            if (!decode_simd_memory(0xF4000000u | (word & 0x00FFFFFFu), in)) undefined(in);
            return;
        }
        if ((op2 & 0x67) == 0x07) return undefined(in);
        if ((op2 & 0x70) == 0x20) return data_register(hw1, hw2, in);
        if ((op2 & 0x78) == 0x30) return multiply(hw1, hw2, in);
        if ((op2 & 0x78) == 0x38) return long_multiply(hw1, hw2, in);
    }
    // coprocessor, floating point, Advanced SIMD (op2 = 1xxxxxx)
    if ((op2 & 0x40) == 0) return undefined(in);
    if ((op2 & 0x30) == 0x30) {  // 111U 1111: Advanced SIMD data processing, as ARM's 1111 001U
        const u32 arm = 0xF2000000u | (u32{bit(word, 28)} << 24) | (word & 0x00FFFFFFu);
        if (!decode_simd_data(arm, in)) undefined(in);
        return;
    }
    // ldc2/stc2 on coprocessors 10 and 11: the reference decodes only the unindexed long forms in Thumb
    if (bit(hw1, 12) && (bits(hw2, 11, 8) & 0xE) == 0xA && (op2 & 0x60) == 0x40 && !bit(hw1, 6)) return undefined(in);
    decode_coprocessor(word, in, bit(hw1, 12));
}

}  // namespace

void ItState::start(u32 firstcond, u32 mask) {
    const u32 cc = firstcond == 15 ? 14 : firstcond;
    const int lowest = std::countr_zero(mask & 0xF);
    const auto push = [&](u32 c) {
        if (count == kCapacity) {  // (only for long runs of nested IT instructions: drop the oldest)
            for (u8 i = 1; i < kCapacity; ++i) conds[i - 1] = conds[i];
            --count;
        }
        conds[count++] = static_cast<u8>(c);
    };
    // A slot's condition is firstcond<3:1> with the slot's mask bit as its low bit. The last slot is pushed
    // first, so the first instruction's condition ends on top.
    for (int pos = lowest + 1; pos <= 3; ++pos) push((cc & 0xE) | ((mask >> pos) & 1));
    push(cc);
}

Instruction decode_thumb(u16 hw1, u16 hw2, u32 address, ItState& it) {
    Instruction in;
    in.address = address;
    in.mode = Mode::Thumb;
    const bool wide = thumb_is_32bit(hw1);
    in.size = wide ? 4 : 2;
    in.raw = wide ? (u32{hw1} << 16) | hw2 : hw1;
    const bool in_block = it.active();
    if (in_block) {
        in.flags |= kInIt;
        in.cond = it.cond();
    }
    if (wide) thumb32(hw1, hw2, in);
    else thumb16(hw1, in);
    // Instructions covered by an IT block take its condition (set above; a conditional branch sets its own), except
    // those that execute unconditionally.
    if (in_block && (in.op == Op::Bkpt || in.op == Op::Udf || in.op == Op::Cbz || in.op == Op::Cbnz ||
                     ((in.op == Op::Cpsie || in.op == Op::Cpsid || in.op == Op::Setend) && !wide)))
        in.cond = Cond::Al;
    // movs rd, rm (16-bit, lsl #0) is not allowed in an IT block; the reference prints it unconditional, setting flags.
    if (in_block && !wide && in.op == Op::Mov && bits(hw1, 15, 6) == 0) {
        in.cond = Cond::Al;
        in.flags |= kSetsFlags | kUnpredictable;
    }
    check_pc_destination(in);
    if (in_block) it.advance();
    if (in.op == Op::It) {
        if (in_block) in.flags |= kUnpredictable;
        if (in.cond == Cond::None) in.cond = Cond::Al;
        it.start(static_cast<u32>(in.cond), in.operands[0].imm);
    }
    return in;
}

}  // namespace psvrecore::arm
