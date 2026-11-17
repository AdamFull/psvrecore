// The codec engine's memory (SceCodecEngineUser): a block of the program's own memory handed over (on the Vita, to be
// unmapped from the CPU for the decoders' use), then allocated from: here the block stays the program's memory and
// allocations are first fit, aligned as asked (a power of two; 0 for the smallest, 8). A block with allocations left
// in it does not close. Running out of room in a block gives INVALID_SIZE.
//
// Its performance monitor (SceCodecEnginePerf): started, stopped and reset; the engine's load is always 0 (nothing
// runs on it).

#include "hle_common.hpp"

#include <map>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceCodecEngineUser = 0x469AB062, kSceCodecEnginePerf = 0x9BF4FFAD;
constexpr i32 kInvalidPointer = err::code(0x80600000), kInvalidSize = err::code(0x80600001),
              kInvalidAlignment = err::code(0x80600002), kInvalidHeap = err::code(0x80600005),
              kInvalidValue = err::code(0x80600009), kNotAllocated = err::code(0x8060000A),
              kInUse = err::code(0x8060000B);

struct Block {
    u32 base = 0, size = 0;
    std::map<u32, u32> used;  // address: size
};
struct Blocks {
    nx::mutex lock;
    std::map<i32, Block> by_uid;
    i32 next = 0x40C0001;
};
Blocks& blocks() {
    static Blocks b;
    return b;
}

i32 sceCodecEngineOpenUnmapMemBlock(u32 memory_block, u32 size) {
    if (!memory_block) return kInvalidPointer;
    if (!size || !writable(memory_block, size)) return kInvalidSize;
    Blocks& all = blocks();
    nx::scoped_lock<nx::mutex> held(all.lock);
    const i32 uid = all.next;
    all.next += 2;
    all.by_uid[uid] = Block{memory_block, size, {}};
    nx::logd(logs::hle, "codec engine block 0x{:x}: 0x{:08x}, 0x{:x} bytes", uid, memory_block, size);
    return uid;
}

i32 sceCodecEngineCloseUnmapMemBlock(i32 uid) {
    Blocks& all = blocks();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_uid.find(uid);
    if (it == all.by_uid.end()) return kInvalidHeap;
    if (!it->second.used.empty()) return kInUse;
    all.by_uid.erase(it);
    return 0;
}

u32 sceCodecEngineAllocMemoryFromUnmapMemBlock(i32 uid, u32 size, u32 alignment) {
    if (alignment == 0) alignment = 8;
    if (alignment & (alignment - 1)) return static_cast<u32>(kInvalidAlignment);
    if (!size) return static_cast<u32>(kInvalidSize);
    Blocks& all = blocks();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_uid.find(uid);
    if (it == all.by_uid.end()) return static_cast<u32>(kInvalidHeap);
    Block& b = it->second;
    const u64 end = u64{b.base} + b.size;
    u64 at = b.base;
    for (auto used = b.used.begin();; ++used) {
        at = (at + alignment - 1) & ~u64{alignment - 1};
        const u64 limit = used == b.used.end() ? end : used->first;
        if (at + size <= limit) {
            b.used[static_cast<u32>(at)] = size;
            return static_cast<u32>(at);
        }
        if (used == b.used.end()) return static_cast<u32>(kInvalidSize);
        at = u64{used->first} + used->second;
    }
}

i32 sceCodecEngineFreeMemoryFromUnmapMemBlock(i32 uid, u32 address) {
    Blocks& all = blocks();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_uid.find(uid);
    if (it == all.by_uid.end()) return kInvalidHeap;
    if (!it->second.used.erase(address)) return kNotAllocated;
    return 0;
}

i32 sceCodecEnginePmonStart() { return 0; }
i32 sceCodecEnginePmonStop() { return 0; }
i32 sceCodecEnginePmonReset() { return 0; }
// SceCodecEnginePmonProcessorLoad: size (8), average load (percent).
i32 sceCodecEnginePmonGetProcessorLoad(Ptr<u32> load) {
    if (!load || !writable(load.address, 8)) return kInvalidPointer;
    if (load.get()[0] != 8) return kInvalidValue;
    load.get()[1] = 0;
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0xF0B4C892, sceCodecEngineOpenUnmapMemBlock),
    HLE_FUNCTION(0x95EA3B3E, sceCodecEngineCloseUnmapMemBlock),
    HLE_FUNCTION(0x1A3A53E5, sceCodecEngineAllocMemoryFromUnmapMemBlock),
    HLE_FUNCTION(0x6A31831D, sceCodecEngineFreeMemoryFromUnmapMemBlock),
};
const Function kPerf[] = {
    HLE_FUNCTION(0x0635EB77, sceCodecEnginePmonStart),
    HLE_FUNCTION(0xD2EFAEA1, sceCodecEnginePmonStop),
    HLE_FUNCTION(0x5D75DF62, sceCodecEnginePmonReset),
    HLE_FUNCTION(0x2C9DA711, sceCodecEnginePmonGetProcessorLoad),
};

}  // namespace

void register_codecengine() {
    {
        Blocks& all = blocks();
        nx::scoped_lock<nx::mutex> held(all.lock);
        all.by_uid.clear();
    }
    register_library({"SceCodecEngineUser", kSceCodecEngineUser, kFunctions});
    register_library({"SceCodecEnginePerf", kSceCodecEnginePerf, kPerf});
}

}  // namespace psvrecore::runtime::hle
