// Save data and the rest of the application utility (SceAppUtil): slots, saves written all or none, removal, the
// quota's count, savedata0: read-only to sceIo, a save cut short, safe memory, the system's settings, and add-ons
// (addcont0:, opened and closed).

#include <algorithm>
#include <cstring>

#include "nxtest.h"
#include "runtime_support.hpp"
#include "psvrecore/runtime/host.hpp"
#include "psvrecore/runtime/savedata.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

constexpr u32 kSceAppUtil = 0x48E01D74;
constexpr u32 kInit = 0xDAFFE671, kSlotCreate = 0x7E8FE96A, kSlotDelete = 0x266A7646, kSlotSetParam = 0x98630136,
              kSlotGetParam = 0x93F0D89F, kSlotSearch = 0xE6057A85, kDataSave = 0x607647BA, kDataRemove = 0xD1C6AB8E,
              kGetQuota = 0xC560E716, kSaveSafe = 0x9D8AC677, kLoadSafe = 0x3424D772, kSysInt = 0x5DFB9CA0,
              kSysString = 0x6E6AA267;
constexpr i32 kParameter = static_cast<i32>(0x80100600u), kNotInitialized = static_cast<i32>(0x80100601u),
              kBusy = static_cast<i32>(0x80100603u), kNotMounted = static_cast<i32>(0x80100604u);

constexpr u32 kOpen = 0x6C60AC61, kClose = 0xC70B8886, kRead = 0xFDB32293, kGetstat = 0xBCA5B623;

u32 u(i32 v) { return static_cast<u32>(v); }

struct TempDir {
    nx::string path;
    TempDir() {
        static u32 counter = 0;
        nx::string base = "/tmp";
        if (auto t = nx::os::environment_variable("TEMP")) base = *t;
        else if (auto t2 = nx::os::environment_variable("TMPDIR")) base = *t2;
        path = nx::format("{}/psvrecore_save_{}_{}", base, nx::os::process_id(), counter++);
        (void)nx::fs::remove_all(nx::fs::path_view(path));
        (void)nx::fs::create_directories(nx::fs::path_view(path));
    }
    ~TempDir() { (void)nx::fs::remove_all(nx::fs::path_view(path)); }
};

u32 init(PsvContext* ctx) {
    const u32 p = scratch(64 + 40);
    std::memset(current_runtime()->memory().host(p), 0, 104);
    return static_cast<u32>(call32(ctx, kSceAppUtil, kInit, {p, p + 64}));
}

u32 slot_param(const char* title, i32 user_param = 0) {
    const u32 p = scratch(sizeof(SaveData::SlotParam));
    auto* s = current_runtime()->memory().host_as<SaveData::SlotParam>(p);
    std::memset(s, 0, sizeof *s);
    std::strcpy(s->title, title);
    std::strcpy(s->subtitle, "Chapter 1");
    std::strcpy(s->detail, "Line one\nline two");
    std::strcpy(s->icon_path, "savedata0:sce_sys/icon0.png");
    s->user_param = user_param;
    return p;
}

SaveData::SlotParam get_param(PsvContext* ctx, u32 id, i32* result) {
    const u32 p = scratch(sizeof(SaveData::SlotParam));
    *result = call32(ctx, kSceAppUtil, kSlotGetParam, {id, p, 0});
    return *current_runtime()->memory().host_as<SaveData::SlotParam>(p);
}

// SceAppUtilSaveDataDataSaveItem items in guest memory.
struct Item {
    const char* path;
    u32 buf;
    u32 size;
    u64 offset;
    u32 mode;
};
u32 save_items(std::initializer_list<Item> items) {
    const u32 at = scratch(64 * static_cast<u32>(items.size()));
    u8* p = current_runtime()->memory().host(at);
    std::memset(p, 0, 64 * items.size());
    for (const Item& i : items) {
        const u32 path = guest_string(i.path);
        std::memcpy(p, &path, 4);
        std::memcpy(p + 4, &i.buf, 4);
        std::memcpy(p + 8, &i.size, 4);
        std::memcpy(p + 16, &i.offset, 8);
        std::memcpy(p + 24, &i.mode, 4);
        p += 64;
    }
    return at;
}
u32 data_slot(u32 id, u32 param) {
    const u32 at = scratch(40);
    u8* p = current_runtime()->memory().host(at);
    std::memset(p, 0, 40);
    std::memcpy(p, &id, 4);
    std::memcpy(p + 4, &param, 4);
    return at;
}
u32 bytes(std::initializer_list<u8> b) {
    const u32 at = scratch(static_cast<u32>(b.size()));
    std::memcpy(current_runtime()->memory().host(at), b.begin(), b.size());
    return at;
}
u32 filled(u32 size, u8 value) {
    const u32 at = scratch(size);
    std::memset(current_runtime()->memory().host(at), value, size);
    return at;
}

