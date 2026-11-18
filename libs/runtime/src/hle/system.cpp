// The system's services a game asks about that have nothing to do on the host:
// - the application's state (SceAppMgrUser): in front, nothing of the system's over it, no system events;
// - the power configuration (ScePower): taken, the host's power is its own;
// - the network libraries' start (SceNetCtl, SceHttp, SceSsl), as on a console that is offline (SceNet: net.cpp);
// - the development kit's debug LEDs (SceDebugLed): a retail console has none; what a game sets is logged (debug);
// - the system's screenshots (SceScreenShot: the PS button and Start): the host has none of its own, so what a game
//   sets for them (titles, an overlay picture, screenshots allowed or not) is checked and kept nowhere;
// - the camera's shutter sound (SceShutterSound): not played;
// - the cameras (SceCamera): none, as on a PlayStation TV: opening one answers "not mounted", and what needs one open
//   "not open".

#include "hle_common.hpp"

#include <atomic>
#include <cstring>
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAppMgrUser = 0xA6605D6F, kScePower = 0x1082DA7F, kSceNetCtl = 0xC6083AE1,
              kSceSsl = 0xCAF5F2E9, kSceSslInternal = 0x68D6C3FF,
              kSceNetInternal = 0x9E8A69FD, kSceDebugLed = 0xAE004C0A,
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

// Running another executable of the application in place of this one: the runtime runs one recompiled executable, so
// it is refused (logged as what the game asked for); the path must name a file directly under app0:.
constexpr i32 kAppMgrInvalidSelfPath = err::code(0x8080201E), kAppMgrTooLongArgv = err::code(0x8080201D);
i32 sceAppMgrLoadExec(Ptr<const char> path, Ptr<const u32> argv, Ptr<const void> opt) {
    if (!path) return kAppMgrNullPointer;
    if (opt) return kAppMgrInvalid;
    const nx::string p = str(path, 1024);
    if (!p.starts_with("app0:") || p.find('/', 6) != nx::string::npos) return kAppMgrInvalidSelfPath;
    u32 length = 0;
    for (u32 i = 0; argv && readable(argv.address + 4 * i, 4) && argv.get()[i]; ++i)
        length += static_cast<u32>(str(Ptr<const char>(argv.get()[i]), 4096).size()) + 1;
    if (length > 4096) return kAppMgrTooLongArgv;
    nx::loge(logs::hle, "the game asks to run '{}' in its place: not supported (one executable a game)", p);
    return kAppMgrState;
}
// (the warning the system shows when the network goes: none here, the network never comes)
i32 sceAppMgrSetNetworkDisconnectionWarningDialogState(i32 /*state*/) { return 0; }

i32 scePowerSetConfigurationMode(i32 mode) {
    nx::logd(logs::hle, "power configuration mode 0x{:x} (the host's is its own)", static_cast<u32>(mode));
    return 0;
}

// (games set them many times a frame: a change logged, at debug level)
i32 sceKernelSetGPO(u32 leds) {
    static std::atomic<u32> last{0};
    if (last.exchange(leds, std::memory_order_relaxed) != leds) nx::logd(logs::hle, "debug LEDs (GPO): 0x{:02x}", leds);
    return 0;
}
u32 sceKernelGetGPI() { return 0; }                // (nor are there switches to read)

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
std::atomic<bool> g_screenshots_disabled{false};
i32 sceScreenShotDisable() {
    nx::logd(logs::hle, "screenshots: not allowed");
    g_screenshots_disabled = true;
    return 0;
}
i32 sceScreenShotEnable() {
    nx::logd(logs::hle, "screenshots: allowed");
    g_screenshots_disabled = false;
    return 0;
}
// (the notification the system shows when one is taken: none is shown)
i32 sceScreenShotDisableNotification() { return 0; }
i32 sceScreenShotEnableNotification() { return 0; }
// A screenshot the game takes itself: the host has no photo storage to save it in (unless screenshots are disallowed
// and the capture is not forced: that is answered first). Nor are there screenshots to read the details of.
constexpr i32 kScreenShotFileNotFound = err::code(0x80102F03), kScreenShotDisabled = err::code(0x80102F07),
              kScreenShotMediaNotExist = err::code(0x80102F0B);
