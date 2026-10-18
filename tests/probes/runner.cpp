// The probe runner: loads a probe module and libc (SELF files built with the SDK, recompiled by psvrecomp and
// linked in here) into guest memory, resolves their imports from each other's exports, runs the probe's
// probe_main as recompiled code, runs the same probe compiled for the host, and compares the two outputs.
//
// It is the smallest loader that can run recompiled code: no system libraries (an import nothing exports traps),
// no module_start, one thread. The runtime's loader replaces it.
//
// Built by tests/probes/CMakeLists.txt, which passes the module files and addresses as definitions.

#include <cstdio>
#include <cstring>

#include "psvrecore/analysis/image.hpp"
#include "psvrecore/recomp/support.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

extern "C" {
extern const PsvModuleCode PSVRECORE_PROBE_MODULE;
extern const PsvModuleCode libc_module;
uint32_t host_probe_main(uint32_t* out);
}

namespace {

constexpr u64 kGuestSpace = 1ull << 32, kGuard = 64 * 1024;
constexpr u32 kStackTop = 0x7F000000u, kStackSize = 1u << 20;
constexpr u32 kOutput = 0x7E000000u, kOutputWords = 1u << 18;
constexpr u32 kReturn = 0xFEED0001u;  // the return address probe_main is given

nx::vector<u32> parse_bases(const char* text) {
    nx::vector<u32> out;
    if (!*text) return out;  // (empty: where the module was linked)
    for (const char* p = text;;) {
        char* end;
        out.push_back(static_cast<u32>(std::strtoul(p, &end, 0)));
        if (*end != ',') break;
        p = end + 1;
    }
    return out;
}

bool commit(u8* base, u32 address, u32 size) {
    const u64 page = nx::vmem::get_page_size();
    const u64 begin = address & ~(page - 1), end = (u64{address} + size + page - 1) & ~(page - 1);
    return nx::vmem::commit(base + begin, static_cast<usize>(end - begin));
}

struct Loaded {
    analysis::Image image;
    const PsvModuleCode* code;
};

}  // namespace

int main() {
    void* reservation = nullptr;
    const usize reserved = static_cast<usize>(kGuestSpace + kGuard);
    if (!nx::vmem::reserve(&reservation, reserved)) {
        std::fprintf(stderr, "probe: cannot reserve the guest address space\n");
        return 2;
    }
    u8* const membase = static_cast<u8*>(reservation);
    psv_membase = membase;

    // The modules, at the addresses they were generated for.
    nx::vector<Loaded> modules;
    const struct {
        const char* path;
        const char* bases;
        const PsvModuleCode* code;
    } files[] = {{PSVRECORE_PROBE_SELF, PSVRECORE_PROBE_BASES, &PSVRECORE_PROBE_MODULE},
                 {PSVRECORE_LIBC_SELF, PSVRECORE_LIBC_BASES, &libc_module}};
    for (const auto& f : files) {
        const auto bases = parse_bases(f.bases);
        auto image = analysis::Image::load(nx::fs::path_view(f.path), bases);
        if (!image) {
            std::fprintf(stderr, "probe: %s\n", image.error().message.c_str());
            return 2;
        }
        for (const auto& s : image->segments()) {
            if (!commit(membase, s.base, s.size())) {
                std::fprintf(stderr, "probe: cannot commit 0x%08x\n", s.base);
                return 2;
            }
            std::memcpy(membase + s.base, s.bytes.data(), s.size());
        }
        recomp::register_module(*f.code);
        modules.push_back(Loaded{std::move(*image), f.code});
    }

    // Imports: each module's imported functions from the others' exports, by library and function NID.
    nx::flat_hash_map<u64, u32> exports;
    for (const auto& m : modules)
        for (const auto& lib : m.image.module().exports)
            for (const auto& fn : lib.functions) exports[(u64{lib.nid} << 32) | fn.nid] = fn.address;
    u32 resolved = 0, unresolved = 0;
    for (const auto& m : modules)
        for (u32 i = 0; i < m.code->import_count; ++i) {
            PsvImport& imp = m.code->imports[i];
            auto it = exports.find((u64{imp.library_nid} << 32) | imp.nid);
            imp.function = it == exports.end() ? nullptr : recomp::find_function(it->second);
            ++(imp.function ? resolved : unresolved);
        }

    // Imported variables: the exporters' addresses (variables nothing exports point at a zeroed page), in the
    // generated table for code, and written into data that refers to them.
    nx::flat_hash_map<u64, u32> exported_variables;
    for (const auto& m : modules)
        for (const auto& lib : m.image.module().exports)
            for (const auto& v : lib.variables) exported_variables[(u64{lib.nid} << 32) | v.nid] = v.address;
    constexpr u32 kUnresolvedVariables = 0x7D000000u;
    if (!commit(membase, kUnresolvedVariables, 0x10000)) {
        std::fprintf(stderr, "probe: cannot commit the variable page\n");
        return 2;
    }
    u32 variables_resolved = 0, variables_unresolved = 0;
    for (const auto& m : modules) {
        for (u32 i = 0; i < m.code->variable_count; ++i) {
            PsvVariable& var = m.code->variables[i];
            auto it = exported_variables.find((u64{var.library_nid} << 32) | var.nid);
            var.address = it == exported_variables.end() ? kUnresolvedVariables : it->second;
            ++(it == exported_variables.end() ? variables_unresolved : variables_resolved);
        }
        const auto uses = m.image.variable_uses();
        if (!uses) {
            std::fprintf(stderr, "probe: %s\n", uses.error().message.c_str());
            return 2;
        }
        for (const auto& u : *uses)
            if (u.type == 2) {
                const u32 value = m.code->variables[u.variable].address + u.addend;
                std::memcpy(membase + u.at, &value, 4);
            }
    }

    // A stack, the output buffer, and the call.
    if (!commit(membase, kStackTop - kStackSize, kStackSize) || !commit(membase, kOutput, kOutputWords * 4)) {
        std::fprintf(stderr, "probe: cannot commit the stack\n");
        return 2;
    }
    PsvContext ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.r[0] = kOutput;
    ctx.r[13] = kStackTop - 16;
    ctx.r[14] = kReturn;
    const PsvFunction entry = recomp::find_function(PSVRECORE_PROBE_ENTRY);
    if (!entry) {
        std::fprintf(stderr, "probe: no recompiled function at 0x%08x\n", static_cast<u32>(PSVRECORE_PROBE_ENTRY));
        return 2;
    }
    entry(&ctx, kReturn);
    const u32 guest_count = ctx.r[0];

    nx::vector<u32> host(kOutputWords, 0);
    const u32 host_count = host_probe_main(host.data());

    const u32* guest = reinterpret_cast<const u32*>(membase + kOutput);
    u32 differ = 0;
    const u32 common = guest_count < host_count ? guest_count : host_count;
    for (u32 i = 0; i < common; ++i) {
        if (guest[i] == host[i]) continue;
        if (++differ <= 20) std::printf("  word %u: recompiled 0x%08x, host 0x%08x\n", i, guest[i], host[i]);
    }
    std::printf("probe %s: %u words (host %u), %u differ; imports %u resolved, %u not; variables %u resolved, %u not\n",
                PSVRECORE_PROBE_NAME, guest_count, host_count, differ, resolved, unresolved, variables_resolved, variables_unresolved);
    nx::vmem::release(reservation, reserved);
    return differ == 0 && guest_count == host_count ? 0 : 1;
}