// A guest file's size through sceIo (-1: missing).
i64 guest_size(PsvContext* ctx, const char* path) {
    const u32 st = scratch(sizeof(runtime::io::Stat));
    if (call32(ctx, kSceLibKernel, kGetstat, {guest_string(path), st}) != 0) return -1;
    return current_runtime()->memory().host_as<runtime::io::Stat>(st)->size;
}
nx::vector<u8> guest_read(PsvContext* ctx, const char* path) {
    const i32 fd = call32(ctx, kSceLibKernel, kOpen, {guest_string(path), 1, 0});
    nx::vector<u8> out;
    if (fd < 0) return out;
    const u32 buf = scratch(8192);
    for (;;) {
        const i32 n = call32(ctx, kSceIofilemgr, kRead, {u(fd), buf, 8192});
        if (n <= 0) break;
        const u8* p = current_runtime()->memory().host(buf);
        out.insert(out.end(), p, p + n);
    }
    call32(ctx, kSceIofilemgr, kClose, {u(fd)});
    return out;
}

void write_text(const nx::string& path, nx::string_view text) {
    (void)nx::fs::file_write_text(nx::fs::path_view(path), text);
}
nx::string read_text(const nx::string& path) {
    auto t = nx::fs::file_read_text(nx::fs::path_view(path));
    return t ? nx::string(*t) : nx::string("<missing>");
}

}  // namespace

