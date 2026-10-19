#include "psvrecore/usse/gxp.hpp"

#include "psvrecore/io/bytes.hpp"

namespace psvrecore::usse {

// The layout, as far as it is read here (offsets in the header; "self-relative" offsets count from their own
// field):
//   0x00 "GXP\0", 0x04 version (major, minor), 0x08 size
//   0x14 flags: bit 0 fragment program, 3 discard, 4 depth replace, 5 sprite coordinate, 6 native color,
//        7 previous color (FRAGCOLOR) read
//   0x24 parameter count, 0x28 self-relative offset of the parameter records (16 bytes each)
//   0x2C self-relative offset of the varyings block
//   0x3C/0x40, 0x44/0x48 primary and secondary programs: word count, self-relative offset
//   0x64 default uniform buffer size in 32-bit words
//   0x70 literals (8 bytes: offset in the constants block, value), 0x78 uniform buffer slots (8: buffer, offset, bias),
//   0x80 sampler state slots (4: word, offset), 0x90 containers (8: index, -, base, size); each a count and a
//   self-relative offset. (These are not checked against libgxm, which has no function reporting them; the
//   interpreter's results on compiled shaders are what checks them.)
// A parameter record: a self-relative name offset (0: no name); a word with the category (bits 0-3), type (4-7),
// component count (8-11), container (12-15), semantic (16-23, attributes only), semantic index (24-27) and cube
// sampler flag (28); the array size; the resource index.
// The varyings block: word 2 is, for a vertex program, the mask of primary registers holding attributes read in
// their declared format; for a fragment program, the output type (bits 16-19) and component count (24-27). A
// vertex program's outputs are flags in word 4 (position 12, fog 9, color0 11, color1 10, point size 8, clip
// planes 0-7) and one 3-bit group per texture coordinate in word 5 (non-zero when written). A fragment program's
// inputs are 16-byte records from 0x14, as many as the low half of word 3, each naming an interpolated input
// (bits 12-15: 0-9 a texture coordinate, 0xA color0, 0xB color1, 0xC fog, 0xD window position, 0xF none; bit 30:
// the sprite coordinate instead) and a texture coordinate sampled without a program instruction (bits 0-3, 0xF
// none; bit 10: the sprite coordinate instead), the interpolated value's format (bits 28-29: 0 f32, 1 fx10,
// 2 f16) and component count less one (bits 22-23); bit 25 marks the last record.

namespace {

constexpr u32 kMagic = 0x00505847u;

u32 interpolated(u32 record) {
    if ((record >> 30) & 1) return varying::kSpriteCoord;
    const u32 which = (record >> 12) & 0xF;
    if (which < 10) return varying::kTexCoord0 << which;
    switch (which) {
        case 0xA: return varying::kColor0;
        case 0xB: return varying::kColor1;
        case 0xC: return varying::kFog;
        case 0xD: return varying::kPosition;
        default: return 0;
    }
}

u32 sampled(u32 record) {
    const u32 which = record & 0xF;
    if (which == 0xF) return 0;
    if ((record >> 10) & 1) return varying::kSpriteCoord;
    return which < 10 ? varying::kTexCoord0 << which : 0;
}

}  // namespace

bool gxp_valid(std::span<const u8> data) {
    io::ByteReader r(data);
    return data.size() >= 8 && r.le<u32>(0) == kMagic && r.byte(4) == 1 && (r.byte(5) == 4 || r.byte(5) == 5);
}

std::optional<usize> Gxp::find(nx::string_view wanted) const {
    for (usize i = 0; i < parameters.size(); ++i)
        if (parameters[i].named && nx::string_view(parameters[i].name) == wanted) return i;
    return std::nullopt;
}

std::optional<usize> Gxp::find(Semantic wanted, u32 index) const {
    for (usize i = 0; i < parameters.size(); ++i) {
        const Parameter& p = parameters[i];
        if (p.category == ParameterCategory::Attribute && p.semantic == wanted && p.semantic_index == index) return i;
    }
    return std::nullopt;
}

Result<Gxp> parse_gxp(std::span<const u8> data) {
    io::ByteReader r(data);
    if (data.size() < 0x68 || r.le<u32>(0) != kMagic) return failure("not a GXP file");
    Gxp g;
    g.version_major = r.byte(4);
    g.version_minor = r.byte(5);
    g.size = r.le<u32>(0x08);
    const u32 flags = r.le<u32>(0x14);
    g.type = (flags & 1) ? ShaderType::Fragment : ShaderType::Vertex;
    g.discard = (flags >> 3) & 1;
    g.depth_replace = (flags >> 4) & 1;
    g.sprite_coord = (flags >> 5) & 1;
    g.native_color = (flags >> 6) & 1;
    g.frag_color = (flags >> 7) & 1;
    g.default_uniform_buffer_size = r.le<u32>(0x64) * 4;

    auto program = [&](u32 count_at, u32 offset_at, nx::vector<u64>& out) -> bool {
        const u32 count = r.le<u32>(count_at);
        const u64 at = u64{offset_at} + r.le<u32>(offset_at);
        if (count && (at > data.size() || count > (data.size() - at) / 8)) return false;
        out.reserve(count);
        for (u32 i = 0; i < count; ++i) out.push_back(r.le<u64>(static_cast<usize>(at + 8 * i)));
        return true;
    };
    if (!program(0x3C, 0x40, g.primary) || !program(0x44, 0x48, g.secondary) || r.failed())
        return failure("GXP program outside the file");

    const usize varyings = 0x2C + usize{r.le<u32>(0x2C)};
    if (!r.contains(varyings, 0x18)) return failure("GXP varyings outside the file");
    if (g.type == ShaderType::Vertex) {
        const u32 flags4 = r.le<u32>(varyings + 0x10), coords = r.le<u32>(varyings + 0x14);
        u32 out = 0;
        if ((flags4 >> 12) & 1) out |= varying::kPosition;
        if ((flags4 >> 9) & 1) out |= varying::kFog;
        if ((flags4 >> 11) & 1) out |= varying::kColor0;
        if ((flags4 >> 10) & 1) out |= varying::kColor1;
        if ((flags4 >> 8) & 1) out |= varying::kPointSize;
        out |= (flags4 & 0xFF) * varying::kClip0;
        for (u32 k = 0; k < 10; ++k)
            if ((coords >> (3 * k)) & 7) out |= varying::kTexCoord0 << k;
        g.vertex_outputs = out;
    } else {
        const u32 format = r.le<u32>(varyings + 0x08);
        g.output_type = static_cast<ParameterType>((format >> 16) & 0xF);
        g.output_components = static_cast<u8>((format >> 24) & 0xF);
        const u32 count = r.le<u32>(varyings + 0x0C) & 0xFFFF;
        if (!r.contains(varyings + 0x14, usize{count} * 16)) return failure("GXP fragment inputs outside the file");
        u32 offset = 0;
        for (u32 i = 0; i < count; ++i) {
            const usize at = varyings + 0x14 + 16 * usize{i};
            const u32 record = r.le<u32>(at);
            g.fragment_inputs |= interpolated(record) | sampled(record);
            FragmentInput in;
            for (u32 j = 0; j < 4; ++j) in.raw[j] = r.le<u32>(at + 4 * j);
            in.interpolated = interpolated(record);
            in.sampled = sampled(record);
            in.format = static_cast<u8>((record >> 28) & 3);
            in.components = static_cast<u8>(((record >> 22) & 3) + 1);
            in.offset = static_cast<u16>(offset);
            // (packed in record order without alignment: f32 a register per component, f16 two components per
            // register, fx10 always two registers; checked by the interpreter on compiled shaders)
            offset += in.format == 0 ? in.components : in.format == 1 ? 2u : (in.components + 1u) / 2;
            g.inputs.push_back(in);
        }
    }
    const u32 reg_formats = g.type == ShaderType::Vertex ? r.le<u32>(varyings + 0x08) : 0;

    const u32 count = r.le<u32>(0x24);
    const usize records = 0x28 + usize{r.le<u32>(0x28)};
    if (count && (records > data.size() || count > (data.size() - records) / 16))
        return failure("GXP parameters outside the file");
    g.parameters.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const usize at = records + 16 * usize{i};
        Parameter p;
        p.offset = static_cast<u32>(at);
        if (const u32 name = r.le<u32>(at); name) {
            if (!r.contains(at + name, 1)) return failure("GXP parameter name outside the file");
            p.name = r.cstr(at + name);
            p.named = true;
        }
        const u32 info = r.le<u32>(at + 4);
        p.category = static_cast<ParameterCategory>(info & 0xF);
        p.type = static_cast<ParameterType>((info >> 4) & 0xF);
        p.components = static_cast<u8>((info >> 8) & 0xF);
        p.container = static_cast<u8>((info >> 12) & 0xF);
        if (p.category == ParameterCategory::Attribute) p.semantic = static_cast<Semantic>((info >> 16) & 0xFF);
        p.semantic_index = static_cast<u8>((info >> 24) & 0xF);
        p.sampler_cube = (info >> 28) & 1;
        p.array_size = r.le<u32>(at + 8);
        p.resource = r.le<u32>(at + 12);
        p.reg_format = p.category == ParameterCategory::Attribute && p.resource < 32 && ((reg_formats >> p.resource) & 1);
        g.parameters.push_back(std::move(p));
    }
    // The tables of the secondary attributes' set-up, each a count and a self-relative offset.
    auto table = [&](usize at, usize entry, auto&& read) -> bool {
        const u32 n = r.le<u32>(at);
        const usize first = at + 4 + usize{r.le<u32>(at + 4)};
        if (n && (first > data.size() || n > (data.size() - first) / entry)) return false;
        for (u32 i = 0; i < n; ++i) read(first + entry * i);
        return true;
    };
    const bool tables =
        table(0x70, 8, [&](usize at) { g.literals.push_back(Literal{r.le<u32>(at), r.le<u32>(at + 4)}); }) &&
        table(0x78, 8, [&](usize at) {
            g.buffer_slots.push_back(BufferSlot{r.le<u16>(at), r.le<u16>(at + 2), static_cast<i32>(r.le<u32>(at + 4))});
        }) &&
        table(0x80, 4, [&](usize at) { g.sampler_slots.push_back(SamplerSlot{r.le<u16>(at), r.le<u16>(at + 2)}); }) &&
        table(0x90, 8, [&](usize at) {
            g.containers.push_back(Container{r.le<u16>(at), r.le<u16>(at + 4), r.le<u16>(at + 6)});
        });
    if (!tables) return failure("GXP table outside the file");
    if (r.failed()) return failure("GXP truncated");
    return g;
}

const Container* Gxp::container(u16 index) const {
    for (const Container& c : containers)
        if (c.index == index) return &c;
    return nullptr;
}

}  // namespace psvrecore::usse
