// The system side of libfios2's overlays (SceFios2User). libfios2 (a module the system preloads for every game,
// recompiled like the game) asks the system which overlays (path remappings) the process has and resolves paths
// through them. The process starts with none; the game adds its own (sceFiosOverlayAdd: an archive mounted by
// libfios2's dearchiver laid over app0:, say), kept here: listed, described, changed, removed, and applied when a
// path is resolved.
//
// An overlay maps a destination path prefix (`dst`) to a source prefix (`src`). Resolving a path goes through the
// overlays in their order (lowest first, then the oldest first), each one whose destination is the path or a folder
// above it giving the path under its source: always (opaque), when that exists (translucent; and for reads,
// writable, which writes always take), or when that exists and is not older than the path it replaces (newer). The
// path so far goes on to the next overlay.
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

#include <algorithm>
#include <cstring>
#include <tuple>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceFios2User = 0xC2ED82D7;

// SceFiosOverlay, as the guest lays it out.
constexpr u32 kPathSize = 292;  // (SCE_FIOS2_OVERLAY_PATH_SIZE: with the terminator)
struct GuestOverlay {
    u8 type;
    u8 order;
    u16 dst_length, src_length;
    u16 pad;
    i32 process;
    i32 id;
    char dst[kPathSize];
    char src[kPathSize];
};
static_assert(sizeof(GuestOverlay) == 0x258);

enum : u8 { kOpaque = 0, kTranslucent = 1, kNewer = 2, kWritable = 3 };
constexpr i32 kBadOverlay = err::code(0x80010016);   // (SCE_ERROR_ERRNO_EINVAL)
constexpr i32 kNoOverlay = err::code(0x80010002);    // (SCE_ERROR_ERRNO_ENOENT)
constexpr i32 kNameTooLong = err::code(0x8001005B);  // (SCE_ERROR_ERRNO_ENAMETOOLONG)

struct Overlay {
    i32 id = 0;
    u8 type = 0, order = 0;
    nx::string dst, src;
};
nx::mutex g_lock;
nx::vector<Overlay> g_overlays;  // (in the order they are applied)
i32 g_next_id = 1;

// A path as an overlay compares it: without a trailing '/' (but "app0:/" stays itself).
nx::string trimmed(nx::string_view path) {
    while (path.size() > 1 && path.back() == '/' && path[path.size() - 2] != ':') path.remove_suffix(1);
    return nx::string(path);
}

// The path under the overlay's source that `path` is under its destination, if it is.
bool remapped(const Overlay& o, nx::string_view path, nx::string* out) {
    const nx::string_view dst = o.dst.view();
    if (path.size() < dst.size() || path.substr(0, dst.size()) != dst) return false;
    nx::string_view rest = path.substr(dst.size());
    const bool dst_ends = dst.back() == '/' || dst.back() == ':';
    if (!rest.empty() && rest.front() != '/' && !dst_ends) return false;  // ("app0:/a" is not over "app0:/ab")
    nx::string result = o.src;
    const bool src_ends = !result.empty() && (result.back() == '/' || result.back() == ':');
    if (!rest.empty() && rest.front() == '/' && src_ends) rest.remove_prefix(1);
    else if (!rest.empty() && rest.front() != '/' && !src_ends && !result.empty()) result += '/';
    result += rest;
    *out = std::move(result);
    return true;
}

bool not_older(const io::DateTime& a, const io::DateTime& b) {
    return std::tie(a.year, a.month, a.day, a.hour, a.minute, a.second, a.microsecond) >=
           std::tie(b.year, b.month, b.day, b.hour, b.minute, b.second, b.microsecond);
}

nx::string resolve(nx::string path, bool for_write, u32 min_order, u32 max_order) {
    nx::vector<Overlay> overlays;
    {
        nx::scoped_lock<nx::mutex> guard(g_lock);
        overlays = g_overlays;
    }
    FileSystem& files = runtime().files();
    for (const Overlay& o : overlays) {
        if (o.order < min_order || o.order > max_order) continue;
        nx::string candidate;
        if (!remapped(o, path.view(), &candidate)) continue;
        bool use = false;
        io::Stat source{}, destination{};
        switch (o.type) {
            case kOpaque: use = true; break;
            case kWritable: use = for_write || files.stat(candidate.view(), &source) >= 0; break;
            case kNewer:
                use = files.stat(candidate.view(), &source) >= 0 &&
                      (files.stat(path.view(), &destination) < 0 || not_older(source.mtime, destination.mtime));
                break;
            default: use = files.stat(candidate.view(), &source) >= 0; break;  // (translucent)
        }
        if (use) path = std::move(candidate);
    }
    return path;
}

