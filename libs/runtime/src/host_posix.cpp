#include "psvrecore/runtime/host.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

namespace psvrecore::runtime::host {

namespace {

FileError from_errno(int e) {
    switch (e) {
        case ENOENT: return FileError::NotFound;
        case EEXIST: return FileError::Exists;
        case EACCES:
        case EPERM:
        case EROFS: return FileError::Access;
        case EISDIR: return FileError::IsDirectory;
        case ENOTDIR: return FileError::NotDirectory;
        case ENOTEMPTY: return FileError::NotEmpty;
        case ENOSPC: return FileError::NoSpace;
        case EINVAL:
        case ENAMETOOLONG: return FileError::Invalid;
        default: return FileError::Other;
    }
}

nx::string c_path(nx::string_view path) { return nx::string(path); }

struct FaultRegion {
    std::atomic<uintptr_t> base{0};
    std::atomic<u64> size{0};
    std::atomic<FaultHandler> handler{nullptr};
    bool installed = false;
    struct sigaction previous_segv {};
    struct sigaction previous_bus {};
};
FaultRegion g_fault;

void on_signal(int sig, siginfo_t* info, void* uc) {
    const FaultHandler handler = g_fault.handler.load(std::memory_order_acquire);
    const uintptr_t base = g_fault.base.load(std::memory_order_relaxed);
    const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
    if (handler && address >= base && address - base < g_fault.size.load(std::memory_order_relaxed))
        handler(address - base, false);  // (POSIX does not say whether it was a write)
    // Hand the signal on to whoever had it before (nx-foundation's crash reporter, or the default action).
    const struct sigaction& previous = sig == SIGBUS ? g_fault.previous_bus : g_fault.previous_segv;
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(sig, info, uc);
            return;
        }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

bool decommit(void* address, usize size) {
    // A fresh inaccessible mapping over the range drops its pages and keeps the address range ours.
    return mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != MAP_FAILED;
}

bool watch(void* const*, const u32*, u32, WatchHandler) { return false; }

void* find_code(const char*) { return nullptr; }

void describe_code(void* pc, char* out, usize out_size) {
    if (out_size) snprintf(out, out_size, "%p", pc);
}

void thread_times(ThreadTime each, void* user) {
#if defined(__linux__)
    // (/proc/self/task/<tid>: comm holds the name, stat the user and system times in clock ticks, fields 14 and 15)
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) return;
    const long ticks = sysconf(_SC_CLK_TCK);
    while (dirent* e = readdir(tasks)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], name[64] = "", stat[1024] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        if (FILE* f = fopen(path, "r")) {
            if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = '\0';
            fclose(f);
        }
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        const bool read = fgets(stat, sizeof stat, f) != nullptr;
        fclose(f);
        const char* after = read ? strrchr(stat, ')') : nullptr;  // (the name in parentheses may hold spaces)
        if (!after || ticks <= 0) continue;
        unsigned long long utime = 0, stime = 0;
        // (after ')': state, then fields 4-13, then utime and stime)
        if (sscanf(after + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) continue;
        each(strtoull(e->d_name, nullptr, 10), name, (utime + stime) * 1'000'000'000ull / static_cast<u64>(ticks), user);
    }
    closedir(tasks);
#else
    (void)each, (void)user;
#endif
}

void set_fault_region(void* base, u64 size, FaultHandler on_fault) {
    g_fault.handler.store(nullptr, std::memory_order_release);
    g_fault.base.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
    g_fault.size.store(size, std::memory_order_relaxed);
    g_fault.handler.store(on_fault, std::memory_order_release);
    if (on_fault && !g_fault.installed) {
        struct sigaction action {};
        action.sa_sigaction = on_signal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &g_fault.previous_segv);
        sigaction(SIGBUS, &action, &g_fault.previous_bus);
        g_fault.installed = true;
    }
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

File File::open(nx::string_view path, const OpenOptions& options, FileError* error) {
    int flags = O_CLOEXEC;
    switch (options.mode) {
        case OpenMode::Read: flags |= O_RDONLY; break;
        case OpenMode::Write: flags |= O_WRONLY; break;
        case OpenMode::ReadWrite: flags |= O_RDWR; break;
    }
    if (options.create) flags |= O_CREAT;
    if (options.create && options.exclusive) flags |= O_EXCL;
    if (options.truncate) flags |= O_TRUNC;
    if (options.append) flags |= O_APPEND;
    File f;
    const int fd = ::open(c_path(path).c_str(), flags, 0644);
    if (fd < 0) {
        if (error) *error = from_errno(errno);
        return f;
    }
    struct stat st {};
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        ::close(fd);
        if (error) *error = FileError::IsDirectory;
        return f;
    }
    f.handle_ = fd;
    if (error) *error = FileError::None;
    return f;
}

void File::close() {
    if (handle_ != kInvalid) ::close(static_cast<int>(handle_));
    handle_ = kInvalid;
}

i64 File::read_at(u64 offset, void* buffer, u64 size) const {
    u8* out = static_cast<u8*>(buffer);
    u64 done = 0;
    while (done < size) {
        const ssize_t n = pread(static_cast<int>(handle_), out + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return done ? static_cast<i64>(done) : -1;
        }
        if (n == 0) break;
        done += static_cast<u64>(n);
    }
    return static_cast<i64>(done);
}

i64 File::write_at(u64 offset, const void* buffer, u64 size) const {
    const u8* in = static_cast<const u8*>(buffer);
    u64 done = 0;
    while (done < size) {
        const ssize_t n = pwrite(static_cast<int>(handle_), in + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return done ? static_cast<i64>(done) : -1;
        }
        done += static_cast<u64>(n);
    }
    return static_cast<i64>(done);
}

i64 File::append(const void* buffer, u64 size) const {
    for (;;) {
        const ssize_t n = ::write(static_cast<int>(handle_), buffer, size);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

i64 File::size() const {
    struct stat st {};
    if (fstat(static_cast<int>(handle_), &st) != 0) return -1;
    return st.st_size;
}

bool File::resize(u64 size) const { return ftruncate(static_cast<int>(handle_), static_cast<off_t>(size)) == 0; }

bool File::flush() const { return fsync(static_cast<int>(handle_)) == 0; }

FileError make_directory(nx::string_view path) {
    return ::mkdir(c_path(path).c_str(), 0755) == 0 ? FileError::None : from_errno(errno);
}

FileError remove_directory(nx::string_view path) {
    return ::rmdir(c_path(path).c_str()) == 0 ? FileError::None : from_errno(errno);
}

FileError remove_file(nx::string_view path) {
    return ::unlink(c_path(path).c_str()) == 0 ? FileError::None : from_errno(errno);
}

FileError rename(nx::string_view from, nx::string_view to) {
    return ::rename(c_path(from).c_str(), c_path(to).c_str()) == 0 ? FileError::None : from_errno(errno);
}

}  // namespace psvrecore::runtime::host