i32 sceScreenShotCapture(u32 mode, Ptr<char> file_info, u32 /*cancel*/, u32 /*userdata*/) {
    if (mode > 1 || !file_info) return kScreenShotInvalid;
    if (mode == 0 && g_screenshots_disabled) return kScreenShotDisabled;
    return kScreenShotMediaNotExist;
}
i32 sceScreenShotGetParam(Ptr<const char> path, Ptr<u8> info) {
    if (!path || !info) return kScreenShotInvalid;
    return kScreenShotFileNotFound;
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
i32 sceCameraSetEV(i32 device, i32) { return not_open(device); }
i32 sceCameraSetEffect(i32 device, i32) { return not_open(device); }

i32 started(const char* what) {
    nx::logi(logs::hle, "{}: started (offline: nothing connects)", what);
    return 0;
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
// (the NAT type is found by asking a STUN server: not connected)
i32 sceNetCtlGetNatInfo(Ptr<u32> info) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!info || !writable(info.address, 16)) return kNetCtlInvalidAddr;
    return kNetCtlNotConnected;
}
// SceNetCtlIfStat (size, seconds, bytes sent and received, the tick counted from): nothing sent or received.
i32 sceNetCtlGetIfStat(i32 /*device*/, Ptr<u32> stat) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!stat || !writable(stat.address, 64)) return kNetCtlInvalidAddr;
    const u32 size = *stat;
    std::memset(stat.get(), 0, 64);
    *stat = size;
    return 0;
}
// No ad hoc peers.
i32 sceNetCtlAdhocGetPeerList(Ptr<u32> count, Ptr<void> /*peers*/) {
    if (const i32 r = netctl_ready(); r < 0) return r;
    if (!count || !writable(count.address, 4)) return kNetCtlInvalidAddr;
    *count = 0;
    return 0;
}
// (functions known only by NID, imported by a game's own network module: answered "not connected")
i32 sceNetCtl_unnamed() {
    if (const i32 r = netctl_ready(); r < 0) return r;
    return kNetCtlNotConnected;
}
std::atomic<u32> g_ssl_pool{0};
std::atomic<bool> g_ssl_started{false};
i32 sceSslInit(u32 pool_size) {
    g_ssl_pool = pool_size;
    g_ssl_started = true;
    return started("SceSsl");
}

i32 sceNetCtlGetPhoneMaxDownloadableSize(Ptr<u32> size) {
    if (!size || !writable(size.address, 4)) return kNetCtlInvalidAddr;
    *size = 0;
    return 0;
}
i32 sceSslTerm() {
    g_ssl_started = false;
    return 0;
}
// SceSslMemoryPoolStats: the pool's size, the most of it ever in use, in use now (nothing).
constexpr i32 kSslFailed = err::code(0x80435201);
i32 sceSslGetMemoryPoolStats(Ptr<u32> stats) {
    if (!stats || !writable(stats.address, 16)) return kSslFailed;
    const u32 v[4] = {g_ssl_pool.load(), 0, 0, 0};
    std::memcpy(stats.get(), v, sizeof v);
    return 0;
}
// The system's internal SSL and network functions a game's own network module (an NP library it ships) imports.
// Most are known only by NID; offline they are not expected to be reached (the module stops at the network's state),
// and answer the SSL library's failure. Whether SSL has started is known.
i32 sceSslInternalIsInitalized() { return g_ssl_started ? 1 : 0; }
i32 ssl_internal_failed() { return kSslFailed; }
i32 net_internal_not_connected() { return kNetCtlNotConnected; }
i32 sceSslGetSerialNumber(u32, u32, u32, u32) { return kNetCtlNotConnected; }
i32 sceShutterSoundPlay(u32 /*type*/) { return 0; }


