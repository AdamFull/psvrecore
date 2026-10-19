#pragma once

// The application's save data (savedata0:) as the application utility keeps it: up to 256 save data slots (their
// parameters in sce_sys/sdslot.dat), and data files written only through it, up to 5 at once and all or none.
//
// A save stages every file it changes (sce_sys/~psvr/), then writes a commit list (sce_sys/~psvr_commit, published
// with one rename: it is there whole or not at all) and carries it out: create the folders, rename the staged files
// over the old ones, remove what is removed. Once carried out the list is removed. A save cut short (the process
// killed, the power gone) before its list is published leaves the old files; after, the next start (recover())
// carries the list out again. The names the runtime uses hold '~', which data paths may not, so they never meet the
// guest's.
//
// A slot given with a save is marked broken first and available once the save is done, as the console does: a save
// cut short before its commit leaves it broken. Space is counted as the console counts it against the save data
// quota (1 GiB): every file rounded up to 32 KiB clusters, 32 KiB per folder, and 768 KiB the system keeps.

#include <span>

#include "psvrecore/runtime/filesystem.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime {

class SaveData {
public:
    static constexpr u32 kSlots = 256;
    static constexpr u32 kMaxItems = 5;
    static constexpr u32 kMaxWrite = 1024 * 1024;  // a save's bytes, in all
    static constexpr u32 kPathMax = 200;            // a data path, its terminator included
    static constexpr u64 kQuotaKiB = 1024 * 1024;
    static constexpr u64 kSystemKiB = 768, kClusterKiB = 32;

    // Guest errors (SCE_APPUTIL_ERROR_*).
    static constexpr i32 kParameter = static_cast<i32>(0x80100600u), kSlotExists = static_cast<i32>(0x80100640u),
                         kSlotNotFound = static_cast<i32>(0x80100641u), kNoSpaceQuota = static_cast<i32>(0x80100642u),
                         kNoSpaceFs = static_cast<i32>(0x80100643u);

    enum : u32 { kAvailable = 0, kBroken = 1 };
    enum SaveMode : u32 { kFile = 0, kFileTruncate = 1, kDirectory = 2 };

    // SceAppUtilSaveDataSlotParam, as the guest lays it out (and sdslot.dat keeps it).
    struct SlotParam {
        u32 status;
        char title[64];
        char subtitle[128];
        char detail[512];
        char icon_path[64];
        i32 user_param;
        u32 size_kib;
        io::DateTime modified;
        u8 reserved[48];
    };
    static_assert(sizeof(SlotParam) == 844);

    struct SaveItem {
        nx::string path;  // inside savedata0:, checked (valid_path)
        std::span<const u8> data;
        u64 offset = 0;
        SaveMode mode = kFile;
    };
    // What a save or removal does to a slot.
    struct SlotUpdate {
        u32 id;
        const SlotParam* param;  // the new parameters (none: the slot's own, or blank for a new slot)
        bool remove = false;     // (removals: the slot goes once the data has)
    };

    // `root` is the host folder of savedata0:; it is made if missing, and a commit cut short is finished.
    explicit SaveData(nx::string root);

    bool ok() const { return ok_; }
    const nx::string& root() const { return root_; }

    // A data path as the console takes it: [A-Za-z0-9-_./], no "." or ".." names, no empty names, up to 4 names
    // of up to 63 characters, up to kPathMax - 1 in all.
    static bool valid_path(nx::string_view path);

    // Slots (the time of a change is set here).
    bool slot(u32 id, SlotParam* out) const;
    i32 create_slot(u32 id, const SlotParam& param);
    i32 set_slot(u32 id, const SlotParam& param);
    i32 delete_slot(u32 id);
    // The ids of the slots in use, in order.
    nx::vector<u32> slots() const;

    // Writes the items (checked: paths valid, at most kMaxItems and kMaxWrite bytes). On kNoSpaceQuota and
    // kNoSpaceFs, `required_kib` is what is missing.
    i32 save(std::span<const SaveItem> items, const SlotUpdate* slot, u32* required_kib);
    // Removes files and folders (with what they hold); every path must exist.
    i32 remove(std::span<const nx::string> paths, const SlotUpdate* slot);

    // The space in use (KiB), counted as the quota counts it.
    u64 used_kib() const;

    // Finishes a commit cut short, or throws away a save that never reached its commit.
    void recover();

private:
    nx::string root_;
    bool ok_ = false;
    bool used_[kSlots] = {};
    SlotParam params_[kSlots] = {};

    nx::string host(nx::string_view path) const;
    bool load_slots();
    // Writes the slot table as it would be to `host_path`.
    bool write_slots(nx::string_view host_path, const bool* used, const SlotParam* params) const;
    i32 store_slots();  // (the table as it is, published with one rename)
    // Publishes the commit list and carries it out.
    i32 commit(const nx::vector<nx::string>& ops);
    bool apply(nx::string_view list);
};

}  // namespace psvrecore::runtime
