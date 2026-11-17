// The real-time clock (SceRtcUser): ticks are microseconds since 0001-01-01 00:00:00 UTC. Also the debug library's
// assertion handler (SceDbg).

#include <chrono>
#include <cstring>
#include <ctime>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceRtcUser = 0x6704D3C6, kSceRtc = 0x3503487E, kSceDbg = 0x623DF986;

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

// No network clock: the console's own (the system's clock is the host's).
i32 sceRtcGetCurrentNetworkTick(Ptr<u64> tick) { return sceRtcGetCurrentTick(tick); }

i32 sceRtcConvertUtcToLocalTime(Ptr<const u64> utc, Ptr<u64> local) {
    if (!utc || !writable(local.address, 8)) return err::kIllegalAddress;
    u64 t;
    std::memcpy(&t, utc.get(), 8);
    t = static_cast<u64>(static_cast<i64>(t) + local_offset_minutes() * 60'000'000);
    std::memcpy(local.get(), &t, 8);
    return err::kOk;
}

// out = in + add units (out and in may be the same tick).
i32 tick_add(Ptr<u64> out, Ptr<const u64> in, i64 add, i64 unit_us) {
    if (!in || !writable(out.address, 8)) return err::kIllegalAddress;
    u64 t;
    std::memcpy(&t, in.get(), 8);
    t = static_cast<u64>(static_cast<i64>(t) + add * unit_us);
    std::memcpy(out.get(), &t, 8);
    return err::kOk;
}
i32 sceRtcTickAddMinutes(Ptr<u64> out, Ptr<const u64> in, i64 add) { return tick_add(out, in, add, 60'000'000); }
i32 sceRtcTickAddHours(Ptr<u64> out, Ptr<const u64> in, i32 add) { return tick_add(out, in, add, 3'600'000'000); }
i32 sceRtcTickAddDays(Ptr<u64> out, Ptr<const u64> in, i32 add) {
    return tick_add(out, in, add, static_cast<i64>(kMicrosPerDay));
}
// The same month and day that many years on (29 February becomes the 28th in a year without it).
i32 sceRtcTickAddYears(Ptr<u64> out, Ptr<const u64> in, i32 add) {
    if (!in || !writable(out.address, 8)) return err::kIllegalAddress;
    u64 t;
    std::memcpy(&t, in.get(), 8);
    io::DateTime d = date_of(t);
    const i64 year = i64{d.year} + add;
    if (year < 1 || year > 9999) return err::kInvalidArgument;
    d.year = static_cast<u16>(year);
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (d.month == 2 && d.day == 29 && !leap) d.day = 28;
    t = tick_of(d);
    std::memcpy(out.get(), &t, 8);
    return err::kOk;
}

// RFC 3339 in local time: "YYYY-MM-DDThh:mm:ss.ss+hh:mm" (hundredths of a second).
i32 sceRtcFormatRFC3339LocalTime(Ptr<char> text, Ptr<const u64> utc) {
    if (!utc || !writable(text.address, 32)) return err::kIllegalAddress;
    u64 t;
    std::memcpy(&t, utc.get(), 8);
    const i64 offset = local_offset_minutes();
    const io::DateTime d = date_of(static_cast<u64>(static_cast<i64>(t) + offset * 60'000'000));
    const i64 a = offset < 0 ? -offset : offset;
    const nx::string s = nx::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:02}{}{:02}:{:02}", d.year, d.month, d.day, d.hour,
                                    d.minute, d.second, d.microsecond / 10'000, offset < 0 ? '-' : '+', a / 60, a % 60);
    std::memcpy(text.get(), s.c_str(), s.size() + 1);
    return err::kOk;
}

// ---- more of the clock: comparisons, C times, Win32 file times, more additions, RFC 2822/3339 text, parsing

constexpr i32 kRtcInvalidValue = err::code(0x80251000), kRtcInvalidPointer = err::code(0x80251001),
              kRtcBadParse = err::code(0x80251080);

bool read_tick(Ptr<const u64> p, u64* out) {
    if (!p || !readable(p.address, 8)) return false;
    std::memcpy(out, p.get(), 8);
    return true;
}
bool write_tick(Ptr<u64> p, u64 t) {
    if (!p || !writable(p.address, 8)) return false;
    std::memcpy(p.get(), &t, 8);
    return true;
}

// -1, 0 or 1.
i32 sceRtcCompareTick(Ptr<const u64> a, Ptr<const u64> b) {
    u64 x, y;
    if (!read_tick(a, &x) || !read_tick(b, &y)) return kRtcInvalidPointer;
    return x < y ? -1 : x > y ? 1 : 0;
}
// 0 Sunday to 6 Saturday.
i32 sceRtcGetDayOfWeek(i32 year, i32 month, i32 day) {
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31) return kRtcInvalidValue;
    const i64 days = days_from_civil(year, static_cast<u32>(month), static_cast<u32>(day));  // (1970-01-01: Thursday)
    return static_cast<i32>(((days % 7) + 7 + 4) % 7);
}
// time_t: 32-bit seconds since 1970 (the C library's).
i32 sceRtcGetTime_t(Ptr<const io::DateTime> in, Ptr<u32> out) {
    if (!in || !writable(out.address, 4)) return kRtcInvalidPointer;
    const i64 seconds = (static_cast<i64>(tick_of(*in)) - kUnixEpochDays * static_cast<i64>(kMicrosPerDay)) / 1'000'000;
    *out = static_cast<u32>(seconds);
    return 0;
}
i32 sceRtcSetTime_t(Ptr<io::DateTime> out, u32 seconds) {
    if (!writable(out.address, sizeof(io::DateTime))) return kRtcInvalidPointer;
    *out = date_of(static_cast<u64>(kUnixEpochDays) * kMicrosPerDay + u64{seconds} * 1'000'000);
    return 0;
}
// Win32 FILETIME: 100 ns units since 1601-01-01.
i32 sceRtcGetWin32FileTime(Ptr<const io::DateTime> in, Ptr<u64> out) {
    if (!in) return kRtcInvalidPointer;
    constexpr u64 k1601 = 584388ull * kMicrosPerDay;  // (days from 0001-01-01 to 1601-01-01)
    const u64 t = tick_of(*in);
    if (t < k1601) return kRtcInvalidValue;
    return write_tick(out, (t - k1601) * 10) ? 0 : kRtcInvalidPointer;
}
i32 sceRtcConvertLocalTimeToUtc(Ptr<const u64> local, Ptr<u64> utc) {
    u64 t;
    if (!read_tick(local, &t)) return kRtcInvalidPointer;
    return write_tick(utc, static_cast<u64>(static_cast<i64>(t) - local_offset_minutes() * 60'000'000)) ? 0 : kRtcInvalidPointer;
}
i32 sceRtcTickAddMicroseconds(Ptr<u64> out, Ptr<const u64> in, i64 add) { return tick_add(out, in, add, 1); }
i32 sceRtcTickAddSeconds(Ptr<u64> out, Ptr<const u64> in, i64 add) { return tick_add(out, in, add, 1'000'000); }
// The same day that many months on (past the month's end: its last day).
i32 sceRtcTickAddMonths(Ptr<u64> out, Ptr<const u64> in, i32 add) {
    u64 t;
    if (!read_tick(in, &t)) return kRtcInvalidPointer;
    io::DateTime d = date_of(t);
    const i64 months = i64{d.year} * 12 + (d.month - 1) + add;
    const i64 year = months / 12;
    if (months < 12 || year > 9999) return kRtcInvalidValue;
    d.year = static_cast<u16>(year);
    d.month = static_cast<u16>(months % 12 + 1);
    const u32 last = static_cast<u32>(days_from_civil(year + (d.month == 12), d.month == 12 ? 1 : d.month + 1u, 1) -
                                      days_from_civil(year, d.month, 1));
    if (d.day > last) d.day = static_cast<u16>(last);
    return write_tick(out, tick_of(d)) ? 0 : kRtcInvalidPointer;
}

