#include "softfloat.hpp"

#include <utility>

namespace psvrecore::interp::fp {

namespace {

using u128 = unsigned __int128;

struct Format {
    int e, f, bias, min_exp;  // exponent bits, fraction bits, bias, smallest normal exponent
};
Format format(int n) {
    switch (n) {
        case 16: return {5, 10, 15, -14};
        case 32: return {8, 23, 127, -126};
        default: return {11, 52, 1023, -1022};
    }
}

enum class Type { Zero, Nonzero, Infinity, QNaN, SNaN };

// A real number: (m + d) * 2^e with 0 < d < 1 when `sticky` (the bits below m that are not all zero).
struct Real {
    bool sign = false;
    u128 m = 0;
    int e = 0;
    bool sticky = false;
};

struct Unpacked {
    Type type;
    Real value;
};

int msb(u128 v) {
    int n = -1;
    while (v) v >>= 1, ++n;
    return n;
}

u64 mask(int bits) { return bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1; }

// FPUnpack: flush-to-zero applies to single and double precision only.
Unpacked unpack(u64 bits, int n, u32 fpscr) {
    const Format fmt = format(n);
    const bool sign = (bits >> (n - 1)) & 1;
    const u64 exp = (bits >> fmt.f) & mask(fmt.e), frac = bits & mask(fmt.f);
    Unpacked u{Type::Zero, Real{sign, 0, 0, false}};
    if (exp == 0) {
        if (frac == 0 || ((fpscr & kFz) && n != 16)) return u;
        u.type = Type::Nonzero;
        u.value.m = frac;
        u.value.e = fmt.min_exp - fmt.f;
        return u;
    }
    if (exp == mask(fmt.e) && !(n == 16 && (fpscr & kAhp))) {
        if (frac == 0) u.type = Type::Infinity;
        else u.type = (frac >> (fmt.f - 1)) & 1 ? Type::QNaN : Type::SNaN;
        return u;
    }
    u.type = Type::Nonzero;
    u.value.m = (u64{1} << fmt.f) | frac;
    u.value.e = static_cast<int>(exp) - fmt.bias - fmt.f;
    return u;
}

u64 zero(bool sign, int n) { return sign ? u64{1} << (n - 1) : 0; }
u64 infinity(bool sign, int n) {
    const Format fmt = format(n);
    return zero(sign, n) | (mask(fmt.e) << fmt.f);
}
u64 default_nan(int n) {
    const Format fmt = format(n);
    return (mask(fmt.e) << fmt.f) | (u64{1} << (fmt.f - 1));
}
u64 max_normal(bool sign, int n) {
    const Format fmt = format(n);
    return zero(sign, n) | ((mask(fmt.e) - 1) << fmt.f) | mask(fmt.f);
}

// FPProcessNaN: quietened, or the default NaN.
u64 process_nan(Type type, u64 bits, int n, u32 fpscr) {
    const Format fmt = format(n);
    if (fpscr & kDn) return default_nan(n);
    return type == Type::SNaN ? bits | (u64{1} << (fmt.f - 1)) : bits;
}

// FPProcessNaNs: signalling NaNs first, then quiet ones, the first operand first.
bool process_nans(Type t1, Type t2, u64 a, u64 b, int n, u32 fpscr, u64& result) {
    if (t1 == Type::SNaN) return result = process_nan(t1, a, n, fpscr), true;
    if (t2 == Type::SNaN) return result = process_nan(t2, b, n, fpscr), true;
    if (t1 == Type::QNaN) return result = process_nan(t1, a, n, fpscr), true;
    if (t2 == Type::QNaN) return result = process_nan(t2, b, n, fpscr), true;
    return false;
}

// FPRound: a nonzero real to format n in mode `mode`. When sticky is set, m has at least f + 3 bits (callers keep
// enough precision), so the rounding decision is exact.
u64 round(const Real& r, int n, u32 fpscr, u32 mode) {
    const Format fmt = format(n);
    const int p = msb(r.m);
    const int exponent = p + r.e;  // the value is in [2^exponent, 2^(exponent + 1))
    if ((fpscr & kFz) && n != 16 && exponent < fmt.min_exp) return zero(r.sign, n);
    int biased_exp = exponent - fmt.min_exp + 1;
    if (biased_exp < 0) biased_exp = 0;
    const int lsb = biased_exp == 0 ? fmt.min_exp - fmt.f : exponent - fmt.f;
    const int shift = lsb - r.e;
    u128 int_mant;
    int error_class;  // 0: exact, 1: below half, 2: half, 3: above half
    if (shift <= 0) {
        int_mant = r.m << -shift;
        error_class = r.sticky ? 1 : 0;  // (not reached with sticky: see above)
    } else if (shift >= 128) {
        int_mant = 0;
        error_class = 1;  // (m is far below half an ulp)
    } else {
        int_mant = r.m >> shift;
        const u128 rem = r.m & ((u128{1} << shift) - 1), half = u128{1} << (shift - 1);
        if (rem == 0 && !r.sticky) error_class = 0;
        else if (rem < half) error_class = 1;
        else if (rem == half) error_class = r.sticky ? 3 : 2;
        else error_class = 3;
    }
    bool round_up, overflow_to_infinity;
    switch (mode) {
        case 0: round_up = error_class == 3 || (error_class == 2 && (int_mant & 1)); overflow_to_infinity = true; break;
        case 1: round_up = error_class != 0 && !r.sign; overflow_to_infinity = !r.sign; break;
        case 2: round_up = error_class != 0 && r.sign; overflow_to_infinity = r.sign; break;
        default: round_up = false; overflow_to_infinity = false; break;
    }
    if (round_up) {
        ++int_mant;
        if (int_mant == (u128{1} << fmt.f)) biased_exp = 1;  // a denormal rounded up to the smallest normal
        if (int_mant == (u128{1} << (fmt.f + 1))) {
            ++biased_exp;
            int_mant >>= 1;
        }
    }
    if (n != 16 || !(fpscr & kAhp)) {
        if (biased_exp >= static_cast<int>(mask(fmt.e))) return overflow_to_infinity ? infinity(r.sign, n) : max_normal(r.sign, n);
        return zero(r.sign, n) | (static_cast<u64>(biased_exp) << fmt.f) | (static_cast<u64>(int_mant) & mask(fmt.f));
    }
    // Alternative half precision: no infinities or NaNs; the largest exponent is a normal one.
    if (biased_exp >= (1 << fmt.e)) return zero(r.sign, n) | 0x7FFF;
    return zero(r.sign, n) | (static_cast<u64>(biased_exp) << fmt.f) | (static_cast<u64>(int_mant) & mask(fmt.f));
}

// a + b for exact operands (the result may carry a sticky bit when their exponents are far apart).
Real real_add(Real a, Real b) {
    if (a.e < b.e) std::swap(a, b);
    const int gap = a.e - b.e;
    u128 x, y;
    int e;
    bool sticky = false;
    if (gap <= 64) {
        x = a.m << gap;
        y = b.m;
        e = b.e;
    } else {
        x = a.m << 64;
        e = a.e - 64;
        const int s = e - b.e;
        y = s >= 128 ? 0 : b.m >> s;
        sticky = s >= 128 ? b.m != 0 : (b.m & ((u128{1} << s) - 1)) != 0;
    }
    Real out;
    out.e = e;
    if (a.sign == b.sign) {
        out.sign = a.sign;
        out.m = x + y;
        out.sticky = sticky;
        return out;
    }
    // Opposite signs: the larger magnitude wins. (With sticky, the truncated operand y is the much smaller one.)
    if (x > y || (x == y && sticky)) {
        out.sign = a.sign;
        out.m = x - y - (sticky ? 1 : 0);
        out.sticky = sticky;
    } else {
        out.sign = b.sign;
        out.m = y - x;
        out.sticky = false;
    }
    return out;
}

u128 isqrt(u128 v, bool& exact) {
    u128 result = 0, bit = u128{1} << 126;
    while (bit > v) bit >>= 2;
    u128 rem = v;
    while (bit) {
        if (rem >= result + bit) {
            rem -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    exact = rem == 0;
    return result;
}

}  // namespace

u64 neg(u64 a, int n) { return a ^ (u64{1} << (n - 1)); }
u64 abs(u64 a, int n) { return a & ~(u64{1} << (n - 1)); }

u64 add(u64 a, u64 b, int n, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr), y = unpack(b, n, fpscr);
    u64 result;
    if (process_nans(x.type, y.type, a, b, n, fpscr, result)) return result;
    const bool inf1 = x.type == Type::Infinity, inf2 = y.type == Type::Infinity;
    const bool zero1 = x.type == Type::Zero, zero2 = y.type == Type::Zero;
    const bool s1 = x.value.sign, s2 = y.value.sign;
    if (inf1 && inf2 && s1 != s2) return default_nan(n);
    if ((inf1 && !s1) || (inf2 && !s2)) return infinity(false, n);
    if ((inf1 && s1) || (inf2 && s2)) return infinity(true, n);
    if (zero1 && zero2 && s1 == s2) return zero(s1, n);
    if (zero1 && !zero2) return round(y.value, n, fpscr, rounding(fpscr));  // (exact: the other operand)
    if (zero2 && !zero1) return round(x.value, n, fpscr, rounding(fpscr));
    const Real sum = real_add(x.value, y.value);
    if (sum.m == 0 && !sum.sticky) return zero(rounding(fpscr) == 2, n);  // exact zero: -0 only when rounding down
    return round(sum, n, fpscr, rounding(fpscr));
}

u64 sub(u64 a, u64 b, int n, u32 fpscr) {
    // FPSub: as FPAdd with the second operand negated, except that NaNs are processed as they come.
    const Unpacked x = unpack(a, n, fpscr), y = unpack(b, n, fpscr);
    u64 result;
    if (process_nans(x.type, y.type, a, b, n, fpscr, result)) return result;
    return add(a, neg(b, n), n, fpscr);
}

u64 mul(u64 a, u64 b, int n, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr), y = unpack(b, n, fpscr);
    u64 result;
    if (process_nans(x.type, y.type, a, b, n, fpscr, result)) return result;
    const bool inf1 = x.type == Type::Infinity, inf2 = y.type == Type::Infinity;
    const bool zero1 = x.type == Type::Zero, zero2 = y.type == Type::Zero;
    const bool sign = x.value.sign != y.value.sign;
    if ((inf1 && zero2) || (zero1 && inf2)) return default_nan(n);
    if (inf1 || inf2) return infinity(sign, n);
    if (zero1 || zero2) return zero(sign, n);
    Real p;
    p.sign = sign;
    p.m = x.value.m * y.value.m;
    p.e = x.value.e + y.value.e;
    return round(p, n, fpscr, rounding(fpscr));
}

u64 div(u64 a, u64 b, int n, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr), y = unpack(b, n, fpscr);
    u64 result;
    if (process_nans(x.type, y.type, a, b, n, fpscr, result)) return result;
    const bool inf1 = x.type == Type::Infinity, inf2 = y.type == Type::Infinity;
    const bool zero1 = x.type == Type::Zero, zero2 = y.type == Type::Zero;
    const bool sign = x.value.sign != y.value.sign;
    if ((inf1 && inf2) || (zero1 && zero2)) return default_nan(n);
    if (inf1 || zero2) return infinity(sign, n);
    if (zero1 || inf2) return zero(sign, n);
    // Normalized to 53 significant bits each, the dividend shifted up 64 more: a quotient of at least 64 bits.
    Real xn = x.value, yn = y.value;
    while (msb(xn.m) < 52) xn.m <<= 1, --xn.e;
    while (msb(yn.m) < 52) yn.m <<= 1, --yn.e;
    // (xn.m << 64) / yn.m by long division in 64-bit steps: both are below 2^53.
    const u64 dividend = static_cast<u64>(xn.m), divisor = static_cast<u64>(yn.m);
    const u64 high = dividend / divisor;
    u64 rem = dividend % divisor, low = 0;
    for (int i = 63; i >= 0; --i) {
        rem <<= 1;
        if (rem >= divisor) {
            rem -= divisor;
            low |= u64{1} << i;
        }
    }
    Real q;
    q.sign = sign;
    q.m = (static_cast<u128>(high) << 64) | low;
    q.sticky = rem != 0;
    q.e = xn.e - 64 - yn.e;
    return round(q, n, fpscr, rounding(fpscr));
}

