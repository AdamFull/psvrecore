// Guest memory, the system library calling convention, files, memory blocks and formatted output.

#include <algorithm>
#include <chrono>
#include <cstring>

#include "nxtest.h"
#include "runtime_support.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

u32 u(i32 v) { return static_cast<u32>(v); }

// A fresh folder under the host's temporary folder, removed afterwards.
struct TempDir {
    nx::string path;
    TempDir() {
        static u32 counter = 0;
        nx::string base = "/tmp";
        if (auto t = nx::os::environment_variable("TEMP")) base = *t;
        else if (auto t2 = nx::os::environment_variable("TMPDIR")) base = *t2;
        path = nx::format("{}/psvrecore_test_{}_{}", base, nx::os::process_id(), counter++);
        (void)nx::fs::remove_all(nx::fs::path_view(path));
        (void)nx::fs::create_directories(nx::fs::path_view(path));
    }
    ~TempDir() { (void)nx::fs::remove_all(nx::fs::path_view(path)); }
};

}  // namespace

TEST_CASE("memory: blocks are mapped, found, protected and given back") {
    TestRuntime t;
    GuestMemory& m = t.rt.memory();
    REQUIRE(m.map_fixed(0x90000000u, 0x3000, Access::ReadWrite, BlockKind::Module, "seg"));
    CHECK_FALSE(m.map_fixed(0x90002000u, 0x1000, Access::ReadWrite, BlockKind::Module, "overlap"));
    const u32 a = m.map_anywhere(Region{0x90000000u, 0x90100000u}, 0x1000, 0x10000, Access::ReadWrite, BlockKind::MemBlock, "a", 7);
    CHECK(a == 0x90010000u);  // the first 64 KiB-aligned address past the first block
    Block b;
    REQUIRE(m.find(0x90002FFFu, &b));
    CHECK(b.name == "seg");
    CHECK_FALSE(m.find(0x90003000u, &b));
    CHECK(m.find_uid(7, &b));
    CHECK(b.base == a);
    CHECK(m.valid(0x90000000u, 0x3000));
    CHECK_FALSE(m.valid(0x90000000u, 0x3001));
    // Fresh memory reads as zero, also after it was given back and mapped again.
    psv_st32(0x90001000u, 0xDEADBEEF);
    CHECK(psv_ld32(0x90001000u) == 0xDEADBEEF);
    CHECK(m.unmap(0x90000000u));
    REQUIRE(m.map_fixed(0x90000000u, 0x3000, Access::ReadWrite, BlockKind::Module, "again"));
    CHECK(psv_ld32(0x90001000u) == 0);
    CHECK(m.protect(0x90000000u, Access::Read));
    CHECK(m.valid(0x90000000u, 4, false));
    CHECK_FALSE(m.valid(0x90000000u, 4, true));
    std::memcpy(m.host(a), "guest\0", 6);
    CHECK(m.string(a) == "guest");
    CHECK(m.string(0x70000000u).empty());  // (unmapped)
}

namespace {
u64 g_abi_args[5];
u64 abi_probe(u32 a, i64 b, u32 c, i64 d, u32 e) {
    g_abi_args[0] = a;
    g_abi_args[1] = static_cast<u64>(b);
    g_abi_args[2] = c;
    g_abi_args[3] = static_cast<u64>(d);
    g_abi_args[4] = e;
    return static_cast<u64>(b) + 1;
}
const hle::Function kAbiTest[] = {HLE_FUNCTION(0x11111111, abi_probe)};
}  // namespace

TEST_CASE("hle: arguments follow the ARM procedure call standard (64-bit ones in aligned pairs)") {
    TestRuntime t;
    hle::register_library({"AbiTest", 0x7E570001, kAbiTest});
    PsvContext* ctx = t.ctx();
    // a in r0; b in r2:r3 (r1 skipped); c on the stack at +0; d 8-byte aligned at +8; e at +16.
    const u64 r = call(ctx, 0x7E570001, 0x11111111,
                       {0xA, 0xFFFF, 0x89ABCDEF, 0x01234567, 0xC, 0xFFFF, 0x55555555, 0x66666666, 0xE});
    CHECK(g_abi_args[0] == 0xA);
    CHECK(g_abi_args[1] == 0x0123456789ABCDEFull);
    CHECK(g_abi_args[2] == 0xC);
    CHECK(g_abi_args[3] == 0x6666666655555555ull);
    CHECK(g_abi_args[4] == 0xE);
    CHECK(r == 0x0123456789ABCDF0ull);  // (r0:r1)
}

