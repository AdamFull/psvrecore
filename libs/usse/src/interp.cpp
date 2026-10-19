// The USSE reference interpreter.
//
// Operands are read lane by lane. A lane names a component of its register (or a constant: 0, 1, 2, 0.5), and its
// width follows the data type: 32-bit types address words (register number + component), 16-bit types halves
// (2 * number + component), 8-bit types bytes (4 * number + component). An internal register is four 32-bit words.
// Results are written after all of an instruction's sources are read; the two operations of a dual-issue word read
// their sources before either writes.
//
// Where the semantics are not certain they are marked "(checked)" once the same-source tests cover them, and
// "(assumed)" otherwise.

#include "psvrecore/usse/interp.hpp"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/foundation/strings/format.h"

namespace psvrecore::usse {

u16 to_half(f32 v) {
    const u32 x = std::bit_cast<u32>(v);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 exp = static_cast<i32>((x >> 23) & 0xFF);
    u32 mant = x & 0x7FFFFFu;
    if (exp == 0xFF) return static_cast<u16>(sign | 0x7C00u | (mant ? 0x200u | (mant >> 13) : 0));
    i32 e = exp - 127 + 15;
    if (e >= 31) return static_cast<u16>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<u16>(sign);
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 h = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return static_cast<u16>(sign | h);
    }
    u32 h = (static_cast<u32>(e) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;  // (a carry into the exponent is still correct)
    return static_cast<u16>(sign | h);
}

f32 from_half(u16 h) {
    const u32 sign = (u32{h} & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1F, mant = h & 0x3FFu;
    if (exp == 0) {
        if (!mant) return std::bit_cast<f32>(sign);
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<f32>(mant), -24);
    }
    if (exp == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (mant << 13));
    return std::bit_cast<f32>(sign | ((exp + 112) << 23) | (mant << 13));
}

namespace {

u32 width_of(Type t) {
    switch (t) {
        case Type::F16:
        case Type::I16:
        case Type::U16:
        case Type::S16: return 16;
        case Type::I8:
        case Type::U8:
        case Type::S8:
        case Type::O8:
        case Type::Fx8: return 8;
        case Type::Fx10: return 10;
        default: return 32;
    }
}

// Fixed point: fx8 is a byte with 1.0 = 255; fx10 a signed 10-bit value with 1.0 = 255 (range about [-2, 2]), the
// lanes x, y, z in bits 0-9, 10-19, 20-29 of a register and w in bits 0-9 of the next (checked: the compiler's fx10
// literals, e.g. 1.0 = 0x0ff3fcff, 0x000000ff).
i32 fx_value(u32 raw, Type t) {
    if (t == Type::Fx10) return static_cast<i32>(raw << 22) >> 22;
    return static_cast<i32>(raw & 0xFF);
}
u32 fx_raw(i32 v, Type t, bool saturate = true) {
    if (t == Type::Fx10) return static_cast<u32>(saturate ? nx::clamp<i32>(v, -512, 511) : v) & 0x3FF;
    return static_cast<u32>(saturate ? nx::clamp<i32>(v, 0, 255) : v) & 0xFF;
}
// The product of two fixed-point values (1.0 = 255), rounded (assumed).
i32 fx_mul(i32 a, i32 b) {
    const i32 p = a * b;
    return p >= 0 ? (p + 127) / 255 : -((-p + 127) / 255);
}

bool is_float(Type t) { return t == Type::F32 || t == Type::F16; }

// A register lane: the word it lives in and its bit range (and, in a 32-bit lane holding a narrower value, the
// value's width).
struct Slot {
    u32* word = nullptr;
    u8 shift = 0;
    u8 bits = 32;
    u8 value_bits = 32;
    bool clear = false;  // a write clears the rest of the word
};

struct Pending {
    Slot slot;
    u32 value;
};

class Exec {
public:
    Exec(Machine& m, ProgramKind kind) : m_(m), kind_(kind) {}

    nx::string error;
    nx::vector<Pending> writes;

    // The register (32-bit word) an operand's register number plus `word` addresses.
    u32* reg_word(const Operand& o, i64 word) {
        i64 n = word;
        if (o.indexed) n += i64{m_.index[o.indexed - 1]} * (o.index_scaled ? 2 : 1);
        u32* bank = nullptr;
        switch (o.bank) {
            case Bank::Temp: bank = m_.temp; break;
            case Bank::Output: bank = m_.output; break;
            case Bank::Primary: bank = kind_ == ProgramKind::Secondary ? m_.secondary : m_.primary; break;
            case Bank::Secondary: bank = m_.secondary; break;
            default: break;
        }
        if (!bank || n < 0 || n >= Machine::kBankSize) {
            fail(nx::format("register {} of bank {} outside the bank", n, static_cast<u32>(o.bank)));
            return nullptr;
        }
        return bank + n;
    }

    // The lane of a register operand reading component c (0-3) as a `bits`-wide value.
    Slot slot(const Operand& o, u32 c, u32 bits) {
        if (o.bank == Bank::Internal) {
            // An internal register has four 32-bit lanes. f16 values are held as f32 (see raw and put); 16-bit
            // integer lanes are the halves of the words in order, and a write to the first half of a word clears
            // the second (checked: the compiler packs a u16 index into lane x, then shifts lanes x and y as a u16
            // pair and uses the word); other widths take a lane each, zero-extended.
            if (o.number < 0 || o.number > 3) return fail("internal register outside 0-3"), Slot{};
            if (bits == 16)
                return Slot{&m_.internal[o.number][(c >> 1) & 3], static_cast<u8>((c & 1) * 16), 16, 16, (c & 1) == 0};
            return Slot{&m_.internal[o.number][c & 3], 0, 32, static_cast<u8>(bits)};
        }
        const i64 n = o.number;
        if (bits == 10) {  // (fx10)
            u32* w = reg_word(o, n + (c == 3 ? 1 : 0));
            return w ? Slot{w, static_cast<u8>(c < 3 ? 10 * c : 0), 10} : Slot{};
        }
        if (bits == 32) {
            u32* w = reg_word(o, n + c);
            return w ? Slot{w, 0, 32} : Slot{};
        }
        if (bits == 16) {
            const i64 h = 2 * n + c;
            u32* w = reg_word(o, h >> 1);
            return w ? Slot{w, static_cast<u8>((h & 1) * 16), 16} : Slot{};
        }
        const i64 b = 4 * n + c;
        u32* w = reg_word(o, b >> 2);
        return w ? Slot{w, static_cast<u8>((b & 3) * 8), 8} : Slot{};
    }

