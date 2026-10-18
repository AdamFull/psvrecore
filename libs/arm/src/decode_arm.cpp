// The ARM (A32) instruction set, following the ARM Architecture Reference Manual's decode tables (ARMv7-A, A5).

#include <bit>

#include "common.hpp"

namespace psvrecore::arm {

using namespace detail;

namespace detail {

Op parallel_op(bool is_unsigned, u32 prefix, u32 op) {
    // op: 0 add16, 1 asx, 2 sax, 3 sub16, 4 add8, 7 sub8 (5 and 6 are undefined)
    static constexpr Op kSigned[3][8] = {
        {Op::Sadd16, Op::Sasx, Op::Ssax, Op::Ssub16, Op::Sadd8, Op::Undefined, Op::Undefined, Op::Ssub8},
        {Op::Qadd16, Op::Qasx, Op::Qsax, Op::Qsub16, Op::Qadd8, Op::Undefined, Op::Undefined, Op::Qsub8},
        {Op::Shadd16, Op::Shasx, Op::Shsax, Op::Shsub16, Op::Shadd8, Op::Undefined, Op::Undefined, Op::Shsub8},
    };
    static constexpr Op kUnsigned[3][8] = {
        {Op::Uadd16, Op::Uasx, Op::Usax, Op::Usub16, Op::Uadd8, Op::Undefined, Op::Undefined, Op::Usub8},
        {Op::Uqadd16, Op::Uqasx, Op::Uqsax, Op::Uqsub16, Op::Uqadd8, Op::Undefined, Op::Undefined, Op::Uqsub8},
        {Op::Uhadd16, Op::Uhasx, Op::Uhsax, Op::Uhsub16, Op::Uhadd8, Op::Undefined, Op::Undefined, Op::Uhsub8},
    };
    if (prefix < 1 || prefix > 3 || op > 7) return Op::Undefined;
    return (is_unsigned ? kUnsigned : kSigned)[prefix - 1][op];
}

void decode_coprocessor(u32 w, Instruction& in, bool unconditional) {
    const u32 op1 = bits(w, 25, 20), cp = bits(w, 11, 8);
    const bool op = bit(w, 4);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rt = static_cast<u8>(bits(w, 15, 12));
    if ((cp & 0xE) == 0xA && !unconditional) {  // the floating-point and Advanced SIMD register space
        bool ok = false;
        if ((op1 & 0x20) == 0 && (op1 & 0x3E) != 0) ok = decode_vfp_transfer(w, in);  // loads/stores, 64-bit moves
        else if ((op1 & 0x30) == 0x20 && !op) ok = decode_vfp(w, in);
        else if ((op1 & 0x30) == 0x20 && op) ok = decode_vfp_transfer(w, in);
        if (!ok) undefined(in);
        return;
    }
    if ((op1 & 0x3E) == 0) return undefined(in);
    if ((cp & 0xE) == 0xA && unconditional) {
        const bool unindexed_load_store = (op1 & 0x20) == 0 && !bit(w, 24) && !bit(w, 21) && bit(w, 23);
        if (!unindexed_load_store) return undefined(in);
    }
    if ((op1 & 0x3E) == 0x04) {  // MCRR / MRRC
        const bool load = bit(w, 20);
        set(in, load ? (unconditional ? Op::Mrrc2 : Op::Mrrc) : (unconditional ? Op::Mcrr2 : Op::Mcrr),
            {coproc(cp), imm(bits(w, 7, 4)), reg(rt), reg(rn), creg(bits(w, 3, 0))});
        unpredictable(in, rt == kPc || rn == kPc || (load && rt == rn));
        return;
    }
    if ((op1 & 0x20) == 0) {  // LDC / STC
        const bool load = bit(w, 20), longf = bit(w, 22), p = bit(w, 24), u = bit(w, 23), wb = bit(w, 21);
        const u32 imm8 = bits(w, 7, 0);
        Op o;
        if (load) o = unconditional ? (longf ? Op::Ldc2l : Op::Ldc2) : (longf ? Op::Ldcl : Op::Ldc);
        else o = unconditional ? (longf ? Op::Stc2l : Op::Stc2) : (longf ? Op::Stcl : Op::Stc);
        if (!p && !wb) {
            if (!u) return undefined(in);
            Operand m{OperandKind::Mem, rn};
            Operand option{OperandKind::CoprocOption};
            option.imm = imm8;
            set(in, o, {coproc(cp), creg(rt), m, option});
            return;
        }
        Operand m = mem_imm(rn, imm8 * 4, u, p, wb);
        if (!p || wb) m.flags |= kMemImm;
        set(in, o, {coproc(cp), creg(rt), m});
        unpredictable(in, rn == kPc && wb);
        return;
    }
    if ((op1 & 0x30) == 0x20) {
        if (!op) {
            set(in, unconditional ? Op::Cdp2 : Op::Cdp,
                {coproc(cp), imm(bits(w, 23, 20)), creg(rt), creg(rn), creg(bits(w, 3, 0)), imm(bits(w, 7, 5))});
            return;
        }
        const bool load = bit(w, 20);
        set(in, load ? (unconditional ? Op::Mrc2 : Op::Mrc) : (unconditional ? Op::Mcr2 : Op::Mcr),
            {coproc(cp), imm(bits(w, 23, 21)), reg(rt), creg(rn), creg(bits(w, 3, 0)), imm(bits(w, 7, 5))});
        if (load && rt == kPc) in.operands[2] = sysreg(kSysApsrNzcv);
        unpredictable(in, !load && rt == kPc);
        return;
    }
    undefined(in);  // (11xxxx: SVC, handled by the caller)
}

}  // namespace detail

namespace {

constexpr Op kDataOps[16] = {Op::And, Op::Eor, Op::Sub, Op::Rsb, Op::Add, Op::Adc, Op::Sbc, Op::Rsc,
                             Op::Tst, Op::Teq, Op::Cmp, Op::Cmn, Op::Orr, Op::Mov, Op::Bic, Op::Mvn};

Operand mod_imm(u32 imm12) {
    Operand o{OperandKind::ModImm};
    o.amount = static_cast<u8>(bits(imm12, 11, 8));
    o.index = static_cast<u8>(bits(imm12, 7, 0));
    o.imm = ror32(o.index, 2u * o.amount);
    return o;
}

// A5.2.1-A5.2.3: data processing with a register, a register-shifted register or an immediate.
void data_processing(u32 w, Instruction& in) {
    const u32 opc = bits(w, 24, 21);
    const bool s = bit(w, 20), immediate = bit(w, 25), by_register = !immediate && bit(w, 4);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rd = static_cast<u8>(bits(w, 15, 12));
    const u8 rm = static_cast<u8>(bits(w, 3, 0)), rs = static_cast<u8>(bits(w, 11, 8));
    const u32 type = bits(w, 6, 5), imm5 = bits(w, 11, 7);
    Operand op2 = immediate ? mod_imm(bits(w, 11, 0)) : by_register ? reg_shifted_reg(rm, type, rs) : shifted_reg(rm, type, imm5);
    const Op op = kDataOps[opc];
    if (s) in.flags |= kSetsFlags;
    if (by_register) unpredictable(in, rd == kPc || rn == kPc || rm == kPc || rs == kPc);

    if (opc >= 8 && opc <= 11) {  // tst, teq, cmp, cmn: no destination (SBZ)
        in.flags &= ~kSetsFlags;
        set(in, op, {reg(rn), op2});
        unpredictable(in, rd != 0);
        return;
    }
    if (opc == 13 || opc == 15) {  // mov, mvn: no first operand (SBZ)
        // A nonzero Rn is unpredictable. Rejected here for every mov and for mvn #imm, as the reference does for
        // all but some plain register moves (which it accepts without a rule this decoder could find).
        if (rn != 0 && (opc == 13 || immediate)) return undefined(in);
        unpredictable(in, rn != 0);
        if (opc == 13 && !immediate) {
            if (by_register) {
                static constexpr Op kShift[4] = {Op::Lsl, Op::Lsr, Op::Asr, Op::Ror};
                set(in, kShift[type], {reg(rd), reg(rm), reg(rs)});
                return;
            }
            if (op2.kind == OperandKind::Reg) return set(in, Op::Mov, {reg(rd), reg(rm)});
            if (op2.shift == ShiftType::Rrx) return set(in, Op::Rrx, {reg(rd), reg(rm)});
            static constexpr Op kShift[4] = {Op::Lsl, Op::Lsr, Op::Asr, Op::Ror};
            set(in, kShift[static_cast<u32>(op2.shift)], {reg(rd), reg(rm), imm(op2.amount)});
            return;
        }
        set(in, op, {reg(rd), op2});
        return;
    }
    set(in, op, {reg(rd), reg(rn), op2});
}

// A5.2.5: multiply and multiply accumulate.
void multiply(u32 w, Instruction& in) {
    const u32 op = bits(w, 23, 20);
    const u8 rd = static_cast<u8>(bits(w, 19, 16)), ra = static_cast<u8>(bits(w, 15, 12));
    const u8 rm = static_cast<u8>(bits(w, 11, 8)), rn = static_cast<u8>(bits(w, 3, 0));
    const bool s = bit(w, 20);
    switch (op >> 1) {
        case 0: set(in, Op::Mul, {reg(rd), reg(rn), reg(rm)}); unpredictable(in, ra != 0); break;
        case 1: set(in, Op::Mla, {reg(rd), reg(rn), reg(rm), reg(ra)}); break;
        case 2:
            if (s) return undefined(in);
            set(in, Op::Umaal, {reg(ra), reg(rd), reg(rn), reg(rm)});
            break;
        case 3:
            if (s) return undefined(in);
            set(in, Op::Mls, {reg(rd), reg(rn), reg(rm), reg(ra)});
            break;
        case 4: set(in, Op::Umull, {reg(ra), reg(rd), reg(rn), reg(rm)}); break;
        case 5: set(in, Op::Umlal, {reg(ra), reg(rd), reg(rn), reg(rm)}); break;
        case 6: set(in, Op::Smull, {reg(ra), reg(rd), reg(rn), reg(rm)}); break;
        default: set(in, Op::Smlal, {reg(ra), reg(rd), reg(rn), reg(rm)}); break;
    }
    if (s && (op >> 1) != 2 && (op >> 1) != 3) in.flags |= kSetsFlags;
    unpredictable(in, rd == kPc || rn == kPc || rm == kPc);
}

// A5.2.7: halfword multiplies.
void halfword_multiply(u32 w, Instruction& in) {
    const u32 op1 = bits(w, 22, 21);
    const bool n_top = bit(w, 5), m_top = bit(w, 6);
    const u8 rd = static_cast<u8>(bits(w, 19, 16)), ra = static_cast<u8>(bits(w, 15, 12));
    const u8 rm = static_cast<u8>(bits(w, 11, 8)), rn = static_cast<u8>(bits(w, 3, 0));
    const u32 xy = (n_top ? 2u : 0u) | (m_top ? 1u : 0u);
    switch (op1) {
        case 0: {
            static constexpr Op k[4] = {Op::Smlabb, Op::Smlabt, Op::Smlatb, Op::Smlatt};
            set(in, k[xy], {reg(rd), reg(rn), reg(rm), reg(ra)});
            break;
        }
        case 1:
            if (!n_top) set(in, m_top ? Op::Smlawt : Op::Smlawb, {reg(rd), reg(rn), reg(rm), reg(ra)});
            else set(in, m_top ? Op::Smulwt : Op::Smulwb, {reg(rd), reg(rn), reg(rm)});
            break;
        case 2: {
            static constexpr Op k[4] = {Op::Smlalbb, Op::Smlalbt, Op::Smlaltb, Op::Smlaltt};
            set(in, k[xy], {reg(ra), reg(rd), reg(rn), reg(rm)});
            break;
        }
        default: {
            static constexpr Op k[4] = {Op::Smulbb, Op::Smulbt, Op::Smultb, Op::Smultt};
            set(in, k[xy], {reg(rd), reg(rn), reg(rm)});
            break;
        }
    }
    unpredictable(in, rd == kPc || rn == kPc || rm == kPc);
}

// A5.2.10: synchronization primitives.
void synchronization(u32 w, Instruction& in) {
    const u32 op = bits(w, 23, 20);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rd = static_cast<u8>(bits(w, 15, 12)), rt = static_cast<u8>(bits(w, 3, 0));
    const Operand m{OperandKind::Mem, rn};
    const bool load_form = (op & 1) != 0;
    if ((op & 0xB) == 0) {
        set(in, bit(w, 22) ? Op::Swpb : Op::Swp, {reg(rd), reg(rt), m});
        unpredictable(in, bits(w, 11, 8) != 0);
        return;
    }
    if (bits(w, 11, 8) != 0xF || (load_form && bits(w, 3, 0) != 0xF)) return undefined(in);
    switch (op) {
        case 0x8: set(in, Op::Strex, {reg(rd), reg(rt), m}); break;
        case 0x9: set(in, Op::Ldrex, {reg(rd), m}); break;
        // (an odd first register is unpredictable; the pair printed is the even register and the next)
        case 0xA:
            if ((rt & 14) == 14) return undefined(in);
            set(in, Op::Strexd, {reg(rd), reg(rt & 14), reg(static_cast<u8>((rt & 14) + 1)), m}); unpredictable(in, rt & 1); break;
        case 0xB:
            if ((rd & 14) == 14) return undefined(in);
            set(in, Op::Ldrexd, {reg(rd & 14), reg(static_cast<u8>((rd & 14) + 1)), m}); unpredictable(in, rd & 1); break;
        case 0xC: set(in, Op::Strexb, {reg(rd), reg(rt), m}); break;
        case 0xD: set(in, Op::Ldrexb, {reg(rd), m}); break;
        case 0xE: set(in, Op::Strexh, {reg(rd), reg(rt), m}); break;
        case 0xF: set(in, Op::Ldrexh, {reg(rd), m}); break;
        default: undefined(in); break;
    }
}

// A5.2.8 and A5.2.9: halfword, signed byte and doubleword loads and stores.
void extra_load_store(u32 w, Instruction& in) {
    const u32 op2 = bits(w, 6, 5);
    const bool load = bit(w, 20), p = bit(w, 24), u = bit(w, 23), wb = bit(w, 21), immediate = bit(w, 22);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rt = static_cast<u8>(bits(w, 15, 12)), rm = static_cast<u8>(bits(w, 3, 0));
    const bool unprivileged = !p && wb;
    Operand m = immediate ? mem_imm(rn, (bits(w, 11, 8) << 4) | bits(w, 3, 0), u, p, wb) : mem_reg(rn, rm, u, p, wb);
    const bool dual_form = (op2 == 2 || op2 == 3) && !load;
    if (dual_form && (unprivileged || rt == kPc)) return undefined(in);
    if (unprivileged && op2 == 1 && !load && !immediate && bits(w, 11, 8) != 0) return undefined(in);  // strht
    unpredictable(in, !immediate && bits(w, 11, 8) != 0);
    if (immediate && (!p || wb)) m.flags |= kMemImm;
    Op op;
    bool dual = false;
    if (op2 == 1) op = load ? (unprivileged ? Op::Ldrht : Op::Ldrh) : (unprivileged ? Op::Strht : Op::Strh);
    else if (op2 == 2) {
        if (load) op = unprivileged ? Op::Ldrsbt : Op::Ldrsb;
        else op = Op::Ldrd, dual = true;
    } else {
        if (load) op = unprivileged ? Op::Ldrsht : Op::Ldrsh;
        else op = Op::Strd, dual = true;
    }
    if (unprivileged) {
        m.flags &= ~kMemPre;
        if (dual) unpredictable(in);
    }
    if (dual) {
        set(in, op, {reg(rt), reg(static_cast<u8>(rt + 1)), m});
        unpredictable(in, (rt & 1) || rt == 14 || (!immediate && bits(w, 11, 8) != 0));
    } else {
        set(in, op, {reg(rt), m});
    }
}

// A5.2.12: miscellaneous instructions.
void miscellaneous(u32 w, Instruction& in) {
    const u32 op2 = bits(w, 6, 4), op = bits(w, 22, 21);
    const u8 rd = static_cast<u8>(bits(w, 15, 12)), rm = static_cast<u8>(bits(w, 3, 0));
    switch (op2) {
        case 0:
            if (bit(w, 9)) return undefined(in);  // banked registers (virtualization)
            if ((op & 1) == 0) {
                // (bits 19-16 should be one; the reference requires bit 16 when reading APSR)
                if (!bit(w, 22) && !bit(w, 16)) return undefined(in);
                set(in, Op::Mrs, {reg(rd), sysreg(bit(w, 22) ? kSysSpsr : kSysApsr)});
                unpredictable(in, bits(w, 19, 16) != 0xF || bits(w, 11, 0) != 0);
                return;
            }
            {
                const u32 mask = bits(w, 19, 16);
                if ((mask == 0 && !bit(w, 22)) || bits(w, 15, 8) != 0xF0) return undefined(in);
                set(in, Op::Msr, {sysreg(kSysMsrBase | (bit(w, 22) ? 0x10u : 0u) | mask), reg(rm)});
            }
            return;
        case 1:
            if (op == 1 && bits(w, 19, 8) == 0xFFF) return set(in, Op::Bx, {reg(rm)});
            if (op == 3 && bits(w, 19, 16) == 0xF && bits(w, 11, 8) == 0xF) return set(in, Op::Clz, {reg(rd), reg(rm)});
            return undefined(in);
        case 2:
            if (op == 1 && bits(w, 19, 8) == 0xFFF) return set(in, Op::Bxj, {reg(rm)});
            return undefined(in);
        case 3:
            if (op == 1 && bits(w, 19, 8) == 0xFFF) return set(in, Op::Blx, {reg(rm)});
            return undefined(in);
        case 5: {
            unpredictable(in, bits(w, 11, 8) != 0);
            static constexpr Op k[4] = {Op::Qadd, Op::Qsub, Op::Qdadd, Op::Qdsub};
            set(in, k[op], {reg(rd), reg(rm), reg(static_cast<u8>(bits(w, 19, 16)))});
            return;
        }
        case 7:
            if (op == 1 && bits(w, 31, 28) == 0xE) return set(in, Op::Bkpt, {imm((bits(w, 19, 8) << 4) | bits(w, 3, 0))});
            if (op == 3 && bits(w, 19, 8) == 0) return set(in, Op::Smc, {imm(bits(w, 3, 0))});
            return undefined(in);
        default: return undefined(in);
    }
}

// A5.2.11: MSR (immediate) and hints.
void msr_and_hints(u32 w, Instruction& in) {
    const u32 op1 = bits(w, 19, 16), op2 = bits(w, 7, 0);
    if (!bit(w, 22) && op1 == 0) {
        if (bits(w, 15, 8) != 0xF0) return undefined(in);
        switch (op2) {
            case 0: return set(in, Op::Nop, {});
            case 1: return set(in, Op::Yield, {});
            case 2: return set(in, Op::Wfe, {});
            case 3: return set(in, Op::Wfi, {});
            case 4: return set(in, Op::Sev, {});
            default:
                if ((op2 & 0xF0) == 0xF0) return set(in, Op::Dbg, {imm(op2 & 0xF)});
                return set(in, Op::Hint, {imm(op2)});
        }
    }
    if (bits(w, 15, 12) != 0xF) return undefined(in);
    set(in, Op::Msr, {sysreg(kSysMsrBase | (bit(w, 22) ? 0x10u : 0u) | op1), mod_imm(bits(w, 11, 0))});
}

// A5.3: word and unsigned byte loads and stores.
void load_store(u32 w, Instruction& in) {
    const bool reg_form = bit(w, 25), p = bit(w, 24), u = bit(w, 23), byte = bit(w, 22), wb = bit(w, 21), load = bit(w, 20);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rt = static_cast<u8>(bits(w, 15, 12)), rm = static_cast<u8>(bits(w, 3, 0));
    const bool unprivileged = !p && wb;
    Op op;
    if (unprivileged) op = load ? (byte ? Op::Ldrbt : Op::Ldrt) : (byte ? Op::Strbt : Op::Strt);
    else op = load ? (byte ? Op::Ldrb : Op::Ldr) : (byte ? Op::Strb : Op::Str);
    Operand m;
    if (reg_form) {
        const Operand s = shifted_reg(rm, bits(w, 6, 5), bits(w, 11, 7));
        m = mem_reg(rn, rm, u, p && !unprivileged, wb, s.kind == OperandKind::Reg ? ShiftType::Lsl : s.shift, s.amount);
        unpredictable(in, rm == kPc);
    } else {
        m = mem_imm(rn, bits(w, 11, 0), u, p && !unprivileged, wb);
        if (!p || wb) m.flags |= kMemImm;
    }
    if (unprivileged) m.flags = static_cast<u8>((m.flags & ~kMemPre) | kMemWriteBack);
    set(in, op, {reg(rt), m});
    unpredictable(in, (wb || !p) && (rn == kPc || rn == rt));
}

// A5.4: media instructions.
void media(u32 w, Instruction& in) {
    const u32 op1 = bits(w, 24, 20), op2 = bits(w, 7, 5);
    const u8 rd = static_cast<u8>(bits(w, 15, 12)), rn = static_cast<u8>(bits(w, 19, 16));
    const u8 rm = static_cast<u8>(bits(w, 3, 0)), rs = static_cast<u8>(bits(w, 11, 8));
    if ((op1 & 0x18) == 0) {  // parallel addition and subtraction
        const Op op = parallel_op(bit(w, 22), bits(w, 21, 20), op2);
        if (op == Op::Undefined) return undefined(in);
        set(in, op, {reg(rd), reg(rn), reg(rm)});
        unpredictable(in, bits(w, 11, 8) != 0xF);
        return;
    }
    if ((op1 & 0x18) == 0x08) {  // packing, unpacking, saturation, reversal
        const u32 o1 = bits(w, 22, 20);
        const u32 rotation = bits(w, 11, 10) * 8;
        const auto rot = [&](u8 r) {
            Operand o = reg(r);
            if (rotation) {
                o.kind = OperandKind::RegShiftImm;
                o.shift = ShiftType::Ror;
                o.amount = static_cast<u8>(rotation);
            }
            return o;
        };
        const auto extend = [&](Op with_add, Op plain) {
            if (rn == kPc) set(in, plain, {reg(rd), rot(rm)});
            else set(in, with_add, {reg(rd), reg(rn), rot(rm)});
        };
        if (o1 == 0 && (op2 & 1) == 0) {
            const bool tb = bit(w, 6);
            const Operand s = shifted_reg(rm, tb ? 2 : 0, bits(w, 11, 7));
            set(in, tb ? Op::Pkhtb : Op::Pkhbt, {reg(rd), reg(rn), s});
            return;
        }
        if ((o1 & 6) == 2 && (op2 & 1) == 0) {  // ssat
            set(in, Op::Ssat, {reg(rd), imm(bits(w, 20, 16) + 1), shifted_reg(rm, bit(w, 6) ? 2 : 0, bits(w, 11, 7))});
            return;
        }
        if ((o1 & 6) == 6 && (op2 & 1) == 0) {  // usat
            set(in, Op::Usat, {reg(rd), imm(bits(w, 20, 16)), shifted_reg(rm, bit(w, 6) ? 2 : 0, bits(w, 11, 7))});
            return;
        }
        const bool rev_fields = bits(w, 19, 16) == 0xF && bits(w, 11, 8) == 0xF;
        const bool sbo_11_8 = bits(w, 11, 8) == 0xF;
        switch ((o1 << 3) | op2) {
            case (0 << 3) | 3: return extend(Op::Sxtab16, Op::Sxtb16);
            case (0 << 3) | 5:
                set(in, Op::Sel, {reg(rd), reg(rn), reg(rm)});
                unpredictable(in, !sbo_11_8);
                return;
            case (2 << 3) | 1:
                if (!sbo_11_8) return undefined(in);
                return set(in, Op::Ssat16, {reg(rd), imm(bits(w, 19, 16) + 1), reg(rm)});
            case (2 << 3) | 3: return extend(Op::Sxtab, Op::Sxtb);
            case (3 << 3) | 1:
                if (!rev_fields) return undefined(in);
                return set(in, Op::Rev, {reg(rd), reg(rm)});
            case (3 << 3) | 3: return extend(Op::Sxtah, Op::Sxth);
            case (3 << 3) | 5:
                if (!rev_fields) return undefined(in);
                return set(in, Op::Rev16, {reg(rd), reg(rm)});
            case (4 << 3) | 3: return extend(Op::Uxtab16, Op::Uxtb16);
            case (6 << 3) | 1:
                if (!sbo_11_8) return undefined(in);
                return set(in, Op::Usat16, {reg(rd), imm(bits(w, 19, 16)), reg(rm)});
            case (6 << 3) | 3: return extend(Op::Uxtab, Op::Uxtb);
            case (7 << 3) | 1:
                if (!rev_fields) return undefined(in);
                return set(in, Op::Rbit, {reg(rd), reg(rm)});
            case (7 << 3) | 3: return extend(Op::Uxtah, Op::Uxth);
            case (7 << 3) | 5:
                if (!rev_fields) return undefined(in);
                return set(in, Op::Revsh, {reg(rd), reg(rm)});
            default: return undefined(in);
        }
    }
    if ((op1 & 0x18) == 0x10) {  // signed multiplies, divides
        const u32 o1 = bits(w, 22, 20);
        const u8 d = rn, a = rd, n = rm, m = rs;  // (fields: Rd 19-16, Ra 15-12, Rm 11-8, Rn 3-0)
        const bool x = bit(w, 5);
        switch ((o1 << 3) | (op2 & 6)) {
            case (0 << 3) | 0:
                if (a == kPc) return set(in, x ? Op::Smuadx : Op::Smuad, {reg(d), reg(n), reg(m)});
                return set(in, x ? Op::Smladx : Op::Smlad, {reg(d), reg(n), reg(m), reg(a)});
            case (0 << 3) | 2:
                if (a == kPc) return set(in, x ? Op::Smusdx : Op::Smusd, {reg(d), reg(n), reg(m)});
                return set(in, x ? Op::Smlsdx : Op::Smlsd, {reg(d), reg(n), reg(m), reg(a)});
            // sdiv and udiv (op1 1 and 3) are not implemented by the Cortex-A9: undefined there.
            case (4 << 3) | 0: return set(in, x ? Op::Smlaldx : Op::Smlald, {reg(a), reg(d), reg(n), reg(m)});
            case (4 << 3) | 2: return set(in, x ? Op::Smlsldx : Op::Smlsld, {reg(a), reg(d), reg(n), reg(m)});
            case (5 << 3) | 0:
                if (a == kPc) return set(in, x ? Op::Smmulr : Op::Smmul, {reg(d), reg(n), reg(m)});
                return set(in, x ? Op::Smmlar : Op::Smmla, {reg(d), reg(n), reg(m), reg(a)});
            case (5 << 3) | 6: return set(in, x ? Op::Smmlsr : Op::Smmls, {reg(d), reg(n), reg(m), reg(a)});
            default: return undefined(in);
        }
    }
    // 11xxx
    if (op1 == 0x18 && op2 == 0) {
        if (rd == kPc) return set(in, Op::Usad8, {reg(rn), reg(rm), reg(rs)});
        return set(in, Op::Usada8, {reg(rn), reg(rm), reg(rs), reg(rd)});
    }
    const u32 lsb = bits(w, 11, 7), msb_or_width = bits(w, 20, 16);
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) return set(in, Op::Sbfx, {reg(rd), reg(rm), imm(lsb), imm(msb_or_width + 1)});
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) return set(in, Op::Ubfx, {reg(rd), reg(rm), imm(lsb), imm(msb_or_width + 1)});
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {
        // msb below lsb is unpredictable; the reference prints it as a 1-bit field at msb.
        const u32 at = msb_or_width >= lsb ? lsb : msb_or_width;
        const u32 width = msb_or_width >= lsb ? msb_or_width - lsb + 1 : 1;
        if (msb_or_width < lsb) unpredictable(in);
        if (rm == kPc) return set(in, Op::Bfc, {reg(rd), imm(at), imm(width)}), unpredictable(in, msb_or_width < lsb);
        set(in, Op::Bfi, {reg(rd), reg(rm), imm(at), imm(width)});
        unpredictable(in, msb_or_width < lsb);
        return;
    }
    if (op1 == 0x1F && op2 == 7) {
        if (bits(w, 31, 28) != 0xE) return undefined(in);
        return set(in, Op::Udf, {imm((bits(w, 19, 8) << 4) | bits(w, 3, 0))});
    }
    undefined(in);
}

