#include "common.hpp"

#include "constants.hpp"

namespace psvrecore::usse {

const char* mnemonic(Op op) {
    static const char* const names[] = {
#define PSVRECORE_USSE_OP_NAME(name, text) text,
        PSVRECORE_USSE_OPS(PSVRECORE_USSE_OP_NAME)
#undef PSVRECORE_USSE_OP_NAME
    };
    return op < Op::Count ? names[static_cast<u32>(op)] : "<invalid>";
}

const u32* constants() { return detail::kConstantWords; }

}  // namespace psvrecore::usse

namespace psvrecore::usse::detail {

Comp comp_of(char c) {
    switch (c) {
        case 'x': return Comp::X;
        case 'y': return Comp::Y;
        case 'z': return Comp::Z;
        case 'w': return Comp::W;
        case '0': return Comp::Zero;
        case '1': return Comp::One;
        case '2': return Comp::Two;
        default: return Comp::Half;  // 'h'
    }
}

Lanes lanes_text(const char* swizzle, u8 count, u8 mask) {
    Lanes l;
    l.count = count;
    l.mask = mask;
    for (u8 i = 0; i < count && swizzle[i]; ++i) l.comp[i] = comp_of(swizzle[i]);
    return l;
}

static void set_lanes(Operand& o, const Lanes& lanes) {
    o.lanes = lanes.count;
    o.mask = lanes.mask;
    for (u8 i = 0; i < 4; ++i) o.swizzle[i] = lanes.comp[i];
}

bool special_valid(i32 code) {
    return (code >= 64 && code <= 98) || (code >= 100 && code <= 107) || code == 109;
}

Operand reg(Bank bank, i32 num, const Lanes& lanes, bool wide, i32 scale) {
    Operand o;
    const i32 top = wide ? 124 : 60;
    if (bank == Bank::Temp && num >= top) {
        o.bank = Bank::Internal;
        o.number = num - top;
    } else {
        o.bank = bank;
        o.number = num * scale;
    }
    set_lanes(o, lanes);
    return o;
}

Operand operand(Kind kind, i32 num, const Lanes& lanes, View view, bool wide, i32 scale, bool idx_mul) {
    static constexpr Bank banks[4] = {Bank::Temp, Bank::Output, Bank::Primary, Bank::Secondary};
    Operand o;
    switch (kind) {
        case Kind::Constant:
            if (num >= 64) {
                o.bank = Bank::Special;
                o.number = num;
            } else {
                o.bank = Bank::Constant;
                o.number = num;
                o.view = view;
            }
            set_lanes(o, lanes);
            return o;
        case Kind::Literal:
            o.bank = Bank::Literal;
            o.number = num;
            set_lanes(o, lanes);
            return o;
        case Kind::Index1:
        case Kind::Index2: {
            const i32 shift = wide ? 5 : 4;
            o.bank = banks[(num >> shift) & 3];
            o.number = (num & ((1 << shift) - 1)) * scale;
            o.indexed = kind == Kind::Index1 ? 1 : 2;
            o.index_scaled = idx_mul;
            set_lanes(o, lanes);
            return o;
        }
        default: return reg(banks[static_cast<u32>(kind)], num, lanes, wide, scale);
    }
}

Operand index_dest(i32 number) {
    Operand o;
    o.bank = Bank::Index;
    o.number = number;
    return o;
}

}  // namespace psvrecore::usse::detail
