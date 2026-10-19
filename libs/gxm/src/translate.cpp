// The USSE to SPIR-V translator. It follows the reference interpreter (libs/usse/src/interp.cpp) operation for
// operation: the same lanes, conversions and orders of evaluation, so that the interpreter's checked semantics carry
// over. Where the two differ it says so here:
// - texture instructions of the draw forms sample the unit the program's sampler slots name, with the GPU's
//   filtering (the interpreter samples through ShaderTextures);
// - an instruction under a predicate computes its results and writes them with a select (no branches);
// - an indexed register outside its bank is clamped to the bank (the interpreter fails);
// - transcendental functions and division are the GPU's, within Vulkan's precision rather than correctly rounded;
// - a load outside the memory buffer, or a sample of a missing texture, gives zeros (the interpreter fails).

#include "psvrecore/gxm/translate.hpp"

#include <bit>

#include "core/foundation/strings/format.h"
#include "psvrecore/gxm/spirv.hpp"
#include "spirv/unified1/GLSL.std.450.h"
#include "spirv/unified1/spirv.h"

namespace psvrecore::gxm {

namespace {

using spirv::Id;
using usse::Bank;
using usse::Comp;
using usse::Cond;
using usse::Decoded;
using usse::Factor;
using usse::Instruction;
using usse::Op;
using usse::Operand;
using usse::Part;
using usse::Pred;
using usse::SopTerm;
using usse::TestMode;
using usse::TexDim;
using usse::Type;

constexpr u32 kBankSize = 256;
constexpr u32 kMaxSteps = 1'000'000;  // (blocks run, not instructions)

u32 width_of(Type t) {
    switch (t) {
        case Type::F16:
        case Type::I16:
        case Type::U16:
        case Type::S16: return 16;
        case Type::I8:
        case Type::U8:
        case Type::S8:
        case Type::O8:
        case Type::Fx8: return 8;
        case Type::Fx10: return 10;
        default: return 32;
    }
}

bool is_float(Type t) { return t == Type::F32 || t == Type::F16; }

u32 lowest_lane(u8 mask) { return mask ? static_cast<u32>(std::countr_zero(static_cast<u32>(mask))) : 0; }

// A register lane: a pointer to its word and its bit range (see interp.cpp).
struct Slot {
    Id ptr = 0;
    u8 shift = 0;
    u8 bits = 32;
    u8 value_bits = 32;
    bool clear = false;
};

struct Pending {
    Slot slot;
    Id value;
    Id cond;  // the instruction's predicate (0: none)
};

// A bank of registers: a function-scope array, sized by the registers the program names (all of the bank when it
// indexes it).
struct BankVar {
    Id var = 0;
    u32 used = 0;
    u32 minimum = 0;
    bool dynamic = false;
};

enum class Form { Compute, Secondary, Vertex, Fragment };

class Translator {
public:
    Translator(Form form, const usse::Gxp& gxp, std::span<const Decoded> program) : form_(form), gxp_(gxp), program_(program) {}

    Result<nx::vector<u32>> translate();
    const VertexShader* vertex_shader = nullptr;
    const FragmentShader* fragment_shader = nullptr;
    // The fragment form: a secondary program to run first in each instance (one that reads per-tile registers), on
    // a private copy of the secondary attributes.
    std::span<const Decoded> inline_secondary;

private:
    // ---- Helpers -------------------------------------------------------------------------------------------------

    Id u(u32 v) { return b_.c_uint(v); }
    Id s(i32 v) { return b_.c_int(v); }
    Id f(f32 v) { return b_.c_float(v); }
    Id tl() {
        int64_ = true;
        return b_.t_int64();
    }
    Id l(i64 v) {
        int64_ = true;
        return b_.c_int64(v);
    }

    Id bin(u32 opcode, Id type, Id a, Id c) { return b_.op(opcode, type, {a, c}); }
    Id un(u32 opcode, Id type, Id a) { return b_.op(opcode, type, {a}); }
    Id iadd(Id a, Id c) { return bin(SpvOpIAdd, tu_, a, c); }
    Id isub(Id a, Id c) { return bin(SpvOpISub, tu_, a, c); }
    Id imul(Id a, Id c) { return bin(SpvOpIMul, tu_, a, c); }
    Id band(Id a, Id c) { return bin(SpvOpBitwiseAnd, tu_, a, c); }
    Id bor(Id a, Id c) { return bin(SpvOpBitwiseOr, tu_, a, c); }
    Id bxor(Id a, Id c) { return bin(SpvOpBitwiseXor, tu_, a, c); }
    Id shl(Id a, Id c) { return bin(SpvOpShiftLeftLogical, tu_, a, c); }
    Id shr(Id a, Id c) { return bin(SpvOpShiftRightLogical, tu_, a, c); }
    Id ieq(Id a, Id c) { return bin(SpvOpIEqual, tb_, a, c); }
    Id ine(Id a, Id c) { return bin(SpvOpINotEqual, tb_, a, c); }
    Id cast(Id type, Id v) { return un(SpvOpBitcast, type, v); }
    Id sel(Id type, Id c, Id a, Id d) { return b_.op(SpvOpSelect, type, {c, a, d}); }
    Id land(Id a, Id c) { return bin(SpvOpLogicalAnd, tb_, a, c); }
    Id lor(Id a, Id c) { return bin(SpvOpLogicalOr, tb_, a, c); }
    Id lnot(Id a) { return un(SpvOpLogicalNot, tb_, a); }
    Id bnot(Id a) { return un(SpvOpNot, tu_, a); }
    // Floating point, each operation rounded on its own.
    Id exact(Id id) {
        b_.decorate(id, SpvDecorationNoContraction);
        return id;
    }
    Id fadd(Id a, Id c) { return exact(bin(SpvOpFAdd, tf_, a, c)); }
    Id fsub(Id a, Id c) { return exact(bin(SpvOpFSub, tf_, a, c)); }
    Id fmul(Id a, Id c) { return exact(bin(SpvOpFMul, tf_, a, c)); }
    Id fdiv(Id a, Id c) { return exact(bin(SpvOpFDiv, tf_, a, c)); }
    Id ext(Id type, u32 instruction, std::initializer_list<Id> operands) {
        return b_.ext(type, instruction, operands);
    }
    Id load(Id type, Id ptr) { return b_.op(SpvOpLoad, type, {ptr}); }
    void store(Id ptr, Id v) { b_.op0(SpvOpStore, {ptr, v}); }
    Id chain(Id ptr_type, Id base, std::initializer_list<Id> index) {
        nx::vector<u32> words{base};
        words.insert(words.end(), index.begin(), index.end());
        return b_.op(SpvOpAccessChain, ptr_type, words);
    }
    Id buffer_word(Id buffer, Id index) { return chain(tp_su_, buffer, {u(0), index}); }

    // Signed values of the low `bits` of v (as i32).
    Id sext(Id v, u32 bits) {
        if (bits == 32) return cast(ti_, v);
        return b_.op(SpvOpBitFieldSExtract, ti_, {cast(ti_, v), u(0), u(bits)});
    }
    Id mask(Id v, u32 bits) { return bits == 32 ? v : band(v, u((1u << bits) - 1)); }
    // i32 or u32 (as a 32-bit value) to i64.
    Id to64s(Id v32) { return un(SpvOpSConvert, tl(), v32); }
    Id to64u(Id v32) {
        int64_ = true;
        return cast(tl(), un(SpvOpUConvert, b_.t_uint64(), v32));
    }
    Id to32(Id v64) { return un(SpvOpUConvert, tu_, cast(b_.t_uint64(), v64)); }

    // IEEE half conversions: from_half exactly; to_half as the interpreter's (round to nearest even), on the bits.
    Id from_half(Id h) {
        const Id pair = ext(b_.t_vector(tf_, 2), GLSLstd450UnpackHalf2x16, {h});
        return b_.op(SpvOpCompositeExtract, tf_, {pair, 0});
    }
    Id to_half(Id x);

    bool fail(nx::string message) {
        if (error_.empty()) error_ = nx::format("word {}: {}", word_, message);
        return false;
    }

    // ---- Registers -----------------------------------------------------------------------------------------------

    Id reg_index(const Operand& o, i64 word, u32 size, BankVar* bank);
    Id word_ptr(const Operand& o, i64 word, bool write);
    Slot slot(const Operand& o, u32 c, u32 bits, bool write);
    Id raw(const Operand& o, u32 lane, Type t);
    Id fval(const Operand& o, u32 lane, Type t);
    void put(const Operand& d, u32 lane, Type t, Id value);
    void putf(const Operand& d, u32 lane, Type t, Id v) { put(d, lane, t, t == Type::F16 ? to_half(cast(tu_, v)) : cast(tu_, v)); }
    void commit();
    // A store now (index registers, predicates), under the instruction's predicate.
    void store_now(Id type, Id ptr, Id v) {
        if (cond_) v = sel(type, cond_, v, load(type, ptr));
        store(ptr, v);
    }

    Id cond_float(Cond c, Id v);
    Id cond_int(Cond c, Id v, Id type, Id zero);
    Id predicate(Pred p);

    // ---- Instructions --------------------------------------------------------------------------------------------

    Id alu(Op op, Id a, Id b, Id c);
    void float_op(const Instruction& in);
    Id convert(Id v, Type from, Type to, bool scale);
    void pack_op(const Instruction& in);
    void move_op(const Instruction& in);
    Id fx_value(Id raw, Type t) { return t == Type::Fx10 ? sext(raw, 10) : cast(ti_, band(raw, u(0xFF))); }
    Id fx_raw(Id v, Type t) {
        if (t == Type::Fx10) return band(cast(tu_, ext(ti_, GLSLstd450SClamp, {v, s(-512), s(511)})), u(0x3FF));
        return band(cast(tu_, ext(ti_, GLSLstd450SClamp, {v, s(0), s(255)})), u(0xFF));
    }
    Id fx_mul(Id a, Id c);
    void sop_op(const Instruction& in);
    void fxmad_op(const Instruction& in);
    Id bitwise(Op op, Id a, Id c, u32 bits);
    void bitwise_op(const Instruction& in);
    void test_op(const Instruction& in);
    Id part_of(Id v, Part p, bool sgn);
    void imad_op(const Instruction& in);
    void sample(Id unit, TexDim dim, const Id coord[4], Id rgba[4]);
    // The draw forms: a combined image sampler's sample (explicit level of detail where `lod` is given).
    Id sampler_variable(u32 binding, bool cube);
    void sample_unit(u32 binding, TexDim dim, const Id coord[4], usse::TexKind kind, const Id extra[4], Id rgba[4]);
    void gather_unit(u32 binding, const Id coord[4], Id regs[6]);
    Id texture(u32 binding);
    void write_sample(const Id rgba[4], u32 format, Id regs[4]);
    void tex_op(const Instruction& in);
    Id memory_read(Id address, u32 size);
    void memory_write(Id address, Id value, u32 size);
    void memory_op(const Instruction& in);
    // Translates one instruction; a branch's target block and condition are left to the caller.
    void emit(usize i);
    // The program's body (straight, or a loop over its blocks); the result is true when the step limit stopped it.
    Id body(auto&& local);
    void compute_prologue(Id gid);
    void compute_epilogue(Id gid, Id stopped);
    void secondary_prologue();
    void secondary_epilogue();
    void vertex_prologue();
    void vertex_epilogue();
    void fragment_prologue();
    void fragment_epilogue();
    Id push_constant(u32 member, Id type);
    Id special(i32 code);
    Id frag_coord();

    Form form_;
    const usse::Gxp& gxp_;
    std::span<const Decoded> program_;
    bool branches_ = false;
    nx::vector<Id> interface_;  // (the entry point's inputs and outputs)
    Id push_ = 0, tp_push_f_ = 0, tp_push_u_ = 0;
    Id sa_base_ = 0;  // (the draw forms: the first word of this program's secondary attributes)
    Id textures_[kDrawBindings] = {};
    Id tv4_ = 0;
    spirv::Builder b_;
    nx::string error_;
    u32 word_ = 0;
    bool int64_ = false;
    bool image_query_ = false;  // (texture size queries: the ImageQuery capability)
    bool stores_ = false;       // (the program stores to memory: its buffers are writable)
    bool in_secondary_ = false;  // (emitting the inline secondary program: its primary bank is sa)
    Id frag_coord_ = 0;          // (the fragment form's FragCoord input)

