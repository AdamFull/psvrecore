#pragma once

// The code generator: a module's functions, as code discovery found them, translated to C that includes guest.h.
//
// Output, for a module named N:
// - N_functions.h: the prototype of every function;
// - N_code_<k>.c: the functions, a few thousand instructions per file so they compile in parallel;
// - N_module.c: the module's PsvModuleCode (functions by address, imports and their stubs).
//
// An instruction the generator does not translate becomes a trap at that place, and is counted in the statistics;
// nothing is skipped silently.

#include "psvrecore/analysis/program.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::recomp {

struct Options {
    nx::string name;                    // C identifier prefix of the module's tables (libc, eboot...)
    bool comments = true;               // each instruction's disassembly as a comment
    u32 instructions_per_file = 20000;  // how much code each .c file gets
};

struct GeneratedFile {
    nx::string name;
    nx::string text;
};

struct Stats {
    usize functions = 0;
    usize instructions = 0;
    usize tail_calls = 0;
    usize import_calls = 0;
    usize indirect_calls = 0;
    usize indirect_jumps = 0;
    usize jump_tables = 0;
    usize literals_folded = 0;
    // Instructions translated to a trap, by mnemonic (undefined, unimplemented, privileged...).
    nx::map<nx::string, usize> traps;
};

struct Output {
    nx::vector<GeneratedFile> files;
    Stats stats;
};

Output generate(const analysis::Program& program, const Options& options);

// The C name of the function at a code address.
nx::string function_name(u32 code);

}  // namespace psvrecore::recomp
