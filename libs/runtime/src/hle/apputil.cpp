// The application utility (SceAppUtil): save data (SaveData, on the folder mounted as savedata0:), safe memory, the
// console's settings (Runtime::Config::system) and application events (none).
//
// savedata0: is the mount the platform layer gives the files (read-only to sceIo, as on the console: data is written
// only through sceAppUtilSaveDataDataSave). Without that mount the save data functions answer "not mounted". Other
// titles' save data (savedata1:) is never there: an application may mount only the titles its param.sfo lists.

#include <algorithm>
#include <optional>

#include "hle_common.hpp"
#include "psvrecore/runtime/host.hpp"
#include "psvrecore/runtime/savedata.hpp"
#include "core/foundation/platform/filesystem.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAppUtil = 0x48E01D74;
constexpr i32 kParameter = err::code(0x80100600), kNotInitialized = err::code(0x80100601), kBusy = err::code(0x80100603),
              kNotMounted = err::code(0x80100604);
constexpr u32 kSafeMemorySize = 64 * 1024;

using SlotParam = SaveData::SlotParam;

struct InitParam {
    u32 work_buf_size;
    u8 reserved[60];
};
struct BootParam {
    u32 attr, app_version;
    u8 reserved[32];
};
struct AppEventParam {
    u32 type;
    u8 data[1024];
};
struct MountPoint {
    char data[16];
};
struct DataSlot {
    u32 id;
    Ptr<SlotParam> param;
    u8 reserved[32];
};
static_assert(sizeof(DataSlot) == 40);
struct DataSaveItem {
    Ptr<const char> path;
    Ptr<const u8> buf;
    u32 buf_size;
    u8 padding[4];
    i64 offset;
    u32 mode;
    u8 reserved[36];
};
static_assert(sizeof(DataSaveItem) == 64);
struct DataRemoveItem {
    Ptr<const char> path;
    u32 mode;
    u8 reserved[36];
};
static_assert(sizeof(DataRemoveItem) == 44);
struct WorkBuffer {
    Ptr<u8> buf;
    u32 size;
    u8 reserved[32];
};
struct SearchCond {
    u32 type, from, range, key, order;
    u8 reserved[32];
};
struct SearchResult {
    u32 hits;
    u32 slot_list;
    u8 reserved[32];
};
struct SlotEntry {  // SceAppUtilSaveDataSlot
    u32 id, status;
    i32 user_param;
    u32 empty_param;
};
static_assert(sizeof(SlotEntry) == 16);

struct AppUtil {
    nx::mutex lock;
    bool initialized = false;
    std::optional<SaveData> save;
    u8 safe_memory[kSafeMemorySize] = {};
    bool safe_memory_loaded = false;
};

AppUtil& apputil() {
    static AppUtil a;
    return a;
}

// The save data of savedata0: (made on first use), or null when it is not mounted.
SaveData* save_data(AppUtil& a) {
    if (a.save) return a.save->ok() ? &*a.save : nullptr;
    nx::string host_path;
    if (runtime().files().resolve("savedata0:", &host_path, nullptr) < 0) return nullptr;
    a.save.emplace(std::move(host_path));
    return a.save->ok() ? &*a.save : nullptr;
}

// NULL or "savedata0:".
bool own_mount_point(Ptr<const MountPoint> mp) {
    if (!mp) return true;
    if (!memory().valid(mp.address, sizeof(MountPoint))) return false;
    return nx::string_view(mp->data, strnlen(mp->data, sizeof mp->data)) == "savedata0:";
}

// The common start of the save data functions: the library initialized, the mount point ours and mounted.
i32 enter(AppUtil& a, Ptr<const MountPoint> mp, SaveData** out) {
    if (!a.initialized) return kNotInitialized;
    if (!own_mount_point(mp)) return kParameter;
    *out = save_data(a);
    return *out ? 0 : kNotMounted;
}

