#pragma once

// A Vita module as its ELF describes it: the module information (name, version, NID, start/stop entry points, TLS,
// exception tables), the libraries it exports and the libraries it imports. Exported and imported functions and
// variables are identified by NIDs, 32-bit numbers that stand for names.
//
// Addresses are those of the file as linked: a relocatable module's contents are already resolved for the address
// its program headers give, so the structures can be read before the module is relocated anywhere else.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/formats/elf.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::module {

// NIDs of the entries in a module's own export (the one with kExportMain).
inline constexpr u32 kNidModuleStart = 0x935CD196;
inline constexpr u32 kNidModuleStop = 0x79F8E492;
inline constexpr u32 kNidModuleExit = 0x913482A9;
inline constexpr u32 kNidModuleInfo = 0x6C2224BA;
inline constexpr u32 kNidModuleProcParam = 0x70FBA1E7;
inline constexpr u32 kNidModuleSdkVersion = 0x936C8A78;

inline constexpr u16 kExportMain = 0x8000;  // export attribute: the module's own entry points, not a library

// Reads the loadable segments of an ELF by guest address. Bytes past a segment's file data and inside its memory
// size read as zero; anything else is out of bounds.
class ImageView {
public:
    explicit ImageView(const elf::File& file);

    bool contains(u32 address, u32 size) const;
    // Fails the view (failed()) when out of bounds, as io::ByteReader does.
    u32 u32_at(u32 address) const;
    u16 u16_at(u32 address) const;
    u8 u8_at(u32 address) const;
    nx::string cstr(u32 address, usize max = 256) const;
    bool failed() const { return failed_; }

    // The segment that holds an address, or -1.
    int segment_of(u32 address) const;

private:
    struct Part {
        u32 vaddr, memsz;
        std::span<const u8> bytes;
        int index;
    };
    const Part* find(u32 address, u32 size) const;

    nx::vector<Part> parts_;
    mutable bool failed_ = false;
};

struct Info {
    u32 address = 0;  // where the module information is (guest address)
    u16 attributes = 0;
    u16 version = 0;
    nx::string name;
    u8 type = 0;
    u32 gp = 0;
    u32 module_nid = 0;
    u32 tls_start = 0, tls_file_size = 0, tls_memory_size = 0;
    // Entry points and table ranges, as guest addresses (0 when absent).
    u32 module_start = 0, module_stop = 0;
    u32 exidx_start = 0, exidx_end = 0, extab_start = 0, extab_end = 0;
};

// One exported or imported function or variable: its NID and, for an export, the address of the function or
// variable; for an imported function, the address of the stub the loader resolves; for an imported variable, the
// address of its entry in the module's variable reference table.
struct Symbol {
    u32 nid = 0;
    u32 address = 0;
};

struct Library {
    nx::string name;  // may be empty (a module's own export has none)
    u32 nid = 0;
    u16 version = 0;
    u16 attributes = 0;
    u32 address = 0;  // where the export or import entry is
    nx::vector<Symbol> functions;
    nx::vector<Symbol> variables;
    nx::vector<Symbol> tls_variables;
};

struct Module {
    Info info;
    nx::vector<Library> exports;
    nx::vector<Library> imports;
};

// The module information's address: e_entry holds the index of the segment it is in (top two bits) and its offset
// in that segment.
Result<u32> info_address(const elf::File& file);

Result<Module> parse(const elf::File& file);

// The name of a NID in a module's own export (module_start, module_info, ...), or null.
const char* main_export_name(u32 nid);

}  // namespace psvrecore::module
