#pragma once

// The code generator's per-function state, shared by the files that translate each group of instructions.

#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/flags.hpp"
#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/analysis/registers.hpp"
#include "psvrecore/analysis/vfp.hpp"
#include "psvrecore/arm/instruction.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::recomp::detail {

using arm::Instruction;
using arm::Op;
using arm::Operand;
using arm::OperandKind;

inline nx::string hex(u32 v) { return nx::format("0x{:08X}u", v); }
inline nx::string dec(u32 v) { return nx::format("{}", v); }
inline nx::string label_name(u32 address) { return nx::format("L_{:08X}", address & ~1u); }

using analysis::FlagSet;
using analysis::RegSet;
using analysis::VfpSet;

// How the generated code holds the core registers.
enum class RegisterMode : u8 {
    Context,  // in the context, as ctx->r[N] (psvrecomp --registers-in-context)
    Probe,    // locals, without the analysis: a first translation that records what each instruction reads and writes
    Locals,   // locals gN, in the context only where the analysis says (analysis/registers.hpp)
};

// How the generated code holds the floating-point registers and the FPSCR.
enum class VfpMode : u8 {
    Context,  // in the context, as ctx->vfp.s[N] / ctx->vfp.d[N] and ctx->fpscr (psvrecomp --vfp-in-context)
    Probe,    // locals, without the analysis: the first translation records what each instruction reads and writes
    Locals,   // typed locals sN (float), dN (double), fpscr and fpnzcv, in the context only where the analysis says
              // (analysis/vfp.hpp)
};

// The C condition of an ARM condition code ("" for always), on the function's flags (the local `fl`).
nx::string condition(arm::Cond c);

class Emitter {
public:
    Emitter(const analysis::Program& program, const analysis::Function& fn, const analysis::FlagLiveness& flags,
            const nx::string& prefix, Stats& stats, RegisterMode mode = RegisterMode::Context,
            const analysis::FunctionRegisters* registers = nullptr, VfpMode vfp_mode = VfpMode::Context,
            const analysis::FunctionVfp* vfp = nullptr);

    // The function's C definition.
    nx::string function();
    // (RegisterMode::Probe) What the translation of each instruction read and wrote, by instruction index.
    const nx::vector<analysis::RegisterEffect>& effects() const { return effects_; }
    // (VfpMode::Probe) What the translation of each instruction read and wrote of the floating-point registers.
    const nx::vector<analysis::VfpEffect>& vfp_effects() const { return vfp_effects_; }
    // The module's register analysis (RegisterMode::Locals): the callees' internal entries (analysis::EntryKind).
    void set_register_analysis(const analysis::RegisterAnalysis* all) { all_ = all; }
    // An internal entry inlined into its callers (generate): its definition static inline, function() giving it alone and
    // wrapper() its f_<address>.
    void set_inline(bool on) { inline_ = on; }
    const nx::string& wrapper() const { return wrapper_; }
    // The functions whose internal entries the function calls (code addresses).
    const nx::flat_hash_set<u32>& internal_callees() const { return internal_callees_; }
    // Indirect calls through the module's table (guest.h, psv_call_in; off: psv_call, psvrecomp --no-call-table).
    void set_call_table(bool on, bool inline_lookup) { call_table_ = on, call_table_inline_ = inline_lookup; }
    // Host implementations (Options::replacements): the C function each replaced function (code address, bit 0 clear)
    // is called through at its calls and tail calls; the name of the function's own definition (fo_<address> for a
    // guarded replacement's guest code; default function_name).
    void set_replacements(const nx::flat_hash_map<u32, nx::string>* callees) { replaced_ = callees; }
    void set_name(nx::string name) { name_ = std::move(name); }
    // The C function a call of `target_code` goes to when it is a replaced function, else nullptr.
    const nx::string* replacement(u32 target_code) const {
        if (!replaced_) return nullptr;
        const auto it = replaced_->find(target_code & ~1u);
        return it == replaced_->end() ? nullptr : &it->second;
    }
    // The statement of an indirect call of `target` returning to `next`.
    nx::string indirect_call(nx::string_view target, nx::string_view next) const {
        if (!call_table_) return nx::format("psv_call(ctx, {}, {});", target, next);
        if (call_table_inline_) return nx::format("psv_call_in(&{}_dispatch, ctx, {}, {});", prefix_, target, next);
        return nx::format("{}_call(ctx, {}, {});", prefix_, target, next);
    }
    // Whether the function is translated as an internal entry (fi_<address>, with f_<address> a wrapper around it).
    bool internal_body() const { return regs_ && all_ && all_->internal_calls && regs_->entry != analysis::EntryKind::Public; }

