#include "psvrecore/analysis/image.hpp"

#include <cstring>

#include "psvrecore/formats/self.hpp"
#include "psvrecore/io/file.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::analysis {

namespace {

void translate_library(const Image& image, module::Library& lib) {
    lib.address = image.translate(lib.address);
    for (auto* list : {&lib.functions, &lib.variables, &lib.tls_variables})
        for (auto& s : *list) s.address = image.translate(s.address);
}

}  // namespace

Result<Image> Image::load(nx::fs::path_view path, std::span<const u32> bases) {
    auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    auto image = from_bytes(io::view(*data), bases);
    if (!image) return nx::err(Error{nx::format("{}: {}", path.native(), image.error().message)});
    return image;
}

Result<Image> Image::from_bytes(std::span<const u8> data, std::span<const u32> bases) {
    nx::vector<u8> elf_bytes;
    if (self::is_self(data)) {
        auto elf = self::to_elf(data);
        if (!elf) return nx::err(elf.error());
        elf_bytes = std::move(*elf);
    } else {
        elf_bytes.assign(data.begin(), data.end());
    }
    const auto file = elf::File::parse(elf_bytes);
    if (!file) return nx::err(file.error());
    auto parsed = module::parse(*file);
    if (!parsed) return nx::err(parsed.error());

    Image image;
    image.elf_type_ = file->type();
    // Every program header gets a base (the relocations name segments by program-header index); loadable ones are
    // moved where `bases` says.
    nx::vector<u32> all_bases;
    nx::vector<nx::vector<u8>> contents;
    usize loadable = 0;
    for (usize i = 0; i < file->segments().size(); ++i) {
        const auto& s = file->segments()[i];
        u32 base = s.vaddr;
        if (s.loadable()) {
            if (!bases.empty()) {
                if (loadable >= bases.size()) return failure(nx::format("no base given for loadable segment {}", i));
                base = bases[loadable];
            }
            ++loadable;
        }
        all_bases.push_back(base);
        const auto bytes = file->contents(s);
        nx::vector<u8> copy(s.loadable() ? s.memsz : s.filesz, 0);
        std::memcpy(copy.data(), bytes.data(), std::min<usize>(bytes.size(), copy.size()));
        contents.push_back(std::move(copy));
    }
    if (!bases.empty() && bases.size() != loadable)
        return failure(nx::format("{} bases given for {} loadable segments", bases.size(), loadable));
    if (!bases.empty() && file->type() == elf::kTypeSceExec)
        return failure("a fixed-address executable cannot be moved");

    auto relocs = reloc::decode_all(*file);
    if (!relocs) return nx::err(relocs.error());
    {
        nx::vector<reloc::Segment> segs;
        for (usize i = 0; i < contents.size(); ++i) segs.push_back(reloc::Segment{all_bases[i], std::span<u8>(contents[i])});
        for (const auto& e : *relocs) {
            if (auto applied = reloc::apply(e, segs); !applied) return nx::err(applied.error());
            const auto v = reloc::value(e, all_bases);
            if (!v) return nx::err(v.error());
            image.pointers_.push_back(Pointer{all_bases[e.patch_segment] + e.offset, *v, e.type});
        }
    }
    for (usize i = 0; i < file->segments().size(); ++i) {
        const auto& s = file->segments()[i];
        if (!s.loadable()) continue;
        Segment seg;
        seg.index = static_cast<int>(i);
        seg.base = all_bases[i];
        seg.linked = s.vaddr;
        seg.file_size = s.filesz;
        seg.flags = s.flags;
        seg.bytes = std::move(contents[i]);
        image.segments_.push_back(std::move(seg));
    }
    for (usize i = 0; i < image.segments_.size(); ++i)
        for (usize j = i + 1; j < image.segments_.size(); ++j) {
            const auto& a = image.segments_[i];
            const auto& b = image.segments_[j];
            if (a.base < b.base + b.size() && b.base < a.base + a.size())
                return failure(nx::format("segments {} and {} overlap", a.index, b.index));
        }

    // The module information, imports and exports, at their new addresses.
    image.module_ = std::move(*parsed);
    auto& info = image.module_.info;
    for (u32* a : {&info.address, &info.module_start, &info.module_stop, &info.exidx_start, &info.exidx_end,
                   &info.extab_start, &info.extab_end, &info.tls_start})
        if (*a) *a = image.translate(*a);
    for (auto& lib : image.module_.exports) translate_library(image, lib);
    for (auto& lib : image.module_.imports) translate_library(image, lib);
    return image;
}

