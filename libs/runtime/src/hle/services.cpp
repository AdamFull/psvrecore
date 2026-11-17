// System services of the console's own UI and hardware that a game asks about, answered for the host:
// - the battery (ScePower): a console on its charger, full (the host's power is its own): never low, never asked to
//   suspend; its callbacks are taken and never called;
// - the LiveArea (SceLiveAreaUtil): the system's screen behind the game, which the host has none of: updates are taken
//   and shown nowhere, its status is "ready", frames have no revision or user data;
// - patches (SceGameUpdate): checking for one finds none (the update a game would download, the host already has, in
//   patch/);
// - exporting a picture to the photo library (ScePhotoExport): there is no photo library on the host;
// - the microphone (SceAudioIn): ports open as on the console, and each input is a grain of silence, given at the
//   grain's pace (a call waits until its grain's time has come, on the kernel's clock).

#include <cstring>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kScePower = 0x1082DA7F, kSceLiveAreaUtil = 0x5303CB07, kSceGameUpdate = 0x2DD9A62D,
              kScePhotoExport = 0x79166BD9;

// ---- ScePower

i32 scePowerIsLowBattery() { return 0; }
i32 scePowerIsBatteryCharging() { return 0; }  // (full: not charging)
i32 scePowerIsPowerOnline() { return 1; }
i32 scePowerIsSuspendRequired() { return 0; }
i32 scePowerGetBatteryLifePercent() { return 100; }
i32 scePowerGetBatteryLifeTime() { return 300; }  // (minutes: what a full battery gives)
i32 scePowerGetBatteryFullCapacity() { return 2210; }    // (mAh: the console's battery)
i32 scePowerGetBatteryRemainCapacity() { return 2210; }
i32 scePowerRegisterCallback(i32 /*callback*/) { return 0; }
i32 scePowerUnregisterCallback(i32 /*callback*/) { return 0; }

const Function kPower[] = {
    HLE_FUNCTION(0xD3075926, scePowerIsLowBattery),
    HLE_FUNCTION(0x1E490401, scePowerIsBatteryCharging),
    HLE_FUNCTION(0x87440F5E, scePowerIsPowerOnline),
    HLE_FUNCTION(0x78A1A796, scePowerIsSuspendRequired),
    HLE_FUNCTION(0x2085D15D, scePowerGetBatteryLifePercent),
    HLE_FUNCTION(0x8EFB3FA2, scePowerGetBatteryLifeTime),
    HLE_FUNCTION(0xFD18A0FF, scePowerGetBatteryFullCapacity),
    HLE_FUNCTION(0x94F5A53F, scePowerGetBatteryRemainCapacity),
    HLE_FUNCTION(0x04B7766E, scePowerRegisterCallback),
    HLE_FUNCTION(0xDFA8BAF8, scePowerUnregisterCallback),
};

// ---- SceLiveAreaUtil

constexpr i32 kLiveAreaParam = err::code(0x80104002), kLiveAreaFrameNotFound = err::code(0x8010400A);
i32 sceLiveAreaReplaceAllSync(Ptr<const char> folder) { return folder ? 0 : kLiveAreaParam; }
i32 sceLiveAreaReplaceAllAsync(Ptr<const char> folder) { return folder ? 0 : kLiveAreaParam; }
i32 sceLiveAreaUpdateFrameSync(Ptr<const char> version, Ptr<const char> xml, i32 length, Ptr<const char> /*top*/,
                               u32 /*flags*/) {
    return version && xml && length > 0 ? 0 : kLiveAreaParam;
}
i32 sceLiveAreaUpdateFrameAsync(Ptr<const char> version, Ptr<const char> xml, i32 length, Ptr<const char> top, u32 flags) {
    return sceLiveAreaUpdateFrameSync(version, xml, length, top, flags);
}
i32 sceLiveAreaGetStatus() { return 0; }
i32 sceLiveAreaGetRevision(Ptr<i64> revision) {
    if (!revision || !writable(revision.address, 8)) return kLiveAreaParam;
    *revision = 0;
    return 0;
}
// (no frame was ever shown: none is found)
i32 sceLiveAreaGetFrameRevision(Ptr<const char> frame, Ptr<i64> revision) {
    if (!frame || !revision) return kLiveAreaParam;
    return kLiveAreaFrameNotFound;
}
i32 sceLiveAreaGetFrameUserData(Ptr<const char> frame, Ptr<char> data, u32 size) {
    if (!frame || !data || !size) return kLiveAreaParam;
    return kLiveAreaFrameNotFound;
}

