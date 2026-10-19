// psvrecore::formats on synthetic files: SFO, SELF -> ELF (stored and zlib segments), module information,
// exports and imports, relocations (both encodings, applying and checking), NID databases and stub archives.

#include <cstdio>
#include <cstring>

#include "formats_builders.hpp"
#include "nxtest.h"
#include "psvrecore/formats/elf.hpp"
#include "psvrecore/formats/module.hpp"
#include "psvrecore/formats/nids.hpp"
#include "psvrecore/formats/reloc.hpp"
#include "psvrecore/formats/self.hpp"
#include "psvrecore/formats/sfo.hpp"
#include "zlib-ng.h"

using namespace psvrecore;
using test::Bytes;

namespace {

std::span<const u8> view(const nx::vector<u8>& v) { return {v.data(), v.size()}; }

}  // namespace

// --- SFO ---

TEST_CASE("sfo: text, integer and raw-text entries") {
    Bytes f;
    f.u32_(0, sfo::kMagic);
    f.u32_(4, 0x0101);
    const u32 count = 3, keys = 20 + count * 16, data = keys + 32;
    f.u32_(8, keys);
    f.u32_(12, data);
    f.u32_(16, count);
    f.str(keys, "TITLE_ID");
    f.str(keys + 9, "PARENTAL_LEVEL");
    f.str(keys + 24, "RAW");
    // TITLE_ID: UTF-8, 10 bytes used of 16.
    f.u16_(20, 0);
    f.u16_(22, 0x0204);
    f.u32_(24, 10);
    f.u32_(28, 16);
    f.u32_(32, 0);
    f.str(data, "PCSB00000");
    // PARENTAL_LEVEL: integer.
    f.u16_(36, 9);
    f.u16_(38, 0x0404);
    f.u32_(40, 4);
    f.u32_(44, 4);
    f.u32_(48, 16);
    f.u32_(data + 16, 3);
    // RAW: UTF-8 without a terminator.
    f.u16_(52, 24);
    f.u16_(54, 0x0004);
    f.u32_(56, 3);
    f.u32_(60, 4);
    f.u32_(64, 20);
    f.u8_(data + 20, 'a');
    f.u8_(data + 21, 'b');
    f.u8_(data + 22, 'c');
    f.grow(data + 24);  // the last value's reserved bytes

    REQUIRE(sfo::is_sfo(view(f.d)));
    const auto parsed = sfo::parse(view(f.d));
    REQUIRE(parsed.has_value());
    CHECK(parsed->entries.size() == 3u);
    CHECK(parsed->text("TITLE_ID").value_or("") == "PCSB00000");
    CHECK(parsed->integer("PARENTAL_LEVEL").value_or(0) == 3u);
    CHECK(parsed->text("RAW").value_or("") == "abc");
    CHECK_FALSE(parsed->text("PARENTAL_LEVEL").has_value());
    CHECK_FALSE(parsed->find("MISSING"));
}

TEST_CASE("sfo: values outside the file are errors") {
    Bytes f;
    f.u32_(0, sfo::kMagic);
    f.u32_(8, 36);
    f.u32_(12, 48);
    f.u32_(16, 1);
    f.str(36, "KEY");
    f.u16_(22, 0x0204);
    f.u32_(24, 4);
    f.u32_(28, 64);  // more than the file has
    CHECK_FALSE(sfo::parse(view(f.d)).has_value());
    CHECK_FALSE(sfo::parse(view(nx::vector<u8>{1, 2, 3})).has_value());
}

// --- SELF ---

