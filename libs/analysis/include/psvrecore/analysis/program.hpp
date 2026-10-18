#pragma once

// Code discovery: the functions of a module, found by following its control flow from the places code is known to
// start, never by decoding bytes linearly.
//
// A function is everything reachable from its entry without passing another function's entry: a branch to another
// entry is a tail call, and running into one is a tail call too. Code two functions both reach (a shared tail) is
// in both. The code generator gives each function's frame its return address, so a function that runs on into
// another keeps returning where it should; splitting or merging functions therefore changes the generated code, but
// not what it does.
//
// A call to a function that never returns ends the flow: what follows it (often a literal pool, or the next
// function) is not decoded as its continuation.
//
// Seeds:
// - the module's entry points and exported functions;
// - every function start in the exception index table (.ARM.exidx);
// - call targets (BL and BLX) found in code already discovered;
// - extra entries given by the caller (from a game's configuration, or from the runtime's record of indirect
//   branch targets it did not know).
// Imported functions' stubs are not code to translate: calls to them become calls through the import table.

#include "psvrecore/analysis/image.hpp"
#include "psvrecore/arm/instruction.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::analysis {

// Code addresses carry the instruction set in bit 0 (1: Thumb), as on the processor.
inline u32 code_address(u32 address, arm::Mode mode) { return (address & ~1u) | (mode == arm::Mode::Thumb ? 1u : 0u); }
inline arm::Mode code_mode(u32 code) { return (code & 1) ? arm::Mode::Thumb : arm::Mode::Arm; }

// A table branch (tbb, tbh, ldr pc / add pc indexed by a register): where each index goes.
struct JumpTable {
    u32 site = 0;             // the branch instruction
    u32 table = 0;            // the first entry
    u8 index_reg = 0;
    bool bounded = false;     // the entry count comes from a bounds check on the index (else: inferred)
    nx::vector<u32> targets;  // code addresses, by index
};

struct Function {
    u32 entry = 0;  // code address (Thumb bit set for Thumb)
    arm::Mode mode = arm::Mode::Thumb;
    nx::vector<arm::Instruction> code;  // in address order
    nx::vector<u32> labels;             // addresses (without the Thumb bit) that something in the function jumps to
    nx::map<u32, JumpTable> tables;     // by site
    // Instructions after which the function runs on into another function (that function's code address, by the
    // address of the instruction).
    nx::map<u32, u32> runs_into;
    // Calls to functions that never return: nothing after them is this function's code.
    nx::vector<u32> noreturn_calls;
    bool returns = true;  // whether any path can return (false: every path ends in a trap or a no-return call)

    u32 start() const { return entry & ~1u; }
};

enum class Severity : u8 { Note, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Warning;
    u32 address = 0;   // where (an instruction, an entry)
    u32 function = 0;  // the function it was found in (0: none)
    nx::string message;
};

struct Options {
    nx::vector<u32> extra_entries;  // code addresses
    bool use_exidx = true;
    // Imported functions that never return (abort, exit, __stack_chk_fail, __cxa_throw...), by NID. Functions of
    // the module itself are found not to return from their code.
    nx::vector<u32> noreturn_imports;
};

struct Program {
    const Image* image = nullptr;
    nx::map<u32, Function> functions;   // by code address
    nx::map<u32, u32> import_stubs;     // stub address -> index in imports() order (library, function)
    nx::vector<Diagnostic> diagnostics;

    struct ImportRef {
        u32 library_nid = 0;
        u32 nid = 0;
        nx::string library;
    };
    nx::vector<ImportRef> imports;  // every imported function, in the module's order

    const Function* function_at(u32 code) const {
        auto it = functions.find(code);
        return it == functions.end() ? nullptr : &it->second;
    }
};

Program analyze(const Image& image, const Options& options = {});

// Whether a function of this name never returns: the C and C++ runtime's (abort, exit, longjmp, the exception
// throwers, the stack protector's failure handler) and the kernel's process and thread exits.
bool is_noreturn_name(nx::string_view name);

// Function starts listed by the exception index table (code addresses).
nx::vector<u32> exidx_entries(const Image& image);

}  // namespace psvrecore::analysis
