// Modules: loading and starting modules the game ships (SceLibKernel).

#include <cstring>

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

// SceKernelModuleInfo (0x1B8 bytes) of the module whose segments hold an address: its UID, attributes, version and
// name, entry points, exception tables, TLS image (libc sets up thread-local storage from it), path and segments.
// The module information's own fields are read where the module is loaded (offsets from its segment's base).
i32 sceKernelGetModuleInfoByAddr(u32 address, Ptr<u8> info) {
    constexpr u32 kSize = 0x1B8;
    if (!info || !writable(info.address, kSize)) return err::kIllegalAddress;
    const LoadedModule* m = runtime().loader().find_by_address(address);
    if (!m) return kModuleNotFound;
    u8* out = info.get();
    std::memset(out, 0, kSize);
    const auto put = [&](u32 offset, u32 value) { std::memcpy(out + offset, &value, 4); };
    const auto segments = m->image.segments();
    u32 base = segments.empty() ? 0 : segments.front().base;
    for (const auto& seg : segments)
        if (m->module_info && seg.contains(m->module_info)) base = seg.base;
    const auto field = [&](u32 offset) {
        return m->module_info && memory().valid(m->module_info + offset, 4) ? psv_ld32(m->module_info + offset) : 0u;
    };
    const auto range = [&](u32 offset) {
        const u32 v = field(offset);
        return v ? base + v : 0u;
    };
    put(0x00, kSize);
    put(0x04, static_cast<u32>(m->uid));
    if (m->module_info) {
        const u32 attributes_version = field(0);
        std::memcpy(out + 0x08, &attributes_version, 4);
        std::memcpy(out + 0x0C, memory().host(m->module_info + 4), 26);  // (the name, at most 26 characters)
    }
    put(0x2C, m->start), put(0x30, m->stop), put(0x34, m->exit);
    put(0x38, range(76)), put(0x3C, range(80)), put(0x40, range(84)), put(0x44, range(88));
    const u32 tls = field(56), tls_file = field(60), tls_memory = field(64);
    if (tls_file || tls_memory) {
        put(0x48, tls < 0x80000000u ? base + tls : tls);
        put(0x4C, tls_file), put(0x50, tls_memory);
    }
    std::memcpy(out + 0x54, m->path.data(), std::min<usize>(m->path.size(), 255));
    for (usize i = 0; i < segments.size() && i < 4; ++i) {
        const u32 at = 0x154 + static_cast<u32>(i) * 0x18;
        put(at, 0x18), put(at + 4, segments[i].flags & 7), put(at + 8, segments[i].base);
        put(at + 12, segments[i].size()), put(at + 16, segments[i].file_size);
    }
    put(0x1B4, m->started ? 6 : 1);  // (started, or loaded)
    return err::kOk;
}

const Function kLibKernel[] = {
    HLE_FUNCTION(0x2DCC4AFA, sceKernelLoadStartModule),
    HLE_FUNCTION(0x2415F8A4, sceKernelStopUnloadModule),
    HLE_FUNCTION(0x15E2A45D, sceKernelCallModuleExit),
    HLE_FUNCTION(0xD11A5103, sceKernelGetModuleInfoByAddr),
};

}  // namespace

void register_modulemgr() { register_library({"SceLibKernel", kSceLibKernel, kLibKernel}); }

}  // namespace psvrecore::runtime::hle
