// The console's profiling and debugging libraries, as games call (and ship with) them (docs/PROFILING.md):
// - ScePerf (Razor's CPU side): the CPU markers are zones in Tracy (PSVRECORE_TRACY=ON), on the calling guest thread's
//   or fiber's lane; a capture is what Tracy records while a profiler is connected, so sceRazorCpuIsCapturing says
//   whether one is, and starting or stopping one does nothing else. The time base is the kernel's clock. The ARM
//   performance monitor's counters (cycles, cache misses, ...) have no host equivalent worth faking: selected,
//   started and stopped, they count nothing and read 0. The user marker and activity monitor traces (Razor's own
//   capture buffers) stay empty.
// - SceRazorHud (Razor's on-screen HUD and its GPU live metrics): accepted, nothing drawn or measured.
// - SceDbg (libdbg's handlers behind SCE_DBG_LOG_* and SCE_DBG_ASSERT*): the game's messages to psvrecore's log, at
//   their level; breaking into a debugger on an error or warning is not done.
// (SceRazorCapture, the GPU capture triggers, is gxm's: a RenderDoc capture.)

#include <atomic>

#include "hle_common.hpp"
#include "psvrecore/profile.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kScePerf = 0x447F047D, kSceRazorHud = 0x250371C2, kSceDbg = 0x623DF986;
constexpr i32 kPerfInvalidArgument = err::code(0x80580000);

// ---- ScePerf: Razor's CPU markers and captures

// A marker's label, as Tracy shows it (an unreadable one: "(marker)").
nx::string label(Ptr<const char> p) {
    nx::string s = str(p, 256);
    if (s.empty()) s = "(marker)";
    return s;
}

i32 sceRazorCpuPushMarker(Ptr<const char> name) {
    if constexpr (profile::kEnabled) profile::push(label(name));
    (void)name;
    return 0;
}
// (the HUD flag shows the marker on Razor's HUD: there is none)
i32 sceRazorCpuPushMarkerWithHud(Ptr<const char> name, u32 color, u32 /*flags*/) {
    if constexpr (profile::kEnabled) profile::push(label(name), profile::rgb_of_abgr(color));
    (void)name, (void)color;
    return 0;
}
i32 sceRazorCpuPopMarker() {
    profile::pop();  // (one never pushed: nothing; the console's library answers success too while not capturing)
    return 0;
}
i32 sceRazorCpuSync() { return 0; }

i32 sceRazorCpuStartCapture() {
    nx::logd(logs::hle, "Razor: the game starts a CPU capture (Tracy records while a profiler is connected)");
    return 0;
}
i32 sceRazorCpuStopCapture() { return 0; }
u32 sceRazorCpuIsCapturing() { return profile::connected() ? 1 : 0; }

// (libperf's set-up, and the fibers' markers from inside libfiber, whose arguments are not known: libfiber's own
// functions are psvrecore's, and mark their fibers' lanes themselves)
i32 _sceRazorCpuInit() { return 0; }
i32 _sceCpuRazorPushFiberUserMarker() { return 0; }
i32 _sceCpuRazorPopFiberUserMarker() { return 0; }
// (libult's packets, written when the capture flag below is set: never)
i32 _sceRazorCpuWriteFiberUltPkt() { return 0; }

// Razor's own trace buffers (user markers, CPU activity): the game's buffer stays empty; the trace read back is that
// buffer with no packets in it.
struct TraceBuffers {
    nx::mutex lock;
    u32 markers = 0, activity = 0;
};
TraceBuffers& traces() {
    static TraceBuffers t;
    return t;
}
i32 start_trace(u32& slot, u32 buffer, u32 size) {
    if (!buffer || !size || !writable(buffer, size)) return kPerfInvalidArgument;
    nx::scoped_lock<nx::mutex> held(traces().lock);
    slot = buffer;
    return 0;
}
i32 stop_trace(u32& slot) {
    nx::scoped_lock<nx::mutex> held(traces().lock);
    slot = 0;
    return 0;
}
i32 trace_buffer(u32 slot, Ptr<u32> out) {
    if (!out || !writable(out.address, 4)) return kPerfInvalidArgument;
    *out = slot;
    return 0;  // (the number of packets in it)
}
i32 sceRazorCpuStartUserMarkerTrace(u32 buffer, u32 size) { return start_trace(traces().markers, buffer, size); }
i32 sceRazorCpuStopUserMarkerTrace() { return stop_trace(traces().markers); }
i32 sceRazorCpuGetUserMarkerTraceBuffer(Ptr<u32> out) { return trace_buffer(traces().markers, out); }
i32 sceRazorCpuStartActivityMonitor(u32 buffer, u32 size) { return start_trace(traces().activity, buffer, size); }
i32 sceRazorCpuStopActivityMonitor() { return stop_trace(traces().activity); }
i32 sceRazorCpuGetActivityMonitorTraceBuffer(Ptr<u32> out) { return trace_buffer(traces().activity, out); }

// ---- ScePerf: the ARM performance monitor (accepted; every counter reads 0) and the time base

