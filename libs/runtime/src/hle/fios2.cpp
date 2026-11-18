// The system side of libfios2's overlays (SceFios2User). libfios2 (a module the system preloads for every game,
// recompiled like the game) asks the system which overlays (path remappings) the process has and resolves paths
// through them. Overlays a game adds are not supported yet: the process has none, so a path resolves to itself.
//
// The functions' arguments are as libfios2's wrappers pass them (its own API, documented in the SDK, plus the process
// in front): GetList02(process, min order, max order, ids, max ids, *actual count) and
// ResolveWithRangeSync02(process, flags, in path, out path, max length, min order, max order).
//
// Older libfios2 builds (firmware 1.x: WipEout 2048's) import the same services from SceFios2Kernel, as
// _sceFiosKernelOverlay*. Most take their arguments as the API does; GetList, ResolveSync and ResolveWithRangeSync
// take the first three and a block of the rest on the caller's stack (laid out below, as libfios2's wrappers fill
// it).

#include "hle_common.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceFios2User = 0xC2ED82D7;

i32 sceFiosOverlayGetList02(i32 /*process*/, u32 /*min_order*/, u32 /*max_order*/, Ptr<i32> /*ids*/, u32 /*max_ids*/,
                            Ptr<u32> actual) {
    if (actual && writable(actual.address, 4)) *actual = 0;
    return err::kOk;
}

i32 resolve_to_itself(i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length) {
    const nx::string path = str(in, 1024);
    nx::logd(logs::io, "fios overlay resolve '{}' (flags 0x{:x}): itself", path, static_cast<u32>(flags));
    if (!out || max_length == 0) return err::kOk;
    if (!writable(out.address, max_length)) return err::kFault;
    const u32 n = std::min<u32>(max_length - 1, static_cast<u32>(path.size()));
    std::memcpy(out.get(), path.data(), n);
    out.get()[n] = 0;
    return err::kOk;
}

i32 sceFiosOverlayResolveWithRangeSync02(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length,
                                         u32 /*min_order*/, u32 /*max_order*/) {
    return resolve_to_itself(flags, in, out, max_length);
}

i32 sceFiosOverlayResolveSync02(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length) {
    return resolve_to_itself(flags, in, out, max_length);
}

GuestLocal<bool> g_overlays_disabled{false};  // (the calling guest thread's)
i32 sceFiosOverlayThreadIsDisabled02() { return g_overlays_disabled.get() ? 1 : 0; }

// The scheduler a path's I/O goes to: the first (only a development kit's host0: paths go to another).
i32 sceFiosOverlayGetRecommendedScheduler02(i32 /*scheduler_count*/, Ptr<const char> /*path*/, Ptr<void> /*args*/) {
    return 0;
}

i32 sceFiosOverlayThreadSetDisabled02(i32 disabled) {
    nx::logd(logs::io, "fios overlays for this thread {}", disabled ? "off" : "on");
    g_overlays_disabled.get() = disabled != 0;
    return err::kOk;
}

i32 unsupported(const char* what) {
    nx::logw(logs::io, "{}: overlays are not supported", what);
    return err::kUnsupported;
}
i32 sceFiosOverlayAddForProcess02() { return unsupported("sceFiosOverlayAddForProcess02"); }
i32 sceFiosOverlayModifyForProcess02() { return unsupported("sceFiosOverlayModifyForProcess02"); }
i32 sceFiosOverlayRemoveForProcess02() { return unsupported("sceFiosOverlayRemoveForProcess02"); }
i32 sceFiosOverlayGetInfoForProcess02() { return unsupported("sceFiosOverlayGetInfoForProcess02"); }

// ---- SceFios2Kernel (older libfios2)

constexpr u32 kSceFios2Kernel = 0x8757B742;