TEST_CASE("apputil: slots, saves through savedata0: (read-only to sceIo), the quota's count, removal") {
    TempDir dir;
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 quota = scratch(8);
    CHECK(call32(ctx, kSceAppUtil, kGetQuota, {quota, quota + 4, 0}) == kNotInitialized);
    const u32 bad_init = scratch(64 + 40);
    std::memset(t.rt.memory().host(bad_init), 0, 104);
    *t.rt.memory().host_as<u32>(bad_init) = 1;  // (workBufSize must be 0)
    CHECK(call32(ctx, kSceAppUtil, kInit, {bad_init, bad_init + 64}) == kParameter);
    REQUIRE(init(ctx) == 0);
    CHECK(init(ctx) == u(kBusy));
    CHECK(call32(ctx, kSceAppUtil, kGetQuota, {quota, quota + 4, 0}) == kNotMounted);
    t.rt.files().mount({"savedata0", dir.path, false});

    // Slots.
    REQUIRE(call32(ctx, kSceAppUtil, kSlotCreate, {0, slot_param("Persona"), 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kSlotCreate, {0, slot_param("Persona"), 0}) == SaveData::kSlotExists);
    CHECK(call32(ctx, kSceAppUtil, kSlotCreate, {256, slot_param("Persona"), 0}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kSlotCreate, {2, slot_param("Two\nlines"), 0}) == kParameter);
    const u32 other_mount = scratch(16);
    std::memset(t.rt.memory().host(other_mount), 0, 16);
    std::strcpy(reinterpret_cast<char*>(t.rt.memory().host(other_mount)), "ux0:");
    CHECK(call32(ctx, kSceAppUtil, kSlotCreate, {2, slot_param("Persona"), other_mount}) == kParameter);
    i32 r;
    SaveData::SlotParam p = get_param(ctx, 0, &r);
    CHECK(r == 0);
    CHECK(nx::string(p.title) == "Persona");
    CHECK(nx::string(p.detail) == "Line one\nline two");
    CHECK(p.status == SaveData::kAvailable);
    CHECK(p.modified.year >= 2024);
    get_param(ctx, 1, &r);
    CHECK(r == SaveData::kSlotNotFound);
    CHECK(call32(ctx, kSceAppUtil, kSlotSetParam, {1, slot_param("x"), 0}) == SaveData::kSlotNotFound);
    CHECK(call32(ctx, kSceAppUtil, kSlotSetParam, {0, slot_param("Renamed", 7), 0}) == 0);
    p = get_param(ctx, 0, &r);
    CHECK(nx::string(p.title) == "Renamed");
    CHECK(p.user_param == 7);

    // A save of two files with slot 1 (new): both there, the slot available.
    const u32 required = scratch(4);
    const u32 a = filled(1000, 0xA5);
    const u32 b = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    CHECK(call32(ctx, kSceAppUtil, kDataSave,
                 {data_slot(1, slot_param("Slot one", 3)),
                  save_items({{"data/a.bin", a, 1000, 0, 0}, {"b.bin", b, 10, 4096, 0}}), 2, 0, required}) == 0);
    CHECK(*t.rt.memory().host_as<u32>(required) == 0);
    p = get_param(ctx, 1, &r);
    CHECK(r == 0);
    CHECK(nx::string(p.title) == "Slot one");
    CHECK(p.status == SaveData::kAvailable);
    CHECK(guest_size(ctx, "savedata0:data/a.bin") == 1000);
    nx::vector<u8> bfile = guest_read(ctx, "savedata0:b.bin");
    REQUIRE(bfile.size() == 4106);
    CHECK(bfile[0] == 0);
    CHECK(bfile[4095] == 0);
    CHECK(bfile[4096] == 1);
    CHECK(bfile[4105] == 10);
    // (sceIo may not write save data)
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("savedata0:c.bin"), 0x202, 0666}) == err::kReadOnlyFs);

    // Overwrites: a file keeps its size; a truncating write cuts it to what it wrote. A folder alone.
    CHECK(call32(ctx, kSceAppUtil, kDataSave,
                 {0, save_items({{"data/a.bin", bytes({9, 9, 9, 9}), 4, 2, 0}, {"b.bin", bytes({7, 7}), 2, 0, 1},
                                 {"dir1/dir2", 0, 0, 0, 2}}),
                  3, 0, required}) == 0);
    nx::vector<u8> afile = guest_read(ctx, "savedata0:data/a.bin");
    REQUIRE(afile.size() == 1000);
    CHECK(afile[1] == 0xA5);
    CHECK(afile[2] == 9);
    CHECK(afile[5] == 9);
    CHECK(afile[6] == 0xA5);
    CHECK(guest_size(ctx, "savedata0:b.bin") == 2);
    CHECK(nx::fs::status(nx::fs::path_view(dir.path + "/dir1/dir2")) == nx::fs::file_type::directory);

    // The count: 768 KiB of the system's, 32 KiB a folder (data, dir1, dir2), each file in 32 KiB clusters.
    CHECK(call32(ctx, kSceAppUtil, kGetQuota, {quota, quota + 4, 0}) == 0);
    CHECK(t.rt.memory().host_as<u32>(quota)[0] == 1024 * 1024);
    CHECK(t.rt.memory().host_as<u32>(quota)[1] == 768 + 3 * 32 + 2 * 32);

    // What a save may not be.
    const u32 big = filled(1024 * 1024 + 1, 1);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"x.bin", big, 1024 * 1024 + 1, 0, 0}}), 1, 0, required}) ==
          kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"../x.bin", b, 1, 0, 0}}), 1, 0, required}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"a/b/c/d/e.bin", b, 1, 0, 0}}), 1, 0, required}) ==
          kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"x y.bin", b, 1, 0, 0}}), 1, 0, required}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"x.bin", b, 1, 0, 0}}), 6, 0, required}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"x.bin", b, 1, 0, 0}}), 1, 0, 0}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"data", b, 1, 0, 0}}), 1, 0, required}) == kParameter);
    CHECK(guest_size(ctx, "savedata0:x.bin") == -1);

    // Searches: the slots in use by user parameter, descending; the empty ones counted.
    const u32 work = scratch(40), cond = scratch(52), result = scratch(40), list = scratch(16 * 256);
    std::memset(t.rt.memory().host(work), 0, 40);
    *t.rt.memory().host_as<u32>(work) = list;
    t.rt.memory().host_as<u32>(work)[1] = 16 * 256;
    std::memset(t.rt.memory().host(cond), 0, 52);
    u32* c = t.rt.memory().host_as<u32>(cond);
    c[0] = 0, c[1] = 0, c[2] = 256, c[3] = 1, c[4] = 1;
    REQUIRE(call32(ctx, kSceAppUtil, kSlotSearch, {work, cond, result, 0}) == 0);
    CHECK(t.rt.memory().host_as<u32>(result)[0] == 2);
    CHECK(t.rt.memory().host_as<u32>(result)[1] == list);
    CHECK(t.rt.memory().host_as<u32>(list)[0] == 0);  // (user parameter 7)
    CHECK(t.rt.memory().host_as<u32>(list)[4] == 1);  // (3)
    c[0] = 1, c[3] = 0;
    REQUIRE(call32(ctx, kSceAppUtil, kSlotSearch, {0, cond, result, 0}) == 0);
    CHECK(t.rt.memory().host_as<u32>(result)[0] == 254);

    // Removal: with slot 1 the slot goes too; keeping slot 0, it stays (available).
    const u32 rm = scratch(44);
    std::memset(t.rt.memory().host(rm), 0, 44);
    *t.rt.memory().host_as<u32>(rm) = guest_string("data");
    CHECK(call32(ctx, kSceAppUtil, kDataRemove, {data_slot(1, 0), rm, 1, 0}) == 0);
    CHECK(guest_size(ctx, "savedata0:data/a.bin") == -1);
    get_param(ctx, 1, &r);
    CHECK(r == SaveData::kSlotNotFound);
    CHECK(call32(ctx, kSceAppUtil, kDataRemove, {data_slot(1, 0), rm, 1, 0}) == err::kNoEntry);
    *t.rt.memory().host_as<u32>(rm) = guest_string("b.bin");
    t.rt.memory().host_as<u32>(rm)[1] = 1;  // SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_KEEP_SLOT
    CHECK(call32(ctx, kSceAppUtil, kDataRemove, {data_slot(0, 0), rm, 1, 0}) == 0);
    CHECK(guest_size(ctx, "savedata0:b.bin") == -1);
    p = get_param(ctx, 0, &r);
    CHECK(r == 0);
    CHECK(p.status == SaveData::kAvailable);
    CHECK(nx::string(p.title) == "Renamed");
    CHECK(call32(ctx, kSceAppUtil, kSlotDelete, {0, 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kSlotDelete, {0, 0}) == SaveData::kSlotNotFound);

    // The slots outlive the process: another runtime reads them back.
    REQUIRE(call32(ctx, kSceAppUtil, kSlotCreate, {9, slot_param("Kept"), 0}) == 0);
    SaveData again(dir.path);
    CHECK(again.slots().size() == 1);
    CHECK(again.slot(9, &p));
    CHECK(nx::string(p.title) == "Kept");
}

