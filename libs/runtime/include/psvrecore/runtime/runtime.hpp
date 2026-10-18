#pragma once

// The runtime of one guest process: its memory, kernel, files and modules. There is one per host process (the
// system library implementations reach it through runtime()); tests create and destroy them one after another.

#include "psvrecore/runtime/filesystem.hpp"
#include "psvrecore/runtime/kernel.hpp"
#include "psvrecore/runtime/loader.hpp"
#include "psvrecore/runtime/memory.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime {

// Where things go in the guest address space (module segments go where their code was generated for).
namespace layout {
inline constexpr Region kUserMemory{0x81000000u, 0xC0000000u};   // memory blocks (main memory)
inline constexpr Region kCdram{0x60000000u, 0x70000000u};        // memory blocks (CDRAM, the GPU's)
inline constexpr Region kStacks{0xE0000000u, 0xF0000000u};       // thread stacks and TLS
inline constexpr Region kSystem{0xF0000000u, 0xFFFF0000u};       // the runtime's own (exported variables)
}  // namespace layout

class Runtime {
public:
    struct Config {
        Kernel::Config kernel{};
        FileSystem::Config files{};
        u64 user_memory_budget = 0x10000000u;  // what memory blocks of main memory may total (256 MiB)
        u64 cdram_budget = 0x08000000u;        // and of CDRAM (128 MiB)
    };

    explicit Runtime(const Config& config);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool ok() const { return ok_; }
    const Config& config() const { return config_; }
    GuestMemory& memory() { return memory_; }
    Kernel& kernel() { return *kernel_; }
    FileSystem& files() { return *files_; }
    Loader& loader() { return *loader_; }

    // Guest memory for the runtime's own use (exported variables, strings), never freed.
    u32 allocate_system(u32 size, nx::string_view name);

    // The process's module parameter block (sceKernelGetProcessParam), set by the loader.
    u32 process_param = 0;
    // The calls a module made to sceKernelExitProcess end here: the exit code, once the process has ended.
    void exit_process(i32 code);
    bool exited() const;
    i32 exit_code() const;
    // Blocks until the process exits (a guest called sceKernelExitProcess or the main thread's libc did).
    i32 wait_for_exit();

private:
    Config config_;
    GuestMemory memory_;
    nx::unique_ptr<Kernel> kernel_;
    nx::unique_ptr<FileSystem> files_;
    nx::unique_ptr<Loader> loader_;
    bool ok_ = false;

    mutable nx::mutex exit_lock_;
    nx::cond_var exit_changed_;
    bool exited_ = false;
    i32 exit_code_ = 0;
};

// The current runtime (nullptr when none exists).
Runtime* current_runtime();
inline Runtime& runtime() { return *current_runtime(); }

}  // namespace psvrecore::runtime