bool valid_utf8(const char* s, usize size, bool allow_newline) {
    usize i = 0;
    while (i < size) {
        const u8 c = static_cast<u8>(s[i]);
        if (c == '\n' && !allow_newline) return false;
        const usize n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > size) return false;
        for (usize k = 1; k < n; ++k)
            if ((static_cast<u8>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// A slot's parameters as the guest gave them, checked: terminated strings, UTF-8, no line feed in the titles, the
// icon path of [A-Za-z0-9-_./:].
bool read_param(Ptr<SlotParam> p, SlotParam* out) {
    if (!p || !memory().valid(p.address, sizeof(SlotParam))) return false;
    std::memcpy(out, p.get(), sizeof(SlotParam));
    auto text = [](const char* s, usize max, bool newline) {
        const usize n = strnlen(s, max);
        return n < max && valid_utf8(s, n, newline);
    };
    if (!text(out->title, sizeof out->title, false) || !text(out->subtitle, sizeof out->subtitle, false) ||
        !text(out->detail, sizeof out->detail, true))
        return false;
    const usize n = strnlen(out->icon_path, sizeof out->icon_path);
    if (n == sizeof out->icon_path) return false;
    for (usize i = 0; i < n; ++i) {
        const char c = out->icon_path[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == '/' || c == ':'))
            return false;
    }
    return true;
}

// A data path the guest gave (inside savedata0:), or empty when it is not one.
nx::string data_path(Ptr<const char> p) {
    if (!p) return {};
    nx::string s = str(p, SaveData::kPathMax + 1);
    return SaveData::valid_path(s) ? s : nx::string{};
}

i32 sceAppUtilInit(Ptr<const InitParam> init, Ptr<BootParam> boot) {
    if (!init || !boot || !memory().valid(init.address, sizeof(InitParam)) || !writable(boot.address, sizeof(BootParam)))
        return kParameter;
    if (init->work_buf_size != 0) return kParameter;
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (a.initialized) return kBusy;
    boot->attr = 0;
    boot->app_version = 0;
    a.initialized = true;
    return 0;
}

i32 sceAppUtilShutdown() {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (!a.initialized) return kNotInitialized;
    a.initialized = false;
    return 0;
}

// No events: the application was started from its LiveArea and nothing has come since.
i32 sceAppUtilReceiveAppEvent(Ptr<AppEventParam> event) {
    if (!apputil().initialized) return kNotInitialized;
    if (!event || !writable(event.address, sizeof(AppEventParam))) return kParameter;
    std::memset(event.get(), 0, sizeof(AppEventParam));
    return 0;
}

i32 sceAppUtilSaveDataSlotCreate(u32 id, Ptr<SlotParam> param, Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    SlotParam p;
    if (id >= SaveData::kSlots || !read_param(param, &p)) return kParameter;
    Kernel::Blocking blocking(kernel());
    return s->create_slot(id, p);
}

i32 sceAppUtilSaveDataSlotDelete(u32 id, Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if (id >= SaveData::kSlots) return kParameter;
    Kernel::Blocking blocking(kernel());
    return s->delete_slot(id);
}

i32 sceAppUtilSaveDataSlotSetParam(u32 id, Ptr<SlotParam> param, Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    SlotParam p;
    if (id >= SaveData::kSlots || !read_param(param, &p)) return kParameter;
    Kernel::Blocking blocking(kernel());
    return s->set_slot(id, p);
}

i32 sceAppUtilSaveDataSlotGetParam(u32 id, Ptr<SlotParam> param, Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if (id >= SaveData::kSlots || !param || !writable(param.address, sizeof(SlotParam))) return kParameter;
    SlotParam p;
    if (!s->slot(id, &p)) return SaveData::kSlotNotFound;
    *param = p;
    return 0;
}

i32 sceAppUtilSaveDataSlotSearch(Ptr<WorkBuffer> work, Ptr<const SearchCond> cond, Ptr<SearchResult> result,
                                 Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if (!cond || !memory().valid(cond.address, sizeof(SearchCond)) || !result ||
        !writable(result.address, sizeof(SearchResult)) || (work && !memory().valid(work.address, sizeof(WorkBuffer))))
        return kParameter;
    const SearchCond c = *cond;
    if (c.type > 1 || c.key > 3 || c.order > 1 || (c.type == 1 && c.key != 0) || c.from >= SaveData::kSlots)
        return kParameter;
    struct Hit {
        u32 id;
        SlotParam param;
    };
    nx::vector<Hit> hits;
    const u32 last = nx::min<u32>(SaveData::kSlots, c.from + nx::min<u32>(c.range, SaveData::kSlots));
    for (u32 id = c.from; id < last; ++id) {
        Hit h{id, {}};
        const bool used = s->slot(id, &h.param);
        if (used == (c.type == 0)) hits.push_back(h);
    }
    auto key = [&](const Hit& h) -> u64 {
        switch (c.key) {
            case 1: return static_cast<u64>(static_cast<i64>(h.param.user_param) + 0x80000000ll);
            case 2: return h.param.size_kib;
            case 3: {
                const io::DateTime& t = h.param.modified;
                return ((((static_cast<u64>(t.year) * 13 + t.month) * 32 + t.day) * 24 + t.hour) * 60 + t.minute) * 60 *
                           1'000'000ull +
                       t.second * 1'000'000ull + t.microsecond;
            }
            default: return h.id;
        }
    };
    std::stable_sort(hits.begin(), hits.end(), [&](const Hit& x, const Hit& y) {
        return c.order == 0 ? key(x) < key(y) : key(x) > key(y);
    });
    result->hits = static_cast<u32>(hits.size());
    result->slot_list = 0;
    if (work && work->buf) {
        // (the work buffer holds the list; its size is asked for per entry by the sort key)
        const u32 element = c.key == 2 ? 20 : c.key == 3 ? 32 : 16;
        const u32 n = nx::min<u32>(static_cast<u32>(hits.size()), work->size / element);
        if (!writable(work->buf.address, n * sizeof(SlotEntry))) return kParameter;
        auto* out = reinterpret_cast<SlotEntry*>(work->buf.get());
        for (u32 i = 0; i < n; ++i) out[i] = SlotEntry{hits[i].id, hits[i].param.status, hits[i].param.user_param, 0};
        result->slot_list = work->buf.address;
    }
    return 0;
}

// The slot a save or removal names, checked (none: no slot).
bool read_slot(Ptr<const DataSlot> slot, SaveData::SlotUpdate* out, SlotParam* param, bool* given) {
    *given = false;
    if (!slot) return true;
    if (!memory().valid(slot.address, sizeof(DataSlot)) || slot->id >= SaveData::kSlots) return false;
    *given = true;
    out->id = slot->id;
    out->param = nullptr;
    if (slot->param) {
        if (!read_param(slot->param, param)) return false;
        out->param = param;
    }
    return true;
}

i32 sceAppUtilSaveDataDataSave(Ptr<const DataSlot> slot, Ptr<const DataSaveItem> data, u32 count,
                               Ptr<const MountPoint> mp, Ptr<u32> required_kib) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if (!required_kib || !writable(required_kib.address, 4) || !data || count < 1 || count > SaveData::kMaxItems ||
        !memory().valid(data.address, count * sizeof(DataSaveItem)))
        return kParameter;
    SaveData::SlotUpdate update{};
    SlotParam param;
    bool has_slot;
    if (!read_slot(slot, &update, &param, &has_slot)) return kParameter;
    nx::vector<SaveData::SaveItem> items;
    for (u32 i = 0; i < count; ++i) {
        const DataSaveItem& d = data.get()[i];
        SaveData::SaveItem item;
        item.path = data_path(d.path);
        if (item.path.empty() || d.offset < 0 || d.mode > SaveData::kDirectory) return kParameter;
        if (d.mode == SaveData::kDirectory && (d.buf || d.buf_size || d.offset)) return kParameter;
        if (d.buf_size) {
            if (!d.buf || !memory().valid(d.buf.address, d.buf_size)) return kParameter;
            item.data = std::span<const u8>(d.buf.get(), d.buf_size);
        }
        item.offset = static_cast<u64>(d.offset);
        item.mode = static_cast<SaveData::SaveMode>(d.mode);
        items.push_back(std::move(item));
    }
    u32 required = 0;
    i32 r;
    {
        Kernel::Blocking blocking(kernel());
        r = s->save(items, has_slot ? &update : nullptr, &required);
    }
    *required_kib = required;
    nx::logd(logs::io, "sceAppUtilSaveDataDataSave({} items, '{}'...) = 0x{:08x}", count, items[0].path,
             static_cast<u32>(r));
    return r;
}

i32 sceAppUtilSaveDataDataRemove(Ptr<const DataSlot> slot, Ptr<const DataRemoveItem> data, u32 count,
                                 Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if (!data || count < 1 || count > SaveData::kMaxItems || !memory().valid(data.address, count * sizeof(DataRemoveItem)))
        return kParameter;
    SaveData::SlotUpdate update{};
    SlotParam param;
    bool has_slot;
    if (!read_slot(slot, &update, &param, &has_slot)) return kParameter;
    nx::vector<nx::string> paths;
    bool keep_slot = false;
    for (u32 i = 0; i < count; ++i) {
        const DataRemoveItem& d = data.get()[i];
        nx::string path = data_path(d.path);
        if (path.empty() || d.mode > 1) return kParameter;
        keep_slot |= d.mode == 1;  // (one item keeping the slot keeps it)
        paths.push_back(std::move(path));
    }
    update.remove = !keep_slot;
    Kernel::Blocking blocking(kernel());
    return s->remove(paths, has_slot ? &update : nullptr);
}

i32 sceAppUtilSaveDataGetQuota(Ptr<u32> quota_kib, Ptr<u32> used_kib, Ptr<const MountPoint> mp) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    SaveData* s;
    if (const i32 r = enter(a, mp, &s)) return r;
    if ((!quota_kib && !used_kib) || (quota_kib && !writable(quota_kib.address, 4)) ||
        (used_kib && !writable(used_kib.address, 4)))
        return kParameter;
    if (quota_kib) *quota_kib = static_cast<u32>(SaveData::kQuotaKiB);
    if (used_kib) {
        Kernel::Blocking blocking(kernel());
        *used_kib = static_cast<u32>(s->used_kib());
    }
    return 0;
}

