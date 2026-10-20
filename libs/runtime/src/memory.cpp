#include "psvrecore/runtime/memory.hpp"

#include <algorithm>
#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/recomp/guest.h"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/platform/os.h"

namespace psvrecore::runtime {

namespace {

constexpr u64 kGuestSpace = 1ull << 32;
constexpr u64 kGuard = 1ull << 20;  // past 4 GiB: an access at the top of the space that runs over its end

u64 host_page() { return nx::vmem::get_page_size(); }

nx::vmem::MemProtFlags host_protection(Access access) {
    switch (access) {
        case Access::None: return nx::vmem::MemProtFlag::None;
        case Access::Read: return nx::vmem::MemProtFlag::Read;
        case Access::ReadWrite: break;
    }
    return nx::vmem::MemProtFlag::Read | nx::vmem::MemProtFlag::Write;
}

}  // namespace

GuestMemory::~GuestMemory() { shutdown(); }

bool GuestMemory::init() {
    if (base_) return true;
    void* reservation = nullptr;
    const usize size = static_cast<usize>(kGuestSpace + kGuard);
    if (!nx::vmem::reserve(&reservation, size)) return false;
    base_ = static_cast<u8*>(reservation);
    reserved_ = size;
    pages_ = nx::make_unique<Pages>();
    for (std::atomic<u8>& a : pages_->access) a.store(0, std::memory_order_relaxed);
    psv_membase = base_;
    return true;
}

void GuestMemory::shutdown() {
    if (!base_) return;
    {
        nx::scoped_lock<nx::mutex> guard(lock_);
        for (const auto& [base, block] : blocks_) set_pages(block.base, block.size, Access::None);
        blocks_.clear();
    }
    if (psv_membase == base_) psv_membase = nullptr;
    nx::vmem::release(base_, reserved_);
    base_ = nullptr;
    reserved_ = 0;
}

bool GuestMemory::commit(u32 base, u32 size, Access access) {
    // Host pages can be larger than the guest's: a host page shared with a neighbouring block keeps read/write
    // access (only pages wholly inside the block get the block's protection).
    const u64 page = host_page();
    const u64 begin = base & ~(page - 1), end = (u64{base} + size + page - 1) & ~(page - 1);
    if (!nx::vmem::commit(base_ + begin, static_cast<usize>(end - begin))) return false;
    // Pages wholly inside the block come fresh (zero) from the host: unmap gives every such page back. Only the
    // ends can share a host page with another block, and still hold what an earlier block there left.
    const u64 block_end = u64{base} + size;
    const u64 head_end = std::min<u64>(block_end, (u64{base} + page - 1) & ~(page - 1));
    if (head_end > base) std::memset(base_ + base, 0, static_cast<usize>(head_end - base));
    const u64 tail_begin = std::max<u64>(head_end, block_end & ~(page - 1));
    if (block_end > tail_begin) std::memset(base_ + tail_begin, 0, static_cast<usize>(block_end - tail_begin));
    if (access != Access::ReadWrite) {
        const u64 inner_begin = (u64{base} + page - 1) & ~(page - 1), inner_end = (u64{base} + size) & ~(page - 1);
        if (inner_end > inner_begin)
            nx::vmem::protect(base_ + inner_begin, static_cast<usize>(inner_end - inner_begin), host_protection(access));
    }
    return true;
}

void GuestMemory::set_pages(u32 base, u32 size, Access access) {
    if (!pages_) return;
    const u64 end = (u64{base} + size + kPageSize - 1) / kPageSize;
    for (u64 p = base / kPageSize; p < end && p < kGuestSpace / kPageSize; ++p)
        pages_->access[p].store(static_cast<u8>(access), std::memory_order_release);
}

bool GuestMemory::overlaps(u32 base, u32 size) const {
    const u64 end = u64{base} + size;
    auto it = blocks_.upper_bound(base);
    if (it != blocks_.end() && it->first < end) return true;
    if (it != blocks_.begin()) {
        --it;
        if (u64{it->second.base} + it->second.size > base) return true;
    }
    return false;
}

bool GuestMemory::map_fixed(u32 base, u32 size, Access access, BlockKind kind, nx::string_view name) {
    if (!base_ || size == 0 || u64{base} + size > kGuestSpace || base % kPageSize) return false;
    size = (size + kPageSize - 1) & ~(kPageSize - 1);
    nx::scoped_lock<nx::mutex> guard(lock_);
    if (overlaps(base, size)) return false;
    if (!commit(base, size, access)) return false;
    blocks_[base] = Block{base, size, kind, access, nx::string(name), 0, 0};
    set_pages(base, size, access);
    return true;
}

u32 GuestMemory::map_anywhere(const Region& region, u32 size, u32 alignment, Access access, BlockKind kind,
                              nx::string_view name, i32 uid, u32 type) {
    if (!base_ || size == 0) return 0;
    if (alignment < kPageSize) alignment = kPageSize;
    size = (size + kPageSize - 1) & ~(kPageSize - 1);
    nx::scoped_lock<nx::mutex> guard(lock_);
    // First fit, lowest address first.
    u64 candidate = (u64{region.begin} + alignment - 1) & ~(u64{alignment} - 1);
    auto it = blocks_.upper_bound(region.begin);
    if (it != blocks_.begin()) {
        auto before = std::prev(it);
        const u64 before_end = u64{before->second.base} + before->second.size;
        if (before_end > candidate) candidate = (before_end + alignment - 1) & ~(u64{alignment} - 1);
    }
    for (;;) {
        if (candidate + size > region.end) return 0;
        while (it != blocks_.end() && u64{it->second.base} + it->second.size <= candidate) ++it;
        u64 next = 0;
        if (it != blocks_.end() && candidate + size > it->second.base) next = u64{it->second.base} + it->second.size;
        for (const Region& r : reserved_ranges_)
            if (candidate < r.end && candidate + size > r.begin) next = std::max<u64>(next, r.end);
        if (!next) break;
        candidate = (next + alignment - 1) & ~(u64{alignment} - 1);
    }
    const u32 base = static_cast<u32>(candidate);
    if (!commit(base, size, access)) return 0;
    blocks_[base] = Block{base, size, kind, access, nx::string(name), uid, type};
    set_pages(base, size, access);
    return base;
}

void GuestMemory::reserve(u32 base, u32 size) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    const u32 begin = base & ~(kPageSize - 1);
    const u32 end = static_cast<u32>(std::min<u64>((u64{base} + size + kPageSize - 1) & ~u64{kPageSize - 1}, 0xFFFFFFFFu));
    reserved_ranges_.push_back(Region{begin, end});
}

