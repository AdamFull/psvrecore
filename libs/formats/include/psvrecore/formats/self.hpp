#pragma once

// SELF: the container Vita executables and modules ship in. It has an SCE header, an application-info block, the
// embedded ELF header and program headers, and per-segment information (where each segment's data is, and whether
// it is compressed or encrypted). This milestone handles plaintext SELFs ("fake SELFs", as SDK tools produce),
// compressed or not; encrypted segments are reported, not decrypted.

#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::self {

inline constexpr u32 kMagic = 0x00454353;  // "SCE\0"
inline constexpr u16 kHeaderTypeSelf = 1;

struct Header {
    u32 version = 0;
    u16 sdk_type = 0;
    u16 header_type = 0;
    u32 metadata_offset = 0;
    u64 header_length = 0;
    u64 elf_size = 0;
    u64 self_size = 0;
    u64 app_info_offset = 0;
    u64 elf_offset = 0;
    u64 program_headers_offset = 0;
    u64 section_headers_offset = 0;
    u64 segment_info_offset = 0;
    u64 sce_version_offset = 0;
    u64 control_info_offset = 0;
    u64 control_info_size = 0;
};

struct AppInfo {
    u64 auth_id = 0;
    u32 vendor_id = 0;
    u32 self_type = 0;
    u64 version = 0;
};

// Where one program header's data is in the SELF.
struct SegmentInfo {
    u64 offset = 0;
    u64 size = 0;
    u64 compression = 0;  // 1 plain, 2 zlib
    u64 encryption = 0;   // 1 encrypted, 2 plain

    bool compressed() const { return compression == 2; }
    bool encrypted() const { return encryption == 1; }
};

struct File {
    Header header;
    AppInfo app_info;
    u16 elf_type = 0;
    nx::vector<SegmentInfo> segments;  // one per program header

    bool encrypted() const;
};

// Starts with the SCE magic?
bool is_self(std::span<const u8> data);

Result<File> parse(std::span<const u8> data);

// The ELF file a SELF holds: the ELF header, the program headers, and each segment's data at its p_offset
// (decompressed). Section headers are dropped, as SELFs do not keep them. Fails for encrypted segments.
Result<nx::vector<u8>> to_elf(std::span<const u8> data);

}  // namespace psvrecore::self
