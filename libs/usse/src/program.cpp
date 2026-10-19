// Words to instructions: repetitions, the repeat increments, branch targets.

#include "common.hpp"

namespace psvrecore::usse {
namespace {

using detail::Word;

enum Role : u8 { kDest, kSrc0, kSrc1, kSrc2 };

struct RepeatField {
    u8 lo, width;
    Role role;
    // The register field is not stepped when the operand is an immediate (an "extended" bank-2 source).
    i8 hex_bank_lo = -1, hex_ext_bit = -1;
};

struct RepeatSpec {
    u8 lo = 0, width = 0;  // the repeat count field (count - 1)
    RepeatField fields[4];
    u8 field_count = 0;
};

bool is_hex(const Word& w, const RepeatField& f) {
    return f.hex_bank_lo >= 0 && w.bit(static_cast<u32>(f.hex_ext_bit)) && w.field(static_cast<u32>(f.hex_bank_lo), 2) == 2;
}

// The repeat count field and the stepped registers of each family that repeats through the increment state.
bool repeat_spec(const Word& w, u32 family, RepeatSpec& spec) {
    auto add = [&](u8 lo, u8 width, Role role, i8 bank_lo = -1, i8 ext = -1) {
        spec.fields[spec.field_count++] = RepeatField{lo, width, role, bank_lo, ext};
    };
    switch (family) {
        case 6:
            spec.lo = 44, spec.width = 4;
            add(21, 7, kDest), add(7, 7, kSrc1, 30, 49);
            return true;
        case 7:
            spec.lo = 44, spec.width = 2;
            add(18, 6, kDest), add(12, 6, kSrc0), add(6, 6, kSrc1, 30, 49), add(0, 6, kSrc2, 28, 48);
            return true;
        case 8: {
            spec.lo = 44, spec.width = 4;
            add(21, 7, kDest);
            const i32 st = w.field(41, 3);
            if (st == 6) add(8, 6, kSrc1, 30, 49), add(1, 6, kSrc1, 28, 48);  // f32: both halves
            else if (st == 5 || st == 7) add(8, 6, kSrc1, 30, 49);              // f16, fx10
            else add(7, 7, kSrc1, 30, 49);
            return true;
        }
        case 10:
        case 11:
        case 12:
        case 13:
            spec.lo = 44, spec.width = 4;
            add(21, 7, kDest), add(7, 7, kSrc1, 30, 49), add(0, 7, kSrc2, 28, 48);
            return true;
        case 16:
            spec.lo = 44, spec.width = 3;
            add(21, 7, kDest), add(7, 7, kSrc1), add(0, 7, kSrc2);
            return true;
        case 20:
        case 21:
            spec.lo = 44, spec.width = 3;
            add(21, 7, kDest), add(14, 7, kSrc0), add(7, 7, kSrc1, 30, 49), add(0, 7, kSrc2, 28, 48);
            return true;
        default: return false;
    }
}

bool decode_one(const Word& w, ProgramKind kind, nx::vector<Instruction>& out) {
    switch (static_cast<u32>(w.w >> 59)) {
        case 4:
        case 5: return detail::decode_dual(w, kind, out);
        case 16:
        case 17:
        case 31: return detail::decode_other(w, kind, out);
        default: return detail::decode_alu(w, kind, out);
    }
}

bool operands_valid(const Instruction& in) {
    auto ok = [](const Operand& o) { return o.bank != Bank::Special || detail::special_valid(o.number); };
    if (!ok(in.dest)) return false;
    for (u8 i = 0; i < in.src_count; ++i)
        if (!ok(in.src[i])) return false;
    return true;
}

// Decodes one repetition, appending its instructions (or an invalid one).
void emit(const Word& w, u64 raw, u32 index, u8 repetition, ProgramKind kind, nx::vector<Decoded>& out) {
    nx::vector<Instruction> ins;
    bool ok = decode_one(w, kind, ins);
    for (const Instruction& in : ins) ok = ok && operands_valid(in);
    if (!ok) {
        Instruction bad;
        bad.raw = raw;
        bad.family = static_cast<u8>(raw >> 59);
        out.push_back(Decoded{bad, index, repetition});
        return;
    }
    for (Instruction& in : ins) {
        in.raw = raw;
        in.family = static_cast<u8>(raw >> 59);
        if (in.op == Op::Br) in.target += static_cast<i32>(index);
        out.push_back(Decoded{in, index, repetition});
    }
}

// Family 3 repeats ignore the increment state: bits 47-48 choose what steps (source 0 by a vec4 of two register
// pairs, by one pair or not at all; and/or the internal-register sources by one). A dot product writes the next
// destination lane; a multiply-add's destination, as the reference prints it, is two registers on (an internal
// register: one).
void repeat_fam3(u64 word, u32 index, ProgramKind kind, nx::vector<Decoded>& out) {
    const Word base_word(word);
    const i32 count = base_word.field(44, 2) + 1;
    const u32 mode = base_word.bit(47) | (base_word.bit(48) << 1);
    static constexpr i32 s0_steps[4] = {2, 0, 2, 1};
    const i32 s0_step = s0_steps[mode];
    const bool gpi_step = mode == 1 || mode == 2;
    for (i32 k = 0; k < count; ++k) {
        Word w(word & ~(u64{3} << 44));
        if (base_word.bit(53)) {
            const bool gpi = (base_word.bit(51) | base_word.bit(32) | base_word.bit(33)) == 0 && base_word.field(22, 6) >= 60;
            w.offset(22, 6, gpi ? 1 : 2);
            if (gpi_step && k) w.offset(28, 2, k), w.offset(12, 2, k);
        } else {
            w.offset(39, 4, (1 << k) - base_word.field(39, 4));
            if (gpi_step && k) w.offset(28, 2, k);
        }
        if (s0_step && k) w.offset(0, 6, s0_step * k);
        emit(w, word, index, static_cast<u8>(k), kind, out);
    }
}

}  // namespace

void decode_word(u64 word, u32 index, ProgramKind kind, DecodeState& state, nx::vector<Decoded>& out) {
    const u32 family = static_cast<u32>(word >> 59);
    const Word w(word);
    if (family == 3 && w.field(44, 2)) {
        repeat_fam3(word, index, kind, out);
        return;
    }
    RepeatSpec spec;
    if (!repeat_spec(w, family, spec)) {
        const usize first = out.size();
        emit(w, word, index, 0, kind, out);
        for (usize i = first; i < out.size(); ++i) {
            const Instruction& in = out[i].in;
            if (in.op == Op::SetIncrements) {
                for (int r = 0; r < 4; ++r) state.increment[r] = in.increment[r];
                state.increment_swizzle = in.increment_swizzle;
            }
        }
        return;
    }
    const i32 count = w.field(spec.lo, spec.width) + 1;
    const u64 base = word & ~(((u64{1} << spec.width) - 1) << spec.lo);
    for (i32 k = 0; k < count; ++k) {
        Word rw(base);
        for (u8 f = 0; f < spec.field_count; ++f) {
            const RepeatField& rf = spec.fields[f];
            if (is_hex(w, rf)) continue;
            const bool swizzle = (state.increment_swizzle >> (3 - rf.role)) & 1;
            const i32 step = swizzle ? (static_cast<u8>(state.increment[rf.role]) >> (2 * (k % 4))) & 3
                                     : k * state.increment[rf.role];
            if (step) rw.offset(rf.lo, rf.width, step);
        }
        emit(rw, word, index, static_cast<u8>(k), kind, out);
    }
}

nx::vector<Decoded> decode_program(std::span<const u64> words, ProgramKind kind) {
    nx::vector<Decoded> out;
    DecodeState state;
    for (usize i = 0; i < words.size(); ++i) decode_word(words[i], static_cast<u32>(i), kind, state, out);
    return out;
}

}  // namespace psvrecore::usse
