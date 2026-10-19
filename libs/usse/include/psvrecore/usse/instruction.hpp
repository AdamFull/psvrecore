#pragma once

// USSE (the Vita GPU's shader instruction set) instructions as the decoder describes them. A program is a list of
// 64-bit words; one word decodes to one instruction, to two (a main and a co-issued operation), or, with a repeat
// count, to one instruction per repetition. Some words are not operations (the program prologue, synchronisation
// markers, the word that sets repeat increments).
//
// Nothing about the encoding is documented: the decoder follows the reference disassembler (psp2shaderperf), checked
// against it word for word (scripts/usse_check.py). Register numbers are in 32-bit units throughout: r2 is the third
// 32-bit temporary, whatever the instruction's data type.

#include "core/foundation/core/foundation.h"

namespace psvrecore::usse {

enum class ProgramKind : u8 { Primary, Secondary };

enum class Bank : u8 {
    None,
    Temp,       // r: temporaries
    Output,     // o
    Primary,    // pa: primary attributes (a secondary program names them sa)
    Secondary,  // sa: secondary attributes (uniforms, constant buffers)
    Internal,   // i0, i1, ...: the 128-bit internal registers
    Index,      // index registers as a destination (number: as printed)
    Special,    // g0-g15 and the named special registers (number: the encoded 64-127)
    Constant,   // the hardware constant table (number: the index; see constants())
    Literal,    // an immediate value (number)
    Predicate,  // p<number>
};

// A lane's source: a component of the register, or one of the constants the swizzles can select.
enum class Comp : u8 { X, Y, Z, W, Zero, One, Two, Half };

// A part of a 32-bit value (integer multiply-adds read 16-bit halves or bytes).
enum class Part : u8 { None, Lo16, Hi16, Lo8, Hi8 };

// How a constant-table operand is read: f32 words, f16 halves, or integer words.
enum class View : u8 { F32, F16, Int };

struct Operand {
    Bank bank = Bank::None;
    i32 number = 0;
    u8 indexed = 0;         // 1 or 2: the register is bank[index<indexed> + number]
    bool index_scaled = false;  // the index counts pairs of registers (printed "index1 * 2 + n")
    u8 lanes = 0;           // the operand's lanes (0: not a lane operand, printed without a suffix)
    u8 mask = 0;            // the lanes used (bit i: lane i)
    Comp swizzle[4] = {Comp::X, Comp::Y, Comp::Z, Comp::W};  // the source of each lane
    View view = View::F32;  // (Constant operands)
    bool negate = false;
    bool absolute = false;
    Part part = Part::None;