// RFC 3339 in the time zone given: "YYYY-MM-DDThh:mm:ss.ss+hh:mm" (hundredths of a second; Z for UTC).
i32 sceRtcFormatRFC3339(Ptr<char> text, Ptr<const u64> utc, i32 zone_minutes) {
    u64 t;
    if (!read_tick(utc, &t) || !writable(text.address, 32)) return kRtcInvalidPointer;
    const io::DateTime d = date_of(static_cast<u64>(static_cast<i64>(t) + i64{zone_minutes} * 60'000'000));
    const i32 a = zone_minutes < 0 ? -zone_minutes : zone_minutes;
    const nx::string zone = zone_minutes ? nx::format("{}{:02}:{:02}", zone_minutes < 0 ? '-' : '+', a / 60, a % 60) : "Z";
    const nx::string s = nx::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:02}{}", d.year, d.month, d.day, d.hour, d.minute,
                                    d.second, d.microsecond / 10'000, zone);
    std::memcpy(text.get(), s.c_str(), s.size() + 1);
    return 0;
}
// RFC 2822 in local time: "Wed, 01 Jan 2014 09:00:00 +0900".
i32 sceRtcFormatRFC2822LocalTime(Ptr<char> text, Ptr<const u64> utc) {
    static const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    u64 t;
    if (!read_tick(utc, &t) || !writable(text.address, 32)) return kRtcInvalidPointer;
    const i64 offset = local_offset_minutes();
    const io::DateTime d = date_of(static_cast<u64>(static_cast<i64>(t) + offset * 60'000'000));
    const i64 a = offset < 0 ? -offset : offset;
    const nx::string s = nx::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} {}{:02}{:02}",
                                    kDays[sceRtcGetDayOfWeek(d.year, d.month, d.day)], d.day, kMonths[d.month - 1], d.year,
                                    d.hour, d.minute, d.second, offset < 0 ? '-' : '+', a / 60, a % 60);
    std::memcpy(text.get(), s.c_str(), s.size() + 1);
    return 0;
}
// RFC 3339 text to a UTC tick: a date, 'T' or a space, a time, optional fractions, Z or an offset.
i32 sceRtcParseRFC3339(Ptr<u64> utc, Ptr<const char> text) {
    if (!utc || !text || !writable(utc.address, 8)) return kRtcInvalidPointer;
    const nx::string s = str(text, 64);
    usize i = 0;
    const auto number = [&](u32 digits, u32* out) {
        u32 v = 0;
        for (u32 k = 0; k < digits; ++k, ++i) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + static_cast<u32>(s[i] - '0');
        }
        *out = v;
        return true;
    };
    const auto expect = [&](const char* any) {
        if (i < s.size() && std::strchr(any, s[i])) return ++i, true;
        return false;
    };
    u32 y, mo, d, h, mi, sec;
    if (!number(4, &y) || !expect("-") || !number(2, &mo) || !expect("-") || !number(2, &d) || !expect("Tt ") ||
        !number(2, &h) || !expect(":") || !number(2, &mi) || !expect(":") || !number(2, &sec))
        return kRtcBadParse;
    u32 micro = 0;
    if (expect(".")) {
        u32 scale = 100000;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            micro += static_cast<u32>(s[i++] - '0') * scale;
            scale /= 10;
        }
    }
    i64 zone = 0;
    if (expect("Zz")) {
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const bool minus = s[i++] == '-';
        u32 zh, zm;
        if (!number(2, &zh) || !expect(":") || !number(2, &zm)) return kRtcBadParse;
        zone = (minus ? -1 : 1) * static_cast<i64>(zh * 60 + zm);
    } else {
        return kRtcBadParse;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return kRtcBadParse;
    io::DateTime dt{};
    dt.year = static_cast<u16>(y), dt.month = static_cast<u16>(mo), dt.day = static_cast<u16>(d);
    dt.hour = static_cast<u16>(h), dt.minute = static_cast<u16>(mi), dt.second = static_cast<u16>(sec);
    dt.microsecond = micro;
    write_tick(utc, static_cast<u64>(static_cast<i64>(tick_of(dt)) - zone * 60'000'000));
    return 0;
}

