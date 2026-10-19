// Arithmetic, move, test, bitwise, fixed-point, integer, texture and memory instructions (every family except dual
// issue, the colour/alpha sums of products and flow control). Field positions and tables were found by reading back
// the reference disassembler's output for chosen words.

#include "common.hpp"

namespace psvrecore::usse::detail {
namespace {

constexpr Pred kPred2[4] = {Pred::None, Pred::P0, Pred::NotP0, Pred::Pn};
constexpr Pred kPred3[8] = {Pred::None, Pred::P0, Pred::P1, Pred::P2, Pred::NotP0, Pred::NotP1, Pred::NotP2, Pred::Pn};
constexpr Pred kPred3b[8] = {Pred::None, Pred::P0, Pred::P1, Pred::P2, Pred::P3, Pred::NotP0, Pred::NotP1, Pred::Pn};
constexpr Pred kPredSop[4] = {Pred::None, Pred::P0, Pred::P1, Pred::NotP0};

// The destination banks of the common destination field (5: an index register).
constexpr Kind kDestKind[8] = {Kind::Temp, Kind::Secondary, Kind::Output, Kind::Constant,
                               Kind::Primary, Kind::Temp, Kind::Index1, Kind::Index2};

View view_of(Type t) { return t == Type::F16 ? View::F16 : t == Type::F32 ? View::F32 : View::Int; }

Operand modified(Operand o, u32 negate, u32 absolute) {
    o.negate = negate != 0;
    o.absolute = absolute != 0;
    return o;
}

u8 mask4(i32 bits) { return static_cast<u8>(bits & 15); }

// Family 0: multiply-add (f32 on two lanes, f16 on four; the internal registers have four f32 lanes).
const char* const kSwz0[3][8] = {
    {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", "xyww", "zwxy"},
    {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xyyz", "yyww", "wyzw"},
    {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xzww", "xxyz", "xyzz"},
};

bool decode_mad0(const Word& w, nx::vector<Instruction>& out) {
    Instruction in;
    in.op = Op::Mad;
    const bool f16 = w.bit(58);
    in.type = f16 ? Type::F16 : Type::F32;
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(22, 6);
    const i32 mask = w.field(39, 4);
    u8 lanes, on;
    if (dbank == 0 && dnum >= 60) {
        lanes = 4, on = mask4(mask);
    } else if (f16) {
        lanes = 4, on = static_cast<u8>((w.bit(39) ? 3 : 0) | (w.bit(41) ? 12 : 0));
    } else {
        lanes = 2, on = static_cast<u8>(w.bit(39) | (w.bit(40) << 1));
    }
    const View view = f16 ? View::F16 : View::F32;
    in.dest = dbank == 5 ? index_dest(dnum * 2) : operand(kDestKind[dbank], dnum, lanes_identity(lanes, on), view);
    in.src[0] = modified(operand(w.bit(34) ? Kind::Primary : Kind::Temp, w.field(12, 6),
                                 lanes_text(kSwz0[0][w.field(18, 2) | (w.bit(53) << 2)], lanes, on), view),
                         0, w.bit(50));
    in.src[1] = modified(operand(src_kind(w.field(30, 2), w.bit(49)), w.field(6, 6),
                                 lanes_text(kSwz0[1][w.field(20, 2) | (w.bit(44) << 2)], lanes, on), view),
                         w.bit(37), w.bit(38));
    in.src[2] = modified(operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 6),
                                 lanes_text(kSwz0[2][w.field(45, 3)], lanes, on), view),
                         w.bit(35), w.bit(36));
    in.src_count = 3;
    in.pred = kPred2[w.field(56, 2)];
    out.push_back(in);
    return true;
}

// The vector destination of families 1-3 (four lanes, a per-lane mask).
Operand vector_dest(const Word& w, View view) {
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(22, 6);
    if (dbank == 5) return index_dest(dnum * 2);
    return operand(kDestKind[dbank], dnum, lanes_identity(4, mask4(w.field(39, 4))), view);
}

constexpr char kExt[] = "xyzw012h";
const char* const kSwz1[16] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz",
                               "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1"};

// Families 1 and 2: f32 and f16 vector operations.
bool decode_alu1(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Op ops[8] = {Op::Mul, Op::Add, Op::Frc, Op::Dsx, Op::Dsy, Op::Min, Op::Max, Op::Dot};
    Instruction in;
    in.op = ops[w.field(12, 3)];
    const bool f16 = w.bit(60);
    in.type = f16 ? Type::F16 : Type::F32;
    const View view = f16 ? View::F16 : View::F32;
    in.dest = vector_dest(w, view);
    const u8 on = in.op == Op::Dot ? 15 : mask4(w.field(39, 4));
    const u8 lanes = 4;
    const i32 sw[4] = {w.field(15, 3), w.field(18, 3), static_cast<i32>(w.bit(21) | (w.bit(34) << 1) | (w.bit(35) << 2)),
                       static_cast<i32>(w.bit(50) | (w.bit(53) << 1) | (w.bit(54) << 2))};
    Lanes l0 = lanes_identity(lanes, on);
    for (int i = 0; i < 4; ++i) l0.comp[i] = comp_of(kExt[sw[i]]);
    in.src[0] = modified(operand(src_kind(w.field(30, 2), w.bit(49)), w.field(6, 6), l0, view), w.bit(37), w.bit(38));
    in.src[1] = modified(
        operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 6), lanes_text(kSwz1[w.field(44, 4)], lanes, on), view),
        0, w.bit(36));
    in.src_count = 2;
    in.pred = kPred3[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Family 3: f32 vector multiply-add and dot product with the internal registers (vec3 codes: the w lane reads x).
const char* const kSwz3[64] = {
    "xxxx", "yyyx", "zzzx", "wwwx", "xyzx", "yzwx", "xxyx", "xyxx", "yyxx", "yyzx", "zxyx", "xzyx", "yzxx",
    "zyxx", "zzyx", "xy1x", "xyyx", "yxyx", "xxzx", "yxxx", "xy0x", "x10x", "000x", "111x", "hhhx", "222x",
    "x00x", "hhhh", "hhhh", "hhhh", "hhhh", "hhhh", "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz",
    "xxyz", "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1", "yzxw", "zwxy", "xzwy", "yyww",
    "wyzw", "wzwz", "xyzx", "zzww", "xwzx", "yyyx", "yyyz", "xzyw", "xxxy", "zyxw", "yyzz", "zzzy",
};

bool decode_fam3(const Word& w, nx::vector<Instruction>& out) {
    Instruction in;
    in.type = Type::F32;
    const u32 vec4 = w.bit(52);
    in.dest = vector_dest(w, View::F32);
    const Kind s0kind = src_kind(w.field(30, 2), w.bit(49));
    if (w.bit(53)) {
        in.op = Op::Mad;
        const u8 on = mask4(w.field(39, 4));
        in.src[0] = modified(operand(s0kind, w.field(0, 6),
                                     lanes_text(kSwz3[w.field(6, 4) | (w.bit(10) << 4) | (vec4 << 5)], 4, on), View::F32),
                             w.bit(38), w.bit(37));
        in.src[1] = modified(reg(Bank::Temp, 60 + w.field(28, 2),
                                 lanes_text(kSwz3[w.field(18, 4) | (w.bit(34) << 4) | (vec4 << 5)], 4, on)),
                             0, w.bit(46));
        in.src[2] = modified(reg(Bank::Temp, 60 + w.field(12, 2),
                                 lanes_text(kSwz3[w.field(14, 4) | (w.bit(54) << 4) | (vec4 << 5)], 4, on)),
                             w.bit(36), w.bit(35));
        in.src_count = 3;
    } else {
        in.op = Op::Dot;
        const u8 n = vec4 ? 4 : 3;
        const u8 on = static_cast<u8>((1 << n) - 1);
        Lanes l0 = lanes_identity(n, on);
        const i32 sw[4] = {w.field(6, 3), w.field(9, 3), w.field(12, 3), w.field(15, 3)};
        for (int i = 0; i < 4; ++i) l0.comp[i] = comp_of(kExt[sw[i]]);
        in.src[0] = modified(operand(s0kind, w.field(0, 6), l0, View::F32), w.bit(38), w.bit(37));
        in.src[1] = modified(reg(Bank::Temp, 60 + w.field(28, 2), lanes_text(kSwz3[w.field(18, 4) | (vec4 << 5)], n, on)),
                             0, w.bit(46));
        in.src_count = 2;
    }
    in.pred = kPred3[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// The 7-bit destination of the scalar unit (bits 21-27) with its mask in bits 0-3.
Operand wide_dest(const Word& w, Type dt) {
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    const i32 mask = w.field(0, 4);
    u8 lanes, on;
    if ((dbank == 0 && dnum >= 124) || (dt != Type::F32 && dbank != 3 && dbank != 6 && dbank != 7)) {
        lanes = 4, on = mask4(mask);
    } else if (dt != Type::F32) {
        lanes = 4, on = static_cast<u8>(((mask & 1) ? 3 : 0) | ((mask & 4) ? 12 : 0));
    } else {
        lanes = 2, on = static_cast<u8>(mask & 3);
    }
    if (dbank == 5) return index_dest(dnum * 2);
    return operand(kDestKind[dbank], dnum, lanes_identity(lanes, on), view_of(dt), true);
}

// Family 6: reciprocal, reciprocal square root, logarithm, exponent (with conversions).
bool decode_fam6(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Type types[4] = {Type::F32, Type::F16, Type::Fx10, Type::None};
    static constexpr Op ops[4] = {Op::Rcp, Op::Rsq, Op::Log, Op::Exp};
    Instruction in;
    in.src_type = types[w.field(39, 2)];
    in.type = types[w.bit(53) | (w.bit(54) << 1)];
    if (in.src_type == Type::None || in.type == Type::None) return false;
    in.op = ops[w.field(41, 2)];
    in.dest = wide_dest(w, in.type);
    const i32 c = w.field(35, 2);
    in.src[0] = modified(operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7),
                                 lanes_identity(4, static_cast<u8>(1 << c)), view_of(in.type), true),
                         w.bit(37), w.bit(38));
    in.src_count = 1;
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

i32 type_scale(Type t) { return (t == Type::F32 || t == Type::F16 || t == Type::Fx10) ? 2 : 1; }

// Family 7: moves and conditional moves.
bool decode_fam7(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Type types[8] = {Type::I8, Type::I16, Type::I32, Type::Fx10, Type::F16, Type::F32, Type::None,
                                      Type::None};
    static constexpr Cond conds[4] = {Cond::EqZero, Cond::NeZero, Cond::LtZero, Cond::LeZero};
    Instruction in;
    in.type = types[w.field(40, 3)];
    const i32 op = w.field(46, 2);
    if (in.type == Type::None || op == 3) return false;
    const Type ty = in.type;
    const u32 dbank = dest_bank(w);
    const i32 mask = w.field(24, 4);
    const bool gpi = dbank == 0 && w.field(18, 6) >= 60;
    u8 lanes, on;
    if (gpi || (ty != Type::F32 && ty != Type::F16)) {
        lanes = 4, on = mask4(mask);
    } else if (ty == Type::F16) {
        lanes = 4, on = static_cast<u8>(((mask & 1) ? 3 : 0) | ((mask & 4) ? 12 : 0));
    } else {
        lanes = 2, on = static_cast<u8>(mask & 3);
    }
    const i32 sc = type_scale(ty);
    const View view = view_of(ty);
    const bool idx_mul = ty != Type::I8;
    const i32 dnum = w.field(18, 6);
    in.dest = dbank == 5 ? index_dest(dnum * sc)
                         : operand(kDestKind[dbank], dnum, lanes_identity(lanes, on), view, false, sc, idx_mul);
    const Lanes sw = lanes_text(kSwz1[w.field(35, 4)], lanes, on);
    const Operand s1 = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(6, 6), sw, view, false, sc, idx_mul);
    in.pred = kPred3b[w.field(56, 3)];
    if (op == 0) {
        in.op = Op::Mov;
        in.src[0] = s1;
        in.src_count = 1;
    } else {
        in.op = op == 1 ? Op::Cmov : Op::Cmov8;
        in.cond = conds[w.bit(39) | (w.bit(54) << 1)];
        in.src[0] = operand(w.bit(34) ? Kind::Primary : Kind::Temp, w.field(12, 6), lanes_identity(lanes, on), view,
                            false, sc);
        in.src[1] = s1;
        in.src[2] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 6), sw, view, false, sc, idx_mul);
        in.src_count = 3;
    }
    out.push_back(in);
    return true;
}