TEST_CASE("apputil: a save over the quota is refused with what is missing") {
    TempDir dir;
    // (a file nearly as large as the quota; sparse where the host allows)
    {
        host::OpenOptions o;
        o.mode = host::OpenMode::Write;
        o.create = true;
        host::FileError e = host::FileError::None;
        host::File f = host::File::open(dir.path + "/big.bin", o, &e);
        REQUIRE(f.valid());
        REQUIRE(f.resize((SaveData::kQuotaKiB - SaveData::kSystemKiB - 64) * 1024));
    }
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    t.rt.files().mount({"savedata0", dir.path, false});
    REQUIRE(init(ctx) == 0);
    const u32 quota = scratch(8), required = scratch(4);
    REQUIRE(call32(ctx, kSceAppUtil, kGetQuota, {quota, quota + 4, 0}) == 0);
    CHECK(t.rt.memory().host_as<u32>(quota)[1] == 1024 * 1024 - 64);
    // 32 KiB fits; 100 KiB more (4 clusters: 128 KiB) is 96 KiB too many.
    const u32 data = filled(100 * 1024, 3);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {0, save_items({{"small.bin", data, 32 * 1024, 0, 0}}), 1, 0, required}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kDataSave, {data_slot(4, 0), save_items({{"more.bin", data, 100 * 1024, 0, 0}}), 1, 0,
                                               required}) == SaveData::kNoSpaceQuota);
    CHECK(*t.rt.memory().host_as<u32>(required) == 96);
    CHECK(guest_size(ctx, "savedata0:more.bin") == -1);
    i32 r;
    get_param(ctx, 4, &r);
    CHECK(r == SaveData::kSlotNotFound);  // (refused before anything changed)
}

