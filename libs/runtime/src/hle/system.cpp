// The system's services a game asks about that have nothing to do on the host:
// - the application's state (SceAppMgrUser): in front, nothing of the system's over it, no system events;
// - the power configuration (ScePower): taken, the host's power is its own;
// - the network libraries (SceNet, SceNetCtl, SceHttp, SceSsl): they start, as on a console that is offline; what
//   would connect is not implemented;
// - the development kit's debug LEDs (SceDebugLed): a retail console has none, so setting them does nothing;
// - the system's screenshots (SceScreenShot: the PS button and Start): the host has none of its own, so what a game
//   sets for them (titles, an overlay picture, screenshots allowed or not) is checked and kept nowhere;
// - the camera's shutter sound (SceShutterSound): not played;
// - the cameras (SceCamera): none, as on a PlayStation TV: opening one answers "not mounted", and what needs one open
//   "not open".

#include "hle_common.hpp"

#include <cstring>
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAppMgrUser = 0xA6605D6F, kScePower = 0x1082DA7F, kSceNet = 0x6BF8B2A2, kSceNetCtl = 0xC6083AE1,
              kSceHttp = 0xE8F15CDE, kSceSsl = 0xCAF5F2E9, kSceDebugLed = 0xAE004C0A,
              kSceScreenShot = 0xF26FC97D, kSceCamera = 0xDA91B3ED, kSceShutterSound = 0x877181ED;
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

constexpr i32 kCameraParam = err::code(0x802E0000), kCameraNotOpen = err::code(0x802E0004),
              kCameraNotMounted = err::code(0x802E0010);
i32 sceCameraOpen(i32 device, Ptr<void> info) {
    if (device < 0 || device > 1 || !info) return kCameraParam;
    nx::logi(logs::hle, "camera {}: none on the host (not mounted)", device);
    return kCameraNotMounted;
}
i32 not_open(i32 device) { return device < 0 || device > 1 ? kCameraParam : kCameraNotOpen; }
i32 sceCameraClose(i32 device) { return not_open(device); }
i32 sceCameraStart(i32 device) { return not_open(device); }
i32 sceCameraStop(i32 device) { return not_open(device); }
i32 sceCameraRead(i32 device, Ptr<void>) { return not_open(device); }
i32 sceCameraGetReverse(i32 device, Ptr<i32>) { return not_open(device); }
i32 sceCameraSetReverse(i32 device, i32) { return not_open(device); }
i32 sceCameraSetWhiteBalance(i32 device, i32) { return not_open(device); }
i32 sceCameraSetBacklight(i32 device, i32) { return not_open(device); }
i32 sceCameraIsActive(i32 device) { return device < 0 || device > 1 ? kCameraParam : 0; }  // (none is)
i32 sceCameraSetAutoControlHold(i32 device, i32) { return not_open(device); }
i32 sceCameraSetISO(i32 device, i32) { return not_open(device); }
i32 sceCameraSetNightmode(i32 device, i32) { return not_open(device); }
i32 sceCameraSetAntiFlicker(i32 device, i32) { return not_open(device); }

i32 started(const char* what) {
    nx::logi(logs::hle, "{}: started (offline: nothing connects)", what);
    return 0;
}
i32 sceNetInit(Ptr<const void>) { return started("SceNet"); }
i32 sceNetTerm() { return 0; }

// Byte order (the guest's is little-endian, the network's big-endian) and addresses as text.
u32 sceNetHtonl(u32 v) { return __builtin_bswap32(v); }
u32 sceNetNtohl(u32 v) { return __builtin_bswap32(v); }
u32 sceNetHtons(u32 v) { return __builtin_bswap16(static_cast<u16>(v)); }
u32 sceNetNtohs(u32 v) { return __builtin_bswap16(static_cast<u16>(v)); }
constexpr i32 kNetAfInet = 2;
constexpr i32 kNetEafnosupport = err::code(0x8041012F);  // (SCE_NET_ERROR_EAFNOSUPPORT)
// IPv4 only: "a.b.c.d" into 4 bytes in network order; 1 if parsed, 0 if not.
i32 sceNetInetPton(i32 af, Ptr<const char> text, Ptr<u8> out) {
    if (af != kNetAfInet) return kNetEafnosupport;
    if (!text || !out || !writable(out.address, 4)) return err::kFault;
    const nx::string t = str(text, 16);
    u32 part[4] = {}, n = 0, digits = 0;
    for (const char c : t) {
        if (c >= '0' && c <= '9') {
            part[n] = part[n] * 10 + static_cast<u32>(c - '0');
            if (++digits > 3 || part[n] > 255) return 0;
        } else if (c == '.' && digits && n < 3) {
            ++n, digits = 0;
        } else {
            return 0;
        }
    }
    if (n != 3 || !digits) return 0;
    for (u32 k = 0; k < 4; ++k) out.get()[k] = static_cast<u8>(part[k]);
    return 1;
}
u32 sceNetInetNtop(i32 af, Ptr<const u8> address, Ptr<char> out, u32 size) {
    if (af != kNetAfInet || !address || !out) return 0;
    const u8* a = address.get();
    const nx::string t = nx::format("{}.{}.{}.{}", a[0], a[1], a[2], a[3]);
    if (size < t.size() + 1 || !writable(out.address, size)) return 0;
    std::memcpy(out.get(), t.c_str(), t.size() + 1);
    return out.address;
}