TEST_CASE("hle: formatted output takes the guest's variadic arguments") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 buffer = scratch(128);
    const u32 fmt = guest_string("%d|%5s|%08x|%lld|%.3f|%c|%-4u|%%");
    const u32 ab = guest_string("ab");
    const u64 big = 1ull << 40;
    double d = 1.5;
    u64 bits;
    std::memcpy(&bits, &d, 8);
    // r0 buffer, r1 size, r2 format, r3 -5; the stack: "ab", 0xbeef, then 2^40 (8-byte aligned), 1.5, 'z', 7.
    const i32 n = call32(ctx, kSceLibKernel, 0x8CBA03D5 /*sceClibSnprintf*/,
                         {buffer, 128, fmt, static_cast<u32>(-5), ab, 0xBEEF, static_cast<u32>(big), static_cast<u32>(big >> 32),
                          static_cast<u32>(bits), static_cast<u32>(bits >> 32), 'z', 7});
    const char* out = reinterpret_cast<const char*>(t.rt.memory().host(buffer));
    CHECK(nx::string(out) == "-5|   ab|0000beef|1099511627776|1.500|z|7   |%");
    CHECK(n == static_cast<i32>(std::strlen(out)));
    // Truncated to the buffer, the full length returned.
    const i32 m = call32(ctx, kSceLibKernel, 0x8CBA03D5, {buffer, 4, guest_string("%s"), guest_string("abcdef")});
    CHECK(m == 6);
    CHECK(nx::string(out) == "abc");
}

TEST_CASE("sysmem: memory blocks by type, size rules, lookup and freeing") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kAlloc = 0xB9D5EBDE, kFree = 0xA91E15EE, kBase = 0xB8EF5818, kFind = 0xA33B99D1;
    constexpr u32 kUserRw = 0x0C20D060, kCdram = 0x09408060, kPhycont = 0x0C80D060;
    const u32 name = guest_string("block");
    const i32 uid = call32(ctx, kSceSysmem, kAlloc, {name, kUserRw, 0x10000, 0});
    REQUIRE(uid > 0);
    const u32 base_out = scratch(4);
    CHECK(call32(ctx, kSceSysmem, kBase, {u(uid), base_out}) == 0);
    const u32 base = psv_ld32(base_out);
    CHECK(base >= layout::kUserMemory.begin);
    CHECK(base < layout::kUserMemory.end);
    CHECK(base % 0x1000 == 0);
    psv_st32(base + 0xFFFC, 1);  // (the whole block is usable)
    CHECK(call32(ctx, kSceSysmem, kFind, {base + 0x1234, 4}) == uid);
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, kUserRw, 0x1001, 0}) == err::kIllegalMemBlockSize);
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, 0x12345678, 0x1000, 0}) == err::kIllegalMemBlockType);
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, kCdram, 0x10000, 0}) == err::kIllegalMemBlockSize);  // (256 KiB units)
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, kPhycont, 0x40000, 0}) == err::kIllegalMemBlockSize);  // (1 MiB units)
    const i32 cdram = call32(ctx, kSceSysmem, kAlloc, {name, kCdram, 0x40000, 0});
    REQUIRE(cdram > 0);
    CHECK(call32(ctx, kSceSysmem, kBase, {u(cdram), base_out}) == 0);
    CHECK(psv_ld32(base_out) >= layout::kCdram.begin);
    CHECK(psv_ld32(base_out) % 0x40000 == 0);
    // An alignment asked for in the options.
    const u32 opt = scratch(20);
    psv_st32(opt, 20);
    psv_st32(opt + 4, 0x4);         // SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT
    psv_st32(opt + 8, 0x100000);    // 1 MiB
    const i32 aligned = call32(ctx, kSceSysmem, kAlloc, {name, kUserRw, 0x1000, opt});
    REQUIRE(aligned > 0);
    CHECK(call32(ctx, kSceSysmem, kBase, {u(aligned), base_out}) == 0);
    CHECK(psv_ld32(base_out) % 0x100000 == 0);
    // One smaller than the type's own (as the SDK's samples ask CDRAM for 256): the type's; not a power of two: refused.
    psv_st32(opt + 8, 0x100);
    const i32 small = call32(ctx, kSceSysmem, kAlloc, {name, kCdram, 0x40000, opt});
    REQUIRE(small > 0);
    CHECK(call32(ctx, kSceSysmem, kBase, {u(small), base_out}) == 0);
    CHECK(psv_ld32(base_out) % 0x40000 == 0);
    psv_st32(opt + 8, 0x3000);
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, kUserRw, 0x1000, opt}) == err::kIllegalAlignment);
    // More than the budget.
    CHECK(call32(ctx, kSceSysmem, kAlloc, {name, kUserRw, 0x20000000, 0}) == err::kNoFreePhysicalPage);
    CHECK(call32(ctx, kSceSysmem, kFree, {u(uid)}) == 0);
    CHECK(call32(ctx, kSceSysmem, kBase, {u(uid), base_out}) == err::kUnknownUid);
    CHECK(call32(ctx, kSceSysmem, kFree, {u(uid)}) == err::kUnknownUid);
}

