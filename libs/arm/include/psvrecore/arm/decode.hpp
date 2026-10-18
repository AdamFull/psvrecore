#pragma once

// Decoding ARM and Thumb instructions (ARMv7-A with VFPv3 and Advanced SIMD, as on the Cortex-A9).
//
// An encoding the architecture leaves undefined decodes to Op::Undefined. One it calls UNPREDICTABLE decodes to
// what it would be, flagged kUnpredictable, so a caller can refuse it.

#include "psvrecore/arm/instruction.hpp"

namespace psvrecore::arm {

// Thumb's IT state: the conditions of the instructions an IT instruction covers, the next one on top.
//
// For an IT block in valid code this is the architecture's ITSTATE. The unpredictable cases follow the reference
// disassembler, which matters when linear disassembly runs through data: an IT inside a block pushes its
// conditions on top of the block's remaining ones.
struct ItState {
    static constexpr u8 kCapacity = 16;
    u8 count = 0;
    u8 conds[kCapacity] = {};

    bool active() const { return count != 0; }
    Cond cond() const { return static_cast<Cond>(conds[count - 1]); }
    bool last() const { return count == 1; }  // the current instruction is the block's last
    void advance() {
        if (count) --count;
    }
    // Starts a block: IT <firstcond> with <mask> (firstcond 1111 is taken as AL, as the reference does).
    void start(u32 firstcond, u32 mask);
};

Instruction decode_arm(u32 word, u32 address);

// Whether a Thumb instruction starting with this halfword is 32 bits long.
inline bool thumb_is_32bit(u16 first) { return (first >> 11) >= 0x1D; }

// Decodes the Thumb instruction at `address` (hw2 is ignored for a 16-bit one) in IT state `it`, which advances
// past it (and starts a block after an IT instruction).
Instruction decode_thumb(u16 hw1, u16 hw2, u32 address, ItState& it);

// The VFP and Advanced SIMD encodings, shared by both instruction sets. `word` is in the ARM form (Advanced SIMD
// data-processing 1111001U..., element loads/stores 11110100...); decode_thumb converts its encodings first.
// Each returns false when the encoding is not in its space.
bool decode_vfp(u32 word, Instruction& out);
bool decode_simd_data(u32 word, Instruction& out);
bool decode_simd_memory(u32 word, Instruction& out);
// 8-, 16-, 32- and 64-bit transfers between core and extension registers, and extension register loads/stores.
bool decode_vfp_transfer(u32 word, Instruction& out);

}  // namespace psvrecore::arm