// ---- SceNetCtl: offline, the connection and ad hoc both disconnected; nothing ever changes, so callbacks never run
constexpr i32 kNetCtlNotInitialized = err::code(0x80412101), kNetCtlCallbackMax = err::code(0x80412103),
              kNetCtlIdNotFound = err::code(0x80412104), kNetCtlInvalidId = err::code(0x80412105),
              kNetCtlInvalidAddr = err::code(0x80412107), kNetCtlNotConnected = err::code(0x80412108);
constexpr u32 kNetCtlCallbacks = 8;
struct NetCtl {
    nx::mutex lock;
    bool initialized = false;
    bool inet[kNetCtlCallbacks] = {}, adhoc[kNetCtlCallbacks] = {};
};
NetCtl& netctl() {
    static NetCtl n;
    return n;
}
i32 sceNetCtlInit() {
    {
        NetCtl& n = netctl();
        nx::scoped_lock<nx::mutex> held(n.lock);
        n.initialized = true;
    }
    return started("SceNetCtl");
}
i32 sceNetCtlTerm() {
    NetCtl& n = netctl();
    nx::scoped_lock<nx::mutex> held(n.lock);
    n.initialized = false;
    for (u32 i = 0; i < kNetCtlCallbacks; ++i) n.inet[i] = n.adhoc[i] = false;
    return 0;
}
i32 netctl_ready() {
    NetCtl& n = netctl();
    nx::scoped_lock<nx::mutex> held(n.lock);
    return n.initialized ? 0 : kNetCtlNotInitialized;
}
i32 sceNetCtlCheckCallback() { return netctl_ready(); }
i32 register_callback(bool* slots, Ptr<i32> cid) {
    NetCtl& n = netctl();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.initialized) return kNetCtlNotInitialized;
    if (!cid || !writable(cid.address, 4)) return kNetCtlInvalidAddr;
    for (u32 i = 0; i < kNetCtlCallbacks; ++i)
        if (!slots[i]) {
            slots[i] = true;
            *cid = static_cast<i32>(i);
            return 0;
        }
    return kNetCtlCallbackMax;
}
i32 unregister_callback(bool* slots, i32 cid) {
    NetCtl& n = netctl();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (!n.initialized) return kNetCtlNotInitialized;
    if (cid < 0 || cid >= static_cast<i32>(kNetCtlCallbacks)) return kNetCtlInvalidId;
    if (!slots[cid]) return kNetCtlIdNotFound;
    slots[cid] = false;
    return 0;
}
i32 sceNetCtlInetRegisterCallback(Ptr<const void>, Ptr<void>, Ptr<i32> cid) { return register_callback(netctl().inet, cid); }
i32 sceNetCtlInetUnregisterCallback(i32 cid) { return unregister_callback(netctl().inet, cid); }
i32 sceNetCtlAdhocRegisterCallback(Ptr<const void>, Ptr<void>, Ptr<i32> cid) { return register_callback(netctl().adhoc, cid); }
i32 sceNetCtlAdhocUnregisterCallback(i32 cid) { return unregister_callback(netctl().adhoc, cid); }
i32 get_state(Ptr<i32> state) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!state || !writable(state.address, 4)) return kNetCtlInvalidAddr;
    *state = 0;  // (SCE_NET_CTL_STATE_DISCONNECTED)
    return 0;
}
i32 sceNetCtlInetGetState(Ptr<i32> state) { return get_state(state); }
i32 sceNetCtlAdhocGetState(Ptr<i32> state) { return get_state(state); }
// (the result of the last event: there was none)
i32 get_result(i32 /*event*/, Ptr<i32> error) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!error || !writable(error.address, 4)) return kNetCtlInvalidAddr;
    *error = 0;
    return 0;
}
i32 sceNetCtlInetGetResult(i32 event, Ptr<i32> error) { return get_result(event, error); }
i32 sceNetCtlAdhocGetResult(i32 event, Ptr<i32> error) { return get_result(event, error); }
i32 sceNetCtlInetGetInfo(i32 code, Ptr<void> info) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!info) return kNetCtlInvalidAddr;
    nx::logd(logs::hle, "network information {}: not connected", code);
    return kNetCtlNotConnected;
}
i32 sceNetCtlAdhocGetInAddr(Ptr<void> address) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!address) return kNetCtlInvalidAddr;
    return kNetCtlNotConnected;
}
i32 sceNetCtlAdhocDisconnect() { return netctl_ready(); }
i32 sceHttpInit(u32) { return started("SceHttp"); }
i32 sceSslInit(u32) { return started("SceSsl"); }

