// SceLibKernel's heaps in memory the caller gives (sceClibMspace*: dlmalloc's mspaces). The heap's bookkeeping is
// kept here, not in the region: the handle is the region's first byte, and blocks come from the rest of it (first
// fit, 8-byte aligned, freed blocks merged with their neighbours).

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

void sceClibMspaceFree(u32 handle, u32 address) {
    if (!address) return;
    Mspaces& all = mspaces();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_handle.find(handle);
    if (it == all.by_handle.end()) return;
    Mspace& m = it->second;
    auto used = m.used.find(address);
    if (used == m.used.end()) {
        nx::logw(logs::kernel, "sceClibMspaceFree(0x{:08x}, 0x{:08x}): not a block of that heap", handle, address);
        return;
    }
    u32 at = address, size = used->second;
    m.used.erase(used);
    // (merged with the free blocks just after and just before it)
    if (auto next = m.free.find(at + size); next != m.free.end()) size += next->second, m.free.erase(next);
    if (auto prev = m.free.lower_bound(at); prev != m.free.begin()) {
        --prev;
        if (prev->first + prev->second == at) at = prev->first, size += prev->second, m.free.erase(prev);
    }
    m.free[at] = size;
}

const Function kFunctions[] = {
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
