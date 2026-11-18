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
// True if [address, address + size) is guest memory the guest may read.
inline bool readable(u32 address, u32 size) { return address && memory().valid(address, size, false); }

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
void register_apputil();
void register_ngs();
void register_avplayer();
void register_system();
void register_systemgesture();
void register_motion();
void register_mspace();
void register_jpegenc();
void register_fiber();
void register_np();
void register_online();
void register_net();
// SceNet's ad hoc mode over the host's local network (net.cpp; Runtime::Config::network): whether it is on, the
// connection made (the network check dialog's ad hoc mode: 0, or why not, in `why`) and ended, and its address (in
// network order) while it is up. SceNetCtl (system.cpp) is told of each change to give its callbacks.
bool adhoc_enabled();
bool adhoc_connect(nx::string* description);
void adhoc_disconnect();
bool adhoc_address(u32* network_order);
void netctl_adhoc_event(i32 event_type);
void register_psn();
void register_services();
void register_audiodec();
void register_videodec();
void register_codecengine();
void register_ime();
void register_voice();
void register_sas();
void register_json();
void register_pvf();
void register_mp4lib();
void register_kernel_more();
void register_commondialog();
void register_debug();

// printf-style formatting of a variadic call's guest arguments (libkernel.cpp).
nx::string format_variadic(nx::string_view fmt, Variadic va);

}  // namespace psvrecore::runtime::hle
