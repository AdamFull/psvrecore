#include "psvrecore/runtime/hle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "psvrecore/log.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::hle {

namespace {

// A library's functions and variables, gathered from every source that registers some (SceLibKernel has threads,
// files and string functions).
struct Registered {
    const char* name;
    u32 nid;
    nx::vector<Function> functions;
    nx::vector<Variable> variables;
    nx::vector<u32> variable_addresses;  // (allocated on first use)
};

nx::mutex g_lock;
nx::vector<Registered>& libraries() {
    static nx::vector<Registered> list;
    return list;
}

Registered* find_library(u32 library_nid, nx::string_view name) {
    for (auto& r : libraries())
        if (r.nid == library_nid) return &r;
    for (auto& r : libraries())
        if (name == r.name) return &r;
    return nullptr;
}

// Counted calls: a trampoline per bound function (up to kCountedSlots), counting its calls and the time spent in
// them (waits included: a function that blocks shows where the game waits).
constexpr u32 kCountedSlots = 768;
struct CountedSlot {
    PsvFunction function = nullptr;
    const char* name = nullptr;
    std::atomic<u64> calls{0};
    std::atomic<u64> ns{0};
    u64 reported = 0;
    u64 reported_ns = 0;
};
CountedSlot g_counted[kCountedSlots];
std::atomic<u32> g_counted_used{0};

// Traced calls (PSVRECORE_HITCH_TRACE): each guest thread's last calls, when each began and ended, for what the threads
// did while a frame came late (describe_window). A ring a thread, written by its thread only.
constexpr usize kTraceEvents = 4096;
struct TraceEvent {
    u32 slot = 0;
    u64 start = 0, end = 0;
};
struct ThreadTrace {
    nx::string name;
    std::atomic<u64> next{0};
    TraceEvent events[kTraceEvents];
};
nx::vector<ThreadTrace*> g_traces;  // (under g_lock; never freed: a thread's may be read after it ends)
thread_local ThreadTrace* t_trace = nullptr;

}  // namespace

bool tracing() {
    static const bool on = [] {
        const char* spec = std::getenv("PSVRECORE_HITCH_TRACE");
        return spec && *spec && *spec != '0';
    }();
    return on;
}

namespace {

bool tracing_calls() { return tracing(); }

void trace_call(u32 slot, u64 start, u64 end) {
    if (!t_trace) {
        auto* t = new ThreadTrace;
        if (kernel::Thread* k = Kernel::current()) t->name = k->name;
        nx::scoped_lock<nx::mutex> held(g_lock);
        g_traces.push_back(t);
        t_trace = t;
    }
    const u64 n = t_trace->next.load(std::memory_order_relaxed);
    t_trace->events[n % kTraceEvents] = TraceEvent{slot, start, end};
    t_trace->next.store(n + 1, std::memory_order_release);
}

template <u32 I>
void counted_entry(PsvContext* ctx, uint32_t ret) {
    g_counted[I].calls.fetch_add(1, std::memory_order_relaxed);
    const u64 start = nx::os::ticks_ns();
    g_counted[I].function(ctx, ret);
    const u64 end = nx::os::ticks_ns();
    g_counted[I].ns.fetch_add(end - start, std::memory_order_relaxed);
    if (tracing_calls()) trace_call(I, start, end);
}
template <u32... I>
constexpr std::array<PsvFunction, sizeof...(I)> counted_entries(std::integer_sequence<u32, I...>) {
    return {&counted_entry<I>...};
}
constexpr auto kCountedEntries = counted_entries(std::make_integer_sequence<u32, kCountedSlots>{});

}  // namespace

bool counting_calls() {
    static const bool on = [] {
        const char* spec = std::getenv("PSVRECORE_HLE_COUNTS");
        return spec && *spec;
    }();
    return on;
}

PsvFunction bind(const Function& f) {
    if (!counting_calls() && !tracing_calls()) return f.function;
    nx::scoped_lock<nx::mutex> held(g_lock);
    const u32 used = g_counted_used.load(std::memory_order_relaxed);
    for (u32 i = 0; i < used; ++i)
        if (g_counted[i].function == f.function) return kCountedEntries[i];
    if (used == kCountedSlots) return f.function;
    g_counted[used].function = f.function;
    g_counted[used].name = f.name;
    g_counted_used.store(used + 1, std::memory_order_release);
    return kCountedEntries[used];
}

nx::string describe_calls() {
    nx::scoped_lock<nx::mutex> held(g_lock);
    struct Called {
        u64 calls, ns;
        const char* name;
    };
    nx::vector<Called> called;
    const u32 used = g_counted_used.load(std::memory_order_acquire);
    for (u32 i = 0; i < used; ++i) {
        const u64 now = g_counted[i].calls.load(std::memory_order_relaxed);
        const u64 ns = g_counted[i].ns.load(std::memory_order_relaxed);
        if (now != g_counted[i].reported) called.push_back({now - g_counted[i].reported, ns - g_counted[i].reported_ns, g_counted[i].name});
        g_counted[i].reported = now;
        g_counted[i].reported_ns = ns;
    }
    std::sort(called.begin(), called.end(), [](const Called& a, const Called& b) { return a.ns > b.ns; });
    nx::string out;
    for (const Called& c : called)
        out += nx::format("  {:>8} {:>9.1f} ms {}\n", c.calls, static_cast<f64>(c.ns) / 1e6, c.name);
    return out;
}