// Other titles' save data: none may be mounted (none is listed in the application's param.sfo).
i32 sceAppUtilSaveDataMount(Ptr<const void>, Ptr<const void>) {
    return apputil().initialized ? kParameter : kNotInitialized;
}
i32 sceAppUtilSaveDataUmount() { return apputil().initialized ? kNotMounted : kNotInitialized; }

// ---- safe memory

void load_safe_memory(AppUtil& a) {
    if (a.safe_memory_loaded) return;
    a.safe_memory_loaded = true;
    const nx::string& path = runtime().config().system.safe_memory;
    if (path.empty() || !nx::fs::exists(nx::fs::path_view(path))) return;
    if (auto data = nx::fs::file_read(nx::fs::path_view(path)))
        std::memcpy(a.safe_memory, data->data(), nx::min<usize>(data->size(), kSafeMemorySize));
}

i32 sceAppUtilSaveSafeMemory(Ptr<const u8> buf, u32 size, i64 offset) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (!a.initialized) return kNotInitialized;
    if (!buf || offset < 0 || static_cast<u64>(offset) + size > kSafeMemorySize || !memory().valid(buf.address, size))
        return kParameter;
    load_safe_memory(a);
    std::memcpy(a.safe_memory + offset, buf.get(), size);
    const nx::string& path = runtime().config().system.safe_memory;
    if (!path.empty()) {
        Kernel::Blocking blocking(kernel());
        if (!nx::fs::file_write_atomic(nx::fs::path_view(path), std::span<const u8>(a.safe_memory, kSafeMemorySize)))
            nx::loge(logs::io, "safe memory: cannot write '{}'", path);
    }
    return static_cast<i32>(size);
}

