#pragma once

// Guest memory: the whole 32-bit guest address space reserved on the host (plus a guard region past its end), with
// host pages committed only where something is mapped. A guest address converts to a host pointer as
// base + address (psv_membase, which generated code uses), so a stray guest access outside mapped memory faults on
// the host; the fault is reported with its guest address and the guest thread that made it.
//
// Mapped memory is handed out in blocks: modules at the addresses their code was generated for, memory blocks the
// game allocates, thread stacks and thread-local storage. Each block has a name, an owner kind, and a range, and the
// address space keeps them sorted so a guest address finds its block.

#include <span>

#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime {

enum class Access : u32 {
    None = 0,
    Read = 1,
    ReadWrite = 3,
};

// What a block is for (shown in diagnostics; memory blocks are also found by the guest).
enum class BlockKind : u32 {
    Module,       // a module's segment
    MemBlock,     // sceKernelAllocMemBlock
    Stack,        // a thread's stack
    ThreadLocal,  // a thread's TLS area
    System,       // the runtime's own (exported variables, argument blocks)
};

struct Block {
    u32 base = 0;
    u32 size = 0;
    BlockKind kind = BlockKind::System;
    Access access = Access::ReadWrite;
    nx::string name;
    i32 uid = 0;    // the memory block's UID (MemBlock)
    u32 type = 0;   // the memory block type it was allocated with (MemBlock)
};

// A range of guest addresses blocks of one kind are taken from.
struct Region {
    u32 begin = 0;
    u32 end = 0;  // exclusive
};

class GuestMemory {
public:
    static constexpr u32 kPageSize = 4096;  // the guest's page size (the host's may be larger: 16 KiB on some arm64)

    GuestMemory() = default;
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    ~GuestMemory();

    // Reserves the address space and makes it the one generated code uses (psv_membase). One at a time.
    bool init();
    void shutdown();
    bool initialized() const { return base_ != nullptr; }

    u8* base() const { return base_; }
    u8* host(u32 address) const { return base_ + address; }
    template <class T>
    T* host_as(u32 address) const {
        return reinterpret_cast<T*>(base_ + address);
    }

    // A block at a fixed address (a module's segment). False if anything already overlaps it.
    bool map_fixed(u32 base, u32 size, Access access, BlockKind kind, nx::string_view name);
    // A block anywhere in `region`, aligned to `alignment` (a power of two, at least a page); 0 if nothing is free.
    u32 map_anywhere(const Region& region, u32 size, u32 alignment, Access access, BlockKind kind, nx::string_view name,
                     i32 uid = 0, u32 type = 0);
    // Unmaps the block starting at `base`; its pages go back to the host and read as zero when mapped again.
    bool unmap(u32 base);
    bool protect(u32 base, Access access);

    // The block containing `address` (copied: blocks can be unmapped by another thread at any time).
    bool find(u32 address, Block* out) const;
    bool find_uid(i32 uid, Block* out) const;
    // Bytes mapped in blocks of a kind (that lie in `region`, if given).
    u64 mapped_bytes(BlockKind kind) const;
    u64 mapped_bytes(BlockKind kind, const Region& region) const;

    // True if [address, address + size) lies in mapped blocks (with write access if `write`).
    bool valid(u32 address, u32 size, bool write = false) const;

    // A guest NUL-terminated string at `address` (at most `max` bytes; empty if it is not in mapped memory).
    nx::string string(u32 address, u32 max = 4096) const;

private:
    bool commit(u32 base, u32 size, Access access);
    bool overlaps(u32 base, u32 size) const;

    u8* base_ = nullptr;
    usize reserved_ = 0;
    mutable nx::mutex lock_;
    nx::map<u32, Block> blocks_;  // by base
};

// The guest address of a host pointer into guest memory.
inline u32 guest_address(const GuestMemory& memory, const void* host) {
    return static_cast<u32>(static_cast<const u8*>(host) - memory.base());
}

}  // namespace psvrecore::runtime