TEST_CASE("files: open, write, read, seek, stat and directories on a mounted folder") {
    TempDir dir;
    TestRuntime t;
    t.rt.files().mount({"ux0", dir.path, true});
    PsvContext* ctx = t.ctx();
    constexpr u32 kOpen = 0x6C60AC61, kClose = 0xC70B8886, kRead = 0xFDB32293, kWrite = 0x34EFD876, kLseek = 0x99BA173E,
                  kPread = 0x52315AD7, kGetstat = 0xBCA5B623, kMkdir = 0x9670D39F, kDopen = 0xA9283DD0, kDread = 0x9C8B6624,
                  kDclose = 0x422A221A, kRemove = 0xE20ED0F3, kRename = 0xF737E369, kRmdir = 0xE9F91EC8;
    constexpr u32 kRdonly = 0x1, kWronly = 0x2, kRdwr = 0x3, kCreat = 0x200, kTrunc = 0x400, kExcl = 0x800, kAppend = 0x100;

    CHECK(call32(ctx, kSceLibKernel, kMkdir, {guest_string("ux0:data"), 0777}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kMkdir, {guest_string("ux0:data"), 0777}) == err::kExists);
    const i32 fd = call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:data/Save.bin"), kWronly | kCreat | kTrunc, 0666});
    REQUIRE(fd > 0);
    const u32 text = guest_string("hello, vita");
    CHECK(call32(ctx, kSceIofilemgr, kWrite, {u(fd), text, 11}) == 11);
    CHECK(call32(ctx, kSceIofilemgr, kClose, {u(fd)}) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kClose, {u(fd)}) == err::kBadFile);
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:data/save.bin"), kWronly | kCreat | kExcl, 0666}) == err::kExists);

    // Names without regard to case; "." and ".." resolved inside the device.
    const i32 rd = call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:/DATA/./x/../save.BIN"), kRdonly, 0});
    REQUIRE(rd > 0);
    const u32 buf = scratch(32);
    CHECK(call32(ctx, kSceIofilemgr, kRead, {u(rd), buf, 5}) == 5);
    CHECK(std::memcmp(t.rt.memory().host(buf), "hello", 5) == 0);
    // sceIoLseek(fd, offset (r2:r3), whence (stack)) returns the 64-bit position.
    CHECK(call(ctx, kSceLibKernel, kLseek, {u(rd), 0, 7, 0, 0}) == 7);
    CHECK(call32(ctx, kSceIofilemgr, kRead, {u(rd), buf, 32}) == 4);
    CHECK(std::memcmp(t.rt.memory().host(buf), "vita", 4) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kRead, {u(rd), buf, 32}) == 0);
    CHECK(call(ctx, kSceLibKernel, kLseek, {u(rd), 0, static_cast<u32>(-3), 0xFFFFFFFF, 2}) == 8);  // (from the end)
    // sceIoPread(fd, buffer, size, offset (stack, 8-byte aligned)) leaves the position alone.
    CHECK(call32(ctx, kSceLibKernel, kPread, {u(rd), buf, 2, 32, 0, 0}) == 2);
    CHECK(std::memcmp(t.rt.memory().host(buf), "he", 2) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kWrite, {u(rd), text, 1}) == err::kBadFile);  // (opened to read)
    call32(ctx, kSceIofilemgr, kClose, {u(rd)});

    const i32 ap = call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:data/save.bin"), kRdwr | kAppend, 0});
    CHECK(call32(ctx, kSceIofilemgr, kWrite, {u(ap), guest_string("!"), 1}) == 1);
    call32(ctx, kSceIofilemgr, kClose, {u(ap)});

    const u32 st = scratch(sizeof(runtime::io::Stat));
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("ux0:data/save.bin"), st}) == 0);
    const auto* stat = t.rt.memory().host_as<runtime::io::Stat>(st);
    CHECK(stat->size == 12);
    CHECK((stat->mode & 0xF000) == static_cast<i32>(runtime::io::kModeFile));
    CHECK(stat->mtime.year >= 2024);
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("ux0:data"), st}) == 0);
    CHECK((stat->mode & 0xF000) == static_cast<i32>(runtime::io::kModeDirectory));
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("ux0:nothing"), st}) == err::kNoEntry);
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("nodevice:x"), st}) == err::kNoEntry);

    const i32 d = call32(ctx, kSceLibKernel, kDopen, {guest_string("ux0:data")});
    REQUIRE(d > 0);
    const u32 entry = scratch(sizeof(runtime::io::Dirent));
    CHECK(call32(ctx, kSceLibKernel, kDread, {u(d), entry}) == 1);
    CHECK(nx::string(t.rt.memory().host_as<runtime::io::Dirent>(entry)->name) == "Save.bin");
    CHECK(call32(ctx, kSceLibKernel, kDread, {u(d), entry}) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kDclose, {u(d)}) == 0);

    CHECK(call32(ctx, kSceLibKernel, kRename, {guest_string("ux0:data/save.bin"), guest_string("ux0:data/b.bin")}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kRmdir, {guest_string("ux0:data")}) == err::kNotEmpty);
    CHECK(call32(ctx, kSceLibKernel, kRemove, {guest_string("ux0:data/b.bin")}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kRmdir, {guest_string("ux0:data")}) == 0);
}

