#pragma once

// Little-endian ELF32 reader (ARM): headers, program headers, sections, symbols; and Unix "ar" archives (the SDK's
// stub libraries). Vita executables and modules are ELF files with their own file types and segment types.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/io/bytes.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::elf {

inline constexpr u16 kTypeRel = 1;               // relocatable object (stub library members)
inline constexpr u16 kTypeExec = 2;
inline constexpr u16 kTypeSceExec = 0xFE00;      // fixed-address executable
inline constexpr u16 kTypeSceRelExec = 0xFE04;   // relocatable executable or module (PRX)
inline constexpr u16 kMachineArm = 40;

inline constexpr u32 kSegmentLoad = 1;                // PT_LOAD
inline constexpr u32 kSegmentSceRela = 0x60000000;    // the module's relocations (their own format, see reloc.hpp)
inline constexpr u32 kSegmentSceComment = 0x6FFFFF00;
inline constexpr u32 kSegmentSceVersion = 0x6FFFFF01;
inline constexpr u32 kSegmentArmExidx = 0x70000001;   // PT_ARM_EXIDX

inline constexpr u32 kSectionSymtab = 2;
inline constexpr u32 kSectionNobits = 8;

inline constexpr u32 kSegmentExecute = 1, kSegmentWrite = 2, kSegmentRead = 4;  // p_flags

struct Section {
    nx::string name;
    u32 type = 0;
    u32 flags = 0;
    u32 addr = 0;
    u32 offset = 0;
    u32 size = 0;
    u32 link = 0;
    u32 info = 0;
    u32 entsize = 0;
};

struct Segment {
    u32 type = 0;
    u32 offset = 0;
    u32 vaddr = 0;
    u32 paddr = 0;
    u32 filesz = 0;
    u32 memsz = 0;
    u32 flags = 0;
    u32 align = 0;

    bool loadable() const { return type == kSegmentLoad; }
    bool contains(u32 address) const { return address >= vaddr && address - vaddr < memsz; }
};

struct Symbol {
    nx::string name;
    u32 value = 0;
    u32 size = 0;
    u8 info = 0;
    u16 section = 0;

    u8 binding() const { return info >> 4; }  // 0 local, 1 global, 2 weak
    u8 type() const { return info & 15; }     // 1 object, 2 function
};

inline constexpr u16 kSectionAbsolute = 0xFFF1;  // SHN_ABS: a symbol whose value is a constant

class File {
public:
    // Parses `data`, which must outlive the File (contents are views into it).
    static Result<File> parse(std::span<const u8> data);

    u16 type() const { return type_; }
    u16 machine() const { return machine_; }
    u32 entry() const { return entry_; }
    u32 flags() const { return flags_; }

    const nx::vector<Section>& sections() const { return sections_; }
    const nx::vector<Segment>& segments() const { return segments_; }
    const Section* find_section(nx::string_view name) const;

    // Raw contents of a section (empty for NOBITS, or when it lies outside the file).
    std::span<const u8> contents(const Section& section) const;
    // The file bytes of a segment (p_filesz of them; empty when they lie outside the file).
    std::span<const u8> contents(const Segment& segment) const;
    nx::vector<Symbol> symbols() const;

    const io::ByteReader& data() const { return data_; }

private:
    File() = default;

    io::ByteReader data_;
    u16 type_ = 0, machine_ = 0;
    u32 entry_ = 0, flags_ = 0;
    nx::vector<Section> sections_;
    nx::vector<Segment> segments_;
};

// The members of a Unix "ar" archive (GNU long-name tables supported).
struct ArchiveMember {
    nx::string name;
    std::span<const u8> data;
};
Result<nx::vector<ArchiveMember>> read_archive(std::span<const u8> data);

}  // namespace psvrecore::elf