namespace {

// Wraps an ELF in a SELF: segment 0 stored, segment 1 zlib-compressed, the rest stored; or every segment marked
// encrypted.
nx::vector<u8> make_self(const nx::vector<u8>& elf, bool encrypted = false) {
    const usize phnum = elf[44] | (elf[45] << 8);
    Bytes s;
    s.u32_(0, self::kMagic);
    s.u32_(4, 3);
    s.u16_(8, 0xC0);
    s.u16_(10, 1);
    const usize app_info = 0x80, elf_at = 0xA0, phdr_at = elf_at + 52, seginfo_at = 0x200, data_at = 0x400;
    s.u64_(16, data_at);
    s.u64_(24, elf.size());
    s.u64_(56, app_info);
    s.u64_(64, elf_at);
    s.u64_(72, phdr_at);
    s.u64_(88, seginfo_at);
    s.u64_(app_info, 0x2F00000000000001ull);
    s.u32_(app_info + 12, 8);
    s.bytes(elf_at, nx::vector<u8>(elf.begin(), elf.begin() + 52));
    s.bytes(phdr_at, nx::vector<u8>(elf.begin() + 52, elf.begin() + 52 + phnum * 32));
    usize at = data_at;
    for (usize i = 0; i < phnum; ++i) {
        u32 off, filesz;
        std::memcpy(&off, elf.data() + 52 + i * 32 + 4, 4);
        std::memcpy(&filesz, elf.data() + 52 + i * 32 + 16, 4);
        nx::vector<u8> data(elf.begin() + off, elf.begin() + off + filesz);
        const bool compress = i == 1;
        if (compress) {
            nx::vector<u8> packed(zng_compressBound(data.size()));
            size_t packed_size = packed.size();
            REQUIRE(zng_compress(packed.data(), &packed_size, data.data(), data.size()) == Z_OK);
            packed.resize(packed_size);
            data = std::move(packed);
        }
        s.u64_(seginfo_at + i * 32, at);
        s.u64_(seginfo_at + i * 32 + 8, data.size());
        s.u64_(seginfo_at + i * 32 + 16, compress ? 2 : 1);
        s.u64_(seginfo_at + i * 32 + 24, encrypted ? 1 : 2);
        s.bytes(at, data);
        at += (data.size() + 15) & ~usize{15};
    }
    s.grow(at);
    return s.d;
}

}  // namespace

TEST_CASE("self: the ELF inside a SELF, with stored and compressed segments") {
    const auto elf = test::module_elf();
    const auto wrapped = make_self(elf);
    REQUIRE(self::is_self(view(wrapped)));
    const auto parsed = self::parse(view(wrapped));
    REQUIRE(parsed.has_value());
    CHECK(parsed->elf_type == 0xFE04);
    CHECK(parsed->app_info.self_type == 8u);
    REQUIRE(parsed->segments.size() == 3u);
    CHECK(parsed->segments[1].compressed());
    CHECK_FALSE(parsed->encrypted());

    const auto out = self::to_elf(view(wrapped));
    REQUIRE(out.has_value());
    // The original ELF, but for the section-header fields, which the output clears (it has no sections).
    auto expected = elf;
    expected[46] = expected[47] = 0;  // e_shentsize
    CHECK(out->size() == expected.size());
    CHECK(std::memcmp(out->data(), expected.data(), expected.size()) == 0);
}

TEST_CASE("self: encrypted segments are refused") {
    const auto wrapped = make_self(test::module_elf(), true);
    const auto parsed = self::parse(view(wrapped));
    REQUIRE(parsed.has_value());
    CHECK(parsed->encrypted());
    CHECK_FALSE(self::to_elf(view(wrapped)).has_value());
}

// --- module information, exports, imports ---

TEST_CASE("module: information, exports and both import entry sizes") {
    const auto bytes = test::module_elf();
    const auto file = elf::File::parse(view(bytes));
    REQUIRE(file.has_value());
    const auto m = module::parse(*file);
    REQUIRE(m.has_value());

    CHECK(m->info.name == "TestModule");
    CHECK(m->info.address == test::kBase0 + 0x100);
    CHECK(m->info.module_nid == 0x12345678u);
    CHECK(m->info.module_start == test::kBase0 + 0x11);
    CHECK(m->info.module_stop == 0u);
    CHECK(m->info.exidx_start == test::kBase0 + 0x300);
    CHECK(m->info.extab_end == test::kBase0 + 0x320);

    REQUIRE(m->exports.size() == 2u);
    const auto& lib = m->exports[0];
    CHECK(lib.name == "ExpLib");
    CHECK(lib.nid == 0xAABBCCDDu);
    REQUIRE(lib.functions.size() == 2u);
    REQUIRE(lib.variables.size() == 1u);
    CHECK(lib.functions[1].nid == 0x02020202u);
    CHECK(lib.functions[1].address == test::kBase0 + 0x31);
    CHECK(lib.variables[0].address == test::kBase1);
    const auto& own = m->exports[1];
    CHECK((own.attributes & module::kExportMain) != 0);
    REQUIRE(own.functions.size() == 1u);
    CHECK(std::strcmp(module::main_export_name(own.functions[0].nid), "module_start") == 0);
    CHECK(own.variables[0].address == m->info.address);

    REQUIRE(m->imports.size() == 2u);
    CHECK(m->imports[0].name == "ImpLib");
    CHECK(m->imports[0].nid == 0x11111111u);
    REQUIRE(m->imports[0].functions.size() == 2u);
    CHECK(m->imports[0].functions[1].nid == 0xF2F2F2F2u);
    CHECK(m->imports[0].functions[1].address == test::kBase0 + 0x390);
    REQUIRE(m->imports[0].variables.size() == 1u);
    CHECK(m->imports[0].variables[0].address == test::kBase1 + 0x10);
    CHECK(m->imports[1].name == "Short");
    CHECK(m->imports[1].nid == 0x22222222u);
    REQUIRE(m->imports[1].functions.size() == 1u);
    CHECK(m->imports[1].functions[0].address == test::kBase0 + 0x3A0);
}