    // The raw bits of lane `lane` of a source read as type t (a constant component gives t's 0, 1, 2 or 0.5).
    u32 raw(const Operand& o, u32 lane, Type t) {
        const Comp c = o.lanes ? o.swizzle[lane & 3] : Comp::X;
        const u32 bits = width_of(t);
        if (c >= Comp::Zero) {
            static constexpr f32 k[4] = {0.0f, 1.0f, 2.0f, 0.5f};
            const f32 v = k[static_cast<u32>(c) - static_cast<u32>(Comp::Zero)];
            if (t == Type::F32) return std::bit_cast<u32>(v);
            if (t == Type::F16) return to_half(v);
            return static_cast<u32>(v);
        }
        const u32 comp = static_cast<u32>(c);
        switch (o.bank) {
            case Bank::Literal: return static_cast<u32>(o.number);
            case Bank::Constant: {
                const u32* table = constants();
                const u32 base = static_cast<u32>(o.number) * 2;
                if (bits == 32) return base + comp < 130 ? table[base + comp] : 0;
                const u32 word = base + comp / 2;
                return word < 130 ? (table[word] >> ((comp & 1) * 16)) & 0xFFFF : 0;
            }
            case Bank::Special: return special(o.number);
            case Bank::Internal:
                // (an internal register holds f32 values: an f16 read narrows; checked, see put)
                if (t == Type::F16) {
                    const Slot s = slot(o, comp, 32);
                    return s.word ? to_half(std::bit_cast<f32>(*s.word)) : 0;
                }
                [[fallthrough]];
            default: {
                const Slot s = slot(o, comp, bits);
                if (!s.word) return 0;
                if (s.bits == 32) return s.value_bits == 32 ? *s.word : *s.word & ((1u << s.value_bits) - 1);
                return (*s.word >> s.shift) & ((1u << s.bits) - 1);
            }
        }
    }

    f32 fval(const Operand& o, u32 lane, Type t) {
        const u32 r = raw(o, lane, t);
        f32 v = t == Type::F16 ? from_half(static_cast<u16>(r)) : std::bit_cast<f32>(r);
        if (o.absolute) v = std::fabs(v);
        if (o.negate) v = -v;
        return v;
    }

    void put(const Operand& d, u32 lane, Type t, u32 value) {
        if (d.bank == Bank::Index) {
            const u32 which = d.number >= 2 ? 1 : 0;  // (assumed)
            m_.index[which] = static_cast<i32>(value);
            return;
        }
        // An f16 result written to an internal register is widened to f32 (checked: the compiler mixes f16 and f32
        // operations on the internal registers without conversions).
        if (d.bank == Bank::Internal && t == Type::F16) {
            const Slot s = slot(d, lane, 32);
            if (s.word) writes.push_back(Pending{s, std::bit_cast<u32>(from_half(static_cast<u16>(value)))});
            return;
        }
        const Slot s = slot(d, lane, width_of(t));
        if (s.word) writes.push_back(Pending{s, value});
    }

    void putf(const Operand& d, u32 lane, Type t, f32 v) {
        put(d, lane, t, t == Type::F16 ? to_half(v) : std::bit_cast<u32>(v));
    }

    void commit() {
        for (const Pending& p : writes) {
            if (p.slot.bits == 32) {
                *p.slot.word = p.value;
            } else if (p.slot.clear) {
                *p.slot.word = (p.value << p.slot.shift) & (((1u << p.slot.bits) - 1) << p.slot.shift);
            } else {
                const u32 mask = ((1u << p.slot.bits) - 1) << p.slot.shift;
                *p.slot.word = (*p.slot.word & ~mask) | ((p.value << p.slot.shift) & mask);
            }
        }
        writes.clear();
    }

    bool fail(nx::string message) {
        if (error.empty()) error = std::move(message);
        return false;
    }

    u32 special(i32 code) {
        (void)code;
        return 0;  // (assumed: the special registers read as zero in a single instance)
    }