    Id tu_ = 0, ti_ = 0, tf_ = 0, tb_ = 0;
    Id tp_fu_ = 0, tp_fi_ = 0, tp_fb_ = 0, tp_su_ = 0;
    Id in_buf_ = 0, sa_buf_ = 0, out_buf_ = 0, tex_buf_ = 0, mem_buf_ = 0;
    BankVar temp_, out_, pa_, sa_;  // (sa_: the secondary form's secondary attributes)
    Id internal_ = 0, index_ = 0, pred_ = 0, killed_ = 0, link_ = 0;
    nx::vector<Pending> writes_;
    Id cond_ = 0;
};

Id Translator::to_half(Id x) {
    const Id sign = band(shr(x, u(16)), u(0x8000));
    const Id exp = band(shr(x, u(23)), u(0xFF));
    const Id mant = band(x, u(0x7FFFFF));
    const Id nan = bor(bor(sign, u(0x7C00)), sel(tu_, ine(mant, u(0)), bor(u(0x200), shr(mant, u(13))), u(0)));
    const Id e = bin(SpvOpISub, ti_, cast(ti_, exp), s(127 - 15));
    // Normal results shift the mantissa by 13; subnormal ones (e <= 0) shift it with its leading one by 14 - e,
    // which at 25 or more leaves zero (the interpreter's "e < -10").
    const Id small = bin(SpvOpSLessThanEqual, tb_, e, s(0));
    const Id m = sel(tu_, small, bor(mant, u(0x800000)), mant);
    const Id shift = sel(tu_, small, cast(tu_, ext(ti_, GLSLstd450SMin, {bin(SpvOpISub, ti_, s(14), e), s(25)})), u(13));
    const Id base = sel(tu_, small, u(0), shl(cast(tu_, e), u(10)));
    Id h = bor(base, shr(m, shift));
    const Id rem = band(m, isub(shl(u(1), shift), u(1)));
    const Id half = shl(u(1), isub(shift, u(1)));
    const Id up = lor(bin(SpvOpUGreaterThan, tb_, rem, half), land(ieq(rem, half), ine(band(h, u(1)), u(0))));
    h = iadd(h, sel(tu_, up, u(1), u(0)));
    Id r = bor(sign, h);
    r = sel(tu_, bin(SpvOpSGreaterThanEqual, tb_, e, s(31)), bor(sign, u(0x7C00)), r);
    return sel(tu_, ieq(exp, u(0xFF)), nan, r);
}

// ---- Registers -------------------------------------------------------------------------------------------------

Id Translator::reg_index(const Operand& o, i64 word, u32 size, BankVar* bank) {
    if (!o.indexed) {
        if (word < 0 || word >= size) {
            fail(nx::format("register {} of bank {} outside the bank", word, static_cast<u32>(o.bank)));
            return u(0);
        }
        if (bank) bank->used = nx::max(bank->used, static_cast<u32>(word) + 1);
        return u(static_cast<u32>(word));
    }
    Id ix = load(ti_, chain(tp_fi_, index_, {u(o.indexed - 1u)}));
    if (o.index_scaled) ix = bin(SpvOpIMul, ti_, ix, s(2));
    ix = bin(SpvOpIAdd, ti_, ix, s(static_cast<i32>(word)));
    ix = ext(ti_, GLSLstd450SClamp, {ix, s(0), s(static_cast<i32>(size) - 1)});
    if (bank) bank->dynamic = true;
    return cast(tu_, ix);
}

Id Translator::word_ptr(const Operand& o, i64 word, bool write) {
    BankVar* bank = nullptr;
    switch (o.bank) {
        case Bank::Temp: bank = &temp_; break;
        case Bank::Output: bank = &out_; break;
        case Bank::Primary: bank = form_ == Form::Secondary || in_secondary_ ? &sa_ : &pa_; break;
        case Bank::Secondary:
            if (form_ == Form::Secondary || !inline_secondary.empty()) {
                bank = &sa_;
                break;
            }
            if (write) return fail("a write to the secondary attributes"), u(0);
            if (sa_base_) return buffer_word(sa_buf_, iadd(sa_base_, reg_index(o, word, ComputeLayout::kSecondary, nullptr)));
            return buffer_word(sa_buf_, reg_index(o, word, ComputeLayout::kSecondary, nullptr));
        default: break;
    }
    if (!bank) {
        fail(nx::format("register {} of bank {} outside the bank", word, static_cast<u32>(o.bank)));
        return 0;
    }
    return chain(tp_fu_, bank->var, {reg_index(o, word, kBankSize, bank)});
}

Slot Translator::slot(const Operand& o, u32 c, u32 bits, bool write) {
    if (o.bank == Bank::Internal) {
        if (o.number < 0 || o.number > 3) return fail("internal register outside 0-3"), Slot{};
        const u32 base = static_cast<u32>(o.number) * 4;
        if (bits == 16)
            return Slot{chain(tp_fu_, internal_, {u(base + ((c >> 1) & 3))}), static_cast<u8>((c & 1) * 16), 16, 16,
                        (c & 1) == 0};
        return Slot{chain(tp_fu_, internal_, {u(base + (c & 3))}), 0, 32, static_cast<u8>(bits)};
    }
    const i64 n = o.number;
    if (bits == 10) {
        const Id w = word_ptr(o, n + (c == 3 ? 1 : 0), write);
        return w ? Slot{w, static_cast<u8>(c < 3 ? 10 * c : 0), 10} : Slot{};
    }
    if (bits == 32) {
        const Id w = word_ptr(o, n + c, write);
        return w ? Slot{w, 0, 32} : Slot{};
    }
    if (bits == 16) {
        const i64 h = 2 * n + c;
        const Id w = word_ptr(o, h >> 1, write);
        return w ? Slot{w, static_cast<u8>((h & 1) * 16), 16} : Slot{};
    }
    const i64 by = 4 * n + c;
    const Id w = word_ptr(o, by >> 2, write);
    return w ? Slot{w, static_cast<u8>((by & 3) * 8), 8} : Slot{};
}

Id Translator::raw(const Operand& o, u32 lane, Type t) {
    const Comp c = o.lanes ? o.swizzle[lane & 3] : Comp::X;
    const u32 bits = width_of(t);
    if (c >= Comp::Zero) {
        // 0, 1, 2 and 0.5 as f32, f16 and integers
        static constexpr u32 kF32[4] = {0, 0x3F800000u, 0x40000000u, 0x3F000000u};
        static constexpr u32 kF16[4] = {0, 0x3C00u, 0x4000u, 0x3800u};
        static constexpr u32 kInt[4] = {0, 1, 2, 0};
        const u32 k = static_cast<u32>(c) - static_cast<u32>(Comp::Zero);
        return u(t == Type::F32 ? kF32[k] : t == Type::F16 ? kF16[k] : kInt[k]);
    }
    const u32 comp = static_cast<u32>(c);
    switch (o.bank) {
        case Bank::Literal: return u(static_cast<u32>(o.number));
        case Bank::Constant: {
            const u32* table = usse::constants();
            const u32 base = static_cast<u32>(o.number) * 2;
            if (bits == 32) return u(base + comp < 130 ? table[base + comp] : 0);
            const u32 word = base + comp / 2;
            return u(word < 130 ? (table[word] >> ((comp & 1) * 16)) & 0xFFFF : 0);
        }
        case Bank::Special: {
            // (the word, then the lane's bits of it)
            const Id w = special(o.number);
            if (bits == 32) return comp == 0 ? w : u(0);
            if (bits == 16) return comp < 2 ? mask(comp ? shr(w, u(16)) : w, 16) : u(0);
            return comp < 4 ? mask(comp ? shr(w, u(8 * comp)) : w, 8) : u(0);
        }
        case Bank::Internal:
            if (t == Type::F16) {
                const Slot sl = slot(o, comp, 32, false);
                return sl.ptr ? to_half(load(tu_, sl.ptr)) : u(0);
            }
            [[fallthrough]];
        default: {
            const Slot sl = slot(o, comp, bits, false);
            if (!sl.ptr) return u(0);
            const Id w = load(tu_, sl.ptr);
            if (sl.bits == 32) return sl.value_bits == 32 ? w : mask(w, sl.value_bits);
            return mask(sl.shift ? shr(w, u(sl.shift)) : w, sl.bits);
        }
    }
}

Id Translator::fval(const Operand& o, u32 lane, Type t) {
    const Id r = raw(o, lane, t);
    Id v = t == Type::F16 ? from_half(r) : cast(tf_, r);
    if (o.absolute) v = ext(tf_, GLSLstd450FAbs, {v});
    if (o.negate) v = un(SpvOpFNegate, tf_, v);
    return v;
}

void Translator::put(const Operand& d, u32 lane, Type t, Id value) {
    if (d.bank == Bank::Index) {
        const u32 which = d.number >= 2 ? 1 : 0;
        store_now(ti_, chain(tp_fi_, index_, {u(which)}), cast(ti_, value));
        return;
    }
    if (d.bank == Bank::Internal && t == Type::F16) {
        const Slot sl = slot(d, lane, 32, true);
        if (sl.ptr) writes_.push_back(Pending{sl, cast(tu_, from_half(value)), cond_});
        return;
    }
    const Slot sl = slot(d, lane, width_of(t), true);
    if (sl.ptr) writes_.push_back(Pending{sl, value, cond_});
}

void Translator::commit() {
    for (const Pending& p : writes_) {
        const Slot& sl = p.slot;
        Id old = 0;
        Id v;
        if (sl.bits == 32) {
            v = p.value;
        } else {
            const u32 m = ((1u << sl.bits) - 1) << sl.shift;
            const Id part = band(sl.shift ? shl(p.value, u(sl.shift)) : p.value, u(m));
            if (sl.clear) {
                v = part;
            } else {
                old = load(tu_, sl.ptr);
                v = bor(band(old, u(~m)), part);
            }
        }
        if (p.cond) v = sel(tu_, p.cond, v, old ? old : load(tu_, sl.ptr));
        store(sl.ptr, v);
    }
    writes_.clear();
}

Id Translator::cond_float(Cond c, Id v) {
    const Id zero = f(0.0f);
    switch (c) {
        case Cond::Always: return b_.c_bool(true);
        case Cond::EqZero: return bin(SpvOpFOrdEqual, tb_, v, zero);
        case Cond::NeZero: return bin(SpvOpFUnordNotEqual, tb_, v, zero);
        case Cond::PosZero: return ieq(shr(cast(tu_, v), u(31)), u(0));
        case Cond::LeZero: return bin(SpvOpFOrdLessThanEqual, tb_, v, zero);
        case Cond::LtZero: return bin(SpvOpFOrdLessThan, tb_, v, zero);
        case Cond::NegZero: return ine(shr(cast(tu_, v), u(31)), u(0));
        case Cond::GeZero: return bin(SpvOpFOrdGreaterThanEqual, tb_, v, zero);
        case Cond::GtZero: return bin(SpvOpFOrdGreaterThan, tb_, v, zero);
    }
    return b_.c_bool(false);
}

Id Translator::cond_int(Cond c, Id v, Id type, Id zero) {
    (void)type;
    switch (c) {
        case Cond::Always: return b_.c_bool(true);
        case Cond::EqZero: return bin(SpvOpIEqual, tb_, v, zero);
        case Cond::NeZero: return bin(SpvOpINotEqual, tb_, v, zero);
        case Cond::PosZero:
        case Cond::GeZero: return bin(SpvOpSGreaterThanEqual, tb_, v, zero);
        case Cond::LeZero: return bin(SpvOpSLessThanEqual, tb_, v, zero);
        case Cond::LtZero:
        case Cond::NegZero: return bin(SpvOpSLessThan, tb_, v, zero);
        case Cond::GtZero: return bin(SpvOpSGreaterThan, tb_, v, zero);
    }
    return b_.c_bool(false);
}

Id Translator::predicate(Pred p) {
    auto get = [&](u32 n) { return load(tb_, chain(tp_fb_, pred_, {u(n)})); };
    switch (p) {
        case Pred::None: return 0;
        case Pred::P0: return get(0);
        case Pred::P1: return get(1);
        case Pred::P2: return get(2);
        case Pred::P3: return get(3);
        case Pred::NotP0: return lnot(get(0));
        case Pred::NotP1: return lnot(get(1));
        case Pred::NotP2: return lnot(get(2));
        case Pred::Pn: fail("per-instance predicate not implemented"); return 0;
    }
    return 0;
}

// ---- Floating point --------------------------------------------------------------------------------------------

Id Translator::alu(Op op, Id a, Id c, Id d) {
    switch (op) {
        case Op::Mad: return fadd(fmul(a, c), d);
        case Op::Add: return fadd(a, c);
        case Op::Mul: return fmul(a, c);
        case Op::Min: return ext(tf_, GLSLstd450NMin, {a, c});
        case Op::Max: return ext(tf_, GLSLstd450NMax, {a, c});
        case Op::Frc:
        case Op::SubFlr: return fsub(a, ext(tf_, GLSLstd450Floor, {c}));
        case Op::Rcp: return fdiv(f(1.0f), a);
        case Op::Rsq: return ext(tf_, GLSLstd450InverseSqrt, {a});
        case Op::Log: return ext(tf_, GLSLstd450Log2, {a});
        case Op::Exp: return ext(tf_, GLSLstd450Exp2, {a});
        case Op::Mov: return a;
        case Op::Cmp: return fsub(a, c);
        default: return f(0.0f);  // (dsx, dsy: one instance has no neighbours)
    }
}

void Translator::float_op(const Instruction& in) {
    const Type t = in.type;
    const Type st = in.src_type != Type::None ? in.src_type : t;
    const Operand& d = in.dest;
    const bool unary = in.op == Op::Rcp || in.op == Op::Rsq || in.op == Op::Log || in.op == Op::Exp;
    if (!is_float(t) || !is_float(st)) {
        fail(nx::format("{} of type {} not implemented", usse::mnemonic(in.op), static_cast<u32>(t)));
        return;
    }
    if (in.op == Op::Dot) {
        Id sum = f(0.0f);
        const Operand& a = in.src[0];
        const Operand& c = in.src[1];
        const u8 m = a.mask ? a.mask : 15;
        for (u32 k = 0; k < 4; ++k)
            if ((m >> k) & 1) sum = fadd(sum, fmul(fval(a, k, st), fval(c, k, st)));
        for (u32 k = 0; k < 4; ++k)
            if ((d.mask >> k) & 1) putf(d, k, t, sum);
        return;
    }
    bool scalar = unary;
    if (!scalar && in.src_count) {
        scalar = true;
        for (u8 i = 0; i < in.src_count; ++i) scalar &= in.src[i].lanes == 1;
    }
    if (scalar) {
        Id v[3] = {f(0.0f), f(0.0f), f(0.0f)};
        for (u8 i = 0; i < in.src_count && i < 3; ++i) v[i] = fval(in.src[i], lowest_lane(in.src[i].mask), st);
        const Id r = alu(in.op, v[0], v[1], v[2]);
        for (u32 k = 0; k < 4; ++k)
            if ((d.mask >> k) & 1) putf(d, k, t, r);
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        Id v[3] = {f(0.0f), f(0.0f), f(0.0f)};
        for (u8 i = 0; i < in.src_count && i < 3; ++i) v[i] = fval(in.src[i], k, st);
        putf(d, k, t, alu(in.op, v[0], v[1], v[2]));
    }
}

// ---- Conversions and moves -------------------------------------------------------------------------------------

Id Translator::convert(Id v, Type from, Type to, bool scale) {
    Id x;
    switch (from) {
        case Type::F32: x = cast(tf_, v); break;
        case Type::F16: x = from_half(mask(v, 16)); break;
        case Type::U8: x = un(SpvOpConvertUToF, tf_, band(v, u(0xFF))); if (scale) x = fdiv(x, f(255.0f)); break;
        case Type::S8: x = un(SpvOpConvertSToF, tf_, sext(v, 8)); if (scale) x = fdiv(x, f(127.0f)); break;
        case Type::U16: x = un(SpvOpConvertUToF, tf_, band(v, u(0xFFFF))); if (scale) x = fdiv(x, f(65535.0f)); break;
        case Type::S16: x = un(SpvOpConvertSToF, tf_, sext(v, 16)); if (scale) x = fdiv(x, f(32767.0f)); break;
        case Type::Fx10: x = fdiv(un(SpvOpConvertSToF, tf_, sext(v, 10)), f(255.0f)); break;
        default: return v;
    }
    auto to_int = [&](f32 lo, f32 hi, f32 k) {
        Id g = scale ? fmul(x, f(k)) : x;
        g = sel(tf_, un(SpvOpIsNan, tb_, g), f(0.0f), ext(tf_, GLSLstd450NMin, {ext(tf_, GLSLstd450NMax, {g, f(lo)}), f(hi)}));
        g = ext(tf_, GLSLstd450Trunc, {g});  // (scaled values too: see interp.cpp)
        return cast(tu_, un(SpvOpConvertFToS, ti_, g));
    };
    switch (to) {
        case Type::F32: return cast(tu_, x);
        case Type::F16: return to_half(cast(tu_, x));
        case Type::U8: return band(to_int(0.0f, 255.0f, 255.0f), u(0xFF));
        case Type::S8: return band(to_int(-128.0f, 127.0f, 127.0f), u(0xFF));
        case Type::U16: return band(to_int(0.0f, 65535.0f, 65535.0f), u(0xFFFF));
        case Type::S16: return band(to_int(-32768.0f, 32767.0f, 32767.0f), u(0xFFFF));
        case Type::Fx10: {
            Id g = fmul(x, f(255.0f));
            g = sel(tf_, un(SpvOpIsNan, tb_, x), f(0.0f), ext(tf_, GLSLstd450FClamp, {g, f(-512.0f), f(511.0f)}));
            return fx_raw(un(SpvOpConvertFToS, ti_, ext(tf_, GLSLstd450RoundEven, {g})), Type::Fx10);
        }
        default: return v;
    }
}

void Translator::pack_op(const Instruction& in) {
    const Type dt = in.type, st = in.src_type;
    const Operand& d = in.dest;
    const bool scale = (in.flags & usse::kScale) != 0;
    if (dt == Type::O8 || st == Type::O8) {
        fail("o8 pack not implemented");
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        Id v;
        if (st == Type::F32 && (in.flags & usse::kPairSource)) {
            const Comp c = in.src[0].swizzle[k];
            const bool second = c == Comp::Z || c == Comp::W;
            Operand o = in.src[second ? 1 : 0];
            o.swizzle[k] = static_cast<Comp>(static_cast<u32>(c) & 1);
            v = raw(o, k, st);
        } else {
            v = raw(in.src[0], k, st);
        }
        put(d, k, dt, convert(v, st, dt, scale));
    }
}

Id Translator::fx_mul(Id a, Id c) {
    const Id p = bin(SpvOpIMul, ti_, a, c);
    const Id pos = bin(SpvOpSDiv, ti_, bin(SpvOpIAdd, ti_, p, s(127)), s(255));
    const Id neg = un(SpvOpSNegate, ti_,
                      bin(SpvOpSDiv, ti_, bin(SpvOpIAdd, ti_, un(SpvOpSNegate, ti_, p), s(127)), s(255)));
    return sel(ti_, bin(SpvOpSGreaterThanEqual, tb_, p, s(0)), pos, neg);
}

void Translator::move_op(const Instruction& in) {
    const Type t = in.type;
    const Operand& d = in.dest;
    if (t == Type::Fx10) {
        for (u32 k = 0; k < 4; ++k) {
            if (!((d.mask >> k) & 1)) continue;
            Id v = raw(in.src[0], k, t);
            if (in.op != Op::Mov) {
                const Id take = cond_int(in.cond, fx_value(raw(in.src[0], k, t), t), ti_, s(0));
                v = sel(tu_, take, raw(in.src[1], k, t), raw(in.src[2], k, t));
            }
            put(d, k, t, band(v, u(0x3FF)));
        }
        return;
    }
    if (!is_float(t)) {
        auto byte = [&](const Operand& o, u32 k) -> Id {
            if (o.bank == Bank::Literal) return u((static_cast<u32>(o.number) >> (8 * k)) & 0xFF);
            const Comp c = o.lanes ? o.swizzle[k & 3] : static_cast<Comp>(k);
            if (c >= Comp::Zero) return raw(o, k, Type::U8);
            Operand word = o;
            word.lanes = 0;
            const Id w = raw(word, 0, Type::U32);
            return band(static_cast<u32>(c) ? shr(w, u(8 * static_cast<u32>(c))) : w, u(0xFF));
        };
        Operand dword = d;
        dword.lanes = 0;
        Id result = d.bank == Bank::Index ? u(0) : raw(dword, 0, Type::U32);
        Id whole = 0;
        if (in.op == Op::Cmov) {
            const u32 bits = width_of(t);
            whole = cond_int(in.cond, sext(raw(in.src[0], 0, Type::U32), bits), ti_, s(0));
        }
        for (u32 k = 0; k < 4; ++k) {
            if (!((d.mask >> k) & 1)) continue;
            Id v;
            if (in.op == Op::Cmov) {
                v = sel(tu_, whole, byte(in.src[1], k), byte(in.src[2], k));
            } else if (in.op == Op::Cmov8) {
                const Id take = cond_int(in.cond, sext(byte(in.src[0], k), 8), ti_, s(0));
                v = sel(tu_, take, byte(in.src[1], k), byte(in.src[2], k));
            } else {
                v = byte(in.src[0], k);
            }
            result = bor(band(result, u(~(0xFFu << (8 * k)))), k ? shl(v, u(8 * k)) : v);
        }
        put(dword, 0, Type::U32, result);
        return;
    }
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1) && d.lanes) continue;
        if (in.op == Op::Mov) {
            Id v = raw(in.src[0], k, t);
            if (in.src[0].negate || in.src[0].absolute) {
                const Id x = fval(in.src[0], k, t);
                v = t == Type::F16 ? to_half(cast(tu_, x)) : cast(tu_, x);
            }
            put(d, k, t, v);
        } else {
            const Id take = cond_float(in.cond, fval(in.src[0], k, t));
            put(d, k, t, sel(tu_, take, raw(in.src[1], k, t), raw(in.src[2], k, t)));
        }
        if (!d.lanes) break;
    }
}