TEST_CASE("module: the image view reads zeros past file data and fails outside segments") {
    const auto bytes = test::module_elf();
    const auto file = elf::File::parse(view(bytes));
    REQUIRE(file.has_value());
    const module::ImageView image(*file);
    CHECK(image.u32_at(test::kBase1 + 0x20) == test::kBase0 + 0x41);
    CHECK(image.u32_at(test::kBase1 + 0x60) == 0u);  // memory size 0x80, file data 0x40
    CHECK_FALSE(image.failed());
    CHECK(image.segment_of(test::kBase1 + 0x7F) == 1);
    CHECK(image.segment_of(test::kBase1 + 0x80) == -1);
    (void)image.u32_at(0x10);
    CHECK(image.failed());
}

TEST_CASE("module: an import entry of unknown size is an error") {
    auto bytes = test::module_elf();
    bytes[test::kSeg0File + 0x1A0] = 0x30;
    const auto file = elf::File::parse(view(bytes));
    REQUIRE(file.has_value());
    CHECK_FALSE(module::parse(*file).has_value());
}

// --- relocations ---

TEST_CASE("reloc: both entry encodings decode, the long one into a MOVW/MOVT pair") {
    const auto entries = reloc::decode(view(test::relocations()));
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 4u);
    const auto& e = *entries;
    CHECK(e[0].type == reloc::kThmMovwAbsNc);
    CHECK(e[1].type == reloc::kThmMovtAbs);
    CHECK(e[0].offset == 0x10u);
    CHECK(e[1].offset == 0x14u);
    CHECK(e[1].symbol_segment == 1);
    CHECK(e[1].addend == 0x24u);
    CHECK(e[2].type == reloc::kAbs32);
    CHECK(e[2].patch_segment == 1);
    CHECK(e[2].offset == 0x20u);
    CHECK(e[2].addend == 0x41u);
    CHECK(e[3].addend == 0xFFFFFFF0u);
}

TEST_CASE("reloc: a short entry's offset and addend span both words") {
    // The largest offset (22 bits) and addend (22 bits) a short entry holds.
    Bytes r;
    test::short_words(r, 0, reloc::kThmMovwAbsNc, 2, 1, 0x3FFFFF, 0x3FFFFF);
    test::short_words(r, 8, reloc::kAbs32, 0, 0, 0x2A5C3, 0x1F00D);
    const auto entries = reloc::decode(view(r.d));
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2u);
    CHECK((*entries)[0].type == reloc::kThmMovwAbsNc);
    CHECK((*entries)[0].symbol_segment == 2);
    CHECK((*entries)[0].patch_segment == 1);
    CHECK((*entries)[0].offset == 0x3FFFFFu);
    CHECK((*entries)[0].addend == 0x3FFFFFu);
    CHECK((*entries)[1].offset == 0x2A5C3u);
    CHECK((*entries)[1].addend == 0x1F00Du);
}

TEST_CASE("reloc: unknown entry formats and truncated entries are errors") {
    Bytes r;
    r.u32_(0, 0x00000202);  // format 2
    r.u32_(4, 0);
    CHECK_FALSE(reloc::decode(view(r.d)).has_value());
    Bytes t;
    t.u32_(0, test::long_word(47, 1, 0, 48, 2));  // a long entry cut short
    t.u32_(4, 0);
    CHECK_FALSE(reloc::decode(view(t.d)).has_value());
}

