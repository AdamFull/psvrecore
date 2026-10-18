#pragma once

// Instructions as text, in the syntax llvm-objdump uses (unified assembler syntax), so the decoder can be checked
// against it (scripts/arm_check.py).

#include "psvrecore/arm/instruction.hpp"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::arm {

nx::string format(const Instruction& in);

const char* register_name(u8 r);
const char* cond_name(Cond c);  // "" for AL and unconditional

}  // namespace psvrecore::arm
