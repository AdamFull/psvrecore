// The guest memory design rests on one host property: a 4 GiB reservation (plus a guard region) is available, so a
// guest address converts to a host pointer as base + (u32)address with no bounds check, and pages outside mapped
// guest regions stay inaccessible. These tests check that property on each host the toolkit is built for.

#include <cstring>

#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/os.h"
#include "nxtest.h"

namespace {

constexpr u64 kGuestSpace = 1ull << 32;
constexpr u64 kGuard = 64 * 1024;

struct Reservation {
    void* base = nullptr;
    usize size = 0;
    ~Reservation() {
        if (base) nx::vmem::release(base, size);
    }
};

}  // namespace

TEST_CASE("address space: the full 32-bit guest space can be reserved with a guard region") {
    Reservation r;
    r.size = static_cast<usize>(kGuestSpace + kGuard);
    REQUIRE(nx::vmem::reserve(&r.base, r.size));
    CHECK(reinterpret_cast<uintptr_t>(r.base) % nx::vmem::get_allocation_granularity() == 0);
}

TEST_CASE("address space: committed guest pages are reachable through base + address") {
    Reservation r;
    r.size = static_cast<usize>(kGuestSpace + kGuard);
    REQUIRE(nx::vmem::reserve(&r.base, r.size));
    u8* const membase = static_cast<u8*>(r.base);

    // Where a main executable is loaded, and the last page below 4 GiB: the far ends of what guest code addresses.
    const u32 page = static_cast<u32>(nx::vmem::get_page_size());
    const u32 addresses[] = {0x81000000u, static_cast<u32>(kGuestSpace - page)};
    for (const u32 guest : addresses) {
        u8* const host = membase + guest;
        REQUIRE(nx::vmem::commit(host, page));
        const u32 pattern = 0xC0FFEE00u ^ guest;
        std::memcpy(host + 16, &pattern, sizeof pattern);
        u32 back = 0;
        std::memcpy(&back, membase + static_cast<u32>(guest + 16), sizeof back);
        CHECK(back == pattern);
    }
}
