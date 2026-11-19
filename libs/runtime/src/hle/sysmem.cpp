// Memory blocks: SceSysmem.

#include <atomic>
#include <cstring>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceSysmem = 0x37FE725A;

constexpr u32 kUserRw = 0x0C20D060, kUserNcRw = 0x0C208060, kUserMainPhycontRw = 0x0C80D060,
              kUserMainPhycontNcRw = 0x0D808060, kUserCdramRw = 0x09408060;
constexpr u32 kAttrHasAlignment = 0x4;
constexpr u32 kKiB = 1024, kMiB = 1024 * 1024;

struct AllocOpt {
    u32 size, attr, alignment, reserved, base_block_name;
};

struct MemBlockInfo {
    u32 size, mapped_base, mapped_size;
    i32 memory_type;
    u32 access, type;
};
static_assert(sizeof(MemBlockInfo) == 24);

struct FreeMemorySizeInfo {
    u32 size, main, cdram, phycont;
};

std::atomic<u32> g_next_block{0};

// The granularity a type's size must be a multiple of, its default and largest alignment, and whether it is CDRAM.
struct TypeRules {
    u32 granule, min_alignment;
    bool alignment_allowed, cdram;
};

bool rules_for(u32 type, TypeRules* out) {
    switch (type) {
        case kUserRw:
        case kUserNcRw: *out = {4 * kKiB, 4 * kKiB, true, false}; return true;
        case kUserMainPhycontRw:
        case kUserMainPhycontNcRw: *out = {1 * kMiB, 1 * kMiB, false, false}; return true;
        case kUserCdramRw: *out = {256 * kKiB, 256 * kKiB, true, true}; return true;
        default: return false;
    }
}

constexpr i32 kNoFreeCdram = err::code(0x80024309);  // SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE_CDRAM

// Physically contiguous main memory is an area of its own on the console, 26 MiB for the game, beside the budget for
// main memory's other blocks: Uncharted: Golden Abyss sizes its heap from the free main memory, then takes 26 MiB
// of the physically contiguous kind. Phycont blocks past that area are charged to the main budget (as all of them
// were before), so nothing that fitted then is refused now.
constexpr u64 kPhycontArea = 26ull * kMiB;
constexpr u32 kPhycontTypes[] = {kUserMainPhycontRw, kUserMainPhycontNcRw};
bool phycont(u32 type) { return type == kUserMainPhycontRw || type == kUserMainPhycontNcRw; }

// Main memory in use: the blocks charged to its budget (the others, and phycont blocks past the area), and the
// phycont blocks.
struct MainUse {
    u64 charged = 0, phycont = 0;
};
MainUse main_use(u32 adding_type = 0, u32 adding = 0) {
    const u64 all = memory().mapped_bytes(BlockKind::MemBlock, layout::kUserMemory) + adding;
    const u64 phy = memory().mapped_bytes(BlockKind::MemBlock, layout::kUserMemory, kPhycontTypes) +
                    (phycont(adding_type) ? adding : 0);
    return {all - phy + (phy > kPhycontArea ? phy - kPhycontArea : 0), phy};
}

i32 sceKernelAllocMemBlock(Ptr<const char> name, u32 type, u32 size, Ptr<AllocOpt> opt) {
    if (!name) return err::kInvalidArgument;
    TypeRules rules{};
    if (!rules_for(type, &rules)) return err::kIllegalMemBlockType;
    if (size == 0 || size % rules.granule) return err::kIllegalMemBlockSize;
    u32 alignment = rules.min_alignment;
    if (opt && (opt->attr & kAttrHasAlignment)) {
        if (!rules.alignment_allowed) return err::kInvalidArgument;
        // (a power of two up to 16 MiB; one below the type's own is the type's: the SDK's samples ask CDRAM for 256)
        if (!opt->alignment || opt->alignment > 16 * kMiB || (opt->alignment & (opt->alignment - 1)))
            return err::kIllegalAlignment;
        alignment = std::max(opt->alignment, rules.min_alignment);
    }
    Runtime& rt = runtime();
    // Main memory and CDRAM each have a budget, as on the console (the game's share of each).
    const Region region = rules.cdram ? layout::kCdram : layout::kUserMemory;
    const u64 budget = rules.cdram ? rt.config().cdram_budget : rt.config().user_memory_budget;
    const i32 no_memory = rules.cdram ? kNoFreeCdram : err::kNoFreePhysicalPage;
    if (rules.cdram ? rt.memory().mapped_bytes(BlockKind::MemBlock, region) + size > budget
                    : main_use(type, size).charged > budget)
        return no_memory;
    const u32 uid = 0x40050001u + 2 * g_next_block.fetch_add(1);
    const nx::string block_name = str(name, 31);
    const u32 base = rt.memory().map_anywhere(region, size, alignment, Access::ReadWrite, BlockKind::MemBlock, block_name,
                                              static_cast<i32>(uid), type);
    if (!base) return no_memory;
    nx::logd(logs::kernel, "memory block '{}' 0x{:08x}+0x{:x} (type 0x{:08x}) UID 0x{:08x}", block_name, base, size, type, uid);
    return static_cast<i32>(uid);
}

