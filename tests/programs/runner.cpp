// The program runner: boots a recompiled SDK-built program on the runtime, as a game is booted (its eboot.bin and
// sce_module/libc.suprx, libfios2.suprx in an application folder mounted as app0:, a writable folder as ux0:), and
// reports what it printed and its exit code.
//
//     program_<name> <application folder> <data folder> [timeout seconds]
//
// Built by tests/programs/CMakeLists.txt with the program's and libc's generated code.

#include <cstdio>
#include <cstdlib>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/threading/thread.h"

using namespace psvrecore;
using namespace psvrecore::runtime;

extern "C" {
extern const PsvModuleCode PSVRECORE_PROGRAM_MODULE;
extern const PsvModuleCode libc_module;
extern const PsvModuleCode libfios2_module;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <application folder> <data folder> [timeout seconds]\n", argv[0]);
        return 2;
    }
    // (PSVRECORE_LOG=debug|info|warn|error: how much the runtime logs, to stderr; warn by default)
    nx::log::Config log_config;
    log_config.level = nx::log::Level::Warn;
    if (auto level = nx::os::environment_variable("PSVRECORE_LOG")) {
        if (*level == "debug") log_config.level = nx::log::Level::Debug;
        else if (*level == "info") log_config.level = nx::log::Level::Info;
        else if (*level == "error") log_config.level = nx::log::Level::Error;
    }
    nx::log::init(log_config);
    const u64 timeout_s = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 120;

    Runtime::Config config;
    config.files.mounts.push_back({"app0", argv[1], false});
    config.files.mounts.push_back({"ux0", argv[2], true});
    Runtime rt(config);
    if (!rt.ok()) return 2;
    rt.loader().add_code(PSVRECORE_PROGRAM_MODULE);
    rt.loader().add_code(libc_module);
    rt.loader().add_code(libfios2_module);

    Loader::BootOptions boot;
    boot.main = "app0:eboot.bin";
    boot.modules = {"app0:sce_module/libc.suprx", "app0:sce_module/libfios2.suprx"};  // (what the system preloads)
    boot.args = {"app0:eboot.bin"};
    auto main_thread = rt.loader().boot(boot);
    if (!main_thread) {
        std::fprintf(stderr, "boot failed: %s\n", main_thread.error().message.c_str());
        return 2;
    }

    // Until the process exits (libc's exit, after main returns), or the time is up.
    const u64 give_up = nx::os::ticks_ns() + timeout_s * 1'000'000'000ull;
    while (!rt.exited() && nx::os::ticks_ns() < give_up) nx::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::fflush(stdout);
    if (!rt.exited()) {
        std::fprintf(stderr, "the program did not exit within %llu s\n", static_cast<unsigned long long>(timeout_s));
        std::fflush(stderr);
        std::_Exit(3);
    }
    const i32 code = rt.exit_code();
    std::fprintf(stderr, "exit code %d\n", code);
    std::fflush(stderr);
    // (guest threads still running are not waited for: the process ends here, as the console's does)
    std::_Exit(code == 0 ? 0 : 1);
}
