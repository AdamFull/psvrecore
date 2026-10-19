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

nx::string lowercase(nx::string_view s) {
    nx::string out;
    for (const char c : s) out.push_back(lower(c));
    return out;
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

// An empty device's root: a folder with nothing in it.
void fill_empty_root(io::Stat* out) {
    std::memset(out, 0, sizeof *out);
    out->mode = static_cast<i32>(io::kModeDirectory | io::kModeRead);
}

nx::string join(nx::string_view a, nx::string_view b) {
    nx::string out(a);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    out += b;
    return out;
}

bool exists(const nx::string& path) { return nx::fs::exists(nx::fs::path_view(path)); }
bool is_directory(const nx::string& path) { return nx::fs::status(nx::fs::path_view(path)) == nx::fs::file_type::directory; }

// The host path of names inside a host folder.
nx::string host_path_of(const nx::string& root, const nx::vector<nx::string>& names) {
    nx::string path = root;
    for (const auto& name : names) {
        nx::string next = join(path, name);
#ifndef _WIN32
        // Case-insensitive names on a case-sensitive host: find the name as the folder spells it.
        if (!exists(next)) {
            if (auto entries = nx::fs::directory_entries(nx::fs::path_view(path))) {
                for (const auto& e : *entries) {
                    const nx::string_view file = nx::fs::path::filename(nx::string_view(e.path));
                    if (iequal(file, name)) {
                        next = join(path, file);
                        break;
                    }
                }
            }
        }
#endif
        path = std::move(next);
    }
    return path;
}

}  // namespace

struct FileSystem::Handle {
    struct Entry {
        nx::string name;
        nx::string folder;  // the host folder it is in (its layer's)
        bool writable = false;
    };
    bool directory = false;
    bool tty = false;
    bool append = false;
    bool writable = false;
    host::File file;
    nx::mutex position_lock;
    u64 position = 0;
    nx::string host_path;
    nx::vector<Entry> entries;  // (directories) read in order
    usize next_entry = 0;
    // On a gated device: the folder at its top this is in (lowercase), which cannot close while this is open.
    nx::string gate_device, gate_folder;
};

// Where a guest path is: its device, the names inside it, and its host path in each layer.
struct FileSystem::Located {
    Mount mount;
    nx::vector<nx::string> names;
    nx::vector<nx::string> paths;  // per layer, the top first (none on an empty device)
    usize layer = 0;               // the first that has it (0 if none does)
    bool found = false;
    bool empty_root() const { return paths.empty() && names.empty(); }
    const nx::string& path() const { return paths[layer]; }
    bool writable() const { return mount.writable && layer == 0; }
};

