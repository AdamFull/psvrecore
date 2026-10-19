#pragma once

// USSE instructions as text, in the syntax of the reference disassembler (psp2shaderperf), so the decoder can be
// checked against it (scripts/usse_check.py).

#include <span>

#include "psvrecore/usse/decode.hpp"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::usse {

nx::string format_operand(const Operand& o, ProgramKind kind);

// One instruction (with its predicate). A branch prints `target_line`, the listing line of its target.
nx::string format(const Instruction& in, ProgramKind kind, i32 target_line = 0);

// A program's listing: one "N: text" line per printed instruction, numbered as the reference numbers them (a
// co-issued operation shares its main operation's number; words that do not print take none). Branch targets are
// marked "#N:".
nx::string format_listing(std::span<const Decoded> program, ProgramKind kind);

}  // namespace psvrecore::usse