TEST_CASE("files: a read-only device refuses writes; paths never leave their device's folder") {
    TempDir dir;
    TestRuntime t;
    (void)nx::fs::create_directories(nx::fs::path_view(dir.path + "/app"));
    (void)nx::fs::file_write_text(nx::fs::path_view(dir.path + "/outside.txt"), "secret");
    (void)nx::fs::file_write_text(nx::fs::path_view(dir.path + "/app/eboot.bin"), "code");
    t.rt.files().mount({"app0", dir.path + "/app", false});
    PsvContext* ctx = t.ctx();
    constexpr u32 kOpen = 0x6C60AC61, kRemove = 0xE20ED0F3;
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("app0:eboot.bin"), 0x1, 0}) > 0);
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("app0:eboot.bin"), 0x2, 0}) == err::kReadOnlyFs);
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("app0:new.bin"), 0x2 | 0x200, 0}) == err::kReadOnlyFs);
    CHECK(call32(ctx, kSceLibKernel, kRemove, {guest_string("app0:eboot.bin")}) == err::kReadOnlyFs);
    // ".." at the root stays at the root.
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("app0:../outside.txt"), 0x1, 0}) == err::kNoEntry);
    nx::string host;
    CHECK(t.rt.files().resolve("app0:../../eboot.bin", &host, nullptr) == 0);
    CHECK(host == dir.path + "/app/eboot.bin");
}

TEST_CASE("files: what the guest writes to stdout reaches the tty sink") {
    TestRuntime t;
    static nx::string captured;
    captured.clear();
    t.rt.files().set_tty_sink(FileSystem::Sink([](i32, nx::string_view text) { captured += text; }));
    PsvContext* ctx = t.ctx();
    const i32 out = call32(ctx, kSceProcessmgr, 0xE5AA625C /*sceKernelGetStdout*/, {});
    CHECK(call32(ctx, kSceIofilemgr, 0x34EFD876 /*sceIoWrite*/, {u(out), guest_string("line\n"), 5}) == 5);
    CHECK(call32(ctx, kSceLibKernel, 0xFA26BC62 /*sceClibPrintf*/, {guest_string("%d+%d"), 2, 3}) == 3);
    CHECK(captured == "line\n2+3");
}

