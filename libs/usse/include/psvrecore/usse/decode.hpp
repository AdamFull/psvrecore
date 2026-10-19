#pragma once

// Decoding USSE programs. A word decodes in the context of the words before it: the repeat increments one word sets
// apply to the repetitions of later ones (DecodeState carries them).

#include <span>

#include "psvrecore/usse/instruction.hpp"

namespace psvrecore::usse {

struct DecodeState {
    i8 increment[4] = {1, 1, 1, 1};  // per repetition, for the dest, src0, src1 and src2 registers
    u8 increment_swizzle = 0;        // bit 3 dest, 2 src0, 1 src1, 0 src2: the increment is four 2-bit offsets (one
                                     // per repetition) instead of a step
};

// One decoded instruction and where it came from.
struct Decoded {
    Instruction in;
    u32 word = 0;        // the index of its word in the program
    u8 repetition = 0;   // which repetition of a repeated word
    // The reference listing shows some families' words as written, without their repetitions or the registers the
    // repeat increments step: such a word decodes to a listed entry that does not execute, then to the executed
    // repetitions, which are not listed.
    bool listed = true;
    bool executed = true;
};

// Appends the instructions of the word at `index`: one, two (dual issue, colour and alpha), one per repetition, or
// one that does not print (prints() false). An encoding the decoder does not know appends an Op::Invalid
// instruction. Branch targets come out absolute (word indices).
void decode_word(u64 word, u32 index, ProgramKind kind, DecodeState& state, nx::vector<Decoded>& out);

nx::vector<Decoded> decode_program(std::span<const u64> words, ProgramKind kind);

}  // namespace psvrecore::usse
