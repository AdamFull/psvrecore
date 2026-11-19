#include "psvrecore/gxm/target_texture.hpp"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "psvrecore/gxm/spirv.hpp"
#include "psvrecore/log.hpp"
#include "spirv/unified1/GLSL.std.450.h"
#include "spirv/unified1/spirv.h"

namespace psvrecore::gxm {

namespace {

// An unsigned 10-bit float is a half's exponent and top five mantissa bits (the same bias). To one: rounded to
// nearest (ties to even); negative values are 0, large ones the largest finite value; infinity and NaN kept.
u32 half_to_f10(u16 h) {
    if (h & 0x8000) return 0;
    if ((h & 0x7C00) == 0x7C00) return (h & 0x3FF) ? 0x3FF : 0x3E0;
    const u32 r = (u32{h} + 0xF + ((h >> 5) & 1)) >> 5;
    return nx::min<u32>(r, 0x3DF);
}
// A half's value (finite; infinity and NaN as large values).
f32 half_value(u16 h) {
    const u32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const f32 v = e ? std::ldexp(static_cast<f32>(m | 0x400), static_cast<i32>(e) - 25) : std::ldexp(static_cast<f32>(m), -24);
    return (h & 0x8000) ? -v : v;
}

u32 code_of(u32 base_format) { return ((base_format >> 24) & 0x1F) | ((base_format >> 31) << 7); }
constexpr u32 kU16 = 0x09, kF32 = 0x12, kF32M = 0x13, kU2F10F10F10 = 0x9A;

// Where the tables are in buffer 1 (words).
constexpr u32 kTableU16 = 0, kTableF10 = 32768, kTableAlpha = 32768 + 512, kTableWords = 32768 + 512 + 2;

// The least half bits whose packed alpha is 1, 2 and 3 (positive halves: the packing is monotonic in their bits;
// negative ones pack to 0). Found from pack_u2f10f10f10 itself; all zero if it is not monotonic.
struct AlphaSteps {
    u32 at[3] = {};
    bool ok = false;
};
const AlphaSteps& alpha_steps() {
    static const AlphaSteps steps = [] {
        AlphaSteps s;
        u32 last = 0;
        bool found[3] = {};
        for (u32 h = 0; h < 0x8000; ++h) {
            const u16 px[4] = {0, 0, 0, static_cast<u16>(h)};
            const u32 a = pack_u2f10f10f10(px) >> 30;
            if (a < last) return AlphaSteps{};
            for (u32 k = last; k < a; ++k)
                if (!found[k]) s.at[k] = h, found[k] = true;
            last = a;
        }
        for (u32 h = 0x8000; h < 0x10000; ++h) {
            const u16 px[4] = {0, 0, 0, static_cast<u16>(h)};
            if (pack_u2f10f10f10(px) >> 30) return AlphaSteps{};
        }
        s.ok = found[0] && found[1] && found[2];
        return s;
    }();
    return steps;
}

// read_texels of a texture whose bytes are `data` (at address 0).
Result<Texels> decode_bytes(const TextureInfo& info, std::span<const u8> data) {
    return read_texels(info, [&](u32 address, void* out, u32 bytes) {
        if (u64{address} + bytes > data.size()) return false;
        std::memcpy(out, data.data() + address, bytes);
        return true;
    }, false, true);
}

TextureInfo probe_info(const TextureInfo& like, u32 width, u32 height, u32 bytes) {
    TextureInfo t;
    t.base_format = like.base_format;
    t.swizzle = like.swizzle;
    t.type = texture_type::kLinearStrided;
    t.width = width, t.height = height;
    t.stride = width * bytes;
    t.levels = 1;
    return t;
}

void create_image_converter(vulkan::Device& device, TargetConverter& c);

u16 half_at(const Texels& t, usize texel, u32 k) {
    u16 h;
    std::memcpy(&h, t.data.data() + texel * 8 + k * 2, 2);
    return h;
}

}  // namespace

u32 pack_u2f10f10f10(const u16 h[4]) {
    const f32 a = nx::clamp(half_value(h[3]), 0.0f, 1.0f);
    const u32 a2 = static_cast<u32>(a * 3.0f + 0.5f);
    return half_to_f10(h[0]) | half_to_f10(h[1]) << 10 | half_to_f10(h[2]) << 20 | a2 << 30;
}

void pack_u2f10f10f10(const u16* halves, u32* words, usize count) {
    const AlphaSteps& s = alpha_steps();
    if (!s.ok) {
        for (usize i = 0; i < count; ++i) words[i] = pack_u2f10f10f10(halves + i * 4);
        return;
    }
    const u32 a1 = s.at[0], a2 = s.at[1], a3 = s.at[2];
    for (usize i = 0; i < count; ++i) {
        const u16* h = halves + i * 4;
        const u32 a = h[3];
        const u32 alpha = (a & 0x8000) ? 0 : u32{a >= a1} + u32{a >= a2} + u32{a >= a3};
        words[i] = half_to_f10(h[0]) | half_to_f10(h[1]) << 10 | half_to_f10(h[2]) << 20 | alpha << 30;
    }
}

u16 depth16_of(f32 z) { return static_cast<u16>(std::lround(z * 65535.0f)); }

const nx::vector<u32>& target_texture_tables() {
    static const nx::vector<u32> tables = [] {
        nx::vector<u32> out(kTableWords, 0);
        // (every U16 value, decoded as R: its half in the first component)
        TextureInfo plain;
        plain.base_format = kU16 << 24;
        nx::vector<u8> values(65536 * 2);
        for (u32 v = 0; v < 65536; ++v) values[v * 2] = static_cast<u8>(v), values[v * 2 + 1] = static_cast<u8>(v >> 8);
        auto a = decode_bytes(probe_info(plain, 256, 256, 2), values);
        if (!a || a->bytes != 8) return nx::vector<u32>{};
        for (u32 v = 0; v < 65536; ++v) out[kTableU16 + v / 2] |= u32{half_at(*a, v, 0)} << ((v & 1) * 16);
        // (every 10-bit float in R, G and B, each 2-bit alpha, decoded in the identity swizzle)
        TextureInfo f10;
        f10.base_format = 0x80000000u | (kU2F10F10F10 & 0x1F) << 24;
        nx::vector<u8> words(1024 * 4);
        for (u32 i = 0; i < 1024; ++i) {
            const u32 w = i | i << 10 | i << 20 | (i & 3) << 30;
            std::memcpy(words.data() + i * 4, &w, 4);
        }
        auto b = decode_bytes(probe_info(f10, 1024, 1, 4), words);
        if (!b || b->bytes != 8) return nx::vector<u32>{};
        for (u32 i = 0; i < 1024; ++i) {
            const u16 h = half_at(*b, i, 0);
            if (half_at(*b, i, 1) != h || half_at(*b, i, 2) != h) return nx::vector<u32>{};
            out[kTableF10 + i / 2] |= u32{h} << ((i & 1) * 16);
        }
        for (u32 i = 0; i < 4; ++i) out[kTableAlpha + i / 2] |= u32{half_at(*b, i, 3)} << ((i & 1) * 16);
        return out;
    }();
    return tables;
}

std::optional<TargetDecode> target_decode(const TextureInfo& info, TargetSource source) {
    if (info.gamma || info.levels != 1 || !alpha_steps().ok || target_texture_tables().empty()) return std::nullopt;
    const u32 code = code_of(info.base_format);
    const nx::vector<u32>& tables = target_texture_tables();
    auto table_half = [&](u32 base, u32 i) { return static_cast<u16>(tables[base + i / 2] >> ((i & 1) * 16)); };
    TargetDecode d;
    // (an output half's source among these halves (the components'), or the constants 0 and 1)
    auto fields = [&](const Texels& t, const u16 c[4], u32 count, u32& select) {
        select = 0;
        for (u32 k = 0; k < 4; ++k) {
            const u16 h = half_at(t, 0, k);
            u32 f = 15;
            for (u32 i = 0; i < count && f == 15; ++i)
                if (h == c[i]) f = i;
            if (f == 15) f = h == 0 ? 4 : h == 0x3C00 ? 5 : 15;
            if (f == 15) return false;
            select |= f << (4 * k);
        }
        return true;
    };
    if (source == TargetSource::Depth32) {
        if (code != kF32 && code != kF32M) return std::nullopt;
        d.single = true;
        d.kind = 0;
        d.mask = code == kF32M ? 0x7FFFFFFFu : ~0u;
        for (const u32 w : {0x80000000u | std::bit_cast<u32>(0.375f), std::bit_cast<u32>(0.8125f)}) {
            u8 bytes[4];
            std::memcpy(bytes, &w, 4);
            auto t = decode_bytes(probe_info(info, 1, 1, 4), bytes);
            if (!t || !t->single || t->bytes != 4) return std::nullopt;
            u32 got;
            std::memcpy(&got, t->data.data(), 4);
            if (got != (w & d.mask)) return std::nullopt;
            std::memcpy(d.rgba_of, t->rgba_of, 4);
        }
        return d;
    }
    if (source == TargetSource::Depth16) {
        if (code != kU16) return std::nullopt;
        d.kind = 1;
        u32 first = 0;
        for (const u32 v : {0x1234u, 0xBEEFu}) {
            const u8 bytes[2] = {static_cast<u8>(v), static_cast<u8>(v >> 8)};
            auto t = decode_bytes(probe_info(info, 1, 1, 2), bytes);
            if (!t || t->bytes != 8 || t->single) return std::nullopt;
            const u16 c[4] = {table_half(kTableU16, v), 0, 0, 0};
            if (!fields(*t, c, 1, d.select) || (first && d.select != first)) return std::nullopt;
            first = d.select;
        }
        return d;
    }
    if (code != kU2F10F10F10) return std::nullopt;
    d.kind = 2;
    d.alpha_low = info.swizzle == 2 || info.swizzle == 3 || info.swizzle == 6 || info.swizzle == 7;
    const u32 at = d.alpha_low ? 2 : 0;
    u32 first = ~0u;
    const u32 probes[2][4] = {{0x101, 0x182, 0x203, 1}, {0x2C4, 0x0E5, 0x346, 2}};
    for (const auto& p : probes) {
        const u32 w = p[0] << at | p[1] << (at + 10) | p[2] << (at + 20) | (d.alpha_low ? p[3] : p[3] << 30);
        u8 bytes[4];
        std::memcpy(bytes, &w, 4);
        auto t = decode_bytes(probe_info(info, 1, 1, 4), bytes);
        if (!t || t->bytes != 8 || t->single) return std::nullopt;
        const u16 c[4] = {table_half(kTableF10, p[0]), table_half(kTableF10, p[1]), table_half(kTableF10, p[2]),
                          table_half(kTableAlpha, p[3])};
        if (!fields(*t, c, 4, d.select) || (first != ~0u && d.select != first)) return std::nullopt;
        first = d.select;
    }
    return d;
}

void target_packing(TargetTextureConstants& k) {
    const AlphaSteps& s = alpha_steps();
    k.alpha1 = s.at[0], k.alpha2 = s.at[1], k.alpha3 = s.at[2];
}

static_assert(sizeof(TargetTextureConstants) == 21 * 4);

TargetTextureConstants target_pack_constants(u32 width, u32 height) {
    TargetTextureConstants k;
    k.width = k.a_width = width, k.height = k.a_height = height;
    k.src = 0, k.src_pitch = width, k.src_step = 1, k.src_first = 0;
    k.out = (width * height * 2 + 3) & ~3u;
    k.source = static_cast<u32>(TargetSource::U2F10F10F10);
    k.kind = 0, k.mask = ~0u, k.select = 0, k.alpha_low = 0;
    target_packing(k);
    return k;
}

nx::vector<u32> target_texture_shader() {
    using spirv::Builder;
    using spirv::Id;
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id tu = b.t_uint(), tb = b.t_bool(), tf = b.t_float(), tv3 = b.t_vector(tu, 3), tvoid = b.t_void();
    const Id words = b.t_runtime_array(tu);
    b.decorate(words, SpvDecorationArrayStride, {4});
    const Id block = b.t_struct({words});
    b.decorate(block, SpvDecorationBlock);
    b.member_decorate(block, 0, SpvDecorationOffset, {0});
    const Id tp_block = b.t_pointer(SpvStorageClassStorageBuffer, block);
    Id buf[3];
    for (u32 i = 0; i < 3; ++i) {
        buf[i] = b.variable(tp_block, SpvStorageClassStorageBuffer);
        b.decorate(buf[i], SpvDecorationDescriptorSet, {0});
        b.decorate(buf[i], SpvDecorationBinding, {i});
    }
    constexpr u32 kConstants = sizeof(TargetTextureConstants) / 4;
    const Id pc_block = b.t_struct({tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu, tu});
    b.decorate(pc_block, SpvDecorationBlock);
    for (u32 i = 0; i < kConstants; ++i) b.member_decorate(pc_block, i, SpvDecorationOffset, {i * 4});
    const Id pc = b.variable(b.t_pointer(SpvStorageClassPushConstant, pc_block), SpvStorageClassPushConstant);
    const Id gid_var = b.variable(b.t_pointer(SpvStorageClassInput, tv3), SpvStorageClassInput);
    b.decorate(gid_var, SpvDecorationBuiltIn, {SpvBuiltInGlobalInvocationId});

    const Id fn = b.begin_function(tvoid, b.t_function(tvoid));
    const Id tp_local = b.t_pointer(SpvStorageClassFunction, tu);
    const Id v_var = b.local(tp_local);
    b.block(b.new_label());
    const Id tp_pc = b.t_pointer(SpvStorageClassPushConstant, tu), tp_word = b.t_pointer(SpvStorageClassStorageBuffer, tu);
    // (the constants by their place in TargetTextureConstants)
    enum : u32 {
        kWidth, kHeight, kAWidth, kAHeight, kSrc, kSrcPitch, kSrcStep, kSrcFirst, kOut, kSource, kKind, kMask, kSelect,
        kAlphaLow, kAlpha1, kAlpha2, kAlpha3, kBg, kBgTiled, kBgPitch, kBgBits
    };
    auto k = [&](u32 i) { return b.op(SpvOpLoad, tu, {b.op(SpvOpAccessChain, tp_pc, {pc, b.c_int(static_cast<i32>(i))})}); };
    auto u = [&](u32 v) { return b.c_uint(v); };
    auto bin = [&](u32 op, Id type, Id x, Id y) { return b.op(op, type, {x, y}); };
    auto add = [&](Id x, Id y) { return bin(SpvOpIAdd, tu, x, y); };
    auto mul = [&](Id x, Id y) { return bin(SpvOpIMul, tu, x, y); };
    auto band = [&](Id x, Id y) { return bin(SpvOpBitwiseAnd, tu, x, y); };
    auto bor = [&](Id x, Id y) { return bin(SpvOpBitwiseOr, tu, x, y); };
    auto shr = [&](Id x, Id y) { return bin(SpvOpShiftRightLogical, tu, x, y); };
    auto shl = [&](Id x, Id y) { return bin(SpvOpShiftLeftLogical, tu, x, y); };
    auto eq = [&](Id x, Id y) { return bin(SpvOpIEqual, tb, x, y); };
    auto lt = [&](Id x, Id y) { return bin(SpvOpULessThan, tb, x, y); };
    auto sel = [&](Id c, Id x, Id y) { return b.op(SpvOpSelect, tu, {c, x, y}); };
    auto ptr = [&](u32 which, Id index) { return b.op(SpvOpAccessChain, tp_word, {buf[which], b.c_int(0), index}); };
    auto load = [&](u32 which, Id index) { return b.op(SpvOpLoad, tu, {ptr(which, index)}); };
    // (a half of a table in buffer 1: half i from word `base`)
    auto table = [&](u32 base, Id i) {
        return band(shr(load(1, add(u(base), shr(i, u(1)))), shl(band(i, u(1)), u(4))), u(0xFFFF));
    };

    const Id gid = b.op(SpvOpLoad, tv3, {gid_var});
    const Id x = b.op(SpvOpCompositeExtract, tu, {gid, 0}), y = b.op(SpvOpCompositeExtract, tu, {gid, 1});
    const Id width = k(kWidth);
    const Id inside = bin(SpvOpLogicalAnd, tb, lt(x, width), lt(y, k(kHeight)));
    const Id body = b.new_label(), merge = b.new_label();
    b.op0(SpvOpSelectionMerge, {merge, SpvSelectionControlMaskNone});
    b.op0(SpvOpBranchConditional, {inside, body, merge});
    b.block(body);
    {
        // ---- The texel's word as guest memory has it: from the image's texel, or the background ----
        const Id in_image = bin(SpvOpLogicalAnd, tb, lt(x, k(kAWidth)), lt(y, k(kAHeight)));
        const Id from_image = b.new_label(), from_bg = b.new_label(), have = b.new_label();
        b.op0(SpvOpSelectionMerge, {have, SpvSelectionControlMaskNone});
        b.op0(SpvOpBranchConditional, {in_image, from_image, from_bg});
        b.block(from_image);
        {
            const Id source = k(kSource);
            const Id two = eq(source, u(2));
            const Id texel = add(mul(y, k(kSrcPitch)), add(k(kSrcFirst), mul(x, k(kSrcStep))));
            const Id i = add(k(kSrc), mul(texel, sel(two, u(2), u(1))));
            const Id w0 = load(0, i), w1 = load(0, sel(two, add(i, u(1)), i));
            // (D16: lround(z * 65535), z not negative)
            const Id z = b.op(SpvOpBitcast, tf, {w0});
            const Id p = bin(SpvOpFMul, tf, z, b.c_float(65535.0f));
            b.decorate(p, SpvDecorationNoContraction);
            const Id q = bin(SpvOpFAdd, tf, p, b.c_float(0.5f));
            b.decorate(q, SpvDecorationNoContraction);
            const Id fl = b.ext(tf, GLSLstd450FMax, {b.ext(tf, GLSLstd450Floor, {q}), b.c_float(0.0f)});
            const Id d16 = band(b.op(SpvOpConvertFToU, tu, {fl}), u(0xFFFF));
            // (U2F10F10F10: the halves packed as pack_u2f10f10f10 packs them)
            auto f10 = [&](Id h) {
                const Id neg = bin(SpvOpINotEqual, tb, band(h, u(0x8000)), u(0));
                const Id e31 = eq(band(h, u(0x7C00)), u(0x7C00));
                const Id special = sel(bin(SpvOpINotEqual, tb, band(h, u(0x3FF)), u(0)), u(0x3FF), u(0x3E0));
                const Id r = b.ext(tu, GLSLstd450UMin, {shr(add(add(h, u(0xF)), band(shr(h, u(5)), u(1))), u(5)), u(0x3DF)});
                return sel(neg, u(0), sel(e31, special, r));
            };
            const Id h0 = band(w0, u(0xFFFF)), h1 = shr(w0, u(16)), h2 = band(w1, u(0xFFFF)), h3 = shr(w1, u(16));
            auto at_least = [&](Id h, u32 c) { return sel(bin(SpvOpUGreaterThanEqual, tb, h, k(c)), u(1), u(0)); };
            const Id a2 = sel(bin(SpvOpINotEqual, tb, band(h3, u(0x8000)), u(0)), u(0),
                              add(add(at_least(h3, kAlpha1), at_least(h3, kAlpha2)), at_least(h3, kAlpha3)));
            const Id packed = bor(bor(f10(h0), shl(f10(h1), u(10))), bor(shl(f10(h2), u(20)), shl(a2, u(30))));
            const Id v = sel(eq(source, u(0)), w0, sel(eq(source, u(1)), d16, packed));
            b.op0(SpvOpStore, {v_var, v});
            b.op0(SpvOpBranch, {have});
        }
        b.block(from_bg);
        {
            const Id tiled = add(mul(add(mul(shr(y, u(5)), k(kBgPitch)), shr(x, u(5))), u(1024)),
                                 add(mul(band(y, u(31)), u(32)), band(x, u(31))));
            const Id j = sel(bin(SpvOpINotEqual, tb, k(kBgTiled), u(0)), tiled, add(mul(y, k(kBgPitch)), x));
            const Id is16 = eq(k(kBgBits), u(16));
            const Id word = load(2, add(k(kBg), sel(is16, shr(j, u(1)), j)));
            const Id v16 = band(shr(word, shl(band(j, u(1)), u(4))), u(0xFFFF));
            b.op0(SpvOpStore, {v_var, sel(is16, v16, word)});
            b.op0(SpvOpBranch, {have});
        }
        b.block(have);
        // ---- Decoded as read_texels decodes it ----
        const Id v = b.op(SpvOpLoad, tu, {v_var});
        const Id kind = k(kKind);
        const Id single = eq(kind, u(0));
        const Id o = add(k(kOut), mul(add(mul(y, width), x), sel(single, u(1), u(2))));
        const Id alpha_low = bin(SpvOpINotEqual, tb, k(kAlphaLow), u(0));
        const Id at = sel(alpha_low, u(2), u(0));
        auto field = [&](u32 n) { return table(kTableF10, band(shr(v, add(at, u(10 * n))), u(0x3FF))); };
        const Id c16 = table(kTableU16, band(v, u(0xFFFF)));
        const Id c[4] = {sel(eq(kind, u(1)), c16, field(0)), field(1), field(2),
                         table(kTableAlpha, sel(alpha_low, band(v, u(3)), shr(v, u(30))))};
        const Id select = k(kSelect);
        Id h[4];
        for (u32 n = 0; n < 4; ++n) {
            const Id f = band(shr(select, u(4 * n)), u(15));
            h[n] = sel(eq(f, u(0)), c[0],
                       sel(eq(f, u(1)), c[1], sel(eq(f, u(2)), c[2], sel(eq(f, u(3)), c[3], sel(eq(f, u(5)), u(0x3C00), u(0))))));
        }
        const Id one_word = b.new_label(), halves = b.new_label(), written = b.new_label();
        b.op0(SpvOpSelectionMerge, {written, SpvSelectionControlMaskNone});
        b.op0(SpvOpBranchConditional, {single, one_word, halves});
        b.block(one_word);
        b.op0(SpvOpStore, {ptr(0, o), band(v, k(kMask))});
        b.op0(SpvOpBranch, {written});
        b.block(halves);
        b.op0(SpvOpStore, {ptr(0, o), bor(h[0], shl(h[1], u(16)))});
        b.op0(SpvOpStore, {ptr(0, add(o, u(1))), bor(h[2], shl(h[3], u(16)))});
        b.op0(SpvOpBranch, {written});
        b.block(written);
        b.op0(SpvOpBranch, {merge});
    }
    b.block(merge);
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {gid_var};
    b.entry_point(SpvExecutionModelGLCompute, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeLocalSize, {8, 8, 1});
    return b.module();
}

static_assert(sizeof(TargetImageConstants) == 9 * 4);

nx::vector<u32> target_image_shader() {
    using spirv::Builder;
    using spirv::Id;
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id tu = b.t_uint(), ti = b.t_int(), tb = b.t_bool(), tf = b.t_float(), tv3 = b.t_vector(tu, 3), tvoid = b.t_void();
    const Id tv2f = b.t_vector(tf, 2), tv4f = b.t_vector(tf, 4), tv2i = b.t_vector(ti, 2);
    // (0: the surface's image, sampled; 1: the tables; 2: the texture, a storage image)
    const Id t_src = b.t_image(tf, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown);
    const Id t_out = b.t_image(tf, SpvDim2D, 0, 0, 0, 2, SpvImageFormatRgba16f);
    const Id src_var = b.variable(b.t_pointer(SpvStorageClassUniformConstant, t_src), SpvStorageClassUniformConstant);
    const Id out_var = b.variable(b.t_pointer(SpvStorageClassUniformConstant, t_out), SpvStorageClassUniformConstant);
    const Id words = b.t_runtime_array(tu);
    b.decorate(words, SpvDecorationArrayStride, {4});
    const Id block = b.t_struct({words});
    b.decorate(block, SpvDecorationBlock);
    b.member_decorate(block, 0, SpvDecorationOffset, {0});
    const Id tables_var = b.variable(b.t_pointer(SpvStorageClassStorageBuffer, block), SpvStorageClassStorageBuffer);
    const Id vars[3] = {src_var, tables_var, out_var};
    for (u32 i = 0; i < 3; ++i) {
        b.decorate(vars[i], SpvDecorationDescriptorSet, {0});
        b.decorate(vars[i], SpvDecorationBinding, {i});
    }
    b.decorate(out_var, SpvDecorationNonReadable);
    constexpr u32 kConstants = sizeof(TargetImageConstants) / 4;
    const Id pc_block = b.t_struct({tu, tu, tu, tu, tu, tu, tu, tu, tu});
    b.decorate(pc_block, SpvDecorationBlock);
    for (u32 i = 0; i < kConstants; ++i) b.member_decorate(pc_block, i, SpvDecorationOffset, {i * 4});
    const Id pc = b.variable(b.t_pointer(SpvStorageClassPushConstant, pc_block), SpvStorageClassPushConstant);
    const Id gid_var = b.variable(b.t_pointer(SpvStorageClassInput, tv3), SpvStorageClassInput);
    b.decorate(gid_var, SpvDecorationBuiltIn, {SpvBuiltInGlobalInvocationId});

    const Id fn = b.begin_function(tvoid, b.t_function(tvoid));
    b.block(b.new_label());
    const Id tp_pc = b.t_pointer(SpvStorageClassPushConstant, tu), tp_word = b.t_pointer(SpvStorageClassStorageBuffer, tu);
    enum : u32 { kWidth, kHeight, kX0, kY0, kSelect, kAlphaLow, kAlpha1, kAlpha2, kAlpha3 };
    auto k = [&](u32 i) { return b.op(SpvOpLoad, tu, {b.op(SpvOpAccessChain, tp_pc, {pc, b.c_int(static_cast<i32>(i))})}); };
    auto u = [&](u32 v) { return b.c_uint(v); };
    auto bin = [&](u32 op, Id type, Id x, Id y) { return b.op(op, type, {x, y}); };
    auto add = [&](Id x, Id y) { return bin(SpvOpIAdd, tu, x, y); };
    auto band = [&](Id x, Id y) { return bin(SpvOpBitwiseAnd, tu, x, y); };
    auto bor = [&](Id x, Id y) { return bin(SpvOpBitwiseOr, tu, x, y); };
    auto shr = [&](Id x, Id y) { return bin(SpvOpShiftRightLogical, tu, x, y); };
    auto shl = [&](Id x, Id y) { return bin(SpvOpShiftLeftLogical, tu, x, y); };
    auto eq = [&](Id x, Id y) { return bin(SpvOpIEqual, tb, x, y); };
    auto lt = [&](Id x, Id y) { return bin(SpvOpULessThan, tb, x, y); };
    auto sel = [&](Id c, Id x, Id y) { return b.op(SpvOpSelect, tu, {c, x, y}); };
    auto table = [&](u32 base, Id i) {
        const Id p = b.op(SpvOpAccessChain, tp_word, {tables_var, b.c_int(0), add(u(base), shr(i, u(1)))});
        return band(shr(b.op(SpvOpLoad, tu, {p}), shl(band(i, u(1)), u(4))), u(0xFFFF));
    };
    auto coord = [&](Id x, Id y) {
        return b.op(SpvOpCompositeConstruct, tv2i, {b.op(SpvOpBitcast, ti, {x}), b.op(SpvOpBitcast, ti, {y})});
    };

    const Id gid = b.op(SpvOpLoad, tv3, {gid_var});
    const Id x = b.op(SpvOpCompositeExtract, tu, {gid, 0}), y = b.op(SpvOpCompositeExtract, tu, {gid, 1});
    const Id inside = bin(SpvOpLogicalAnd, tb, lt(x, k(kWidth)), lt(y, k(kHeight)));
    const Id body = b.new_label(), merge = b.new_label();
    b.op0(SpvOpSelectionMerge, {merge, SpvSelectionControlMaskNone});
    b.op0(SpvOpBranchConditional, {inside, body, merge});
    b.block(body);
    {
        // ---- The image's halves, packed as pack_u2f10f10f10 packs them ----
        const Id src = b.op(SpvOpLoad, t_src, {src_var});
        const Id texel = b.op(SpvOpImageFetch, tv4f, {src, coord(add(x, k(kX0)), add(y, k(kY0))), SpvImageOperandsLodMask,
                                                     b.c_int(0)});
        auto halves = [&](u32 i) {
            const Id pair = b.op(SpvOpCompositeConstruct, tv2f, {b.op(SpvOpCompositeExtract, tf, {texel, i}),
                                                                 b.op(SpvOpCompositeExtract, tf, {texel, i + 1})});
            return b.ext(tu, GLSLstd450PackHalf2x16, {pair});
        };
        const Id w0 = halves(0), w1 = halves(2);
        auto f10 = [&](Id h) {
            const Id neg = bin(SpvOpINotEqual, tb, band(h, u(0x8000)), u(0));
            const Id e31 = eq(band(h, u(0x7C00)), u(0x7C00));
            const Id special = sel(bin(SpvOpINotEqual, tb, band(h, u(0x3FF)), u(0)), u(0x3FF), u(0x3E0));
            const Id r = b.ext(tu, GLSLstd450UMin, {shr(add(add(h, u(0xF)), band(shr(h, u(5)), u(1))), u(5)), u(0x3DF)});
            return sel(neg, u(0), sel(e31, special, r));
        };
        const Id h0 = band(w0, u(0xFFFF)), h1 = shr(w0, u(16)), h2 = band(w1, u(0xFFFF)), h3 = shr(w1, u(16));
        auto at_least = [&](Id h, u32 c) { return sel(bin(SpvOpUGreaterThanEqual, tb, h, k(c)), u(1), u(0)); };
        const Id a2 = sel(bin(SpvOpINotEqual, tb, band(h3, u(0x8000)), u(0)), u(0),
                          add(add(at_least(h3, kAlpha1), at_least(h3, kAlpha2)), at_least(h3, kAlpha3)));
        // ---- That word decoded as read_texels decodes it (target_texture_shader's kind 2) ----
        const Id v = bor(bor(f10(h0), shl(f10(h1), u(10))), bor(shl(f10(h2), u(20)), shl(a2, u(30))));
        const Id alpha_low = bin(SpvOpINotEqual, tb, k(kAlphaLow), u(0));
        const Id at = sel(alpha_low, u(2), u(0));
        auto field = [&](u32 n) { return table(kTableF10, band(shr(v, add(at, u(10 * n))), u(0x3FF))); };
        const Id c[4] = {field(0), field(1), field(2), table(kTableAlpha, sel(alpha_low, band(v, u(3)), shr(v, u(30))))};
        const Id select = k(kSelect);
        Id h[4];
        for (u32 n = 0; n < 4; ++n) {
            const Id f = band(shr(select, u(4 * n)), u(15));
            h[n] = sel(eq(f, u(0)), c[0],
                       sel(eq(f, u(1)), c[1], sel(eq(f, u(2)), c[2], sel(eq(f, u(3)), c[3], sel(eq(f, u(5)), u(0x3C00), u(0))))));
        }
        const Id lo = b.ext(tv2f, GLSLstd450UnpackHalf2x16, {bor(h[0], shl(h[1], u(16)))});
        const Id hi = b.ext(tv2f, GLSLstd450UnpackHalf2x16, {bor(h[2], shl(h[3], u(16)))});
        const Id out = b.op(SpvOpCompositeConstruct, tv4f, {b.op(SpvOpCompositeExtract, tf, {lo, 0}),
                                                           b.op(SpvOpCompositeExtract, tf, {lo, 1}),
                                                           b.op(SpvOpCompositeExtract, tf, {hi, 0}),
                                                           b.op(SpvOpCompositeExtract, tf, {hi, 1})});
        b.op0(SpvOpImageWrite, {b.op(SpvOpLoad, t_out, {out_var}), coord(x, y), out});
        b.op0(SpvOpBranch, {merge});
    }
    b.block(merge);
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {gid_var};
    b.entry_point(SpvExecutionModelGLCompute, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeLocalSize, {8, 8, 1});
    return b.module();
}

// ---- On a device ---------------------------------------------------------------------------------------------

Result<void> create_target_converter(vulkan::Device& device, TargetConverter& c) {
    const VkDevice dev = device.device();
    const nx::vector<u32>& tables = target_texture_tables();
    if (tables.empty() || !alpha_steps().ok) return failure("target textures: the decoder's tables could not be made");
    VkDescriptorSetLayoutBinding bindings[3];
    for (u32 i = 0; i < 3; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 3;
    si.pBindings = bindings;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TargetTextureConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &c.set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    const nx::vector<u32> code = target_texture_shader();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4;
    mi.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    constexpr u32 kSets = 64;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSets};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    bool ok = vkCreateDescriptorSetLayout(dev, &si, nullptr, &c.set_layout) == VK_SUCCESS &&
              vkCreatePipelineLayout(dev, &li, nullptr, &c.layout) == VK_SUCCESS &&
              vkCreateShaderModule(dev, &mi, nullptr, &module) == VK_SUCCESS &&
              vkCreateDescriptorPool(dev, &pi, nullptr, &c.pool) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = c.layout;
        ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &c.pipeline) == VK_SUCCESS;
    }
    if (module) vkDestroyShaderModule(dev, module, nullptr);
    // (the tables into a device buffer, waited for)
    if (ok) {
        const VkDeviceSize bytes = tables.size() * 4;
        auto table_buffer = vulkan::create_device_buffer(device, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto staging = vulkan::create_host_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        ok = table_buffer && staging;
        if (ok) {
            c.tables = *table_buffer;
            std::memcpy(staging->mapped, tables.data(), bytes);
            vulkan::flush(device, *staging);
            vulkan::OneShot shot(device);
            auto cmd = shot.begin();
            ok = static_cast<bool>(cmd);
            if (ok) {
                const VkBufferCopy region{0, 0, bytes};
                vkCmdCopyBuffer(*cmd, staging->buffer, c.tables.buffer, 1, &region);
                VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                                     nullptr, 0, nullptr);
                ok = static_cast<bool>(shot.submit_and_wait());
            }
        } else if (table_buffer) {
            vulkan::DeviceBuffer t = *table_buffer;
            vulkan::destroy_buffer(device, t);
        }
        if (staging) vulkan::destroy_buffer(device, *staging);
    }
    if (!ok) {
        destroy_target_converter(device, c);
        return failure("target textures: the conversion's pipeline could not be made");
    }
    create_image_converter(device, c);
    return {};
}