struct GetListArgs {  // GetList(process, min order, max order, this)
    Ptr<i32> ids;
    u32 max_ids;
    Ptr<u32> actual;
    u32 ids_size;  // (bytes: max_ids * 4, at most 0x200)
};
struct ResolveArgs {  // ResolveSync(process, flags, in path, this)
    Ptr<char> out;
    u32 max_length;
    u32 in_length;  // (strlen(in) + 1)
    u32 out_size;
};
struct ResolveWithRangeArgs {  // ResolveWithRangeSync(process, flags, in path, this)
    Ptr<char> out;
    u32 max_length;
    u8 min_order, max_order, pad[2];
    u32 in_length;
    u32 out_size;
};
static_assert(sizeof(GetListArgs) == 16 && sizeof(ResolveArgs) == 16 && sizeof(ResolveWithRangeArgs) == 20);

i32 _sceFiosKernelOverlayGetList(i32 process, u32 min_order, u32 max_order, Ptr<GetListArgs> args) {
    if (!args || !readable(args.address, sizeof(GetListArgs))) return err::kFault;
    const GetListArgs a = *args;
    return sceFiosOverlayGetList02(process, min_order, max_order, a.ids, a.max_ids, a.actual);
}
i32 _sceFiosKernelOverlayResolveSync(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<ResolveArgs> args) {
    if (!args || !readable(args.address, sizeof(ResolveArgs))) return err::kFault;
    const ResolveArgs a = *args;
    return resolve_to_itself(flags, in, a.out, a.max_length);
}
i32 _sceFiosKernelOverlayResolveWithRangeSync(i32 /*process*/, i32 flags, Ptr<const char> in,
                                              Ptr<ResolveWithRangeArgs> args) {
    if (!args || !readable(args.address, sizeof(ResolveWithRangeArgs))) return err::kFault;
    const ResolveWithRangeArgs a = *args;
    return resolve_to_itself(flags, in, a.out, a.max_length);
}
// Which of `available` I/O schedulers suits a path (its length in the block): libfios2 runs a path on its second
// scheduler when the answer is 1. The host's files are all on one device: the first.
i32 _sceFiosKernelOverlayGetRecommendedScheduler(i32 available, Ptr<const char> path, Ptr<const u32> /*length*/) {
    nx::logd(logs::io, "fios scheduler for '{}' (of {}): the first", path ? str(path, 1024) : nx::string(), available);
    return 0;
}
i32 _sceFiosKernelOverlayThreadSetDisabled(i32 disabled) { return sceFiosOverlayThreadSetDisabled02(disabled); }
i32 _sceFiosKernelOverlayThreadIsDisabled() { return g_overlays_disabled.get() ? 1 : 0; }
i32 _sceFiosKernelOverlayAdd() { return unsupported("_sceFiosKernelOverlayAdd"); }
i32 _sceFiosKernelOverlayAddForProcess() { return unsupported("_sceFiosKernelOverlayAddForProcess"); }
i32 _sceFiosKernelOverlayModify() { return unsupported("_sceFiosKernelOverlayModify"); }
i32 _sceFiosKernelOverlayModifyForProcess() { return unsupported("_sceFiosKernelOverlayModifyForProcess"); }
i32 _sceFiosKernelOverlayRemove() { return unsupported("_sceFiosKernelOverlayRemove"); }
i32 _sceFiosKernelOverlayRemoveForProcess() { return unsupported("_sceFiosKernelOverlayRemoveForProcess"); }
i32 _sceFiosKernelOverlayGetInfo() { return unsupported("_sceFiosKernelOverlayGetInfo"); }
i32 _sceFiosKernelOverlayGetInfoForProcess() { return unsupported("_sceFiosKernelOverlayGetInfoForProcess"); }
// The overlay directory handles (DH*: a folder listed through the overlays, merged): their argument blocks are not
// known yet and nothing has called them, so each is refused as overlays are, naming itself.
i32 _sceFiosKernelOverlayDHOpenSync() { return unsupported("_sceFiosKernelOverlayDHOpenSync"); }
i32 _sceFiosKernelOverlayDHReadSync() { return unsupported("_sceFiosKernelOverlayDHReadSync"); }
i32 _sceFiosKernelOverlayDHCloseSync() { return unsupported("_sceFiosKernelOverlayDHCloseSync"); }
i32 _sceFiosKernelOverlayDHStatSync() { return unsupported("_sceFiosKernelOverlayDHStatSync"); }
i32 _sceFiosKernelOverlayDHChstatSync() { return unsupported("_sceFiosKernelOverlayDHChstatSync"); }
i32 _sceFiosKernelOverlayDHSyncSync() { return unsupported("_sceFiosKernelOverlayDHSyncSync"); }

