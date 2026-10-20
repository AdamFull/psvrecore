#include "psvrecore/recomp/support.hpp"

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

TrapHandler g_handler = nullptr;

[[noreturn]] void raise(PsvContext* ctx, const TrapInfo& trap) {
    if (g_handler) g_handler(ctx, trap);
    default_handler(ctx, trap);  // (a handler must not return)
}

}  // namespace

void register_module(const PsvModuleCode& module) {
    auto& map = functions();
    for (u32 i = 0; i < module.function_count; ++i) map[module.functions[i].address] = module.functions[i].function;
}

void clear_modules() { functions().clear(); }

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
    return previous;
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
    if (PsvFunction f = find_function(target)) return f(ctx, ret);
    TrapInfo t;
    t.pc = ret;
    t.kind = kTrapUnknownTarget;
    t.target = target;
    raise(ctx, t);
}

void psv_jump(PsvContext* ctx, uint32_t target, uint32_t ret, uint32_t from) {
    if (PsvFunction f = find_function(target)) return f(ctx, ret);
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
