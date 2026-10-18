#include "psvrecore/formats/elf.hpp"

#include "psvrecore/text.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::elf {

namespace {

// ELF32 header fields (byte offsets).
enum HeaderField : usize {
    kIdentClass = 4, kIdentData = 5, kType = 16, kMachine = 18, kEntry = 24, kPhOff = 28, kShOff = 32, kFlags = 36,
    kPhEntSize = 42, kPhNum = 44, kShEntSize = 46, kShNum = 48, kShStrNdx = 50, kHeaderSize = 52,
};
constexpr u8 kClass32 = 1, kLittleEndian = 1;
constexpr usize kProgramHeaderSize = 32, kSectionHeaderSize = 40, kSymbolSize = 16;
constexpr usize kArchiveMagicSize = 8, kArchiveHeaderSize = 60;

}  // namespace

Result<File> File::parse(std::span<const u8> bytes) {
    File f;
    f.data_ = io::ByteReader(bytes);
    const io::ByteReader& d = f.data_;
    if (d.size() < kHeaderSize || !d.matches(0, "\x7f" "ELF")) return failure("not an ELF file");
    if (d.byte(kIdentClass) != kClass32 || d.byte(kIdentData) != kLittleEndian)
        return failure("only little-endian ELF32 is supported");
    f.type_ = d.le<u16>(kType);
    f.machine_ = d.le<u16>(kMachine);
    f.entry_ = d.le<u32>(kEntry);
    f.flags_ = d.le<u32>(kFlags);
    const u32 phoff = d.le<u32>(kPhOff), shoff = d.le<u32>(kShOff);
    const u16 phentsize = d.le<u16>(kPhEntSize), phnum = d.le<u16>(kPhNum);
    const u16 shentsize = d.le<u16>(kShEntSize), shnum = d.le<u16>(kShNum), shstrndx = d.le<u16>(kShStrNdx);
    if (phnum && phentsize < kProgramHeaderSize) return failure("ELF program header entries too small");
    if (shnum && shentsize < kSectionHeaderSize) return failure("ELF section header entries too small");

    f.segments_.reserve(phnum);
    for (usize i = 0; i < phnum; ++i) {
        const usize at = phoff + i * phentsize;
        f.segments_.push_back(Segment{d.le<u32>(at), d.le<u32>(at + 4), d.le<u32>(at + 8), d.le<u32>(at + 12),
                                      d.le<u32>(at + 16), d.le<u32>(at + 20), d.le<u32>(at + 24), d.le<u32>(at + 28)});
    }
    nx::vector<u32> name_offsets;
    f.sections_.reserve(shnum);
    for (usize i = 0; i < shnum; ++i) {
        const usize at = shoff + i * shentsize;
        name_offsets.push_back(d.le<u32>(at));
        f.sections_.push_back(Section{{}, d.le<u32>(at + 4), d.le<u32>(at + 8), d.le<u32>(at + 12), d.le<u32>(at + 16),
                                      d.le<u32>(at + 20), d.le<u32>(at + 24), d.le<u32>(at + 28), d.le<u32>(at + 36)});
    }
    if (d.failed()) return failure("ELF headers out of bounds");
    if (shstrndx < f.sections_.size()) {
        const io::ByteReader names(f.contents(f.sections_[shstrndx]));
        for (usize i = 0; i < f.sections_.size(); ++i) f.sections_[i].name = names.cstr(name_offsets[i]);
    }
    return f;
}

const Section* File::find_section(nx::string_view name) const {
    for (const auto& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}

std::span<const u8> File::contents(const Section& section) const {
    if (section.type == kSectionNobits) return {};
    return io::ByteReader(data_.data()).slice(section.offset, section.size);
}

std::span<const u8> File::contents(const Segment& segment) const {
    return io::ByteReader(data_.data()).slice(segment.offset, segment.filesz);
}

nx::vector<Symbol> File::symbols() const {
    nx::vector<Symbol> out;
    for (const auto& s : sections_) {
        if (s.type != kSectionSymtab) continue;
        const io::ByteReader table(contents(s));
        const io::ByteReader names(s.link < sections_.size() ? contents(sections_[s.link]) : std::span<const u8>{});
        for (usize at = kSymbolSize; at + kSymbolSize <= table.size(); at += kSymbolSize)  // (0: the null symbol)
            out.push_back(Symbol{names.cstr(table.le<u32>(at)), table.le<u32>(at + 4), table.le<u32>(at + 8),
                                 table.byte(at + 12), table.le<u16>(at + 14)});
    }
    return out;
}

Result<nx::vector<ArchiveMember>> read_archive(std::span<const u8> bytes) {
    const io::ByteReader d(bytes);
    if (!d.matches(0, "!<arch>\n")) return failure("not an ar archive");
    nx::vector<ArchiveMember> out;
    io::ByteReader long_names;
    for (usize at = kArchiveMagicSize; at + kArchiveHeaderSize <= d.size();) {
        const auto field = [&](usize off, usize len) {  // (space-padded text)
            nx::string s = d.cstr(at + off, len);
            s.erase(s.view().find_last_not_of(' ') + 1);
            return s;
        };
        nx::string name = field(0, 16);
        const auto size = parse_u64(field(48, 10));
        if (!size) return failure(nx::format("archive member '{}' with no size", name));
        const auto body = d.slice(at + kArchiveHeaderSize, *size);
        if (body.size() != *size) return failure(nx::format("archive member '{}' out of bounds", name));

        if (name == "//") {
            long_names = io::ByteReader(body);
        } else if (name != "/" && name != "/SYM64/") {
            if (name.size() > 1 && name[0] == '/' && long_names.size()) {  // GNU long name reference
                const auto off = parse_u64(name.view().substr(1));
                if (!off || *off >= long_names.size())
                    return failure(nx::format("archive member with a bad long-name reference: '{}'", name));
                name = long_names.cstr(*off);
                name = name.substr(0, name.find('\n'));
            }
            if (!name.empty() && name.back() == '/') name.pop_back();
            out.push_back(ArchiveMember{std::move(name), body});
        }
        at += kArchiveHeaderSize + *size + (*size & 1);
    }
    return out;
}

}  // namespace psvrecore::elf
