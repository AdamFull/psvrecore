// The process: SceProcessmgr, and sceKernelExitProcess (SceLibKernel).
//
// libc's time functions call the sceKernelLibc* ones: their layouts are those libc's code reads (a 32-bit time in
// seconds since 1970, timeval {sec, usec}, timezone {minutes west, dst}, and tm {sec, min, hour, mday, mon, year,
// wday, yday, isdst} with the full year, from which libc takes 1900).

#include <chrono>
#include <ctime>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceProcessmgr = 0x2DD91812;

struct Timeval {
    i32 sec, usec;
};
struct Timezone {
    i32 minutes_west, dst;
};
struct Tm {
    i32 sec, min, hour, mday, mon, year, wday, yday, isdst;
};

u64 unix_microseconds() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

void put_tm(Tm* out, const std::tm& tm) {
    out->sec = tm.tm_sec;
    out->min = tm.tm_min;
    out->hour = tm.tm_hour;
    out->mday = tm.tm_mday;
    out->mon = tm.tm_mon;
    out->year = tm.tm_year + 1900;
    out->wday = tm.tm_wday;
    out->yday = tm.tm_yday;
    out->isdst = tm.tm_isdst > 0 ? 1 : 0;
}

i32 sceKernelGetStdin() { return runtime().files().stdin_fd(); }
i32 sceKernelGetStdout() { return runtime().files().stdout_fd(); }
i32 sceKernelGetStderr() { return runtime().files().stderr_fd(); }

i32 sceKernelExitProcess(i32 code) {
    nx::logi(logs::kernel, "process exit ({})", code);
    runtime().exit_process(code);
    kernel().exit_thread(code, false);
}

i32 sceKernelCallAbortHandler() {
    nx::loge(logs::kernel, "the guest called abort()");
    runtime().exit_process(134);  // (the code a host process killed by SIGABRT reports)
    kernel().exit_thread(134, false);
}

u32 sceKernelGetProcessParam() { return runtime().process_param; }

i32 sceKernelPowerTick(i32 /*type*/) { return err::kOk; }

u32 sceKernelLibcTime(Ptr<u32> out) {
    const u32 now = static_cast<u32>(unix_microseconds() / 1'000'000);
    if (out && writable(out.address, 4)) *out = now;
    return now;
}

i32 sceKernelLibcGettimeofday(Ptr<Timeval> tv, Ptr<Timezone> tz) {
    const u64 now = unix_microseconds();
    if (tv) {
        if (!writable(tv.address, sizeof(Timeval))) return err::kFault;
        tv->sec = static_cast<i32>(now / 1'000'000);
        tv->usec = static_cast<i32>(now % 1'000'000);
    }
    if (tz) {
        if (!writable(tz.address, sizeof(Timezone))) return err::kFault;
        const std::time_t t = static_cast<std::time_t>(now / 1'000'000);
        std::tm local{}, utc{};
#ifdef _WIN32
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
#else
        localtime_r(&t, &local);
        gmtime_r(&t, &utc);
#endif
        const i64 west = (static_cast<i64>(std::mktime(&utc)) - static_cast<i64>(std::mktime(&local))) / 60;
        tz->minutes_west = static_cast<i32>(west);
        tz->dst = local.tm_isdst > 0 ? 1 : 0;
    }
    return err::kOk;
}

u32 sceKernelLibcClock() { return static_cast<u32>(kernel().process_time()); }  // (CLOCKS_PER_SEC is 1000000)

u32 time_tm(Ptr<const u32> t, Ptr<Tm> out, bool local) {
    if (!t || !memory().valid(t.address, 4) || !writable(out.address, sizeof(Tm))) return 0;
    const std::time_t seconds = static_cast<std::time_t>(*t);
    std::tm tm{};
#ifdef _WIN32
    if (local) localtime_s(&tm, &seconds);
    else gmtime_s(&tm, &seconds);
#else
    if (local) localtime_r(&seconds, &tm);
    else gmtime_r(&seconds, &tm);
#endif
    put_tm(out.get(), tm);
    return out.address;
}
u32 sceKernelLibcGmtime_r(Ptr<const u32> t, Ptr<Tm> out) { return time_tm(t, out, false); }
u32 sceKernelLibcLocaltime_r(Ptr<const u32> t, Ptr<Tm> out) { return time_tm(t, out, true); }

const Function kLibKernel[] = {
    HLE_FUNCTION(0x7595D9AA, sceKernelExitProcess),
};

const Function kProcessmgr[] = {
    HLE_FUNCTION(0xC1727F59, sceKernelGetStdin),
    HLE_FUNCTION(0xE5AA625C, sceKernelGetStdout),
    HLE_FUNCTION(0xFA5E3ADA, sceKernelGetStderr),
    HLE_FUNCTION(0x2BE3E066, sceKernelGetProcessParam),
    HLE_FUNCTION(0x0039BE45, sceKernelLibcTime),
    HLE_FUNCTION(0x4B879059, sceKernelLibcGettimeofday),
    HLE_FUNCTION(0x9E45DA09, sceKernelLibcClock),
    HLE_FUNCTION(0xBCA437CD, sceKernelLibcGmtime_r),
    HLE_FUNCTION(0x94F041ED, sceKernelLibcLocaltime_r),
    HLE_FUNCTION(0x2252890C, sceKernelPowerTick),
    HLE_FUNCTION(0xEB6E50BB, sceKernelCallAbortHandler),
};

}  // namespace

void register_processmgr() {
    register_library({"SceLibKernel", kSceLibKernel, kLibKernel});
    register_library({"SceProcessmgr", kSceProcessmgr, kProcessmgr});
}

}  // namespace psvrecore::runtime::hle