namespace {

// Applies one relocation to a 4-byte word at offset 0 of segment 0 (base 0, value = addend), returning the word.
u32 patch(u8 type, u32 word, u32 addend, u32 base = 0) {
    nx::vector<u8> bytes(4);
    std::memcpy(bytes.data(), &word, 4);
    const reloc::Segment segments[] = {{base, std::span<u8>(bytes)}};
    const reloc::Entry e{type, 0, 0, 0, addend};
    const auto applied = reloc::apply(e, segments);
    if (!applied) return 0xDEADBEEF;
    std::memcpy(&word, bytes.data(), 4);
    return word;
}

}  // namespace

TEST_CASE("reloc: MOVW/MOVT immediates match the assembler's encodings") {
    // Expected words are the encodings clang's integrated assembler gives for these instructions (Thumb-2: first
    // halfword in the low half).
    CHECK(patch(reloc::kThmMovwAbsNc, 0x0000F240, 0x00001234) == 0x2034F241u);  // movw r0, #0x1234
    CHECK(patch(reloc::kThmMovtAbs, 0x0000F2C0, 0x81050000) == 0x1005F2C8u);    // movt r0, #0x8105
    CHECK(patch(reloc::kThmMovwAbsNc, 0x0300F240, 0x0000FFFF) == 0x73FFF64Fu);  // movw r3, #0xffff
    CHECK(patch(reloc::kThmMovtAbs, 0x0900F2C0, 0xA5C30000) == 0x59C3F2CAu);    // movt r9, #0xa5c3
    CHECK(patch(reloc::kMovwAbsNc, 0xE3000000, 0x00001234) == 0xE3010234u);     // movw r0, #0x1234
    CHECK(patch(reloc::kMovtAbs, 0xE3400000, 0x81050000) == 0xE3480105u);       // movt r0, #0x8105
    CHECK(patch(reloc::kMovwAbsNc, 0xE3003000, 0x0000FFFF) == 0xE30F3FFFu);     // movw r3, #0xffff
}

TEST_CASE("reloc: data relocations") {
    CHECK(patch(reloc::kAbs32, 0, 0x10, 0x81000000) == 0x81000010u);
    CHECK(patch(reloc::kTarget1, 0, 0x10, 0x81000000) == 0x81000010u);
    // PC-relative: S + A - P, with P the patched word's own address (base + 0).
    CHECK(patch(reloc::kRel32, 0, 0x30, 0x81000000) == 0x30u);
    CHECK(patch(reloc::kRel32, 0, 0xFFFFFFF0u, 0x81000000) == 0xFFFFFFF0u);
    // PREL31 keeps bit 31 of the word.
    CHECK(patch(reloc::kPrel31, 0x80000000u, 0x7FFFFFFFu) == 0xFFFFFFFFu);
    CHECK(patch(reloc::kPrel31, 0, 0xFFFFFFFCu) == 0x7FFFFFFCu);
    // Unsupported types are errors.
    CHECK(patch(28 /* R_ARM_CALL */, 0, 0) == 0xDEADBEEFu);
}