// A5.5: branches and block transfers.
void branch_block(u32 w, Instruction& in) {
    if (bit(w, 25)) {
        const u32 target = in.address + 8 + (sign_extend(bits(w, 23, 0), 24) << 2);
        set(in, bit(w, 24) ? Op::Bl : Op::B, {label(target)});
        return;
    }
    const bool p = bit(w, 24), u = bit(w, 23), user = bit(w, 22), wb = bit(w, 21), load = bit(w, 20);
    const u8 rn = static_cast<u8>(bits(w, 19, 16));
    const u32 list = bits(w, 15, 0);
    Op op;
    if (load) op = p ? (u ? Op::Ldmib : Op::Ldmdb) : (u ? Op::Ldm : Op::Ldmda);
    else op = p ? (u ? Op::Stmib : Op::Stmdb) : (u ? Op::Stm : Op::Stmda);
    if (wb) in.flags |= kWriteBack;
    if (user) in.flags |= kUserRegs;
    const unsigned count = static_cast<unsigned>(std::popcount(list));
    if (count == 0) return undefined(in);
    if (rn == kSp && wb && !user && count >= 2 && ((op == Op::Ldm) || (op == Op::Stmdb))) {
        in.flags &= ~kWriteBack;
        set(in, op == Op::Ldm ? Op::Pop : Op::Push, {reg_list(list)});
        return;
    }
    set(in, op, {reg(rn), reg_list(list)});
    unpredictable(in, rn == kPc || count == 0);
}

