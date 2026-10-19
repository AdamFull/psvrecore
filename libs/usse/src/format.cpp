#include "psvrecore/usse/format.hpp"

#include "constants.hpp"
#include <algorithm>

#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::usse {
namespace {

using detail::kConstantF16Text;
using detail::kConstantF32Text;
using detail::kConstantWords;

const char* type_name(Type t) {
    switch (t) {
        case Type::F32: return "f32";
        case Type::F16: return "f16";
        case Type::Fx10: return "fx10";
        case Type::Fx8: return "fx8";
        case Type::I8: return "i8";
        case Type::I16: return "i16";
        case Type::I32: return "i32";
        case Type::U8: return "u8";
        case Type::S8: return "s8";
        case Type::O8: return "o8";
        case Type::U16: return "u16";
        case Type::S16: return "s16";
        case Type::U32: return "u32";
        default: return "";
    }
}

const char* pred_text(Pred p) {
    switch (p) {
        case Pred::P0: return "p0 ";
        case Pred::P1: return "p1 ";
        case Pred::P2: return "p2 ";
        case Pred::P3: return "p3 ";
        case Pred::NotP0: return "!p0 ";
        case Pred::NotP1: return "!p1 ";
        case Pred::NotP2: return "!p2 ";
        case Pred::Pn: return "Pn ";
        default: return "";
    }
}

const char* cond_text(Cond c) {
    static const char* const names[] = {"always", "eqzero", "nezero", "poszero", "lezero",
                                        "ltzero", "negzero", "gezero", "gtzero"};
    return names[static_cast<u32>(c)];
}

char comp_char(Comp c) { return "xyzw012h"[static_cast<u32>(c)]; }
bool is_constant(Comp c) { return c >= Comp::Zero; }
const char* constant_text(Comp c) {
    switch (c) {
        case Comp::Zero: return "0";
        case Comp::One: return "1";
        case Comp::Two: return "2";
        default: return "0.5";
    }
}

const char* bank_name(Bank b, ProgramKind kind) {
    switch (b) {
        case Bank::Temp: return "r";
        case Bank::Output: return "o";
        case Bank::Primary: return kind == ProgramKind::Secondary ? "sa" : "pa";
        case Bank::Secondary: return "sa";
        default: return "?";
    }
}

const char* special_name(i32 code) {
    static const char* const named[] = {
        "backFaceControl", "pipeNumber", "outputBase", "sequenceNumber", "invalidInstanceMask", "taskId",
        "slotNumber", "pixelX", "pixelY", "instanceNumberInSlot", "moeIteration", "taskQueue0", "taskQueue1",
        "taskQueue2", "taskQueue3", "taskQueue4", "taskQueue5", "taskQueue6", "taskType"};
    static const char* const named2[] = {"edgeMask0", "edgeMask1", "edgeMask2", "edgeMask3", "instanceNumberInTask",
                                         "secondaryAttributeSize", "batchNumber", "tileXY"};
    if (code >= 80 && code <= 98) return named[code - 80];
    if (code >= 100 && code <= 107) return named2[code - 100];
    if (code == 109) return "coreNumber";
    return nullptr;
}

nx::string lane_suffix(const Operand& o) {
    nx::string s;
    for (u8 i = 0; i < o.lanes; ++i) s += ((o.mask >> i) & 1) ? comp_char(o.swizzle[i]) : '-';
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? nx::string() : "." + s;
}

// A list of lane values in braces: disabled lanes print 0, trailing disabled lanes are dropped.
template <class F>
nx::string brace_lanes(const Operand& o, F value) {
    nx::vector<nx::string> vals;
    for (u8 i = 0; i < o.lanes; ++i) vals.push_back(((o.mask >> i) & 1) ? value(i) : nx::string("0"));
    while (!vals.empty() && !((o.mask >> (vals.size() - 1)) & 1)) vals.pop_back();
    nx::string s = "{";
    for (usize i = 0; i < vals.size(); ++i) s += (i ? ", " : "") + vals[i];
    return s + "}";
}

const char* part_text(Part p) {
    switch (p) {
        case Part::Lo16: return ".lo16";
        case Part::Hi16: return ".hi16";
        case Part::Lo8: return ".lo8";
        case Part::Hi8: return ".hi8";
        default: return "";
    }
}

// The operand without its modifiers.
nx::string operand_body(const Operand& o, ProgramKind kind) {
    // Lanes that all select constants print as an immediate.
    bool any = false, all_const = true;
    for (u8 i = 0; i < o.lanes; ++i)
        if ((o.mask >> i) & 1) any = true, all_const = all_const && is_constant(o.swizzle[i]);
    if (any && all_const && o.bank != Bank::Predicate && o.bank != Bank::Index)
        return brace_lanes(o, [&](u8 i) { return nx::string(constant_text(o.swizzle[i])); });
    switch (o.bank) {
        case Bank::Constant: {
            if (o.view == View::Int) return nx::format("{}", kConstantWords[2 * o.number]);
            return brace_lanes(o, [&](u8 i) {
                const Comp c = o.swizzle[i];
                if (is_constant(c)) return nx::string(constant_text(c));
                const u32 ci = static_cast<u32>(c);
                if (o.view == View::F16) return nx::string(kConstantF16Text[(2 * o.number + (ci >> 1)) * 2 + (ci & 1)]);
                return nx::string(kConstantF32Text[2 * o.number + ci]);
            });
        }
        case Bank::Special: {
            const char* name = special_name(o.number);
            return (name ? nx::string(name) : nx::format("g{}", o.number - 64)) + lane_suffix(o);
        }
        case Bank::Literal: return nx::format("0x{:x}", static_cast<u32>(o.number));
        case Bank::Internal: return nx::format("i{}", o.number) + lane_suffix(o);
        case Bank::Index: return nx::format("index{}", o.number);
        case Bank::Predicate: return nx::format("p{}", o.number);
        case Bank::None: return "?";
        default:
            if (o.indexed)
                return nx::format("{}[index{}{} + {}]", bank_name(o.bank, kind), o.indexed, o.index_scaled ? " * 2" : "",
                                  o.number) +
                       lane_suffix(o);
            return nx::format("{}{}", bank_name(o.bank, kind), o.number) + lane_suffix(o);
    }
}

nx::string with_modifiers(nx::string t, bool negate, bool absolute) {
    if (absolute) t = "abs(" + t + ")";
    if (negate) t = "-" + t;
    return t;
}

nx::string line(const nx::string& mnemonic, std::initializer_list<nx::string> operands) {
    nx::string s = mnemonic;
    while (s.size() < 15) s += ' ';
    s += ' ';
    bool first = true;
    for (const nx::string& o : operands) {
        if (!first) s += ", ";
        s += o;
        first = false;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

nx::string line_n(const nx::string& mnemonic, const nx::vector<nx::string>& operands) {
    nx::string s = mnemonic;
    while (s.size() < 15) s += ' ';
    s += ' ';
    for (usize i = 0; i < operands.size(); ++i) s += (i ? ", " : "") + operands[i];
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// The sums of products: a term is factor * source, simplified when the factor is 0 or 1.
struct SopText {
    nx::string a, b, s0;  // the sources
    bool alpha_line = false;

    nx::string alpha(const nx::string& t) const {
        if (alpha_line || t.empty() || (t[0] >= '0' && t[0] <= '9') || t[0] == '{') return t;
        const usize bracket = t.find(']');
        if (bracket != nx::string::npos) return t.substr(0, bracket + 1) + ".www";
        const usize dot = t.find('.');
        return (dot == nx::string::npos ? t : t.substr(0, dot)) + ".www";
    }
    nx::string factor(Factor f) const {
        switch (f) {
            case Factor::Sat: return "min(" + alpha(a) + ",1.0 - " + alpha(b) + ")";
            case Factor::A: return a;
            case Factor::AAlpha: return alpha(a);
            case Factor::S0: return s0;
            case Factor::S0Alpha: return alpha(s0);
            case Factor::B: return b;
            case Factor::BAlpha: return alpha(b);
            default: return "?";
        }
    }
    nx::string term(const nx::string& x, const SopTerm& t) const {
        if (t.factor == Factor::Zero) return t.complement ? x : nx::string("0");
        return t.complement ? "(1.0 - " + factor(t.factor) + ") * " + x : factor(t.factor) + " * " + x;
    }
};

nx::string format_sop(const Instruction& in, ProgramKind kind) {
    SopText st;
    st.a = format_operand(in.src[0], kind);
    st.b = format_operand(in.src[1], kind);
    if (in.op == Op::Sop2) st.s0 = format_operand(in.src[2], kind);
    st.alpha_line = (in.flags & kCoissued) != 0;
    nx::string d = format_operand(in.dest, kind);
    if (in.op == Op::Sop && in.dest.mask == 0) d += ".----";
    const nx::string ty = type_name(in.type);
    const nx::string x = in.complement_a ? "(1.0 - " + st.a + ")" : st.a;
    const SopTerm& t0 = in.term[0];
    const SopTerm& t1 = in.term[1];
    if (in.sop_op == Op::Min || in.sop_op == Op::Max) return line(nx::format("{}.{}", mnemonic(in.sop_op), ty), {d, x, st.b});
    if (!t0.complement && !t1.complement &&
        ((t0.factor == Factor::B && t1.factor == Factor::Zero) || (t0.factor == Factor::Zero && t1.factor == Factor::A)))
        return line("mul." + ty, {d, x, st.b});
    return line(nx::format("{}.{}", mnemonic(in.sop_op), ty), {d, st.term(x, t0), st.term(st.b, t1)});
}

nx::string format_pack(const Instruction& in, ProgramKind kind) {
    nx::string m = in.op == Op::Mov ? nx::format("mov.{}", type_name(in.type))
                                    : nx::format("pack.{}.{}", type_name(in.type), type_name(in.src_type));
    if (in.flags & kScale) m += ".scl";
    nx::string s;
    if (in.flags & kPairSource) {
        Operand a = in.src[0], b = in.src[1];
        a.lanes = 1, a.mask = in.src[0].mask & 1, a.swizzle[0] = in.src[0].swizzle[0];
        b.lanes = 1, b.mask = (in.src[0].mask >> 1) & 1, b.swizzle[0] = static_cast<Comp>(static_cast<u32>(in.src[0].swizzle[1]) & 1);
        s = "(" + format_operand(a, kind) + ", " + format_operand(b, kind) + ")";
    } else if (in.flags & kSameHalves) {
        Operand a = in.src[0];
        a.lanes = 2, a.mask &= 3;
        s = format_operand(a, kind);
    } else {
        s = format_operand(in.src[0], kind);
    }
    return line(m, {format_operand(in.dest, kind), s});
}

nx::string test_mnemonic(const Instruction& in) {
    nx::string m;
    if (in.alu == Op::Cmp) {
        switch (in.cond) {
            case Cond::EqZero: m = "cmp.eq"; break;
            case Cond::NeZero: m = "cmp.ne"; break;
            case Cond::LeZero: m = "cmp.le"; break;
            case Cond::LtZero: m = "cmp.lt"; break;
            case Cond::GeZero: m = "cmp.ge"; break;
            case Cond::GtZero: m = "cmp.gt"; break;
            default: m = nx::format("sub.{}", cond_text(in.cond)); break;
        }
    } else {
        m = nx::format("{}.{}", mnemonic(in.alu), cond_text(in.cond));
    }
    switch (in.test_mode) {
        case TestMode::AndAll: m += ".andall"; break;
        case TestMode::OrAll: m += ".orall"; break;
        case TestMode::Or2: m += ".or2"; break;
        default: break;
    }
    return m + "." + type_name(in.type);
}

nx::string tex_mnemonic(const Instruction& in) {
    static const char* const dims[] = {"1D", "2D", "Cube"};
    static const char* const kinds[] = {"", "Bias", "Replace", "Grad"};
    nx::string m = nx::format("tex{}{}", dims[static_cast<u32>(in.tex_dim)], kinds[static_cast<u32>(in.tex_kind)]);
    const bool rsd = in.flags & kTexRsd, info = in.flags & kTexInfo;
    if (rsd && info) m += ".irsd";
    else if (rsd) m += ".rsd";
    else if (info) m += ".info";
    if (in.flags & kTexMinP) m += ".minp";
    if (in.type != Type::None) m += nx::format(".{}", type_name(in.type));
    return m;
}

nx::string memory_text(const Instruction& in, ProgramKind kind) {
    static const char* const spaces[2][3] = {{"lda", "ldl", "ldt"}, {"sta", "stl", "stt"}};
    nx::string m = nx::format("{}{}", spaces[in.op == Op::Store][static_cast<u32>(in.space)], in.size);
    if (in.fetch) m += nx::format(".fetch{}", in.fetch);
    nx::string off = format_operand(in.src[1], kind);
    const char* step = (in.flags & kDecrement) ? "--" : "++";
    if (in.flags & kPreIncrement) off = step + off;
    if (in.flags & kPostIncrement) off = off + step;
    const nx::string addr = "[" + format_operand(in.src[0], kind) + ", " + off + "]";
    if (in.op == Op::Store) return line(m, {addr, format_operand(in.src[2], kind)});
    return line(m, {format_operand(in.dest, kind), addr});
}

}  // namespace

nx::string format_operand(const Operand& o, ProgramKind kind) {
    return with_modifiers(operand_body(o, kind) + part_text(o.part), o.negate, o.absolute);
}

nx::string format(const Instruction& in, ProgramKind kind, i32 target_line) {
    const nx::string p = pred_text(in.pred);
    const nx::string co = (in.flags & kCoissued) ? "+" : "";
    auto ops = [&](bool with_dest) {
        nx::vector<nx::string> v;
        if (with_dest) v.push_back(format_operand(in.dest, kind));
        for (u8 i = 0; i < in.src_count; ++i) v.push_back(format_operand(in.src[i], kind));
        return v;
    };
    switch (in.op) {
        case Op::Invalid: return nx::format("<invalid {:016x}>", in.raw);
        case Op::SetIncrements:
        case Op::Hidden: return "";
        case Op::Mov:
        case Op::Pack:
            if (in.family == 8) return p + co + format_pack(in, kind);
            if (in.family == 7) return p + line_n(nx::format("mov.{}", type_name(in.type)), ops(true));
            return p + co + line_n(nx::format("mov.{}", type_name(in.type)), ops(true));
        case Op::Cmov:
        case Op::Cmov8:
            return p + line_n(nx::format("{}.{}.{}", in.op == Op::Cmov ? "cmov" : "cmov8", cond_text(in.cond),
                                         type_name(in.type)),
                              ops(true));
        case Op::Test: {
            nx::vector<nx::string> v = ops(true);
            return p + line_n(test_mnemonic(in), v);
        }
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Shl:
        case Op::Rol:
        case Op::Shr:
        case Op::Asr:
        case Op::Rlp: {
            nx::string b = format_operand(in.src[1], kind);
            if (in.flags & kInvertB) b = "~" + b;
            return p + line(nx::format("{}.{}", mnemonic(in.op), type_name(in.type)),
                            {format_operand(in.dest, kind), format_operand(in.src[0], kind), b});
        }
        case Op::Sop:
        case Op::Sop2:
        case Op::Blend: return p + co + format_sop(in, kind);
        case Op::FxMad:
            return p + line_n(nx::format("mad.{}{}", (in.flags & kSat) ? "sat." : "", type_name(in.type)), ops(true));
        case Op::IMad:
            if (in.flags & kScaleVariant)
                return p + line_n(nx::format("mad.{}.s{}", type_name(in.type), (in.flags & kSignedShift) ? 1 : 0), ops(true));
            return p + line_n(nx::format("mad.{}{}", type_name(in.type), (in.flags & kSat) ? ".sat" : ""), ops(true));
        case Op::Tex: return p + line_n(tex_mnemonic(in), ops(true));
        case Op::Load:
        case Op::Store: return p + memory_text(in, kind);
        case Op::Br: return p + line("br", {nx::format("#{}", target_line)});
        case Op::Ba: return p + line("ba", {nx::format("0x{:x}", static_cast<u32>(in.target))});
        case Op::Ret: return p + "ret";
        case Op::MovToLink: return p + line("mov", {"lnk0", "r0"});
        case Op::MovFromLink: return p + line("mov", {"r0", "lnk0"});
        case Op::Nop: return p + "nop";
        case Op::Kill: return p + "kill";
        case Op::PCoeff: return "pcoeff";
        case Op::DepthF: return p + line("depthf", {format_operand(in.dest, kind)});
        case Op::MovImm: return p + line_n("mov.u32", ops(true));
        default: {
            nx::string m = nx::format("{}.{}", mnemonic(in.op), type_name(in.type));
            if (in.src_type != Type::None && in.src_type != in.type) m += nx::format(".{}", type_name(in.src_type));
            return p + co + line_n(m, ops(true));
        }
    }
}

nx::string format_listing(std::span<const Decoded> program, ProgramKind kind) {
    // Listing numbers: one per printed instruction except co-issued ones (which share the previous number); a word's
    // number is that of its first line (or of the next printed line).
    u32 words = 0;
    for (const Decoded& d : program) words = std::max(words, d.word + 1);
    nx::vector<i32> word_line(words + 1, -1);
    nx::vector<i32> numbers(program.size(), -1);
    i32 next = 0;
    for (usize i = 0; i < program.size(); ++i) {
        const Decoded& d = program[i];
        if (word_line[d.word] < 0) word_line[d.word] = next;
        if ((!d.in.prints() && d.in.op != Op::Invalid) || !d.listed) continue;
        if ((d.in.flags & kCoissued) && i > 0 && numbers[i - 1] >= 0) numbers[i] = numbers[i - 1];
        else numbers[i] = next++;
    }
    word_line[words] = next;
    nx::flat_hash_set<i32> targets;
    bool end_label = false;
    for (const Decoded& d : program)
        if (d.in.op == Op::Br) {
            if (d.in.target >= 0 && static_cast<u32>(d.in.target) < words) targets.insert(word_line[d.in.target]);
            else end_label = true;
        }
    nx::string out;
    for (usize i = 0; i < program.size(); ++i) {
        if (numbers[i] < 0) continue;
        const Instruction& in = program[i].in;
        const i32 target = in.op == Op::Br ? (in.target >= 0 && static_cast<u32>(in.target) < words ? word_line[in.target] : next) : 0;
        out += nx::format("{}{}: {}\n", targets.contains(numbers[i]) ? "#" : "", numbers[i], format(in, kind, target));
    }
    if (end_label) out += nx::format("#{}:\n", next);
    return out;
}

}  // namespace psvrecore::usse