// Family 8: moves and packs between types.
bool decode_fam8(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Type types[8] = {Type::U8, Type::S8, Type::O8, Type::U16, Type::S16, Type::F16, Type::F32, Type::Fx10};
    Instruction in;
    const Type dt = types[w.field(38, 3)], st = types[w.field(41, 3)];
    in.op = dt == st ? Op::Mov : Op::Pack;
    in.type = dt;
    in.src_type = st;
    if (w.bit(18)) in.flags |= kScale;
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    const u8 on = static_cast<u8>(w.field(34, 4));
    in.dest = dbank == 5 ? index_dest(dnum)
                         : operand(kDestKind[dbank], dnum, lanes_identity(4, on), view_of(dt), true, 1,
                                   dt == Type::F32 || dt == Type::F16 || dt == Type::Fx10);
    const Kind skind = src_kind(w.field(30, 2), w.bit(49));
    const View view = view_of(st);
    in.src_count = 1;
    if (st == Type::F32) {
        // Two 64-bit halves: a (window words 0, 1) and b (words 2, 3); the lanes select window words.
        const Kind bkind = src_kind(w.field(28, 2), w.bit(48));
        const i32 a = w.field(8, 6), b = w.field(1, 6);
        const i32 sel[4] = {static_cast<i32>(w.bit(0) | (w.bit(7) << 1)), w.field(16, 2), w.field(14, 2),
                            w.field(19, 2)};
        i32 l[4] = {sel[0], sel[1], sel[2], sel[3]};
        if (dt != Type::F32 && dt != Type::F16 && dt != Type::Fx10) {
            // (packing to integers: the selects go to the used lanes in order)
            int k = 0;
            for (int i = 0; i < 4; ++i) l[i] = ((on >> i) & 1) ? sel[k++] : 0;
        }
        bool uses_b = false;
        for (int i = 0; i < 4; ++i) uses_b |= ((on >> i) & 1) && l[i] >= 2;
        const bool gpi = skind == Kind::Temp && a >= 60;
        const bool plain = static_cast<u32>(skind) < 4 && bkind == skind;
        Lanes lanes = lanes_identity(4, on);
        if (gpi || !uses_b || (plain && b == a + 1)) {
            for (int i = 0; i < 4; ++i) lanes.comp[i] = static_cast<Comp>(l[i]);
            in.src[0] = operand(skind, a, lanes, view);
        } else if (plain && b == a) {
            for (int i = 0; i < 4; ++i) lanes.comp[i] = static_cast<Comp>(l[i] & 1);
            in.src[0] = operand(skind, a, lanes, view);
            in.flags |= kSameHalves;
        } else {
            for (int i = 0; i < 4; ++i) lanes.comp[i] = static_cast<Comp>(l[i]);
            in.src[0] = operand(skind, a, lanes, view);
            in.src[1] = operand(bkind, b, lanes, view);
            in.src_count = 2;
            in.flags |= kPairSource;
        }
    } else {
        i32 sel[4] = {w.field(0, 2), w.field(16, 2), w.field(14, 2), w.field(19, 2)};
        i32 num, sc;
        bool wide;
        if (st == Type::F16 || st == Type::Fx10) {
            num = w.field(8, 6), wide = false, sc = 2;
        } else {
            num = w.field(7, 7), wide = true, sc = 1;
            if (dt == st) sel[2] = sel[0], sel[3] = sel[1];
        }
        Lanes lanes = lanes_identity(4, on);
        if (dt == Type::F32 || dt == Type::F16 || st == Type::F16) {
            for (int i = 0; i < 4; ++i) lanes.comp[i] = static_cast<Comp>(sel[i]);
        } else {
            // (the selects go to the used lanes in order)
            int k = 0;
            for (int i = 0; i < 4; ++i) lanes.comp[i] = ((on >> i) & 1) ? static_cast<Comp>(sel[k++]) : Comp::X;
        }
        in.src[0] = operand(skind, num, lanes, view, wide, sc, sc == 2);
    }
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Families 9 and 15: tests (an operation whose result's comparison with zero sets a predicate, or a register).
bool decode_test(const Word& w, bool reg_dest, nx::vector<Instruction>& out) {
    struct TestOp {
        Op op;
        Type type;  // None: from the type bits
    };
    static const TestOp ops[64] = {
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Add, Type::None}, {Op::Frc, Type::None},
        {Op::Rcp, Type::F32}, {Op::Rsq, Type::F32}, {Op::Log, Type::F32}, {Op::Exp, Type::F32},
        {Op::Dot, Type::None}, {Op::Min, Type::None}, {Op::Max, Type::None}, {Op::Dsx, Type::None},
        {Op::Dsy, Type::None}, {Op::Mul, Type::None}, {Op::Cmp, Type::None}, {Op::Invalid, Type::None},
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None},
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Add, Type::I16}, {Op::Cmp, Type::I16},
        {Op::Mul, Type::I16}, {Op::Add, Type::U16}, {Op::Cmp, Type::U16}, {Op::Mul, Type::U16},
        {Op::Add, Type::I32}, {Op::Add, Type::U32}, {Op::Cmp, Type::I32}, {Op::Cmp, Type::U32},
        {Op::Add, Type::U8}, {Op::Cmp, Type::U8}, {Op::Add, Type::U8}, {Op::Cmp, Type::U8},
        {Op::Mul, Type::U8}, {Op::Mul, Type::U8}, {Op::Mul, Type::U8}, {Op::Add, Type::U8},
        {Op::Cmp, Type::U8}, {Op::Sablnd, Type::U8}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None},
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None},
        {Op::And, Type::None}, {Op::Or, Type::None}, {Op::Xor, Type::None}, {Op::Shl, Type::None},
        {Op::Shr, Type::None}, {Op::Rol, Type::None}, {Op::Invalid, Type::None}, {Op::Asr, Type::None},
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None},
        {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None}, {Op::Invalid, Type::None},
    };
    static const i8 cond_of[32] = {-1, 0, -1, 1, -1, 2, -1, -1, -1, 3, 4, -1, -1, 5, -1, -1,
                                   -1, 6, 7, -1, -1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    static constexpr Cond conds[9] = {Cond::Always, Cond::EqZero, Cond::NeZero, Cond::PosZero, Cond::LeZero,
                                      Cond::LtZero, Cond::NegZero, Cond::GeZero, Cond::GtZero};
    const i32 code = w.field(14, 6);
    const TestOp t = ops[code];
    const i8 c5 = cond_of[w.field(39, 5)];
    if (t.op == Op::Invalid || c5 < 0) return false;
    Instruction in;
    in.op = Op::Test;
    in.alu = t.op;
    in.cond = conds[c5];
    if (t.type != Type::None) in.type = t.type;
    else if (code >= 48) in.type = w.bit(50) ? Type::U16 : Type::U32;
    else in.type = w.bit(47) ? Type::F32 : Type::F16;
    const bool flt = in.type == Type::F32 || in.type == Type::F16;
    const bool unary = code >= 4 && code <= 7;
    bool vec = flt && !unary && (w.bit(38) || t.op == Op::Dot);
    i32 c = w.field(36, 2);
    if (reg_dest) {
        vec = flt && !unary;
        if (c == 3) return false;
    } else if (flt && !unary && w.bit(38)) {
        static constexpr TestMode modes[4] = {TestMode::AndAll, TestMode::OrAll, TestMode::PerLane, TestMode::Or2};
        in.test_mode = modes[c];
    }
    Lanes la, lb;
    if (reg_dest && vec) {
        la = lanes_identity(4, 15);
        lb = lanes_text(w.bit(46) ? "xxxx" : "xyzw", 4, 15);
    } else if (vec) {
        const u8 n = (w.bit(38) && c == 3 && t.op != Op::Dot) ? 2 : 4;
        la = lanes_identity(n, static_cast<u8>((1 << n) - 1));
        lb = lanes_text(w.bit(46) ? "xxxx" : "xyzw", n, la.mask);
    } else if (code >= 48 && in.type == Type::U16) {
        la = lb = lanes_identity(2, 3);
    } else {
        c = (flt && !unary) ? c : 0;
        la = lanes_identity(1, 1);
        la.comp[0] = static_cast<Comp>(c);
        lb = la;
        if (w.bit(46)) lb.comp[0] = Comp::X;
    }
    const i32 sc = flt ? 2 : 1;
    const View view = flt ? view_of(in.type) : View::Int;
    i32 na, nb;
    bool wide;
    if (!(code >= 40 && code <= 41 && w.bit(50))) {
        na = w.field(7, 7), nb = w.field(0, 7), wide = true;
    } else {
        na = w.field(7, 6), nb = w.field(0, 6), wide = false;
    }
    in.src[0] = operand(src_kind(w.field(30, 2), w.bit(49)), na, la, view, wide, sc, flt);
    if (flt) in.src[0].negate = w.bit(50) != 0;
    in.src_count = 1;
    if (!unary) {
        in.src[1] = operand(src_kind(w.field(28, 2), w.bit(48)), nb, lb, view, wide, sc, flt);
        in.src_count = 2;
    }
    if (reg_dest) {
        const u32 dbank = dest_bank(w);
        const i32 dn = w.field(21, 7);
        const u8 dl = (flt && c == 2) ? 4 : 1;
        in.dest = dbank == 5 ? index_dest(dn * sc)
                             : operand(kDestKind[dbank], dn, lanes_identity(dl, static_cast<u8>((1 << dl) - 1)), view,
                                       true, sc, flt);
    } else {
        in.dest.bank = Bank::Predicate;
        in.dest.number = w.field(34, 2);
    }
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Families 10-14: bitwise operations, shifts, rotates.
bool decode_bitwise(const Word& w, u32 family, nx::vector<Instruction>& out) {
    static constexpr Op ops[5][2] = {{Op::And, Op::Or}, {Op::Xor, Op::Xor}, {Op::Shl, Op::Rol}, {Op::Shr, Op::Asr},
                                     {Op::Rlp, Op::Rlp}};
    Instruction in;
    in.op = ops[family - 10][w.bit(35)];
    const bool u16 = w.bit(34);
    in.type = u16 ? Type::U16 : Type::U32;
    const Lanes lanes = u16 ? lanes_identity(2, 3) : lanes_identity(1, 1);
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    in.dest = dbank == 5 ? index_dest(dnum) : operand(kDestKind[dbank], dnum, lanes, View::Int, true, 1, false);
    in.src[0] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), lanes, View::Int, true, 1, false);
    const Kind bk = src_kind(w.field(28, 2), w.bit(48));
    if (in.op == Op::Rlp) {
        in.src[1].bank = Bank::Predicate;
        in.src[1].number = w.field(14, 7);
    } else if (bk == Kind::Literal) {
        u32 v = static_cast<u32>(w.field(0, 7) | (w.field(14, 7) << 7) | (w.field(36, 2) << 14));
        const u32 r = static_cast<u32>(w.field(38, 5));
        if (r) v = (v << r) | (v >> (32 - r));
        in.src[1].bank = Bank::Literal;
        in.src[1].number = static_cast<i32>(v);
    } else {
        in.src[1] = operand(bk, w.field(0, 7), lanes, View::Int, true, 1, false);
    }
    if (w.bit(43) && in.op != Op::Rlp) in.flags |= kInvertB;
    in.src_count = 2;
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// The fixed-point families' 7-bit temporaries: 124-127 are the internal registers, the others alias to 6 bits.
Operand fixed_reg(i32 n, const Lanes& lanes) {
    Operand o;
    if (n >= 124) {
        o.bank = Bank::Internal;
        o.number = n - 124;
    } else {
        o.bank = Bank::Temp;
        o.number = n & 63;
    }
    o.lanes = lanes.count;
    o.mask = lanes.mask;
    for (int i = 0; i < 4; ++i) o.swizzle[i] = lanes.comp[i];
    return o;
}

}  // namespace

