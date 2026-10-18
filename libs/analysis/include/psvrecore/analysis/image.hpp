#pragma once

// A module as it will be in guest memory: its loadable segments at the addresses chosen for them, with its
// relocations applied for those addresses, and its module information, imports and exports translated to them.
//
// A module's segments are normally loaded where the module was linked. Relocatable modules are often linked at the
// same address (0x81000000), so several of them can only be loaded together by moving some: `bases` gives a new
// address for each loadable segment, in program-header order.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/formats/elf.hpp"
#include "psvrecore/formats/module.hpp"
#include "psvrecore/formats/reloc.hpp"
#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::analysis {

struct Segment {
    int index = 0;      // program header index
    u32 base = 0;       // guest address
    u32 linked = 0;     // the address the module was linked for
    u32 file_size = 0;
    u32 flags = 0;      // elf::kSegment{Read,Write,Execute}
    nx::vector<u8> bytes;  // memsz bytes, relocated; zero past the file data

    u32 size() const { return static_cast<u32>(bytes.size()); }
    bool contains(u32 address) const { return address >= base && address - base < size(); }
    bool executable() const { return (flags & elf::kSegmentExecute) != 0; }
    bool writable() const { return (flags & elf::kSegmentWrite) != 0; }
};

// A relocation as applied: where it wrote and what it computed (the address of something in the module).
struct Pointer {
    u32 at = 0;      // the patched address
    u32 value = 0;   // the value it computes (a code address keeps its Thumb bit)
    u8 type = 0;     // reloc::Type
};

class Image {
public:
    // Reads a SELF or an ELF file. `bases` is empty to load the module where it was linked.
    static Result<Image> load(nx::fs::path_view path, std::span<const u32> bases = {});
    // The same from the file's bytes (copied).
    static Result<Image> from_bytes(std::span<const u8> data, std::span<const u32> bases = {});
    // An image of these segments and nothing else (no module information): for synthetic code in tests.
    static Image from_segments(nx::vector<Segment> segments);

    const nx::vector<Segment>& segments() const { return segments_; }
    const module::Module& module() const { return module_; }
    const nx::vector<Pointer>& pointers() const { return pointers_; }
    u16 elf_type() const { return elf_type_; }

    const Segment* segment_of(u32 address) const;
    bool contains(u32 address, u32 size) const;
    bool executable(u32 address) const;
    bool writable(u32 address) const;

    // Reads; an address outside the image reads as 0 (check contains() first where it matters).
    u8 u8_at(u32 address) const;
    u16 u16_at(u32 address) const;
    u32 u32_at(u32 address) const;

    // The address in this image of an address as the module was linked (unchanged when no segment holds it).
    u32 translate(u32 linked) const;

private:
    nx::vector<Segment> segments_;
    module::Module module_;
    nx::vector<Pointer> pointers_;
    u16 elf_type_ = 0;
};

}  // namespace psvrecore::analysis
