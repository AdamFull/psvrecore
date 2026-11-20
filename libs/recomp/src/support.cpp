#include "psvrecore/recomp/support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "psvrecore/log.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

extern "C" {
uint8_t* psv_membase = nullptr;
}

namespace psvrecore::recomp {

namespace {

// Registered before guest code runs; read-only afterwards.
nx::flat_hash_map<u32, PsvFunction>& functions() {
    static nx::flat_hash_map<u32, PsvFunction> map;
    return map;
}

// Each module's import stubs, by address. Registered with the functions.
nx::flat_hash_map<u32, const PsvImport*>& import_stubs() {
    static nx::flat_hash_map<u32, const PsvImport*> map;
    return map;
}

// The registered modules' indirect-call tables (guest.h, PsvDispatch), and the memory each holds.
struct Table {
    PsvDispatch* dispatch = nullptr;
    u32 base = 0, span = 0;  // (the dispatch's, also for a wide table, whose dispatch has no span: psv_call only)
    nx::vector<u16> index16;
    nx::vector<u32> index32;
    nx::vector<PsvFunction> functions;
    nx::vector<u32> addresses;  // (by index in `functions`: the code address)
};
nx::vector<nx::unique_ptr<Table>>& tables() {
    static nx::vector<nx::unique_ptr<Table>> list;
    return list;
}
// The same tables for psv_call, which other threads may run while a module is registered (a module the game loads
// itself): published one by one, each complete before the count says it is there.
constexpr u32 kMaxTables = 64;
std::atomic<Table*> g_table_slots[kMaxTables];
std::atomic<u32> g_table_count{0};

// The table's function at a Thumb code address, or nullptr.
PsvFunction* table_slot(Table& t, u32 code) {
    const u32 off = code - t.base - 1u;
    if (off >= t.span || (off & 1u)) return nullptr;
    const u32 i = t.index32.empty() ? t.index16[off >> 1] : t.index32[off >> 1];
    return i ? &t.functions[i - 1] : nullptr;
}

// Fills a module's table from its functions (the Thumb ones).
void fill_table(const PsvModuleCode& module) {
    PsvDispatch* d = module.dispatch;
    *d = PsvDispatch{};
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (u32 i = 0; i < module.function_count; ++i) {
        const u32 a = module.functions[i].address;
        if (!(a & 1u)) continue;
        lo = std::min(lo, a & ~1u), hi = std::max(hi, a & ~1u);
    }
    if (lo > hi) return;  // (no Thumb function)
    auto t = nx::make_unique<Table>();
    t->dispatch = d;
    t->functions.reserve(module.function_count);
    const u32 entries = (hi - lo) / 2 + 1;
    const bool wide = module.function_count >= 0xFFFFu;
    if (wide) t->index32.assign(entries, 0);
    else t->index16.assign(entries, 0);
    for (u32 i = 0; i < module.function_count; ++i) {
        const u32 a = module.functions[i].address;
        if (!(a & 1u)) continue;
        // (what the registry gives for the address: an earlier override_function included)
        t->functions.push_back(find_function(a));
        t->addresses.push_back(a);
        const u32 k = ((a & ~1u) - lo) / 2;
        if (wide) t->index32[k] = static_cast<u32>(t->functions.size());
        else t->index16[k] = static_cast<u16>(t->functions.size());
    }
    t->base = lo, t->span = entries * 2;
    d->base = lo;
    d->index = wide ? nullptr : t->index16.data();
    d->functions = t->functions.data();
    // (last: the table is complete before code sees a span; a wide table is psv_call's alone)
    if (!wide) __atomic_store_n(&d->span, entries * 2, __ATOMIC_RELEASE);
    nx::logd(logs::kernel, "{}: indirect-call table of {} functions over {} KiB of code: {} KiB", module.name ? module.name : "?",
             t->functions.size(), t->span / 1024, ((wide ? 4 : 2) * entries + t->functions.size() * sizeof(PsvFunction)) / 1024);
    if (const u32 n = g_table_count.load(std::memory_order_relaxed); n < kMaxTables) {
        g_table_slots[n].store(t.get(), std::memory_order_relaxed);
        g_table_count.store(n + 1, std::memory_order_release);
    }
    tables().push_back(std::move(t));
}

[[noreturn]] void default_handler(PsvContext* ctx, const TrapInfo& trap) {
    nx::loge(logs::kernel, "recompiled code stopped: {}", describe(trap));
    if (ctx)
        nx::loge(logs::kernel, "  r0 {:08x} r1 {:08x} r2 {:08x} r3 {:08x} r4 {:08x} r5 {:08x} r6 {:08x} r7 {:08x}", ctx->r[0],
                 ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7]);
    if (ctx)
        nx::loge(logs::kernel, "  r8 {:08x} r9 {:08x} r10 {:08x} r11 {:08x} r12 {:08x} sp {:08x} lr {:08x}", ctx->r[8], ctx->r[9],
                 ctx->r[10], ctx->r[11], ctx->r[12], ctx->r[13], ctx->r[14]);
    std::fflush(stdout);
    std::abort();
}

// Each registered module's condition-flag contract (guest.h, PsvModuleCode), and what does not fit.
struct FlagContract {
    nx::string name;
    u32 needed_at_entry, given_at_entry, needed_at_return, given_at_return;
};
nx::vector<FlagContract>& flag_contracts() {
    static nx::vector<FlagContract> list;
    return list;
}
nx::string& flag_problems() {
    static nx::string text;
    return text;
}

nx::string flag_letters(u32 f) {
    nx::string s;
    if (f & 8) s += 'n';
    if (f & 4) s += 'z';
    if (f & 2) s += 'c';
    if (f & 1) s += 'v';
    return s;
}

// `reader`'s code reads flags that `provider`'s code must have put in the context.
void check_flags(const FlagContract& reader, const FlagContract& provider) {
    if (const u32 missing = reader.needed_at_entry & ~provider.given_at_entry) {
        const nx::string problem = nx::format(
            "condition flags: {}'s functions read {} at entry, but {}'s calls and jumps to code outside it provide only {} "
            "(generate {} with psvrecomp --flags-given-at-entry {})",
            reader.name, flag_letters(missing), provider.name, provider.given_at_entry ? flag_letters(provider.given_at_entry) : "none",
            provider.name, flag_letters(provider.given_at_entry | reader.needed_at_entry));
        nx::loge(logs::kernel, "{}", problem);
        flag_problems() += problem + "\n";
    }
    if (const u32 missing = reader.needed_at_return & ~provider.given_at_return) {
        const nx::string problem = nx::format(
            "condition flags: {} reads {} after its indirect calls and jumps return, but {}'s functions leave only {} "
            "(generate {} with psvrecomp --flags-given-at-return {})",
            reader.name, flag_letters(missing), provider.name, provider.given_at_return ? flag_letters(provider.given_at_return) : "none",
            provider.name, flag_letters(provider.given_at_return | reader.needed_at_return));
        nx::loge(logs::kernel, "{}", problem);
        flag_problems() += problem + "\n";
    }
}

TrapHandler g_handler = nullptr;

[[noreturn]] void raise(PsvContext* ctx, const TrapInfo& trap) {
    if (g_handler) g_handler(ctx, trap);
    default_handler(ctx, trap);  // (a handler must not return)
}

}  // namespace