// ---- Fixed point -----------------------------------------------------------------------------------------------

void Translator::sop_op(const Instruction& in) {
    const Type t = in.type;
    const Operand& d = in.dest;
    const Id one = s(255);
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        Id a = fx_value(raw(in.src[0], k, t), t);
        const Id c = fx_value(raw(in.src[1], k, t), t);
        if (in.complement_a) a = bin(SpvOpISub, ti_, one, a);
        Id r;
        if (in.sop_op == Op::Min || in.sop_op == Op::Max) {
            r = ext(ti_, in.sop_op == Op::Min ? GLSLstd450SMin : GLSLstd450SMax, {a, c});
        } else {
            auto alpha = [&](const Operand& o) { return fx_value(raw(o, 3, t), t); };
            auto factor = [&](Factor fa) -> Id {
                switch (fa) {
                    case Factor::Zero: return s(0);
                    case Factor::Sat:
                        return ext(ti_, GLSLstd450SMin, {alpha(in.src[0]), bin(SpvOpISub, ti_, one, alpha(in.src[1]))});
                    case Factor::A: return a;
                    case Factor::AAlpha: return alpha(in.src[0]);
                    case Factor::B: return c;
                    case Factor::BAlpha: return alpha(in.src[1]);
                    case Factor::S0: return fx_value(raw(in.src[2], k, t), t);
                    case Factor::S0Alpha: return alpha(in.src[2]);
                    default: return s(0);
                }
            };
            auto term = [&](Id v, const SopTerm& st) {
                const Id fa = factor(st.factor);
                return fx_mul(st.complement ? bin(SpvOpISub, ti_, one, fa) : fa, v);
            };
            const Id t0 = term(a, in.term[0]), t1 = term(c, in.term[1]);
            r = bin(in.sop_op == Op::Sub ? SpvOpISub : SpvOpIAdd, ti_, t0, t1);
        }
        put(d, k, t, fx_raw(r, t));
    }
}

void Translator::fxmad_op(const Instruction& in) {
    const Type t = in.type == Type::Fx10 ? Type::Fx10 : Type::Fx8;
    const Operand& d = in.dest;
    for (u32 k = 0; k < 4; ++k) {
        if (!((d.mask >> k) & 1)) continue;
        const Id a = fx_value(raw(in.src[0], k, t), t), c = fx_value(raw(in.src[1], k, t), t);
        Id e = fx_value(raw(in.src[2], k, t), t);
        if (in.src[2].negate) e = un(SpvOpSNegate, ti_, e);
        put(d, k, t, fx_raw(bin(SpvOpIAdd, ti_, fx_mul(a, c), e), t));
    }
}

// ---- Integers --------------------------------------------------------------------------------------------------

Id Translator::bitwise(Op op, Id a, Id c, u32 bits) {
    const Id m = u(bits == 32 ? ~0u : (1u << bits) - 1);
    const Id sh = band(c, u(bits - 1));
    switch (op) {
        case Op::And: return band(a, c);
        case Op::Or: return bor(a, c);
        case Op::Xor: return bxor(a, c);
        case Op::Shl: return band(shl(a, sh), m);
        case Op::Shr: return shr(band(a, m), sh);
        case Op::Asr: {
            const Id v = bits == 32 ? cast(ti_, a) : sext(a, 16);
            return band(cast(tu_, bin(SpvOpShiftRightArithmetic, ti_, v, sh)), m);
        }
        case Op::Rol: {
            // (a shift by the full width is undefined in SPIR-V and selected away)
            const Id rot = band(bor(shl(a, sh), shr(band(a, m), isub(u(bits), sh))), m);
            return sel(tu_, ieq(sh, u(0)), band(a, m), rot);
        }
        default: return u(0);
    }
}

void Translator::bitwise_op(const Instruction& in) {
    const Type t = in.type;
    const u32 bits = width_of(t);
    const Operand& d = in.dest;
    if (in.op == Op::Rlp) {
        fail("rlp not implemented");
        return;
    }
    const u32 n = d.bank == Bank::Index ? 1 : (d.lanes ? d.lanes : 1);
    for (u32 k = 0; k < n; ++k) {
        if (d.lanes && !((d.mask >> k) & 1)) continue;
        const Id a = raw(in.src[0], k, t);
        Id c = raw(in.src[1], k, t);
        if (in.src[1].bank == Bank::Literal && bits == 16) c = u((static_cast<u32>(in.src[1].number) >> (16 * k)) & 0xFFFF);
        if (in.flags & usse::kInvertB) c = un(SpvOpNot, tu_, c);
        put(d, k, t, bitwise(in.op, a, c, bits));
    }
}