    Machine& m_;
    ProgramKind kind_;
};

u32 lowest_lane(u8 mask) { return mask ? static_cast<u32>(std::countr_zero(static_cast<u32>(mask))) : 0; }

f32 alu(Op op, f32 a, f32 b, f32 c) {
    switch (op) {
        case Op::Mad: return a * b + c;  // (assumed: unfused, rounded twice)
        case Op::Add: return a + b;
        case Op::Mul: return a * b;
        case Op::Min: return std::fmin(a, b);
        case Op::Max: return std::fmax(a, b);
        case Op::Frc:
        case Op::SubFlr: return a - std::floor(b);  // (frc of x is "frc x, x")
        case Op::Rcp: return 1.0f / a;
        case Op::Rsq: return 1.0f / std::sqrt(a);
        case Op::Log: return std::log2(a);
        case Op::Exp: return std::exp2(a);
        case Op::Mov: return a;
        case Op::Cmp: return a - b;
        case Op::Dsx:
        case Op::Dsy: return 0.0f;  // (one instance: no neighbours)
        default: return 0.0f;
    }
}

bool cond_holds(Cond c, f32 v) {
    switch (c) {
        case Cond::Always: return true;
        case Cond::EqZero: return v == 0.0f;
        case Cond::NeZero: return v != 0.0f;
        case Cond::PosZero: return !std::signbit(v);
        case Cond::LeZero: return v <= 0.0f;
        case Cond::LtZero: return v < 0.0f;
        case Cond::NegZero: return std::signbit(v);
        case Cond::GeZero: return v >= 0.0f;
        case Cond::GtZero: return v > 0.0f;
    }
    return false;
}

bool cond_holds_int(Cond c, i64 v) {
    switch (c) {
        case Cond::Always: return true;
        case Cond::EqZero: return v == 0;
        case Cond::NeZero: return v != 0;
        case Cond::PosZero:
        case Cond::GeZero: return v >= 0;
        case Cond::LeZero: return v <= 0;
        case Cond::LtZero:
        case Cond::NegZero: return v < 0;
        case Cond::GtZero: return v > 0;
    }
    return false;
}

bool pred_holds(const Machine& m, Pred p, bool& ok) {
    ok = true;
    switch (p) {
        case Pred::None: return true;
        case Pred::P0: return m.pred[0];
        case Pred::P1: return m.pred[1];
        case Pred::P2: return m.pred[2];
        case Pred::P3: return m.pred[3];
        case Pred::NotP0: return !m.pred[0];
        case Pred::NotP1: return !m.pred[1];
        case Pred::NotP2: return !m.pred[2];
        case Pred::Pn: ok = false; return false;
    }
    return false;
}

// Floating-point operations of families 0-6 and dual issue.
void float_op(Exec& x, const Instruction& in) {
    const Type t = in.type;
    const Type st = in.src_type != Type::None ? in.src_type : t;
    const Operand& d = in.dest;
    const bool unary = in.op == Op::Rcp || in.op == Op::Rsq || in.op == Op::Log || in.op == Op::Exp;
    if (!is_float(t) || !is_float(st)) {
        x.fail(nx::format("{} of type {} not implemented", mnemonic(in.op), static_cast<u32>(t)));
        return;
    }
    if (in.op == Op::Dot) {
        f32 sum = 0.0f;
        const Operand& a = in.src[0];
        const Operand& b = in.src[1];
        const u8 mask = a.mask ? a.mask : 15;
        for (u32 k = 0; k < 4; ++k)
            if ((mask >> k) & 1) sum += x.fval(a, k, st) * x.fval(b, k, st);
        for (u32 k = 0; k < 4; ++k)
            if ((d.mask >> k) & 1) x.putf(d, k, t, sum);
        return;
    }
    bool scalar = unary;
    if (!scalar && in.src_count) {
        scalar = true;
        for (u8 i = 0; i < in.src_count; ++i) scalar &= in.src[i].lanes == 1;
    }
    if (scalar) {
        f32 v[3] = {};
        for (u8 i = 0; i < in.src_count && i < 3; ++i) v[i] = x.fval(in.src[i], lowest_lane(in.src[i].mask), st);
        const f32 r = alu(in.op, v[0], v[1], v[2]);
        for (u32 k = 0; k < 4; ++k)
            if ((d.mask >> k) & 1) x.putf(d, k, t, r);
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        f32 v[3] = {};
        for (u8 i = 0; i < in.src_count && i < 3; ++i) v[i] = x.fval(in.src[i], k, st);
        x.putf(d, k, t, alu(in.op, v[0], v[1], v[2]));
    }
}

// Conversions between the types of family 8 (and the moves of families 7 and 8).
u32 convert(u32 v, Type from, Type to, bool scale) {
    f32 f;
    switch (from) {
        case Type::F32: f = std::bit_cast<f32>(v); break;
        case Type::F16: f = from_half(static_cast<u16>(v)); break;
        case Type::U8: f = static_cast<f32>(v & 0xFF) / (scale ? 255.0f : 1.0f); break;
        case Type::S8: f = static_cast<f32>(static_cast<i8>(v)) / (scale ? 127.0f : 1.0f); break;
        case Type::U16: f = static_cast<f32>(v & 0xFFFF) / (scale ? 65535.0f : 1.0f); break;
        case Type::S16: f = static_cast<f32>(static_cast<i16>(v)) / (scale ? 32767.0f : 1.0f); break;
        case Type::Fx10: f = static_cast<f32>(fx_value(v, Type::Fx10)) / 255.0f; break;  // (always scaled: see below)
        default: return v;
    }
    // To integers: truncated (checked: the compiler's float-to-int sequences rely on it), saturated; scaled
    // (normalized) values round to nearest (assumed).
    auto to_int = [&](f32 lo, f32 hi, f32 k) {
        f32 g = scale ? f * k : f;
        g = std::isnan(g) ? 0.0f : std::fmin(std::fmax(g, lo), hi);
        return static_cast<u32>(static_cast<i32>(scale ? std::nearbyint(g) : std::trunc(g)));
    };
    switch (to) {
        case Type::F32: return std::bit_cast<u32>(f);
        case Type::F16: return to_half(f);
        case Type::U8: return to_int(0.0f, 255.0f, 255.0f) & 0xFF;
        case Type::S8: return to_int(-128.0f, 127.0f, 127.0f) & 0xFF;
        case Type::U16: return to_int(0.0f, 65535.0f, 65535.0f) & 0xFFFF;
        case Type::S16: return to_int(-32768.0f, 32767.0f, 32767.0f) & 0xFFFF;
        // (fx10 conversions scale whether or not .scl is printed: checked, the compiler converts an f16 dot product
        // to fx10 without it and back with it)
        case Type::Fx10:
            return fx_raw(static_cast<i32>(std::nearbyint(std::isnan(f) ? 0.0f : nx::clamp(f * 255.0f, -512.0f, 511.0f))),
                          Type::Fx10);
        default: return v;
    }
}

void pack_op(Exec& x, const Instruction& in) {
    const Type dt = in.type, st = in.src_type;
    const Operand& d = in.dest;
    const bool scale = (in.flags & kScale) != 0;
    if (dt == Type::O8 || st == Type::O8) {
        x.fail("o8 pack not implemented");
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        u32 v;
        if (st == Type::F32 && (in.flags & kPairSource)) {
            const Comp c = in.src[0].swizzle[k];
            const bool second = c == Comp::Z || c == Comp::W;
            Operand o = in.src[second ? 1 : 0];
            o.swizzle[k] = static_cast<Comp>(static_cast<u32>(c) & 1);
            v = x.raw(o, k, st);
        } else {
            v = x.raw(in.src[0], k, st);
        }
        x.put(d, k, dt, convert(v, st, dt, scale));
    }
}

// Family 7: moves and conditional moves. Floating-point types go lane by lane; the integer types work on one 32-bit
// register whose lanes are its bytes (the mask enables bytes): cmov8 decides each byte by the byte of its first
// source, cmov decides by the whole value (checked: the float-to-integer conversions select whole words with it).
void move_op(Exec& x, const Instruction& in) {
    const Type t = in.type;
    const Operand& d = in.dest;
    if (t == Type::Fx10) {
        for (u32 k = 0; k < 4; ++k) {
            if (!((d.mask >> k) & 1)) continue;
            const Operand* from = &in.src[0];
            if (in.op != Op::Mov)
                from = &in.src[cond_holds_int(in.cond, fx_value(x.raw(in.src[0], k, t), t)) ? 1 : 2];
            x.put(d, k, t, x.raw(*from, k, t) & 0x3FF);
        }
        return;
    }
    if (!is_float(t)) {
        // (the bytes of the register's first 32-bit lane, an internal register's too: checked, the conversions
        // assemble words in one with cmov8 and mask them with and.u32)
        auto byte = [&](const Operand& o, u32 k) -> u32 {
            if (o.bank == Bank::Literal) return (static_cast<u32>(o.number) >> (8 * k)) & 0xFF;
            const Comp c = o.lanes ? o.swizzle[k & 3] : static_cast<Comp>(k);
            if (c >= Comp::Zero) return x.raw(o, k, Type::U8);
            Operand word = o;
            word.lanes = 0;
            return (x.raw(word, 0, Type::U32) >> (8 * static_cast<u32>(c))) & 0xFF;
        };
        Operand dword = d;
        dword.lanes = 0;
        u32 result = d.bank == Bank::Index ? 0 : x.raw(dword, 0, Type::U32);
        bool whole = true;
        if (in.op == Op::Cmov) {
            const u32 bits = width_of(t);
            const u32 r = x.raw(in.src[0], 0, Type::U32);
            const i64 s = bits == 32 ? i64{static_cast<i32>(r)} : bits == 16 ? i64{static_cast<i16>(r)}
                                                                             : i64{static_cast<i8>(r)};
            whole = cond_holds_int(in.cond, s);
        }
        for (u32 k = 0; k < 4; ++k) {
            if (!((d.mask >> k) & 1)) continue;
            const Operand* from = &in.src[0];
            if (in.op == Op::Cmov) from = &in.src[whole ? 1 : 2];
            if (in.op == Op::Cmov8) {
                const i64 s = static_cast<i8>(byte(in.src[0], k));
                from = &in.src[cond_holds_int(in.cond, s) ? 1 : 2];
            }
            result = (result & ~(0xFFu << (8 * k))) | (byte(*from, k) << (8 * k));
        }
        x.put(dword, 0, Type::U32, result);
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1) && d.lanes) continue;
        if (in.op == Op::Mov) {
            u32 v = x.raw(in.src[0], k, t);
            if (is_float(t) && (in.src[0].negate || in.src[0].absolute)) {
                const f32 f = x.fval(in.src[0], k, t);
                v = t == Type::F16 ? to_half(f) : std::bit_cast<u32>(f);
            }
            x.put(d, k, t, v);
        } else {
            bool take;
            if (is_float(t)) {
                take = cond_holds(in.cond, x.fval(in.src[0], k, t));
            } else {
                const u32 bits = width_of(t);
                u32 r = x.raw(in.src[0], k, t);
                const i64 s = bits == 32 ? static_cast<i64>(static_cast<i32>(r))
                              : bits == 16 ? static_cast<i64>(static_cast<i16>(r))
                                           : static_cast<i64>(static_cast<i8>(r));
                take = cond_holds_int(in.cond, s);
            }
            x.put(d, k, t, x.raw(in.src[take ? 1 : 2], k, t));
        }
        if (!d.lanes) break;
    }
}