u64 sqrt(u64 a, int n, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr);
    if (x.type == Type::SNaN || x.type == Type::QNaN) return process_nan(x.type, a, n, fpscr);
    if (x.type == Type::Zero) return zero(x.value.sign, n);
    if (x.type == Type::Infinity && !x.value.sign) return infinity(false, n);
    if (x.value.sign) return default_nan(n);
    Real v = x.value;
    if (v.e & 1) v.m <<= 1, --v.e;
    while (msb(v.m) < 110) v.m <<= 2, v.e -= 2;
    bool exact;
    Real r;
    r.m = isqrt(v.m, exact);
    r.sticky = !exact;
    r.e = v.e / 2;
    return round(r, n, fpscr, rounding(fpscr));
}

u32 compare(u64 a, u64 b, int n, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr), y = unpack(b, n, fpscr);
    const auto nan = [](Type t) { return t == Type::QNaN || t == Type::SNaN; };
    if (nan(x.type) || nan(y.type)) return 0b0011;
    // Compare the values: zeros of either sign are equal.
    const auto as_signed = [](const Unpacked& u, int& sign) -> int {
        if (u.type == Type::Zero) return sign = 0, 0;
        sign = u.value.sign ? -1 : 1;
        return u.type == Type::Infinity ? 2 : 1;
    };
    int sx, sy;
    const int kx = as_signed(x, sx), ky = as_signed(y, sy);
    int order;  // -1 less, 0 equal, 1 greater
    if (sx != sy) {
        order = sx < sy ? -1 : 1;
    } else if (sx == 0) {
        order = 0;
    } else if (kx == 2 || ky == 2) {
        order = kx == ky ? 0 : (kx < ky ? -1 : 1) * sx;
    } else {
        // Same sign, both finite: compare magnitudes exactly.
        const Real negated{!y.value.sign, y.value.m, y.value.e, false};
        const Real d = real_add(x.value, negated);
        order = d.m == 0 && !d.sticky ? 0 : (d.sign ? -1 : 1);
    }
    return order == 0 ? 0b0110 : order < 0 ? 0b1000 : 0b0010;
}

