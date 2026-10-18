#include "psvrecore/formats/self.hpp"

#include <cstring>

#include "psvrecore/io/bytes.hpp"
#include "core/foundation/strings/format.h"
#include "zlib-ng.h"

namespace psvrecore::self {

namespace {

// SCE header fields (byte offsets).
enum HeaderField : usize {
    kMagicField = 0, kVersion = 4, kSdkType = 8, kHeaderType = 10, kMetadataOffset = 12, kHeaderLength = 16,
    kElfSize = 24, kSelfSize = 32, kAppInfoOffset = 56, kElfOffset = 64, kPhdrOffset = 72, kShdrOffset = 80,
    kSegmentInfoOffset = 88, kSceVersionOffset = 96, kControlInfoOffset = 104, kControlInfoSize = 112,
    kHeaderSize = 120,
};
constexpr usize kAppInfoSize = 24, kSegmentInfoSize = 32;
constexpr usize kElfHeaderSize = 52, kElfPhOff = 28, kElfShOff = 32, kElfPhEntSize = 42, kElfPhNum = 44,
                kElfShEntSize = 46, kElfShNum = 48, kElfShStrNdx = 50, kElfType = 16;
constexpr usize kProgramHeaderSize = 32;

template <class T>
void store_le(nx::vector<u8>& out, usize at, T value) {
    std::memcpy(out.data() + at, &value, sizeof value);
}

}  // namespace

bool File::encrypted() const {
    for (const auto& s : segments)
        if (s.encrypted()) return true;
    return false;
}

bool is_self(std::span<const u8> data) {
    return io::ByteReader(data).le<u32>(kMagicField) == kMagic && data.size() >= kHeaderSize;
}

Result<File> parse(std::span<const u8> bytes) {
    const io::ByteReader d(bytes);
    if (!is_self(bytes)) return failure("not a SELF file");
    File f;
    Header& h = f.header;
    h.version = d.le<u32>(kVersion);
    h.sdk_type = d.le<u16>(kSdkType);
    h.header_type = d.le<u16>(kHeaderType);
    h.metadata_offset = d.le<u32>(kMetadataOffset);
    h.header_length = d.le<u64>(kHeaderLength);
    h.elf_size = d.le<u64>(kElfSize);
    h.self_size = d.le<u64>(kSelfSize);
    h.app_info_offset = d.le<u64>(kAppInfoOffset);
    h.elf_offset = d.le<u64>(kElfOffset);
    h.program_headers_offset = d.le<u64>(kPhdrOffset);
    h.section_headers_offset = d.le<u64>(kShdrOffset);
    h.segment_info_offset = d.le<u64>(kSegmentInfoOffset);
    h.sce_version_offset = d.le<u64>(kSceVersionOffset);
    h.control_info_offset = d.le<u64>(kControlInfoOffset);
    h.control_info_size = d.le<u64>(kControlInfoSize);
    if (h.header_type != kHeaderTypeSelf) return failure(nx::format("SCE header type {} is not a SELF", h.header_type));

    const usize ai = static_cast<usize>(h.app_info_offset);
    if (!d.contains(ai, kAppInfoSize)) return failure("SELF application info out of bounds");
    f.app_info = AppInfo{d.le<u64>(ai), d.le<u32>(ai + 8), d.le<u32>(ai + 12), d.le<u64>(ai + 16)};

    const usize eo = static_cast<usize>(h.elf_offset);
    if (!d.matches(eo, "\x7f" "ELF")) return failure("SELF does not contain an ELF header");
    f.elf_type = d.le<u16>(eo + kElfType);
    const u16 phnum = d.le<u16>(eo + kElfPhNum);
    for (usize i = 0; i < phnum; ++i) {
        const usize at = static_cast<usize>(h.segment_info_offset) + i * kSegmentInfoSize;
        f.segments.push_back(SegmentInfo{d.le<u64>(at), d.le<u64>(at + 8), d.le<u64>(at + 16), d.le<u64>(at + 24)});
    }
    if (d.failed()) return failure("SELF headers out of bounds");
    for (usize i = 0; i < f.segments.size(); ++i) {
        const auto& s = f.segments[i];
        if ((s.compression != 1 && s.compression != 2) || (s.encryption != 1 && s.encryption != 2))
            return failure(nx::format("SELF segment {}: unknown compression {} / encryption {}", i, s.compression,
                                      s.encryption));
        if (!d.contains(static_cast<usize>(s.offset), static_cast<usize>(s.size)))
            return failure(nx::format("SELF segment {} out of bounds", i));
    }
    return f;
}

Result<nx::vector<u8>> to_elf(std::span<const u8> bytes) {
    auto parsed = parse(bytes);
    if (!parsed) return nx::err(parsed.error());
    const File& f = *parsed;
    if (f.encrypted()) return failure("SELF is encrypted: decrypting it needs keys, which this tool does not handle yet");

    const io::ByteReader d(bytes);
    const usize eo = static_cast<usize>(f.header.elf_offset);
    const usize po = static_cast<usize>(f.header.program_headers_offset);
    const u32 phoff = d.le<u32>(eo + kElfPhOff);
    const u16 phentsize = d.le<u16>(eo + kElfPhEntSize), phnum = d.le<u16>(eo + kElfPhNum);
    if (phentsize != kProgramHeaderSize) return failure(nx::format("unexpected program header size {}", phentsize));

    // The output's size: the headers, then every segment's data where its program header says.
    usize size = nx::max<usize>(kElfHeaderSize, phoff + usize{phnum} * kProgramHeaderSize);
    for (usize i = 0; i < phnum; ++i) {
        const usize at = po + i * kProgramHeaderSize;
        size = nx::max<usize>(size, usize{d.le<u32>(at + 4)} + d.le<u32>(at + 16));
    }
    if (d.failed()) return failure("SELF program headers out of bounds");

    nx::vector<u8> out(size, 0);
    std::memcpy(out.data(), d.slice(eo, kElfHeaderSize).data(), kElfHeaderSize);
    std::memcpy(out.data() + phoff, d.slice(po, usize{phnum} * kProgramHeaderSize).data(), usize{phnum} * kProgramHeaderSize);
    store_le<u32>(out, kElfShOff, 0);  // no section headers in the output
    store_le<u16>(out, kElfShEntSize, 0);
    store_le<u16>(out, kElfShNum, 0);
    store_le<u16>(out, kElfShStrNdx, 0);

    for (usize i = 0; i < phnum; ++i) {
        const usize at = po + i * kProgramHeaderSize;
        const u32 p_offset = d.le<u32>(at + 4), p_filesz = d.le<u32>(at + 16);
        if (p_filesz == 0) continue;
        const SegmentInfo& s = f.segments[i];
        const auto src = d.slice(static_cast<usize>(s.offset), static_cast<usize>(s.size));
        if (!s.compressed()) {
            if (src.size() != p_filesz)
                return failure(nx::format("SELF segment {}: {} bytes stored, the program header says {}", i, src.size(),
                                          p_filesz));
            std::memcpy(out.data() + p_offset, src.data(), p_filesz);
            continue;
        }
        size_t out_size = p_filesz;
        const int32_t status = zng_uncompress(out.data() + p_offset, &out_size, src.data(), src.size());
        if (status != Z_OK || out_size != p_filesz)
            return failure(nx::format("SELF segment {}: decompression failed (zlib status {}, {} of {} bytes)", i,
                                      status, out_size, p_filesz));
    }
    return out;
}

}  // namespace psvrecore::self