// The destination of the fixed-point families: a direct register has a 6-bit number and the type in bit 27 (fx10
// when set), anything else a 7-bit number (and fx8).
Operand fixed_dest(const Word& w, const Lanes& lanes, Type& type) {
    const u32 dbank = dest_bank(w);
    const bool direct = dbank == 0 || dbank == 1 || dbank == 2 || dbank == 4 || dbank == 5;
    i32 dnum = w.field(21, 7);
    if (dbank == 0) {
        type = (dnum >= 124 || !w.bit(27)) ? Type::Fx8 : Type::Fx10;
    } else if (direct) {
        type = w.bit(27) ? Type::Fx10 : Type::Fx8;
        dnum &= 63;
    } else {
        type = Type::Fx8;
    }
    if (dbank == 5) return index_dest(dnum);
    if (dbank == 0) return fixed_reg(dnum, lanes);
    return operand(kDestKind[dbank], dnum, lanes, View::Int, !direct, 1, false);
}

// A source of the fixed-point families: direct registers and immediates have 6-bit numbers.
Operand fixed_src(const Word& w, Kind kind, u32 lo, const Lanes& lanes) {
    i32 n = w.field(lo, 7);
    if (kind == Kind::Temp) return fixed_reg(n, lanes);
    const bool wide = kind == Kind::Index1 || kind == Kind::Index2 || kind == Kind::Constant;
    if (!wide) n &= 63;
    return operand(kind, n, lanes, View::Int, wide, 1, false);
}