void destroy_target_image_converter(vulkan::Device& device, TargetConverter& c) {
    const VkDevice dev = device.device();
    if (c.image_pipeline) vkDestroyPipeline(dev, c.image_pipeline, nullptr);
    if (c.image_pool) vkDestroyDescriptorPool(dev, c.image_pool, nullptr);
    if (c.image_layout) vkDestroyPipelineLayout(dev, c.image_layout, nullptr);
    if (c.image_set_layout) vkDestroyDescriptorSetLayout(dev, c.image_set_layout, nullptr);
    c.image_pipeline = VK_NULL_HANDLE, c.image_pool = VK_NULL_HANDLE;
    c.image_layout = VK_NULL_HANDLE, c.image_set_layout = VK_NULL_HANDLE;
}

namespace {

// target_image_shader's pipeline (none where the device cannot sample RGBA16F images or store to them).
void create_image_converter(vulkan::Device& device, TargetConverter& c) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(device.physical(), VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((fp.optimalTilingFeatures & need) != need) return;
    const VkDevice dev = device.device();
    const VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 3;
    si.pBindings = bindings;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TargetImageConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &c.image_set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    const nx::vector<u32> code = target_image_shader();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4;
    mi.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    constexpr u32 kSets = 64;
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSets},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kSets;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    bool ok = vkCreateDescriptorSetLayout(dev, &si, nullptr, &c.image_set_layout) == VK_SUCCESS &&
              vkCreatePipelineLayout(dev, &li, nullptr, &c.image_layout) == VK_SUCCESS &&
              vkCreateShaderModule(dev, &mi, nullptr, &module) == VK_SUCCESS &&
              vkCreateDescriptorPool(dev, &pi, nullptr, &c.image_pool) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = c.image_layout;
        ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &c.image_pipeline) == VK_SUCCESS;
    }
    if (module) vkDestroyShaderModule(dev, module, nullptr);
    if (!ok) destroy_target_image_converter(device, c);
}

}  // namespace

