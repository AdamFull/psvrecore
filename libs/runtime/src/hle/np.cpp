// PlayStation Network, as on a console that is offline and signed out: the libraries start (SceNpManager,
// SceNpScore, SceNpActivity, SceNpTrophy); asking for the user's NP ID or posting an activity answers "signed out";
// the service state callback is never called (the state does not change). Trophies are local, as on the console:
// contexts and handles are made, and an unlocked trophy is logged and remembered for the run (unlocking it again
// answers "already unlocked". Authentication (SceNpCommon's NpAuth), friends and presence (SceNpBasic) and user
// lookups (SceNpUtility) start, and what would reach PSN answers "signed out".

#include <cstring>

#include "hle_common.hpp"
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNpManager = 0xD8835093, kSceNpScore = 0x75F052DB, kSceNpActivity = 0xFEF492ED,
              kSceNpTrophy = 0x4332C10D, kSceNpCommon = 0xECB62224, kSceNpBasic = 0x605BA760, kSceNpUtility = 0x7B47D9CF;
constexpr i32 kAuthSignedOut = err::code(0x80550310), kBasicSignedOut = err::code(0x80551D06);
constexpr i32 kServiceStateSignedOut = 1;
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
i32 sceNpTerm() { return 0; }
i32 sceNpGetServiceState(Ptr<i32> state) {
    if (!state || !writable(state.address, 4)) return kNpInvalidArgument;
    *state = kServiceStateSignedOut;
    return 0;
}
i32 sceNpManagerGetChatRestrictionFlag(Ptr<i32> flag) { return flag ? kManagerSignedOut : kNpInvalidArgument; }
i32 sceNpManagerGetContentRatingFlag(Ptr<i32> restricted, Ptr<i32> age) {
    return restricted && age ? kManagerSignedOut : kNpInvalidArgument;
}

// (NpAuth: a ticket request needs PSN)
i32 sceNpAuthInit() { return 0; }
i32 sceNpAuthTerm() { return 0; }
i32 sceNpAuthCreateStartRequest(Ptr<const void> param) { return param ? kAuthSignedOut : kNpInvalidArgument; }

// (NpBasic: friends and presence need PSN; handlers are kept and never called)
i32 sceNpBasicInit(Ptr<const void>) { return 0; }
i32 sceNpBasicTerm() { return 0; }
i32 sceNpBasicRegisterHandler(Ptr<const void>, Ptr<const void>, Ptr<void>) { return 0; }
i32 sceNpBasicUnregisterHandler() { return 0; }
i32 sceNpBasicCheckCallback() { return 0; }
i32 sceNpBasicSetInGamePresence(Ptr<const void>) { return kBasicSignedOut; }
i32 sceNpBasicGetGameJoiningPresence(Ptr<const void>, Ptr<void>) { return kBasicSignedOut; }
i32 sceNpBasicGetFriendListEntryCount(Ptr<u32>) { return kBasicSignedOut; }
i32 sceNpBasicGetFriendListEntries(u32, Ptr<void>, u32, Ptr<u32>) { return kBasicSignedOut; }

// (NpLookup: a title context needs PSN; the error assumed as the manager's "signed out")
i32 sceNpLookupInit(i32 /*async*/, i32 /*priority*/, i32 /*affinity*/, Ptr<const void>) { return 0; }
i32 sceNpLookupTerm() { return 0; }
i32 sceNpLookupCreateTitleCtx(Ptr<const void> comm_id, Ptr<const void> self) {
    return comm_id && self ? kManagerSignedOut : kNpInvalidArgument;
}

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
    HLE_FUNCTION(0x19E40AE1, sceNpTerm),
    HLE_FUNCTION(0x54060DF6, sceNpGetServiceState),
    HLE_FUNCTION(0x60C575B1, sceNpManagerGetChatRestrictionFlag),
    HLE_FUNCTION(0xAF0073B2, sceNpManagerGetContentRatingFlag),
};
const Function kNpCommon[] = {
    HLE_FUNCTION(0x441D8B4E, sceNpAuthInit),
    HLE_FUNCTION(0x6093B689, sceNpAuthTerm),
    HLE_FUNCTION(0xED42079F, sceNpAuthCreateStartRequest),
};
const Function kNpBasic[] = {
    HLE_FUNCTION(0xEFB91A99, sceNpBasicInit),
    HLE_FUNCTION(0x389BCB3B, sceNpBasicTerm),
    HLE_FUNCTION(0x26E6E048, sceNpBasicRegisterHandler),
    HLE_FUNCTION(0x050AE072, sceNpBasicUnregisterHandler),
    HLE_FUNCTION(0x20146AEC, sceNpBasicCheckCallback),
    HLE_FUNCTION(0x51D75562, sceNpBasicSetInGamePresence),
    HLE_FUNCTION(0x863249CB, sceNpBasicGetGameJoiningPresence),
    HLE_FUNCTION(0xDF41F308, sceNpBasicGetFriendListEntryCount),
    HLE_FUNCTION(0xFF07E787, sceNpBasicGetFriendListEntries),
};
const Function kNpUtility[] = {
    HLE_FUNCTION(0x9246A673, sceNpLookupInit),
    HLE_FUNCTION(0x0158B61B, sceNpLookupTerm),
    HLE_FUNCTION(0x5110E17E, sceNpLookupCreateTitleCtx),
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
i32 sceNpTrophyDestroyContext(i32) { return 0; }
i32 sceNpTrophyTerm() { return 0; }
// SceNpTrophyFlagArray (128 bits): the trophies unlocked in this run; `count` is the set's size, unknown here (0).
i32 sceNpTrophyGetTrophyUnlockState(i32 /*context*/, i32 /*handle*/, Ptr<u32> flags, Ptr<u32> count) {
    if (!flags || !count || !writable(flags.address, 16) || !writable(count.address, 4)) return kTrophyInvalidArgument;
    u32 bits[4] = {};
    nx::scoped_lock<nx::mutex> held(np().lock);
    for (const auto& [trophy, on] : np().unlocked)
        if (on && trophy >= 0 && trophy < 128) bits[trophy >> 5] |= 1u << (trophy & 31);
    std::memcpy(flags.get(), bits, sizeof bits);
    *count = 0;
    return 0;
}

const Function kNpTrophy[] = {
    HLE_FUNCTION(0x34516838, sceNpTrophyInit),
    HLE_FUNCTION(0xC49FD33F, sceNpTrophyCreateContext),
    HLE_FUNCTION(0x4EBC6977, sceNpTrophyCreateHandle),
    HLE_FUNCTION(0xFF142071, sceNpTrophyDestroyHandle),
    HLE_FUNCTION(0xB397AA24, sceNpTrophyUnlockTrophy),
    HLE_FUNCTION(0x56F5CBA5, sceNpTrophyDestroyContext),
    HLE_FUNCTION(0xBFE0F28F, sceNpTrophyTerm),
    HLE_FUNCTION(0xC8D2A4DE, sceNpTrophyGetTrophyUnlockState),
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
    register_library({"SceNpCommon", kSceNpCommon, kNpCommon});
    register_library({"SceNpBasic", kSceNpBasic, kNpBasic});
    register_library({"SceNpUtility", kSceNpUtility, kNpUtility});
}

}  // namespace psvrecore::runtime::hle