FileSystem::FileSystem(const Config& config) {
    for (const auto& m : config.mounts) mount(m);
    for (const i32 fd : {kStdin, kStdout, kStderr}) {
        auto h = std::make_shared<Handle>();
        h->tty = true;
        handles_[fd] = std::move(h);
    }
    // The console has one tty for both streams (and libc writes both to the same one), so does the host: its
    // standard output.
    sink_ = Sink([](i32, nx::string_view text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
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

i32 FileSystem::locate(nx::string_view guest, Located* out) const {
    const usize colon = guest.find(':');
    if (colon == nx::string_view::npos || colon == 0) return err::kNoEntry;
    const nx::string_view device = guest.substr(0, colon);
    {
        nx::scoped_lock<nx::mutex> guard(lock_);
        const Mount* mount = nullptr;
        for (const auto& m : mounts_)
            if (iequal(m.device, device)) mount = &m;
        if (!mount) return err::kNoEntry;
        out->mount = *mount;
    }
    // The names inside the device, with "." and ".." resolved (never above its root).
    nx::string_view rest = guest.substr(colon + 1);
    while (!rest.empty()) {
        const usize slash = rest.find_first_of("/\\");
        const nx::string_view name = rest.substr(0, slash);
        rest = slash == nx::string_view::npos ? nx::string_view{} : rest.substr(slash + 1);
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            if (!out->names.empty()) out->names.pop_back();
            continue;
        }
        if (name.size() > 255) return err::kNameTooLong;
        out->names.emplace_back(name);
    }
    const Mount& m = out->mount;
    // Inside a gated device's folder: only while it is open.
    if (m.gated && out->names.size() > 1 && !folder_open(m.device, out->names[0])) return err::kNoEntry;
    if (m.host.empty()) return out->names.empty() ? err::kOk : err::kNoEntry;
    out->paths.push_back(host_path_of(m.host, out->names));
    for (const auto& under : m.under) out->paths.push_back(host_path_of(under, out->names));
    for (usize i = 0; i < out->paths.size(); ++i)
        if (exists(out->paths[i])) {
            out->layer = i;
            out->found = true;
            break;
        }
    return err::kOk;
}

i32 FileSystem::resolve(nx::string_view guest, nx::string* host_path, bool* writable, bool must_exist) const {
    Located at;
    if (const i32 r = locate(guest, &at); r < 0) return r;
    if (at.empty_root()) return err::kNoEntry;  // (no host folder to give)
    if (must_exist && !at.found) return err::kNoEntry;
    if (host_path) *host_path = at.path();
    if (writable) *writable = at.writable();
    return err::kOk;
}

i32 FileSystem::open_folder(nx::string_view device, nx::string_view folder) {
    Located at;
    if (const i32 r = locate(nx::format("{}:{}", device, folder), &at); r < 0) return r;
    if (!at.mount.gated || at.names.size() != 1 || !at.found || !is_directory(at.path())) return err::kNoEntry;
    nx::scoped_lock<nx::mutex> guard(lock_);
    const auto key = std::make_pair(lowercase(at.mount.device), lowercase(at.names[0]));
    for (const auto& f : open_folders_)
        if (f == key) return err::kAlready;
    open_folders_.push_back(key);
    return err::kOk;
}

i32 FileSystem::close_folder(nx::string_view device, nx::string_view folder) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    const auto key = std::make_pair(lowercase(device), lowercase(folder));
    for (usize i = 0; i < open_folders_.size(); ++i) {
        if (open_folders_[i] != key) continue;
        for (const auto& [fd, h] : handles_)
            if (h->gate_device == key.first && h->gate_folder == key.second) return err::kBusy;
        open_folders_.erase(open_folders_.begin() + static_cast<isize>(i));
        return err::kOk;
    }
    return err::kNoEntry;
}

bool FileSystem::folder_open(nx::string_view device, nx::string_view folder) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    const auto key = std::make_pair(lowercase(device), lowercase(folder));
    for (const auto& f : open_folders_)
        if (f == key) return true;
    return false;
}

std::shared_ptr<FileSystem::Handle> FileSystem::handle(i32 fd) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = handles_.find(fd);
    return it == handles_.end() ? nullptr : it->second;
}

i32 FileSystem::add_handle(std::shared_ptr<Handle> h) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    const i32 fd = next_fd_++;
    handles_[fd] = std::move(h);
    return fd;
}

i32 FileSystem::open(nx::string_view path, u32 flags, i32 /*mode*/) {
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    if (at.empty_root()) return err::kIsDirectory;
    const bool wants_write = flags & (io::kWrite | io::kCreate | io::kTruncate | io::kAppend);
    if (wants_write && !at.writable()) return err::kReadOnlyFs;
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
    host::File file = host::File::open(at.path(), options, &error);
    if (!file.valid()) return to_guest(error);
    auto h = std::make_shared<Handle>();
    h->file = std::move(file);
    h->append = options.append;
    h->writable = options.mode != host::OpenMode::Read;
    h->host_path = at.path();
    if (at.mount.gated) {
        h->gate_device = lowercase(at.mount.device);
        h->gate_folder = lowercase(at.names[0]);
    }
    return add_handle(std::move(h));
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
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    if (at.empty_root()) {
        fill_empty_root(out);
        return err::kOk;
    }
    if (!at.found) return err::kNoEntry;
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(at.path()));
    if (st.type == nx::fs::file_type::not_found || st.type == nx::fs::file_type::none) return err::kNoEntry;
    fill_stat(out, st, at.writable());
    return err::kOk;
}