void destroy_target_converter(vulkan::Device& device, TargetConverter& c) {
    destroy_target_image_converter(device, c);
    const VkDevice dev = device.device();
    if (c.pipeline) vkDestroyPipeline(dev, c.pipeline, nullptr);
    if (c.pool) vkDestroyDescriptorPool(dev, c.pool, nullptr);
    if (c.layout) vkDestroyPipelineLayout(dev, c.layout, nullptr);
    if (c.set_layout) vkDestroyDescriptorSetLayout(dev, c.set_layout, nullptr);
    if (c.tables.buffer) vulkan::destroy_buffer(device, c.tables);
    c = TargetConverter{};
}

VkDescriptorSet target_converter_set(vulkan::Device& device, TargetConverter& c, VkBuffer work, VkDeviceSize work_bytes,
                                     VkBuffer background, VkDeviceSize background_bytes) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device.device(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (!background) background = work, background_bytes = work_bytes;
    const VkDescriptorBufferInfo bi[3] = {{work, 0, work_bytes}, {c.tables.buffer, 0, VK_WHOLE_SIZE},
                                          {background, 0, background_bytes}};
    VkWriteDescriptorSet w[3];
    for (u32 i = 0; i < 3; ++i) {
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(device.device(), 3, w, 0, nullptr);
    return set;
}

void free_target_converter_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set) {
    if (set) vkFreeDescriptorSets(device.device(), c.pool, 1, &set);
}

