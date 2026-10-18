#pragma once

// A module's relocations: the PT_SCE_RELA segments hold entries in two encodings (12-byte "long" and 8-byte
// "short"), each naming the ARM relocation type, the segment to patch and the offset in it, and the segment whose
// address the value is relative to, plus an addend. A long entry may describe a second relocation a few bytes after
// the first (a MOVW/MOVT pair); decode() returns it as its own entry.
//
// Supported: the absolute and PC-relative data types and MOVW/MOVT (ARM and Thumb-2) - all that SDK-built
// binaries have been seen to use are absolute data and Thumb-2 MOVW/MOVT. Any other type (branches, GOT-relative,
// ...) is an error, never ignored.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/formats/elf.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::reloc {

// ARM ELF relocation types (AAELF numbering).
enum Type : u8 {
    kNone = 0,
    kAbs32 = 2,
    kRel32 = 3,
    kTarget1 = 38,
    kV4bx = 40,
    kPrel31 = 42,
    kMovwAbsNc = 43,
    kMovtAbs = 44,
    kThmMovwAbsNc = 47,
    kThmMovtAbs = 48,
};

struct Entry {
    u8 type = 0;
    u8 symbol_segment = 0;  // the value is this segment's address plus the addend
    u8 patch_segment = 0;   // the segment patched
    u32 offset = 0;         // in the patched segment
    u32 addend = 0;         // (32-bit wrap-around arithmetic: long entries' addends may be negative)
};

// Decodes one PT_SCE_RELA segment's bytes.
Result<nx::vector<Entry>> decode(std::span<const u8> data);
// Every PT_SCE_RELA segment of a module, in order.
Result<nx::vector<Entry>> decode_all(const elf::File& file);

const char* type_name(u8 type);

// A segment's memory: where it is in guest memory and its bytes (at least its file data).
struct Segment {
    u32 base = 0;
    std::span<u8> bytes;
};

// The value the relocation computes for a module whose segments are at these bases.
Result<u32> value(const Entry& entry, std::span<const u32> bases);

// Writes the relocation into its segment.
Result<void> apply(const Entry& entry, std::span<const Segment> segments);

// Whether the segment already holds what applying the relocation would write (a module as linked holds its
// relocations applied for its own addresses). Fails only when the relocation cannot be evaluated.
Result<bool> is_applied(const Entry& entry, std::span<const Segment> segments);

}  // namespace psvrecore::reloc
