// SceLibKernel's heaps in memory the caller gives (sceClibMspace*: dlmalloc's mspaces). The heap's bookkeeping is
// kept here, not in the region: the handle is the region's first byte, and blocks come from the rest of it (first
// fit, 8-byte aligned, freed blocks merged with their neighbours).

#include <cstring>

#include "hle_common.hpp"

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6;
constexpr u32 kHeader = 64;  // (the start of the region, kept as the console's heap keeps its state there)
constexpr u32 kAlign = 8;

struct Mspace {
    u32 base = 0, end = 0;
    nx::map<u32, u32> free;  // address -> size, in address order
    nx::flat_hash_map<u32, u32> used;  // address -> size
    u32 in_use = 0, peak = 0;          // (bytes in blocks)
};

struct Mspaces {
    nx::mutex lock;
    nx::flat_hash_map<u32, Mspace> by_handle;
};
Mspaces& mspaces() {
    static Mspaces m;
    return m;
}

u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

u32 sceClibMspaceCreate(u32 base, u32 capacity) {
    if (!base || capacity < kHeader + 64 || !memory().valid(base, capacity, true)) return 0;
    Mspace m;
    m.base = base;
    m.end = base + capacity;
    const u32 first = align_up(base + kHeader, kAlign);
    m.free[first] = m.end - first;
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    all.by_handle[base] = std::move(m);
    return base;
}

u32 sceClibMspaceDestroy(u32 handle) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_handle.find(handle);
    if (it == all.by_handle.end()) return 0;
    const u32 bytes = it->second.end - it->second.base;
    all.by_handle.erase(it);
    return bytes;
}

u32 allocate(Mspace& m, u32 alignment, u32 size) {
    size = align_up(nx::max<u32>(size, 1), kAlign);
    alignment = nx::max<u32>(alignment, kAlign);
    if (alignment & (alignment - 1)) return 0;
    for (auto it = m.free.begin(); it != m.free.end(); ++it) {
        const u32 at = it->first, span = it->second;
        const u32 start = align_up(at, alignment);
        if (start < at || start - at + u64{size} > span) continue;
        m.free.erase(it);
        if (start > at) m.free[at] = start - at;  // (the gap the alignment leaves)
        const u32 rest = static_cast<u32>(at + u64{span} - start - size);
        if (rest) m.free[start + size] = rest;
        m.used[start] = size;
        m.in_use += size;
        m.peak = nx::max(m.peak, m.in_use);
        return start;
    }
    return 0;
}

u32 sceClibMspaceMalloc(u32 handle, u32 size) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? 0 : allocate(it->second, kAlign, size);
}

u32 sceClibMspaceMemalign(u32 handle, u32 alignment, u32 size) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_handle.find(handle);
    return it == all.by_handle.end() ? 0 : allocate(it->second, alignment, size);
}

void release(Mspace& m, u32 handle, u32 address);
void sceClibMspaceFree(u32 handle, u32 address) {
    if (!address) return;
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_handle.find(handle);
    if (it == all.by_handle.end()) return;
    release(it->second, handle, address);
}
void release(Mspace& m, u32 handle, u32 address) {
    auto used = m.used.find(address);
    if (used == m.used.end()) {
        nx::logw(logs::kernel, "sceClibMspaceFree(0x{:08x}, 0x{:08x}): not a block of that heap", handle, address);
        return;
    }
    u32 at = address, size = used->second;
    m.used.erase(used);
    m.in_use -= size;
    // (merged with the free blocks just after and just before it)
    if (auto next = m.free.find(at + size); next != m.free.end()) size += next->second, m.free.erase(next);
    if (auto prev = m.free.lower_bound(at); prev != m.free.begin()) {
        --prev;
        if (prev->first + prev->second == at) at = prev->first, size += prev->second, m.free.erase(prev);
    }
    m.free[at] = size;
}