    // --- used by the instruction translators ---

    // Appends a statement line (inside the current instruction's block).
    void line(nx::string_view text) {
        body_ += "        ";
        body_ += text;
        body_ += '\n';
    }
    // A fresh temporary name.
    nx::string temp(const char* stem = "t") { return nx::format("{}{}", stem, temp_counter_++); }
    // Ends the instruction with a trap (and counts it under `what`).
    void trap(const Instruction& in, const char* kind, nx::string_view what);
    void unimplemented(const Instruction& in);

    // The value of a core register as an operand of `in` (the PC reads as the instruction's address plus 8 or 4).
    nx::string read(const Instruction& in, u32 r);
    // A core register other than the PC, read (its local, or the context's field).
    nx::string reg(u32 r);
    // The statement that writes a core register other than the PC (for the translation to place).
    nx::string assignment(u32 r, nx::string_view value);
    // Writes a core register; a write to the PC is a branch. `kind`: how the PC write treats bit 0 (see branch_to_value).
    enum class PcWrite { Interworking, Branch };
    void write(const Instruction& in, u32 r, nx::string_view value, PcWrite kind = PcWrite::Interworking);
    // A branch to a computed code address (the instruction's jump table if it has one). `target` is an expression,
    // evaluated once; a table branch evaluates it only for an index outside its table (it may read guest memory).
    void branch_to_value(const Instruction& in, nx::string_view target);
    // A table branch: a switch on the index register (see analysis::JumpTable).
    void table_branch(const Instruction& in, const analysis::JumpTable& table, nx::string_view target);
    // A branch to a known code address: a goto, or a tail call. `after`: the continuation after the instruction's
    // translation (running into the next function), not an exit inside it.
    void branch_to(const Instruction& in, u32 target_code, bool after = false);
    // A call: LR = the return address, then the function, the import or the runtime's lookup.
    void call(const Instruction& in, u32 target_code);

    // Flexible second operand: its value, and its carry out when `carry` is given (else unused).
    nx::string operand2(const Instruction& in, const Operand& o, nx::string* carry);
    // A memory operand's address. `writeback` receives the statement updating the base register (empty if none).
    nx::string address(const Instruction& in, const Operand& m, nx::string* writeback);

    // Where the module's code builds an imported variable's address (movw/movt patched by the loader): the C
    // expression of that address, and the patch type; empty elsewhere.
    nx::string variable_address(u32 site, u8& type) const;

    // Literal pools: a load from a read-only segment of the module can be done at generation time.
    bool constant_load(u32 address, u32 size, u32& value) const;

    // --- condition flags (analysis/flags.hpp): kept in the local `fl`, computed only where they are live ---

    // The flags live after the current instruction's own effect: of those it writes, the ones to compute.
    FlagSet live() const { return flags_fn_->after[index_]; }
    // fl.n and fl.z from a 32-bit result, those of `which` that are live.
    void set_nz(nx::string_view r, FlagSet which);
    // const uint32_t r = x + y + carry_in (AddWithCarry), setting the flags of `which` that are live.
    void add_with_carry(nx::string_view r, nx::string_view x, nx::string_view y, nx::string_view carry_in, FlagSet which);
    // Writes flags to the context (before something outside the function may read them), and reads them back.
    void store_flags(FlagSet f);
    void load_flags(FlagSet f);
    // Around the current instruction's call (to a function, an import, through the runtime, svc): the flags the
    // callee may read, then those read back after it (or a trap after a function that never returns).
    void call_flags_before();
    void call_flags_after(const Instruction& in);
    // Before a trap: every local flag in the context (what the function computed; see analysis::FunctionFlags), and
    // the registers whose locals differ from it (with those the instruction has written so far).
    void store_for_trap();
    void store_flags_for_trap() { store_flags(analysis::kAllFlags); }
    // The register stores alone, for a trap inside a helper: they must be on the trapping path only (the analysis
    // takes the context to hold what it held before them on every path that goes on).
    nx::string trap_register_stores();

