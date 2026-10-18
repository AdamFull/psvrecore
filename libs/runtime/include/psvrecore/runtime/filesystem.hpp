#pragma once

// The guest's files: device names (app0:, ux0:, savedata0:, ...) mounted on host folders, and the descriptors
// sceIoOpen and sceIoDopen hand out.
//
// A guest path is "device:path", the device name without regard to case, '/' between names. Inside a device, "."
// and ".." are resolved and never leave the device's folder. File names are matched without regard to case, as on
// the console's file systems (on hosts whose file system is case-sensitive, by looking through the folder).
//
// The tty devices (stdin, stdout, stderr of sceKernelGetStd*) are descriptors too: what the guest writes to them goes
// to a sink (the host's standard output by default).

#include <memory>

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime {

namespace io {

// Open flags (SCE_O_*).
inline constexpr u32 kRead = 0x0001, kWrite = 0x0002, kNonBlock = 0x0004, kAppend = 0x0100, kCreate = 0x0200,
                     kTruncate = 0x0400, kExclusive = 0x0800, kNoBuffer = 0x4000, kAsync = 0x8000;
inline constexpr i32 kSeekSet = 0, kSeekCur = 1, kSeekEnd = 2;
// st_mode
inline constexpr u32 kModeDirectory = 0x1000, kModeFile = 0x2000, kModeReadWrite = 0600, kModeRead = 0400;

struct DateTime {
    u16 year, month, day, hour, minute, second;
    u32 microsecond;
};
static_assert(sizeof(DateTime) == 16);

// SceIoStat, as the guest lays it out.
struct Stat {
    i32 mode;
    u32 attr;
    i64 size;
    DateTime ctime, atime, mtime;
    u32 reserved[6];
};
static_assert(sizeof(Stat) == 88);

// SceIoDirent.
struct Dirent {
    Stat stat;
    char name[256];
    u32 reserved_pointer;
    i32 dummy;
};
static_assert(sizeof(Dirent) == 352);

}  // namespace io

class FileSystem {
public:
    struct Mount {
        nx::string device;  // without the colon ("app0")
        nx::string host;    // the host folder
        bool writable = false;
    };
    struct Config {
        nx::vector<Mount> mounts;
    };
    using Sink = nx::function<void(i32 fd, nx::string_view text)>;

    explicit FileSystem(const Config& config);
    ~FileSystem();

    void mount(const Mount& mount);
    void set_tty_sink(Sink sink);

    // The host path of a guest path, or a guest error code.
    i32 resolve(nx::string_view guest, nx::string* host, bool* writable, bool must_exist = false) const;

    i32 stdin_fd() const { return kStdin; }
    i32 stdout_fd() const { return kStdout; }
    i32 stderr_fd() const { return kStderr; }

    // These take host buffers (the HLE layer checks guest pointers) and return a guest result.
    i32 open(nx::string_view path, u32 flags, i32 mode);
    i32 close(i32 fd);
    i64 read(i32 fd, void* buffer, u32 size);
    i64 write(i32 fd, const void* buffer, u32 size);
    i64 pread(i32 fd, void* buffer, u32 size, i64 offset);
    i64 pwrite(i32 fd, const void* buffer, u32 size, i64 offset);
    i64 seek(i32 fd, i64 offset, i32 whence);
    i32 sync(i32 fd);
    i32 stat(nx::string_view path, io::Stat* out);
    i32 stat_fd(i32 fd, io::Stat* out);
    i32 dopen(nx::string_view path);
    // 1 and an entry, 0 at the end, or an error.
    i32 dread(i32 fd, io::Dirent* out);
    i32 dclose(i32 fd);
    i32 mkdir(nx::string_view path, i32 mode);
    i32 rmdir(nx::string_view path);
    i32 remove(nx::string_view path);
    i32 rename(nx::string_view from, nx::string_view to);

private:
    static constexpr i32 kStdin = 1, kStdout = 2, kStderr = 3;
    struct Handle;
    std::shared_ptr<Handle> handle(i32 fd) const;

    mutable nx::mutex lock_;
    nx::vector<Mount> mounts_;
    nx::flat_hash_map<i32, std::shared_ptr<Handle>> handles_;
    i32 next_fd_ = 0x100;
    Sink sink_;
};

}  // namespace psvrecore::runtime
