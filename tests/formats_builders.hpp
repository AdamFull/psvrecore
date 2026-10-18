#pragma once

// Synthetic files for the format tests: a small Vita module ELF (module information, exports, both import entry
// sizes, relocations in both encodings), and helpers to write little-endian values at fixed offsets. Nothing here
// comes from SDK or game files.

#include <cstring>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace test {

struct Bytes {
    nx::vector<u8> d;

    explicit Bytes(usize size = 0) : d(size, 0) {}
    void grow(usize size) {
        if (d.size() < size) d.resize(size, 0);
    }
    void u8_(usize at, u8 v) {
        grow(at + 1);
        d[at] = v;
    }
    void u16_(usize at, u16 v) {
        grow(at + 2);
        std::memcpy(d.data() + at, &v, 2);
    }
    void u32_(usize at, u32 v) {
        grow(at + 4);
        std::memcpy(d.data() + at, &v, 4);
    }
    void u64_(usize at, u64 v) {
        grow(at + 8);
        std::memcpy(d.data() + at, &v, 8);
    }
    void str(usize at, nx::string_view s) {
        grow(at + s.size() + 1);
        std::memcpy(d.data() + at, s.data(), s.size());
        d[at + s.size()] = 0;
    }
    void bytes(usize at, const nx::vector<u8>& b) {
        grow(at + b.size());
        std::memcpy(d.data() + at, b.data(), b.size());
    }
    u32 read32(usize at) const {
        u32 v;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    }
};

// An ELF32 header and program headers at the start of `out`.
struct ProgramHeader {
    u32 type, offset, vaddr, filesz, memsz, flags;
};
inline void elf_header(Bytes& out, u16 type, u32 entry, const nx::vector<ProgramHeader>& phdrs) {
    out.str(0, "\x7f" "ELF");
    out.u8_(4, 1);  // ELFCLASS32
    out.u8_(5, 1);  // little-endian
    out.u8_(6, 1);  // version
    out.u16_(16, type);
    out.u16_(18, 40);  // ARM
    out.u32_(20, 1);
    out.u32_(24, entry);
    out.u32_(28, 52);  // e_phoff
    out.u32_(32, 0);   // e_shoff
    out.u32_(36, 0x05000000);
    out.u16_(40, 52);
    out.u16_(42, 32);
    out.u16_(44, static_cast<u16>(phdrs.size()));
    out.u16_(46, 40);
    for (usize i = 0; i < phdrs.size(); ++i) {
        const auto& p = phdrs[i];
        const usize at = 52 + i * 32;
        out.u32_(at, p.type);
        out.u32_(at + 4, p.offset);
        out.u32_(at + 8, p.vaddr);
        out.u32_(at + 12, 0);
        out.u32_(at + 16, p.filesz);
        out.u32_(at + 20, p.memsz);
        out.u32_(at + 24, p.flags);
        out.u32_(at + 28, 0x10);
    }
}

// The test module, linked for segment 0 at 0x81000000 and segment 1 at 0x81010000.
inline constexpr u32 kBase0 = 0x81000000, kBase1 = 0x81010000;
inline constexpr u32 kSeg0File = 0x100, kSeg0Size = 0x400, kSeg1File = 0x500, kSeg1Size = 0x40, kSeg1Mem = 0x80;
inline constexpr u32 kRelaFile = 0x540;

// Relocation entry words, from their fields (see reloc.cpp for the layout).
inline u32 long_word(u32 type, u32 symbol_segment, u32 patch_segment, u32 second_type = 0, u32 distance = 0) {
    return 0u | (symbol_segment << 4) | (type << 8) | (patch_segment << 16) | (second_type << 20) | (distance << 28);
}
inline void short_words(Bytes& r, usize at, u32 type, u32 symbol_segment, u32 patch_segment, u32 offset, u32 addend) {
    r.u32_(at, 1u | (symbol_segment << 4) | (type << 8) | (patch_segment << 16) | ((offset & 0xFFF) << 20));
    r.u32_(at + 4, ((offset >> 12) & 0x3FF) | (addend << 10));
}

inline nx::vector<u8> relocations() {
    Bytes r;
    // Long entry with a second relocation 4 bytes (two halfwords) on: THM_MOVW_ABS_NC + THM_MOVT_ABS at
    // segment 0 + 0x10, value segment 1 + 0x24.
    r.u32_(0, long_word(47, 1, 0, 48, 2));
    r.u32_(4, 0x24);
    r.u32_(8, 0x10);
    // Short entry: ABS32 at segment 1 + 0x20, value segment 0 + 0x41.
    short_words(r, 12, 2, 0, 1, 0x20, 0x41);
    // Long entry with a negative addend: ABS32 at segment 0 + 0x18, value segment 1 - 0x10.
    r.u32_(20, long_word(2, 1, 0));
    r.u32_(24, 0xFFFFFFF0u);
    r.u32_(28, 0x18);
    return r.d;
}