// Families 16-18: op(fa * a, fb * b) lane by lane on fixed-point values; a factor is 0, a lane of a, b or (family
// 17) a third source, the alpha (w) lane of one, or min(a.w, 1 - b.w), and may be complemented (1 - f); family 16
// may complement a itself. Min and max ignore the factors. Families 16 and 17 give the colour lanes and the alpha
// lane as two co-issued instructions.
void sop_op(Exec& x, const Instruction& in) {
    const Type t = in.type;
    const Operand& d = in.dest;
    const i32 one = 255;
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        i32 a = fx_value(x.raw(in.src[0], k, t), t);
        const i32 b = fx_value(x.raw(in.src[1], k, t), t);
        if (in.complement_a) a = one - a;
        i32 r;
        if (in.sop_op == Op::Min || in.sop_op == Op::Max) {
            r = in.sop_op == Op::Min ? nx::min(a, b) : nx::max(a, b);
        } else {
            auto alpha = [&](const Operand& o) { return fx_value(x.raw(o, 3, t), t); };
            auto factor = [&](Factor f) -> i32 {
                switch (f) {
                    case Factor::Zero: return 0;
                    case Factor::Sat: return nx::min(alpha(in.src[0]), one - alpha(in.src[1]));
                    case Factor::A: return a;
                    case Factor::AAlpha: return alpha(in.src[0]);
                    case Factor::B: return b;
                    case Factor::BAlpha: return alpha(in.src[1]);
                    case Factor::S0: return fx_value(x.raw(in.src[2], k, t), t);
                    case Factor::S0Alpha: return alpha(in.src[2]);
                    default: return 0;
                }
            };
            auto term = [&](i32 v, const SopTerm& st) {
                const i32 f = factor(st.factor);
                return fx_mul(st.complement ? one - f : f, v);
            };
            const i32 t0 = term(a, in.term[0]), t1 = term(b, in.term[1]);
            r = in.sop_op == Op::Sub ? t0 - t1 : t0 + t1;
        }
        x.put(d, k, t, fx_raw(r, t));
    }
}