void Translator::test_op(const Instruction& in) {
    const Type t = in.type;
    const bool flt = is_float(t);
    const Operand& a = in.src[0];
    const Operand& c = in.src[1];
    auto lane_result = [&](u32 k) -> Id {
        if (flt) {
            if (in.alu == Op::Dot) {
                Id sum = f(0.0f);
                for (u32 j = 0; j < 4; ++j)
                    if ((a.mask >> j) & 1) sum = fadd(sum, fmul(fval(a, j, t), fval(c, j, t)));
                return cond_float(in.cond, sum);
            }
            const Id va = fval(a, k, t), vb = in.src_count > 1 ? fval(c, k, t) : f(0.0f);
            return cond_float(in.cond, alu(in.alu, va, vb, f(0.0f)));
        }
        const u32 bits = width_of(t);
        const bool sgn = t == Type::I16 || t == Type::I32 || t == Type::S8 || t == Type::S16;
        auto ext64 = [&](Id r) { return sgn ? to64s(sext(r, bits)) : to64u(mask(r, bits)); };
        const Id va = ext64(raw(a, k, t)), vb = in.src_count > 1 ? ext64(raw(c, k, t)) : l(0);
        Id r;
        switch (in.alu) {
            case Op::Add: r = bin(SpvOpIAdd, tl(), va, vb); break;
            case Op::Mul: r = bin(SpvOpIMul, tl(), va, vb); break;
            case Op::Cmp: r = bin(SpvOpISub, tl(), va, vb); break;
            case Op::And: r = bin(SpvOpBitwiseAnd, tl(), va, vb); break;
            case Op::Or: r = bin(SpvOpBitwiseOr, tl(), va, vb); break;
            case Op::Xor: r = bin(SpvOpBitwiseXor, tl(), va, vb); break;
            default:
                fail(nx::format("test of {} not implemented", usse::mnemonic(in.alu)));
                return b_.c_bool(false);
        }
        if (!sgn && in.alu != Op::Cmp) r = bin(SpvOpBitwiseAnd, tl(), r, l((i64{1} << bits) - 1));
        return cond_int(in.cond, r, tl(), l(0));
    };
    if (in.dest.bank == Bank::Predicate) {
        Id r;
        const u8 m = a.mask ? a.mask : 1;
        switch (in.test_mode) {
            case TestMode::Scalar: r = lane_result(lowest_lane(m)); break;
            case TestMode::AndAll:
            case TestMode::OrAll: {
                const bool all = in.test_mode == TestMode::AndAll;
                r = b_.c_bool(all);
                for (u32 k = 0; k < 4; ++k)
                    if ((m >> k) & 1) r = all ? land(r, lane_result(k)) : lor(r, lane_result(k));
                break;
            }
            default: fail("per-lane test not implemented"); return;
        }
        store_now(tb_, chain(tp_fb_, pred_, {u(static_cast<u32>(in.dest.number) & 3)}), r);
        return;
    }
    const Operand& d = in.dest;
    const u32 n = d.lanes == 4 ? 4 : 1;
    for (u32 k = 0; k < n; ++k) {
        if (n == 4 && !((d.mask >> k) & 1)) continue;
        const Id r = lane_result(k);
        const u32 bits = width_of(t);
        if (is_float(t) && n == 4) putf(d, k, t, sel(tf_, r, f(1.0f), f(0.0f)));  // (1.0 or 0.0: see interp.cpp)
        else put(d, k, t, sel(tu_, r, u(bits == 32 ? ~0u : (1u << bits) - 1), u(0)));
    }
}

Id Translator::part_of(Id v, Part p, bool sgn) {
    switch (p) {
        case Part::Lo16: return sgn ? to64s(sext(v, 16)) : to64u(band(v, u(0xFFFF)));
        case Part::Hi16: return sgn ? to64s(sext(shr(v, u(16)), 16)) : to64u(shr(v, u(16)));
        case Part::Lo8: return sgn ? to64s(sext(v, 8)) : to64u(band(v, u(0xFF)));
        case Part::Hi8: return sgn ? to64s(sext(shr(v, u(8)), 8)) : to64u(band(shr(v, u(8)), u(0xFF)));
        case Part::None: return sgn ? to64s(cast(ti_, v)) : to64u(v);
    }
    return l(0);
}

void Translator::imad_op(const Instruction& in) {
    const bool sgn = in.type == Type::I32 || in.type == Type::I16;
    const Operand& d = in.dest;
    auto clamp64 = [&](Id r, i64 lo, i64 hi) { return ext(tl(), GLSLstd450SClamp, {r, l(lo), l(hi)}); };
    if (in.flags & usse::kScaleVariant) {
        const Id a = raw(in.src[0], 0, Type::U32);
        Id c = raw(in.src[1], 0, Type::U32);
        Id e = raw(in.src[2], 0, Type::U32);
        if (in.src[1].negate) c = isub(u(0), c);
        if (in.src[2].negate) e = isub(u(0), e);
        const Id r = (in.flags & usse::kSignedShift) ? iadd(shl(imul(a, shr(c, u(16))), u(16)), e)
                                                     : iadd(imul(a, band(c, u(0xFFFF))), e);
        put(d, 0, Type::U32, r);
        return;
    }
    if (in.type == Type::I16 || in.type == Type::U16) {
        if (d.absolute) {
            fail("16-bit multiply-add with an absolute destination not implemented");
            return;
        }
        auto value = [&](const Operand& o, u32 k) -> Id {
            if (o.part == Part::Lo8 || o.part == Part::Hi8) {
                const Id w = o.bank == Bank::Literal ? u(static_cast<u32>(o.number)) : raw(o, 0, Type::U32);
                return part_of(w, o.part, sgn);
            }
            const Id h = o.bank == Bank::Literal ? u(static_cast<u32>(o.number) & 0xFFFF) : raw(o, k, Type::U16);
            return sgn ? to64s(sext(h, 16)) : to64u(band(h, u(0xFFFF)));
        };
        const u32 n = d.bank == Bank::Index ? 1 : 2;
        for (u32 k = 0; k < n; ++k) {
            const Id a = raw(in.src[0], k, Type::U16);
            const Id s0 = sgn ? to64s(sext(a, 16)) : to64u(band(a, u(0xFFFF)));
            Id e = value(in.src[2], k);
            if (in.src[2].negate) e = un(SpvOpSNegate, tl(), e);
            Id r = bin(SpvOpIAdd, tl(), bin(SpvOpIMul, tl(), s0, value(in.src[1], k)), e);
            if (in.flags & usse::kSat) r = sgn ? clamp64(r, INT16_MIN, INT16_MAX) : clamp64(r, 0, UINT16_MAX);
            put(d, k, Type::U16, band(to32(r), u(0xFFFF)));
        }
        return;
    }
    const Id a = part_of(raw(in.src[0], 0, Type::U32), in.src[0].part, sgn);
    const Id c = part_of(raw(in.src[1], 0, Type::U32), in.src[1].part, sgn);
    const Id e = part_of(raw(in.src[2], 0, Type::U32), in.src[2].part, sgn);
    Id r = bin(SpvOpIAdd, tl(), bin(SpvOpIMul, tl(), a, c), e);
    if (in.flags & usse::kSat) r = sgn ? clamp64(r, INT32_MIN, INT32_MAX) : clamp64(r, 0, UINT32_MAX);
    put(d, 0, Type::U32, to32(r));
}

// ---- Textures and memory ---------------------------------------------------------------------------------------

// The test model (psvusse --run): the image of the unit the state's first word names, at the nearest texel with
// wrapping; cube maps pick the face of the largest coordinate and sample it at the others divided by that one.
void Translator::sample(Id unit, TexDim dim, const Id coord[4], Id rgba[4]) {
    auto data = [&](Id index) { return load(tu_, buffer_word(tex_buf_, index)); };
    const Id header = imul(ext(tu_, GLSLstd450UMin, {unit, u(ComputeLayout::kTextures - 1)}), u(4));
    const Id width = data(header), height = data(iadd(header, u(1))), first = data(iadd(header, u(2)));
    Id cu = coord[0], cv = dim == TexDim::D1 ? f(0.5f) : coord[1];
    Id face = u(0), rows = height;
    if (dim == TexDim::Cube) {
        const Id ax = ext(tf_, GLSLstd450FAbs, {coord[0]}), ay = ext(tf_, GLSLstd450FAbs, {coord[1]}),
                 az = ext(tf_, GLSLstd450FAbs, {coord[2]});
        auto ge = [&](Id p, Id q) { return bin(SpvOpFOrdGreaterThanEqual, tb_, p, q); };
        const Id on_x = land(ge(ax, ay), ge(ax, az)), on_y = ge(ay, az);
        auto side = [&](Id c, u32 pos) { return sel(tu_, ge(c, f(0.0f)), u(pos), u(pos + 1)); };
        face = sel(tu_, on_x, side(coord[0], 0), sel(tu_, on_y, side(coord[1], 2), side(coord[2], 4)));
        const Id m = sel(tf_, on_x, ax, sel(tf_, on_y, ay, az));
        const Id su = sel(tf_, on_x, coord[2], coord[0]);
        const Id sv = sel(tf_, on_x, coord[1], sel(tf_, on_y, coord[2], coord[1]));
        cu = fmul(fadd(fdiv(su, m), f(1.0f)), f(0.5f));
        cv = fmul(fadd(fdiv(sv, m), f(1.0f)), f(0.5f));
        rows = bin(SpvOpUDiv, tu_, height, u(6));
    }
    auto wrap = [&](Id c, Id n) {
        const Id nn = ext(tu_, GLSLstd450UMax, {n, u(1)});
        Id g = ext(tf_, GLSLstd450Floor, {fmul(c, un(SpvOpConvertUToF, tf_, nn))});
        g = ext(tf_, GLSLstd450FClamp, {g, f(-1073741824.0f), f(1073741824.0f)});
        return cast(tu_, bin(SpvOpSMod, ti_, un(SpvOpConvertFToS, ti_, g), cast(ti_, nn)));
    };
    const Id texel_index = iadd(first, iadd(imul(iadd(imul(face, rows), wrap(cv, rows)), width), wrap(cu, width)));
    const Id last = isub(b_.op(SpvOpArrayLength, tu_, {tex_buf_, 0}), u(1));
    const Id present = ine(width, u(0));
    const Id texel = sel(tu_, present, data(ext(tu_, GLSLstd450UMin, {texel_index, last})), u(0));
    for (u32 k = 0; k < 4; ++k)
        rgba[k] = fdiv(un(SpvOpConvertUToF, tf_, band(k ? shr(texel, u(8 * k)) : texel, u(0xFF))), f(255.0f));
}

void Translator::write_sample(const Id rgba[4], u32 format, Id regs[4]) {
    if (format == 0) {
        Id w = u(0);
        for (u32 k = 0; k < 4; ++k) {
            const Id c = sel(tf_, un(SpvOpIsNan, tb_, rgba[k]), f(0.0f), ext(tf_, GLSLstd450FClamp, {rgba[k], f(0.0f), f(1.0f)}));
            const Id q = cast(tu_, un(SpvOpConvertFToS, ti_, ext(tf_, GLSLstd450RoundEven, {fmul(c, f(255.0f))})));
            w = bor(w, k ? shl(q, u(8 * k)) : q);
        }
        regs[0] = w;
    } else if (format == 1) {
        regs[0] = bor(to_half(cast(tu_, rgba[0])), shl(to_half(cast(tu_, rgba[1])), u(16)));
        regs[1] = bor(to_half(cast(tu_, rgba[2])), shl(to_half(cast(tu_, rgba[3])), u(16)));
    } else {
        for (u32 k = 0; k < 4; ++k) regs[k] = cast(tu_, rgba[k]);
    }
}

Id Translator::texture(u32 binding) { return textures_[binding]; }

// The .irsd query of a 2D texture (f32): the bilinear footprint's four texels (their first component) and their
// weights, as the hardware returns them (inferred from the programs that use it: no oracle has it yet):
// registers 0-3 the texels (x0, y0), (x1, y0), (x0, y1), (x1, y1) as f32, registers 4-5 four f16 weights with
// texel k's at index 3 - k.
void Translator::gather_unit(u32 binding, const Id coord[4], Id regs[6]) {
    const Id image = b_.t_image(tf_, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown);
    const Id sampled = b_.t_sampled_image(image);
    const Id t = load(sampled, sampler_variable(binding, false));
    image_query_ = true;
    const Id tc = b_.t_vector(tf_, 2);
    const Id c = b_.op(SpvOpCompositeConstruct, tc, {coord[0], coord[1]});
    // (Vulkan's gather order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0))
    const Id g = b_.op(SpvOpImageGather, tv4_, {t, c, b_.c_int(0)});
    const Id size = b_.op(SpvOpImageQuerySizeLod, b_.t_vector(ti_, 2), {b_.op(SpvOpImage, image, {t}), b_.c_int(0)});
    Id frac[2];
    for (u32 k = 0; k < 2; ++k) {
        const Id extent = un(SpvOpConvertSToF, tf_, b_.op(SpvOpCompositeExtract, ti_, {size, k}));
        frac[k] = ext(tf_, GLSLstd450Fract, {fsub(fmul(coord[k], extent), f(0.5f))});
    }
    const Id texel[4] = {b_.op(SpvOpCompositeExtract, tf_, {g, 3u}), b_.op(SpvOpCompositeExtract, tf_, {g, 2u}),
                         b_.op(SpvOpCompositeExtract, tf_, {g, 0u}), b_.op(SpvOpCompositeExtract, tf_, {g, 1u})};
    const Id fu = frac[0], fv = frac[1], gu = fsub(f(1.0f), fu), gv = fsub(f(1.0f), fv);
    const Id weight[4] = {fmul(gu, gv), fmul(fu, gv), fmul(gu, fv), fmul(fu, fv)};
    for (u32 k = 0; k < 4; ++k) regs[k] = cast(tu_, texel[k]);
    auto h = [&](u32 k) { return to_half(cast(tu_, weight[k])); };
    regs[4] = bor(h(3), shl(h(2), u(16)));
    regs[5] = bor(h(1), shl(h(0), u(16)));
}