    // --- core registers (analysis/registers.hpp): locals, in the context where code outside the function may read them ---

    // Before the current instruction's call, and after it.
    void call_registers_before();
    void call_registers_after(const Instruction& in);
    // Before an exit inside the instruction's translation (a return, a jump through the runtime, a tail call).
    // (and the floating-point registers)
    void exit_registers();
    void store_registers(RegSet r);
    void load_registers(RegSet r);

    // --- floating-point registers (analysis/vfp.hpp): typed locals, in the context where code outside may read them ---

    bool vfp_locals() const { return vfp_mode_ != VfpMode::Context; }
    // S register (or word of the register file) k as a float, D register n as a double, and their bits.
    nx::string vs(u32 k);
    nx::string vd(u32 n);
    nx::string vs_bits(u32 k) { return nx::format("psv_b32({})", vs(k)); }
    nx::string vd_bits(u32 n) { return nx::format("psv_b64({})", vd(n)); }
    // Writes them (a statement line).
    void set_vs(u32 k, nx::string_view value);
    void set_vd(u32 n, nx::string_view value);
    void set_vs_bits(u32 k, nx::string_view value) { set_vs(k, nx::format("psv_f32({})", value)); }
    void set_vd_bits(u32 n, nx::string_view value) { set_vd(n, nx::format("psv_f64({})", value)); }
    // Advanced SIMD on values (emit_neon.cpp): D register n as a PsvD, Q register p as a PsvQ, whatever local holds
    // them (an S pair, a double, a Q vector), and their writes (a statement line; `value` evaluated once).
    nx::string vdv(u32 n);
    nx::string vq(u32 p);
    void set_vdv(u32 n, nx::string_view value);
    void set_vq(u32 p, nx::string_view value);
    // The FPSCR's rest as a pointer for a saturating helper's QC ("&fpscr": read and written).
    nx::string fpscr_qc();
    // Advanced SIMD instructions translated on values (emit_neon); off: all of them in the context form
    // (emit_simd.cpp's guest_simd.h helpers with the locals synced around each: psvrecomp --neon-in-context).
    void set_neon_locals(bool on) { neon_locals_ = on; }
    // D registers (bit n) whose 32-bit lanes are known here to hold no denormal (results of Advanced SIMD
    // floating-point operations, whose results never are, and copies of them; known within a straight stretch of code:
    // forgotten at labels, calls and every other write); and the current instruction's results that are such.
    bool known_flushed(u32 dmask) const { return dmask && (flushed_ & dmask) == dmask; }
    void produces_flushed(u32 dmask) { flushed_out_ |= dmask; }
    bool neon_locals() const { return neon_locals_ && vfp_mode_ != VfpMode::Context; }
    // The FPSCR's locals: `fpscr` (bits 27-0) and `fpnzcv` (bits 31-28).
    nx::string fpscr();
    nx::string fpnzcv();
    void set_fpnzcv(nx::string_view value);
    void set_fpscr(nx::string_view value);  // the whole register (vmsr): both
    // A vcmp whose NZCV the next instruction (vmrs APSR_nzcv) moves to the flags: the compare's width (32, 64), else
    // 0. The operands are kept in fc<w>a, fc<w>b; that vmrs and the conditions that follow it compare them.
    u32 fused_compare() const { return fuse_.empty() || (fuse_[index_] & 0x80u) ? 0u : fuse_[index_]; }
    // The vmrs after such a compare: its width, else 0.
    u32 fused_vmrs() const { return fuse_.empty() || !(fuse_[index_] & 0x80u) ? 0u : fuse_[index_] & 0x7Fu; }
    FlagSet fused_flags() const { return fused_flags_[index_]; }
    void use_fused(u32 width) { (width == 64 ? uses_fc64_ : uses_fc32_) = true; }