// Family 25: a * b + c on four fixed-point (fx10) or byte (u8) lanes.
void fxmad_op(Exec& x, const Instruction& in) {
    const Type t = in.type == Type::Fx10 ? Type::Fx10 : Type::Fx8;
    const Operand& d = in.dest;
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        const i32 a = fx_value(x.raw(in.src[0], k, t), t), b = fx_value(x.raw(in.src[1], k, t), t);
        i32 c = fx_value(x.raw(in.src[2], k, t), t);
        if (in.src[2].negate) c = -c;
        x.put(d, k, t, fx_raw(fx_mul(a, b) + c, t));
    }
}

u32 bitwise(Op op, u32 a, u32 b, u32 bits) {
    const u32 mask = bits == 32 ? ~0u : (1u << bits) - 1;
    const u32 sh = b & (bits - 1);
    switch (op) {
        case Op::And: return a & b;
        case Op::Or: return a | b;
        case Op::Xor: return a ^ b;
        case Op::Shl: return (a << sh) & mask;
        case Op::Shr: return (a & mask) >> sh;
        case Op::Asr: {
            const i32 s = bits == 32 ? static_cast<i32>(a) : static_cast<i32>(static_cast<i16>(a));
            return static_cast<u32>(s >> sh) & mask;
        }
        case Op::Rol: return sh ? ((a << sh) | ((a & mask) >> (bits - sh))) & mask : a & mask;
        default: return 0;
    }
}

void bitwise_op(Exec& x, const Instruction& in) {
    const Type t = in.type;
    const u32 bits = width_of(t);
    const Operand& d = in.dest;
    if (in.op == Op::Rlp) {
        x.fail("rlp not implemented");
        return;
    }
    const u32 n = d.bank == Bank::Index ? 1 : (d.lanes ? d.lanes : 1);
    for (u32 k = 0; k < n; ++k) {
        if (d.lanes && !((d.mask >> k) & 1)) continue;
        const u32 a = x.raw(in.src[0], k, t);
        u32 b = x.raw(in.src[1], k, t);
        if (in.src[1].bank == Bank::Literal && bits == 16) b = (b >> (16 * k)) & 0xFFFF;  // (assumed)
        if (in.flags & kInvertB) b = ~b;
        x.put(d, k, t, bitwise(in.op, a, b, bits));
    }
}

// Tests: the operation's result compared with zero sets a predicate (or, family 15, a register).
void test_op(Exec& x, const Instruction& in) {
    const Type t = in.type;
    const bool flt = is_float(t);
    const Operand& a = in.src[0];
    const Operand& b = in.src[1];
    auto lane_result = [&](u32 k) -> bool {
        if (flt) {
            if (in.alu == Op::Dot) {
                f32 s = 0.0f;
                for (u32 j = 0; j < 4; ++j)
                    if ((a.mask >> j) & 1) s += x.fval(a, j, t) * x.fval(b, j, t);
                return cond_holds(in.cond, s);
            }
            const f32 va = x.fval(a, k, t), vb = in.src_count > 1 ? x.fval(b, k, t) : 0.0f;
            return cond_holds(in.cond, alu(in.alu, va, vb, 0.0f));
        }
        const u32 bits = width_of(t);
        const bool sgn = t == Type::I16 || t == Type::I32 || t == Type::S8 || t == Type::S16;
        auto ext = [&](u32 r) -> i64 {
            if (bits == 32) return sgn ? i64{static_cast<i32>(r)} : i64{r};
            if (bits == 16) return sgn ? i64{static_cast<i16>(r)} : i64{static_cast<u16>(r)};
            return sgn ? i64{static_cast<i8>(r)} : i64{static_cast<u8>(r)};
        };
        const i64 va = ext(x.raw(a, k, t)), vb = in.src_count > 1 ? ext(x.raw(b, k, t)) : 0;
        i64 r;
        switch (in.alu) {
            case Op::Add: r = va + vb; break;
            case Op::Mul: r = va * vb; break;
            case Op::Cmp: r = va - vb; break;
            case Op::And: r = va & vb; break;
            case Op::Or: r = va | vb; break;
            case Op::Xor: r = va ^ vb; break;
            default:
                x.fail(nx::format("test of {} not implemented", mnemonic(in.alu)));
                return false;
        }
        if (bits < 64 && !sgn && in.alu != Op::Cmp) r &= (i64{1} << bits) - 1;
        return cond_holds_int(in.cond, r);
    };
    if (in.dest.bank == Bank::Predicate) {
        bool r;
        const u8 mask = a.mask ? a.mask : 1;
        switch (in.test_mode) {
            case TestMode::Scalar: r = lane_result(lowest_lane(mask)); break;
            case TestMode::AndAll:
            case TestMode::OrAll: {
                const bool all = in.test_mode == TestMode::AndAll;
                r = all;
                for (u32 k = 0; k < 4; ++k)
                    if ((mask >> k) & 1) r = all ? (r && lane_result(k)) : (r || lane_result(k));
                break;
            }
            default:
                x.fail("per-lane test not implemented");
                return;
        }
        x.m_.pred[in.dest.number & 3] = r;
        return;
    }
    // Family 15: the result is all ones or zero for integer types (checked: integer conversions select with cmov8
    // on it), 1.0 or 0.0 for float types (checked: the compiler weighs the results of a four-lane comparison with a
    // dot product, as in percentage-closer shadow filtering); from lane x, or lane by lane for a four-lane
    // destination.
    const Operand& d = in.dest;
    const u32 n = d.lanes == 4 ? 4 : 1;
    for (u32 k = 0; k < n; ++k) {
        if (n == 4 && !((d.mask >> k) & 1)) continue;
        const bool r = lane_result(k);
        const u32 bits = width_of(t);
        if (flt) x.putf(d, k, t, r ? 1.0f : 0.0f);
        else x.put(d, k, t, r ? (bits == 32 ? ~0u : (1u << bits) - 1) : 0u);
    }
}