// (SceRtc) The time the console has run, in microseconds: the process's here.
u64 sceRtcGetAccumulativeTime() { return kernel().clock_ns() / 1000; }

const Function kRtcMore[] = {
    HLE_FUNCTION(0xC7385158, sceRtcCompareTick),
    HLE_FUNCTION(0x2F3531EB, sceRtcGetDayOfWeek),
    HLE_FUNCTION(0x8DE6FEB7, sceRtcGetTime_t),
    HLE_FUNCTION(0x3A332F81, sceRtcSetTime_t),
    HLE_FUNCTION(0x8A95E119, sceRtcGetWin32FileTime),
    HLE_FUNCTION(0x0A05E201, sceRtcConvertLocalTimeToUtc),
    HLE_FUNCTION(0xAE26D920, sceRtcTickAddMicroseconds),
    HLE_FUNCTION(0x979AFD79, sceRtcTickAddSeconds),
    HLE_FUNCTION(0x6321B4AA, sceRtcTickAddMonths),
    HLE_FUNCTION(0xCCEA2B54, sceRtcFormatRFC3339),
    HLE_FUNCTION(0x42CA8EB5, sceRtcFormatRFC2822LocalTime),
    HLE_FUNCTION(0x2D18AEEC, sceRtcParseRFC3339),
};
const Function kRtcKernel[] = {HLE_FUNCTION(0x258BE8EC, sceRtcGetAccumulativeTime)};

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
    HLE_FUNCTION(0xCDDD25FE, sceRtcGetCurrentNetworkTick),
    HLE_FUNCTION(0x1282C436, sceRtcConvertUtcToLocalTime),
    HLE_FUNCTION(0x4C358871, sceRtcTickAddMinutes),
    HLE_FUNCTION(0x6F193F55, sceRtcTickAddHours),
    HLE_FUNCTION(0x58DE3C70, sceRtcTickAddDays),
    HLE_FUNCTION(0xDF6C3E1B, sceRtcTickAddYears),
    HLE_FUNCTION(0x742250A9, sceRtcFormatRFC3339LocalTime),
};

const Function kDbg[] = {
    HLE_FUNCTION(0x1AF3678B, sceDbgAssertionHandler),
};

}  // namespace

void register_rtc() {
    register_library({"SceRtcUser", kSceRtcUser, kRtc});
    register_library({"SceRtcUser", kSceRtcUser, kRtcMore});
    register_library({"SceRtc", kSceRtc, kRtcKernel});
    register_library({"SceDbg", kSceDbg, kDbg});
}

}  // namespace psvrecore::runtime::hle