    u32 next_code(const Instruction& in) const {
        return (in.address + in.size) | (in.mode == arm::Mode::Thumb ? 1u : 0u);
    }
    const analysis::Program& program() const { return program_; }
    const analysis::Function& fn() const { return fn_; }
    Stats& stats() { return stats_; }

private:
    void instruction(const Instruction& in);
    void plan_fused_compares();
    nx::string vfp_line(VfpSet units, bool store) const;
    nx::string dv_of(u32 n) const;
    void dv_assign(u32 n, nx::string_view value);
    void store_vfp(VfpSet units, usize& counter);
    void call_vfp_before();
    void call_vfp_after(const Instruction& in);
    nx::string vfp_trap_stores();
    bool falls_through(const Instruction& in) const;
    void count_flags(const Instruction& in);
    nx::string flags_line(FlagSet f, bool store) const;

    nx::string register_line(RegSet r, bool store) const;
    // An internal entry's return statement (its results by value).
    nx::string internal_return() const;
    // The context stores before a trap inside the current instruction (and it counts as one that may trap): the
    // flags and the registers, on the trapping path only.
    nx::string trap_stores();
    bool noreturn_call(const Instruction& in) const;

    const analysis::Program& program_;
    const analysis::Function& fn_;
    const analysis::FlagLiveness& flags_;
    const analysis::FunctionFlags* flags_fn_;
    analysis::Cfg cfg_;
    usize index_ = 0;  // the current instruction's, in fn_.code
    const nx::string& prefix_;
    Stats& stats_;
    nx::string body_;
    u32 temp_counter_ = 0;
    nx::flat_hash_set<u32> labels_;
    bool ended_ = false;  // the current instruction's translation ends the flow (trap)
    bool sp_trap_ = false;  // a call's stack pointer check goes to the function's trap for it
    RegisterMode mode_;
    const analysis::FunctionRegisters* regs_;  // RegisterMode::Locals
    const analysis::RegisterAnalysis* all_ = nullptr;
    bool call_table_ = false, call_table_inline_ = false;
    bool inline_ = false;
    nx::string wrapper_;
    const nx::flat_hash_map<u32, nx::string>* replaced_ = nullptr;
    nx::string name_;
    nx::flat_hash_set<u32> internal_callees_;
    // The current instruction's register accesses so far, and (Probe) every instruction's.
    analysis::RegisterEffect access_;
    nx::vector<analysis::RegisterEffect> effects_;
    VfpMode vfp_mode_;
    const analysis::FunctionVfp* vfp_;  // VfpMode::Locals
    analysis::VfpEffect vaccess_;
    nx::vector<analysis::VfpEffect> vfp_effects_;
    // Fused compares, by instruction index: a vcmp's width (32, 64), with bit 7 on the vmrs after it; the flags that
    // vmrs computes (those live after the conditions that compare the operands instead); the conditions that do.
    nx::vector<u8> fuse_;
    nx::vector<FlagSet> fused_flags_;
    nx::vector<u8> fused_cond_;
    bool uses_fc32_ = false, uses_fc64_ = false;
    bool neon_locals_ = true;
    u32 flushed_ = 0, flushed_out_ = 0;
};

// The translators, by instruction group; each returns false when the instruction is not in its group.
bool emit_core(Emitter& e, const Instruction& in);
bool emit_memory(Emitter& e, const Instruction& in);
bool emit_vfp(Emitter& e, const Instruction& in);
// (the floating-point registers in typed locals: emit_vfp_locals.cpp; emit_vfp.cpp has the context form)
bool emit_vfp_locals(Emitter& e, const Instruction& in);
bool emit_simd(Emitter& e, const Instruction& in);
// (Advanced SIMD on the registers' values in locals; false for an instruction it has no such translation for)
bool emit_neon(Emitter& e, const Instruction& in);
// AdvSIMDExpandImm (op, cmode, abcdefgh): the 64-bit pattern of an Advanced SIMD modified immediate.
u64 simd_expand_imm(u32 op, u32 cmode, u32 imm8);

}  // namespace psvrecore::recomp::detail
