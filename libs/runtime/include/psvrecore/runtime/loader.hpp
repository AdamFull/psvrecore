#pragma once

// The module loader: puts a module's segments into guest memory and links it, for modules whose code was generated
// ahead of time and linked into the program.
//
// Every module the program can load was given to psvrecomp, which recorded the module's fingerprint (SHA-256 of its
// ELF) and the address of each segment. Loading a file finds the generated code with the same fingerprint (a file
// with none is refused: its code was never translated), maps its segments where the code expects them, links its
// imports to the exports of loaded modules or to the runtime's system libraries, and patches the data that refers
// to imported variables. Imports nothing provides stay unresolved: calling one stops with a diagnostic, and an
// unresolved variable has address 0 (as a loosely linked import on the console).
//
// Process start-up (boot): the main module is loaded, then the modules the system loads for it (sce_module/libc
// and the like), each started with module_start, and the main thread runs the main module's start entry with the
// thread parameters from its process parameter block.

#include <span>

#include "psvrecore/analysis/image.hpp"
#include "psvrecore/error.hpp"
#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime {

class Runtime;

struct LoadedModule {
    i32 uid = 0;
    nx::string name;
    nx::string path;  // as the guest named it
    const PsvModuleCode* code = nullptr;
    analysis::Image image;
    u32 start = 0, stop = 0, exit = 0;  // entry points (code addresses), 0 if none
    u32 process_param = 0, module_info = 0;
    bool started = false;
};

class Loader {
public:
    explicit Loader(Runtime& runtime);

    // The generated code the program has. Registers its functions (for calls through registers).
    void add_code(const PsvModuleCode& code);

    // Loads a module: `host_path` is the file, `guest_path` what the guest calls it. Returns its UID.
    Result<i32> load(nx::string_view host_path, nx::string_view guest_path);
    // Resolves every loaded module's imports (again: a later module can provide what earlier ones import).
    void link();
    // Runs module_start(args, argp) on a new thread of default priority and the main stack size, and waits for it.
    // Returns module_start's result (or an error).
    i32 start(i32 uid, u32 args, u32 argp);

    struct BootOptions {
        nx::string main = "app0:eboot.bin";
        nx::vector<nx::string> modules;  // started before the main thread (guest paths)
        nx::vector<nx::string> args;     // the main thread's argument strings
    };
    // Loads and starts the process; returns the main thread's UID.
    Result<i32> boot(const BootOptions& options);

    const LoadedModule* find(i32 uid) const;
    const LoadedModule* find_by_address(u32 address) const;
    const LoadedModule* find_by_path(nx::string_view guest_path) const;

private:
    void link_module(LoadedModule& m);

    Runtime& runtime_;
    nx::vector<const PsvModuleCode*> code_;
    nx::vector<nx::unique_ptr<LoadedModule>> modules_;
    mutable nx::mutex lock_;
    i32 next_uid_ = 0;
};

Loader* current_loader();

}  // namespace psvrecore::runtime