TEST_CASE("files: a layered device shows the update's files over the base game's; folders list both") {
    TempDir dir;
    TestRuntime t;
    const nx::string base = dir.path + "/app", update = dir.path + "/patch";
    for (const nx::string& f : {base + "/data", base + "/sce_module", update + "/data"})
        (void)nx::fs::create_directories(nx::fs::path_view(f));
    (void)nx::fs::file_write_text(nx::fs::path_view(base + "/eboot.bin"), "base");
    (void)nx::fs::file_write_text(nx::fs::path_view(base + "/data/a.txt"), "old");
    (void)nx::fs::file_write_text(nx::fs::path_view(base + "/data/b.txt"), "B");
    (void)nx::fs::file_write_text(nx::fs::path_view(base + "/sce_module/libc.suprx"), "libc");
    (void)nx::fs::file_write_text(nx::fs::path_view(update + "/eboot.bin"), "update");
    (void)nx::fs::file_write_text(nx::fs::path_view(update + "/data/A.TXT"), "new!");  // (the same name, other case)
    (void)nx::fs::file_write_text(nx::fs::path_view(update + "/data/patch.cpk"), "cpk");
    FileSystem::Mount app0{"app0", update, false};
    app0.under.push_back(base);
    t.rt.files().mount(app0);
    PsvContext* ctx = t.ctx();
    constexpr u32 kOpen = 0x6C60AC61, kClose = 0xC70B8886, kRead = 0xFDB32293, kGetstat = 0xBCA5B623,
                  kDopen = 0xA9283DD0, kDread = 0x9C8B6624, kDclose = 0x422A221A;
    const u32 buf = scratch(16);
    const auto contents = [&](const char* path) {
        const i32 fd = call32(ctx, kSceLibKernel, kOpen, {guest_string(path), 0x1, 0});
        if (fd < 0) return nx::string("(error)");
        const i32 n = call32(ctx, kSceIofilemgr, kRead, {u(fd), buf, 16});
        call32(ctx, kSceIofilemgr, kClose, {u(fd)});
        return nx::string(reinterpret_cast<const char*>(t.rt.memory().host(buf)), static_cast<usize>(n));
    };
    CHECK(contents("app0:eboot.bin") == "update");
    CHECK(contents("app0:data/a.txt") == "new!");
    CHECK(contents("app0:data/b.txt") == "B");
    CHECK(contents("app0:sce_module/libc.suprx") == "libc");
    CHECK(contents("app0:data/patch.cpk") == "cpk");
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("app0:data/b.txt"), 0x2, 0}) == err::kReadOnlyFs);
    const u32 st = scratch(sizeof(runtime::io::Stat));
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("app0:data/b.txt"), st}) == 0);
    CHECK(t.rt.memory().host_as<runtime::io::Stat>(st)->size == 1);
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("app0:sce_module"), st}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kGetstat, {guest_string("app0:none"), st}) == err::kNoEntry);

    // A folder lists every layer's entries once, the update's spelling for a name both have.
    const auto list = [&](const char* path) {
        nx::vector<nx::string> names;
        const i32 d = call32(ctx, kSceLibKernel, kDopen, {guest_string(path)});
        if (d < 0) return nx::string("(error)");
        const u32 entry = scratch(sizeof(runtime::io::Dirent));
        while (call32(ctx, kSceLibKernel, kDread, {u(d), entry}) == 1)
            names.push_back(nx::string(t.rt.memory().host_as<runtime::io::Dirent>(entry)->name));
        call32(ctx, kSceIofilemgr, kDclose, {u(d)});
        std::sort(names.begin(), names.end(), [](const nx::string& a, const nx::string& b) { return a.view() < b.view(); });
        nx::string joined;
        for (const auto& n : names) joined += joined.empty() ? n : nx::format(",{}", n);
        return joined;
    };
    CHECK(list("app0:data") == "A.TXT,b.txt,patch.cpk");
    CHECK(list("app0:") == "data,eboot.bin,sce_module");
    CHECK(list("app0:sce_module") == "libc.suprx");
    nx::string host;
    CHECK(t.rt.files().resolve("app0:data/b.txt", &host, nullptr) == 0);
    CHECK(host == base + "/data/b.txt");
    CHECK(t.rt.files().resolve("app0:data/c.txt", &host, nullptr) == 0);  // (none has it: the top layer's)
    CHECK(host == update + "/data/c.txt");
    CHECK(t.rt.files().resolve("app0:data/c.txt", &host, nullptr, true) == err::kNoEntry);

    // On a writable device, only the top layer is written: a lower layer's file is read-only, new files go on top.
    FileSystem::Mount ux0{"ux0", update, true};
    ux0.under.push_back(base);
    t.rt.files().mount(ux0);
    CHECK(call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:data/b.txt"), 0x3, 0}) == err::kReadOnlyFs);
    CHECK(t.rt.files().remove("ux0:data/b.txt") == err::kReadOnlyFs);
    CHECK(t.rt.files().mkdir("ux0:sce_module", 0777) == err::kExists);
    const i32 fd = call32(ctx, kSceLibKernel, kOpen, {guest_string("ux0:data/c.txt"), 0x2 | 0x200, 0});
    REQUIRE(fd > 0);
    call32(ctx, kSceIofilemgr, kClose, {u(fd)});
    CHECK(nx::fs::exists(nx::fs::path_view(update + "/data/c.txt")));
}