namespace {

// Family 18: fixed-point sum of products: op(a * fa, b * fb).
bool decode_sop(const Word& w, nx::vector<Instruction>& out) {
    static constexpr Op ops[4] = {Op::Add, Op::Sub, Op::Min, Op::Max};
    static constexpr Factor factors[8] = {Factor::Zero,    Factor::Sat, Factor::A,     Factor::AAlpha,
                                          Factor::Invalid, Factor::Invalid, Factor::B, Factor::BAlpha};
    Instruction in;
    in.op = Op::Sop;
    in.sop_op = ops[w.field(52, 2)];
    const u8 on = static_cast<u8>(w.bit(44) | (w.bit(45) << 1) | (w.bit(46) << 2) | (w.bit(43) << 3));
    const Lanes lanes = lanes_identity(4, on);
    in.dest = fixed_dest(w, lanes, in.type);
    in.src[0] = fixed_src(w, src_kind(w.field(30, 2), w.bit(49)), 7, lanes);
    in.src[1] = fixed_src(w, src_kind(w.field(28, 2), w.bit(48)), 0, lanes);
    in.src_count = 2;
    in.term[0] = SopTerm{factors[w.field(38, 3)], w.bit(56) != 0};
    in.term[1] = SopTerm{factors[w.field(35, 3)], w.bit(47) != 0};
    if (in.term[0].factor == Factor::Invalid || in.term[1].factor == Factor::Invalid) return false;
    in.pred = kPredSop[w.field(57, 2)];
    out.push_back(in);
    return true;
}

// Family 25: fixed-point (fx10) or byte (u8) multiply-add on four lanes.
bool decode_mad25(const Word& w, nx::vector<Instruction>& out) {
    Instruction in;
    in.op = Op::FxMad;
    in.type = w.bit(27) ? Type::Fx10 : Type::U8;
    if (w.bit(43)) in.flags |= kSat;
    const Lanes all = lanes_identity(4, 15);
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 6);
    in.dest = dbank == 5 ? index_dest(dnum) : operand(kDestKind[dbank], dnum, all, View::Int, false, 1, false);
    in.src[0] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 6), all, View::Int, false, 1, false);
    in.src[1] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 6), all, View::Int, false, 1, false);
    in.src[2] = modified(operand(w.bit(34) ? Kind::Primary : Kind::Temp, w.field(14, 6), all, View::Int, false, 1), w.bit(35),
                         0);
    in.src_count = 3;
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Family 28: texture sampling.
bool decode_tex(const Word& w, nx::vector<Instruction>& out) {
    const i32 dim = w.field(42, 2), ty = w.field(46, 2);
    if (dim == 3 || ty == 1 || w.field(35, 2) == 3) return false;
    Instruction in;
    in.op = Op::Tex;
    in.tex_dim = static_cast<TexDim>(dim);
    in.tex_kind = static_cast<TexKind>(w.field(40, 2));
    in.type = ty == 0 ? Type::None : ty == 2 ? Type::F16 : Type::F32;
    if (w.bit(37)) in.flags |= kTexRsd;
    if (w.bit(38)) in.flags |= kTexInfo;
    if (w.bit(51)) in.flags |= kTexMinP;
    in.dest.bank = w.bit(39) ? Bank::Primary : Bank::Temp;
    in.dest.number = w.field(21, 7);
    const u8 n = static_cast<u8>(dim + 1);
    static constexpr Kind coord_kind[4] = {Kind::Temp, Kind::Primary, Kind::Output, Kind::Secondary};
    in.src[0] = operand(coord_kind[w.bit(34) | (w.bit(50) << 1)], w.field(14, 7),
                        lanes_identity(n, static_cast<u8>((1 << n) - 1)), View::F32, true);
    in.src[1] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), Lanes{}, View::Int, true, 2, false);
    in.src_count = 2;
    if (in.tex_kind != TexKind::Plain) {
        in.src[2] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 7), Lanes{}, View::F32, true, 2, false);
        in.src_count = 3;
    }
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Families 29 and 30: loads and stores.
bool decode_memory(const Word& w, bool store, nx::vector<Instruction>& out) {
    static constexpr u8 sizes[4] = {32, 16, 8, 64};
    const i32 space = w.field(42, 2);
    if (space == 3) return false;
    Instruction in;
    in.op = store ? Op::Store : Op::Load;
    in.space = static_cast<Space>(space);
    in.size = sizes[w.field(36, 2)];
    if (!w.bit(53)) in.fetch = static_cast<u8>(w.field(44, 4) + 1);
    if (!store) {
        in.dest.bank = w.bit(39) ? Bank::Primary : Bank::Temp;
        in.dest.number = w.field(21, 7);
    }
    static constexpr Bank base_bank[4] = {Bank::Temp, Bank::Primary, Bank::Output, Bank::Secondary};
    in.src[0] = reg(base_bank[w.bit(34) | (w.bit(50) << 1)], w.field(14, 7), Lanes{}, true, 1);
    in.src[1] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), Lanes{}, View::Int, true, 1, false);
    in.src_count = 2;
    const i32 inc = w.field(40, 2);
    if (inc == 1) in.flags |= kPreIncrement;
    if (inc == 2) in.flags |= kPostIncrement;
    if (w.bit(35)) in.flags |= kDecrement;
    if (store) {
        in.src[2] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 7), Lanes{}, View::Int, true, 1, false);
        in.src_count = 3;
    }
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