TEST_CASE("reloc: a module as linked holds its relocations; moving it and back round-trips") {
    auto bytes = test::module_elf();
    const auto file = elf::File::parse(view(bytes));
    REQUIRE(file.has_value());
    const auto entries = reloc::decode_all(*file);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 4u);

    nx::vector<u8> seg0(bytes.begin() + test::kSeg0File, bytes.begin() + test::kSeg0File + test::kSeg0Size);
    nx::vector<u8> seg1(bytes.begin() + test::kSeg1File, bytes.begin() + test::kSeg1File + test::kSeg1Size);
    const nx::vector<u8> original0 = seg0, original1 = seg1;
    nx::vector<u8> unused;
    reloc::Segment linked[] = {{test::kBase0, seg0}, {test::kBase1, seg1}, {0, unused}};
    for (const auto& e : *entries) {
        const auto applied = reloc::is_applied(e, linked);
        REQUIRE(applied.has_value());
        CHECK(*applied);
    }

    reloc::Segment moved[] = {{0x82000000, seg0}, {0x83000000, seg1}, {0, unused}};
    for (const auto& e : *entries) REQUIRE(reloc::apply(e, moved).has_value());
    Bytes m0, m1;
    m0.d = seg0;
    m1.d = seg1;
    CHECK(m0.read32(0x10) == 0x0024F240u);  // movw r0, #0x0024 (0x83000024)
    CHECK(m0.read32(0x14) == 0x3000F2C8u);  // movt r0, #0x8300
    CHECK(m0.read32(0x18) == 0x82FFFFF0u);  // segment 1 - 0x10
    CHECK(m1.read32(0x20) == 0x82000041u);  // segment 0 + 0x41
    // Against the link addresses, the moved data word no longer matches; the MOVW's low half happens to.
    CHECK_FALSE(reloc::is_applied((*entries)[3], linked).value_or(true));
    CHECK(reloc::is_applied((*entries)[0], linked).value_or(false));

    for (const auto& e : *entries) REQUIRE(reloc::apply(e, linked).has_value());
    CHECK(std::memcmp(seg0.data(), original0.data(), seg0.size()) == 0);
    CHECK(std::memcmp(seg1.data(), original1.data(), seg1.size()) == 0);
}

TEST_CASE("reloc: a relocation outside its segment's data is an error") {
    nx::vector<u8> bytes(8);
    const reloc::Segment segments[] = {{0, std::span<u8>(bytes)}};
    CHECK_FALSE(reloc::apply(reloc::Entry{reloc::kAbs32, 0, 0, 6, 0}, segments).has_value());
    CHECK_FALSE(reloc::apply(reloc::Entry{reloc::kAbs32, 0, 3, 0, 0}, segments).has_value());
    CHECK_FALSE(reloc::apply(reloc::Entry{reloc::kAbs32, 5, 0, 0, 0}, segments).has_value());
}

// --- NID databases ---

TEST_CASE("nids: vita-headers YAML, with a library's NID after its functions") {
    const char* yaml =
        "version: 2\n"
        "firmware: 3.60\n"
        "modules:\n"
        "  SceTestModule:\n"
        "    nid: 0xF0000001\n"
        "    libraries:\n"
        "      SceTest:\n"
        "        functions:\n"
        "          sceTestOpen: 0x1234ABCD\n"
        "          sceTestClose: 0x0000BEEF\n"
        "        kernel: false\n"
        "        nid: 0xCAFE0001\n"
        "        variables:\n"
        "          sceTestValue: 0x55AA55AA\n"
        "      SceTestOther:\n"
        "        nid: 0xCAFE0002\n"
        "        functions:\n"
        "          sceTestOpen: 0x99999999\n";
    nids::Database db;
    REQUIRE(db.load_yaml(yaml).has_value());
    REQUIRE(db.library(0xCAFE0001));
    CHECK(*db.library(0xCAFE0001) == "SceTest");
    REQUIRE(db.find(0xCAFE0001, 0x1234ABCD));
    CHECK(*db.find(0xCAFE0001, 0x1234ABCD) == "sceTestOpen");
    CHECK(*db.find(0xCAFE0001, 0x55AA55AA) == "sceTestValue");
    CHECK(*db.find(0xCAFE0002, 0x99999999) == "sceTestOpen");
    CHECK_FALSE(db.find(0xCAFE0002, 0x1234ABCD));
    CHECK(db.size() == 4u);
    CHECK_FALSE(db.load_yaml("version: 3\n").has_value());
    CHECK_FALSE(db.load_yaml("modules:\n  M:\n    libraries:\n      L:\n        functions:\n          f: 0x1\n").has_value());
}

TEST_CASE("nids: tab-separated round trip") {
    nids::Database db;
    db.add_library(0xCAFE0001, "SceTest");
    db.add(0xCAFE0001, 0x1234ABCD, nids::Kind::Function, "sceTestOpen");
    db.add(0xCAFE0001, 0x55AA55AA, nids::Kind::Variable, "sceTestValue");
    nids::Database again;
    REQUIRE(again.load_tsv(db.to_tsv()).has_value());
    CHECK(again.to_tsv() == db.to_tsv());
    CHECK(*again.find(0xCAFE0001, 0x55AA55AA) == "sceTestValue");
    CHECK(again.entries()[1].kind == nids::Kind::Variable);
    CHECK_FALSE(again.load_tsv("X\t1\t2\n").has_value());
}