TEST_CASE("apputil: a save cut short is finished at the next start once committed, else never happened") {
    TempDir dir;
    {
        SaveData s(dir.path);
        REQUIRE(s.ok());
        const u8 old_data[] = {'o', 'l', 'd'};
        SaveData::SaveItem item{"a.txt", old_data, 0, SaveData::kFile};
        u32 required = 0;
        REQUIRE(s.save({&item, 1}, nullptr, &required) == 0);
    }
    CHECK(read_text(dir.path + "/a.txt") == "old");
    // Cut after the commit list was published: the next start carries it out.
    (void)nx::fs::create_directories(nx::fs::path_view(dir.path + "/sce_sys/~psvr"));
    write_text(dir.path + "/sce_sys/~psvr/0", "new");
    write_text(dir.path + "/sce_sys/~psvr/1", "made");
    write_text(dir.path + "/sce_sys/~psvr_commit", "dir d\nmove sce_sys/~psvr/0 a.txt\nmove sce_sys/~psvr/1 d/e/f.txt\n");
    {
        SaveData s(dir.path);
        REQUIRE(s.ok());
    }
    CHECK(read_text(dir.path + "/a.txt") == "new");
    CHECK(read_text(dir.path + "/d/e/f.txt") == "made");
    CHECK(!nx::fs::exists(nx::fs::path_view(dir.path + "/sce_sys/~psvr_commit")));
    CHECK(!nx::fs::exists(nx::fs::path_view(dir.path + "/sce_sys/~psvr")));
    // Cut before: the staged files and the unpublished list are thrown away.
    (void)nx::fs::create_directories(nx::fs::path_view(dir.path + "/sce_sys/~psvr"));
    write_text(dir.path + "/sce_sys/~psvr/0", "lost");
    write_text(dir.path + "/sce_sys/~psvr_commit~new", "move sce_sys/~psvr/0 a.txt\n");
    {
        SaveData s(dir.path);
        REQUIRE(s.ok());
        // (and a slot given with a save is available once it is done)
        SaveData::SlotParam param{};
        param.status = SaveData::kBroken;
        std::strcpy(param.title, "t");
        const u8 x[] = {1};
        SaveData::SaveItem item{"x.bin", x, 0, SaveData::kFile};
        SaveData::SlotUpdate slot{3, &param};
        u32 required = 0;
        REQUIRE(s.save({&item, 1}, &slot, &required) == 0);
        SaveData::SlotParam out;
        REQUIRE(s.slot(3, &out));
        CHECK(out.status == SaveData::kAvailable);
    }
    CHECK(read_text(dir.path + "/a.txt") == "new");
    CHECK(!nx::fs::exists(nx::fs::path_view(dir.path + "/sce_sys/~psvr_commit~new")));
    CHECK(!nx::fs::exists(nx::fs::path_view(dir.path + "/sce_sys/~psvr")));
}

TEST_CASE("apputil: safe memory survives the process; the system's settings") {
    TempDir dir;
    Runtime::Config config;
    config.system.safe_memory = dir.path + "/safemem.dat";
    config.system.language = 18;
    config.system.user_name = "Narukami";
    {
        TestRuntime t(config);
        PsvContext* ctx = t.ctx();
        const u32 buf = bytes({1, 2, 3, 4, 5, 6, 7, 8});
        CHECK(call32(ctx, kSceAppUtil, kSaveSafe, {buf, 8, 0, 0}) == kNotInitialized);
        REQUIRE(init(ctx) == 0);
        CHECK(call32(ctx, kSceAppUtil, kSaveSafe, {buf, 8, 100, 0}) == 8);
        CHECK(call32(ctx, kSceAppUtil, kSaveSafe, {buf, 8, 64 * 1024 - 4, 0}) == kParameter);
        const u32 value = scratch(4);
        CHECK(call32(ctx, kSceAppUtil, kSysInt, {1, value}) == 0);
        CHECK(*t.rt.memory().host_as<i32>(value) == 18);
        CHECK(call32(ctx, kSceAppUtil, kSysInt, {2, value}) == kParameter);
        const u32 name = scratch(32);
        CHECK(call32(ctx, kSceAppUtil, kSysString, {3, name, 16}) == kParameter);
        CHECK(call32(ctx, kSceAppUtil, kSysString, {3, name, 17}) == 0);
        CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(name))) == "Narukami");
    }
    {
        TestRuntime t(config);
        PsvContext* ctx = t.ctx();
        REQUIRE(init(ctx) == 0);
        const u32 buf = filled(16, 0xEE);
        CHECK(call32(ctx, kSceAppUtil, kLoadSafe, {buf, 16, 96, 0}) == 16);
        const u8* p = t.rt.memory().host(buf);
        CHECK(p[3] == 0);
        CHECK(p[4] == 1);
        CHECK(p[11] == 8);
        CHECK(p[12] == 0);
    }
}