const Function kLiveArea[] = {
    HLE_FUNCTION(0x29C65C0B, sceLiveAreaReplaceAllSync),
    HLE_FUNCTION(0x18066BA9, sceLiveAreaReplaceAllAsync),
    HLE_FUNCTION(0xCFCEDE95, sceLiveAreaUpdateFrameSync),
    HLE_FUNCTION(0xD330285D, sceLiveAreaUpdateFrameAsync),
    HLE_FUNCTION(0x7FE5B83F, sceLiveAreaGetStatus),
    HLE_FUNCTION(0x833F5D0A, sceLiveAreaGetRevision),
    HLE_FUNCTION(0xD6EE2214, sceLiveAreaGetFrameRevision),
    HLE_FUNCTION(0xD6FD367F, sceLiveAreaGetFrameUserData),
};

// ---- SceGameUpdate: SceGameUpdateResult (size, whether a patch exists, its version "XX.XX")

constexpr i32 kUpdateNotInitialized = err::code(0x80412801), kUpdateAlreadyInitialized = err::code(0x80412802),
              kUpdateInvalidArg = err::code(0x80412803);
bool g_update_started = false;
i32 sceGameUpdateInit(Ptr<const void>) {
    if (g_update_started) return kUpdateAlreadyInitialized;
    g_update_started = true;
    return 0;
}
i32 sceGameUpdateTerm() {
    if (!g_update_started) return kUpdateNotInitialized;
    g_update_started = false;
    return 0;
}
i32 sceGameUpdateRun(Ptr<u32> result) {
    if (!g_update_started) return kUpdateNotInitialized;
    if (!result || !writable(result.address, 16) || result.get()[0] != 16) return kUpdateInvalidArg;
    std::memset(result.get() + 1, 0, 12);  // (no patch)
    return 0;
}
i32 sceGameUpdateAbort() { return g_update_started ? 0 : kUpdateNotInitialized; }

const Function kGameUpdate[] = {
    HLE_FUNCTION(0x2F401247, sceGameUpdateInit),
    HLE_FUNCTION(0x48D879BA, sceGameUpdateTerm),
    HLE_FUNCTION(0x3C616238, sceGameUpdateRun),
    HLE_FUNCTION(0x14116007, sceGameUpdateAbort),
};

// ---- ScePhotoExport

constexpr i32 kPhotoParam = err::code(0x80101A01), kPhotoInternal = err::code(0x80101A0C);
i32 export_photo(Ptr<const void> source, Ptr<const void> param) {
    if (!source || !param) return kPhotoParam;
    nx::logi(logs::hle, "the game exports a picture to the photo library: there is none on the host");
    return kPhotoInternal;
}
i32 scePhotoExportFromData(Ptr<const void> data, i32 /*size*/, Ptr<const void> param, Ptr<void>, u32, u32, Ptr<char>, i32) {
    return export_photo(data, param);
}
i32 scePhotoExportFromFile(Ptr<const char> path, Ptr<const void> param, Ptr<void>, u32, u32, Ptr<char>, i32) {
    return export_photo(Ptr<const void>(path.address), param);
}
// (whether it can be: not)
i32 scePhotoExportIsAvailableFromData() { return 0; }
i32 scePhotoExportIsAvailableFromFile() { return 0; }

const Function kPhotoExport[] = {
    HLE_FUNCTION(0x70512321, scePhotoExportFromData),
    HLE_FUNCTION(0x84FD9FC5, scePhotoExportFromFile),
    HLE_FUNCTION(0xE6E6AFD5, scePhotoExportIsAvailableFromData),
    HLE_FUNCTION(0x5655F971, scePhotoExportIsAvailableFromFile),
};

// ---- SceAudioIn: (port type, grain, rate) voice 256 or 512 at 16 kHz, raw 256 at 16 kHz or 768 at 48 kHz; 16-bit mono

