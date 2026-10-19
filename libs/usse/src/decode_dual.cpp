// Families 4 and 5: dual issue, a main operation and a co-issued one in one word (family 4 on vec3, 5 on vec4).
//
// The word has two destinations: an internal register with a lane mask (bits 28-29, mask in bits 7-9 and, for vec4,
// 54) and a register of any bank (bits 21-27, 32-33); bit 42 says which operation writes which. Its sources come from
// four slots: S (any bank, bits 0-6) and three internal registers G1-G3 (bits 10-11, 12-13, 14-15); the routing bits
// (16-17) say which slot feeds which source, depending on how many sources each operation reads (the table below is
// the reference disassembler's, read back for every combination).

#include "common.hpp"

namespace psvrecore::usse::detail {
namespace {

struct DualOp {
    Op op;
    u8 reads;
    bool vector;
};
// The operation codes (main: bits 44-46, and 54 for vec3; co-issued: bits 18-20 and 43).
constexpr DualOp kOps[16] = {
    {Op::Mad, 3, true},  {Op::Dot, 2, false}, {Op::Dot, 1, false},   {Op::Mul, 2, true},
    {Op::Add, 2, true},  {Op::Mov, 1, true},  {Op::Rsq, 1, false},   {Op::Rcp, 1, false},
    {Op::Mad, 3, false}, {Op::Add, 2, false}, {Op::Mul, 2, false},   {Op::SubFlr, 2, false},
    {Op::Exp, 1, false}, {Op::Log, 1, false}, {Op::Invalid, 0, false}, {Op::Invalid, 0, false},
};
// Code 2 is a dot product of a vector with itself: one read, printed twice.

struct Route {
    u8 main[3], co[3];
};
constexpr u8 kNone = 0xFF;
// [main reads - 1][co reads - 1][routing]: the slot of each source (0: S, 1-3: G1-G3); main slot kNone: invalid.
constexpr Route kRoutes[3][3][4] = {
    {   // one main read
        {{{0}, {1}}, {{1}, {0}}, {{2}, {0}}, {{3}, {0}}},
        {{{0}, {1, 2}}, {{1}, {0, 2}}, {{2}, {1, 0}}, {{3}, {2, 0}}},
        {{{0}, {1, 2, 3}}, {{1}, {0, 2, 3}}, {{2}, {1, 0, 3}}, {{3}, {1, 0, 2}}},
    },
    {   // two
        {{{0, 2}, {1}}, {{1, 0}, {2}}, {{1, 2}, {0}}, {{kNone}, {}}},
        {{{0, 2}, {1, 3}}, {{1, 0}, {2, 3}}, {{1, 2}, {3, 0}}, {{kNone}, {}}},
        {{{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}},
    },
    {   // three
        {{{0, 2, 3}, {1}}, {{1, 0, 3}, {2}}, {{1, 2, 0}, {3}}, {{1, 2, 3}, {0}}},
        {{{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}},
        {{{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}, {{kNone}, {}}},
    },
};

const char* const kVec4[32] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz", "xyxy", "xywz", "zxyw",
                               "zwzw", "0000", "hhhh", "1111", "2222", "yzxw", "zwxy", "xzwy", "yyww", "wyzw", "wzwz",
                               "xyzx", "zzww", "xwzx", "yyyx", "yyyz", "zwzw", "yzxz", "xxyy", "xzww", "xyz1"};
// (the fourth lane of a vec3 swizzle reads x)
const char* const kVec3[32] = {"xxxx", "yyyx", "zzzx", "wwwx", "xyzx", "yzwx", "xxyx", "xyxx", "yyxx", "yyzx", "zxyx",
                               "xzyx", "000x", "hhhx", "111x", "222x", "xyyx", "yxyx", "xxzx", "yxxx", "xy0x", "x10x",
                               "xzyx", "yzxx", "zyxx", "zzyx", "xy1x", "hhhx", "hhhx", "hhhx", "hhhx", "hhhx"};

// A scalar source reads the first lane of its swizzle (a constant lane: x, w, y, z for 0, 0.5, 1, 2).
Comp scalar_comp(Comp c) {
    switch (c) {
        case Comp::Zero: return Comp::X;
        case Comp::Half: return Comp::W;
        case Comp::One: return Comp::Y;
        case Comp::Two: return Comp::Z;
        default: return c;
    }
}

}  // namespace

bool decode_dual(const Word& w, ProgramKind, nx::vector<Instruction>& out) {
    const bool vec4 = (w.w >> 59) == 5;
    const bool f16 = w.bit(53);
    const u32 mcode = static_cast<u32>(w.field(44, 3)) | (vec4 ? 0 : (w.bit(54) << 3));
    const u32 ccode = static_cast<u32>(w.field(18, 3)) | (w.bit(43) << 3);
    const DualOp& m = kOps[mcode];
    const DualOp& c = kOps[ccode];
    if (m.op == Op::Invalid || c.op == Op::Invalid) return false;
    const Route& route = kRoutes[m.reads - 1][c.reads - 1][w.field(16, 2)];
    if (route.main[0] == kNone) return false;
    bool uses_g3 = false;
    for (u8 i = 0; i < m.reads; ++i) uses_g3 |= route.main[i] == 3;
    for (u8 i = 0; i < c.reads; ++i) uses_g3 |= route.co[i] == 3;
    // With a one-read main operation the third internal-register slot is i2, and bits 14-15 belong to S.
    const bool g3_free = m.reads == 1 || !uses_g3;
    const char* const* swz = vec4 ? kVec4 : kVec3;
    const u8 width = vec4 ? 4 : 3;
    const u8 gmask = static_cast<u8>(w.bit(7) | (w.bit(8) << 1) | (w.bit(9) << 2) | (vec4 ? (w.bit(54) << 3) : 0));
    const bool main_writes_gpi = !w.bit(42);
    static constexpr Bank banks[4] = {Bank::Temp, Bank::Output, Bank::Primary, Bank::Secondary};

    auto wide_dest = [&](const DualOp& op) {
        const i32 n = w.field(21, 7);
        const Bank bank = banks[w.field(32, 2)];
        if (bank == Bank::Temp && n >= 124) return reg(bank, n, lanes_identity(width, static_cast<u8>((1 << width) - 1)), true);
        const u8 lanes = op.vector ? (f16 ? 4 : 2) : (f16 ? 2 : 1);
        return reg(bank, n, lanes_identity(lanes, static_cast<u8>((1 << lanes) - 1)), true, op.vector ? 2 : 1);
    };
    auto gpi_dest = [&]() {
        Operand o;
        o.bank = Bank::Internal;
        o.number = w.field(28, 2);
        o.lanes = width;
        o.mask = gmask;
        return o;
    };
    auto source = [&](u8 slot, const DualOp& op, const Operand& dest) {
        Operand o;
        i32 code = -1;  // swizzle code (-1: identity)
        if (slot == 0) {
            const i32 n = w.field(0, 7);
            const Bank bank = banks[w.field(30, 2)];
            o = reg(bank, n, Lanes{}, true, (op.vector || op.op == Op::Dot) ? 2 : 1);
            code = w.field(48, 4) | (g3_free ? static_cast<i32>(w.bit(15) << 4) : 0);
            o.negate = w.bit(47) != 0;
            o.absolute = g3_free && w.bit(14);
        } else {
            o.bank = Bank::Internal;
            if (slot == 1) {
                o.number = w.field(10, 2);
                code = w.field(38, 4);
            } else if (slot == 2) {
                o.number = w.field(12, 2);
                code = w.field(34, 4) | static_cast<i32>(w.bit(52) << 4);
                o.negate = w.bit(58) != 0;
            } else {
                o.number = m.reads == 1 ? 2 : w.field(14, 2);
            }
        }
        const char* text = code >= 0 ? swz[code] : "xyzw";
        if (op.vector || op.op == Op::Dot) {
            o.lanes = op.vector ? dest.lanes : width;
            o.mask = op.vector ? dest.mask : static_cast<u8>((1 << width) - 1);
            for (int i = 0; i < 4; ++i) o.swizzle[i] = comp_of(text[i]);
            if (code < 0 && width == 3) o.swizzle[3] = Comp::X;
        } else {
            const Comp sc = scalar_comp(comp_of(text[0]));
            const u8 lane = static_cast<u8>(sc);
            if (op.op == Op::Rsq || op.op == Op::Rcp || op.op == Op::Exp || op.op == Op::Log) {
                o.lanes = 4;  // (unary: the component is printed in its lane)
                o.mask = static_cast<u8>(1 << lane);
                o.swizzle[lane] = sc;
            } else {
                o.lanes = 1;
                o.mask = 1;
                o.swizzle[0] = sc;
            }
        }
        return o;
    };
    auto make = [&](const DualOp& op, u32 code, const u8* slots, Operand dest, bool coissued) {
        Instruction in;
        in.op = op.op;
        in.type = (!f16 || (coissued && op.op == Op::Mov)) ? Type::F32 : Type::F16;
        in.dest = dest;
        in.src_count = op.reads;
        for (u8 i = 0; i < op.reads; ++i) in.src[i] = source(slots[i], op, dest);
        if (code == 2) {  // dot of a vector with itself
            in.src[1] = in.src[0];
            in.src_count = 2;
        }
        static constexpr Pred preds[4] = {Pred::None, Pred::P0, Pred::NotP0, Pred::Pn};
        in.pred = preds[w.field(56, 2)];
        if (coissued) in.flags |= kCoissued;
        return in;
    };
    const Operand mdest = main_writes_gpi ? gpi_dest() : wide_dest(m);
    const Operand cdest = main_writes_gpi ? wide_dest(c) : gpi_dest();
    out.push_back(make(m, mcode, route.main, mdest, false));
    out.push_back(make(c, ccode, route.co, cdest, true));
    return true;
}

}  // namespace psvrecore::usse::detail