bool GuestMemory::unmap(u32 base) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = blocks_.find(base);
    if (it == blocks_.end()) return false;
    const u64 page = host_page();
    const u64 begin = it->second.base, end = u64{it->second.base} + it->second.size;
    set_pages(it->second.base, it->second.size, Access::None);
    blocks_.erase(it);
    // Give back the host pages no other block touches.
    u64 free_begin = (begin + page - 1) & ~(page - 1), free_end = end & ~(page - 1);
    if ((begin & (page - 1)) && !overlaps(static_cast<u32>(begin & ~(page - 1)), static_cast<u32>(page))) free_begin = begin & ~(page - 1);
    if ((end & (page - 1)) && end < kGuestSpace && !overlaps(static_cast<u32>(end & ~(page - 1)), static_cast<u32>(page)))
        free_end = (end + page - 1) & ~(page - 1);
    if (free_end > free_begin) host::decommit(base_ + free_begin, static_cast<usize>(free_end - free_begin));
    return true;
}

bool GuestMemory::protect(u32 base, Access access) {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = blocks_.find(base);
    if (it == blocks_.end()) return false;
    const u64 page = host_page();
    const u64 inner_begin = (u64{base} + page - 1) & ~(page - 1), inner_end = (u64{base} + it->second.size) & ~(page - 1);
    if (inner_end > inner_begin)
        nx::vmem::protect(base_ + inner_begin, static_cast<usize>(inner_end - inner_begin), host_protection(access));
    it->second.access = access;
    set_pages(it->second.base, it->second.size, access);
    return true;
}

bool GuestMemory::find(u32 address, Block* out) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    auto it = blocks_.upper_bound(address);
    if (it == blocks_.begin()) return false;
    --it;
    if (address - it->second.base >= it->second.size) return false;
    if (out) *out = it->second;
    return true;
}

bool GuestMemory::find_uid(i32 uid, Block* out) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    for (const auto& [base, block] : blocks_)
        if (block.uid == uid && block.kind == BlockKind::MemBlock) {
            if (out) *out = block;
            return true;
        }
    return false;
}

u64 GuestMemory::mapped_bytes(BlockKind kind) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    u64 total = 0;
    for (const auto& [base, block] : blocks_)
        if (block.kind == kind) total += block.size;
    return total;
}

u64 GuestMemory::mapped_bytes(BlockKind kind, const Region& region) const {
    nx::scoped_lock<nx::mutex> guard(lock_);
    u64 total = 0;
    for (const auto& [base, block] : blocks_)
        if (block.kind == kind && base >= region.begin && base < region.end) total += block.size;
    return total;
}

bool GuestMemory::valid(u32 address, u32 size, bool write) const {
    if (size == 0) return true;
    if (pages_) {
        // (by page, without the lock: blocks are whole pages)
        const u64 end = (u64{address} + size - 1) / kPageSize;
        if (end >= kGuestSpace / kPageSize) return false;
        const u8 want = static_cast<u8>(write ? Access::ReadWrite : Access::Read);
        for (u64 p = address / kPageSize; p <= end; ++p)
            if ((pages_->access[p].load(std::memory_order_acquire) & want) != want) return false;
        return true;
    }
    nx::scoped_lock<nx::mutex> guard(lock_);
    u64 at = address;
    const u64 end = u64{address} + size;
    while (at < end) {
        auto it = blocks_.upper_bound(static_cast<u32>(at));
        if (it == blocks_.begin()) return false;
        --it;
        const u64 block_end = u64{it->second.base} + it->second.size;
        if (at >= block_end) return false;
        if (write && it->second.access != Access::ReadWrite) return false;
        if (it->second.access == Access::None) return false;
        at = block_end;
    }
    return true;
}

nx::string GuestMemory::string(u32 address, u32 max) const {
    nx::string out;
    if (!address) return out;
    for (u32 i = 0; i < max; ++i) {
        const u32 at = address + i;
        if ((i == 0 || (at % kPageSize) == 0) && !valid(at, 1)) break;
        const char c = static_cast<char>(base_[at]);
        if (!c) break;
        out += c;
    }
    return out;
}

}  // namespace psvrecore::runtime