i32 resolve_into(i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length, u32 min_order, u32 max_order) {
    const nx::string path = str(in, 1024);
    const nx::string resolved = resolve(path, (flags & 1) != 0, min_order, max_order);
    nx::logd(logs::io, "fios overlay resolve '{}' (flags 0x{:x}, orders {}-{}): '{}'", path, static_cast<u32>(flags),
             min_order, max_order, resolved);
    if (!out || max_length == 0) return err::kOk;
    if (!writable(out.address, max_length)) return err::kFault;
    if (resolved.size() + 1 > max_length) return kNameTooLong;
    std::memcpy(out.get(), resolved.data(), resolved.size());
    out.get()[resolved.size()] = 0;
    return err::kOk;
}

i32 overlay_list(u32 min_order, u32 max_order, Ptr<i32> ids, u32 max_ids, Ptr<u32> actual) {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    u32 n = 0;
    for (const Overlay& o : g_overlays) {
        if (o.order < min_order || o.order > max_order) continue;
        if (n < max_ids && ids && writable(ids.address + 4 * n, 4)) ids.get()[n] = o.id;
        ++n;
    }
    if (actual && writable(actual.address, 4)) *actual = std::min(n, max_ids);
    return err::kOk;
}

// An overlay as the guest gives it, checked.
i32 read_overlay(Ptr<const GuestOverlay> in, Overlay* out) {
    if (!in || !readable(in.address, sizeof(GuestOverlay))) return err::kFault;
    const GuestOverlay& g = *in;
    if (g.type > kWritable) return kBadOverlay;
    const auto field = [](const char* s) { return nx::string(s, strnlen(s, kPathSize)); };
    out->type = g.type, out->order = g.order;
    out->dst = trimmed(field(g.dst).view()), out->src = trimmed(field(g.src).view());
    if (out->dst.size() >= kPathSize || out->src.size() >= kPathSize) return kNameTooLong;
    if (out->dst.empty()) return kBadOverlay;
    return err::kOk;
}

void sort_overlays() {
    std::stable_sort(g_overlays.begin(), g_overlays.end(), [](const Overlay& a, const Overlay& b) {
        return a.order != b.order ? a.order < b.order : a.id < b.id;
    });
}

i32 overlay_add(i32 process, Ptr<const GuestOverlay> in, Ptr<i32> out_id) {
    Overlay o;
    if (const i32 r = read_overlay(in, &o); r < 0) return r;
    nx::scoped_lock<nx::mutex> guard(g_lock);
    o.id = g_next_id++;
    nx::logi(logs::io, "fios overlay {} added (process 0x{:x}): '{}' over '{}' (type {}, order {})", o.id,
             static_cast<u32>(process), o.src, o.dst, o.type, o.order);
    if (out_id && writable(out_id.address, 4)) *out_id = o.id;
    g_overlays.push_back(std::move(o));
    sort_overlays();
    return err::kOk;
}

i32 overlay_modify(i32 id, Ptr<const GuestOverlay> in) {
    Overlay o;
    if (const i32 r = read_overlay(in, &o); r < 0) return r;
    nx::scoped_lock<nx::mutex> guard(g_lock);
    for (Overlay& e : g_overlays)
        if (e.id == id) {
            o.id = id;
            e = std::move(o);
            sort_overlays();
            return err::kOk;
        }
    return kNoOverlay;
}

i32 overlay_remove(i32 id) {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    for (auto it = g_overlays.begin(); it != g_overlays.end(); ++it)
        if (it->id == id) {
            nx::logi(logs::io, "fios overlay {} removed", id);
            g_overlays.erase(it);
            return err::kOk;
        }
    return kNoOverlay;
}

i32 overlay_info(i32 process, i32 id, Ptr<GuestOverlay> out) {
    if (!out || !writable(out.address, sizeof(GuestOverlay))) return err::kFault;
    nx::scoped_lock<nx::mutex> guard(g_lock);
    for (const Overlay& o : g_overlays)
        if (o.id == id) {
            GuestOverlay g{};
            g.type = o.type, g.order = o.order;
            g.dst_length = static_cast<u16>(o.dst.size()), g.src_length = static_cast<u16>(o.src.size());
            g.process = process, g.id = o.id;
            std::memcpy(g.dst, o.dst.data(), o.dst.size());
            std::memcpy(g.src, o.src.data(), o.src.size());
            *out = g;
            return err::kOk;
        }
    return kNoOverlay;
}

