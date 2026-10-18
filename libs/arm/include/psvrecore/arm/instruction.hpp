#pragma once

// ARMv7-A instructions as the decoder describes them (ARM and Thumb, VFPv3, Advanced SIMD): an operation, a
// condition, flags and up to six operands. The same operation decodes to the same description whichever instruction
// set and encoding it comes from, so the recompiler implements each operation once. Only the formatter (format.hpp)
// cares about the encoding, to print what the reference disassembler prints.
//
// Operands are in the order the assembler writes them. Two-operand data-processing forms (Thumb's ands r0, r1;
// add sp, #imm) keep two operands: the first is both a source and the destination.

#include "core/foundation/core/foundation.h"

namespace psvrecore::arm {

enum class Mode : u8 { Arm, Thumb };

inline constexpr u8 kSp = 13, kLr = 14, kPc = 15;

enum class Cond : u8 { Eq, Ne, Cs, Cc, Mi, Pl, Vs, Vc, Hi, Ls, Ge, Lt, Gt, Le, Al, None };

enum class ShiftType : u8 { Lsl, Lsr, Asr, Ror, Rrx };

// Every operation, with the base mnemonic the formatter prints.
#define PSVRECORE_ARM_OPS(X)                                                                                          \
    X(Undefined, "<unknown>")                                                                                        \
    /* data processing */                                                                                            \
    X(And, "and") X(Eor, "eor") X(Sub, "sub") X(Rsb, "rsb") X(Add, "add") X(Adc, "adc") X(Sbc, "sbc") X(Rsc, "rsc")   \
    X(Tst, "tst") X(Teq, "teq") X(Cmp, "cmp") X(Cmn, "cmn") X(Orr, "orr") X(Mov, "mov") X(Bic, "bic") X(Mvn, "mvn")   \
    X(Orn, "orn") X(Adr, "adr") X(Lsl, "lsl") X(Lsr, "lsr") X(Asr, "asr") X(Ror, "ror") X(Rrx, "rrx") X(Movw, "movw") \
    X(Movt, "movt")                                                                                                  \
    /* multiply and divide */                                                                                        \
    X(Mul, "mul") X(Mla, "mla") X(Mls, "mls") X(Umull, "umull") X(Umlal, "umlal") X(Smull, "smull")                  \
    X(Smlal, "smlal") X(Umaal, "umaal") X(Smlabb, "smlabb") X(Smlabt, "smlabt") X(Smlatb, "smlatb")                  \
    X(Smlatt, "smlatt") X(Smlawb, "smlawb") X(Smlawt, "smlawt") X(Smulbb, "smulbb") X(Smulbt, "smulbt")              \
    X(Smultb, "smultb") X(Smultt, "smultt") X(Smulwb, "smulwb") X(Smulwt, "smulwt") X(Smlalbb, "smlalbb")            \
    X(Smlalbt, "smlalbt") X(Smlaltb, "smlaltb") X(Smlaltt, "smlaltt") X(Smuad, "smuad") X(Smuadx, "smuadx")          \
    X(Smusd, "smusd") X(Smusdx, "smusdx") X(Smlad, "smlad") X(Smladx, "smladx") X(Smlsd, "smlsd")                    \
    X(Smlsdx, "smlsdx") X(Smlald, "smlald") X(Smlaldx, "smlaldx") X(Smlsld, "smlsld") X(Smlsldx, "smlsldx")          \
    X(Smmul, "smmul") X(Smmulr, "smmulr") X(Smmla, "smmla") X(Smmlar, "smmlar") X(Smmls, "smmls")                    \
    X(Smmlsr, "smmlsr") X(Sdiv, "sdiv") X(Udiv, "udiv")                                                              \
    /* saturation, parallel arithmetic */                                                                            \
    X(Qadd, "qadd") X(Qsub, "qsub") X(Qdadd, "qdadd") X(Qdsub, "qdsub") X(Ssat, "ssat") X(Ssat16, "ssat16")          \
    X(Usat, "usat") X(Usat16, "usat16") X(Sadd16, "sadd16") X(Sasx, "sasx") X(Ssax, "ssax") X(Ssub16, "ssub16")      \
    X(Sadd8, "sadd8") X(Ssub8, "ssub8") X(Qadd16, "qadd16") X(Qasx, "qasx") X(Qsax, "qsax") X(Qsub16, "qsub16")      \
    X(Qadd8, "qadd8") X(Qsub8, "qsub8") X(Shadd16, "shadd16") X(Shasx, "shasx") X(Shsax, "shsax")                    \
    X(Shsub16, "shsub16") X(Shadd8, "shadd8") X(Shsub8, "shsub8") X(Uadd16, "uadd16") X(Uasx, "uasx")                \
    X(Usax, "usax") X(Usub16, "usub16") X(Uadd8, "uadd8") X(Usub8, "usub8") X(Uqadd16, "uqadd16")                    \
    X(Uqasx, "uqasx") X(Uqsax, "uqsax") X(Uqsub16, "uqsub16") X(Uqadd8, "uqadd8") X(Uqsub8, "uqsub8")                \
    X(Uhadd16, "uhadd16") X(Uhasx, "uhasx") X(Uhsax, "uhsax") X(Uhsub16, "uhsub16") X(Uhadd8, "uhadd8")              \
    X(Uhsub8, "uhsub8")                                                                                              \
    /* extension, bit fields, miscellaneous */                                                                       \
    X(Sxtab16, "sxtab16") X(Sxtb16, "sxtb16") X(Sxtab, "sxtab") X(Sxtb, "sxtb") X(Sxtah, "sxtah") X(Sxth, "sxth")     \
    X(Uxtab16, "uxtab16") X(Uxtb16, "uxtb16") X(Uxtab, "uxtab") X(Uxtb, "uxtb") X(Uxtah, "uxtah") X(Uxth, "uxth")     \
    X(Clz, "clz") X(Rbit, "rbit") X(Rev, "rev") X(Rev16, "rev16") X(Revsh, "revsh") X(Sel, "sel") X(Usad8, "usad8")  \
    X(Usada8, "usada8") X(Pkhbt, "pkhbt") X(Pkhtb, "pkhtb") X(Bfc, "bfc") X(Bfi, "bfi") X(Sbfx, "sbfx")              \
    X(Ubfx, "ubfx")                                                                                                  \
    /* branches */                                                                                                   \
    X(B, "b") X(Bl, "bl") X(Blx, "blx") X(Bx, "bx") X(Bxj, "bxj") X(Cbz, "cbz") X(Cbnz, "cbnz") X(Tbb, "tbb")         \
    X(Tbh, "tbh")                                                                                                    \
    /* loads and stores */                                                                                           \
    X(Ldr, "ldr") X(Ldrb, "ldrb") X(Ldrh, "ldrh") X(Ldrsb, "ldrsb") X(Ldrsh, "ldrsh") X(Ldrd, "ldrd") X(Str, "str")   \
    X(Strb, "strb") X(Strh, "strh") X(Strd, "strd") X(Ldrt, "ldrt") X(Ldrbt, "ldrbt") X(Ldrht, "ldrht")              \
    X(Ldrsbt, "ldrsbt") X(Ldrsht, "ldrsht") X(Strt, "strt") X(Strbt, "strbt") X(Strht, "strht") X(Ldrex, "ldrex")    \
    X(Ldrexb, "ldrexb") X(Ldrexh, "ldrexh") X(Ldrexd, "ldrexd") X(Strex, "strex") X(Strexb, "strexb")                \
    X(Strexh, "strexh") X(Strexd, "strexd") X(Clrex, "clrex") X(Ldm, "ldm") X(Ldmib, "ldmib") X(Ldmda, "ldmda")      \
    X(Ldmdb, "ldmdb") X(Stm, "stm") X(Stmib, "stmib") X(Stmda, "stmda") X(Stmdb, "stmdb") X(Push, "push")            \
    X(Pop, "pop") X(Pld, "pld") X(Pldw, "pldw") X(Pli, "pli") X(Swp, "swp") X(Swpb, "swpb") X(Srsda, "srsda")        \
    X(Srsdb, "srsdb") X(Srsia, "srsia") X(Srsib, "srsib") X(Rfeda, "rfeda") X(Rfedb, "rfedb") X(Rfeia, "rfeia")      \
    X(Rfeib, "rfeib")                                                                                                \
    /* system */                                                                                                     \
    X(Mrs, "mrs") X(Msr, "msr") X(Cps, "cps") X(Cpsie, "cpsie") X(Cpsid, "cpsid") X(Setend, "setend") X(Svc, "svc")  \
    X(Bkpt, "bkpt") X(Smc, "smc") X(Nop, "nop") X(Yield, "yield") X(Wfe, "wfe") X(Wfi, "wfi") X(Sev, "sev")          \
    X(Hint, "hint") X(Dbg, "dbg") X(Dmb, "dmb") X(Dsb, "dsb") X(Isb, "isb") X(It, "it") X(Udf, "udf")                \
    X(Subs, "subs") X(Eret, "eret")                                                                                  \
    /* coprocessor */                                                                                                \
    X(Cdp, "cdp") X(Cdp2, "cdp2") X(Mcr, "mcr") X(Mcr2, "mcr2") X(Mrc, "mrc") X(Mrc2, "mrc2") X(Mcrr, "mcrr")        \
    X(Mcrr2, "mcrr2") X(Mrrc, "mrrc") X(Mrrc2, "mrrc2") X(Ldc, "ldc") X(Ldcl, "ldcl") X(Ldc2, "ldc2")                \
    X(Ldc2l, "ldc2l") X(Stc, "stc") X(Stcl, "stcl") X(Stc2, "stc2") X(Stc2l, "stc2l")                                \
    /* floating point */                                                                                             \
    X(Vadd, "vadd") X(Vsub, "vsub") X(Vmul, "vmul") X(Vnmul, "vnmul") X(Vdiv, "vdiv") X(Vmla, "vmla")                \
    X(Vmls, "vmls") X(Vnmla, "vnmla") X(Vnmls, "vnmls") X(Vfma, "vfma") X(Vfms, "vfms") X(Vfnma, "vfnma")            \
    X(Vfnms, "vfnms") X(Vabs, "vabs") X(Vneg, "vneg") X(Vsqrt, "vsqrt") X(Vcmp, "vcmp") X(Vcmpe, "vcmpe")            \
    X(Vcvt, "vcvt") X(Vcvtr, "vcvtr") X(Vcvtb, "vcvtb") X(Vcvtt, "vcvtt") X(Vmov, "vmov") X(Vmrs, "vmrs")            \
    X(Vmsr, "vmsr") X(Vldr, "vldr") X(Vstr, "vstr") X(Vldmia, "vldmia") X(Vldmdb, "vldmdb") X(Vstmia, "vstmia")      \
    X(Vstmdb, "vstmdb") X(Vpush, "vpush") X(Vpop, "vpop")                                                            \
    /* Advanced SIMD */                                                                                              \
    X(Vaba, "vaba") X(Vabal, "vabal") X(Vabd, "vabd") X(Vabdl, "vabdl") X(Vacge, "vacge") X(Vacgt, "vacgt")          \
    X(Vaddhn, "vaddhn") X(Vaddl, "vaddl") X(Vaddw, "vaddw") X(Vand, "vand") X(Vbic, "vbic") X(Vbif, "vbif")          \
    X(Vbit, "vbit") X(Vbsl, "vbsl") X(Vceq, "vceq") X(Vcge, "vcge") X(Vcgt, "vcgt") X(Vcle, "vcle") X(Vclt, "vclt")  \
    X(Vcls, "vcls") X(Vclz, "vclz") X(Vcnt, "vcnt") X(Vdup, "vdup") X(Veor, "veor") X(Vext, "vext")                  \
    X(Vhadd, "vhadd") X(Vhsub, "vhsub") X(Vld1, "vld1") X(Vld2, "vld2") X(Vld3, "vld3") X(Vld4, "vld4")              \
    X(Vmax, "vmax") X(Vmin, "vmin") X(Vmlal, "vmlal") X(Vmlsl, "vmlsl") X(Vmovl, "vmovl") X(Vmovn, "vmovn")          \
    X(Vmull, "vmull") X(Vmvn, "vmvn") X(Vorn, "vorn") X(Vorr, "vorr") X(Vpadal, "vpadal") X(Vpadd, "vpadd")          \
    X(Vpaddl, "vpaddl") X(Vpmax, "vpmax") X(Vpmin, "vpmin") X(Vqabs, "vqabs") X(Vqadd, "vqadd")                      \
    X(Vqdmlal, "vqdmlal") X(Vqdmlsl, "vqdmlsl") X(Vqdmulh, "vqdmulh") X(Vqdmull, "vqdmull") X(Vqmovn, "vqmovn")      \
    X(Vqmovun, "vqmovun") X(Vqneg, "vqneg") X(Vqrdmulh, "vqrdmulh") X(Vqrshl, "vqrshl") X(Vqrshrn, "vqrshrn")        \
    X(Vqrshrun, "vqrshrun") X(Vqshl, "vqshl") X(Vqshlu, "vqshlu") X(Vqshrn, "vqshrn") X(Vqshrun, "vqshrun")          \
    X(Vqsub, "vqsub") X(Vraddhn, "vraddhn") X(Vrecpe, "vrecpe") X(Vrecps, "vrecps") X(Vrev16, "vrev16")              \
    X(Vrev32, "vrev32") X(Vrev64, "vrev64") X(Vrhadd, "vrhadd") X(Vrshl, "vrshl") X(Vrshr, "vrshr")                  \
    X(Vrshrn, "vrshrn") X(Vrsqrte, "vrsqrte") X(Vrsqrts, "vrsqrts") X(Vrsra, "vrsra") X(Vrsubhn, "vrsubhn")          \
    X(Vshl, "vshl") X(Vshll, "vshll") X(Vshr, "vshr") X(Vshrn, "vshrn") X(Vsli, "vsli") X(Vsra, "vsra")              \
    X(Vsri, "vsri") X(Vst1, "vst1") X(Vst2, "vst2") X(Vst3, "vst3") X(Vst4, "vst4") X(Vsubhn, "vsubhn")              \
    X(Vsubl, "vsubl") X(Vsubw, "vsubw") X(Vswp, "vswp") X(Vtbl, "vtbl") X(Vtbx, "vtbx") X(Vtrn, "vtrn")              \
    X(Vtst, "vtst") X(Vuzp, "vuzp") X(Vzip, "vzip")

enum class Op : u16 {
#define PSVRECORE_ARM_OP_ENUM(name, text) name,
    PSVRECORE_ARM_OPS(PSVRECORE_ARM_OP_ENUM)
#undef PSVRECORE_ARM_OP_ENUM
        Count
};

const char* mnemonic(Op op);

// Element types of floating-point and Advanced SIMD instructions (.f32, .s16, .i8, .64, ...).
enum class Dt : u8 {
    None,
    S8, S16, S32, S64, U8, U16, U32, U64, I8, I16, I32, I64, F16, F32, F64, P8, P16, B8, B16, B32, B64,
};
const char* dt_name(Dt dt);  // without the dot

enum class OperandKind : u8 {
    None,
    Reg,          // a core register: reg
    RegShiftImm,  // reg, <shift> #amount (RRX: no amount)
    RegShiftReg,  // reg, <shift> reg2
    Imm,          // #imm (printed signed when kSigned)
    ModImm,       // ARM modified immediate: imm is the value; amount the rotation, index the 8-bit value (printing)
    Label,        // an absolute address (branch target, literal): imm
    Mem,          // [reg ...] — see the kMem* flags; count: Advanced SIMD alignment in bytes (0: none)
    RegList,      // {r0, ...}: imm is the register mask
    SReg, DReg, QReg,  // floating-point/SIMD register: reg
    SRegList, DRegList,  // consecutive registers: reg (first), count; DRegList: index = spacing (1 or 2)
    DScalar,      // reg[index]
    DLaneList,    // {d0[1], d1[1]} or {d0[], d1[]} (kAllLanes): reg, count, spacing (amount), index
    CoprocNum,    // p<imm>
    CoprocReg,    // c<reg>
    CoprocOption, // {imm}
    Sysreg,       // a named special register or field: imm is the formatter's code (see format.cpp)
    Barrier,      // dmb/dsb/isb option: imm
    Text,         // a fixed word: imm is the formatter's code (endianness, cps flags, it mask, ...)
    FloatImm,     // a floating-point constant: imm holds its single-precision bits (exact for VFP immediates)
    SimdImm,      // an Advanced SIMD modified immediate: index = abcdefgh, amount = cmode, count = op (expanded on use)
};

// Operand flags.
inline constexpr u8 kSigned = 1;           // Imm: print as a signed value
inline constexpr u8 kMemPre = 1;           // Mem: offset applied before the access ([r0, #4] / [r0, #4]!)
inline constexpr u8 kMemWriteBack = 2;     // Mem: base written back (! or post-index)
inline constexpr u8 kMemSubtract = 4;      // Mem: offset subtracted
inline constexpr u8 kMemIndexReg = 8;      // Mem: offset is a register (reg2, shifted by shift/amount)
inline constexpr u8 kMemPostReg = 16;      // Mem (Advanced SIMD): post-index by register reg2
inline constexpr u8 kMemImm = 32;          // Mem: an immediate offset is printed even when it is 0
inline constexpr u8 kAllLanes = 1;         // DLaneList: {dN[]}
inline constexpr u8 kListWriteBack = 1;    // RegList/DRegList preceding register written back (!)

struct Operand {
    OperandKind kind = OperandKind::None;
    u8 reg = 0;
    u8 reg2 = 0;
    ShiftType shift = ShiftType::Lsl;
    u8 amount = 0;
    u8 flags = 0;
    u8 count = 0;
    u8 index = 0;
    u32 imm = 0;
};

// Instruction flags.
inline constexpr u16 kSetsFlags = 1;        // the S suffix
inline constexpr u16 kWide = 2;             // a 32-bit Thumb encoding the formatter marks .w
inline constexpr u16 kUnpredictable = 4;    // decoded, but the architecture calls this encoding UNPREDICTABLE
inline constexpr u16 kInIt = 8;             // inside an IT block (Thumb)
inline constexpr u16 kWriteBack = 16;       // ldm/stm/vldm base written back (printed "rn!")
inline constexpr u16 kUserRegs = 32;        // ldm/stm with ^
inline constexpr u16 kNarrow = 64;          // a 16-bit Thumb encoding the formatter marks .n (where llvm does)
inline constexpr u16 kImm12 = 128;          // Thumb add/sub with a plain 12-bit immediate (printed addw/subw)
inline constexpr u16 kXForm = 256;          // vldm/vstm of doublewords with an odd word count (fldmx/fstmx)
inline constexpr u16 kImmCarry = 512;       // the modified immediate sets the carry flag (to its bit 31) in flag-setting
                                            // logical operations; without it they leave the carry unchanged

struct Instruction {
    u32 address = 0;
    u32 raw = 0;  // the encoding; for 32-bit Thumb, the first halfword in bits 31-16
    Op op = Op::Undefined;
    Cond cond = Cond::Al;
    Mode mode = Mode::Arm;
    u8 size = 4;
    u16 flags = 0;
    Dt dt = Dt::None, dt2 = Dt::None;  // conversions have two (vcvt.f32.s32)
    u8 operand_count = 0;
    Operand operands[6];

    bool valid() const { return op != Op::Undefined; }
    Operand& add(const Operand& o) { return operands[operand_count++] = o; }
};

// Operand helpers for the decoders.
inline Operand reg(u8 r) { return Operand{OperandKind::Reg, r}; }
inline Operand imm(u32 v, u8 flags = 0) {
    Operand o{OperandKind::Imm};
    o.imm = v;
    o.flags = flags;
    return o;
}
inline Operand label(u32 address) {
    Operand o{OperandKind::Label};
    o.imm = address;
    return o;
}

}  // namespace psvrecore::arm
