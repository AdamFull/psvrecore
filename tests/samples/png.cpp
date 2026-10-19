// A minimal PNG writer: 8-bit RGBA, deflate's stored blocks (no compression).

#include "png.hpp"

#include <cstdio>

namespace {

u32 crc32(const u8* data, usize size, u32 crc = 0) {
    static u32 table[256];
    static bool made = false;
    if (!made) {
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        made = true;
    }
    crc = ~crc;
    for (usize i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void be32(nx::vector<u8>& out, u32 v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<u8>(v >> s));
}

void chunk(nx::vector<u8>& out, const char* type, const nx::vector<u8>& data) {
    be32(out, static_cast<u32>(data.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

bool write_png(const char* path, u32 width, u32 height, const u8* rgba) {
    nx::vector<u8> raw;
    raw.reserve(usize{height} * (width * 4 + 1));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + usize{y} * width * 4, rgba + usize{y + 1} * width * 4);
    }
    nx::vector<u8> z{0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    for (usize at = 0; at < raw.size() || at == 0;) {
        const usize n = nx::min<usize>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<u8>(n)), z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n)), z.push_back(static_cast<u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<isize>(at), raw.begin() + static_cast<isize>(at + n));
        at += n;
        if (!n) break;
    }
    be32(z, b << 16 | a);
    nx::vector<u8> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    nx::vector<u8> ihdr;
    be32(ihdr, width), be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}