nx::string describe_window(u64 from, u64 to) {
    if (!tracing_calls()) return {};
    nx::vector<ThreadTrace*> traces;
    {
        nx::scoped_lock<nx::mutex> held(g_lock);
        traces = g_traces;
    }
    const auto ms = [](u64 ns) { return static_cast<f64>(ns) / 1e6; };
    nx::string out;
    for (ThreadTrace* t : traces) {
        const u64 next = t->next.load(std::memory_order_acquire);
        nx::vector<TraceEvent> in;
        u64 last_end = 0;  // (the last call's end before the window)
        for (u64 k = next > kTraceEvents ? next - kTraceEvents : 0; k < next; ++k) {
            const TraceEvent& e = t->events[k % kTraceEvents];
            if (e.end >= from && e.start <= to) in.push_back(e);
            else if (e.end < from) last_end = nx::max(last_end, e.end);
        }
        if (in.empty()) {
            // (no call in the window: in its own code all along, if it called within the second before)
            if (last_end && from - last_end < 1'000'000'000)
                out += nx::format("  {}: no calls; [its own code] since {:.1f} ms before\n",
                                  t->name.empty() ? "(host)" : t->name.c_str(), ms(from - last_end));
            continue;
        }
        std::sort(in.begin(), in.end(), [](const TraceEvent& a, const TraceEvent& b) { return a.start < b.start; });
        // (the long calls, and the long stretches of the game's own code between calls: 2 ms or more)
        struct Part {
            u64 at, ns;
            const char* what;
        };
        nx::vector<Part> parts;
        u64 in_calls = 0;
        for (usize i = 0; i < in.size(); ++i) {
            const u64 s = nx::max(in[i].start, from), e = nx::min(in[i].end, to);
            in_calls += e > s ? e - s : 0;
            if (e > s && e - s >= 2'000'000) parts.push_back({s, e - s, g_counted[in[i].slot].name});
            const u64 gap_from = i ? in[i - 1].end : from;
            if (in[i].start > gap_from && in[i].start - gap_from >= 2'000'000)
                parts.push_back({nx::max(gap_from, from), in[i].start - nx::max(gap_from, from), nullptr});
        }
        if (in.back().end < to && to - in.back().end >= 2'000'000) parts.push_back({in.back().end, to - in.back().end, nullptr});
        out += nx::format("  {}: {} calls, {:.1f} ms in them;", t->name.empty() ? "(host)" : t->name.c_str(), in.size(),
                          ms(in_calls));
        for (const Part& p : parts)
            out += nx::format(" {} {:.1f} at +{:.1f};", p.what ? p.what : "[its own code]", ms(p.ns), ms(p.at - from));
        out += "\n";
    }
    return out;
}

void detail::after_call() {
    if (Runtime* rt = current_runtime()) rt->kernel().checkpoint();
}

void register_library(const Library& library) {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    Registered* r = nullptr;
    for (auto& existing : libraries())
        if (existing.nid == library.nid) r = &existing;
    if (!r) {
        libraries().push_back(Registered{library.name, library.nid, {}, {}, {}});
        r = &libraries().back();
    }
    for (const auto& f : library.functions) {
        auto it = std::find_if(r->functions.begin(), r->functions.end(), [&](const Function& g) { return g.nid == f.nid; });
        if (it != r->functions.end()) *it = f;
        else r->functions.push_back(f);
    }
    for (const auto& v : library.variables) {
        r->variables.push_back(v);
        r->variable_addresses.push_back(0);
    }
}

void clear_libraries() {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    libraries().clear();
}

const Function* find_function(u32 library_nid, nx::string_view library, u32 nid) {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    Registered* r = find_library(library_nid, library);
    if (!r) return nullptr;
    for (const auto& f : r->functions)
        if (f.nid == nid) return &f;
    return nullptr;
}

const Variable* find_variable(u32 library_nid, nx::string_view library, u32 nid, u32* guest_address) {
    nx::scoped_lock<nx::mutex> guard(g_lock);
    Registered* r = find_library(library_nid, library);
    if (!r) return nullptr;
    for (usize i = 0; i < r->variables.size(); ++i) {
        const Variable& v = r->variables[i];
        if (v.nid != nid) continue;
        if (!r->variable_addresses[i]) {
            Runtime* rt = current_runtime();
            if (!rt) return nullptr;
            const u32 at = rt->allocate_system(v.size ? v.size : 4, v.name);
            if (!at) return nullptr;
            if (v.init) v.init(rt->memory().host(at), at);
            r->variable_addresses[i] = at;
        }
        if (guest_address) *guest_address = r->variable_addresses[i];
        return &v;
    }
    return nullptr;
}

u32 call_guest(PsvContext* ctx, u32 code, std::span<const u32> args) {
    const PsvFunction f = recomp::find_function(code);
    if (!f) psv_trap(ctx, code, recomp::kTrapUnknownTarget);
    const u32 saved_sp = ctx->r[13];
    for (usize i = 0; i < args.size() && i < 4; ++i) ctx->r[i] = args[i];
    if (args.size() > 4) {
        const u32 extra = static_cast<u32>(args.size() - 4);
        u32 sp = (saved_sp - 4 * extra) & ~7u;
        for (u32 i = 0; i < extra; ++i) psv_st32(sp + 4 * i, args[4 + i]);
        ctx->r[13] = sp;
    }
    ctx->r[14] = kHostReturn;
    f(ctx, kHostReturn);
    ctx->r[13] = saved_sp;
    return ctx->r[0];
}

}  // namespace psvrecore::runtime::hle