i32 FileSystem::stat_fd(i32 fd, io::Stat* out) {
    auto h = handle(fd);
    if (!h) return err::kBadFile;
    if (h->tty) {
        std::memset(out, 0, sizeof *out);
        return err::kOk;
    }
    if (h->directory && h->host_path.empty()) {
        fill_empty_root(out);
        return err::kOk;
    }
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(h->host_path));
    fill_stat(out, st, h->writable);
    if (!h->directory) out->size = h->file.size();
    return err::kOk;
}

i32 FileSystem::dopen(nx::string_view path) {
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    auto h = std::make_shared<Handle>();
    h->directory = true;
    if (at.empty_root()) return add_handle(std::move(h));
    if (!at.found) return err::kNoEntry;
    const Mount& m = at.mount;
    if (m.gated && !at.names.empty()) {
        if (!folder_open(m.device, at.names[0])) return err::kNoEntry;
        h->gate_device = lowercase(m.device);
        h->gate_folder = lowercase(at.names[0]);
    }
    // Every layer's entries, the first layer's for a name more than one has.
    bool any_directory = false;
    for (usize layer = at.layer; layer < at.paths.size(); ++layer) {
        const nx::string& folder = at.paths[layer];
        if (!is_directory(folder)) continue;
        if (!any_directory) h->host_path = folder;
        any_directory = true;
        auto entries = nx::fs::directory_entries(nx::fs::path_view(folder));
        if (!entries) return err::kIo;
        for (const auto& e : *entries) {
            const nx::string_view name = nx::fs::path::filename(nx::string_view(e.path));
            bool seen = false;
            for (const auto& had : h->entries) seen = seen || iequal(had.name, name);
            if (!seen) h->entries.push_back({nx::string(name), folder, m.writable && layer == 0});
        }
    }
    if (!any_directory) return err::kNotDirectory;
    return add_handle(std::move(h));
}

i32 FileSystem::dread(i32 fd, io::Dirent* out) {
    auto h = handle(fd);
    if (!h || !h->directory) return err::kBadFile;
    nx::scoped_lock<nx::mutex> guard(h->position_lock);
    std::memset(out, 0, sizeof *out);
    if (h->next_entry >= h->entries.size()) return 0;
    const Handle::Entry& e = h->entries[h->next_entry++];
    const nx::fs::file_stat st = nx::fs::stat_file(nx::fs::path_view(join(e.folder, e.name)));
    fill_stat(&out->stat, st, e.writable);
    std::memcpy(out->name, e.name.data(), std::min<usize>(e.name.size(), sizeof out->name - 1));
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
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    if (at.empty_root() || !at.mount.writable) return err::kReadOnlyFs;
    if (at.found && at.layer != 0) return err::kExists;  // (a lower layer's)
    return to_guest(host::make_directory(at.path()));
}

i32 FileSystem::rmdir(nx::string_view path) {
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    if (at.empty_root() || !at.writable()) return err::kReadOnlyFs;
    return to_guest(host::remove_directory(at.path()));
}

i32 FileSystem::remove(nx::string_view path) {
    Located at;
    if (const i32 r = locate(path, &at); r < 0) return r;
    if (at.empty_root() || !at.writable()) return err::kReadOnlyFs;
    return to_guest(host::remove_file(at.path()));
}

i32 FileSystem::rename(nx::string_view from, nx::string_view to) {
    Located a, b;
    if (const i32 r = locate(from, &a); r < 0) return r;
    if (const i32 r = locate(to, &b); r < 0) return r;
    if (a.empty_root() || b.empty_root() || !a.writable() || !b.writable()) return err::kReadOnlyFs;
    if (!iequal(a.mount.device, b.mount.device)) return err::kCrossDevice;
    return to_guest(host::rename(a.path(), b.path()));
}

}  // namespace psvrecore::runtime
