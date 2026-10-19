// The colour/alpha sums of products (families 16 and 17: one word, a colour and an alpha operation) and flow control
// (family 31, which also holds the words that are not operations).

#include "common.hpp"

namespace psvrecore::usse::detail {
namespace {

constexpr Pred kPred3b[8] = {Pred::None, Pred::P0, Pred::P1, Pred::P2, Pred::P3, Pred::NotP0, Pred::NotP1, Pred::Pn};
constexpr Pred kPredSop[4] = {Pred::None, Pred::P0, Pred::P1, Pred::NotP0};

constexpr u8 kRgb = 7, kAlpha = 8;

// Family 17: three sources (s0 an extra factor source), colour and alpha parts sharing the second term's factor.
bool decode_sop2(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Factor factors[8] = {Factor::Zero, Factor::Sat, Factor::A, Factor::AAlpha,
                                          Factor::S0,   Factor::S0Alpha, Factor::B, Factor::BAlpha};
    static constexpr Factor alpha_factors[4] = {Factor::Zero, Factor::S0, Factor::A, Factor::B};
    if (w.bit(53) || w.bit(42)) return false;
    for (int part = 0; part < 2; ++part) {
        const Lanes lanes = lanes_identity(4, part ? kAlpha : kRgb);
        Instruction in;
        in.op = Op::Sop2;
        in.dest = fixed_dest(w, lanes, in.type);
        in.src[0] = fixed_src(w, src_kind(w.field(30, 2), w.bit(49)), 7, lanes);
        in.src[1] = fixed_src(w, src_kind(w.field(28, 2), w.bit(48)), 0, lanes);
        in.src[2] = fixed_src(w, w.bit(34) ? Kind::Primary : Kind::Temp, 14, lanes);
        in.src_count = 3;
        in.term[1] = SopTerm{factors[w.field(35, 3)], w.bit(47) != 0};
        if (part == 0) {
            in.sop_op = w.bit(52) ? Op::Sub : Op::Add;
            in.term[0] = SopTerm{factors[w.field(38, 3)], w.bit(56) != 0};
        } else {
            in.sop_op = w.bit(41) ? Op::Sub : Op::Add;
            in.term[0] = SopTerm{alpha_factors[w.field(44, 2)], w.bit(46) != 0};
            in.flags |= kCoissued;
        }
        in.pred = kPredSop[w.field(57, 2)];
        out.push_back(in);
    }
    return true;
}

// Family 16: two-source blend (colour and alpha parts; the first source may be complemented).
bool decode_blend(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Op ops[4] = {Op::Add, Op::Sub, Op::Min, Op::Max};
    static constexpr Factor rgb[8] = {Factor::Zero, Factor::A, Factor::B, Factor::AAlpha,
                                      Factor::BAlpha, Factor::Sat, Factor::B, Factor::Invalid};
    static constexpr Factor alpha_a[4] = {Factor::Zero, Factor::A, Factor::B, Factor::B};
    static constexpr Factor alpha_b[4] = {Factor::Zero, Factor::A, Factor::B, Factor::Invalid};
    for (int part = 0; part < 2; ++part) {
        const Lanes lanes = lanes_identity(4, part ? kAlpha : kRgb);
        Instruction in;
        in.op = Op::Blend;
        in.dest = fixed_dest(w, lanes, in.type);
        in.src[0] = fixed_src(w, src_kind(w.field(30, 2), w.bit(49)), 7, lanes);
        in.src[1] = fixed_src(w, src_kind(w.field(28, 2), w.bit(48)), 0, lanes);
        in.src_count = 2;
        if (part == 0) {
            in.sop_op = ops[w.field(18, 2)];
            in.term[0] = SopTerm{rgb[w.field(38, 3)], w.bit(56) != 0};
            in.term[1] = SopTerm{rgb[w.field(35, 3)], w.bit(47) != 0};
            in.complement_a = w.bit(20) != 0;
        } else {
            in.sop_op = ops[w.field(16, 2)];
            in.term[0] = SopTerm{alpha_a[w.field(52, 2)], w.bit(43) != 0};
            in.term[1] = SopTerm{alpha_b[w.field(41, 2)], w.bit(34) != 0};
            in.flags |= kCoissued;
        }
        // (an invalid factor only matters in a sum; min and max ignore the factors)
        if (in.sop_op != Op::Min && in.sop_op != Op::Max &&
            (in.term[0].factor == Factor::Invalid || in.term[1].factor == Factor::Invalid))
            return false;
        if (part == 1 && in.term[1].factor == Factor::Invalid) return false;
        in.pred = kPredSop[w.field(57, 2)];
        out.push_back(in);
    }
    return true;
}

Instruction control(Op op, Pred pred = Pred::None) {
    Instruction in;
    in.op = op;
    in.pred = pred;
    return in;
}

// mov.u32 with a 32-bit immediate.
Instruction mov_imm32(const Word& w) {
    Instruction in;
    in.op = Op::MovImm;
    in.type = Type::U32;
    const u32 v = static_cast<u32>(w.field(0, 21)) | (static_cast<u32>(w.field(36, 5)) << 21) |
                  (static_cast<u32>(w.field(44, 6)) << 26);
    static constexpr Kind dest_kind[8] = {Kind::Temp, Kind::Secondary, Kind::Output, Kind::Constant,
                                          Kind::Primary, Kind::Temp, Kind::Index1, Kind::Index2};
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    in.dest = dbank == 5 ? index_dest(dnum) : operand(dest_kind[dbank], dnum, lanes_identity(1, 1), View::Int, true, 1, false);
    in.src[0].bank = Bank::Literal;
    in.src[0].number = static_cast<i32>(v);
    in.src_count = 1;
    in.pred = kPred3b[w.field(41, 3)];
    return in;
}

bool decode_control(const Word& w, nx::vector<Instruction>& out) {
    const i32 s = w.field(56, 3), sub = w.field(52, 3);
    if (sub == 0) {
        const Pred p = kPred3b[s];
        switch (w.field(38, 3)) {
            case 0: {
                Instruction in = control(Op::Ba, p);
                in.target = w.field(0, 20);
                out.push_back(in);
                return true;
            }
            case 1: {
                Instruction in = control(Op::Br, p);
                const i32 off = w.field(0, 20);
                in.target = (off & (1 << 19)) ? off - (1 << 20) : off;  // (relative; decode_program makes it absolute)
                out.push_back(in);
                return true;
            }
            case 2: out.push_back(control(Op::Ret, p)); return true;
            case 3: out.push_back(control(Op::MovToLink, p)); return true;
            case 4: out.push_back(control(Op::MovFromLink, p)); return true;
            case 5: out.push_back(control(Op::Nop, p)); return true;
            default: return false;
        }
    }
    if (sub == 2 || sub == 6) {
        if (s == 4) {
            out.push_back(mov_imm32(w));
            return true;
        }
        if (s <= 3 || s == 5) {
            out.push_back(control(Op::Hidden));
            return true;
        }
        return false;
    }
    if (sub == 3 || sub == 7) {
        if (s == 0) {
            out.push_back(control(Op::PCoeff));
            return true;
        }
        if (s == 1) {
            static constexpr Pred kill_pred[4] = {Pred::None, Pred::NotP0, Pred::NotP1, Pred::P0};
            out.push_back(control(Op::Kill, kill_pred[w.field(41, 2)]));
            return true;
        }
        if (s == 3) {
            Instruction in = control(Op::DepthF);
            in.dest.bank = w.bit(34) ? Bank::Primary : Bank::Temp;
            in.dest.number = w.field(14, 7);
            out.push_back(in);
            return true;
        }
        return false;
    }
    if ((sub == 1 && s < 6) || (sub == 4 && s == 2)) {
        Instruction in = control(Op::Hidden);
        // The word that sets the repeat increments: four signed bytes (src2, src1, src0, dest) and their swizzle-mode
        // flags.
        if (sub == 1 && s == 2) {
            in.op = Op::SetIncrements;
            in.increment[0] = static_cast<i8>(w.field(24, 8));  // dest
            in.increment[1] = static_cast<i8>(w.field(16, 8));  // src0
            in.increment[2] = static_cast<i8>(w.field(8, 8));   // src1
            in.increment[3] = static_cast<i8>(w.field(0, 8));   // src2
            in.increment_swizzle = static_cast<u8>(w.field(32, 4));
        }
        out.push_back(in);
        return true;
    }
    return false;
}

}  // namespace

bool decode_other(const Word& w, ProgramKind, nx::vector<Instruction>& out) {
    switch (static_cast<u32>(w.w >> 59)) {
        case 16: return decode_blend(w, out);
        case 17: return decode_sop2(w, out);
        case 31: return decode_control(w, out);
        default: return false;
    }
}

}  // namespace psvrecore::usse::detail
