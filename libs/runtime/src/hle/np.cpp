// PlayStation Network, as on a console that is offline and signed out: the libraries start (SceNpManager,
// SceNpScore, SceNpActivity, SceNpTrophy); asking for the user's NP ID or posting an activity answers "signed out";
// the service state callback is never called (the state does not change). Trophies are local, as on the console:
// contexts and handles are made, and an unlocked trophy is logged and remembered for the run (unlocking it again
// answers "already unlocked").

#include "hle_common.hpp"
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNpManager = 0xD8835093, kSceNpScore = 0x75F052DB, kSceNpActivity = 0xFEF492ED,
              kSceNpTrophy = 0x4332C10D;
constexpr i32 kManagerSignedOut = err::code(0x8055050A), kActivitySignedOut = err::code(0x80552306),
              kNpInvalidArgument = err::code(0x80550003);
// (trophy errors, assumed from the library's numbering: invalid argument, already unlocked)
constexpr i32 kTrophyInvalidArgument = err::code(0x80551604), kTrophyAlreadyUnlocked = err::code(0x80551619);

struct Np {
    nx::mutex lock;
    u32 next_context = 1, next_handle = 1;
    nx::hash_map<i32, bool> unlocked;
};
Np& np() {
    static Np n;
    return n;
}

i32 sceNpInit(Ptr<const void>, Ptr<const void>) {
    nx::logi(logs::hle, "PSN: started offline, signed out");
    return 0;
}
i32 sceNpCheckCallback() { return 0; }
i32 sceNpRegisterServiceStateCallback(u32 /*callback*/, u32 /*userdata*/) { return 0; }
i32 sceNpManagerGetNpId(Ptr<void> out) { return out ? kManagerSignedOut : kNpInvalidArgument; }

i32 sceNpScoreInit(i32, i32, Ptr<const void>) { return 0; }
i32 sceNpScoreTerm() { return 0; }

i32 sceNpActivityInit(Ptr<const void>) { return 0; }
i32 sceNpActivityTerm() { return 0; }
i32 sceNpActivityPostStatus(Ptr<const void>, Ptr<const void>, Ptr<const void>) { return kActivitySignedOut; }

i32 sceNpTrophyInit(Ptr<const void>) { return 0; }
i32 sceNpTrophyCreateContext(Ptr<i32> context, Ptr<const void> /*comm_id*/, Ptr<const void> /*comm_sign*/, u64 /*options*/) {
    if (!context || !writable(context.address, 4)) return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    *context = static_cast<i32>(np().next_context++);
    return 0;
}
i32 sceNpTrophyCreateHandle(Ptr<i32> handle) {
    if (!handle || !writable(handle.address, 4)) return kTrophyInvalidArgument;
    nx::scoped_lock<nx::mutex> held(np().lock);
    *handle = static_cast<i32>(np().next_handle++);
    return 0;
}
i32 sceNpTrophyDestroyHandle(i32) { return 0; }
i32 sceNpTrophyUnlockTrophy(i32 /*context*/, i32 /*handle*/, i32 trophy, Ptr<i32> platinum) {
    if (platinum && writable(platinum.address, 4)) *platinum = -1;  // (SCE_NP_TROPHY_INVALID_TROPHY_ID: none unlocked)
    nx::scoped_lock<nx::mutex> held(np().lock);
    if (np().unlocked.contains(trophy)) return kTrophyAlreadyUnlocked;
    np().unlocked[trophy] = true;
    nx::logi(logs::hle, "trophy {} unlocked", trophy);
    return 0;
}

const Function kNpManager[] = {
    HLE_FUNCTION(0x04D9F484, sceNpInit),
    HLE_FUNCTION(0x3B0AE9A9, sceNpCheckCallback),
    HLE_FUNCTION(0x3C94B4B4, sceNpManagerGetNpId),
    HLE_FUNCTION(0x44239C35, sceNpRegisterServiceStateCallback),
};
const Function kNpScore[] = {
    HLE_FUNCTION(0x0433069F, sceNpScoreInit),
    HLE_FUNCTION(0x2050F98F, sceNpScoreTerm),
};
const Function kNpActivity[] = {
    HLE_FUNCTION(0xE0FFEE97, sceNpActivityInit),
    HLE_FUNCTION(0x9EA4901F, sceNpActivityTerm),
    HLE_FUNCTION(0xBC7FDC77, sceNpActivityPostStatus),
};
const Function kNpTrophy[] = {
    HLE_FUNCTION(0x34516838, sceNpTrophyInit),
    HLE_FUNCTION(0xC49FD33F, sceNpTrophyCreateContext),
    HLE_FUNCTION(0x4EBC6977, sceNpTrophyCreateHandle),
    HLE_FUNCTION(0xFF142071, sceNpTrophyDestroyHandle),
    HLE_FUNCTION(0xB397AA24, sceNpTrophyUnlockTrophy),
};

}  // namespace

void register_np() {
    {
        nx::scoped_lock<nx::mutex> held(np().lock);
        np().unlocked.clear();
        np().next_context = np().next_handle = 1;
    }
    register_library({"SceNpManager", kSceNpManager, kNpManager});
    register_library({"SceNpScore", kSceNpScore, kNpScore});
    register_library({"SceNpActivity", kSceNpActivity, kNpActivity});
    register_library({"SceNpTrophy", kSceNpTrophy, kNpTrophy});
}

}  // namespace psvrecore::runtime::hle
