// Input (SceCtrl) and system modules (SceSysmodule). The controller reads as released buttons and centred sticks
// until the platform layer feeds it.

#include <chrono>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceCtrl = 0xD197E3C7, kSceSysmodule = 0x03FCF19D;
constexpr i32 kCtrlInvalidArg = err::code(0x80340001);

struct CtrlData {
    u64 timestamp;
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 reserved[16];
};
static_assert(sizeof(CtrlData) == 32);

u32 g_sampling_mode = 0;  // SCE_CTRL_MODE_DIGITAL

// (timestamps: microseconds of the process time, the epoch every guest-visible time shares)
u64 now_us() { return kernel().process_time(); }

i32 read(i32 port, Ptr<CtrlData> data, i32 count, bool negative) {
    if (port != 0 && port != 1) return kCtrlInvalidArg;
    if (!data || count < 1 || count > 64 || !writable(data.address, sizeof(CtrlData) * static_cast<u32>(count)))
        return kCtrlInvalidArg;
    for (i32 i = 0; i < count; ++i) {
        CtrlData& d = data.get()[i];
        std::memset(&d, 0, sizeof d);
        d.timestamp = now_us();
        d.buttons = negative ? 0xFFFFFFFFu : 0;
        d.lx = d.ly = d.rx = d.ry = 128;
    }
    return count;
}

i32 sceCtrlPeekBufferPositive(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, false); }
i32 sceCtrlPeekBufferNegative(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, true); }
i32 sceCtrlReadBufferPositive(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, false); }
i32 sceCtrlReadBufferNegative(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, true); }

i32 sceCtrlSetSamplingMode(u32 mode) {
    if (mode > 2) return kCtrlInvalidArg;
    const u32 old = g_sampling_mode;
    g_sampling_mode = mode;
    return static_cast<i32>(old);
}
i32 sceCtrlGetSamplingMode(Ptr<u32> mode) {
    if (!mode) return kCtrlInvalidArg;
    *mode = g_sampling_mode;
    return 0;
}

// System modules are part of the runtime: loading one succeeds, and every one is loaded.
i32 sceSysmoduleLoadModule(u32) { return 0; }
i32 sceSysmoduleUnloadModule(u32) { return 0; }
i32 sceSysmoduleIsLoaded(u32) { return 0; }

const Function kCtrl[] = {
    HLE_FUNCTION(0xA9C3CED6, sceCtrlPeekBufferPositive),
    HLE_FUNCTION(0x104ED1A7, sceCtrlPeekBufferNegative),
    HLE_FUNCTION(0x67E7AB83, sceCtrlReadBufferPositive),
    HLE_FUNCTION(0x15F96FB0, sceCtrlReadBufferNegative),
    HLE_FUNCTION(0xA497B150, sceCtrlSetSamplingMode),
    HLE_FUNCTION(0xEC752AAF, sceCtrlGetSamplingMode),
};
const Function kSysmodule[] = {
    HLE_FUNCTION(0x79A0160A, sceSysmoduleLoadModule),
    HLE_FUNCTION(0x31D87805, sceSysmoduleUnloadModule),
    HLE_FUNCTION(0x53099B7A, sceSysmoduleIsLoaded),
};

}  // namespace

void register_ctrl() {
    register_library({"SceCtrl", kSceCtrl, kCtrl});
    register_library({"SceSysmodule", kSceSysmodule, kSysmodule});
}

}  // namespace psvrecore::runtime::hle