void record_target_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set,
                              const TargetTextureConstants& k) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof k, &k);
    vkCmdDispatch(cmd, (k.width + 7) / 8, (k.height + 7) / 8, 1);
}

VkDescriptorSet target_image_set(vulkan::Device& device, TargetConverter& c) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.image_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c.image_set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device.device(), &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    return set;
}

void write_target_image_set(vulkan::Device& device, const TargetConverter& c, VkDescriptorSet set, VkImageView image,
                            VkImageView texture) {
    const VkDescriptorImageInfo src{VK_NULL_HANDLE, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo tables{c.tables.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo out{VK_NULL_HANDLE, texture, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3];
    const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (u32 i = 0; i < 3; ++i) {
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = types[i];
    }
    w[0].pImageInfo = &src;
    w[1].pBufferInfo = &tables;
    w[2].pImageInfo = &out;
    vkUpdateDescriptorSets(device.device(), 3, w, 0, nullptr);
}

void free_target_image_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set) {
    if (set) vkFreeDescriptorSets(device.device(), c.image_pool, 1, &set);
}

void record_target_image_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set,
                                    const TargetImageConstants& k) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.image_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.image_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, c.image_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof k, &k);
    vkCmdDispatch(cmd, (k.width + 7) / 8, (k.height + 7) / 8, 1);
}

