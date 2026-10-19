#pragma once

// Shared by the decoders: field access (with the per-repetition register offsets of repeated instructions) and
// operand construction.

#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/instruction.hpp"

namespace psvrecore::usse::detail {

// A word being decoded. A repeated instruction is decoded once per repetition with some register fields offset: a
// field read through field() with exactly that position and width gets the offset added (and may then exceed its
// width, as the disassembler's register numbers do).
struct Word {
    u64 w = 0;
    struct Offset {
        u8 lo = 0, width = 0;
        i32 delta = 0;
    };
    Offset offsets[4];
    u8 offset_count = 0;

    explicit Word(u64 word) : w(word) {}

    u32 bit(u32 b) const { return static_cast<u32>((w >> b) & 1); }
    i32 field(u32 lo, u32 width) const {
        i32 v = static_cast<i32>((w >> lo) & ((u64{1} << width) - 1));
        for (u8 i = 0; i < offset_count; ++i)
            if (offsets[i].lo == lo && offsets[i].width == width) v += offsets[i].delta;
        return v;
    }
    void offset(u32 lo, u32 width, i32 delta) {
        for (u8 i = 0; i < offset_count; ++i)
            if (offsets[i].lo == lo && offsets[i].width == width) {
                offsets[i].delta = delta;
                return;
            }
        offsets[offset_count++] = Offset{static_cast<u8>(lo), static_cast<u8>(width), delta};
    }
};

// What a source field addresses: a register bank, an indexed register, the constant table or an immediate.
enum class Kind : u8 { Temp, Output, Primary, Secondary, Index1, Index2, Constant, Literal };

// Sources with two bank bits and an "extended" bit (the extended forms are indexed registers, the constant table and
// immediates).
inline Kind src_kind(i32 bank2, u32 ext) {
    static constexpr Kind plain[4] = {Kind::Temp, Kind::Output, Kind::Primary, Kind::Secondary};
    static constexpr Kind extended[4] = {Kind::Index1, Kind::Constant, Kind::Literal, Kind::Index2};
    return ext ? extended[bank2 & 3] : plain[bank2 & 3];
}

// The destination bank field common to most families (bits 51, 32, 33).
inline u32 dest_bank(const Word& w) { return w.bit(51) | (w.bit(32) << 1) | (w.bit(33) << 2); }

// Lanes of an operand: their count, the used ones and each one's source.
struct Lanes {
    u8 count = 0;
    u8 mask = 0;
    Comp comp[4] = {Comp::X, Comp::Y, Comp::Z, Comp::W};
};

inline Lanes lanes_identity(u8 count, u8 mask) {
    Lanes l;
    l.count = count;
    l.mask = mask;
    return l;
}
// Lanes from a swizzle given as text ("xyz1": x y z w 0 1 2 h).
Lanes lanes_text(const char* swizzle, u8 count, u8 mask);
Comp comp_of(char c);

// Builds an operand. num is the encoded field; for registers the printed number is num * scale (scale 1: 32-bit
// units, 2: the field counts pairs). wide: a 7-bit field (the internal registers are 124-127; else 60-63; in either
// case the 6-bit or 7-bit constant indices 64-127 are the special registers). idx_mul: an indexed operand's index
// counts pairs.
Operand operand(Kind kind, i32 num, const Lanes& lanes, View view, bool wide = false, i32 scale = 2,
                bool idx_mul = true);
// A register of a fixed bank (no extended forms).
Operand reg(Bank bank, i32 num, const Lanes& lanes, bool wide = false, i32 scale = 2);
// An index-register destination (printed index<n>).
Operand index_dest(i32 number);

// Whether a special-register code (64-127) has a name.
bool special_valid(i32 code);

// The fixed-point families' destination (sets the type: fx10 or fx8) and sources.
Operand fixed_dest(const Word& w, const Lanes& lanes, Type& type);
Operand fixed_src(const Word& w, Kind kind, u32 lo, const Lanes& lanes);

// The decoders: each appends its word's instruction(s); false for an encoding it does not know.
bool decode_alu(const Word& w, ProgramKind kind, nx::vector<Instruction>& out);
bool decode_dual(const Word& w, ProgramKind kind, nx::vector<Instruction>& out);
bool decode_other(const Word& w, ProgramKind kind, nx::vector<Instruction>& out);

}  // namespace psvrecore::usse::detail
