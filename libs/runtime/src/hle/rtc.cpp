// The real-time clock (SceRtcUser): ticks are microseconds since 0001-01-01 00:00:00 UTC. Also the debug library's
// assertion handler (SceDbg).

#include <chrono>
#include <cstring>
#include <ctime>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceRtcUser = 0x6704D3C6, kSceDbg = 0x623DF986;

// Days from 0001-01-01 to 1970-01-01 (proleptic Gregorian).
constexpr i64 kUnixEpochDays = 719162;
constexpr u64 kMicrosPerDay = 86'400'000'000ull;

// Days since 1970-01-01 of a civil date, and back (H. Hinnant's algorithms).
i64 days_from_civil(i64 y, u32 m, u32 d) {
    y -= m <= 2;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const u32 yoe = static_cast<u32>(y - era * 400);
    const u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

void civil_from_days(i64 z, i64* y, u32* m, u32* d) {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u32 doe = static_cast<u32>(z - era * 146097);
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<i64>(yoe) + era * 400 + (*m <= 2);
}

u64 tick_of(const io::DateTime& t) {
    const i64 days = days_from_civil(t.year, t.month, t.day) + kUnixEpochDays;
    return static_cast<u64>(days) * kMicrosPerDay + (u64{t.hour} * 3600 + u64{t.minute} * 60 + t.second) * 1'000'000 +
           t.microsecond;
}

io::DateTime date_of(u64 tick) {
    io::DateTime t{};
    const i64 days = static_cast<i64>(tick / kMicrosPerDay) - kUnixEpochDays;
    u64 rest = tick % kMicrosPerDay;
    i64 y;
    u32 m, d;
    civil_from_days(days, &y, &m, &d);
    t.year = static_cast<u16>(y);
    t.month = static_cast<u16>(m);
    t.day = static_cast<u16>(d);
    t.microsecond = static_cast<u32>(rest % 1'000'000);
    rest /= 1'000'000;
    t.second = static_cast<u16>(rest % 60);
    t.minute = static_cast<u16>((rest / 60) % 60);
    t.hour = static_cast<u16>(rest / 3600);
    return t;
}

u64 now_tick() {
    using namespace std::chrono;
    const u64 unix_us = static_cast<u64>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
    return static_cast<u64>(kUnixEpochDays) * kMicrosPerDay + unix_us;
}

// The host's offset from UTC, in minutes (for local time).
i64 local_offset_minutes() {
    const std::time_t t = std::time(nullptr);
    std::tm local{}, utc{};
#ifdef _WIN32
    localtime_s(&local, &t);
    gmtime_s(&utc, &t);
#else
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
#endif
    local.tm_isdst = 0;
    utc.tm_isdst = 0;
    return (static_cast<i64>(std::mktime(&local)) - static_cast<i64>(std::mktime(&utc))) / 60;
}

u32 sceRtcGetTickResolution() { return 1'000'000; }

i32 sceRtcGetCurrentTick(Ptr<u64> tick) {
    if (!writable(tick.address, 8)) return err::kIllegalAddress;
    const u64 now = now_tick();
    std::memcpy(tick.get(), &now, 8);
    return err::kOk;
}

i32 sceRtcGetCurrentClock(Ptr<io::DateTime> out, i32 time_zone_minutes) {
    if (!writable(out.address, sizeof(io::DateTime))) return err::kIllegalAddress;
    *out = date_of(static_cast<u64>(static_cast<i64>(now_tick()) + i64{time_zone_minutes} * 60'000'000));
    return err::kOk;
}

i32 sceRtcGetCurrentClockLocalTime(Ptr<io::DateTime> out) {
    if (!writable(out.address, sizeof(io::DateTime))) return err::kIllegalAddress;
    *out = date_of(static_cast<u64>(static_cast<i64>(now_tick()) + local_offset_minutes() * 60'000'000));
    return err::kOk;
}

i32 sceRtcGetTick(Ptr<const io::DateTime> in, Ptr<u64> tick) {
    if (!in || !writable(tick.address, 8)) return err::kIllegalAddress;
    const u64 t = tick_of(*in);
    std::memcpy(tick.get(), &t, 8);
    return err::kOk;
}

i32 sceRtcSetTick(Ptr<io::DateTime> out, Ptr<const u64> tick) {
    if (!tick || !writable(out.address, sizeof(io::DateTime))) return err::kIllegalAddress;
    u64 t;
    std::memcpy(&t, tick.get(), 8);
    *out = date_of(t);
    return err::kOk;
}

i32 sceRtcGetTime64_t(Ptr<const io::DateTime> in, Ptr<u64> out) {
    if (!in || !writable(out.address, 8)) return err::kIllegalAddress;
    const u64 seconds = (tick_of(*in) - static_cast<u64>(kUnixEpochDays) * kMicrosPerDay) / 1'000'000;
    std::memcpy(out.get(), &seconds, 8);
    return err::kOk;
}

i32 sceRtcSetTime64_t(Ptr<io::DateTime> out, u64 seconds) {
    if (!writable(out.address, sizeof(io::DateTime))) return err::kIllegalAddress;
    *out = date_of(static_cast<u64>(kUnixEpochDays) * kMicrosPerDay + seconds * 1'000'000);
    return err::kOk;
}

i32 sceDbgAssertionHandler(Ptr<const char> file, i32 line, i32 /*unknown*/, Ptr<const char> component, Ptr<const char> message) {
    nx::loge(logs::hle, "guest assertion failed: {}:{} [{}] {}", str(file, 256), line, str(component, 64), str(message, 512));
    return err::kOk;
}

const Function kRtc[] = {
    HLE_FUNCTION(0x811313B3, sceRtcGetTickResolution),
    HLE_FUNCTION(0x23F79274, sceRtcGetCurrentTick),
    HLE_FUNCTION(0x70FDE8F1, sceRtcGetCurrentClock),
    HLE_FUNCTION(0x0572EDDC, sceRtcGetCurrentClockLocalTime),
    HLE_FUNCTION(0xF2B238E2, sceRtcGetTick),
    HLE_FUNCTION(0xCD89F464, sceRtcSetTick),
    HLE_FUNCTION(0xC995DE02, sceRtcGetTime64_t),
    HLE_FUNCTION(0xA6C36B6A, sceRtcSetTime64_t),
};

const Function kDbg[] = {
    HLE_FUNCTION(0x1AF3678B, sceDbgAssertionHandler),
};

}  // namespace

void register_rtc() {
    register_library({"SceRtcUser", kSceRtcUser, kRtc});
    register_library({"SceDbg", kSceDbg, kDbg});
}

}  // namespace psvrecore::runtime::hle
