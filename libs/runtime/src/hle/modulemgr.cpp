// Modules: loading and starting modules the game ships (SceLibKernel).

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6;
constexpr i32 kStartFailed = err::code(0x8002D000), kModuleNotFound = err::code(0x8002D01C);
constexpr i32 kStartNoResident = 1, kStartFailedResult = 2;

i32 sceKernelLoadStartModule(Ptr<const char> path, u32 args, u32 argp, u32 /*flags*/, Ptr<const void> /*opt*/, Ptr<i32> result) {
    const nx::string guest_path = str(path, 1024);
    Loader& loader = runtime().loader();
    if (const LoadedModule* m = loader.find_by_path(guest_path)) {
        nx::logw(logs::loader, "{} is already loaded: its UID is returned", guest_path);
        if (result && writable(result.address, 4)) *result = 0;
        return m->uid;
    }
    nx::string host_path;
    if (runtime().files().resolve(guest_path, &host_path, nullptr, true) < 0) return kModuleNotFound;
    Result<i32> uid = [&] {
        Kernel::Blocking blocking(kernel());
        return loader.load(host_path, guest_path);
    }();
    if (!uid) {
        nx::loge(logs::loader, "{}", uid.error().message);
        return kStartFailed;
    }
    loader.link();
    const i32 status = loader.start(*uid, args, argp);
    if (result && writable(result.address, 4)) *result = status;
    if (status == kStartFailedResult) return kStartFailed;
    if (status == kStartNoResident) nx::logi(logs::loader, "{} does not stay resident (left loaded)", guest_path);
    return *uid;
}

i32 sceKernelStopUnloadModule(i32 uid, u32, u32, u32, Ptr<const void>, Ptr<i32>) {
    // Modules stay where their code was generated for: unloading one is not supported.
    nx::logw(logs::loader, "sceKernelStopUnloadModule(0x{:08x}) not supported", static_cast<u32>(uid));
    return err::kUnsupported;
}

i32 sceKernelCallModuleExit() { return err::kOk; }  // (module_exit runs only in the development kits' mode)

const Function kLibKernel[] = {
    HLE_FUNCTION(0x2DCC4AFA, sceKernelLoadStartModule),
    HLE_FUNCTION(0x2415F8A4, sceKernelStopUnloadModule),
    HLE_FUNCTION(0x15E2A45D, sceKernelCallModuleExit),
};

}  // namespace

void register_modulemgr() { register_library({"SceLibKernel", kSceLibKernel, kLibKernel}); }

}  // namespace psvrecore::runtime::hle