// Families 20 and 21: integer multiply-adds of bytes (16-bit form) or 16-bit halves (32-bit form).
bool decode_imad(const Word& w, bool i16, nx::vector<Instruction>& out) {
    Instruction in;
    in.op = Op::IMad;
    in.type = i16 ? (w.bit(43) ? Type::I16 : Type::U16) : (w.bit(43) ? Type::I32 : Type::U32);
    if (w.bit(42)) in.flags |= kSat;
    const Lanes lanes = i16 ? lanes_identity(2, 3) : lanes_identity(1, 1);
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    in.dest = dbank == 5 ? index_dest(dnum) : operand(kDestKind[dbank], dnum, lanes, View::Int, true, 1, false);
    static constexpr Bank s0_bank[2] = {Bank::Temp, Bank::Primary};
    in.src_count = 3;
    if (i16) {
        in.dest.absolute = w.bit(56) != 0;
        in.src[0] = reg(s0_bank[w.bit(34)], w.field(14, 7), lanes_identity(2, 3), true, 1);
        in.src[1] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), Lanes{}, View::Int, true, 1, false);
        if (w.field(38, 2)) in.src[1].part = w.bit(52) ? Part::Hi8 : Part::Lo8;
        in.src[2] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 7), lanes, View::Int, true, 1, false);
        if (w.bit(40) || w.bit(41)) in.src[2].part = w.bit(37) ? Part::Hi8 : Part::Lo8;
        in.src[2].negate = w.bit(53) != 0;
        in.pred = kPredSop[w.field(57, 2)];
    } else {
        in.src[0] = reg(s0_bank[w.bit(34)], w.field(14, 7), Lanes{}, true, 1);
        in.src[0].part = w.bit(56) ? Part::Hi16 : Part::Lo16;
        in.src[1] = operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), Lanes{}, View::Int, true, 1, false);
        in.src[1].part = w.bit(53) ? Part::Hi16 : Part::Lo16;
        in.src[2] = operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 7), lanes, View::Int, true, 1, false);
        in.pred = kPredSop[w.field(57, 2)];
    }
    out.push_back(in);
    return true;
}