constexpr u32 kSceAudioIn = 0xF8DC61A3;
constexpr i32 kInPort = err::code(0x80260101), kInSize = err::code(0x80260102), kInFreq = err::code(0x80260103),
              kInPortType = err::code(0x80260104), kInPointer = err::code(0x80260105),
              kInPortParam = err::code(0x80260106), kInFull = err::code(0x80260107), kInNotOpened = err::code(0x80260109);
struct InPort {
    bool open = false;
    u32 type = 0, grain = 0, rate = 0;
    u64 next_ns = 0;  // (when the next grain is due)
};
nx::mutex g_in_lock;
InPort g_in[2];  // (one port of each type)

i32 sceAudioInOpenPort(u32 type, u32 grain, u32 rate, u32 param) {
    if (type != 0 && type != 2) return kInPortType;
    if (param != 0) return kInPortParam;
    const bool ok = type == 0 ? (rate == 16000 && (grain == 256 || grain == 512))
                              : ((rate == 16000 && grain == 256) || (rate == 48000 && grain == 768));
    if (!ok) return rate == 16000 || rate == 48000 ? kInSize : kInFreq;
    nx::scoped_lock<nx::mutex> held(g_in_lock);
    InPort& p = g_in[type == 0 ? 0 : 1];
    if (p.open) return kInFull;
    p = InPort{true, type, grain, rate, kernel().clock_ns()};
    nx::logi(logs::hle, "microphone: port open ({} samples at {} Hz): silence (the host's microphone is not used)", grain, rate);
    return static_cast<i32>(type == 0 ? 0 : 1);
}
i32 sceAudioInReleasePort(i32 port) {
    if (port < 0 || port > 1) return kInPort;
    nx::scoped_lock<nx::mutex> held(g_in_lock);
    if (!g_in[port].open) return kInNotOpened;
    g_in[port] = InPort{};
    return 0;
}
i32 sceAudioInInput(i32 port, Ptr<u8> dest) {
    if (port < 0 || port > 1) return kInPort;
    u64 due = 0;
    u32 bytes = 0;
    {
        nx::scoped_lock<nx::mutex> held(g_in_lock);
        InPort& p = g_in[port];
        if (!p.open) return kInNotOpened;
        bytes = p.grain * 2;
        if (!dest || !writable(dest.address, bytes)) return kInPointer;
        const u64 now = kernel().clock_ns();
        if (p.next_ns + 100'000'000 < now) p.next_ns = now;  // (not asked for a while: no backlog of silence)
        p.next_ns += u64{p.grain} * 1'000'000'000 / p.rate;
        due = p.next_ns;
    }
    std::memset(dest.get(), 0, bytes);
    if (const u64 now = kernel().clock_ns(); due > now) kernel().delay_thread(static_cast<u32>((due - now) / 1000), false);
    return 0;
}
// (both port types can be had; nothing is muted or plugged in)
i32 sceAudioInGetAdopt(u32 type) { return type == 0 || type == 2 ? 1 : kInPortType; }
i32 sceAudioInGetStatus(i32 select) { return select == 1 ? 0 : kInPortParam; }

const Function kAudioIn[] = {
    HLE_FUNCTION(0x39B50DC1, sceAudioInOpenPort),
    HLE_FUNCTION(0x3A61B8C4, sceAudioInReleasePort),
    HLE_FUNCTION(0x638ADD2D, sceAudioInInput),
    HLE_FUNCTION(0x566AC433, sceAudioInGetAdopt),
    HLE_FUNCTION(0x2F940377, sceAudioInGetStatus),
};

}  // namespace

void register_services() {
    {
        nx::scoped_lock<nx::mutex> held(g_in_lock);
        g_in[0] = g_in[1] = InPort{};
    }
    g_update_started = false;
    register_library({"ScePower", kScePower, kPower});
    register_library({"SceLiveAreaUtil", kSceLiveAreaUtil, kLiveArea});
    register_library({"SceGameUpdate", kSceGameUpdate, kGameUpdate});
    register_library({"ScePhotoExport", kScePhotoExport, kPhotoExport});
    register_library({"SceAudioIn", kSceAudioIn, kAudioIn});
}

}  // namespace psvrecore::runtime::hle