inline nx::vector<u8> module_elf() {
    const auto rela = relocations();
    Bytes f;
    elf_header(f, 0xFE04, 0x100,  // module information at segment 0 + 0x100
               {{1, kSeg0File, kBase0, kSeg0Size, kSeg0Size, 5},
                {1, kSeg1File, kBase1, kSeg1Size, kSeg1Mem, 6},
                {0x60000000, kRelaFile, 0, static_cast<u32>(rela.size()), 0, 0}});
    const usize s0 = kSeg0File, s1 = kSeg1File;

    // Code with the relocations applied for the link addresses: movw r0, #0x0024 / movt r0, #0x8101 (segment 1 +
    // 0x24 = 0x81010024), and a data word holding segment 1 - 0x10.
    f.u32_(s0 + 0x10, 0x0024F240);
    f.u32_(s0 + 0x14, 0x1001F2C8);
    f.u32_(s0 + 0x18, kBase1 - 0x10);
    f.u32_(s1 + 0x20, kBase0 + 0x41);

    // Module information.
    const usize mi = s0 + 0x100;
    f.u16_(mi, 0);
    f.u16_(mi + 2, 0x0101);
    f.str(mi + 4, "TestModule");
    f.u8_(mi + 31, 6);
    f.u32_(mi + 36, 0x160);  // exports
    f.u32_(mi + 40, 0x1A0);
    f.u32_(mi + 44, 0x1A0);  // imports
    f.u32_(mi + 48, 0x1F8);
    f.u32_(mi + 52, 0x12345678);
    f.u32_(mi + 68, 0x11);         // module_start
    f.u32_(mi + 72, 0xFFFFFFFF);  // no module_stop
    f.u32_(mi + 76, 0x300);
    f.u32_(mi + 80, 0x310);
    f.u32_(mi + 84, 0x310);
    f.u32_(mi + 88, 0x320);

    // A library export: two functions, one variable.
    const usize ex = s0 + 0x160;
    f.u8_(ex, 0x20);
    f.u16_(ex + 2, 1);
    f.u16_(ex + 4, 1);
    f.u16_(ex + 6, 2);
    f.u16_(ex + 8, 1);
    f.u32_(ex + 16, 0xAABBCCDD);
    f.u32_(ex + 20, kBase0 + 0x200);
    f.u32_(ex + 24, kBase0 + 0x220);
    f.u32_(ex + 28, kBase0 + 0x240);
    // The module's own export: module_start and module_info.
    const usize mx = s0 + 0x180;
    f.u8_(mx, 0x20);
    f.u16_(mx + 4, 0x8000);
    f.u16_(mx + 6, 1);
    f.u16_(mx + 8, 1);
    f.u32_(mx + 24, kBase0 + 0x260);
    f.u32_(mx + 28, kBase0 + 0x270);

    // A long import entry (0x34 bytes): two functions, one variable.
    const usize im = s0 + 0x1A0;
    f.u8_(im, 0x34);
    f.u16_(im + 2, 1);
    f.u16_(im + 6, 2);
    f.u16_(im + 8, 1);
    f.u32_(im + 16, 0x11111111);
    f.u32_(im + 20, kBase0 + 0x210);
    f.u32_(im + 28, kBase0 + 0x280);
    f.u32_(im + 32, kBase0 + 0x290);
    f.u32_(im + 36, kBase0 + 0x2A0);
    f.u32_(im + 40, kBase0 + 0x2A4);
    // A short import entry (0x24 bytes): one function.
    const usize is = s0 + 0x1D4;
    f.u8_(is, 0x24);
    f.u16_(is + 2, 1);
    f.u16_(is + 6, 1);
    f.u32_(is + 12, 0x22222222);
    f.u32_(is + 16, kBase0 + 0x218);
    f.u32_(is + 20, kBase0 + 0x2B0);
    f.u32_(is + 24, kBase0 + 0x2B4);

    f.str(s0 + 0x200, "ExpLib");
    f.str(s0 + 0x210, "ImpLib");
    f.str(s0 + 0x218, "Short");
    f.u32_(s0 + 0x220, 0x01010101);
    f.u32_(s0 + 0x224, 0x02020202);
    f.u32_(s0 + 0x228, 0x03030303);
    f.u32_(s0 + 0x240, kBase0 + 0x21);
    f.u32_(s0 + 0x244, kBase0 + 0x31);
    f.u32_(s0 + 0x248, kBase1);
    f.u32_(s0 + 0x260, 0x935CD196);  // module_start
    f.u32_(s0 + 0x264, 0x6C2224BA);  // module_info
    f.u32_(s0 + 0x270, kBase0 + 0x11);
    f.u32_(s0 + 0x274, kBase0 + 0x100);
    f.u32_(s0 + 0x280, 0xF1F1F1F1);
    f.u32_(s0 + 0x284, 0xF2F2F2F2);
    f.u32_(s0 + 0x290, kBase0 + 0x380);
    f.u32_(s0 + 0x294, kBase0 + 0x390);
    f.u32_(s0 + 0x2A0, 0x5A5A5A5A);
    f.u32_(s0 + 0x2A4, kBase1 + 0x10);
    f.u32_(s0 + 0x2B0, 0xF3F3F3F3);
    f.u32_(s0 + 0x2B4, kBase0 + 0x3A0);

    f.grow(kSeg1File + kSeg1Size);
    f.bytes(kRelaFile, rela);
    return f.d;
}

}  // namespace test
