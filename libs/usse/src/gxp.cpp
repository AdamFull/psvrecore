#include "psvrecore/usse/gxp.hpp"

#include "psvrecore/io/bytes.hpp"

namespace psvrecore::usse {

// The header fields used here: "GXP\0", the size at 0x08, the program type at 0x1C, and the two programs as a
// word count followed by a self-relative offset (primary 0x3C/0x40, secondary 0x44/0x48).
Result<Gxp> parse_gxp(std::span<const u8> data) {
    io::ByteReader r(data);
    if (data.size() < 0x50 || r.le<u32>(0) != 0x00505847u) return failure("not a GXP file");
    Gxp g;
    g.size = r.le<u32>(0x08);
    g.type = (r.le<u32>(0x1C) & 1) ? ShaderType::Fragment : ShaderType::Vertex;
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
    return g;
}

}  // namespace psvrecore::usse