i32 sceAppUtilLoadSafeMemory(Ptr<u8> buf, u32 size, i64 offset) {
    AppUtil& a = apputil();
    nx::scoped_lock<nx::mutex> held(a.lock);
    if (!a.initialized) return kNotInitialized;
    if (!buf || offset < 0 || static_cast<u64>(offset) + size > kSafeMemorySize || !writable(buf.address, size))
        return kParameter;
    load_safe_memory(a);
    std::memcpy(buf.get(), a.safe_memory + offset, size);
    return static_cast<i32>(size);
}

// ---- parameters

i32 sceAppUtilSystemParamGetInt(u32 id, Ptr<i32> value) {
    if (!apputil().initialized) return kNotInitialized;
    if (!value || !writable(value.address, 4)) return kParameter;
    const auto& sys = runtime().config().system;
    switch (id) {
        case 1: *value = sys.language; return 0;
        case 4: *value = sys.date_format; return 0;
        case 5: *value = sys.time_format; return 0;
        case 6: *value = sys.time_zone_minutes; return 0;
        case 7: *value = sys.summer_time ? 1 : 0; return 0;
        default: return kParameter;
    }
}

i32 sceAppUtilSystemParamGetString(u32 id, Ptr<char> buf, u32 size) {
    if (!apputil().initialized) return kNotInitialized;
    constexpr u32 kUserNameMax = 17;
    if (id != 3 || !buf || size < kUserNameMax || !writable(buf.address, size)) return kParameter;
    const nx::string& name = runtime().config().system.user_name;
    // (up to 16 characters: whole UTF-8 sequences that fit 16 bytes)
    usize n = nx::min<usize>(name.size(), kUserNameMax - 1);
    while (n > 0 && n < name.size() && (static_cast<u8>(name[n]) & 0xC0) == 0x80) --n;
    std::memcpy(buf.get(), name.data(), n);
    buf.get()[n] = 0;
    return 0;
}