u64 convert(u64 a, int n, int m, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr);
    const Format fn = format(n), fm = format(m);
    const bool alternative = m == 16 && (fpscr & kAhp);
    if (x.type == Type::SNaN || x.type == Type::QNaN) {
        if (alternative) return 0;  // (no NaNs in the alternative format: zero, Invalid Operation)
        if (fpscr & kDn) return default_nan(m);
        // The sign, a quiet NaN, and the top of the payload (below the quiet bit).
        const u64 payload = a & mask(fn.f - 1);
        const u64 moved = fn.f > fm.f ? payload >> (fn.f - fm.f) : payload << (fm.f - fn.f);
        return zero(x.value.sign, m) | (mask(fm.e) << fm.f) | (u64{1} << (fm.f - 1)) | (moved & mask(fm.f - 1));
    }
    if (x.type == Type::Infinity) return alternative ? zero(x.value.sign, m) | 0x7FFF : infinity(x.value.sign, m);
    if (x.type == Type::Zero) return zero(x.value.sign, m);
    return round(x.value, m, fpscr, rounding(fpscr));
}

u64 to_fixed(u64 a, int n, int bits, int fraction, bool is_unsigned, bool round_zero, u32 fpscr) {
    const Unpacked x = unpack(a, n, fpscr);
    if (x.type == Type::SNaN || x.type == Type::QNaN) return 0;
    const i64 hi = is_unsigned ? static_cast<i64>(mask(bits)) : (i64{1} << (bits - 1)) - 1;
    const i64 lo = is_unsigned ? 0 : -(i64{1} << (bits - 1));
    i64 v;
    if (x.type == Type::Zero) {
        v = 0;
    } else if (x.type == Type::Infinity) {
        v = x.value.sign ? lo : hi;
    } else {
        // The magnitude scaled by 2^fraction: its integer part and how the rest compares with a half.
        const int e = x.value.e + fraction;
        u128 int_part;
        int error_class;  // 0 exact, 1 below half, 2 half, 3 above half
        const u128 m = x.value.m;
        if (e >= 0) {
            int_part = e >= 64 ? (u128{1} << 100) : m << e;  // (huge: saturates)
            error_class = 0;
        } else {
            const int s = -e;
            int_part = s >= 128 ? 0 : m >> s;
            const u128 rem = s >= 128 ? m : m & ((u128{1} << s) - 1);
            const u128 half = s >= 128 ? 0 : u128{1} << (s - 1);
            if (rem == 0) error_class = 0;
            else if (s >= 128 || rem < half) error_class = 1;
            else if (rem == half) error_class = 2;
            else error_class = 3;
        }
        const u32 mode = round_zero ? 3 : rounding(fpscr);
        const bool sign = x.value.sign;
        bool up;  // round the magnitude up
        switch (mode) {
            case 0: up = error_class == 3 || (error_class == 2 && (int_part & 1)); break;
            case 1: up = error_class != 0 && !sign; break;
            case 2: up = error_class != 0 && sign; break;
            default: up = false; break;
        }
        const u128 magnitude = int_part + (up ? 1 : 0);
        const u128 limit = u128{1} << 62;  // (anything larger saturates: keep it within i64)
        const i64 mag = magnitude > limit ? (i64{1} << 62) : static_cast<i64>(magnitude);
        v = sign ? -mag : mag;
        if (v > hi) v = hi;
        if (v < lo) v = lo;
    }
    return static_cast<u64>(v) & (bits == 64 ? ~u64{0} : (is_unsigned ? mask(bits) : ~u64{0}));
}