Result<nx::vector<u32>> run_target_conversion(vulkan::Device& device, TargetConverter& c, const TargetTextureConstants& k,
                                              std::span<const u32> image, std::span<const u8> background) {
    const usize out_words = usize{k.width} * k.height * (k.kind == 0 ? 1 : 2);
    const usize work_words = nx::max<usize>(usize{k.out} + out_words, image.size() + k.src);
    auto work = vulkan::create_host_buffer(device, work_words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!work) return nx::err(work.error());
    const usize bg_bytes = nx::max<usize>((background.size() + 3) & ~usize{3}, 4);
    auto bg = vulkan::create_host_buffer(device, bg_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!bg) {
        vulkan::destroy_buffer(device, *work);
        return nx::err(bg.error());
    }
    std::memset(work->mapped, 0, work_words * 4);
    std::memcpy(static_cast<u32*>(work->mapped) + k.src, image.data(), image.size() * 4);
    std::memset(bg->mapped, 0, bg_bytes);
    if (!background.empty()) std::memcpy(bg->mapped, background.data(), background.size());
    vulkan::flush(device, *work);
    vulkan::flush(device, *bg);
    Result<nx::vector<u32>> result = failure("target textures: no descriptor set");
    if (const VkDescriptorSet set = target_converter_set(device, c, work->buffer, work_words * 4, bg->buffer, bg_bytes)) {
        vulkan::OneShot shot(device);
        auto cmd = shot.begin();
        if (!cmd) {
            result = nx::err(cmd.error());
        } else {
            record_target_conversion(*cmd, c, set, k);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
            if (auto done = shot.submit_and_wait(); !done) {
                result = nx::err(done.error());
            } else {
                vulkan::invalidate(device, *work);
                const u32* p = static_cast<const u32*>(work->mapped) + k.out;
                result = nx::vector<u32>(p, p + out_words);
            }
        }
        free_target_converter_set(device, c, set);
    }
    vulkan::destroy_buffer(device, *work);
    vulkan::destroy_buffer(device, *bg);
    return result;
}