const Function kFios2Kernel[] = {
    HLE_FUNCTION(0x9379E2D5, _sceFiosKernelOverlayGetList),
    HLE_FUNCTION(0xE9AE60FB, _sceFiosKernelOverlayResolveSync),
    HLE_FUNCTION(0x8CCA471A, _sceFiosKernelOverlayResolveWithRangeSync),
    HLE_FUNCTION(0xB02E0B26, _sceFiosKernelOverlayGetRecommendedScheduler),
    HLE_FUNCTION(0x3E9172EA, _sceFiosKernelOverlayThreadSetDisabled),
    HLE_FUNCTION(0x629F4FE4, _sceFiosKernelOverlayThreadIsDisabled),
    HLE_FUNCTION(0x6DBCF0B2, _sceFiosKernelOverlayAdd),
    HLE_FUNCTION(0x2A381357, _sceFiosKernelOverlayAddForProcess),
    HLE_FUNCTION(0x6D6CDE05, _sceFiosKernelOverlayModify),
    HLE_FUNCTION(0x6DF2FC05, _sceFiosKernelOverlayModifyForProcess),
    HLE_FUNCTION(0xB4927173, _sceFiosKernelOverlayRemove),
    HLE_FUNCTION(0xF8277E07, _sceFiosKernelOverlayRemoveForProcess),
    HLE_FUNCTION(0xF44F3505, _sceFiosKernelOverlayGetInfo),
    HLE_FUNCTION(0xBC6B3CC5, _sceFiosKernelOverlayGetInfoForProcess),
    HLE_FUNCTION(0x5D6A1CCE, _sceFiosKernelOverlayDHOpenSync),
    HLE_FUNCTION(0x2F06ADC6, _sceFiosKernelOverlayDHReadSync),
    HLE_FUNCTION(0x021B4AF7, _sceFiosKernelOverlayDHCloseSync),
    HLE_FUNCTION(0x759EBEE6, _sceFiosKernelOverlayDHStatSync),
    HLE_FUNCTION(0xF6A3E335, _sceFiosKernelOverlayDHChstatSync),
    HLE_FUNCTION(0x2A9724C9, _sceFiosKernelOverlayDHSyncSync),
};

const Function kFios2User[] = {
    HLE_FUNCTION(0x1DD808D1, sceFiosOverlayGetList02),
    HLE_FUNCTION(0x61C4AAC4, sceFiosOverlayResolveWithRangeSync02),
    HLE_FUNCTION(0x70321220, sceFiosOverlayThreadSetDisabled02),
    HLE_FUNCTION(0xF5C1F928, sceFiosOverlayGetRecommendedScheduler02),
    HLE_FUNCTION(0x6C4BE9CD, sceFiosOverlayAddForProcess02),
    HLE_FUNCTION(0xCA388053, sceFiosOverlayModifyForProcess02),
    HLE_FUNCTION(0xE5D1B6F5, sceFiosOverlayRemoveForProcess02),
    HLE_FUNCTION(0xAB7B4213, sceFiosOverlayGetInfoForProcess02),
    HLE_FUNCTION(0x9C3AAAF0, sceFiosOverlayResolveSync02),
    HLE_FUNCTION(0x23B8DB1D, sceFiosOverlayThreadIsDisabled02),
};

}  // namespace

void register_fios2() {
    register_library({"SceFios2User", kSceFios2User, kFios2User});
    register_library({"SceFios2Kernel", kSceFios2Kernel, kFios2Kernel});
}

}  // namespace psvrecore::runtime::hle
