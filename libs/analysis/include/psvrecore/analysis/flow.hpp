#pragma once

// What an instruction does to the flow of control, for code discovery and the code generator.

#include "psvrecore/arm/instruction.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::analysis {

enum class FlowKind : u8 {
    Next,            // continues with the next instruction
    Branch,          // to a known address (b, cbz, cbnz)
    Call,            // bl, blx (immediate): returns to the next instruction
    IndirectBranch,  // writes the PC from a register or memory (bx, pop {pc}, ldr pc, mov pc, add pc, tbb, tbh)
    IndirectCall,    // blx (register)
    Trap,            // stops: undefined, udf, bkpt, and what user code cannot execute (exception returns)
};

struct Flow {
    FlowKind kind = FlowKind::Next;
    bool conditional = false;  // may also continue with the next instruction
    u32 target = 0;            // Branch, Call: code address (Thumb bit set for Thumb)
};

Flow flow_of(const arm::Instruction& in);

// Whether the instruction executes only when its condition holds (not AL, not unconditional).
inline bool is_conditional(const arm::Instruction& in) { return in.cond != arm::Cond::Al && in.cond != arm::Cond::None; }

// The value of the PC as an operand: the instruction's address plus 8 (ARM) or 4 (Thumb).
inline u32 pc_value(const arm::Instruction& in) { return in.address + (in.mode == arm::Mode::Arm ? 8 : 4); }

}  // namespace psvrecore::analysis