namespace {

constexpr u32 kDrmOpen = 0x2DB7BE3B, kDrmClose = 0x6A140498;
constexpr i32 kNoEntry = static_cast<i32>(0x80010002u), kNoSpace = static_cast<i32>(0x8001001Cu),
              kAlready = static_cast<i32>(0x80010078u);

// A SceAppUtilDrmAddcontId (and a mount point) in guest memory.
u32 addcont_id(const char* name) {
    const u32 p = scratch(20);
    std::memset(current_runtime()->memory().host(p), 0, 20);
    std::memcpy(current_runtime()->memory().host(p), name, std::min<usize>(std::strlen(name), 17));
    return p;
}
u32 mount_point(const char* name) {
    const u32 p = scratch(16);
    std::memset(current_runtime()->memory().host(p), 0, 16);
    std::strcpy(reinterpret_cast<char*>(current_runtime()->memory().host(p)), name);
    return p;
}

}  // namespace

TEST_CASE("apputil: add-ons in addcont0: open with sceAppUtilDrmOpen, 16 at once, and close when nothing is in use") {
    TempDir dir;
    (void)nx::fs::create_directories(nx::fs::path_view(dir.path + "/ADDCONTDLC00001"));
    (void)nx::fs::file_write_text(nx::fs::path_view(dir.path + "/ADDCONTDLC00001/song.cpk"), "song");
    for (int i = 0; i < 16; ++i)
        (void)nx::fs::create_directories(nx::fs::path_view(nx::format("{}/ADDCONTMORE{:05}", dir.path, i)));
    TestRuntime t;
    t.rt.files().mount({"addcont0", dir.path, false, {}, true});
    PsvContext* ctx = t.ctx();
    const u32 song = addcont_id("ADDCONTDLC00001");
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, 0}) == kNotInitialized);
    REQUIRE(init(ctx) == 0);

    // The folders are listed, but what is in one opens only while it is open.
    const u32 st = scratch(88);
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("addcont0:ADDCONTDLC00001"), st}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("addcont0:ADDCONTDLC00001/song.cpk"), 0x1, 0}) == kNoEntry);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, mount_point("addcont0:")}) == kAlready);
    const i32 fd = call32(ctx, kSceLibKernel, kOpen, {guest_string("addcont0:ADDCONTDLC00001/song.cpk"), 0x1, 0});
    REQUIRE(fd > 0);
    const u32 buf = scratch(8);
    CHECK(call32(ctx, kSceIofilemgr, kRead, {u(fd), buf, 8}) == 4);
    CHECK(call32(ctx, kSceAppUtil, kDrmClose, {song, 0}) == kBusy);  // (a file in it is open)
    call32(ctx, kSceIofilemgr, kClose, {u(fd)});
    CHECK(call32(ctx, kSceAppUtil, kDrmClose, {song, 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kDrmClose, {song, 0}) == kParameter);  // (not open)
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("addcont0:ADDCONTDLC00001/song.cpk"), 0x1, 0}) == kNoEntry);

    // Names, mount points.
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id("NOTINSTALLED0000"), 0}) == kNoEntry);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id("../ADDCONTDLC001"), 0}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id(""), 0}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {0, 0}) == kParameter);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, mount_point("addcont1:")}) == kNotMounted);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, mount_point("savedata0:")}) == kParameter);

    // 16 open at once.
    for (int i = 0; i < 16; ++i)
        CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id(nx::format("ADDCONTMORE{:05}", i).c_str()), 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, 0}) == kNoSpace);
    CHECK(call32(ctx, kSceAppUtil, kDrmClose, {addcont_id("ADDCONTMORE00003"), 0}) == 0);
    CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {song, 0}) == 0);
}

TEST_CASE("apputil: without add-ons addcont0: is an empty device; without it, not mounted") {
    {
        TestRuntime t;
        t.rt.files().mount({"addcont0", "", false, {}, true});
        PsvContext* ctx = t.ctx();
        REQUIRE(init(ctx) == 0);
        const i32 d = call32(ctx, kSceLibKernel, 0xA9283DD0 /*sceIoDopen*/, {guest_string("addcont0:")});
        REQUIRE(d > 0);
        CHECK(call32(ctx, kSceLibKernel, 0x9C8B6624 /*sceIoDread*/, {u(d), scratch(352)}) == 0);
        CHECK(call32(ctx, kSceIofilemgr, 0x422A221A /*sceIoDclose*/, {u(d)}) == 0);
        CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id("ADDCONTDLC00001"), 0}) == kNoEntry);
        CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("addcont0:x"), 0x1, 0}) == kNoEntry);
    }
    {
        TestRuntime t;
        PsvContext* ctx = t.ctx();
        REQUIRE(init(ctx) == 0);
        CHECK(call32(ctx, kSceAppUtil, kDrmOpen, {addcont_id("ADDCONTDLC00001"), 0}) == kNotMounted);
    }
}