u64 from_fixed(u64 x, int bits, int fraction, bool is_unsigned, int n, bool round_nearest, u32 fpscr) {
    const u64 raw = x & mask(bits);
    bool sign = false;
    u64 magnitude = raw;
    if (!is_unsigned && bits < 64 && ((raw >> (bits - 1)) & 1)) {
        sign = true;
        magnitude = (~raw + 1) & mask(bits);
        if (magnitude == 0) magnitude = u64{1} << (bits - 1);
    }
    if (magnitude == 0) return 0;  // +0
    Real r;
    r.sign = sign;
    r.m = magnitude;
    r.e = -fraction;
    return round(r, n, fpscr, round_nearest ? 0 : rounding(fpscr));
}

namespace {

// recip_estimate and recip_sqrt_estimate (the ARM ARM's C functions), exactly: the result in units of 1/256.
u32 recip_estimate_units(u32 q /* RoundDown(a * 512), 256..511 */) {
    // s = RoundDown(256 * 512 / (q + 0.5) + 0.5) = RoundDown((2^19 + 2q + 1) / (4q + 2))
    return (524288u + 2 * q + 1) / (4 * q + 2);
}
u32 recip_sqrt_estimate_units(u32 q, bool below_half /* a in [0.25, 0.5): q = a * 512, else q = a * 256 */) {
    // s = RoundDown(256 / sqrt(t) + 0.5), t = (q + 0.5) / 512 or (q + 0.5) / 256: the largest s with
    // (2s - 1)^2 (2q + 1) <= 2^28 (or 2^27).
    const u64 k = 2 * u64{q} + 1, limit = below_half ? (u64{1} << 28) : (u64{1} << 27);
    u32 s = 512;
    while ((2 * u64{s} - 1) * (2 * u64{s} - 1) * k > limit) --s;
    return s;
}

}  // namespace