i32 sceNetCtlGetPhoneMaxDownloadableSize(Ptr<u32> size) {
    if (!size || !writable(size.address, 4)) return kNetCtlInvalidAddr;
    *size = 0;
    return 0;
}
i32 sceSslTerm() { return 0; }
i32 sceSslGetSerialNumber(u32, u32, u32, u32) { return kNetCtlNotConnected; }
i32 sceShutterSoundPlay(u32 /*type*/) { return 0; }


const Function kAppMgrUser[] = {
    HLE_FUNCTION(0x5E86319A, _sceAppMgrGetAppState),
    HLE_FUNCTION(0x10B5765F, sceAppMgrReceiveSystemEvent),
};
const Function kPower[] = {HLE_FUNCTION(0x3CE187B6, scePowerSetConfigurationMode)};
const Function kNet[] = {
    HLE_FUNCTION(0xEB03E265, sceNetInit),
    HLE_FUNCTION(0xEA3CC286, sceNetTerm),
    HLE_FUNCTION(0x4C30B03C, sceNetHtonl),
    HLE_FUNCTION(0xD2EAA645, sceNetNtohl),
    HLE_FUNCTION(0x9FA3207B, sceNetHtons),
    HLE_FUNCTION(0x07845128, sceNetNtohs),
    HLE_FUNCTION(0xD5EEB048, sceNetInetPton),
    HLE_FUNCTION(0x98839B74, sceNetInetNtop),
};
const Function kNetCtl[] = {
    HLE_FUNCTION(0x495CA1DB, sceNetCtlInit),
    HLE_FUNCTION(0xCD188648, sceNetCtlTerm),
    HLE_FUNCTION(0xDFFC3ED4, sceNetCtlCheckCallback),
    HLE_FUNCTION(0xEAEE6185, sceNetCtlInetRegisterCallback),
    HLE_FUNCTION(0xD0C3BF3F, sceNetCtlInetUnregisterCallback),
    HLE_FUNCTION(0xFFA9D594, sceNetCtlAdhocRegisterCallback),
    HLE_FUNCTION(0xA4471E10, sceNetCtlAdhocUnregisterCallback),
    HLE_FUNCTION(0x6D26AC68, sceNetCtlInetGetState),
    HLE_FUNCTION(0x0961A561, sceNetCtlAdhocGetState),
    HLE_FUNCTION(0x6B20EC02, sceNetCtlInetGetResult),
    HLE_FUNCTION(0x7AE0ED19, sceNetCtlAdhocGetResult),
    HLE_FUNCTION(0xB26D07F3, sceNetCtlInetGetInfo),
    HLE_FUNCTION(0x7118C99D, sceNetCtlAdhocGetInAddr),
    HLE_FUNCTION(0xED43B79A, sceNetCtlAdhocDisconnect),
};
const Function kNetCtlMore[] = {HLE_FUNCTION(0x4FB4BEA1, sceNetCtlGetPhoneMaxDownloadableSize)};
const Function kHttp[] = {HLE_FUNCTION(0x214926D9, sceHttpInit)};
const Function kSsl[] = {
    HLE_FUNCTION(0x3C733316, sceSslInit),
    HLE_FUNCTION(0x03CE6E3A, sceSslTerm),
    HLE_FUNCTION(0x901C5C15, sceSslGetSerialNumber),
};
const Function kShutterSound[] = {HLE_FUNCTION(0x7FFB6D79, sceShutterSoundPlay)};
const Function kDebugLed[] = {HLE_FUNCTION(0x78E702D3, sceKernelSetGPO)};
const Function kCamera[] = {
    HLE_FUNCTION(0xA462F801, sceCameraOpen),
    HLE_FUNCTION(0xCD6E1CFC, sceCameraClose),
    HLE_FUNCTION(0xA8FEAE35, sceCameraStart),
    HLE_FUNCTION(0x1DD9C9CE, sceCameraStop),
    HLE_FUNCTION(0x79B5C2DE, sceCameraRead),
    HLE_FUNCTION(0x44F6043F, sceCameraGetReverse),
    HLE_FUNCTION(0x1175F477, sceCameraSetReverse),
    HLE_FUNCTION(0x4D4514AC, sceCameraSetWhiteBalance),
    HLE_FUNCTION(0xAE071044, sceCameraSetBacklight),
    HLE_FUNCTION(0x103A75B8, sceCameraIsActive),
    HLE_FUNCTION(0x3A0DABBD, sceCameraSetAutoControlHold),
    HLE_FUNCTION(0x3CF630A1, sceCameraSetISO),
    HLE_FUNCTION(0x3F26233E, sceCameraSetNightmode),
    HLE_FUNCTION(0xE312958A, sceCameraSetAntiFlicker),
};
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
    register_library({"SceCamera", kSceCamera, kCamera});
    register_library({"SceShutterSound", kSceShutterSound, kShutterSound});
    register_library({"SceNetCtl", kSceNetCtl, kNetCtlMore});
}

}  // namespace psvrecore::runtime::hle
