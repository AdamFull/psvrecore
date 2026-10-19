// The system's services a game asks about that have nothing to do on the host:
// - the application's state (SceAppMgrUser): in front, nothing of the system's over it, no system events;
// - the power configuration (ScePower): taken, the host's power is its own;
// - the network libraries (SceNet, SceNetCtl, SceHttp, SceSsl): they start, as on a console that is offline; what
//   would connect is not implemented.

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAppMgrUser = 0xA6605D6F, kScePower = 0x1082DA7F, kSceNet = 0x6BF8B2A2, kSceNetCtl = 0xC6083AE1,
              kSceHttp = 0xE8F15CDE, kSceSsl = 0xCAF5F2E9;
constexpr i32 kAppMgrNullPointer = err::code(0x80802016), kAppMgrInvalid = err::code(0x8080201A),
              kAppMgrState = err::code(0x80802013);

// SceAppMgrAppState: system events waiting, application events waiting, the system's UI over the application.
struct AppState {
    u32 system_events, app_events;
    i32 system_ui_overlaid;
    u8 reserved[116];
};
static_assert(sizeof(AppState) == 128);

i32 _sceAppMgrGetAppState(Ptr<AppState> state, u32 size, u32 /*build_version*/) {
    if (!state) return kAppMgrNullPointer;
    if (size != sizeof(AppState) || !writable(state.address, sizeof(AppState))) return kAppMgrInvalid;
    std::memset(state.get(), 0, sizeof(AppState));
    return 0;
}

// (no system event is ever waiting: the state says so, and asking anyway finds none; assumed to answer "state")
i32 sceAppMgrReceiveSystemEvent(Ptr<void> event) {
    if (!event) return kAppMgrNullPointer;
    return kAppMgrState;
}

i32 scePowerSetConfigurationMode(i32 mode) {
    nx::logd(logs::hle, "power configuration mode 0x{:x} (the host's is its own)", static_cast<u32>(mode));
    return 0;
}

i32 started(const char* what) {
    nx::logi(logs::hle, "{}: started (offline: nothing connects)", what);
    return 0;
}
i32 sceNetInit(Ptr<const void>) { return started("SceNet"); }
i32 sceNetCtlInit() { return started("SceNetCtl"); }
i32 sceHttpInit(u32) { return started("SceHttp"); }
i32 sceSslInit(u32) { return started("SceSsl"); }

const Function kAppMgrUser[] = {
    HLE_FUNCTION(0x5E86319A, _sceAppMgrGetAppState),
    HLE_FUNCTION(0x10B5765F, sceAppMgrReceiveSystemEvent),
};
const Function kPower[] = {HLE_FUNCTION(0x3CE187B6, scePowerSetConfigurationMode)};
const Function kNet[] = {HLE_FUNCTION(0xEB03E265, sceNetInit)};
const Function kNetCtl[] = {HLE_FUNCTION(0x495CA1DB, sceNetCtlInit)};
const Function kHttp[] = {HLE_FUNCTION(0x214926D9, sceHttpInit)};
const Function kSsl[] = {HLE_FUNCTION(0x3C733316, sceSslInit)};

}  // namespace

void register_system() {
    register_library({"SceAppMgrUser", kSceAppMgrUser, kAppMgrUser});
    register_library({"ScePower", kScePower, kPower});
    register_library({"SceNet", kSceNet, kNet});
    register_library({"SceNetCtl", kSceNetCtl, kNetCtl});
    register_library({"SceHttp", kSceHttp, kHttp});
    register_library({"SceSsl", kSceSsl, kSsl});
}

}  // namespace psvrecore::runtime::hle