u32 recip_estimate(u32 a) {
    const Unpacked x = unpack(a, 32, kStandard);
    if (x.type == Type::QNaN || x.type == Type::SNaN) return static_cast<u32>(process_nan(x.type, a, 32, kStandard));
    if (x.type == Type::Infinity) return static_cast<u32>(zero(x.value.sign, 32));
    if (x.type == Type::Zero) return static_cast<u32>(infinity(x.value.sign, 32));
    const u32 exp = (a >> 23) & 0xFF;
    if (exp >= 253) return static_cast<u32>(zero(x.value.sign, 32));  // |a| >= 2^126
    // scaled = 0.1fraction in [0.5, 1): RoundDown(scaled * 512) = 256 + the top eight fraction bits
    const u32 s = recip_estimate_units(256 + ((a >> 15) & 0xFF));
    return (a & 0x80000000u) | ((253 - exp) << 23) | ((s - 256) << 15);
}

u32 rsqrt_estimate(u32 a) {
    const Unpacked x = unpack(a, 32, kStandard);
    if (x.type == Type::QNaN || x.type == Type::SNaN) return static_cast<u32>(process_nan(x.type, a, 32, kStandard));
    if (x.type == Type::Zero) return static_cast<u32>(infinity(x.value.sign, 32));
    if (x.value.sign) return static_cast<u32>(default_nan(32));
    if (x.type == Type::Infinity) return 0;
    const u32 exp = (a >> 23) & 0xFF;
    // The fraction scaled into [0.5, 1) (exponent even) or [0.25, 0.5) (odd): either way q is 128 plus the top
    // seven fraction bits.
    const bool odd = (exp & 1) != 0;
    const u32 s = recip_sqrt_estimate_units(128 + ((a >> 16) & 0x7F), odd);
    return (((380 - exp) / 2) << 23) | ((s - 256) << 15);
}