// The combined image sampler variable of a draw binding.
Id Translator::sampler_variable(u32 binding, bool cube) {
    const Id image = b_.t_image(tf_, cube ? SpvDimCube : SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown);
    const Id sampled = b_.t_sampled_image(image);
    if (!textures_[binding]) {
        const Id v = b_.variable(b_.t_pointer(SpvStorageClassUniformConstant, sampled), SpvStorageClassUniformConstant);
        b_.decorate(v, SpvDecorationDescriptorSet, {0});
        b_.decorate(v, SpvDecorationBinding, {binding});
        textures_[binding] = v;
    }
    return textures_[binding];
}

void Translator::sample_unit(u32 binding, TexDim dim, const Id coord[4], usse::TexKind kind, const Id extra[4],
                             Id rgba[4]) {
    const bool cube = dim == TexDim::Cube;
    const Id image = b_.t_image(tf_, cube ? SpvDimCube : SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown);
    const Id sampled = b_.t_sampled_image(image);
    const Id t = load(sampled, sampler_variable(binding, cube));
    const u32 n = cube ? 3 : 2;
    const Id tc = b_.t_vector(tf_, n);
    const Id c = cube ? b_.op(SpvOpCompositeConstruct, tc, {coord[0], coord[1], coord[2]})
                      : b_.op(SpvOpCompositeConstruct, tc, {coord[0], dim == TexDim::D1 ? f(0.5f) : coord[1]});
    // Implicit levels of detail need derivatives: fragment shaders without branches only (else level 0).
    const bool implicit = form_ == Form::Fragment && !branches_;
    Id r;
    switch (kind) {
        case usse::TexKind::Bias:
            r = implicit ? b_.op(SpvOpImageSampleImplicitLod, tv4_, {t, c, SpvImageOperandsBiasMask, extra[0]})
                         : b_.op(SpvOpImageSampleExplicitLod, tv4_, {t, c, SpvImageOperandsLodMask, f(0.0f)});
            break;
        case usse::TexKind::Replace:
            r = b_.op(SpvOpImageSampleExplicitLod, tv4_, {t, c, SpvImageOperandsLodMask, extra[0]});
            break;
        case usse::TexKind::Grad: {
            const Id dx = cube ? b_.op(SpvOpCompositeConstruct, tc, {extra[0], extra[1], f(0.0f)})
                               : b_.op(SpvOpCompositeConstruct, tc, {extra[0], extra[1]});
            const Id dy = cube ? b_.op(SpvOpCompositeConstruct, tc, {extra[2], extra[3], f(0.0f)})
                               : b_.op(SpvOpCompositeConstruct, tc, {extra[2], extra[3]});
            r = b_.op(SpvOpImageSampleExplicitLod, tv4_, {t, c, SpvImageOperandsGradMask, dx, dy});
            break;
        }
        default:
            r = implicit ? b_.op(SpvOpImageSampleImplicitLod, tv4_, {t, c})
                         : b_.op(SpvOpImageSampleExplicitLod, tv4_, {t, c, SpvImageOperandsLodMask, f(0.0f)});
            break;
    }
    for (u32 k = 0; k < 4; ++k) rgba[k] = b_.op(SpvOpCompositeExtract, tf_, {r, k});
}

void Translator::tex_op(const Instruction& in) {
    const bool query = in.flags & (usse::kTexRsd | usse::kTexInfo | usse::kTexMinP);
    if (query && (form_ == Form::Compute || (in.flags & usse::kTexMinP) || !(in.flags & usse::kTexRsd) ||
                  !(in.flags & usse::kTexInfo) || in.tex_dim != TexDim::D2 || in.type != Type::F32)) {
        fail("texture query not implemented (only .irsd of 2D textures, f32, in the draw forms)");
        return;
    }
    Operand state = in.src[1];
    state.lanes = 0;
    Id coord[4] = {f(0.0f), f(0.0f), f(0.0f), f(0.0f)};
    const u32 n = static_cast<u32>(in.tex_dim) + 1;
    for (u32 k = 0; k < n; ++k) coord[k] = fval(in.src[0], k, Type::F32);
    Id rgba[4];
    if (form_ == Form::Compute) {
        const Id unit = raw(state, 0, Type::U32);
        for (u32 k = 1; k < 4; ++k)
            if (!word_ptr(state, state.number + k, false)) return;
        sample(unit, in.tex_dim, coord, rgba);
    } else {
        // The unit whose state words the driver put where the instruction reads them (the program's sampler slots).
        const usse::Container* k = gxp_.container(usse::kConstantsContainer);
        i32 unit = -1;
        // (a secondary program names the secondary attributes as its primary ones)
        const bool sa_bank =
            state.bank == Bank::Secondary || ((form_ == Form::Secondary || in_secondary_) && state.bank == Bank::Primary);
        if (k && sa_bank && !state.indexed)
            for (const usse::SamplerSlot& slot : gxp_.sampler_slots)
                if (slot.word % 4 == 0 && static_cast<i32>(k->base + slot.offset) == state.number) unit = slot.word / 4;
        const bool vertex = gxp_.type == usse::ShaderType::Vertex;
        const u32 units = vertex ? 4 : 16;
        if (unit < 0 || static_cast<u32>(unit) >= units) {
            fail("a texture instruction whose sampler is not one of the program's");
            return;
        }
        Id extra[4] = {f(0.0f), f(0.0f), f(0.0f), f(0.0f)};
        if (in.src_count > 2) {
            // (four words from the operand: registers, an internal register's lanes, or constants)
            Operand e = in.src[2];
            e.lanes = 4;
            e.negate = e.absolute = false;
            for (u32 j = 0; j < 4; ++j) {
                e.swizzle[j] = static_cast<Comp>(j);
                extra[j] = cast(tf_, raw(e, j, Type::U32));
            }
        }
        const u32 binding = (vertex ? kDrawBindingVertexTextures : kDrawBindingFragmentTextures) + static_cast<u32>(unit);
        if (query) {
            Id regs[6];
            gather_unit(binding, coord, regs);
            for (u32 k = 0; k < 6; ++k) {
                Operand d = in.dest;
                d.lanes = 0;
                if (const Id w = word_ptr(d, d.number + k, true)) writes_.push_back(Pending{Slot{w, 0, 32}, regs[k], cond_});
            }
            return;
        }
        sample_unit(binding, in.tex_dim, coord, in.tex_kind, extra, rgba);
    }
    const u32 format = in.type == Type::F32 ? 3 : in.type == Type::F16 ? 1 : 0;
    Id regs[4];
    write_sample(rgba, format, regs);
    const u32 count = format == 0 ? 1u : format == 1 ? 2u : 4u;
    for (u32 k = 0; k < count; ++k) {
        Operand d = in.dest;
        d.lanes = 0;
        if (const Id w = word_ptr(d, d.number + k, true)) writes_.push_back(Pending{Slot{w, 0, 32}, regs[k], cond_});
    }
}

// `size` bytes (1-4) at a guest address, little endian; zero outside the memory buffer.
Id Translator::memory_read(Id address, u32 size) {
    auto data = [&](Id index) { return load(tu_, buffer_word(mem_buf_, index)); };
    const Id base = data(u(0)), count = data(u(1));
    const Id offset = isub(address, base);
    const Id inside = bin(SpvOpULessThanEqual, tb_, iadd(offset, u(size)), imul(count, u(4)));
    const Id inside_too = bin(SpvOpULessThan, tb_, offset, imul(count, u(4)));  // (offset + size wrapping)
    const Id index = ext(tu_, GLSLstd450UMin, {shr(offset, u(2)), count});
    const Id w0 = data(iadd(index, u(ComputeLayout::kMemoryHeader)));
    const Id w1 = data(iadd(index, u(ComputeLayout::kMemoryHeader + 1)));
    const Id sh = shl(band(offset, u(3)), u(3));
    const Id hi = sel(tu_, ieq(sh, u(0)), u(0), shl(w1, isub(u(32), sh)));
    const Id v = mask(bor(shr(w0, sh), hi), 8 * size);
    return sel(tu_, land(inside, inside_too), v, u(0));
}

// `size` bytes (1-4) of `value` at a guest address, under the instruction's predicate; outside the memory buffer
// (or predicated off) they go to a scratch word past its end. A store of 1 or 2 bytes reads, modifies and writes its
// word (not atomically: instances storing bytes of one word can race). Stores of 4 bytes are assumed aligned.
void Translator::memory_write(Id address, Id value, u32 size) {
    auto word = [&](Id index) { return buffer_word(mem_buf_, index); };
    const Id base = load(tu_, word(u(0))), count = load(tu_, word(u(1)));
    const Id offset = isub(address, base);
    Id ok = land(bin(SpvOpULessThanEqual, tb_, iadd(offset, u(size)), imul(count, u(4))),
                 bin(SpvOpULessThan, tb_, offset, imul(count, u(4))));
    if (cond_) ok = land(ok, cond_);
    const Id scratch = iadd(count, u(ComputeLayout::kMemoryHeader));
    const Id index = sel(tu_, ok, iadd(shr(offset, u(2)), u(ComputeLayout::kMemoryHeader)), scratch);
    const Id p = word(index);
    if (size == 4) {
        store(p, value);
        return;
    }
    const Id sh = shl(band(offset, u(3)), u(3));
    const Id m = shl(u(size == 1 ? 0xFFu : 0xFFFFu), sh);
    store(p, bor(band(load(tu_, p), bnot(m)), band(shl(value, sh), m)));
}

void Translator::memory_op(const Instruction& in) {
    if (in.space != usse::Space::Absolute || (in.flags & (usse::kPreIncrement | usse::kPostIncrement))) {
        fail("load/store addressing mode not implemented");
        return;
    }
    const u32 bytes = in.size / 8u;
    Operand base_reg = in.src[0];
    base_reg.lanes = 0;
    const Id base = raw(base_reg, 0, Type::U32);
    const Id off = raw(in.src[1], 0, Type::U32);
    const u32 count = in.fetch ? in.fetch : 1;
    // (as the interpreter: a register offset's low half in bytes; a burst starts one element further)
    Id address = in.src[1].bank == Bank::Literal ? iadd(base, imul(off, u(bytes))) : iadd(base, band(off, u(0xFFFF)));
    if (in.fetch) address = iadd(address, u(bytes));
    const u32 words = in.size == 64 ? 2 : 1;
    if (in.op == Op::Store) {
        for (u32 i = 0; i < count; ++i) {
            for (u32 j = 0; j < words; ++j) {
                Operand s = in.src[2];
                s.lanes = 0;
                const Id p = word_ptr(s, s.number + i * words + j, false);
                if (!p) return;
                memory_write(iadd(address, u(i * bytes + j * 4)), load(tu_, p), nx::min<u32>(bytes, 4));
            }
        }
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        for (u32 j = 0; j < words; ++j) {
            const Id at = iadd(address, u(i * bytes + j * 4));
            const Id v = memory_read(at, nx::min<u32>(bytes, 4));
            Operand d = in.dest;
            d.lanes = 0;
            if (const Id w = word_ptr(d, d.number + i * words + j, true)) writes_.push_back(Pending{Slot{w, 0, 32}, v, cond_});
        }
    }
}

// ---- Programs --------------------------------------------------------------------------------------------------

void Translator::emit(usize i) {
    const Decoded& dec = program_[i];
    const Instruction& in = dec.in;
    word_ = dec.word;
    if (!dec.executed) return;
    cond_ = predicate(in.pred);
    switch (in.op) {
        case Op::Hidden:
        case Op::SetIncrements:
        case Op::Nop:
        case Op::PCoeff:
        case Op::Br: break;
        case Op::Mad:
        case Op::Add:
        case Op::Mul:
        case Op::Min:
        case Op::Max:
        case Op::Frc:
        case Op::Dsx:
        case Op::Dsy:
        case Op::Dot:
        case Op::Rcp:
        case Op::Rsq:
        case Op::Log:
        case Op::Exp:
        case Op::SubFlr: float_op(in); break;
        case Op::Mov:
        case Op::Pack:
            if (in.family == 8) pack_op(in);
            else if (in.family == 7) move_op(in);
            else float_op(in);
            break;
        case Op::Cmov:
        case Op::Cmov8: move_op(in); break;
        case Op::MovImm: put(in.dest, 0, Type::U32, u(static_cast<u32>(in.src[0].number))); break;
        case Op::Test: test_op(in); break;
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Shl:
        case Op::Rol:
        case Op::Shr:
        case Op::Asr:
        case Op::Rlp: bitwise_op(in); break;
        case Op::IMad: imad_op(in); break;
        case Op::Sop:
        case Op::Sop2:
        case Op::Blend: sop_op(in); break;
        case Op::FxMad: fxmad_op(in); break;
        case Op::Load:
        case Op::Store: memory_op(in); break;
        case Op::Tex: tex_op(in); break;
        case Op::Kill: store_now(tb_, killed_, b_.c_bool(true)); break;
        case Op::MovToLink: store_now(tu_, link_, load(tu_, chain(tp_fu_, temp_.var, {u(0)}))), temp_.used = nx::max(temp_.used, 1u); break;
        case Op::MovFromLink: put(Operand{Bank::Temp, 0, 0, false, 1, 1}, 0, Type::U32, load(tu_, link_)); break;
        default: fail(nx::format("{} not implemented", usse::mnemonic(in.op))); break;
    }
    // The co-issued half of a dual-issue word reads the registers as they were before the word.
    const bool pair = in.op != Op::Br && i + 1 < program_.size() && (program_[i + 1].in.flags & usse::kCoissued);
    if (!pair) commit();
}