// The SKU flag: none (an application sold one way).
i32 sceAppUtilAppParamGetInt(u32 id, Ptr<i32> value) {
    if (!apputil().initialized) return kNotInitialized;
    if (id != 0 || !value || !writable(value.address, 4)) return kParameter;
    *value = 0;
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0xDAFFE671, sceAppUtilInit),
    HLE_FUNCTION(0xB220B00B, sceAppUtilShutdown),
    HLE_FUNCTION(0xEE0DBED9, sceAppUtilReceiveAppEvent),
    HLE_FUNCTION(0x7E8FE96A, sceAppUtilSaveDataSlotCreate),
    HLE_FUNCTION(0x266A7646, sceAppUtilSaveDataSlotDelete),
    HLE_FUNCTION(0x98630136, sceAppUtilSaveDataSlotSetParam),
    HLE_FUNCTION(0x93F0D89F, sceAppUtilSaveDataSlotGetParam),
    HLE_FUNCTION(0xE6057A85, sceAppUtilSaveDataSlotSearch),
    HLE_FUNCTION(0x607647BA, sceAppUtilSaveDataDataSave),
    HLE_FUNCTION(0xD1C6AB8E, sceAppUtilSaveDataDataRemove),
    HLE_FUNCTION(0xC560E716, sceAppUtilSaveDataGetQuota),
    HLE_FUNCTION(0xE61453B0, sceAppUtilSaveDataMount),
    HLE_FUNCTION(0xEB720402, sceAppUtilSaveDataUmount),
    HLE_FUNCTION(0x9D8AC677, sceAppUtilSaveSafeMemory),
    HLE_FUNCTION(0x3424D772, sceAppUtilLoadSafeMemory),
    HLE_FUNCTION(0x5DFB9CA0, sceAppUtilSystemParamGetInt),
    HLE_FUNCTION(0x6E6AA267, sceAppUtilSystemParamGetString),
    HLE_FUNCTION(0xCD7FD67A, sceAppUtilAppParamGetInt),
};

}  // namespace

void register_apputil() {
    // (a new runtime starts with the library not initialized, its save data and safe memory read again)
    AppUtil& a = apputil();
    a.initialized = false;
    a.save.reset();
    a.safe_memory_loaded = false;
    std::memset(a.safe_memory, 0, sizeof a.safe_memory);
    register_library({"SceAppUtil", kSceAppUtil, kFunctions});
}

}  // namespace psvrecore::runtime::hle
