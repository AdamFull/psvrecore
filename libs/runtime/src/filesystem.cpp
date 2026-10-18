#include "psvrecore/runtime/filesystem.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/errors.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/platform/filesystem.h"

namespace psvrecore::runtime {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequal(nx::string_view a, nx::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

i32 to_guest(host::FileError e) {
    switch (e) {
        case host::FileError::None: return err::kOk;
        case host::FileError::NotFound: return err::kNoEntry;
        case host::FileError::Exists: return err::kExists;
        case host::FileError::Access: return err::kAccess;
        case host::FileError::IsDirectory: return err::kIsDirectory;
        case host::FileError::NotDirectory: return err::kNotDirectory;
        case host::FileError::NotEmpty: return err::kNotEmpty;
        case host::FileError::NoSpace: return err::kNoSpace;
        case host::FileError::Invalid: return err::kInvalid;
        case host::FileError::Other: break;
    }
    return err::kIo;
}

io::DateTime date_time(u64 unix_ns) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ns / 1'000'000'000ull);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    io::DateTime d{};
    d.year = static_cast<u16>(tm.tm_year + 1900);
    d.month = static_cast<u16>(tm.tm_mon + 1);
    d.day = static_cast<u16>(tm.tm_mday);
    d.hour = static_cast<u16>(tm.tm_hour);
    d.minute = static_cast<u16>(tm.tm_min);
    d.second = static_cast<u16>(tm.tm_sec);
    d.microsecond = static_cast<u32>((unix_ns / 1000) % 1'000'000);
    return d;
}

void fill_stat(io::Stat* out, const nx::fs::file_stat& st, bool writable) {
    std::memset(out, 0, sizeof *out);
    const bool dir = st.type == nx::fs::file_type::directory;
    out->mode = static_cast<i32>((dir ? io::kModeDirectory : io::kModeFile) | (writable ? io::kModeReadWrite : io::kModeRead));
    out->size = dir ? 0 : static_cast<i64>(st.size);
    out->ctime = out->atime = out->mtime = date_time(st.mtime_ns);
}

nx::string join(nx::string_view a, nx::string_view b) {
    nx::string out(a);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    out += b;
    return out;
}

}  // namespace

struct FileSystem::Handle {
    bool directory = false;
    bool tty = false;
    bool append = false;
    bool writable = false;
    host::File file;
    nx::mutex position_lock;
    u64 position = 0;
    nx::string host_path;
    nx::vector<nx::string> entries;  // (directories) names, read in order
    usize next_entry = 0;
};

FileSystem::FileSystem(const Config& config) {
    for (const auto& m : config.mounts) mount(m);
    for (const i32 fd : {kStdin, kStdout, kStderr}) {
        auto h = std::make_shared<Handle>();
        h->tty = true;
        handles_[fd] = std::move(h);
    }
    sink_ = Sink([](i32 fd, nx::string_view text) {
        FILE* f = fd == kStderr ? stderr : stdout;
        std::fwrite(text.data(), 1, text.size(), f);
        std::fflush(f);
    });
}

FileSystem::~FileSystem() = default;

void FileSystem::mount(const Mount& mount) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (auto& m : mounts_)
        if (iequal(m.device, mount.device)) {
            m = mount;
            return;
        }
    mounts_.push_back(mount);
}

void FileSystem::set_tty_sink(Sink sink) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    sink_ = std::move(sink);
}

i32 FileSystem::resolve(nx::string_view guest, nx::string* host_path, bool* writable, bool must_exist) const {
    const usize colon = guest.find(':');
    if (colon == nx::string_view::npos || colon == 0) return err::kNoEntry;
    const nx::string_view device = guest.substr(0, colon);
    const Mount* mount = nullptr;
    {
        nx::scoped_lock<nx::mutex> guard(lock_);
        for (const auto& m : mounts_)
            if (iequal(m.device, device)) mount = &m;
        if (!mount) return err::kNoEntry;
    }
    // The names inside the device, with "." and ".." resolved (never above its root).
    nx::vector<nx::string_view> names;
    nx::string_view rest = guest.substr(colon + 1);
    while (!rest.empty()) {
        const usize slash = rest.find_first_of("/\\");
        const nx::string_view name = rest.substr(0, slash);
        rest = slash == nx::string_view::npos ? nx::string_view{} : rest.substr(slash + 1);
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            if (!names.empty()) names.pop_back();
            continue;
        }
        if (name.size() > 255) return err::kNameTooLong;
        names.push_back(name);
    }
    nx::string path = mount->host;
    for (usize i = 0; i < names.size(); ++i) {
        nx::string next = join(path, names[i]);
#ifndef _WIN32
        // Case-insensitive names on a case-sensitive host: find the name as the folder spells it.
        if (!nx::fs::exists(nx::fs::path_view(next))) {
            if (auto entries = nx::fs::directory_entries(nx::fs::path_view(path))) {
                for (const auto& e : *entries) {
                    const nx::string_view file = nx::fs::path::filename(nx::string_view(e.path));
                    if (iequal(file, names[i])) {
                        next = join(path, file);
                        break;
                    }
                }
            }
        }
#endif
        path = std::move(next);
    }
    if (must_exist && !nx::fs::exists(nx::fs::path_view(path))) return err::kNoEntry;
    if (host_path) *host_path = std::move(path);
    if (writable) *writable = mount->writable;
    return err::kOk;
}