u32 recip_step(u32 a, u32 b) {
    const Unpacked x = unpack(a, 32, kStandard), y = unpack(b, 32, kStandard);
    u64 result;
    if (process_nans(x.type, y.type, a, b, 32, kStandard, result)) return static_cast<u32>(result);
    const bool inf_zero = (x.type == Type::Infinity && y.type == Type::Zero) || (x.type == Type::Zero && y.type == Type::Infinity);
    const u64 product = inf_zero ? 0 : mul(a, b, 32, kStandard);
    return static_cast<u32>(sub(0x40000000u, product, 32, kStandard));  // 2.0 - product
}

u32 rsqrt_step(u32 a, u32 b) {
    const Unpacked x = unpack(a, 32, kStandard), y = unpack(b, 32, kStandard);
    u64 result;
    if (process_nans(x.type, y.type, a, b, 32, kStandard, result)) return static_cast<u32>(result);
    const bool inf_zero = (x.type == Type::Infinity && y.type == Type::Zero) || (x.type == Type::Zero && y.type == Type::Infinity);
    const u64 product = inf_zero ? 0 : mul(a, b, 32, kStandard);
    // FPHalvedSub(3.0, product)
    const Unpacked p = unpack(product, 32, kStandard);
    if (p.type == Type::QNaN || p.type == Type::SNaN) return static_cast<u32>(default_nan(32));
    if (p.type == Type::Infinity) return static_cast<u32>(infinity(!p.value.sign, 32));
    const Real three{false, 3, 0, false};
    Real neg = p.value;
    neg.sign = !neg.sign;
    Real diff = p.type == Type::Zero ? three : real_add(three, neg);
    if (diff.m == 0 && !diff.sticky) return 0;  // (+0 in round to nearest)
    diff.e -= 1;  // halved, exactly
    return static_cast<u32>(round(diff, 32, kStandard, 0));
}

u32 max(u32 a, u32 b) {
    const Unpacked x = unpack(a, 32, kStandard), y = unpack(b, 32, kStandard);
    u64 result;
    if (process_nans(x.type, y.type, a, b, 32, kStandard, result)) return static_cast<u32>(result);
    // value1 > value2 picks the first; a zero result takes the AND of the signs ("the most positive").
    const bool first = compare(a, b, 32, kStandard) == 0b0010;
    const Unpacked& pick = first ? x : y;
    if (pick.type == Type::Infinity) return static_cast<u32>(infinity(pick.value.sign, 32));
    if (pick.type == Type::Zero) return static_cast<u32>(zero(x.value.sign && y.value.sign, 32));
    return static_cast<u32>(round(pick.value, 32, kStandard, 0));
}

u32 min(u32 a, u32 b) {
    const Unpacked x = unpack(a, 32, kStandard), y = unpack(b, 32, kStandard);
    u64 result;
    if (process_nans(x.type, y.type, a, b, 32, kStandard, result)) return static_cast<u32>(result);
    const bool first = compare(a, b, 32, kStandard) == 0b1000;  // value1 < value2
    const Unpacked& pick = first ? x : y;
    if (pick.type == Type::Infinity) return static_cast<u32>(infinity(pick.value.sign, 32));
    if (pick.type == Type::Zero) return static_cast<u32>(zero(x.value.sign || y.value.sign, 32));
    return static_cast<u32>(round(pick.value, 32, kStandard, 0));
}

}  // namespace psvrecore::interp::fp
