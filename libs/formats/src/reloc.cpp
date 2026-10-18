#include "psvrecore/formats/reloc.hpp"

#include <cstring>

#include "psvrecore/io/bytes.hpp"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::reloc {

namespace {

// The first word of every entry:
//   bits 0-3 format (0 long, 1 short), 4-7 symbol segment, 8-15 type, 16-19 patched segment
// long  (12 bytes): bits 20-27 second type, 28-31 distance to the second relocation in halfwords;
//                   then the addend and the offset (32 bits each)
// short (8 bytes):  bits 20-31 the offset's low 12 bits; the second word's bits 0-9 its high 10 bits and
//                   bits 10-31 the addend
enum Format : u32 { kLong = 0, kShort = 1 };

// Thumb-2 MOVW/MOVT (T3/T1): imm16 is split over imm4 (first halfword bits 0-3), i (bit 10), imm3 (second halfword
// bits 12-14) and imm8 (bits 0-7).
void set_thumb_imm16(u16& first, u16& second, u16 imm) {
    first = static_cast<u16>((first & ~0x040Fu) | ((imm >> 12) & 0xF) | (((imm >> 11) & 1) << 10));
    second = static_cast<u16>((second & ~0x70FFu) | (((imm >> 8) & 7) << 12) | (imm & 0xFF));
}
// ARM MOVW/MOVT (A2/A1): imm16 is imm4 (bits 16-19) and imm12 (bits 0-11).
u32 set_arm_imm16(u32 insn, u16 imm) { return (insn & ~0x000F0FFFu) | ((u32{imm} & 0xF000) << 4) | (imm & 0xFFF); }

u32 load32(std::span<const u8> bytes, u32 at) {
    u32 v;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}
void store32(std::span<u8> bytes, u32 at, u32 v) { std::memcpy(bytes.data() + at, &v, 4); }

Result<const Segment*> target(const Entry& e, std::span<const Segment> segments) {
    if (e.patch_segment >= segments.size())
        return failure(nx::format("relocation patches segment {}, which the module does not have", e.patch_segment));
    const Segment& s = segments[e.patch_segment];
    if (e.offset > s.bytes.size() || s.bytes.size() - e.offset < 4)
        return failure(nx::format("relocation at segment {} + 0x{:x} is outside its data", e.patch_segment, e.offset));
    return &s;
}

}  // namespace

Result<nx::vector<Entry>> decode(std::span<const u8> data) {
    const io::ByteReader d(data);
    nx::vector<Entry> out;
    for (usize at = 0; at < d.size();) {
        const u32 w = d.le<u32>(at);
        Entry e;
        e.symbol_segment = static_cast<u8>((w >> 4) & 0xF);
        e.type = static_cast<u8>((w >> 8) & 0xFF);
        e.patch_segment = static_cast<u8>((w >> 16) & 0xF);
        switch (w & 0xF) {
            case kLong: {
                if (!d.contains(at, 12)) return failure(nx::format("truncated relocation entry at 0x{:x}", at));
                e.addend = d.le<u32>(at + 4);
                e.offset = d.le<u32>(at + 8);
                out.push_back(e);
                const u8 second = static_cast<u8>((w >> 20) & 0xFF);
                if (second != kNone) {
                    Entry e2 = e;
                    e2.type = second;
                    e2.offset = e.offset + 2 * (w >> 28);
                    out.push_back(e2);
                }
                at += 12;
                break;
            }
            case kShort: {
                if (!d.contains(at, 8)) return failure(nx::format("truncated relocation entry at 0x{:x}", at));
                const u32 w2 = d.le<u32>(at + 4);
                e.offset = ((w >> 20) & 0xFFF) | ((w2 & 0x3FF) << 12);
                e.addend = w2 >> 10;
                out.push_back(e);
                at += 8;
                break;
            }
            default:
                return failure(nx::format("relocation entry at 0x{:x} has unknown format {}", at, w & 0xF));
        }
    }
    return out;
}

