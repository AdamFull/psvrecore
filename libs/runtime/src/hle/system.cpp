// The system's services a game asks about that have nothing to do on the host:
// - the application's state (SceAppMgrUser): in front, nothing of the system's over it, no system events;
// - the power configuration (ScePower): taken, the host's power is its own;
// - the network libraries (SceNet, SceNetCtl, SceHttp, SceSsl): they start, as on a console that is offline; what
//   would connect is not implemented;
// - the development kit's debug LEDs (SceDebugLed): a retail console has none, so setting them does nothing;
// - the system's screenshots (SceScreenShot: the PS button and Start): the host has none of its own, so what a game
//   sets for them (titles, an overlay picture, screenshots allowed or not) is checked and kept nowhere.

#include "hle_common.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAppMgrUser = 0xA6605D6F, kScePower = 0x1082DA7F, kSceNet = 0x6BF8B2A2, kSceNetCtl = 0xC6083AE1,
              kSceHttp = 0xE8F15CDE, kSceSsl = 0xCAF5F2E9, kSceDebugLed = 0xAE004C0A,
              kSceScreenShot = 0xF26FC97D;
constexpr i32 kScreenShotInvalid = err::code(0x80102F01);
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

i32 sceKernelSetGPO(u32 /*leds*/) { return 0; }  // (games set them many times a frame: not logged)

// SceScreenShotParam: the photo's title, the game's title, a comment (UTF-8, any may be null), reserved (null).
struct ScreenShotParam {
    Ptr<const char> photo_title, game_title, game_comment;
    u32 reserved;
};
i32 sceScreenShotSetParam(Ptr<const ScreenShotParam> param) {
    if (!param || !memory().valid(param.address, sizeof(ScreenShotParam), false) || param->reserved != 0)
        return kScreenShotInvalid;
    nx::logd(logs::hle, "screenshots: photo title '{}', game title '{}' (the host has no system screenshots)",
             param->photo_title ? str(param->photo_title, 256) : nx::string(),
             param->game_title ? str(param->game_title, 256) : nx::string());
    return 0;
}
i32 sceScreenShotSetOverlayImage(Ptr<const char> path, i32 x, i32 y) {
    if (!path) return kScreenShotInvalid;
    nx::logd(logs::hle, "screenshots: overlay '{}' at {},{}", str(path, 1024), x, y);
    return 0;
}
i32 sceScreenShotDisable() {
    nx::logd(logs::hle, "screenshots: not allowed");
    return 0;
}
i32 sceScreenShotEnable() {
    nx::logd(logs::hle, "screenshots: allowed");
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
const Function kDebugLed[] = {HLE_FUNCTION(0x78E702D3, sceKernelSetGPO)};
const Function kScreenShot[] = {
    HLE_FUNCTION(0x05DB59C7, sceScreenShotSetParam),
    HLE_FUNCTION(0x7061665B, sceScreenShotSetOverlayImage),
    HLE_FUNCTION(0x50AE9FF9, sceScreenShotDisable),
    HLE_FUNCTION(0x76E674D1, sceScreenShotEnable),
};

}  // namespace

void register_system() {
    register_library({"SceAppMgrUser", kSceAppMgrUser, kAppMgrUser});
    register_library({"ScePower", kScePower, kPower});
    register_library({"SceNet", kSceNet, kNet});
    register_library({"SceNetCtl", kSceNetCtl, kNetCtl});
    register_library({"SceHttp", kSceHttp, kHttp});
    register_library({"SceSsl", kSceSsl, kSsl});
    register_library({"SceDebugLed", kSceDebugLed, kDebugLed});
    register_library({"SceScreenShot", kSceScreenShot, kScreenShot});
}

}  // namespace psvrecore::runtime::hle