Id Translator::push_constant(u32 member, Id type) {
    const Id ptr = chain(type == tf_ ? tp_push_f_ : tp_push_u_, push_, {u(member)});
    return load(type, ptr);
}

// The program: straight, or (with branches) a loop around a switch on the block to run next (pc); a block ends by
// setting pc. Returns the condition "the step limit stopped it" (0 without branches).
Id Translator::body(auto&& local) {
    u32 words_count = 0;
    for (const Decoded& d : program_) words_count = nx::max(words_count, d.word + 1);
    const u32 size = static_cast<u32>(program_.size());
    nx::vector<u32> start(words_count + 1, size);
    for (u32 i = size; i-- > 0;) start[program_[i].word] = i;
    for (u32 w = words_count; w-- > 0;)
        if (start[w] == size) start[w] = start[w + 1];
    nx::vector<u8> leader(size + 1, 0);
    leader[0] = 1;
    for (u32 i = 0; i < size; ++i) {
        const Decoded& d = program_[i];
        if (!d.executed || d.in.op != Op::Br) continue;
        if (d.in.target < 0 || static_cast<u32>(d.in.target) > words_count) {
            word_ = d.word;
            fail("branch outside the program");
            continue;
        }
        leader[start[static_cast<u32>(d.in.target)]] = 1;
        leader[i + 1] = 1;
    }
    if (!branches_) {
        for (usize i = 0; i < size; ++i) emit(i);
        return 0;
    }
    nx::vector<u32> block_of(size + 1, 0);
    nx::vector<u32> firsts;
    for (u32 i = 0; i <= size; ++i) {
        if (i < size && leader[i]) firsts.push_back(i);
        block_of[i] = static_cast<u32>(firsts.size()) - 1;
    }
    const u32 end = static_cast<u32>(firsts.size());
    auto case_of = [&](u32 instruction) { return instruction >= size ? end : block_of[instruction]; };
    const Id pc = local(tu_, "pc");
    const Id steps = local(tu_, "steps");
    const Id header = b_.new_label(), dispatch = b_.new_label(), cont = b_.new_label(), merge = b_.new_label(),
             sw_merge = b_.new_label();
    b_.op0(SpvOpBranch, {header});
    b_.block(header);
    b_.op0(SpvOpLoopMerge, {merge, cont, SpvLoopControlMaskNone});
    b_.op0(SpvOpBranch, {dispatch});
    b_.block(dispatch);
    const Id pcv = load(tu_, pc);
    nx::vector<Id> labels;
    nx::vector<u32> sw{pcv, sw_merge};  // (the default: no block)
    for (u32 k = 0; k < end; ++k) {
        labels.push_back(b_.new_label());
        sw.push_back(k);
        sw.push_back(labels.back());
    }
    b_.op0(SpvOpSelectionMerge, {sw_merge, SpvSelectionControlMaskNone});
    b_.op0(SpvOpSwitch, sw);
    for (u32 k = 0; k < end; ++k) {
        b_.block(labels[k]);
        const u32 last = (k + 1 < end ? firsts[k + 1] : size) - 1;
        for (u32 i = firsts[k]; i <= last; ++i) emit(i);
        const Decoded& d = program_[last];
        Id next = u(k + 1);
        if (d.executed && d.in.op == Op::Br && d.in.target >= 0 && static_cast<u32>(d.in.target) <= words_count) {
            const Id target = u(case_of(start[static_cast<u32>(d.in.target)]));
            next = cond_ ? sel(tu_, cond_, target, next) : target;
        }
        store(pc, next);
        b_.op0(SpvOpBranch, {sw_merge});
    }
    b_.block(sw_merge);
    b_.op0(SpvOpBranch, {cont});
    b_.block(cont);
    const Id count = iadd(load(tu_, steps), u(1));
    store(steps, count);
    const Id more = land(bin(SpvOpULessThan, tb_, load(tu_, pc), u(end)), bin(SpvOpULessThan, tb_, count, u(kMaxSteps)));
    b_.op0(SpvOpBranchConditional, {more, header, merge});
    b_.block(merge);
    return bin(SpvOpUGreaterThanEqual, tb_, count, u(kMaxSteps));
}

// ---- The compute form ------------------------------------------------------------------------------------------

void Translator::compute_prologue(Id gid) {
    const Id in_base = imul(gid, u(ComputeLayout::kIn));
    for (u32 k = 0; k < ComputeLayout::kPrimary; ++k)
        store(chain(tp_fu_, pa_.var, {u(k)}), load(tu_, buffer_word(in_buf_, iadd(in_base, u(k)))));
}

void Translator::compute_epilogue(Id gid, Id stopped) {
    const Id out_base = imul(gid, u(ComputeLayout::kOut));
    for (u32 k = 0; k < ComputeLayout::kPrimary; ++k)
        store(buffer_word(out_buf_, iadd(out_base, u(k))), load(tu_, chain(tp_fu_, pa_.var, {u(k)})));
    for (u32 k = 0; k < ComputeLayout::kOutput; ++k)
        store(buffer_word(out_buf_, iadd(out_base, u(ComputeLayout::kPrimary + k))),
              load(tu_, chain(tp_fu_, out_.var, {u(k)})));
    store(buffer_word(out_buf_, iadd(out_base, u(ComputeLayout::kPrimary + ComputeLayout::kOutput))),
          bor(sel(tu_, load(tb_, killed_), u(1), u(0)), stopped ? sel(tu_, stopped, u(2), u(0)) : u(0)));
}

// ---- The secondary form ----------------------------------------------------------------------------------------

// The draw's secondary attributes (of the stage the push constants name), into the program's registers and back.
void Translator::secondary_prologue() {
    sa_base_ = iadd(imul(push_constant(0, tu_), u(kDrawSecondaryWords)), imul(push_constant(5, tu_), u(256)));
    sa_.minimum = 256;
    for (u32 k = 0; k < 256; ++k)
        store(chain(tp_fu_, sa_.var, {u(k)}), load(tu_, buffer_word(sa_buf_, iadd(sa_base_, u(k)))));
}

void Translator::secondary_epilogue() {
    for (u32 k = 0; k < 256; ++k)
        store(buffer_word(sa_buf_, iadd(sa_base_, u(k))), load(tu_, chain(tp_fu_, sa_.var, {u(k)})));
}

// ---- The vertex form -------------------------------------------------------------------------------------------

u32 location_of(u32 bit) {
    if (bit == usse::varying::kColor0) return kVaryingColor0;
    if (bit == usse::varying::kColor1) return kVaryingColor1;
    if (bit == usse::varying::kFog) return kVaryingFog;
    for (u32 k = 0; k < 10; ++k)
        if (bit == (usse::varying::kTexCoord0 << k)) return kVaryingTexCoord0 + k;
    return ~0u;
}

void Translator::vertex_prologue() {
    sa_base_ = imul(push_constant(0, tu_), u(kDrawSecondaryWords));
    auto pa = [&](u32 r) { return chain(tp_fu_, pa_.var, {u(r)}); };
    const Id tuv4 = b_.t_vector(tu_, 4);
    for (const VertexInput& in : vertex_shader->inputs) {
        const usse::Parameter* p = nullptr;
        for (const usse::Parameter& q : gxp_.parameters)
            if (q.category == usse::ParameterCategory::Attribute && q.resource == in.reg) p = &q;
        if (!p) continue;  // (the program does not read it)
        const VertexRead read = vertex_input_read(in);
        const Id tiv4 = b_.t_vector(ti_, 4);
        const Id type = read == VertexRead::Float ? tv4_ : read == VertexRead::Signed ? tiv4 : tuv4;
        const Id var = b_.variable(b_.t_pointer(SpvStorageClassInput, type), SpvStorageClassInput);
        b_.decorate(var, SpvDecorationLocation, {in.location});
        interface_.push_back(var);
        const Id v = load(type, var);
        // Component k as a float (raw: its bits); past the attribute's components, 0, 0, 0, 1.
        auto comp = [&](u32 k) -> Id {
            if (read == VertexRead::Raw) return b_.op(SpvOpCompositeExtract, tu_, {v, k});
            if (k >= in.components) return f(k == 3 ? 1.0f : 0.0f);
            if (read == VertexRead::Float) return b_.op(SpvOpCompositeExtract, tf_, {v, k});
            const Id x = b_.op(SpvOpCompositeExtract, read == VertexRead::Signed ? ti_ : tu_, {v, k});
            return un(read == VertexRead::Signed ? SpvOpConvertSToF : SpvOpConvertUToF, tf_, x);
        };
        if (read == VertexRead::Raw) {
            // The data's bits, packed into the registers in order.
            const u32 bits = vertex_input_bits(in.format);
            const u32 count = in.components;  // (untyped: 32-bit words)
            const u32 words = (count * bits + 31) / 32;
            nx::vector<Id> w(words, u(0));
            for (u32 k = 0; k < count && k < 4; ++k) {
                const u32 at = k * bits;
                w[at / 32] = bor(w[at / 32], at % 32 ? shl(mask(comp(k), bits), u(at % 32)) : mask(comp(k), bits));
            }
            for (u32 j = 0; j < words; ++j) {
                pa_.used = nx::max(pa_.used, in.reg + j + 1);
                store(pa(in.reg + j), w[j]);
            }
            continue;
        }
        // Converted to the program's type for the attribute (the missing components: 0, 0, 0, 1).
        const u32 n = nx::max<u32>(p->components, 1);
        switch (p->type) {
            case usse::ParameterType::F16:
                for (u32 k = 0; k < n; k += 2) {
                    const Id lo = to_half(cast(tu_, comp(k))), hi = k + 1 < n ? to_half(cast(tu_, comp(k + 1))) : u(0);
                    pa_.used = nx::max(pa_.used, in.reg + k / 2 + 1);
                    store(pa(in.reg + k / 2), bor(lo, shl(hi, u(16))));
                }
                break;
            case usse::ParameterType::U8:
            case usse::ParameterType::S8:
            case usse::ParameterType::U16:
            case usse::ParameterType::S16:
            case usse::ParameterType::U32:
            case usse::ParameterType::S32: {
                const bool sgn = p->type == usse::ParameterType::S8 || p->type == usse::ParameterType::S16 ||
                                 p->type == usse::ParameterType::S32;
                const u32 bits = p->type == usse::ParameterType::U8 || p->type == usse::ParameterType::S8 ? 8
                                 : p->type == usse::ParameterType::U32 || p->type == usse::ParameterType::S32 ? 32 : 16;
                const u32 words = (n * bits + 31) / 32;
                nx::vector<Id> w(words, u(0));
                for (u32 k = 0; k < n; ++k) {
                    const Id x = sgn ? cast(tu_, un(SpvOpConvertFToS, ti_, comp(k))) : un(SpvOpConvertFToU, tu_, comp(k));
                    const u32 at = k * bits;
                    w[at / 32] = bor(w[at / 32], at % 32 ? shl(mask(x, bits), u(at % 32)) : mask(x, bits));
                }
                for (u32 j = 0; j < words; ++j) {
                    pa_.used = nx::max(pa_.used, in.reg + j + 1);
                    store(pa(in.reg + j), w[j]);
                }
                break;
            }
            default:  // F32 (and the formats read as floats)
                for (u32 k = 0; k < n && k < 4; ++k) {
                    pa_.used = nx::max(pa_.used, in.reg + k + 1);
                    store(pa(in.reg + k), cast(tu_, comp(k)));
                }
                break;
        }
    }
    // The vertex and instance indices (the index buffer's value, the instance number).
    for (u32 which = 0; which < 2; ++which) {
        const i32 reg = which ? gxp_.instance_register : gxp_.index_register;
        if (reg < 0) continue;
        const Id var = b_.variable(b_.t_pointer(SpvStorageClassInput, ti_), SpvStorageClassInput);
        b_.decorate(var, SpvDecorationBuiltIn, {which ? static_cast<u32>(SpvBuiltInInstanceIndex) : static_cast<u32>(SpvBuiltInVertexIndex)});
        interface_.push_back(var);
        pa_.used = nx::max(pa_.used, static_cast<u32>(reg) + 1);
        store(pa(static_cast<u32>(reg)), cast(tu_, load(ti_, var)));
    }
}

