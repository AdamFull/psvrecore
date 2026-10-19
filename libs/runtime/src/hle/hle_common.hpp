#pragma once

// What every HLE source uses: the runtime, guest strings and the guest views of kernel structures.

#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/errors.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/runtime.hpp"

namespace psvrecore::runtime::hle {

inline Kernel& kernel() { return runtime().kernel(); }
inline GuestMemory& memory() { return runtime().memory(); }

// A guest string argument (empty for a null or unmapped pointer).
inline nx::string str(Ptr<const char> p, u32 max = 4096) { return memory().string(p.address, max); }

// A guest timeout argument: the guest's remaining-time variable, or none.
inline kernel::Timeout timeout(Ptr<u32> p) { return kernel::Timeout{p.get()}; }

// Copies a name into a guest char[32] (SCE_UID_NAMELEN + 1).
inline void put_name(char* out, nx::string_view name) {
    std::memset(out, 0, 32);
    std::memcpy(out, name.data(), std::min<usize>(name.size(), 31));
}

// True if [address, address + size) is guest memory the guest may write.
inline bool writable(u32 address, u32 size) { return address && memory().valid(address, size, true); }

void register_threadmgr();
void register_sysmem();
void register_iofilemgr();
void register_processmgr();
void register_libkernel();
void register_modulemgr();
void register_rtc();
void register_fios2();
void register_ctrl();
void register_audioout();
void register_touch();

}  // namespace psvrecore::runtime::hle