// Family 26: 32-bit integer multiply-add (.s0/.s1).
bool decode_imad26(const Word& w, nx::vector<Instruction>& out) {
    if (w.bit(53)) return false;
    Instruction in;
    in.op = Op::IMad;
    in.type = w.bit(41) ? Type::I32 : Type::U32;
    in.flags |= kScaleVariant;
    if (w.bit(52)) in.flags |= kSignedShift;
    const Lanes x = lanes_identity(1, 1);
    const u32 dbank = dest_bank(w);
    const i32 dnum = w.field(21, 7);
    in.dest = dbank == 5 ? index_dest(dnum) : operand(kDestKind[dbank], dnum, x, View::Int, true, 1, false);
    static constexpr Bank s0_bank[4] = {Bank::Temp, Bank::Primary, Bank::Output, Bank::Secondary};
    in.src[0] = reg(s0_bank[w.bit(34) | (w.bit(47) << 1)], w.field(14, 7), x, true, 1);
    in.src[1] = modified(operand(src_kind(w.field(30, 2), w.bit(49)), w.field(7, 7), x, View::Int, true, 1, false),
                         w.bit(40), 0);
    in.src[2] = modified(operand(src_kind(w.field(28, 2), w.bit(48)), w.field(0, 7), x, View::Int, true, 1, false),
                         w.bit(39), 0);
    in.src_count = 3;
    in.pred = kPred3b[w.field(56, 3)];
    out.push_back(in);
    return true;
}

}  // namespace

bool decode_alu(const Word& w, ProgramKind, nx::vector<Instruction>& out) {
    const u32 family = static_cast<u32>(w.w >> 59);
    switch (family) {
        case 0: return decode_mad0(w, out);
        case 1:
        case 2: return decode_alu1(w, out);
        case 3: return decode_fam3(w, out);
        case 6: return decode_fam6(w, out);
        case 7: return decode_fam7(w, out);
        case 8: return decode_fam8(w, out);
        case 9: return decode_test(w, false, out);
        case 10:
        case 11:
        case 12:
        case 13:
        case 14: return decode_bitwise(w, family, out);
        case 15: return decode_test(w, true, out);
        case 18: return decode_sop(w, out);
        case 20:
        case 21: return decode_imad(w, family == 20, out);
        case 25: return decode_mad25(w, out);
        case 26: return decode_imad26(w, out);
        case 28: return decode_tex(w, out);
        case 29:
        case 30: return decode_memory(w, family == 30, out);
        default: return false;
    }
}

}  // namespace psvrecore::usse::detail