void register_module(const PsvModuleCode& module) {
    auto& map = functions();
    for (u32 i = 0; i < module.function_count; ++i) map[module.functions[i].address] = module.functions[i].function;
    if (module.import_stubs)
        for (u32 i = 0; i < module.import_count; ++i) import_stubs()[module.import_stubs[i]] = &module.imports[i];
    if (module.flag_contract != 0 && module.dispatch) fill_table(module);
    // (code generated before the contract has every flag in the context everywhere, and is taken to read them all)
    FlagContract c{module.name && *module.name ? module.name : "(a module without a name)", 15, 15, 15, 15};
    if (module.flag_contract != 0)
        c = {c.name, module.flags_needed_at_entry & 15, module.flags_given_at_entry & 15, module.flags_needed_at_return & 15,
             module.flags_given_at_return & 15};
    auto& list = flag_contracts();
    list.push_back(c);
    for (const auto& other : list) {
        check_flags(c, other);
        if (&other != &list.back()) check_flags(other, c);
    }
}

nx::string flag_contract_problems() { return flag_problems(); }

void clear_modules() {
    g_table_count.store(0, std::memory_order_release);
    for (auto& t : tables()) *t->dispatch = PsvDispatch{};
    tables().clear();
    functions().clear();
    import_stubs().clear();
    flag_contracts().clear();
    flag_problems().clear();
}

const PsvImport* find_import_stub(u32 code) {
    auto& map = import_stubs();
    auto it = map.find(code);
    return it == map.end() ? nullptr : it->second;
}

PsvFunction find_function(u32 code) {
    auto& map = functions();
    auto it = map.find(code);
    return it == map.end() ? nullptr : it->second;
}

PsvFunction override_function(u32 code, PsvFunction f) {
    auto& map = functions();
    auto it = map.find(code);
    const PsvFunction previous = it == map.end() ? nullptr : it->second;
    map[code] = f;
    // (and in the indirect-call tables, where the address is a function start of a registered module's)
    if (code & 1u)
        for (auto& t : tables())
            if (PsvFunction* slot = table_slot(*t, code)) __atomic_store_n(slot, f, __ATOMIC_RELAXED);
    return previous;
}