// A5.7: unconditional instructions (cond = 1111).
void unconditional(u32 w, Instruction& in) {
    in.cond = Cond::None;
    const u32 op1 = bits(w, 27, 20);
    if ((op1 & 0x80) == 0) {  // A5.7.1: memory hints, Advanced SIMD, miscellaneous
        const u32 o = bits(w, 26, 20), op2 = bits(w, 7, 4);
        const u8 rn = static_cast<u8>(bits(w, 19, 16));
        if (o == 0x10) {
            if ((op2 & 2) == 0 && (rn & 1) == 0) {  // cps
                const u32 imod = bits(w, 19, 18), aif = bits(w, 8, 6), mode = bits(w, 4, 0);
                const bool m = bit(w, 17);
                if (imod == 1 || (imod == 0 && !m) || bits(w, 16, 9) != 0 || bit(w, 5)) return undefined(in);
                if ((imod & 2) && aif == 0) return undefined(in);
                if ((!m && mode != 0) || (imod == 0 && aif != 0)) return undefined(in);
                if (imod & 2) {
                    set(in, imod == 2 ? Op::Cpsie : Op::Cpsid, {text(kTextIflagsBase | aif)});
                    if (m) in.add(imm(mode));
                } else {
                    set(in, Op::Cps, {imm(mode)});
                }
                return;
            }
            if ((w & 0xFFFFFDFF) == 0xF1010000) return set(in, Op::Setend, {text(bit(w, 9) ? kTextBe : kTextLe)});
            return undefined(in);
        }
        if ((o & 0x60) == 0x20) {
            if (!decode_simd_data(w, in)) undefined(in);
            return;
        }
        if ((o & 0x71) == 0x40) {
            if (!decode_simd_memory(w, in)) undefined(in);
            return;
        }
        const bool reg_form = bit(w, 25), u = bit(w, 23);
        const auto hint = [&](Op op) {
            if (bits(w, 15, 12) != 0xF) return undefined(in);
            if (reg_form) {
                const Operand s = shifted_reg(static_cast<u8>(bits(w, 3, 0)), bits(w, 6, 5), bits(w, 11, 7));
                set(in, op, {mem_reg(rn, static_cast<u8>(bits(w, 3, 0)), u, true, false,
                                     s.kind == OperandKind::Reg ? ShiftType::Lsl : s.shift, s.amount)});
            } else {
                set(in, op, {mem_imm(rn, bits(w, 11, 0), u, true, false)});
            }
        };
        if ((o & 0x77) == 0x45) return hint(Op::Pli);                              // 100x101: pli (immediate)
        if ((o & 0x77) == 0x51) return hint(Op::Pldw);                              // 101x001
        if ((o & 0x77) == 0x55) return hint(Op::Pld);                               // 101x101
        if (o == 0x57) {
            if (bits(w, 19, 8) != 0xFF0 || (op2 == 1 && bits(w, 3, 0) != 0xF)) return undefined(in);
            switch (op2) {
                case 1: return set(in, Op::Clrex, {});
                case 4: return set(in, Op::Dsb, {Operand{OperandKind::Barrier, 0, 0, ShiftType::Lsl, 0, 0, 0, 0, bits(w, 3, 0)}});
                case 5: return set(in, Op::Dmb, {Operand{OperandKind::Barrier, 0, 0, ShiftType::Lsl, 0, 0, 0, 0, bits(w, 3, 0)}});
                case 6: return set(in, Op::Isb, {Operand{OperandKind::Barrier, 0, 0, ShiftType::Lsl, 0, 0, 0, 0, bits(w, 3, 0)}});
                default: return undefined(in);
            }
        }
        if (reg_form && (op2 & 1) == 0) {
            if ((o & 0x77) == 0x65) return hint(Op::Pli);   // 110x101: pli (register)
            if ((o & 0x77) == 0x71) return hint(Op::Pldw);  // 111x001
            if ((o & 0x77) == 0x75) return hint(Op::Pld);   // 111x101
        }
        return undefined(in);
    }
    if ((op1 & 0xE5) == 0x84) {  // srs
        if ((w & 0x000FFFE0) != 0x000D0500) return undefined(in);
        static constexpr Op k[4] = {Op::Srsda, Op::Srsia, Op::Srsdb, Op::Srsib};
        set(in, k[bits(w, 24, 23)], {});
        if (bit(w, 21)) in.flags |= kWriteBack;
        in.add(reg(kSp));
        in.add(imm(bits(w, 4, 0)));
        return;
    }
    if ((op1 & 0xE5) == 0x81) {  // rfe (privileged; the reference decodes only the writeback form)
        if (!bit(w, 21)) return undefined(in);
        static constexpr Op k[4] = {Op::Rfeda, Op::Rfeia, Op::Rfedb, Op::Rfeib};
        set(in, k[bits(w, 24, 23)], {reg(static_cast<u8>(bits(w, 19, 16)))});
        if (bit(w, 21)) in.flags |= kWriteBack;
        return;
    }
    if ((op1 & 0xE0) == 0xA0) {  // blx (immediate): to ARM... from ARM to Thumb
        const u32 target = in.address + 8 + (sign_extend(bits(w, 23, 0), 24) << 2) + (bit(w, 24) ? 2 : 0);
        set(in, Op::Blx, {label(target)});
        return;
    }
    if ((op1 & 0xE0) == 0xC0 || (op1 & 0xF0) == 0xE0) return decode_coprocessor(w, in, true);
    undefined(in);
}

}  // namespace

