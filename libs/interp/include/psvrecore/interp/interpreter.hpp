#pragma once

// The reference interpreter: executes ARM and Thumb instructions one at a time on a PsvContext, following the ARM
// Architecture Reference Manual's pseudocode. It exists to check the code generator (differential tests run the
// same instructions through both) and is never linked into a game.
//
// It is written independently of the generator and of guest.h's helpers: only the decoder, the context layout and
// guest memory's base address are shared. Its own correctness is checked against hand-computed vectors.

#include "psvrecore/arm/decode.hpp"
#include "psvrecore/arm/instruction.hpp"
#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::interp {

enum class Result : u8 {
    Next,         // the instruction completed; pc is the next one
    Branched,     // the instruction wrote the PC (pc and thumb are the target's)
    Trap,         // it cannot execute (undefined, unpredictable, privileged, not implemented); see trap_kind
};

class Interpreter {
public:
    explicit Interpreter(PsvContext& ctx) : ctx_(ctx) {}

    // The address of the next instruction and its instruction set; the IT state of the instructions after an IT.
    u32 pc = 0;
    bool thumb = true;
    arm::ItState it;

    // Fetches the instruction at pc from guest memory, decodes it and executes it.
    Result step();
    // Executes an instruction decoded elsewhere (its address must be pc; its condition comes from the decoder,
    // which applied the IT state).
    Result execute(const arm::Instruction& in);

    u32 trap_kind = 0;  // PSV_TRAP_* when execute() returned Trap
    u32 address_mask = 0xFFFFFFFFu;  // guest addresses are ANDed with this (tests fold memory into a small buffer)

private:
    PsvContext& ctx_;
};

}  // namespace psvrecore::interp