i64 part_of(u32 v, Part p, bool sgn) {
    switch (p) {
        case Part::Lo16: return sgn ? i64{static_cast<i16>(v)} : i64{v & 0xFFFF};
        case Part::Hi16: return sgn ? i64{static_cast<i16>(v >> 16)} : i64{v >> 16};
        case Part::Lo8: return sgn ? i64{static_cast<i8>(v)} : i64{v & 0xFF};
        case Part::Hi8: return sgn ? i64{static_cast<i8>(v >> 8)} : i64{(v >> 8) & 0xFF};
        case Part::None: return sgn ? i64{static_cast<i32>(v)} : i64{v};
    }
    return 0;
}

void imad_op(Exec& x, const Instruction& in) {
    const bool sgn = in.type == Type::I32 || in.type == Type::I16;
    const Operand& d = in.dest;
    if (in.flags & kScaleVariant) {
        // Family 26: a 32 x 16 multiply-add, the halves of a 32 x 32 product: .s0 is a * lo16(b) + c, .s1 is
        // (a * hi16(b) << 16) + c, modulo 2^32 (checked: the compiler's integer absolute values and conversions).
        const u32 a = x.raw(in.src[0], 0, Type::U32);
        u32 b = x.raw(in.src[1], 0, Type::U32);
        u32 c = x.raw(in.src[2], 0, Type::U32);
        if (in.src[1].negate) b = 0u - b;
        if (in.src[2].negate) c = 0u - c;
        const u32 r = (in.flags & kSignedShift) ? ((a * (b >> 16)) << 16) + c : a * (b & 0xFFFF) + c;
        x.put(d, 0, Type::U32, r);
        return;
    }
    if (in.type == Type::I16 || in.type == Type::U16) {
        // Family 20: on two 16-bit lanes, s0 * s1 + s2, where s1 and s2 are a byte of their register (.lo8/.hi8)
        // or, without a part, its 16-bit lane (checked: the compiler's index computations; the forms without a
        // part are assumed).
        if (d.absolute) {
            x.fail("16-bit multiply-add with an absolute destination not implemented");
            return;
        }
        auto value = [&](const Operand& o, u32 k) -> i64 {
            if (o.part == Part::Lo8 || o.part == Part::Hi8) {
                const u32 w = o.bank == Bank::Literal ? static_cast<u32>(o.number) : x.raw(o, 0, Type::U32);
                return part_of(w, o.part, sgn);
            }
            const u32 h = o.bank == Bank::Literal ? static_cast<u32>(o.number) & 0xFFFF : x.raw(o, k, Type::U16);
            return sgn ? i64{static_cast<i16>(h)} : i64{static_cast<u16>(h)};
        };
        const u32 n = d.bank == Bank::Index ? 1 : 2;
        for (u32 k = 0; k < n; ++k) {
            const u32 a = x.raw(in.src[0], k, Type::U16);
            const i64 s0 = sgn ? i64{static_cast<i16>(a)} : i64{static_cast<u16>(a)};
            i64 c = value(in.src[2], k);
            if (in.src[2].negate) c = -c;
            i64 r = s0 * value(in.src[1], k) + c;
            if (in.flags & kSat) r = sgn ? nx::clamp<i64>(r, INT16_MIN, INT16_MAX) : nx::clamp<i64>(r, 0, UINT16_MAX);
            x.put(d, k, Type::U16, static_cast<u32>(r) & 0xFFFF);
        }
        return;
    }
    const i64 a = part_of(x.raw(in.src[0], 0, Type::U32), in.src[0].part, sgn);
    const i64 b = part_of(x.raw(in.src[1], 0, Type::U32), in.src[1].part, sgn);
    const i64 c = part_of(x.raw(in.src[2], 0, Type::U32), in.src[2].part, sgn);
    i64 r = a * b + c;
    if (in.flags & kSat) r = sgn ? nx::clamp<i64>(r, INT32_MIN, INT32_MAX) : nx::clamp<i64>(r, 0, UINT32_MAX);
    x.put(d, 0, Type::U32, static_cast<u32>(r));
}

// The four lanes of a sample written in a texture result format: u8x4 (one register), f16x4 (two), f32x4 (four).
void write_sample(const f32 rgba[4], u32 format, u32* regs) {
    if (format == 0) {
        u32 w = 0;
        for (u32 k = 0; k < 4; ++k)
            w |= static_cast<u32>(std::nearbyint(nx::clamp(std::isnan(rgba[k]) ? 0.0f : rgba[k], 0.0f, 1.0f) * 255.0f))
                 << (8 * k);
        regs[0] = w;
    } else if (format == 1) {
        regs[0] = u32{to_half(rgba[0])} | u32{to_half(rgba[1])} << 16;
        regs[1] = u32{to_half(rgba[2])} | u32{to_half(rgba[3])} << 16;
    } else {
        for (u32 k = 0; k < 4; ++k) regs[k] = std::bit_cast<u32>(rgba[k]);
    }
}

u32 sample_registers(u32 format) { return format == 0 ? 1u : format == 1 ? 2u : 4u; }