Instruction decode_arm(u32 w, u32 address) {
    Instruction in;
    in.address = address;
    in.raw = w;
    in.mode = Mode::Arm;
    in.size = 4;
    const u32 cond = bits(w, 31, 28);
    if (cond == 0xF) {
        unconditional(w, in);
        return in;
    }
    in.cond = static_cast<Cond>(cond);
    const u32 op1 = bits(w, 27, 25);
    switch (op1) {
        case 0:
        case 1: {
            const u32 o1 = bits(w, 24, 20), o2 = bits(w, 7, 4);
            if (op1 == 1) {
                if (o1 == 0x10) return set(in, Op::Movw, {reg(static_cast<u8>(bits(w, 15, 12))), imm((bits(w, 19, 16) << 12) | bits(w, 11, 0))}), in;
                if (o1 == 0x14) return set(in, Op::Movt, {reg(static_cast<u8>(bits(w, 15, 12))), imm((bits(w, 19, 16) << 12) | bits(w, 11, 0))}), in;
                if ((o1 & 0x1B) == 0x12) return msr_and_hints(w, in), in;
                data_processing(w, in);
                return in;
            }
            if ((o1 & 0x19) == 0x10) {  // 10xx0
                if ((o2 & 8) == 0) miscellaneous(w, in);
                else if ((o2 & 9) == 8) halfword_multiply(w, in);
                else if (o2 == 0x9 && (o1 & 0x10)) synchronization(w, in);
                else if (o2 == 0xB || (o2 & 0xD) == 0xD) extra_load_store(w, in);
                else undefined(in);
                return in;
            }
            if (o2 == 0x9) {
                if ((o1 & 0x10) == 0) multiply(w, in);
                else synchronization(w, in);
                return in;
            }
            if (o2 == 0xB || (o2 & 0xD) == 0xD) {
                extra_load_store(w, in);
                return in;
            }
            if ((o2 & 1) == 0 || (o2 & 9) == 1) {
                data_processing(w, in);
                return in;
            }
            undefined(in);
            return in;
        }
        case 2: load_store(w, in); return in;
        case 3:
            if (bit(w, 4)) media(w, in);
            else load_store(w, in);
            return in;
        case 4:
        case 5: branch_block(w, in); return in;
        default:
            if ((bits(w, 27, 24)) == 0xF) {
                set(in, Op::Svc, {imm(bits(w, 23, 0))});
                return in;
            }
            decode_coprocessor(w, in, false);
            return in;
    }
}

}  // namespace psvrecore::arm