void Translator::vertex_epilogue() {
    const usse::VertexOutputLayout l = usse::vertex_output_layout(gxp_);
    out_.used = nx::max(out_.used, l.registers);
    auto o = [&](i32 r) { return cast(tf_, load(tu_, chain(tp_fu_, out_.var, {u(static_cast<u32>(r))}))); };
    auto output = [&](Id type, auto&& decorate) {
        const Id var = b_.variable(b_.t_pointer(SpvStorageClassOutput, type), SpvStorageClassOutput);
        decorate(var);
        interface_.push_back(var);
        return var;
    };
    // The position, as GXM's viewport transform puts it on the render target, back in Vulkan's clip space (w kept
    // for clipping and perspective-correct interpolation).
    const Id position = output(tv4_, [&](Id v) { b_.decorate(v, SpvDecorationBuiltIn, {SpvBuiltInPosition}); });
    const Id point_size = output(tf_, [&](Id v) { b_.decorate(v, SpvDecorationBuiltIn, {SpvBuiltInPointSize}); });
    Id cx = f(0.0f), cy = f(0.0f), cz = f(0.0f), cw = f(1.0f);
    if (l.position >= 0) cx = o(l.position), cy = o(l.position + 1), cz = o(l.position + 2), cw = o(l.position + 3);
    const Id clamp = push_constant(4, tf_);
    cw = sel(tf_, land(bin(SpvOpFOrdGreaterThan, tb_, clamp, f(0.0f)), bin(SpvOpFOrdLessThan, tb_, cw, clamp)), clamp, cw);
    auto vp = [&](u32 k) { return load(tf_, chain(tp_push_f_, push_, {u(3), u(k)})); };
    const Id width = load(tf_, chain(tp_push_f_, push_, {u(2), u(0)})), height = load(tf_, chain(tp_push_f_, push_, {u(2), u(1)}));
    const Id flags = push_constant(1, tu_);
    const Id enabled = ine(band(flags, u(kDrawViewport)), u(0));
    const Id vx = fsub(fdiv(fmul(f(2.0f), fadd(fmul(vp(0), cw), fmul(vp(1), cx))), width), cw);
    const Id vy = fsub(fdiv(fmul(f(2.0f), fadd(fmul(vp(2), cw), fmul(vp(3), cy))), height), cw);
    const Id vz = fadd(fmul(vp(4), cw), fmul(vp(5), cz));
    const Id sx = fsub(fdiv(fmul(f(2.0f), cx), width), f(1.0f));
    const Id sy = fsub(fdiv(fmul(f(2.0f), cy), height), f(1.0f));
    const Id pos = b_.op(SpvOpCompositeConstruct, tv4_,
                         {sel(tf_, enabled, vx, sx), sel(tf_, enabled, vy, sy), sel(tf_, enabled, vz, cz),
                          sel(tf_, enabled, cw, f(1.0f))});
    store(position, pos);
    store(point_size, l.point_size >= 0 ? o(l.point_size) : f(1.0f));
    // The varyings: four components, the missing ones 0, 0, 0, 1.
    auto varying = [&](u32 location, i32 first, u32 count) {
        if (first < 0) return;
        const Id var = output(tv4_, [&](Id v) { b_.decorate(v, SpvDecorationLocation, {location}); });
        Id c[4] = {f(0.0f), f(0.0f), f(0.0f), f(1.0f)};
        for (u32 k = 0; k < count && k < 4; ++k) c[k] = o(first + static_cast<i32>(k));
        store(var, b_.op(SpvOpCompositeConstruct, tv4_, {c[0], c[1], c[2], c[3]}));
    };
    varying(kVaryingColor0, l.color[0], 4);
    varying(kVaryingColor1, l.color[1], 4);
    varying(kVaryingFog, l.fog, 2);
    for (u32 k = 0; k < 10; ++k) varying(kVaryingTexCoord0 + k, l.texcoord[k], l.texcoord_registers[k]);
}

// ---- The fragment form -----------------------------------------------------------------------------------------

// The fragment form's special registers: the pixel's position in its 16 x 16 tile (pixelX, pixelY) and the tile's
// (tileXY: x in the low byte, y in the next), as the shader compiler's __pixel_x, __pixel_y and __tile_xy address a
// linear surface (16 * tile + pixel). The others read as zero.
Id Translator::frag_coord() {
    if (!frag_coord_) {
        frag_coord_ = b_.variable(b_.t_pointer(SpvStorageClassInput, tv4_), SpvStorageClassInput);
        b_.decorate(frag_coord_, SpvDecorationBuiltIn, {SpvBuiltInFragCoord});
        interface_.push_back(frag_coord_);
    }
    return frag_coord_;
}

Id Translator::special(i32 code) {
    if (form_ != Form::Fragment || (code != 87 && code != 88 && code != 107)) return u(0);
    const Id fc = load(tv4_, frag_coord());
    auto coord = [&](u32 k) { return un(SpvOpConvertFToU, tu_, b_.op(SpvOpCompositeExtract, tf_, {fc, k})); };
    if (code == 87) return band(coord(0), u(15));
    if (code == 88) return band(coord(1), u(15));
    return bor(band(shr(coord(0), u(4)), u(0xFF)), shl(band(shr(coord(1), u(4)), u(0xFF)), u(8)));
}

void Translator::fragment_prologue() {
    sa_base_ = iadd(imul(push_constant(0, tu_), u(kDrawSecondaryWords)), u(256));
    if (!inline_secondary.empty()) {
        // (the secondary attributes, privately: the inline secondary program writes them)
        sa_.minimum = 256;
        for (u32 k = 0; k < 256; ++k)
            store(chain(tp_fu_, sa_.var, {u(k)}), load(tu_, buffer_word(sa_buf_, iadd(sa_base_, u(k)))));
    }
    if (gxp_.native_color && gxp_.frag_color) {
        // The colour attachment's value, back in the output register's lanes and format.
        b_.capability(SpvCapabilityInputAttachment);
        const Id image = b_.t_image(tf_, SpvDimSubpassData, 0, 0, 0, 2, SpvImageFormatUnknown);
        const Id var = b_.variable(b_.t_pointer(SpvStorageClassUniformConstant, image), SpvStorageClassUniformConstant);
        b_.decorate(var, SpvDecorationDescriptorSet, {0});
        b_.decorate(var, SpvDecorationBinding, {kDrawBindingColor});
        b_.decorate(var, SpvDecorationInputAttachmentIndex, {0});
        const Id v2i = b_.t_vector(ti_, 2);
        const Id texel = b_.op(SpvOpImageRead, tv4_, {load(image, var), b_.c_composite(v2i, {s(0), s(0)})});
        Id lane[4] = {f(0.0f), f(0.0f), f(0.0f), f(1.0f)};
        for (u32 c = 0; c < 4; ++c) {
            const u8 k = fragment_shader->swizzle[c];
            if (k < 4) lane[k] = b_.op(SpvOpCompositeExtract, tf_, {texel, c});
        }
        auto o = [&](u32 r, Id v) {
            out_.used = nx::max(out_.used, r + 1);
            store(chain(tp_fu_, out_.var, {u(r)}), v);
        };
        auto unorm8 = [&](Id x) {
            const Id c = ext(tf_, GLSLstd450FClamp, {x, f(0.0f), f(1.0f)});
            return cast(tu_, un(SpvOpConvertFToS, ti_, ext(tf_, GLSLstd450RoundEven, {fmul(c, f(255.0f))})));
        };
        switch (gxp_.output_type) {
            case usse::ParameterType::U8:
                o(0, bor(bor(unorm8(lane[0]), shl(unorm8(lane[1]), u(8))), bor(shl(unorm8(lane[2]), u(16)), shl(unorm8(lane[3]), u(24)))));
                break;
            case usse::ParameterType::F16:
                o(0, bor(to_half(cast(tu_, lane[0])), shl(to_half(cast(tu_, lane[1])), u(16))));
                o(1, bor(to_half(cast(tu_, lane[2])), shl(to_half(cast(tu_, lane[3])), u(16))));
                break;
            case usse::ParameterType::F32:
                for (u32 k = 0; k < nx::max<u32>(gxp_.output_components, 1); ++k) o(k, cast(tu_, lane[k]));
                break;
            default: fail("a colour read in this output format not implemented"); break;
        }
    }
    auto pa = [&](u32 r, Id v) {
        pa_.used = nx::max(pa_.used, r + 1);
        store(chain(tp_fu_, pa_.var, {u(r)}), v);
    };
    nx::hash_map<u32, Id> sources;  // varying bit -> its value (vec4)
    auto source = [&](u32 bit) -> Id {
        if (auto it = sources.find(bit); it != sources.end()) return it->second;
        Id value;
        if (bit == usse::varying::kPosition || bit == usse::varying::kSpriteCoord) {
            const bool coord = bit == usse::varying::kSpriteCoord;
            const Id type = coord ? b_.t_vector(tf_, 2) : tv4_;
            Id var = 0;
            if (coord) {
                var = b_.variable(b_.t_pointer(SpvStorageClassInput, type), SpvStorageClassInput);
                b_.decorate(var, SpvDecorationBuiltIn, {SpvBuiltInPointCoord});
                interface_.push_back(var);
            } else {
                var = frag_coord();
            }
            value = load(type, var);
            if (coord) {
                const Id x = b_.op(SpvOpCompositeExtract, tf_, {value, 0}), y = b_.op(SpvOpCompositeExtract, tf_, {value, 1});
                value = b_.op(SpvOpCompositeConstruct, tv4_, {x, y, f(0.0f), f(1.0f)});
            }
        } else {
            const u32 location = location_of(bit);
            if (location == ~0u) {
                fail(nx::format("fragment input of varying 0x{:x} not implemented", bit));
                return b_.c_null(tv4_);
            }
            const Id var = b_.variable(b_.t_pointer(SpvStorageClassInput, tv4_), SpvStorageClassInput);
            b_.decorate(var, SpvDecorationLocation, {location});
            interface_.push_back(var);
            value = load(tv4_, var);
        }
        sources[bit] = value;
        return value;
    };
    for (const usse::FragmentInput& in : gxp_.inputs) {
        if (in.interpolated) {
            const Id v = source(in.interpolated);
            auto c = [&](u32 k) { return b_.op(SpvOpCompositeExtract, tf_, {v, k}); };
            const u32 n = nx::min<u32>(in.components, 4);
            if (in.format == 0) {
                for (u32 k = 0; k < n; ++k) pa(in.offset + k, cast(tu_, c(k)));
            } else if (in.format == 2) {
                for (u32 k = 0; k < n; k += 2)
                    pa(in.offset + k / 2, bor(to_half(cast(tu_, c(k))), k + 1 < n ? shl(to_half(cast(tu_, c(k + 1))), u(16)) : u(0)));
            } else {  // fx10: x, y, z in one register, w in the next (1.0 = 255)
                Id q[4] = {u(0), u(0), u(0), u(0)};
                for (u32 k = 0; k < n; ++k) {
                    Id g = ext(tf_, GLSLstd450FClamp, {fmul(c(k), f(255.0f)), f(-512.0f), f(511.0f)});
                    q[k] = fx_raw(un(SpvOpConvertFToS, ti_, ext(tf_, GLSLstd450RoundEven, {g})), Type::Fx10);
                }
                pa(in.offset, bor(bor(q[0], shl(q[1], u(10))), shl(q[2], u(20))));
                pa(in.offset + 1, q[3]);
            }
        }
        if (in.sampled) {
            const Id v = source(in.sampled);
            Id coord[4];
            for (u32 k = 0; k < 4; ++k) coord[k] = b_.op(SpvOpCompositeExtract, tf_, {v, k});
            if (in.projected)
                for (u32 k = 0; k < 2; ++k) coord[k] = fdiv(coord[k], coord[3]);
            bool cube = false;
            for (const usse::Parameter& q : gxp_.parameters)
                if (q.category == usse::ParameterCategory::Sampler && q.resource == in.sampler) cube = q.sampler_cube;
            const Id extra[4] = {f(0.0f), f(0.0f), f(0.0f), f(0.0f)};
            Id rgba[4];
            // (before the program: uniform control flow, implicit levels of detail are defined)
            const bool saved = branches_;
            branches_ = false;
            sample_unit(kDrawBindingFragmentTextures + in.sampler, cube ? TexDim::Cube : TexDim::D2, coord,
                        usse::TexKind::Plain, extra, rgba);
            branches_ = saved;
            Id regs[4];
            write_sample(rgba, in.sample_format, regs);
            const u32 count = in.sample_format == 0 ? 1u : in.sample_format == 1 ? 2u : 4u;
            for (u32 k = 0; k < count; ++k) pa(in.sample_offset + k, regs[k]);
        }
    }
}