constexpr u32 kCycleCounter = 31, kCounters = 6;
bool counter_valid(u32 counter) { return counter < kCounters || counter == kCycleCounter; }

i32 scePerfArmPmonReset(i32 /*thread*/) { return 0; }
i32 scePerfArmPmonSelectEvent(i32 /*thread*/, u32 counter, u32 /*event*/) {
    return counter < kCounters ? 0 : kPerfInvalidArgument;
}
i32 scePerfArmPmonStart(i32 /*thread*/) { return 0; }
i32 scePerfArmPmonStop(i32 /*thread*/) { return 0; }
i32 scePerfArmPmonGetCounterValue(i32 /*thread*/, u32 counter, Ptr<u32> value) {
    if (!counter_valid(counter) || !value || !writable(value.address, 4)) return kPerfInvalidArgument;
    *value = 0;
    return 0;
}
i32 scePerfArmPmonSetCounterValue(i32 /*thread*/, u32 counter, u32 /*value*/) {
    return counter_valid(counter) ? 0 : kPerfInvalidArgument;
}
i32 scePerfArmPmonSoftwareIncrement(u32 /*mask*/) { return 0; }

// The time base: the kernel's clock (the guest's time, without what the game spent suspended), in nanoseconds; its
// frequency, as the SDK's samples read it, in ticks a microsecond.
u64 scePerfGetTimebaseValue() { return kernel().clock_ns(); }
u32 scePerfGetTimebaseFrequency() { return 1000; }

const Function kPerf[] = {
    HLE_FUNCTION(0x7AD6AC30, _sceRazorCpuInit),
    HLE_FUNCTION(0xC3DE4C0A, sceRazorCpuPushMarker),
    HLE_FUNCTION(0x5CDC09EA, sceRazorCpuPushMarkerWithHud),
    HLE_FUNCTION(0xDC3224C3, sceRazorCpuPopMarker),
    HLE_FUNCTION(0x8F7B522C, _sceCpuRazorPushFiberUserMarker),
    HLE_FUNCTION(0x453AED06, _sceCpuRazorPopFiberUserMarker),
    HLE_FUNCTION(0x409D966A, _sceRazorCpuWriteFiberUltPkt),
    HLE_FUNCTION(0x4F1385E3, sceRazorCpuSync),
    HLE_FUNCTION(0xDAB63FC3, sceRazorCpuStartCapture),
    HLE_FUNCTION(0x9C2C05C2, sceRazorCpuStopCapture),
    HLE_FUNCTION(0x8DA87B35, sceRazorCpuIsCapturing),
    HLE_FUNCTION(0xAED36A40, sceRazorCpuStartUserMarkerTrace),
    HLE_FUNCTION(0x4B47E144, sceRazorCpuStopUserMarkerTrace),
    HLE_FUNCTION(0x0DE231D4, sceRazorCpuGetUserMarkerTraceBuffer),
    HLE_FUNCTION(0x3F29E937, sceRazorCpuStartActivityMonitor),
    HLE_FUNCTION(0x9D5B9621, sceRazorCpuStopActivityMonitor),
    HLE_FUNCTION(0xF7F8ADCE, sceRazorCpuGetActivityMonitorTraceBuffer),
    HLE_FUNCTION(0x35151735, scePerfArmPmonReset),
    HLE_FUNCTION(0x63CBEA8B, scePerfArmPmonSelectEvent),
    HLE_FUNCTION(0xC9D969D5, scePerfArmPmonStart),
    HLE_FUNCTION(0xD1A40F54, scePerfArmPmonStop),
    HLE_FUNCTION(0x6132A497, scePerfArmPmonGetCounterValue),
    HLE_FUNCTION(0x12F6C708, scePerfArmPmonSetCounterValue),
    HLE_FUNCTION(0x4264B4E7, scePerfArmPmonSoftwareIncrement),
    HLE_FUNCTION(0xBD9615E5, scePerfGetTimebaseValue),
    HLE_FUNCTION(0x78EA4FFB, scePerfGetTimebaseFrequency),
};
const Variable kPerfVariables[] = {
    // (the pointer, then the flag it points to: libult reads the flag through it unconditionally; 0, no packets)
    {0x936A5F31, "_pLibPerfCaptureFlagPtr", 8, [](u8* host, u32 guest) {
         const u32 words[2] = {guest + 4, 0};
         std::memcpy(host, words, sizeof words);
     }},
};

// ---- SceRazorHud: accepted (no HUD, no GPU metrics)

constexpr i32 kGpuPerfModeNone = 1;  // SCE_RAZOR_GPU_PERF_MODE_NONE

i32 razor_hud_accepted() { return 0; }
i32 sceRazorGpuPerfGetMode() { return kGpuPerfModeNone; }