TEST_CASE("timers: count microseconds of the process's time while started") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kCreate = 0x2255B2A5, kDelete = 0xAB1E42C4, kStart = 0x48091E0C, kStop = 0x869E9F20, kTime = 0x3EFD3165;
    const i32 uid = call32(ctx, kSceLibKernel, kCreate, {guest_string("frame timer"), 0, 0});
    REQUIRE(uid > 0);
    CHECK(call(ctx, kSceThreadmgr, kTime, {u(uid)}) == 0);  // (not started: nothing counted)
    CHECK(call32(ctx, kSceThreadmgr, kStart, {u(uid)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kStart, {u(uid)}) == 1);  // (already counting)
    nx::this_thread::sleep_for(std::chrono::milliseconds(20));
    const u64 running = call(ctx, kSceThreadmgr, kTime, {u(uid)});
    CHECK(running >= 15000);
    CHECK(running < 2000000);
    CHECK(call32(ctx, kSceThreadmgr, kStop, {u(uid)}) == 1);
    CHECK(call32(ctx, kSceThreadmgr, kStop, {u(uid)}) == 0);  // (already stopped)
    const u64 stopped = call(ctx, kSceThreadmgr, kTime, {u(uid)});
    nx::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(call(ctx, kSceThreadmgr, kTime, {u(uid)}) == stopped);  // (stopped: it stands still)
    CHECK(call32(ctx, kSceThreadmgr, kStart, {u(uid)}) == 0);     // (and goes on from there)
    CHECK(call(ctx, kSceThreadmgr, kTime, {u(uid)}) >= stopped);
    CHECK(call32(ctx, kSceThreadmgr, kDelete, {u(uid)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kStart, {u(uid)}) == static_cast<i32>(0x80028241u));
    CHECK(call(ctx, kSceThreadmgr, kTime, {u(uid)}) == ~u64{0});
}

TEST_CASE("recomp: a function stood in for a registered one is what lookups find, and gives back the one it replaced") {
    static PsvFunction original = nullptr;
    static u32 calls = 0;
    auto first = [](PsvContext*, uint32_t) {};
    auto second = [](PsvContext* ctx, uint32_t ret) {
        ++calls;
        original(ctx, ret);
    };
    constexpr u32 kAt = 0xDEAD0001;  // (Thumb code: bit 0 set, as calls give it)
    CHECK(recomp::override_function(kAt, first) == nullptr);
    CHECK(recomp::find_function(kAt) == static_cast<PsvFunction>(first));
    original = recomp::override_function(kAt, second);
    CHECK(original == static_cast<PsvFunction>(first));
    recomp::find_function(kAt)(nullptr, 0);
    CHECK(calls == 1);
    recomp::override_function(kAt, nullptr);
}