const Function kAppMgrUser[] = {
    HLE_FUNCTION(0x5E86319A, _sceAppMgrGetAppState),
    HLE_FUNCTION(0x10B5765F, sceAppMgrReceiveSystemEvent),
    HLE_FUNCTION(0xE6774ABC, sceAppMgrLoadExec),
    HLE_FUNCTION(0xE352B29C, sceAppMgrSetNetworkDisconnectionWarningDialogState),
};
const Function kPower[] = {HLE_FUNCTION(0x3CE187B6, scePowerSetConfigurationMode)};
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
const Function kNetCtlMore[] = {
    HLE_FUNCTION(0x4FB4BEA1, sceNetCtlGetPhoneMaxDownloadableSize),
    HLE_FUNCTION(0x4DDD6149, sceNetCtlGetNatInfo),
    HLE_FUNCTION(0xDA130450, sceNetCtlGetIfStat),
    HLE_FUNCTION(0x77586C59, sceNetCtlAdhocGetPeerList),
    Function{0xC33D7374, "(SceNetCtl 0xC33D7374)", &entry<&sceNetCtl_unnamed>},
    Function{0xE8E8DF18, "(SceNetCtl 0xE8E8DF18)", &entry<&sceNetCtl_unnamed>},
    Function{0xF30365D6, "(SceNetCtl 0xF30365D6)", &entry<&sceNetCtl_unnamed>},
};
const Function kSsl[] = {
    HLE_FUNCTION(0x3C733316, sceSslInit),
    HLE_FUNCTION(0x03CE6E3A, sceSslTerm),
    HLE_FUNCTION(0x901C5C15, sceSslGetSerialNumber),
};
const Function kSslMore[] = {HLE_FUNCTION(0xBD203262, sceSslGetMemoryPoolStats)};
const Function kSslInternal[] = {
    HLE_FUNCTION(0x4B4A2307, sceSslInternalIsInitalized),
    Function{0xF39A49C6, "sceSslInternalWrite", &entry<&ssl_internal_failed>},
    Function{0x064DFC99, "sceSslInternalGetCertificateAuthority", &entry<&ssl_internal_failed>},
    Function{0x0DB6C206, "(SceSslInternal 0x0DB6C206)", &entry<&ssl_internal_failed>},
    Function{0x19B2BB58, "(SceSslInternal 0x19B2BB58)", &entry<&ssl_internal_failed>},
    Function{0x2185BB05, "(SceSslInternal 0x2185BB05)", &entry<&ssl_internal_failed>},
    Function{0x260F5A85, "(SceSslInternal 0x260F5A85)", &entry<&ssl_internal_failed>},
    Function{0x2F0FC3CD, "(SceSslInternal 0x2F0FC3CD)", &entry<&ssl_internal_failed>},
    Function{0x4A1557A7, "(SceSslInternal 0x4A1557A7)", &entry<&ssl_internal_failed>},
    Function{0x4F781BB6, "(SceSslInternal 0x4F781BB6)", &entry<&ssl_internal_failed>},
    Function{0x67FB1CD3, "(SceSslInternal 0x67FB1CD3)", &entry<&ssl_internal_failed>},
    Function{0xAC1DBE40, "(SceSslInternal 0xAC1DBE40)", &entry<&ssl_internal_failed>},
    Function{0xAF50B945, "(SceSslInternal 0xAF50B945)", &entry<&ssl_internal_failed>},
    Function{0xCC7F4211, "(SceSslInternal 0xCC7F4211)", &entry<&ssl_internal_failed>},
    Function{0xCEE46B33, "(SceSslInternal 0xCEE46B33)", &entry<&ssl_internal_failed>},
    Function{0xD28834E6, "(SceSslInternal 0xD28834E6)", &entry<&ssl_internal_failed>},
    Function{0xE5EDD91B, "(SceSslInternal 0xE5EDD91B)", &entry<&ssl_internal_failed>},
    Function{0xE746465E, "(SceSslInternal 0xE746465E)", &entry<&ssl_internal_failed>},
    Function{0xEC9FAF42, "(SceSslInternal 0xEC9FAF42)", &entry<&ssl_internal_failed>},
    Function{0xEF9EBAC7, "(SceSslInternal 0xEF9EBAC7)", &entry<&ssl_internal_failed>},
    Function{0xF2C9B744, "(SceSslInternal 0xF2C9B744)", &entry<&ssl_internal_failed>},
    Function{0xF71DB06F, "(SceSslInternal 0xF71DB06F)", &entry<&ssl_internal_failed>},
    Function{0xFBEB1FD7, "(SceSslInternal 0xFBEB1FD7)", &entry<&ssl_internal_failed>},
    Function{0xFFE2ED51, "(SceSslInternal 0xFFE2ED51)", &entry<&ssl_internal_failed>},
};
const Function kNetInternal[] = {
    Function{0x333F9CB3, "(SceNetInternal 0x333F9CB3)", &entry<&net_internal_not_connected>},
    Function{0x689B9D7D, "(SceNetInternal 0x689B9D7D)", &entry<&net_internal_not_connected>},
};
const Function kShutterSound[] = {HLE_FUNCTION(0x7FFB6D79, sceShutterSoundPlay)};
const Function kDebugLed[] = {HLE_FUNCTION(0x78E702D3, sceKernelSetGPO), HLE_FUNCTION(0x14F582CF, sceKernelGetGPI)};
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
    HLE_FUNCTION(0x62AFF0B8, sceCameraSetEV),
    HLE_FUNCTION(0xE9D2CFB1, sceCameraSetEffect),
};
const Function kScreenShot[] = {
    HLE_FUNCTION(0x05DB59C7, sceScreenShotSetParam),
    HLE_FUNCTION(0x7061665B, sceScreenShotSetOverlayImage),
    HLE_FUNCTION(0x50AE9FF9, sceScreenShotDisable),
    HLE_FUNCTION(0x76E674D1, sceScreenShotEnable),
    HLE_FUNCTION(0xDA80A228, sceScreenShotDisableNotification),
    HLE_FUNCTION(0x816633FE, sceScreenShotEnableNotification),
    HLE_FUNCTION(0xD509FB08, sceScreenShotCapture),
    HLE_FUNCTION(0xBAE9E534, sceScreenShotGetParam),
};

}  // namespace

void register_system() {
    g_ssl_started = false;
    g_screenshots_disabled = false;
    register_library({"SceAppMgrUser", kSceAppMgrUser, kAppMgrUser});
    register_library({"ScePower", kScePower, kPower});
    register_library({"SceNetCtl", kSceNetCtl, kNetCtl});
    register_library({"SceSsl", kSceSsl, kSsl});
    register_library({"SceSsl", kSceSsl, kSslMore});
    register_library({"SceSslInternal", kSceSslInternal, kSslInternal});
    register_library({"SceNetInternal", kSceNetInternal, kNetInternal});
    register_library({"SceDebugLed", kSceDebugLed, kDebugLed});
    register_library({"SceScreenShot", kSceScreenShot, kScreenShot});
    register_library({"SceCamera", kSceCamera, kCamera});
    register_library({"SceShutterSound", kSceShutterSound, kShutterSound});
    register_library({"SceNetCtl", kSceNetCtl, kNetCtlMore});
}

}  // namespace psvrecore::runtime::hle