Result<nx::vector<Entry>> decode_all(const elf::File& file) {
    nx::vector<Entry> out;
    for (const auto& s : file.segments()) {
        if (s.type != elf::kSegmentSceRela) continue;
        const auto data = file.contents(s);
        if (data.size() != s.filesz) return failure("relocation segment outside the file");
        auto entries = decode(data);
        if (!entries) return nx::err(entries.error());
        out.insert(out.end(), entries->begin(), entries->end());
    }
    return out;
}

const char* type_name(u8 type) {
    switch (type) {
        case kNone: return "NONE";
        case kAbs32: return "ABS32";
        case kRel32: return "REL32";
        case kTarget1: return "TARGET1";
        case kV4bx: return "V4BX";
        case kPrel31: return "PREL31";
        case kMovwAbsNc: return "MOVW_ABS_NC";
        case kMovtAbs: return "MOVT_ABS";
        case kThmMovwAbsNc: return "THM_MOVW_ABS_NC";
        case kThmMovtAbs: return "THM_MOVT_ABS";
        default: return "?";
    }
}

Result<u32> value(const Entry& e, std::span<const u32> bases) {
    if (e.symbol_segment >= bases.size())
        return failure(nx::format("relocation refers to segment {}, which the module does not have", e.symbol_segment));
    const u32 s = bases[e.symbol_segment] + e.addend;  // S + A
    switch (e.type) {
        case kAbs32:
        case kTarget1:
        case kMovwAbsNc:
        case kMovtAbs:
        case kThmMovwAbsNc:
        case kThmMovtAbs: return s;
        case kRel32:
        case kPrel31: {
            if (e.patch_segment >= bases.size())
                return failure(nx::format("relocation patches segment {}, which the module does not have", e.patch_segment));
            return s - (bases[e.patch_segment] + e.offset);  // S + A - P
        }
        default: return failure(nx::format("relocation type {} ({}) is not supported", e.type, type_name(e.type)));
    }
}

namespace {

// The bytes `apply` would leave at the relocation's place, from the bytes there now.
Result<u32> patched_word(const Entry& e, u32 v, u32 current) {
    switch (e.type) {
        case kAbs32:
        case kTarget1:
        case kRel32: return v;
        case kPrel31: return (current & 0x80000000u) | (v & 0x7FFFFFFFu);
        case kMovwAbsNc: return set_arm_imm16(current, static_cast<u16>(v));
        case kMovtAbs: return set_arm_imm16(current, static_cast<u16>(v >> 16));
        case kThmMovwAbsNc:
        case kThmMovtAbs: {
            u16 first = static_cast<u16>(current), second = static_cast<u16>(current >> 16);
            set_thumb_imm16(first, second, static_cast<u16>(e.type == kThmMovwAbsNc ? v : v >> 16));
            return u32{first} | (u32{second} << 16);
        }
        default: return failure(nx::format("relocation type {} ({}) is not supported", e.type, type_name(e.type)));
    }
}

Result<u32> evaluate(const Entry& e, std::span<const Segment> segments) {
    nx::small_vector<u32, 8> bases;
    for (const auto& s : segments) bases.push_back(s.base);
    return value(e, std::span<const u32>(bases.data(), bases.size()));
}

}  // namespace

Result<void> apply(const Entry& e, std::span<const Segment> segments) {
    if (e.type == kNone || e.type == kV4bx) return {};
    const auto seg = target(e, segments);
    if (!seg) return nx::err(seg.error());
    const auto v = evaluate(e, segments);
    if (!v) return nx::err(v.error());
    const auto word = patched_word(e, *v, load32((*seg)->bytes, e.offset));
    if (!word) return nx::err(word.error());
    store32((*seg)->bytes, e.offset, *word);
    return {};
}

Result<bool> is_applied(const Entry& e, std::span<const Segment> segments) {
    if (e.type == kNone || e.type == kV4bx) return true;
    const auto seg = target(e, segments);
    if (!seg) return nx::err(seg.error());
    const auto v = evaluate(e, segments);
    if (!v) return nx::err(v.error());
    const u32 current = load32((*seg)->bytes, e.offset);
    const auto word = patched_word(e, *v, current);
    if (!word) return nx::err(word.error());
    return *word == current;
}

}  // namespace psvrecore::reloc
