#pragma once

// The USSE reference interpreter: runs decoded programs on one shader instance. It is the oracle of the shader
// translator's tests and is never linked into a game. Its semantics are checked by running compiled Cg shaders and
// comparing with the same expressions evaluated on the host (scripts/usse_exec_check.py).

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"

namespace psvrecore::usse {

// Guest memory as the interpreter's loads and stores see it.
class ShaderMemory {
public:
    virtual ~ShaderMemory() = default;
    virtual bool read(u32 address, void* out, u32 size) = 0;
    virtual bool write(u32 address, const void* in, u32 size) = 0;
};

// The register state of one instance. Banks are arrays of 32-bit registers, numbered as the decoder numbers them.
struct Machine {
    static constexpr u32 kBankSize = 256;
    u32 temp[kBankSize] = {};
    u32 primary[kBankSize] = {};
    u32 secondary[kBankSize] = {};
    u32 output[kBankSize] = {};
    u32 internal[4][4] = {};
    i32 index[2] = {};
    bool pred[4] = {};
    u32 link = 0;
    bool killed = false;
    ShaderMemory* memory = nullptr;
};

struct RunLimits {
    u64 max_steps = 1'000'000;
    // Called after each executed instruction (for debugging).
    void (*trace)(void* user, const Decoded& d, const Machine& m) = nullptr;
    void* trace_user = nullptr;
};

// Runs a program from its first word until it ends (runs past its last word). Fails on an instruction the
// interpreter does not implement, an access outside a bank or memory, or when the step limit is reached.
Result<void> run(std::span<const Decoded> program, ProgramKind kind, Machine& m, const RunLimits& limits = {});

// Fills the secondary attributes as the driver does before a draw: the default uniform buffer (32-bit words), the
// uniform buffers' addresses (by BUFFER index; index kDefaultUniformContainer: the default buffer's copy in memory)
// and the literals.
Result<void> load_secondary(const Gxp& gxp, std::span<const u32> default_uniforms,
                            std::span<const u32> buffer_addresses, Machine& m);

// IEEE half-precision conversions (round to nearest even).
u16 to_half(f32 v);
f32 from_half(u16 h);

}  // namespace psvrecore::usse