    bool valid() const { return bank != Bank::None; }
};

// Data types (the mnemonic's suffixes).
enum class Type : u8 { None, F32, F16, Fx10, Fx8, I8, I16, I32, U8, S8, O8, U16, S16, U32 };

// Guards: the predicate (or its negation) an instruction is executed under. Pn is the "predicate per instance".
enum class Pred : u8 { None, P0, P1, P2, P3, NotP0, NotP1, NotP2, Pn };

#define PSVRECORE_USSE_OPS(X)                                                                                         \
    X(Invalid, "<invalid>")                                                                                          \
    /* floating point */                                                                                             \
    X(Mad, "mad") X(Add, "add") X(Mul, "mul") X(Min, "min") X(Max, "max") X(Frc, "frc") X(Dsx, "dsx")                \
    X(Dsy, "dsy") X(Dot, "dot") X(Rcp, "rcp") X(Rsq, "rsq") X(Log, "log") X(Exp, "exp") X(SubFlr, "subflr")          \
    X(Cmp, "cmp") X(Sub, "sub") X(Sablnd, "sablnd") /* (tested operations only) */                                  \
    /* moves */                                                                                                      \
    X(Mov, "mov") X(Cmov, "cmov") X(Cmov8, "cmov8") X(Pack, "pack")                                                  \
    /* tests (an operation whose result sets a predicate or a register) */                                           \
    X(Test, "test")                                                                                                  \
    /* bitwise */                                                                                                    \
    X(And, "and") X(Or, "or") X(Xor, "xor") X(Shl, "shl") X(Rol, "rol") X(Shr, "shr") X(Asr, "asr") X(Rlp, "rlp")    \
    /* integer and fixed point */                                                                                    \
    X(IMad, "mad") X(Sop, "sop") X(Sop2, "sop2") X(Blend, "blend") X(FxMad, "mad")                                   \
    /* texture and memory */                                                                                         \
    X(Tex, "tex") X(Load, "ld") X(Store, "st")                                                                       \
    /* flow control and special */                                                                                   \
    X(Br, "br") X(Ba, "ba") X(Ret, "ret") X(MovToLink, "mov") X(MovFromLink, "mov") X(Nop, "nop") X(Kill, "kill")    \
    X(DepthF, "depthf") X(PCoeff, "pcoeff") X(MovImm, "mov")                                                         \
    /* words that are not operations */                                                                              \
    X(SetIncrements, "<increments>") X(Hidden, "<hidden>")

enum class Op : u8 {
#define PSVRECORE_USSE_OP_ENUM(name, text) name,
    PSVRECORE_USSE_OPS(PSVRECORE_USSE_OP_ENUM)
#undef PSVRECORE_USSE_OP_ENUM
        Count
};

const char* mnemonic(Op op);

// Conditions of tests and conditional moves: a comparison of the operation's result with zero.
enum class Cond : u8 { Always, EqZero, NeZero, PosZero, LeZero, LtZero, NegZero, GeZero, GtZero };

// How a vector test combines its lanes into one predicate.
enum class TestMode : u8 { Scalar, AndAll, OrAll, PerLane, Or2 };

// Sum-of-products factors (fixed-point blending): the factor a term's source is multiplied by.
enum class Factor : u8 { Zero, Sat, A, AAlpha, S0, S0Alpha, B, BAlpha, Invalid };
// Texture sampling.
enum class TexDim : u8 { D1, D2, Cube };
enum class TexKind : u8 { Plain, Bias, Replace, Grad };
// Memory.
enum class Space : u8 { Absolute, Local, Thread };

// Instruction flags.
inline constexpr u32 kSat = 1;           // saturate
inline constexpr u32 kScale = 2;         // pack/mov .scl
inline constexpr u32 kCoissued = 4;      // the co-issued half of a dual-issue word ("+" in the listing)
inline constexpr u32 kPairSource = 8;    // f32 pack/mov: lanes come from two 64-bit halves (src[0], src[1])
inline constexpr u32 kTexRsd = 16;       // texture: .rsd, .info, .minp
inline constexpr u32 kTexInfo = 32;
inline constexpr u32 kTexMinP = 64;
inline constexpr u32 kPreIncrement = 128;  // load/store address update: ++x / x++ (kDecrement: --)
inline constexpr u32 kPostIncrement = 256;
inline constexpr u32 kDecrement = 512;
inline constexpr u32 kInvertB = 1024;    // bitwise: the second source is inverted (~)
inline constexpr u32 kSignedShift = 2048;  // family 26 multiply-add: .s1 (else .s0)
inline constexpr u32 kScaleVariant = 4096;  // family 26 multiply-add (printed with .s0/.s1)
inline constexpr u32 kSameHalves = 8192;    // f32 pack/mov whose two source halves are the same register

struct SopTerm {
    Factor factor = Factor::Zero;
    bool complement = false;  // (1 - factor)
};

struct Instruction {
    u64 raw = 0;
    u8 family = 0;  // the top five bits
    Op op = Op::Invalid;
    Op alu = Op::Invalid;     // Test: the operation tested
    Type type = Type::None;
    Type src_type = Type::None;  // conversions (pack.f16.f32, rcp.f16.f32)
    Pred pred = Pred::None;
    Cond cond = Cond::Always;
    TestMode test_mode = TestMode::Scalar;
    u32 flags = 0;
    u8 src_count = 0;
    Operand dest;
    Operand src[4];
    // Sum of products: colour (and alpha) terms of sources a and b, and the operation joining them.
    Op sop_op = Op::Add, sop_alpha_op = Op::Add;
    SopTerm term[2], alpha_term[2];
    bool complement_a = false;  // blend: the first source is complemented (1 - a)
    // Texture sampling.
    TexDim tex_dim = TexDim::D2;
    TexKind tex_kind = TexKind::Plain;
    // Memory.
    Space space = Space::Absolute;
    u8 size = 32;           // bits per element
    u8 fetch = 0;           // elements per access (0: one, no .fetch suffix)
    // Flow control: the target word (Br: an index into the program; Ba: an absolute address).
    i32 target = 0;
    // SetIncrements: per-operand repeat increments (dest, src0, src1, src2) and their swizzle-mode flags.
    i8 increment[4] = {1, 1, 1, 1};
    u8 increment_swizzle = 0;

    bool valid() const { return op != Op::Invalid; }
    bool prints() const { return op != Op::SetIncrements && op != Op::Hidden; }
};

// The 130 words of the hardware constant table.
const u32* constants();

}  // namespace psvrecore::usse