TargetCheck check_target_conversion(vulkan::Device& device, TargetConverter& c) {
    TargetCheck check;
    const u64 start = nx::os::ticks_ns();
    u32 seed = 0x2545F491u;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    // (cases: a source, a texture format and swizzle, its type and size; the image's part and its scale's step)
    struct Case {
        TargetSource source;
        u32 format, swizzle, type, width, height, a_width, a_height, step;
    };
    const Case cases[] = {
        {TargetSource::Depth32, 0x13000000u, 0, texture_type::kLinearStrided, 40, 24, 20, 12, 1},
        {TargetSource::Depth32, 0x12000000u, 0, texture_type::kLinearStrided, 33, 17, 33, 17, 2},
        {TargetSource::Depth32, 0x13000000u, 3, texture_type::kTiled, 64, 64, 40, 64, 1},
        {TargetSource::Depth16, 0x09000000u, 0, texture_type::kTiled, 64, 64, 64, 40, 2},
        {TargetSource::Depth16, 0x09000000u, 5, texture_type::kLinearStrided, 30, 10, 18, 10, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 0, texture_type::kLinearStrided, 64, 32, 64, 32, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 1, texture_type::kLinearStrided, 37, 9, 37, 9, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 2, texture_type::kLinearStrided, 37, 9, 20, 9, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 3, texture_type::kLinearStrided, 16, 16, 16, 16, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 4, texture_type::kLinearStrided, 16, 16, 16, 16, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 5, texture_type::kLinearStrided, 16, 16, 16, 16, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 6, texture_type::kLinearStrided, 16, 16, 16, 16, 1},
        {TargetSource::U2F10F10F10, 0x9A000000u, 7, texture_type::kTiled, 32, 32, 32, 32, 1},
    };
    check.ran = true;
    check.ok = true;
    for (const Case& cs : cases) {
        TextureInfo info;
        info.base_format = cs.format, info.swizzle = cs.swizzle, info.type = cs.type;
        info.width = cs.width, info.height = cs.height, info.levels = 1;
        const u32 bytes = cs.source == TargetSource::Depth16 ? 2 : 4;
        info.stride = cs.type == texture_type::kTiled ? 0 : (cs.width + 3) * bytes;  // (a stride past the width)
        const auto d = target_decode(info, cs.source);
        if (!d) {
            check.ok = false;
            check.what = nx::format("format {:08x} swizzle {}: no decode", cs.format, cs.swizzle);
            break;
        }
        TargetTextureConstants k;
        k.width = cs.width, k.height = cs.height, k.a_width = cs.a_width, k.a_height = cs.a_height;
        k.src = 4;
        k.src_step = cs.step, k.src_first = cs.step / 2, k.src_pitch = cs.a_width * cs.step;
        k.source = static_cast<u32>(cs.source);
        k.kind = d->kind, k.mask = d->mask, k.select = d->select, k.alpha_low = d->alpha_low;
        target_packing(k);
        k.bg_tiled = cs.type == texture_type::kTiled;
        k.bg_pitch = k.bg_tiled ? (cs.width + 31) / 32 : info.stride / bytes;
        k.bg_bits = bytes * 8;
        const u32 tw = target_source_words(cs.source);
        // (the image's texels: depths in 0-1 with D16's rounding boundaries, halves of every kind)
        nx::vector<u32> image(usize{k.src_pitch} * cs.a_height * tw);
        for (usize i = 0; i < image.size(); ++i) {
            const u32 r = next();
            if (cs.source == TargetSource::U2F10F10F10) {
                const u32 kinds = r & 7;
                u16 h = static_cast<u16>(r >> 16);
                if (kinds == 0) h = static_cast<u16>(0x7C00 | (r & 0x3FF)) ^ ((r >> 12) & 0x8000);  // (infinity, NaN)
                else if (kinds == 1) h = static_cast<u16>((r >> 16) & 0x83FF);                       // (denormal)
                else if (kinds < 5) h = static_cast<u16>((r >> 16) & 0x3FFF);                       // (0-2)
                const u32 other = next();
                image[i] = h | (other & 0x3FFF0000u);
            } else {
                f32 z = static_cast<f32>(r >> 8) / 16777216.0f;
                if ((r & 3) == 0) z = std::nextafter((static_cast<f32>(r >> 16) + 0.5f) / 65535.0f, (r & 4) ? 2.0f : -1.0f);
                if ((r & 15) == 1) z = (r & 16) ? 1.0f : 0.0f;
                image[i] = std::bit_cast<u32>(z);
            }
        }
        // (guest memory as the write-back leaves it: noise, the image's part written over it)
        const u32 size = texture_size(info);
        nx::vector<u8> guest(size + 8);
        for (u8& v : guest) v = static_cast<u8>(next() >> 24);
        for (u32 y = 0; y < cs.a_height; ++y)
            for (u32 x = 0; x < cs.a_width; ++x) {
                const u32* t = image.data() + (usize{y} * k.src_pitch + k.src_first + usize{x} * k.src_step) * tw;
                const u64 index = k.bg_tiled ? ((u64{y / 32} * k.bg_pitch + x / 32) * 1024 + (y % 32) * 32 + x % 32)
                                             : u64{y} * k.bg_pitch + x;
                if (cs.source == TargetSource::Depth16) {
                    const u16 v = depth16_of(std::bit_cast<f32>(t[0]));
                    std::memcpy(guest.data() + index * 2, &v, 2);
                } else if (cs.source == TargetSource::Depth32) {
                    std::memcpy(guest.data() + index * 4, t, 4);
                } else {
                    const u16 h[4] = {static_cast<u16>(t[0]), static_cast<u16>(t[0] >> 16), static_cast<u16>(t[1]),
                                      static_cast<u16>(t[1] >> 16)};
                    const u32 v = pack_u2f10f10f10(h);
                    std::memcpy(guest.data() + index * 4, &v, 4);
                }
            }
        auto cpu = decode_bytes(info, guest);
        k.out = static_cast<u32>(k.src + image.size() + 3) & ~3u;
        auto gpu = run_target_conversion(device, c, k, image, std::span<const u8>(guest.data(), size));
        if (!cpu || !gpu || cpu->data.size() != gpu->size() * 4) {
            check.ok = false;
            check.what = nx::format("format {:08x} swizzle {}: not decoded or not run", cs.format, cs.swizzle);
            break;
        }
        if (std::memcmp(cpu->data.data(), gpu->data(), cpu->data.size()) != 0) {
            usize at = 0;
            while (cpu->data[at] == reinterpret_cast<const u8*>(gpu->data())[at]) ++at;
            const usize texel = at / cpu->bytes;
            check.ok = false;
            check.what = nx::format("format {:08x} swizzle {}, texel ({}, {}): the GPU's bytes differ from the decoder's",
                                    cs.format, cs.swizzle, texel % cs.width, texel / cs.width);
            break;
        }
    }
    // (the write-back's packing on the GPU: halves of every kind, as pack_u2f10f10f10 packs them)
    if (check.ok) {
        constexpr u32 w = 37, h = 23;
        const TargetTextureConstants k = target_pack_constants(w, h);
        nx::vector<u32> image(usize{w} * h * 2);
        for (u32& v : image) {
            u32 word = 0;
            for (u32 half = 0; half < 2; ++half) {
                const u32 r = next();
                const u32 kinds = r & 7;
                u16 hv = static_cast<u16>(r >> 16);
                if (kinds == 0) hv = static_cast<u16>(0x7C00 | (r & 0x3FF)) ^ ((r >> 12) & 0x8000);  // (infinity, NaN)
                else if (kinds == 1) hv = static_cast<u16>((r >> 16) & 0x83FF);                       // (denormal)
                else if (kinds < 5) hv = static_cast<u16>((r >> 16) & 0x3FFF);                       // (0-2)
                word |= u32{hv} << (16 * half);
            }
            v = word;
        }
        nx::vector<u32> cpu(usize{w} * h);
        pack_u2f10f10f10(reinterpret_cast<const u16*>(image.data()), cpu.data(), cpu.size());
        auto gpu = run_target_conversion(device, c, k, image, {});
        if (!gpu || gpu->size() != cpu.size()) {
            check.ok = false;
            check.what = "the packing on the GPU: not run";
        } else if (std::memcmp(gpu->data(), cpu.data(), cpu.size() * 4) != 0) {
            usize at = 0;
            while ((*gpu)[at] == cpu[at]) ++at;
            check.ok = false;
            check.what = nx::format("the packing on the GPU, pixel {}: {:08x}, the CPU's {:08x}", at, (*gpu)[at], cpu[at]);
        }
    }
    check.ms = static_cast<f64>(nx::os::ticks_ns() - start) / 1e6;
    return check;
}

