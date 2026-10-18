#include "psvrecore/formats/module.hpp"

#include <cstring>

#include "core/foundation/strings/format.h"

namespace psvrecore::module {

namespace {

constexpr usize kInfoSize = 0x5C;
constexpr usize kNameSize = 27;
constexpr u16 kExportSize = 0x20;
constexpr u16 kImportSize = 0x34, kImportShortSize = 0x24;
constexpr u32 kNoEntry = 0xFFFFFFFF;  // module_start / module_stop absent

// Reads a table of `count` 32-bit values at `address` (an empty table when the address is 0).
Result<nx::vector<u32>> read_table(const ImageView& image, u32 address, u32 count, nx::string_view what) {
    nx::vector<u32> out;
    if (count == 0) return out;
    if (address == 0 || !image.contains(address, count * 4)) return failure(nx::format("{} table out of bounds", what));
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) out.push_back(image.u32_at(address + 4 * i));
    return out;
}

Result<nx::vector<Symbol>> read_symbols(const ImageView& image, u32 nids, u32 entries, u32 count,
                                        nx::string_view what) {
    const auto nid_table = read_table(image, nids, count, what);
    if (!nid_table) return nx::err(nid_table.error());
    const auto entry_table = read_table(image, entries, count, what);
    if (!entry_table) return nx::err(entry_table.error());
    nx::vector<Symbol> out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) out.push_back(Symbol{(*nid_table)[i], (*entry_table)[i]});
    return out;
}

}  // namespace

ImageView::ImageView(const elf::File& file) {
    for (usize i = 0; i < file.segments().size(); ++i) {
        const auto& s = file.segments()[i];
        if (s.loadable()) parts_.push_back(Part{s.vaddr, s.memsz, file.contents(s), static_cast<int>(i)});
    }
}

const ImageView::Part* ImageView::find(u32 address, u32 size) const {
    for (const auto& p : parts_)
        if (address >= p.vaddr && address - p.vaddr <= p.memsz && size <= p.memsz - (address - p.vaddr)) return &p;
    return nullptr;
}

bool ImageView::contains(u32 address, u32 size) const { return find(address, size) != nullptr; }

int ImageView::segment_of(u32 address) const {
    const Part* p = find(address, 1);
    return p ? p->index : -1;
}

u8 ImageView::u8_at(u32 address) const {
    const Part* p = find(address, 1);
    if (!p) {
        failed_ = true;
        return 0;
    }
    const u32 off = address - p->vaddr;
    return off < p->bytes.size() ? p->bytes[off] : u8{0};
}

u16 ImageView::u16_at(u32 address) const { return static_cast<u16>(u8_at(address) | (u8_at(address + 1) << 8)); }

u32 ImageView::u32_at(u32 address) const {
    const Part* p = find(address, 4);
    if (!p) {
        failed_ = true;
        return 0;
    }
    const u32 off = address - p->vaddr;
    if (off + 4 <= p->bytes.size()) {
        u32 value;
        std::memcpy(&value, p->bytes.data() + off, 4);
        return value;
    }
    return u32{u16_at(address)} | (u32{u16_at(address + 2)} << 16);  // (straddles the end of the file data)
}