void Translator::fragment_epilogue() {
    // The output register's lanes, in the type the program declares for its output (the patcher converts that to
    // the format the fragment program was made for, as the attachment's format does here).
    u32 format = 0;
    {
        switch (gxp_.output_type) {
            case usse::ParameterType::F32: format = gxp_.output_components <= 1 ? 8 : 7; break;
            case usse::ParameterType::U8: format = 1; break;
            case usse::ParameterType::S8: format = 2; break;
            case usse::ParameterType::U16: format = 3; break;
            case usse::ParameterType::S16: format = 4; break;
            default: format = gxp_.output_components <= 2 ? 6 : 5; break;  // F16
        }
    }
    // (programs with __nativecolor leave their colour in the output registers, the others in the primary
    // attributes: checked in the shader compiler's programs)
    BankVar& bank = gxp_.native_color ? out_ : pa_;
    auto p = [&](u32 r) {
        bank.used = nx::max(bank.used, r + 1);
        return load(tu_, chain(tp_fu_, bank.var, {u(r)}));
    };
    Id lane[6] = {f(0.0f), f(0.0f), f(0.0f), f(1.0f), f(0.0f), f(1.0f)};
    auto half = [&](Id w, u32 k) { return from_half(mask(k ? shr(w, u(16)) : w, 16)); };
    switch (format) {
        case 1:  // UCHAR4
        case 2: {  // CHAR4
            const Id w = p(0);
            for (u32 k = 0; k < 4; ++k) {
                const Id b = k ? shr(w, u(8 * k)) : w;
                lane[k] = format == 1 ? fdiv(un(SpvOpConvertUToF, tf_, band(b, u(0xFF))), f(255.0f))
                                      : ext(tf_, GLSLstd450FMax, {fdiv(un(SpvOpConvertSToF, tf_, sext(b, 8)), f(127.0f)), f(-1.0f)});
            }
            break;
        }
        case 3:  // USHORT2
        case 4: {  // SHORT2
            const Id w = p(0);
            for (u32 k = 0; k < 2; ++k) {
                const Id h = k ? shr(w, u(16)) : w;
                lane[k] = format == 3 ? fdiv(un(SpvOpConvertUToF, tf_, band(h, u(0xFFFF))), f(65535.0f))
                                      : ext(tf_, GLSLstd450FMax, {fdiv(un(SpvOpConvertSToF, tf_, sext(h, 16)), f(32767.0f)), f(-1.0f)});
            }
            break;
        }
        case 5:  // HALF4
            lane[0] = half(p(0), 0), lane[1] = half(p(0), 1), lane[2] = half(p(1), 0), lane[3] = half(p(1), 1);
            break;
        case 6:  // HALF2
            lane[0] = half(p(0), 0), lane[1] = half(p(0), 1);
            break;
        case 7:  // FLOAT2
            lane[0] = cast(tf_, p(0)), lane[1] = cast(tf_, p(1));
            break;
        case 8:  // FLOAT
            lane[0] = cast(tf_, p(0));
            break;
        default: fail(nx::format("output register format {} not implemented", format)); break;
    }
    if (fragment_shader->color_output) {
        const Id color = b_.variable(b_.t_pointer(SpvStorageClassOutput, tv4_), SpvStorageClassOutput);
        b_.decorate(color, SpvDecorationLocation, {0});
        interface_.push_back(color);
        const u8* z = fragment_shader->swizzle;
        store(color, b_.op(SpvOpCompositeConstruct, tv4_, {lane[nx::min<u8>(z[0], 5)], lane[nx::min<u8>(z[1], 5)],
                                                          lane[nx::min<u8>(z[2], 5)], lane[nx::min<u8>(z[3], 5)]}));
    }
    // A discarded instance: no colour, no depth.
    const Id kill = b_.new_label(), merge = b_.new_label();
    const Id killed = load(tb_, killed_);
    b_.op0(SpvOpSelectionMerge, {merge, SpvSelectionControlMaskNone});
    b_.op0(SpvOpBranchConditional, {killed, kill, merge});
    b_.block(kill);
    b_.op0(SpvOpKill, {});
    b_.block(merge);
}

// ---- The module ------------------------------------------------------------------------------------------------

Result<nx::vector<u32>> Translator::translate() {
    b_.capability(SpvCapabilityShader);
    tu_ = b_.t_uint();
    ti_ = b_.t_int();
    tf_ = b_.t_float();
    tb_ = b_.t_bool();
    tv4_ = b_.t_vector(tf_, 4);
    tp_fu_ = b_.t_pointer(SpvStorageClassFunction, tu_);
    tp_fi_ = b_.t_pointer(SpvStorageClassFunction, ti_);
    tp_fb_ = b_.t_pointer(SpvStorageClassFunction, tb_);
    tp_su_ = b_.t_pointer(SpvStorageClassStorageBuffer, tu_);
    for (const Decoded& d : program_) {
        branches_ |= d.executed && d.in.op == Op::Br;
        stores_ |= d.executed && d.in.op == Op::Store;
    }
    for (const Decoded& d : inline_secondary) stores_ |= d.executed && d.in.op == Op::Store;

    // The storage buffers: one struct of a runtime array of words.
    const Id words = b_.t_runtime_array(tu_);
    b_.decorate(words, SpvDecorationArrayStride, {4});
    const Id block = b_.t_struct({words});
    b_.decorate(block, SpvDecorationBlock);
    b_.member_decorate(block, 0, SpvDecorationOffset, {0});
    // (a draw's shaders only read their buffers unless they store: storing needs a device feature, the vertex and
    // fragment stages' stores)
    if ((form_ == Form::Vertex || form_ == Form::Fragment) && !stores_) b_.member_decorate(block, 0, SpvDecorationNonWritable);
    const Id tp_block = b_.t_pointer(SpvStorageClassStorageBuffer, block);
    auto buffer = [&](u32 binding, const char* name) {
        const Id v = b_.variable(tp_block, SpvStorageClassStorageBuffer);
        b_.decorate(v, SpvDecorationDescriptorSet, {0});
        b_.decorate(v, SpvDecorationBinding, {binding});
        b_.name(v, name);
        return v;
    };
    Id gid_var = 0;
    const Id tv3 = b_.t_vector(tu_, 3);
    if (form_ == Form::Compute) {
        in_buf_ = buffer(kBindingIn, "in");
        sa_buf_ = buffer(kBindingSecondary, "sa");
        out_buf_ = buffer(kBindingOut, "out");
        tex_buf_ = buffer(kBindingTextures, "textures");
        mem_buf_ = buffer(kBindingMemory, "memory");
        gid_var = b_.variable(b_.t_pointer(SpvStorageClassInput, tv3), SpvStorageClassInput);
        b_.decorate(gid_var, SpvDecorationBuiltIn, {SpvBuiltInGlobalInvocationId});
        interface_.push_back(gid_var);
    } else {
        sa_buf_ = buffer(kDrawBindingSecondary, "sa");
        mem_buf_ = buffer(kDrawBindingMemory, "memory");
        // The draw's push constants (DrawConstants).
        const Id f2 = b_.t_array(tf_, 2), f6 = b_.t_array(tf_, 6);
        b_.decorate(f2, SpvDecorationArrayStride, {4});
        b_.decorate(f6, SpvDecorationArrayStride, {4});
        const Id pc = b_.t_struct({tu_, tu_, f2, f6, tf_, tu_});
        b_.decorate(pc, SpvDecorationBlock);
        const u32 offsets[] = {0, 4, 8, 16, 40, 44};
        for (u32 m = 0; m < 6; ++m) b_.member_decorate(pc, m, SpvDecorationOffset, {offsets[m]});
        push_ = b_.variable(b_.t_pointer(SpvStorageClassPushConstant, pc), SpvStorageClassPushConstant);
        b_.name(push_, "draw");
        tp_push_f_ = b_.t_pointer(SpvStorageClassPushConstant, tf_);
        tp_push_u_ = b_.t_pointer(SpvStorageClassPushConstant, tu_);
    }

    const Id tvoid = b_.t_void();
    const Id fn = b_.begin_function(tvoid, b_.t_function(tvoid));
    b_.name(fn, "main");
    b_.block(b_.new_label());

    // The registers. Arrays are created at the end, when their sizes are known; the others now.
    temp_.var = b_.reserve();
    out_.var = b_.reserve();
    pa_.var = b_.reserve();
    sa_.var = b_.reserve();
    if (form_ == Form::Compute) {
        out_.minimum = ComputeLayout::kOutput;
        pa_.minimum = ComputeLayout::kPrimary;
    }
    b_.name(temp_.var, "r");
    b_.name(out_.var, "o");
    b_.name(pa_.var, "pa");
    b_.name(sa_.var, "sa");
    auto local = [&](Id type, const char* name) {
        const Id id = b_.reserve();
        b_.local(id, b_.t_pointer(SpvStorageClassFunction, type), b_.c_null(type));
        b_.name(id, name);
        return id;
    };
    internal_ = local(b_.t_array(tu_, 16), "i");
    index_ = local(b_.t_array(ti_, 2), "index");
    pred_ = local(b_.t_array(tb_, 4), "p");
    killed_ = local(tb_, "killed");
    link_ = local(tu_, "link");

    Id gid = 0;
    switch (form_) {
        case Form::Compute:
            gid = b_.op(SpvOpCompositeExtract, tu_, {load(tv3, gid_var), 0});
            compute_prologue(gid);
            break;
        case Form::Secondary: secondary_prologue(); break;
        case Form::Vertex: vertex_prologue(); break;
        case Form::Fragment: fragment_prologue(); break;
    }
    if (form_ == Form::Fragment && !inline_secondary.empty()) {
        // The secondary program first (its own blocks and branches), then the primary.
        const std::span<const Decoded> primary = program_;
        const bool primary_branches = branches_;
        program_ = inline_secondary;
        branches_ = false;
        for (const Decoded& d : program_) branches_ |= d.executed && d.in.op == Op::Br;
        in_secondary_ = true;
        (void)body(local);
        in_secondary_ = false;
        program_ = primary;
        branches_ = primary_branches;
    }
    const Id stopped = body(local);
    switch (form_) {
        case Form::Compute: compute_epilogue(gid, stopped); break;
        case Form::Secondary: secondary_epilogue(); break;
        case Form::Vertex: vertex_epilogue(); break;
        case Form::Fragment: fragment_epilogue(); break;
    }
    b_.op0(SpvOpReturn, {});
    b_.end_function();

    for (BankVar* bank : {&temp_, &out_, &pa_, &sa_}) {
        const u32 n = bank->dynamic ? kBankSize : nx::max(nx::max(bank->used, bank->minimum), 1u);
        const Id type = b_.t_array(tu_, n);
        b_.local(bank->var, b_.t_pointer(SpvStorageClassFunction, type), b_.c_null(type));
    }
    switch (form_) {
        case Form::Compute:
        case Form::Secondary:
            b_.entry_point(SpvExecutionModelGLCompute, fn, "main", interface_);
            b_.execution_mode(fn, SpvExecutionModeLocalSize, {1, 1, 1});
            break;
        case Form::Vertex: b_.entry_point(SpvExecutionModelVertex, fn, "main", interface_); break;
        case Form::Fragment:
            b_.entry_point(SpvExecutionModelFragment, fn, "main", interface_);
            b_.execution_mode(fn, SpvExecutionModeOriginUpperLeft);
            break;
    }
    if (int64_) b_.capability(SpvCapabilityInt64);
    if (image_query_) b_.capability(SpvCapabilityImageQuery);
    if (!error_.empty()) return failure(error_);
    return b_.module();
}

}  // namespace

VertexRead vertex_input_read(const VertexInput& in) {
    if (in.raw || in.format > 9) return VertexRead::Raw;
    if (in.format == 0 || in.format == 2) return VertexRead::Unsigned;
    if (in.format == 1 || in.format == 3) return VertexRead::Signed;
    return VertexRead::Float;
}

u32 vertex_input_bits(u8 format) {
    switch (format) {
        case 0: case 1: case 4: case 5: return 8;    // U8, S8, U8N, S8N
        case 2: case 3: case 6: case 7: case 8: return 16;  // U16, S16, U16N, S16N, F16
        default: return 32;                          // F32, untyped
    }
}

Result<nx::vector<u32>> translate_compute(const usse::Gxp& gxp, std::span<const usse::Decoded> program) {
    return Translator(Form::Compute, gxp, program).translate();
}

Result<nx::vector<u32>> translate_secondary(const usse::Gxp& gxp, std::span<const usse::Decoded> program) {
    return Translator(Form::Secondary, gxp, program).translate();
}

Result<nx::vector<u32>> translate_vertex(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                         const VertexShader& shader) {
    Translator t(Form::Vertex, gxp, program);
    t.vertex_shader = &shader;
    return t.translate();
}

Result<nx::vector<u32>> translate_fragment(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                           const FragmentShader& shader, std::span<const usse::Decoded> secondary) {
    Translator t(Form::Fragment, gxp, program);
    t.fragment_shader = &shader;
    t.inline_secondary = secondary;
    return t.translate();
}

void fill_secondary(const usse::Gxp& gxp, std::span<const u32> default_uniforms, std::span<const u32> buffer_addresses,
                    std::span<u32, 256> out) {
    std::fill(out.begin(), out.end(), 0u);
    if (const usse::Container* c = gxp.container(usse::kDefaultUniformContainer))
        for (u32 i = 0; i < c->size && c->base + i < 256; ++i) out[c->base + i] = i < default_uniforms.size() ? default_uniforms[i] : 0;
    const usse::Container* k = gxp.container(usse::kConstantsContainer);
    if (!k) return;
    for (const usse::BufferSlot& s : gxp.buffer_slots)
        if (k->base + s.offset < 256u)
            out[k->base + s.offset] =
                s.buffer < buffer_addresses.size() ? buffer_addresses[s.buffer] + static_cast<u32>(s.bias) : 0;
    for (const usse::Literal& l : gxp.literals)
        if (k->base + l.offset < 256u) out[k->base + l.offset] = l.value;
}

}  // namespace psvrecore::gxm