// Family 28: the sampler state is four registers from the second source; the coordinates are f32 lanes.
void tex_op(Exec& x, const Instruction& in) {
    if (!x.m_.textures) {
        x.fail("texture sampling without textures");
        return;
    }
    if (in.flags & (kTexRsd | kTexInfo | kTexMinP)) {
        x.fail("texture queries not implemented");
        return;
    }
    SamplerState state;
    for (u32 k = 0; k < 4; ++k) {
        u32* w = x.reg_word(in.src[1], in.src[1].number + k);
        if (!w) return;
        state.words[k] = *w;
    }
    TextureRequest r;
    r.dim = in.tex_dim;
    r.kind = in.tex_kind;
    const u32 n = static_cast<u32>(in.tex_dim) + 1;
    for (u32 k = 0; k < n; ++k) r.coord[k] = x.fval(in.src[0], k, Type::F32);
    if (in.src_count > 2) {
        for (u32 k = 0; k < 4; ++k) {
            u32* w = x.reg_word(in.src[2], in.src[2].number + k);
            if (!w) return;
            r.extra[k] = std::bit_cast<f32>(*w);
        }
    }
    f32 rgba[4];
    if (!x.m_.textures->sample(state, r, rgba)) {
        x.fail("texture sampling failed");
        return;
    }
    const u32 format = in.type == Type::F32 ? 3 : in.type == Type::F16 ? 1 : 0;
    u32 regs[4];
    write_sample(rgba, format, regs);
    for (u32 k = 0; k < sample_registers(format); ++k) {
        Operand d = in.dest;
        d.lanes = 0;
        if (u32* w = x.reg_word(d, d.number + k)) x.writes.push_back(Pending{Slot{w, 0, 32}, regs[k]});
    }
}

void memory_op(Exec& x, const Instruction& in) {
    if (!x.m_.memory) {
        x.fail("load or store without memory");
        return;
    }
    if (in.space != Space::Absolute || (in.flags & (kPreIncrement | kPostIncrement))) {
        x.fail("load/store addressing mode not implemented");
        return;
    }
    const u32 bytes = in.size / 8u;
    u32* base = x.reg_word(in.src[0], in.src[0].number);
    if (!base) return;
    // An immediate offset counts elements; a register offset is a byte offset (low half) plus a count of
    // elements (high half). (Checked for the default uniform buffer's arrays.)
    const u32 off = x.raw(in.src[1], 0, Type::U32);
    const u32 count = in.fetch ? in.fetch : 1;
    const u32 address = in.src[1].bank == Bank::Literal ? *base + off * bytes
                                                        : *base + (off >> 16) * bytes + (off & 0xFFFF);
    const u32 words = in.size == 64 ? 2 : 1;
    for (u32 i = 0; i < count; ++i) {
        for (u32 j = 0; j < words; ++j) {
            const u32 at = address + i * bytes + j * 4;
            u32 v = 0;
            if (in.op == Op::Load) {
                if (!x.m_.memory->read(at, &v, nx::min<u32>(bytes, 4))) {
                    x.fail(nx::format("load from 0x{:08x} failed", at));
                    return;
                }
                Operand d = in.dest;
                d.lanes = 0;
                if (u32* w = x.reg_word(d, d.number + i * words + j)) x.writes.push_back(Pending{Slot{w, 0, 32}, v});
            } else {
                Operand s = in.src[2];
                s.lanes = 0;
                u32* w = x.reg_word(s, s.number + i * words + j);
                if (!w) return;
                v = *w;
                if (!x.m_.memory->write(at, &v, nx::min<u32>(bytes, 4))) {
                    x.fail(nx::format("store to 0x{:08x} failed", at));
                    return;
                }
            }
        }
    }
}

}  // namespace

Result<void> run(std::span<const Decoded> program, ProgramKind kind, Machine& m, const RunLimits& limits) {
    // Where each word's instructions start.
    u32 words = 0;
    for (const Decoded& d : program) words = nx::max(words, d.word + 1);
    nx::vector<u32> start(words + 1, static_cast<u32>(program.size()));
    for (u32 i = static_cast<u32>(program.size()); i-- > 0;) start[program[i].word] = i;
    for (u32 w = words; w-- > 0;)
        if (start[w] == program.size()) start[w] = start[w + 1];

    Exec x(m, kind);
    u64 steps = 0;
    usize i = 0;
    while (i < program.size()) {
        if (++steps > limits.max_steps) return failure("step limit reached");
        const Decoded& dec = program[i];
        const Instruction& in = dec.in;
        if (!dec.executed) {
            ++i;
            continue;
        }
        bool ok;
        const bool run_it = pred_holds(m, in.pred, ok);
        if (!ok) return failure(nx::format("word {}: per-instance predicate not implemented", dec.word));
        usize next = i + 1;
        if (run_it) {
            switch (in.op) {
                case Op::Hidden:
                case Op::SetIncrements:
                case Op::Nop:
                case Op::PCoeff: break;
                case Op::Mad:
                case Op::Add:
                case Op::Mul:
                case Op::Min:
                case Op::Max:
                case Op::Frc:
                case Op::Dsx:
                case Op::Dsy:
                case Op::Dot:
                case Op::Rcp:
                case Op::Rsq:
                case Op::Log:
                case Op::Exp:
                case Op::SubFlr: float_op(x, in); break;
                case Op::Mov:
                case Op::Pack:
                    if (in.family == 8) pack_op(x, in);
                    else if (in.family == 7) move_op(x, in);
                    else float_op(x, in);
                    break;
                case Op::Cmov:
                case Op::Cmov8: move_op(x, in); break;
                case Op::MovImm: x.put(in.dest, 0, Type::U32, static_cast<u32>(in.src[0].number)); break;
                case Op::Test: test_op(x, in); break;
                case Op::And:
                case Op::Or:
                case Op::Xor:
                case Op::Shl:
                case Op::Rol:
                case Op::Shr:
                case Op::Asr:
                case Op::Rlp: bitwise_op(x, in); break;
                case Op::IMad: imad_op(x, in); break;
                case Op::Sop:
                case Op::Sop2:
                case Op::Blend: sop_op(x, in); break;
                case Op::FxMad: fxmad_op(x, in); break;
                case Op::Load:
                case Op::Store: memory_op(x, in); break;
                case Op::Tex: tex_op(x, in); break;
                case Op::Br: {
                    if (in.target < 0 || static_cast<u32>(in.target) > words)
                        return failure(nx::format("word {}: branch outside the program", dec.word));
                    next = start[static_cast<u32>(in.target)];
                    break;
                }
                case Op::Kill: m.killed = true; break;
                case Op::MovToLink: m.link = m.temp[0]; break;
                case Op::MovFromLink: x.put(Operand{Bank::Temp, 0, 0, false, 1, 1}, 0, Type::U32, m.link); break;
                default:
                    return failure(nx::format("word {}: {} not implemented", dec.word, mnemonic(in.op)));
            }
        }
        // The co-issued half of a dual-issue word reads the registers as they were before the word.
        const bool pair = next == i + 1 && i + 1 < program.size() && (program[i + 1].in.flags & kCoissued);
        if (!pair) x.commit();
        if (!x.error.empty()) return failure(nx::format("word {}: {}", dec.word, x.error));
        if (limits.trace && run_it && !pair) {
            limits.trace(limits.trace_user, dec, m);
            if (i > 0 && (dec.in.flags & kCoissued)) limits.trace(limits.trace_user, program[i - 1], m);
        }
        i = next;
    }
    x.commit();
    return {};
}