usize call_table_bytes() {
    usize bytes = 0;
    for (const auto& t : tables())
        bytes += t->index16.size() * sizeof(u16) + t->index32.size() * sizeof(u32) + t->functions.size() * sizeof(PsvFunction);
    return bytes;
}

const char* trap_name(u32 kind) {
    switch (kind) {
        case PSV_TRAP_UNDEFINED: return "undefined instruction";
        case PSV_TRAP_UNPREDICTABLE: return "unpredictable instruction";
        case PSV_TRAP_UDF: return "udf";
        case PSV_TRAP_BKPT: return "bkpt";
        case PSV_TRAP_UNIMPLEMENTED: return "instruction not translated";
        case PSV_TRAP_PRIVILEGED: return "privileged instruction";
        case PSV_TRAP_UNDISCOVERED: return "code discovery did not follow this path";
        case PSV_TRAP_RETURNED: return "a function found never to return returned";
        case PSV_TRAP_BAD_JUMP: return "jump table index outside the table";
        case PSV_TRAP_FPSCR: return "FPSCR setting not implemented";
        case PSV_TRAP_BIG_ENDIAN: return "big-endian data (setend be)";
        case PSV_TRAP_STACK_POINTER:
            return "a call to code outside the module returned with another stack pointer, in the function at this address (the "
                   "module relies on it: generate it with psvrecomp --no-callee-saved)";
        case PSV_TRAP_KEPT_REGISTER: return "a callee did not preserve a register it was found to preserve (psvrecomp --no-callee-saved)";
        case kTrapUnknownTarget: return "branch to an address no recompiled function starts at";
        case kTrapMissingImport: return "call to an import nothing implements";
        case kTrapSvc: return "svc";
        default: return "trap";
    }
}

nx::string describe(const TrapInfo& trap) {
    nx::string s = nx::format("{} at 0x{:08x}", trap_name(trap.kind), trap.pc);
    if (trap.kind == kTrapUnknownTarget) s += nx::format(" (target 0x{:08x})", trap.target);
    if (trap.import)
        s += nx::format(" ({} NID 0x{:08x}, library NID 0x{:08x})", trap.import->library ? trap.import->library : "?",
                        trap.import->nid, trap.import->library_nid);
    return s;
}

TrapHandler set_trap_handler(TrapHandler handler) {
    const TrapHandler previous = g_handler;
    g_handler = handler;
    return previous;
}

}  // namespace psvrecore::recomp

using namespace psvrecore;
using namespace psvrecore::recomp;

extern "C" {

void psv_trap(PsvContext* ctx, uint32_t pc, uint32_t kind) {
    TrapInfo t;
    t.pc = pc;
    t.kind = kind;
    raise(ctx, t);
}

void psv_call(PsvContext* ctx, uint32_t target, uint32_t ret) {
    // (another module's Thumb function: its table, before the registry's map)
    if (target & 1u)
        for (u32 i = 0, n = g_table_count.load(std::memory_order_acquire); i < n; ++i)
            if (PsvFunction* slot = table_slot(*g_table_slots[i].load(std::memory_order_relaxed), target))
                return __atomic_load_n(slot, __ATOMIC_RELAXED)(ctx, ret);
    if (PsvFunction f = find_function(target)) return f(ctx, ret);
    if (const PsvImport* import = find_import_stub(target)) return psv_call_import(ctx, import, ret);
    TrapInfo t;
    t.pc = ret;
    t.kind = kTrapUnknownTarget;
    t.target = target;
    raise(ctx, t);
}

void psv_jump(PsvContext* ctx, uint32_t target, uint32_t ret, uint32_t from) {
    if (PsvFunction f = find_function(target)) return f(ctx, ret);
    if (const PsvImport* import = find_import_stub(target)) return psv_call_import(ctx, import, ret);
    TrapInfo t;
    t.pc = from;
    t.kind = kTrapUnknownTarget;
    t.target = target;
    raise(ctx, t);
}

void psv_svc(PsvContext* ctx, uint32_t pc, uint32_t imm) {
    (void)imm;
    TrapInfo t;
    t.pc = pc;
    t.kind = kTrapSvc;
    raise(ctx, t);
}

void psv_yield(PsvContext*) { nx::this_thread::yield(); }

void psv_import_missing(PsvContext* ctx, const PsvImport* import, uint32_t ret) {
    TrapInfo t;
    t.pc = ret;
    t.kind = kTrapMissingImport;
    t.import = import;
    raise(ctx, t);
}

}  // extern "C"