Image Image::from_segments(nx::vector<Segment> segments) {
    Image image;
    image.segments_ = std::move(segments);
    for (auto& s : image.segments_) s.linked = s.base;
    return image;
}

Result<nx::vector<VariableUse>> Image::variable_uses(nx::vector<ImportedVariable>* variables) const {
    nx::vector<VariableUse> out;
    if (segments_.empty()) return out;
    const u32 base = segments_.front().base;
    u32 index = 0;
    for (const auto& lib : module_.imports)
        for (const auto& v : lib.variables) {
            if (variables) variables->push_back(ImportedVariable{lib.nid, v.nid, lib.name});
            const u32 table = v.address;
            const u32 header = u32_at(table);
            if (!contains(table, 4) || (header & 0x7F) != 0x40)
                return failure(nx::format("variable 0x{:08x} of {}: reference table at 0x{:08x} has header 0x{:08x}", v.nid, lib.name,
                                          table, header));
            const u32 entries = header >> 7;
            for (u32 i = 0; i < entries; ++i) {
                const u32 word = u32_at(table + 4 + 8 * i), offset = u32_at(table + 8 + 8 * i);
                const u8 type = static_cast<u8>(word >> 8);
                if ((word & 0xFF) != 1 || (type != reloc::kAbs32 && type != reloc::kThmMovwAbsNc && type != reloc::kThmMovtAbs &&
                                           type != reloc::kMovwAbsNc && type != reloc::kMovtAbs))
                    return failure(nx::format("variable 0x{:08x} of {}: reference entry 0x{:08x} not supported", v.nid, lib.name, word));
                out.push_back(VariableUse{index, base + offset, type, word >> 16});
            }
            ++index;
        }
    return out;
}

u32 Image::translate(u32 linked) const {
    // (a code address may carry the Thumb bit: translate the address of the byte it names)
    for (const auto& s : segments_)
        if (linked >= s.linked && linked - s.linked < s.size()) return linked - s.linked + s.base;
    return linked;
}

const Segment* Image::segment_of(u32 address) const {
    for (const auto& s : segments_)
        if (s.contains(address)) return &s;
    return nullptr;
}

bool Image::contains(u32 address, u32 size) const {
    const Segment* s = segment_of(address);
    return s && size <= s->size() - (address - s->base);
}

bool Image::executable(u32 address) const {
    const Segment* s = segment_of(address);
    return s && s->executable();
}

bool Image::writable(u32 address) const {
    const Segment* s = segment_of(address);
    return s && s->writable();
}

u8 Image::u8_at(u32 address) const {
    const Segment* s = segment_of(address);
    return s ? s->bytes[address - s->base] : u8{0};
}

u16 Image::u16_at(u32 address) const {
    if (!contains(address, 2)) return static_cast<u16>(u8_at(address) | (u8_at(address + 1) << 8));
    const Segment* s = segment_of(address);
    u16 v;
    std::memcpy(&v, s->bytes.data() + (address - s->base), 2);
    return v;
}

u32 Image::u32_at(u32 address) const {
    if (!contains(address, 4)) return u32{u16_at(address)} | (u32{u16_at(address + 2)} << 16);
    const Segment* s = segment_of(address);
    u32 v;
    std::memcpy(&v, s->bytes.data() + (address - s->base), 4);
    return v;
}

}  // namespace psvrecore::analysis
