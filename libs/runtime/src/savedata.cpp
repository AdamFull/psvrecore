#include "psvrecore/runtime/savedata.hpp"

#include <chrono>
#include <cstring>
#include <ctime>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/errors.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/platform/filesystem.h"

namespace psvrecore::runtime {

namespace {

constexpr char kSlotFile[] = "sce_sys/sdslot.dat";
constexpr char kStaging[] = "sce_sys/~psvr";
constexpr char kCommit[] = "sce_sys/~psvr_commit";
constexpr char kCommitNew[] = "sce_sys/~psvr_commit~new";
constexpr char kMagic[8] = {'P', 'S', 'V', 'R', 'S', 'D', 'S', 'L'};
constexpr u32 kVersion = 1;
constexpr u64 kCluster = SaveData::kClusterKiB * 1024;

u64 clusters_kib(u64 bytes) { return (bytes + kCluster - 1) / kCluster * SaveData::kClusterKiB; }

bool is_file(nx::string_view path) { return nx::fs::status(nx::fs::path_view(path)) == nx::fs::file_type::regular; }
bool is_directory(nx::string_view path) {
    return nx::fs::status(nx::fs::path_view(path)) == nx::fs::file_type::directory;
}
u64 file_size(nx::string_view path) { return nx::fs::stat_file(nx::fs::path_view(path)).size; }

nx::string parent_of(nx::string_view path) {
    const usize slash = path.rfind('/');
    return slash == nx::string_view::npos ? nx::string{} : nx::string(path.substr(0, slash));
}

// The local time now (the slots' modification times are local, as the console's clock is).
io::DateTime local_now() {
    using namespace std::chrono;
    const u64 unix_us = static_cast<u64>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
    const std::time_t t = static_cast<std::time_t>(unix_us / 1'000'000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    io::DateTime d{};
    d.year = static_cast<u16>(local.tm_year + 1900);
    d.month = static_cast<u16>(local.tm_mon + 1);
    d.day = static_cast<u16>(local.tm_mday);
    d.hour = static_cast<u16>(local.tm_hour);
    d.minute = static_cast<u16>(local.tm_min);
    d.second = static_cast<u16>(local.tm_sec);
    d.microsecond = static_cast<u32>(unix_us % 1'000'000);
    return d;
}

// Writes `data` as the whole of a new file, flushed to the device.
host::FileError write_file(nx::string_view path, const void* data, usize size) {
    host::OpenOptions o;
    o.mode = host::OpenMode::Write;
    o.create = o.truncate = true;
    host::FileError e = host::FileError::None;
    host::File f = host::File::open(path, o, &e);
    if (!f.valid()) return e;
    if (size && f.write_at(0, data, size) != static_cast<i64>(size)) return host::FileError::NoSpace;
    return f.flush() ? host::FileError::None : host::FileError::Other;
}

i32 to_guest(host::FileError e) { return e == host::FileError::NoSpace ? SaveData::kNoSpaceFs : err::kIo; }

}  // namespace

SaveData::SaveData(nx::string root) : root_(std::move(root)) {
    if (!nx::fs::create_directories(nx::fs::path_view(host("sce_sys")))) {
        nx::loge(logs::io, "save data: cannot make the folder '{}'", root_);
        return;
    }
    recover();
    ok_ = load_slots();
}

nx::string SaveData::host(nx::string_view path) const {
    nx::string out = root_;
    if (!path.empty()) {
        out += '/';
        out += path;
    }
    return out;
}

bool SaveData::valid_path(nx::string_view path) {
    if (path.empty() || path.size() >= kPathMax) return false;
    u32 names = 0;
    usize start = 0;
    for (usize i = 0; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            const char c = path[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.';
            if (!ok) return false;
            continue;
        }
        const nx::string_view name = path.substr(start, i - start);
        if (name.empty() || name == "." || name == ".." || name.size() > 63 || ++names > 4) return false;
        start = i + 1;
    }
    return true;
}

// ---- slots

bool SaveData::load_slots() {
    std::memset(used_, 0, sizeof used_);
    const nx::string path = host(kSlotFile);
    if (!is_file(path)) return true;
    auto data = nx::fs::file_read(nx::fs::path_view(path));
    if (!data) {
        nx::loge(logs::io, "save data: cannot read '{}'", path);
        return false;
    }
    const u8* p = data->data();
    const usize size = data->size();
    u32 version = 0, count = 0;
    if (size < 16 || std::memcmp(p, kMagic, 8) != 0) {
        nx::loge(logs::io, "save data: '{}' is not a slot table", path);
        return false;
    }
    std::memcpy(&version, p + 8, 4);
    std::memcpy(&count, p + 12, 4);
    constexpr usize kRecord = 4 + sizeof(SlotParam);
    if (version != kVersion || count > kSlots || size < 16 + count * kRecord) {
        nx::loge(logs::io, "save data: '{}' is not a slot table this runtime reads", path);
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        u32 id;
        std::memcpy(&id, p + 16 + i * kRecord, 4);
        if (id >= kSlots) continue;
        used_[id] = true;
        std::memcpy(&params_[id], p + 16 + i * kRecord + 4, sizeof(SlotParam));
    }
    return true;
}

bool SaveData::write_slots(nx::string_view host_path, const bool* used, const SlotParam* params) const {
    nx::vector<u8> out;
    u32 count = 0;
    for (u32 i = 0; i < kSlots; ++i) count += used[i] ? 1 : 0;
    out.resize(16 + count * (4 + sizeof(SlotParam)));
    std::memcpy(out.data(), kMagic, 8);
    std::memcpy(out.data() + 8, &kVersion, 4);
    std::memcpy(out.data() + 12, &count, 4);
    u8* p = out.data() + 16;
    for (u32 i = 0; i < kSlots; ++i) {
        if (!used[i]) continue;
        std::memcpy(p, &i, 4);
        std::memcpy(p + 4, &params[i], sizeof(SlotParam));
        p += 4 + sizeof(SlotParam);
    }
    return write_file(host_path, out.data(), out.size()) == host::FileError::None;
}

i32 SaveData::store_slots() {
    if (!nx::fs::create_directories(nx::fs::path_view(host(kStaging)))) return err::kIo;
    const nx::string staged = nx::string(kStaging) + "/slots";
    if (!write_slots(host(staged), used_, params_)) return err::kIo;
    nx::vector<nx::string> ops;
    ops.push_back("move " + staged + " " + kSlotFile);
    return commit(ops);
}

bool SaveData::slot(u32 id, SlotParam* out) const {
    if (id >= kSlots || !used_[id]) return false;
    if (out) *out = params_[id];
    return true;
}

nx::vector<u32> SaveData::slots() const {
    nx::vector<u32> out;
    for (u32 i = 0; i < kSlots; ++i)
        if (used_[i]) out.push_back(i);
    return out;
}

i32 SaveData::create_slot(u32 id, const SlotParam& param) {
    if (id >= kSlots) return kParameter;
    if (used_[id]) return kSlotExists;
    used_[id] = true;
    params_[id] = param;
    params_[id].modified = local_now();
    if (const i32 r = store_slots(); r < 0) {
        used_[id] = false;
        return r;
    }
    return 0;
}

i32 SaveData::set_slot(u32 id, const SlotParam& param) {
    if (id >= kSlots) return kParameter;
    if (!used_[id]) return kSlotNotFound;
    const SlotParam old = params_[id];
    params_[id] = param;
    params_[id].modified = local_now();
    if (const i32 r = store_slots(); r < 0) {
        params_[id] = old;
        return r;
    }
    return 0;
}

i32 SaveData::delete_slot(u32 id) {
    if (id >= kSlots) return kParameter;
    if (!used_[id]) return kSlotNotFound;
    used_[id] = false;
    if (const i32 r = store_slots(); r < 0) {
        used_[id] = true;
        return r;
    }
    return 0;
}

// ---- space

u64 SaveData::used_kib() const {
    u64 kib = kSystemKiB;
    auto entries = nx::fs::recursive_directory_entries(nx::fs::path_view(root_));
    if (!entries) return kib;
    for (const auto& e : *entries) {
        nx::string_view rel(e.path);
        if (rel.size() > root_.size() && rel.substr(0, root_.size()) == nx::string_view(root_)) rel = rel.substr(root_.size() + 1);
        nx::string name(rel);
        for (char& c : name)
            if (c == '\\') c = '/';
        // (the system's own: sce_sys itself, the slot table and the runtime's staging)
        if (name == "sce_sys" || name == kSlotFile || name.find('~') != nx::string::npos) continue;
        if (e.type == nx::fs::file_type::directory) kib += kClusterKiB;
        else kib += clusters_kib(nx::fs::stat_file(nx::fs::path_view(e.path)).size);
    }
    return kib;
}

// ---- commits

i32 SaveData::commit(const nx::vector<nx::string>& ops) {
    nx::string list;
    for (const auto& op : ops) {
        list += op;
        list += '\n';
    }
    if (const host::FileError e = write_file(host(kCommitNew), list.data(), list.size()); e != host::FileError::None)
        return to_guest(e);
    // (the list is published whole by this rename; from here on the commit happens, now or at the next start)
    if (host::rename(host(kCommitNew), host(kCommit)) != host::FileError::None) return err::kIo;
    const bool done = apply(list);
    (void)host::remove_file(host(kCommit));
    (void)nx::fs::remove_all(nx::fs::path_view(host(kStaging)));
    return done ? 0 : err::kIo;
}

bool SaveData::apply(nx::string_view list) {
    bool ok = true;
    while (!list.empty()) {
        const usize end = list.find('\n');
        const nx::string_view line = list.substr(0, end);
        list = end == nx::string_view::npos ? nx::string_view{} : list.substr(end + 1);
        if (line.empty()) continue;
        const usize space = line.find(' ');
        const nx::string_view op = line.substr(0, space);
        const nx::string_view args = space == nx::string_view::npos ? nx::string_view{} : line.substr(space + 1);
        if (op == "dir") {
            ok &= static_cast<bool>(nx::fs::create_directories(nx::fs::path_view(host(args))));
        } else if (op == "move") {
            const usize split = args.find(' ');
            const nx::string from = host(args.substr(0, split)), to = host(args.substr(split + 1));
            // (carried out already, if the staged file is gone)
            if (!is_file(from)) continue;
            const nx::string parent = parent_of(to);
            if (!parent.empty() && !is_directory(parent))
                ok &= static_cast<bool>(nx::fs::create_directories(nx::fs::path_view(parent)));
            ok &= host::rename(from, to) == host::FileError::None;
        } else if (op == "remove") {
            const nx::string path = host(args);
            if (nx::fs::exists(nx::fs::path_view(path))) ok &= static_cast<bool>(nx::fs::remove_all(nx::fs::path_view(path)));
        } else {
            nx::loge(logs::io, "save data: unknown commit step '{}'", line);
            ok = false;
        }
    }
    return ok;
}

void SaveData::recover() {
    const nx::string list = host(kCommit);
    if (is_file(list)) {
        if (auto text = nx::fs::file_read_text(nx::fs::path_view(list))) {
            nx::logi(logs::io, "save data: finishing a save cut short");
            apply(nx::string_view(*text));
        }
        (void)host::remove_file(list);
    }
    // (a save that never reached its commit leaves only staged files, and an unpublished list)
    (void)host::remove_file(host(kCommitNew));
    if (nx::fs::exists(nx::fs::path_view(host(kStaging)))) (void)nx::fs::remove_all(nx::fs::path_view(host(kStaging)));
}

// ---- data

i32 SaveData::save(std::span<const SaveItem> items, const SlotUpdate* slot, u32* required_kib) {
    if (required_kib) *required_kib = 0;
    if (items.empty() || items.size() > kMaxItems || (slot && slot->id >= kSlots)) return kParameter;
    u64 total = 0;
    for (const SaveItem& item : items) {
        if (!valid_path(item.path) || item.mode > kDirectory) return kParameter;
        if (item.mode == kDirectory && (!item.data.empty() || item.offset != 0)) return kParameter;
        total += item.data.size();
        const nx::string path = host(item.path);
        if (item.mode == kDirectory ? is_file(path) : is_directory(path)) return kParameter;
    }
    if (total > kMaxWrite) return kParameter;

    // The space after the save: new folders, and files' new sizes (an item may follow another of the same file).
    struct Staged {
        nx::string path;
        u64 old_size;
        bool existed;
        u64 size;
    };
    nx::vector<Staged> files;
    nx::vector<nx::string> new_dirs;
    i64 delta = 0;
    auto add_dir = [&](const nx::string& dir) {
        if (is_directory(host(dir))) return;
        for (const auto& d : new_dirs)
            if (d == dir) return;
        new_dirs.push_back(dir);
        delta += static_cast<i64>(kClusterKiB);
    };
    for (const SaveItem& item : items) {
        // (every folder on the way)
        for (usize slash = item.path.find('/'); slash != nx::string::npos; slash = item.path.find('/', slash + 1))
            add_dir(nx::string(nx::string_view(item.path).substr(0, slash)));
        if (item.mode == kDirectory) {
            add_dir(item.path);
            continue;
        }
        Staged* f = nullptr;
        for (auto& s : files)
            if (s.path == item.path) f = &s;
        if (!f) {
            const nx::string path = host(item.path);
            const bool existed = is_file(path);
            const u64 size = existed ? file_size(path) : 0;
            files.push_back({item.path, size, existed, size});
            f = &files.back();
        }
        const u64 end = item.offset + item.data.size();
        f->size = item.mode == kFile ? nx::max(f->size, end) : end;
    }
    for (const auto& f : files)
        delta += static_cast<i64>(clusters_kib(f.size)) - static_cast<i64>(f.existed ? clusters_kib(f.old_size) : 0);
    const i64 after = static_cast<i64>(used_kib()) + delta;
    if (after > static_cast<i64>(kQuotaKiB)) {
        if (required_kib) *required_kib = static_cast<u32>(after - static_cast<i64>(kQuotaKiB));
        return kNoSpaceQuota;
    }

    // The slot is broken until the save is done.
    bool slot_used[kSlots];
    SlotParam slot_params[kSlots];
    if (slot) {
        if (!used_[slot->id]) {
            used_[slot->id] = true;
            params_[slot->id] = slot->param ? *slot->param : SlotParam{};
        }
        params_[slot->id].status = kBroken;
        if (const i32 r = store_slots(); r < 0) return r;
        std::memcpy(slot_used, used_, sizeof used_);
        std::memcpy(slot_params, params_, sizeof params_);
        if (slot->param) slot_params[slot->id] = *slot->param;
        slot_params[slot->id].status = kAvailable;
        slot_params[slot->id].modified = local_now();
    }

    // Stage every file as it will be.
    auto fail = [&](i32 r) {
        (void)nx::fs::remove_all(nx::fs::path_view(host(kStaging)));
        if (r == kNoSpaceFs && required_kib) *required_kib = static_cast<u32>(nx::max<i64>(delta, 1));
        return r;
    };
    if (!nx::fs::create_directories(nx::fs::path_view(host(kStaging)))) return err::kIo;
    nx::vector<nx::string> ops;
    for (const auto& d : new_dirs) ops.push_back("dir " + d);
    for (usize i = 0; i < files.size(); ++i) {
        const nx::string staged = nx::format("{}/{}", kStaging, i);
        if (files[i].existed &&
            !nx::fs::copy_file(nx::fs::path_view(host(files[i].path)), nx::fs::path_view(host(staged)), true))
            return fail(err::kIo);
        host::OpenOptions o;
        o.mode = host::OpenMode::ReadWrite;
        o.create = true;
        host::FileError e = host::FileError::None;
        host::File f = host::File::open(host(staged), o, &e);
        if (!f.valid()) return fail(to_guest(e));
        for (const SaveItem& item : items) {
            if (item.mode == kDirectory || item.path != files[i].path) continue;
            if (!item.data.empty() &&
                f.write_at(item.offset, item.data.data(), item.data.size()) != static_cast<i64>(item.data.size()))
                return fail(kNoSpaceFs);
            const u64 end = item.offset + item.data.size();
            const u64 size = static_cast<u64>(f.size());
            if (item.mode == kFileTruncate ? size != end : size < end)
                if (!f.resize(end)) return fail(kNoSpaceFs);
        }
        if (!f.flush()) return fail(err::kIo);
        ops.push_back("move " + staged + " " + files[i].path);
    }
    if (slot) {
        const nx::string staged = nx::string(kStaging) + "/slots";
        if (!write_slots(host(staged), slot_used, slot_params)) return fail(err::kIo);
        ops.push_back("move " + staged + " " + kSlotFile);
    }
    if (const i32 r = commit(ops); r < 0) return r;
    if (slot) params_[slot->id] = slot_params[slot->id];
    return 0;
}

i32 SaveData::remove(std::span<const nx::string> paths, const SlotUpdate* slot) {
    if (paths.empty() || paths.size() > kMaxItems || (slot && slot->id >= kSlots)) return kParameter;
    for (const auto& p : paths) {
        if (!valid_path(p)) return kParameter;
        if (!nx::fs::exists(nx::fs::path_view(host(p)))) return err::kNoEntry;
    }
    bool slot_used[kSlots];
    SlotParam slot_params[kSlots];
    const bool slot_kept = slot && !slot->remove;
    if (slot && (used_[slot->id] || slot_kept)) {
        if (!used_[slot->id]) {
            used_[slot->id] = true;
            params_[slot->id] = slot->param ? *slot->param : SlotParam{};
        }
        params_[slot->id].status = kBroken;
        if (const i32 r = store_slots(); r < 0) return r;
        std::memcpy(slot_used, used_, sizeof used_);
        std::memcpy(slot_params, params_, sizeof params_);
        if (slot_kept) {
            if (slot->param) slot_params[slot->id] = *slot->param;
            slot_params[slot->id].status = kAvailable;
            slot_params[slot->id].modified = local_now();
        } else {
            slot_used[slot->id] = false;
        }
    } else {
        slot = nullptr;  // (no slot to delete)
    }
    nx::vector<nx::string> ops;
    for (const auto& p : paths) ops.push_back("remove " + p);
    if (slot) {
        if (!nx::fs::create_directories(nx::fs::path_view(host(kStaging)))) return err::kIo;
        const nx::string staged = nx::string(kStaging) + "/slots";
        if (!write_slots(host(staged), slot_used, slot_params)) return err::kIo;
        ops.push_back("move " + staged + " " + kSlotFile);
    }
    if (const i32 r = commit(ops); r < 0) return r;
    if (slot) {
        used_[slot->id] = slot_used[slot->id];
        params_[slot->id] = slot_params[slot->id];
    }
    return 0;
}

}  // namespace psvrecore::runtime