Mspace* heap(u32 handle) {
    auto it = mspaces().by_handle.find(handle);
    return it == mspaces().by_handle.end() ? nullptr : &it->second;
}
u32 sceClibMspaceCalloc(u32 handle, u32 count, u32 size) {
    const u64 bytes = u64{count} * size;
    if (bytes > 0xFFFFFFFFull) return 0;
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Mspace* m = heap(handle);
    const u32 at = m ? allocate(*m, kAlign, static_cast<u32>(bytes)) : 0;
    if (at) std::memset(memory().host(at), 0, static_cast<usize>(bytes));
    return at;
}
// A block of the new size (aligned as asked), its contents kept as far as they fit; null and size give a new block,
// a size of 0 frees it. When no block of that size is free, the old one stays as it was (0 returned).
u32 reallocate(u32 handle, u32 address, u32 size, u32 alignment) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Mspace* m = heap(handle);
    if (!m) return 0;
    if (!address) return allocate(*m, alignment, size);
    auto used = m->used.find(address);
    if (used == m->used.end()) return 0;
    if (!size) {
        release(*m, handle, address);
        return 0;
    }
    const u32 old = used->second;
    if (align_up(size, kAlign) <= old && address % nx::max(alignment, kAlign) == 0) return address;
    const u32 at = allocate(*m, alignment, size);
    if (!at) return 0;
    std::memmove(memory().host(at), memory().host(address), nx::min(old, size));
    release(*m, handle, address);
    return at;
}
u32 sceClibMspaceRealloc(u32 handle, u32 address, u32 size) { return reallocate(handle, address, size, kAlign); }
u32 sceClibMspaceReallocalign(u32 handle, u32 address, u32 size, u32 alignment) {
    return reallocate(handle, address, size, alignment);
}
u32 sceClibMspaceIsHeapEmpty(u32 handle) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Mspace* m = heap(handle);
    return m && m->used.empty() ? 1 : 0;
}
// SceClibMspaceStats: the capacity, the bytes the heap has from the system (all of it), the most in use, in use now.
void sceClibMspaceMallocStats(u32 handle, Ptr<u32> stats) {
    if (!stats || !writable(stats.address, 16)) return;
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Mspace* m = heap(handle);
    if (!m) return;
    const u32 v[4] = {m->end - m->base, m->end - m->base, m->peak, m->in_use};
    std::memcpy(stats.get(), v, sizeof v);
}
void sceClibMspaceMallocStatsFast(u32 handle, Ptr<u32> stats) { sceClibMspaceMallocStats(handle, stats); }
// A block's size (as allocated: rounded up to 8), from whichever heap holds it; 0 for one no heap does.
u32 sceClibMspaceMallocUsableSize(u32 address) {
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    for (auto& [handle, m] : all.by_handle)
        if (auto it = m.used.find(address); it != m.used.end()) return it->second;
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x678374AD, sceClibMspaceCalloc),
    HLE_FUNCTION(0x774891D6, sceClibMspaceRealloc),
    HLE_FUNCTION(0x586AC68D, sceClibMspaceReallocalign),
    HLE_FUNCTION(0xD1D59701, sceClibMspaceIsHeapEmpty),
    HLE_FUNCTION(0x8CC1D38E, sceClibMspaceMallocStats),
    HLE_FUNCTION(0x738E0322, sceClibMspaceMallocStatsFast),
    HLE_FUNCTION(0x46A02279, sceClibMspaceMallocUsableSize),
    HLE_FUNCTION(0x3B9E301A, sceClibMspaceCreate),
    HLE_FUNCTION(0xAE1A21EC, sceClibMspaceDestroy),
    HLE_FUNCTION(0x86EF7680, sceClibMspaceMalloc),
    HLE_FUNCTION(0x3C847D57, sceClibMspaceMemalign),
    HLE_FUNCTION(0x9C56B4D1, sceClibMspaceFree),
};

}  // namespace

void register_mspace() {
    {
        Mspaces& all = mspaces();
        nx::scoped_lock<nx::mutex> held(all.lock);
        all.by_handle.clear();
    }
    register_library({"SceLibKernel", kSceLibKernel, kFunctions});
}

}  // namespace psvrecore::runtime::hle