TargetCheck check_target_image_conversion(vulkan::Device& device, TargetConverter& c) {
    TargetCheck check;
    if (!c.images_ready()) return check;
    const u64 start = nx::os::ticks_ns();
    u32 seed = 0x9E3779B9u;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    auto is_nan = [](u16 h) { return (h & 0x7C00) == 0x7C00 && (h & 0x3FF); };
    check.ran = true;
    check.ok = true;
    // (each swizzle: a texture of w x h from the image's texels at (x0, y0), the image larger)
    constexpr u32 sw = 45, sh = 29, w = 37, h = 23, x0 = 5, y0 = 3;
    for (u32 swizzle = 0; swizzle < 8 && check.ok; ++swizzle) {
        TextureInfo info;
        info.base_format = 0x9A000000u, info.swizzle = swizzle, info.type = texture_type::kLinearStrided;
        info.width = w, info.height = h, info.levels = 1, info.stride = w * 4;
        const auto d = target_decode(info, TargetSource::U2F10F10F10);
        if (!d) {
            check.ok = false;
            check.what = nx::format("swizzle {}: no decode", swizzle);
            break;
        }
        // (a NaN's sign is the device's once fetched as a float: NaNs positive here, which pack to the same)
        nx::vector<u16> halves(usize{sw} * sh * 4);
        for (u16& hv : halves) {
            const u32 r = next();
            const u32 kinds = r & 7;
            hv = static_cast<u16>(r >> 16);
            if (kinds == 0) hv = static_cast<u16>(0x7C00 | (r & 0x3FF)) ^ ((r & 0x3FF) ? 0 : (r >> 12) & 0x8000);  // (infinity, NaN)
            else if (kinds == 1) hv = static_cast<u16>((r >> 16) & 0x83FF);  // (denormal)
            else if (kinds < 5) hv = static_cast<u16>((r >> 16) & 0x3FFF);  // (0-2)
            else if (is_nan(hv)) hv &= 0x7FFF;
        }
        nx::vector<u8> guest(usize{w} * h * 4);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                const u32 v = pack_u2f10f10f10(halves.data() + (usize{y0 + y} * sw + x0 + x) * 4);
                std::memcpy(guest.data() + (usize{y} * w + x) * 4, &v, 4);
            }
        auto cpu = decode_bytes(info, guest);
        vulkan::ImageDesc sd;
        sd.format = VK_FORMAT_R16G16B16A16_SFLOAT, sd.width = sw, sd.height = sh;
        vulkan::ImageDesc od = sd;
        od.width = w, od.height = h;
        od.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        auto src = vulkan::create_image(device, sd);
        auto out = vulkan::create_image(device, od);
        Result<nx::vector<u8>> gpu = failure("not run");
        const VkDescriptorSet set = src && out ? target_image_set(device, c) : VK_NULL_HANDLE;
        if (set && vulkan::upload_image(device, *src, {reinterpret_cast<const u8*>(halves.data()), halves.size() * 2})) {
            write_target_image_set(device, c, set, src->view, out->view);
            TargetImageConstants k;
            k.width = w, k.height = h, k.x0 = x0, k.y0 = y0;
            k.select = d->select, k.alpha_low = d->alpha_low ? 1 : 0;
            TargetTextureConstants packing;
            target_packing(packing);
            k.alpha1 = packing.alpha1, k.alpha2 = packing.alpha2, k.alpha3 = packing.alpha3;
            vulkan::OneShot shot(device);
            if (auto cmd = shot.begin()) {
                vulkan::transition(*cmd, *out, VK_IMAGE_LAYOUT_GENERAL);
                record_target_image_conversion(*cmd, c, set, k);
                if (shot.submit_and_wait()) gpu = vulkan::read_image(device, *out);
            }
        }
        free_target_image_set(device, c, set);
        if (src) vulkan::destroy_image(device, *src);
        if (out) vulkan::destroy_image(device, *out);
        if (!cpu || !gpu || cpu->data.size() != gpu->size()) {
            check.ok = false;
            check.what = nx::format("swizzle {}: not decoded or not run", swizzle);
            break;
        }
        for (usize i = 0; i + 1 < gpu->size(); i += 2) {
            u16 g, e;
            std::memcpy(&g, gpu->data() + i, 2);
            std::memcpy(&e, cpu->data.data() + i, 2);
            if (g == e || (is_nan(g) && is_nan(e))) continue;
            const usize texel = i / 8;
            check.ok = false;
            check.what = nx::format("swizzle {}, texel ({}, {}) half {}: the GPU's {:04x}, the decoder's {:04x}", swizzle,
                                    texel % w, texel / w, i / 2 % 4, g, e);
            break;
        }
    }
    if (!check.ok) destroy_target_image_converter(device, c);
    check.ms = static_cast<f64>(nx::os::ticks_ns() - start) / 1e6;
    return check;
}

}  // namespace psvrecore::gxm
