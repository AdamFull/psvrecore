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

// Counted calls: a trampoline per bound function (up to kCountedSlots), counting its calls.
constexpr u32 kCountedSlots = 768;
struct CountedSlot {
    PsvFunction function = nullptr;
    const char* name = nullptr;
    std::atomic<u64> calls{0};
    u64 reported = 0;
};
CountedSlot g_counted[kCountedSlots];
std::atomic<u32> g_counted_used{0};

template <u32 I>
void counted_entry(PsvContext* ctx, uint32_t ret) {
    g_counted[I].calls.fetch_add(1, std::memory_order_relaxed);
    g_counted[I].function(ctx, ret);
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
    if (!counting_calls()) return f.function;
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
    nx::vector<std::pair<u64, const char*>> called;
    const u32 used = g_counted_used.load(std::memory_order_acquire);
    for (u32 i = 0; i < used; ++i) {
        const u64 now = g_counted[i].calls.load(std::memory_order_relaxed);
        if (now != g_counted[i].reported) called.push_back({now - g_counted[i].reported, g_counted[i].name});
        g_counted[i].reported = now;
    }
    std::sort(called.begin(), called.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    nx::string out;
    for (const auto& [n, name] : called) out += nx::format("  {:>8} {}\n", n, name);
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