nx::string ImageView::cstr(u32 address, usize max) const {
    nx::string out;
    for (usize i = 0; i < max; ++i) {
        if (!contains(address + static_cast<u32>(i), 1)) break;
        const u8 c = u8_at(address + static_cast<u32>(i));
        if (c == 0) break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

Result<u32> info_address(const elf::File& file) {
    const u32 index = file.entry() >> 30, offset = file.entry() & 0x3FFFFFFF;
    if (index >= file.segments().size() || !file.segments()[index].loadable())
        return failure(nx::format("module information in segment {}, which is not a loadable segment", index));
    const auto& s = file.segments()[index];
    if (offset + kInfoSize > s.filesz)
        return failure(nx::format("module information at offset 0x{:x} is outside segment {}", offset, index));
    return s.vaddr + offset;
}

Result<Module> parse(const elf::File& file) {
    if (file.machine() != elf::kMachineArm) return failure("not an ARM ELF file");
    if (file.type() != elf::kTypeSceExec && file.type() != elf::kTypeSceRelExec)
        return failure(nx::format("ELF type 0x{:04x} is not a Vita executable or module", file.type()));
    const auto where = info_address(file);
    if (!where) return nx::err(where.error());

    const ImageView image(file);
    Module m;
    Info& info = m.info;
    const u32 a = *where;
    const u32 base = file.segments()[file.entry() >> 30].vaddr;  // the module information's offsets count from here
    info.address = a;
    info.attributes = image.u16_at(a);
    info.version = image.u16_at(a + 2);
    info.name = image.cstr(a + 4, kNameSize);
    info.type = image.u8_at(a + 31);
    info.gp = image.u32_at(a + 32);
    const u32 export_top = image.u32_at(a + 36), export_end = image.u32_at(a + 40);
    const u32 import_top = image.u32_at(a + 44), import_end = image.u32_at(a + 48);
    info.module_nid = image.u32_at(a + 52);
    info.tls_start = image.u32_at(a + 56);
    info.tls_file_size = image.u32_at(a + 60);
    info.tls_memory_size = image.u32_at(a + 64);
    const u32 start = image.u32_at(a + 68), stop = image.u32_at(a + 72);
    info.module_start = start == kNoEntry ? 0 : base + start;
    info.module_stop = stop == kNoEntry ? 0 : base + stop;
    const auto range = [&](u32 offset) { return offset ? base + offset : 0; };
    info.exidx_start = range(image.u32_at(a + 76));
    info.exidx_end = range(image.u32_at(a + 80));
    info.extab_start = range(image.u32_at(a + 84));
    info.extab_end = range(image.u32_at(a + 88));
    if (image.failed()) return failure("module information out of bounds");

    // Exports: fixed-size entries.
    for (u32 at = base + export_top; at < base + export_end;) {
        const u16 size = image.u8_at(at);
        if (size != kExportSize) return failure(nx::format("export entry at 0x{:08x} has size 0x{:x}", at, size));
        Library lib;
        lib.address = at;
        lib.version = image.u16_at(at + 2);
        lib.attributes = image.u16_at(at + 4);
        const u16 funcs = image.u16_at(at + 6), vars = image.u16_at(at + 8), tls = image.u16_at(at + 10);
        lib.nid = image.u32_at(at + 16);
        const u32 name = image.u32_at(at + 20), nids = image.u32_at(at + 24), entries = image.u32_at(at + 28);
        if (name) lib.name = image.cstr(name);
        // One table of NIDs and one of addresses, functions first, then variables, then TLS variables.
        auto all = read_symbols(image, nids, entries, u32{funcs} + vars + tls, "export");
        if (!all) return nx::err(Error{nx::format("export '{}': {}", lib.name, all.error().message)});
        lib.functions.assign(all->begin(), all->begin() + funcs);
        lib.variables.assign(all->begin() + funcs, all->begin() + funcs + vars);
        lib.tls_variables.assign(all->begin() + funcs + vars, all->end());
        m.exports.push_back(std::move(lib));
        at += size;
    }

    // Imports: two entry sizes, the shorter one without TLS variables.
    for (u32 at = base + import_top; at < base + import_end;) {
        const u16 size = image.u8_at(at);
        Library lib;
        lib.address = at;
        lib.version = image.u16_at(at + 2);
        lib.attributes = image.u16_at(at + 4);
        const u16 funcs = image.u16_at(at + 6), vars = image.u16_at(at + 8);
        u16 tls = 0;
        u32 name = 0, func_nids = 0, func_entries = 0, var_nids = 0, var_entries = 0, tls_nids = 0, tls_entries = 0;
        if (size == kImportSize) {
            tls = image.u16_at(at + 10);
            lib.nid = image.u32_at(at + 16);
            name = image.u32_at(at + 20);
            func_nids = image.u32_at(at + 28);
            func_entries = image.u32_at(at + 32);
            var_nids = image.u32_at(at + 36);
            var_entries = image.u32_at(at + 40);
            tls_nids = image.u32_at(at + 44);
            tls_entries = image.u32_at(at + 48);
        } else if (size == kImportShortSize) {
            lib.nid = image.u32_at(at + 12);
            name = image.u32_at(at + 16);
            func_nids = image.u32_at(at + 20);
            func_entries = image.u32_at(at + 24);
            var_nids = image.u32_at(at + 28);
            var_entries = image.u32_at(at + 32);
        } else {
            return failure(nx::format("import entry at 0x{:08x} has size 0x{:x}", at, size));
        }
        if (name) lib.name = image.cstr(name);
        const auto fail = [&](const Error& e) { return nx::err(Error{nx::format("import '{}': {}", lib.name, e.message)}); };
        auto f = read_symbols(image, func_nids, func_entries, funcs, "function");
        if (!f) return fail(f.error());
        auto v = read_symbols(image, var_nids, var_entries, vars, "variable");
        if (!v) return fail(v.error());
        auto t = read_symbols(image, tls_nids, tls_entries, tls, "TLS variable");
        if (!t) return fail(t.error());
        lib.functions = std::move(*f);
        lib.variables = std::move(*v);
        lib.tls_variables = std::move(*t);
        m.imports.push_back(std::move(lib));
        at += size;
    }
    if (image.failed()) return failure("export or import entries out of bounds");
    return m;
}

const char* main_export_name(u32 nid) {
    switch (nid) {
        case kNidModuleStart: return "module_start";
        case kNidModuleStop: return "module_stop";
        case kNidModuleExit: return "module_exit";
        case kNidModuleInfo: return "module_info";
        case kNidModuleProcParam: return "module_proc_param";
        case kNidModuleSdkVersion: return "module_sdk_version";
        default: return nullptr;
    }
}

}  // namespace psvrecore::module