i32 sceFiosOverlayGetList02(i32 /*process*/, u32 min_order, u32 max_order, Ptr<i32> ids, u32 max_ids, Ptr<u32> actual) {
    return overlay_list(min_order, max_order, ids, max_ids, actual);
}

i32 sceFiosOverlayResolveWithRangeSync02(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length,
                                         u32 min_order, u32 max_order) {
    return resolve_into(flags, in, out, max_length, min_order, max_order);
}

i32 sceFiosOverlayResolveSync02(i32 /*process*/, i32 flags, Ptr<const char> in, Ptr<char> out, u32 max_length) {
    return resolve_into(flags, in, out, max_length, 0, 0xFF);
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
    nx::logw(logs::io, "{}: not supported", what);
    return err::kUnsupported;
}
i32 sceFiosOverlayAddForProcess02(i32 process, Ptr<const GuestOverlay> in, Ptr<i32> out_id) {
    return overlay_add(process, in, out_id);
}
i32 sceFiosOverlayModifyForProcess02(i32 /*process*/, i32 id, Ptr<const GuestOverlay> in) {
    return overlay_modify(id, in);
}
i32 sceFiosOverlayRemoveForProcess02(i32 /*process*/, i32 id) { return overlay_remove(id); }
i32 sceFiosOverlayGetInfoForProcess02(i32 process, i32 id, Ptr<GuestOverlay> out) {
    return overlay_info(process, id, out);
}

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
    return resolve_into(flags, in, a.out, a.max_length, 0, 0xFF);
}
i32 _sceFiosKernelOverlayResolveWithRangeSync(i32 /*process*/, i32 flags, Ptr<const char> in,
                                              Ptr<ResolveWithRangeArgs> args) {
    if (!args || !readable(args.address, sizeof(ResolveWithRangeArgs))) return err::kFault;
    const ResolveWithRangeArgs a = *args;
    return resolve_into(flags, in, a.out, a.max_length, a.min_order, a.max_order);
}
// Which of `available` I/O schedulers suits a path (its length in the block): libfios2 runs a path on its second
// scheduler when the answer is 1. The host's files are all on one device: the first.
i32 _sceFiosKernelOverlayGetRecommendedScheduler(i32 available, Ptr<const char> path, Ptr<const u32> /*length*/) {
    nx::logd(logs::io, "fios scheduler for '{}' (of {}): the first", path ? str(path, 1024) : nx::string(), available);
    return 0;
}
i32 _sceFiosKernelOverlayThreadSetDisabled(i32 disabled) { return sceFiosOverlayThreadSetDisabled02(disabled); }
i32 _sceFiosKernelOverlayThreadIsDisabled() { return g_overlays_disabled.get() ? 1 : 0; }
// (as the kernel's API: Add(overlay, *id), AddForProcess(process, overlay, *id), Modify(id, overlay),
// ModifyForProcess(process, id, overlay), Remove(id), RemoveForProcess(process, id), GetInfo(id, *overlay),
// GetInfoForProcess(process, id, *overlay); libfios2's wrappers pass their arguments through)
i32 _sceFiosKernelOverlayAdd(Ptr<const GuestOverlay> in, Ptr<i32> out_id) { return overlay_add(0, in, out_id); }
i32 _sceFiosKernelOverlayAddForProcess(i32 process, Ptr<const GuestOverlay> in, Ptr<i32> out_id) {
    return overlay_add(process, in, out_id);
}
i32 _sceFiosKernelOverlayModify(i32 id, Ptr<const GuestOverlay> in) { return overlay_modify(id, in); }
i32 _sceFiosKernelOverlayModifyForProcess(i32 /*process*/, i32 id, Ptr<const GuestOverlay> in) {
    return overlay_modify(id, in);
}
i32 _sceFiosKernelOverlayRemove(i32 id) { return overlay_remove(id); }
i32 _sceFiosKernelOverlayRemoveForProcess(i32 /*process*/, i32 id) { return overlay_remove(id); }
i32 _sceFiosKernelOverlayGetInfo(i32 id, Ptr<GuestOverlay> out) { return overlay_info(0, id, out); }
i32 _sceFiosKernelOverlayGetInfoForProcess(i32 process, i32 id, Ptr<GuestOverlay> out) {
    return overlay_info(process, id, out);
}
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
