// The system side of libfios2's overlays (SceFios2User). libfios2 (a module the system preloads for every game,
// recompiled like the game) asks the system which overlays (path remappings) the process has and resolves paths
// through them. Overlays a game adds are not supported yet: the process has none, so a path resolves to itself.
//
// The functions' arguments are as libfios2's wrappers pass them (its own API, documented in the SDK, plus the process
// in front): GetList02(process, min order, max order, ids, max ids, *actual count) and
// ResolveWithRangeSync02(process, flags, in path, out path, max length, min order, max order).

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

i32 sceFiosOverlayResolveWithRangeSync02(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length,
                                         u32 /*min_order*/, u32 /*max_order*/) {
    const nx::string path = str(in, 1024);
    nx::logd(logs::io, "fios overlay resolve '{}' (flags 0x{:x}): itself", path, static_cast<u32>(flags));
    if (!out || max_length == 0) return err::kOk;
    if (!writable(out.address, max_length)) return err::kFault;
    const u32 n = std::min<u32>(max_length - 1, static_cast<u32>(path.size()));
    std::memcpy(out.get(), path.data(), n);
    out.get()[n] = 0;
    return err::kOk;
}

i32 sceFiosOverlayThreadSetDisabled02(i32 disabled) {
    nx::logd(logs::io, "fios overlays for this thread {}", disabled ? "off" : "on");
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

const Function kFios2User[] = {
    HLE_FUNCTION(0x1DD808D1, sceFiosOverlayGetList02),
    HLE_FUNCTION(0x61C4AAC4, sceFiosOverlayResolveWithRangeSync02),
    HLE_FUNCTION(0x70321220, sceFiosOverlayThreadSetDisabled02),
    HLE_FUNCTION(0x6C4BE9CD, sceFiosOverlayAddForProcess02),
    HLE_FUNCTION(0xCA388053, sceFiosOverlayModifyForProcess02),
    HLE_FUNCTION(0xE5D1B6F5, sceFiosOverlayRemoveForProcess02),
    HLE_FUNCTION(0xAB7B4213, sceFiosOverlayGetInfoForProcess02),
};

}  // namespace

void register_fios2() { register_library({"SceFios2User", kSceFios2User, kFios2User}); }

}  // namespace psvrecore::runtime::hle