std::shared_ptr<FileSystem::Handle> FileSystem::handle(i32 fd) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = handles_.find(fd);
    return it == handles_.end() ? nullptr : it->second;
}

i32 FileSystem::open(nx::string_view path, u32 flags, i32 /*mode*/) {
    nx::string host_path;
    bool writable = false;
    if (const i32 r = resolve(path, &host_path, &writable); r < 0) return r;
    const bool wants_write = flags & (io::kWrite | io::kCreate | io::kTruncate | io::kAppend);
    if (wants_write && !writable) return err::kReadOnlyFs;
    if (!(flags & (io::kRead | io::kWrite))) return err::kInvalid;
    host::OpenOptions options;
    options.mode = (flags & io::kRead) && (flags & io::kWrite) ? host::OpenMode::ReadWrite
                   : (flags & io::kWrite)                      ? host::OpenMode::Write
                                                               : host::OpenMode::Read;
    options.create = flags & io::kCreate;
    options.exclusive = flags & io::kExclusive;
    options.truncate = flags & io::kTruncate;
    options.append = flags & io::kAppend;
    host::FileError error = host::FileError::None;
    host::File file = host::File::open(host_path, options, &error);
    if (!file.valid()) return to_guest(error);
    auto h = std::make_shared<Handle>();
    h->file = std::move(file);
    h->append = options.append;
    h->writable = options.mode != host::OpenMode::Read;
    h->host_path = std::move(host_path);
    nx::scoped_lock<nx::mutex> guard(lock_);
    const i32 fd = next_fd_++;
    handles_[fd] = std::move(h);
    return fd;
}

i32 FileSystem::close(i32 fd) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = handles_.find(fd);
    if (it == handles_.end() || it->second->directory) return err::kBadFile;
    if (it->second->tty) return err::kOk;
    handles_.erase(it);
    return err::kOk;
}

i64 FileSystem::read(i32 fd, void* buffer, u32 size) {
    auto h = handle(fd);
    if (!h || h->directory) return err::kBadFile;
    if (h->tty) return 0;  // (no input)
    nx::scoped_lock<nx::mutex> guard(h->position_lock);
    const i64 n = h->file.read_at(h->position, buffer, size);
    if (n < 0) return err::kIo;
    h->position += static_cast<u64>(n);
    return n;
}

i64 FileSystem::write(i32 fd, const void* buffer, u32 size) {
    auto h = handle(fd);
    if (!h || h->directory) return err::kBadFile;
    if (h->tty) {
        Sink sink;
        {
            nx::scoped_lock<nx::mutex> guard(lock_);
            sink = sink_;
        }
        if (sink) sink(fd, nx::string_view(static_cast<const char*>(buffer), size));
        return size;
    }
    if (!h->writable) return err::kBadFile;
    nx::scoped_lock<nx::mutex> guard(h->position_lock);
    i64 n;
    if (h->append) {
        n = h->file.append(buffer, size);
        if (n >= 0) h->position = static_cast<u64>(h->file.size());
    } else {
        n = h->file.write_at(h->position, buffer, size);
        if (n >= 0) h->position += static_cast<u64>(n);
    }
    return n < 0 ? err::kIo : n;
}

i64 FileSystem::pread(i32 fd, void* buffer, u32 size, i64 offset) {
    auto h = handle(fd);
    if (!h || h->directory || h->tty) return err::kBadFile;
    if (offset < 0) return err::kInvalid;
    const i64 n = h->file.read_at(static_cast<u64>(offset), buffer, size);
    return n < 0 ? err::kIo : n;
}

i64 FileSystem::pwrite(i32 fd, const void* buffer, u32 size, i64 offset) {
    auto h = handle(fd);
    if (!h || h->directory || h->tty || !h->writable) return err::kBadFile;
    if (offset < 0) return err::kInvalid;
    const i64 n = h->file.write_at(static_cast<u64>(offset), buffer, size);
    return n < 0 ? err::kIo : n;
}