Result<void> load_secondary(const Gxp& gxp, std::span<const u32> default_uniforms,
                            std::span<const u32> buffer_addresses, Machine& m, std::span<const SamplerState> samplers) {
    if (const Container* c = gxp.container(kDefaultUniformContainer)) {
        for (u32 i = 0; i < c->size; ++i) {
            if (c->base + i >= Machine::kBankSize) return failure("default uniform buffer outside the bank");
            m.secondary[c->base + i] = i < default_uniforms.size() ? default_uniforms[i] : 0;
        }
    }
    const Container* k = gxp.container(kConstantsContainer);
    if (!k) {
        if (!gxp.literals.empty() || !gxp.buffer_slots.empty()) return failure("constants without their container");
        return {};
    }
    auto at = [&](u32 offset) -> u32* {
        const u32 r = u32{k->base} + offset;
        return r < Machine::kBankSize ? &m.secondary[r] : nullptr;
    };
    for (const BufferSlot& s : gxp.buffer_slots) {
        u32* r = at(s.offset);
        if (!r) return failure("uniform buffer address outside the bank");
        *r = s.buffer < buffer_addresses.size() ? buffer_addresses[s.buffer] + static_cast<u32>(s.bias) : 0;
    }
    for (const SamplerSlot& s : gxp.sampler_slots) {
        u32* r = at(s.offset);
        if (!r) return failure("sampler state outside the bank");
        const u32 unit = s.word / 4u;
        *r = unit < samplers.size() ? samplers[unit].words[s.word % 4u] : 0;
    }
    for (const Literal& l : gxp.literals) {
        u32* r = at(l.offset);
        if (!r) return failure("literal outside the bank");
        *r = l.value;
    }
    return {};
}

Result<void> load_fragment_inputs(const Gxp& gxp, std::span<const VaryingValue> varyings,
                                  std::span<const SamplerState> samplers, Machine& m) {
    auto find = [&](u32 bit) -> const VaryingValue* {
        for (const VaryingValue& v : varyings)
            if (v.varying == bit) return &v;
        return nullptr;
    };
    auto reg = [&](u32 at) -> u32* { return at < Machine::kBankSize ? &m.primary[at] : nullptr; };
    for (const FragmentInput& in : gxp.inputs) {
        if (in.interpolated) {
            const VaryingValue* v = find(in.interpolated);
            if (!v) return failure(nx::format("no value for the varying 0x{:x}", in.interpolated));
            const u32 n = in.components;
            if (in.format == 0) {
                for (u32 k = 0; k < n; ++k)
                    if (u32* r = reg(in.offset + k)) *r = std::bit_cast<u32>(v->value[k]);
            } else if (in.format == 2) {
                for (u32 k = 0; k < n; k += 2) {
                    const u32 lo = to_half(v->value[k]), hi = k + 1 < n ? to_half(v->value[k + 1]) : 0u;
                    if (u32* r = reg(in.offset + k / 2)) *r = lo | hi << 16;
                }
            } else {  // fx10: x, y, z in one register, w in the next (1.0 = 255)
                u32 q[4] = {};
                for (u32 k = 0; k < n && k < 4; ++k)
                    q[k] = fx_raw(static_cast<i32>(std::nearbyint(nx::clamp(v->value[k] * 255.0f, -512.0f, 511.0f))),
                                  Type::Fx10);
                if (u32* r = reg(in.offset)) *r = q[0] | q[1] << 10 | q[2] << 20;
                if (u32* r = reg(in.offset + 1u)) *r = q[3];
            }
        }
        if (in.sampled) {
            const VaryingValue* v = find(in.sampled);
            if (!v) return failure(nx::format("no value for the varying 0x{:x}", in.sampled));
            if (!m.textures) return failure("a sampled input without textures");
            TextureRequest r;
            for (u32 k = 0; k < 4; ++k) r.coord[k] = v->value[k];
            if (in.projected)
                for (u32 k = 0; k < 2; ++k) r.coord[k] /= v->value[3];
            const SamplerState state = in.sampler < samplers.size() ? samplers[in.sampler] : SamplerState{};
            f32 rgba[4];
            if (!m.textures->sample(state, r, rgba)) return failure("texture sampling failed");
            u32 regs[4];
            write_sample(rgba, in.sample_format, regs);
            for (u32 k = 0; k < sample_registers(in.sample_format); ++k)
                if (u32* q = reg(in.sample_offset + k)) *q = regs[k];
        }
    }
    return {};
}

}  // namespace psvrecore::usse