const Function kRazorHud[] = {
    Function{0x29CEFDAA, "sceRazorGpuLiveSetBuffer", &entry<&razor_hud_accepted>},
    Function{0x528AA4C7, "sceRazorGpuTraceTrigger", &entry<&razor_hud_accepted>},
    Function{0x6DFBAC39, "sceRazorCpuHudUserMarkerEnableThreadId", &entry<&razor_hud_accepted>},
    Function{0x6FD64DE6, "sceRazorGpuLiveSetMetricsGroup", &entry<&razor_hud_accepted>},
    Function{0x73F83357, "sceRazorGpuTraceRingBufferSize", &entry<&razor_hud_accepted>},
    Function{0x77F2F606, "sceRazorHudSetDisplayEnabled", &entry<&razor_hud_accepted>},
    Function{0x7A51F1A0, "sceRazorGpuTraceRingBufferUsed", &entry<&razor_hud_accepted>},
    Function{0xB46382FC, "sceRazorGpuLiveStart", &entry<&razor_hud_accepted>},
    Function{0xBCB790E9, "sceRazorGpuLiveTriggerDebugDump", &entry<&razor_hud_accepted>},
    Function{0xBD360A69, "sceRazorGpuLiveStop", &entry<&razor_hud_accepted>},
    Function{0xBFEADB77, "sceRazorGpuTraceSetMetricsGroup", &entry<&razor_hud_accepted>},
    Function{0xCC390860, "sceRazorHudSetDisplayFrameCount", &entry<&razor_hud_accepted>},
    Function{0xDFD00BAE, "sceRazorHudReloadSettings", &entry<&razor_hud_accepted>},
    HLE_FUNCTION(0xE3709B44, sceRazorGpuPerfGetMode),
    Function{0xECE363EF, "sceRazorCpuHudSetUserMarkerTraceBuffer", &entry<&razor_hud_accepted>},
    Function{0xF867EFE1, "sceRazorGpuTraceSetFilename", &entry<&razor_hud_accepted>},
    Function{0xFD290A17, "sceRazorCpuHudUserMarkerEnableAllThreads", &entry<&razor_hud_accepted>},
};

// ---- SceDbg: libdbg's handlers

// SceDbgLogLevel: trace, debug, info, warning, error.
std::atomic<i32> g_minimum_level{0};

nx::string dbg_message(Ptr<const char> file, i32 line, Ptr<const char> component, Ptr<const char> message,
                       Variadic va) {
    const nx::string text = format_variadic(str(message, 4096), va);
    const nx::string comp = str(component, 64);
    nx::string where = str(file, 256);
    if (!where.empty()) where = nx::format(" ({}:{})", where, line);
    return comp.empty() ? nx::format("{}{}", text, where) : nx::format("[{}] {}{}", comp, text, where);
}

// sceDbgAssertionHandler(file, line, stop, component, message, ...): an assertion the game's code failed.
i32 sceDbgAssertionHandler(Ptr<const char> file, i32 line, u32 stop, Ptr<const char> component,
                           Ptr<const char> message, Variadic va) {
    nx::loge(logs::hle, "guest assertion failed{}: {}", stop ? "" : " (going on)",
             dbg_message(file, line, component, message, va));
    return err::kOk;
}

// sceDbgLoggingHandler(file, line, severity, component, message, ...): SCE_DBG_LOG_*'s output, at its level.
i32 sceDbgLoggingHandler(Ptr<const char> file, i32 line, i32 severity, Ptr<const char> component,
                         Ptr<const char> message, Variadic va) {
    if (severity < g_minimum_level.load(std::memory_order_relaxed)) return err::kOk;
    const nx::string text = dbg_message(file, line, component, message, va);
    switch (severity) {
        case 0:
        case 1: nx::logd(logs::hle, "guest: {}", text); break;
        case 2: nx::logi(logs::hle, "guest: {}", text); break;
        case 3: nx::logw(logs::hle, "guest: {}", text); break;
        default: nx::loge(logs::hle, "guest: {}", text); break;
    }
    return err::kOk;
}

i32 sceDbgSetMinimumLogLevel(i32 level) {
    g_minimum_level.store(level, std::memory_order_relaxed);
    return err::kOk;
}
// (breaking into a debugger when an error or a warning is logged: there is none to break into)
i32 sceDbgSetBreakOnErrorState(i32 /*state*/) { return err::kOk; }
i32 sceDbgSetBreakOnWarningState(i32 /*state*/) { return err::kOk; }

const Function kDbg[] = {
    HLE_FUNCTION(0x1AF3678B, sceDbgAssertionHandler),
    HLE_FUNCTION(0x6605AB19, sceDbgLoggingHandler),
    HLE_FUNCTION(0x941622FA, sceDbgSetMinimumLogLevel),
    HLE_FUNCTION(0xED4A00BA, sceDbgSetBreakOnErrorState),
    HLE_FUNCTION(0x3DEAECD3, sceDbgSetBreakOnWarningState),
};

}  // namespace

void register_debug() {
    g_minimum_level.store(0, std::memory_order_relaxed);
    {
        nx::scoped_lock<nx::mutex> held(traces().lock);
        traces().markers = traces().activity = 0;
    }
    register_library({"ScePerf", kScePerf, kPerf, kPerfVariables});
    register_library({"SceRazorHud", kSceRazorHud, kRazorHud});
    register_library({"SceDbg", kSceDbg, kDbg});
}

}  // namespace psvrecore::runtime::hle