i64 FileSystem::seek(i32 fd, i64 offset, i32 whence) {
    auto h = handle(fd);
    if (!h || h->directory) return err::kBadFile;
    if (h->tty) return err::kIllegalSeek;
    nx::scoped_lock<nx::mutex> guard(h->position_lock);
    i64 base = 0;
    switch (whence) {
        case io::kSeekSet: base = 0; break;
        case io::kSeekCur: base = static_cast<i64>(h->position); break;
        case io::kSeekEnd: base = h->file.size(); break;
        default: return err::kInvalid;
    }
    const i64 at = base + offset;
    if (at < 0) return err::kInvalid;
    h->position = static_cast<u64>(at);
    return at;
}

i32 FileSystem::sync(i32 fd) {
    auto h = handle(fd);
    if (!h || h->directory) return err::kBadFile;
    if (!h->tty && h->writable) h->file.flush();
    return err::kOk;
}

i32 FileSystem::stat(nx::string_view path, io::Stat* out) {
    nx::string host_path;
    bool writable = false;
    if (const i32 r = resolve(path, &host_path, &writable); r < 0) return r;
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(host_path));
    if (st.type == nx::fs::file_type::not_found || st.type == nx::fs::file_type::none) return err::kNoEntry;
    fill_stat(out, st, writable);
    return err::kOk;
}

i32 FileSystem::stat_fd(i32 fd, io::Stat* out) {
    auto h = handle(fd);
    if (!h) return err::kBadFile;
    if (h->tty) {
        std::memset(out, 0, sizeof *out);
        return err::kOk;
    }
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(h->host_path));
    fill_stat(out, st, h->writable);
    if (!h->directory) out->size = h->file.size();
    return err::kOk;
}

i32 FileSystem::dopen(nx::string_view path) {
    nx::string host_path;
    if (const i32 r = resolve(path, &host_path, nullptr); r < 0) return r;
    const auto type = nx::fs::status(nx::fs::path_view(host_path));
    if (type == nx::fs::file_type::not_found || type == nx::fs::file_type::none) return err::kNoEntry;
    if (type != nx::fs::file_type::directory) return err::kNotDirectory;
    auto entries = nx::fs::directory_entries(nx::fs::path_view(host_path));
    if (!entries) return err::kIo;
    auto h = std::make_shared<Handle>();
    h->directory = true;
    h->host_path = host_path;
    for (const auto& e : *entries) h->entries.push_back(nx::string(nx::fs::path::filename(nx::string_view(e.path))));
    nx::scoped_lock<nx::mutex> guard(lock_);
    const i32 fd = next_fd_++;
    handles_[fd] = std::move(h);
    return fd;
}

i32 FileSystem::dread(i32 fd, io::Dirent* out) {
    auto h = handle(fd);
    if (!h || !h->directory) return err::kBadFile;
    nx::scoped_lock<nx::mutex> guard(h->position_lock);
    std::memset(out, 0, sizeof *out);
    if (h->next_entry >= h->entries.size()) return 0;
    const nx::string& name = h->entries[h->next_entry++];
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(join(h->host_path, name)));
    fill_stat(&out->stat, st, true);
    std::memcpy(out->name, name.data(), std::min<usize>(name.size(), sizeof out->name - 1));
    return 1;
}

i32 FileSystem::dclose(i32 fd) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = handles_.find(fd);
    if (it == handles_.end() || !it->second->directory) return err::kBadFile;
    handles_.erase(it);
    return err::kOk;
}

i32 FileSystem::mkdir(nx::string_view path, i32 /*mode*/) {
    nx::string host_path;
    bool writable = false;
    if (const i32 r = resolve(path, &host_path, &writable); r < 0) return r;
    if (!writable) return err::kReadOnlyFs;
    return to_guest(host::make_directory(host_path));
}

i32 FileSystem::rmdir(nx::string_view path) {
    nx::string host_path;
    bool writable = false;
    if (const i32 r = resolve(path, &host_path, &writable); r < 0) return r;
    if (!writable) return err::kReadOnlyFs;
    return to_guest(host::remove_directory(host_path));
}

i32 FileSystem::remove(nx::string_view path) {
    nx::string host_path;
    bool writable = false;
    if (const i32 r = resolve(path, &host_path, &writable); r < 0) return r;
    if (!writable) return err::kReadOnlyFs;
    return to_guest(host::remove_file(host_path));
}

i32 FileSystem::rename(nx::string_view from, nx::string_view to) {
    nx::string a, b;
    bool wa = false, wb = false;
    if (const i32 r = resolve(from, &a, &wa); r < 0) return r;
    if (const i32 r = resolve(to, &b, &wb); r < 0) return r;
    if (!wa || !wb) return err::kReadOnlyFs;
    const usize da = from.find(':'), db = to.find(':');
    if (!iequal(from.substr(0, da), to.substr(0, db))) return err::kCrossDevice;
    return to_guest(host::rename(a, b));
}

}  // namespace psvrecore::runtime