namespace {

// A relocatable ELF object with absolute symbols, as SDK stub library members have.
nx::vector<u8> stub_object(const nx::vector<std::pair<nx::string, u32>>& symbols) {
    Bytes strtab(1), symtab(16);
    for (const auto& [name, value] : symbols) {
        const usize at = symtab.d.size();
        symtab.u32_(at, static_cast<u32>(strtab.d.size()));
        symtab.u32_(at + 4, value);
        symtab.u8_(at + 12, 0x10);  // global, no type
        symtab.u16_(at + 14, elf::kSectionAbsolute);
        strtab.str(strtab.d.size(), name);
    }
    const nx::string shstr = nx::string("\0.symtab\0.strtab\0.shstrtab\0", 27);
    Bytes f;
    test::elf_header(f, elf::kTypeRel, 0, {});
    const usize sym_at = 64, str_at = sym_at + symtab.d.size(), shs_at = str_at + strtab.d.size();
    const usize sh_at = (shs_at + shstr.size() + 3) & ~usize{3};
    f.bytes(sym_at, symtab.d);
    f.bytes(str_at, strtab.d);
    f.bytes(shs_at, nx::vector<u8>(shstr.begin(), shstr.end()));
    f.u32_(32, static_cast<u32>(sh_at));
    f.u16_(48, 4);
    f.u16_(50, 3);
    const auto section = [&](usize i, u32 name, u32 type, usize off, usize size, u32 link) {
        const usize at = sh_at + i * 40;
        f.u32_(at, name);
        f.u32_(at + 4, type);
        f.u32_(at + 16, static_cast<u32>(off));
        f.u32_(at + 20, static_cast<u32>(size));
        f.u32_(at + 24, link);
        f.u32_(at + 36, type == elf::kSectionSymtab ? 16 : 0);
    };
    section(0, 0, 0, 0, 0, 0);
    section(1, 1, elf::kSectionSymtab, sym_at, symtab.d.size(), 2);
    section(2, 9, 3, str_at, strtab.d.size(), 0);
    section(3, 17, 3, shs_at, shstr.size(), 0);
    return f.d;
}

void ar_member(Bytes& ar, nx::string_view name, const nx::vector<u8>& body) {
    char header[61];
    std::snprintf(header, sizeof header, "%-16.16s%-12s%-6s%-6s%-8s%-10zu`\n", nx::string(name).c_str(), "0", "0", "0",
                  "644", body.size());
    const usize at = ar.d.size();
    ar.grow(at + 60);
    std::memcpy(ar.d.data() + at, header, 60);
    ar.bytes(at + 60, body);
    if (body.size() & 1) ar.u8_(ar.d.size(), '\n');
}

}  // namespace

TEST_CASE("nids: stub archives name their library, functions and variables") {
    Bytes ar;
    ar.str(0, "!<arch>\n");
    ar.d.resize(8);
    ar_member(ar, "_L_head.o/", stub_object({{"_INID_7_SceTest", 0xCAFE0001}}));
    ar_member(ar, "_L_F00_1.o/", stub_object({{"_INID_7_SceTest__11_sceTestOpen", 0x1234ABCD},
                                                    {"sceTestOpen", 0}}));
    ar_member(ar, "_L_V00_2.o/", stub_object({{"_INID_7_SceTest__12_sceTest__Val", 0x55AA55AA}}));
    const auto found = nids::stub_archive_nids(view(ar.d));
    REQUIRE(found.has_value());
    REQUIRE(found->libraries.size() == 1u);
    CHECK(found->libraries[0].name == "SceTest");
    CHECK(found->libraries[0].nid == 0xCAFE0001u);
    REQUIRE(found->names.size() == 2u);
    CHECK(found->names[0].name == "sceTestOpen");
    CHECK(found->names[0].library_nid == 0xCAFE0001u);
    CHECK(found->names[0].kind == nids::Kind::Function);
    CHECK(found->names[1].name == "sceTest__Val");  // a name with "__" in it
    CHECK(found->names[1].kind == nids::Kind::Variable);
}