i32 sceKernelFreeMemBlock(i32 uid) {
    Block b;
    if (!memory().find_uid(uid, &b)) return err::kUnknownUid;
    memory().unmap(b.base);
    return err::kOk;
}

i32 sceKernelGetMemBlockBase(i32 uid, Ptr<u32> base) {
    if (!writable(base.address, 4)) return err::kIllegalAddress;
    Block b;
    if (!memory().find_uid(uid, &b)) return err::kUnknownUid;
    *base = b.base;
    return err::kOk;
}

i32 sceKernelFindMemBlockByAddr(u32 address, u32 size) {
    Block b;
    if (!memory().find(address, &b) || b.kind != BlockKind::MemBlock) return err::kUnknownUid;
    if (size && u64{address} + size > u64{b.base} + b.size) return err::kUnknownUid;
    return b.uid;
}

i32 block_info(const Block& b, Ptr<MemBlockInfo> out) {
    MemBlockInfo info{};
    info.size = out->size;
    info.mapped_base = b.base;
    info.mapped_size = b.size;
    info.memory_type = (b.type == kUserNcRw || b.type == kUserMainPhycontNcRw || b.type == kUserCdramRw) ? 0x80 : 0xD0;
    info.access = 0x6;  // (read | write)
    info.type = b.type;
    std::memcpy(out.get(), &info, std::min<u32>(info.size ? info.size : sizeof info, sizeof info));
    return err::kOk;
}

i32 sceKernelGetMemBlockInfoByAddr(u32 address, Ptr<MemBlockInfo> out) {
    if (!writable(out.address, 4)) return err::kIllegalAddress;
    Block b;
    if (!memory().find(address, &b) || b.kind != BlockKind::MemBlock) return err::kUnknownUid;
    return block_info(b, out);
}

// The same for a range, which must lie within one block.
i32 sceKernelGetMemBlockInfoByRange(u32 address, u32 size, Ptr<MemBlockInfo> out) {
    if (!writable(out.address, 4)) return err::kIllegalAddress;
    Block b;
    if (!memory().find(address, &b) || b.kind != BlockKind::MemBlock) return err::kUnknownUid;
    if (u64{address} + size > u64{b.base} + b.size) return err::kUnknownUid;
    return block_info(b, out);
}

i32 sceKernelGetFreeMemorySize(Ptr<FreeMemorySizeInfo> out) {
    if (!writable(out.address, 16)) return err::kIllegalAddress;
    const Runtime& rt = runtime();
    const MainUse used = main_use();
    const u64 cdram_used = memory().mapped_bytes(BlockKind::MemBlock, layout::kCdram);
    FreeMemorySizeInfo info{};
    info.size = out->size;
    info.main = static_cast<u32>(used.charged >= rt.config().user_memory_budget ? 0
                                                                                 : rt.config().user_memory_budget - used.charged);
    info.cdram = static_cast<u32>(cdram_used >= rt.config().cdram_budget ? 0 : rt.config().cdram_budget - cdram_used);
    info.phycont = static_cast<u32>(used.phycont >= kPhycontArea ? 0 : kPhycontArea - used.phycont);
    std::memcpy(out.get(), &info, std::min<u32>(info.size ? info.size : sizeof info, sizeof info));
    return err::kOk;
}

i32 sceKernelIsPSVitaTV() { return 0; }

const Function kSysmem[] = {
    HLE_FUNCTION(0xB9D5EBDE, sceKernelAllocMemBlock),
    HLE_FUNCTION(0xA91E15EE, sceKernelFreeMemBlock),
    HLE_FUNCTION(0xB8EF5818, sceKernelGetMemBlockBase),
    HLE_FUNCTION(0xA33B99D1, sceKernelFindMemBlockByAddr),
    HLE_FUNCTION(0x4010AD65, sceKernelGetMemBlockInfoByAddr),
    HLE_FUNCTION(0x006F3DB4, sceKernelGetMemBlockInfoByRange),
    HLE_FUNCTION(0x87CC580B, sceKernelGetFreeMemorySize),
    HLE_FUNCTION(0x1453A5E5, sceKernelIsPSVitaTV),
};

}  // namespace

void register_sysmem() { register_library({"SceSysmem", kSceSysmem, kSysmem}); }

}  // namespace psvrecore::runtime::hle
