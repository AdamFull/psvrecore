// psvrecore::gxm shader translator: the modules it emits are well formed (header, instruction lengths, result ids
// within the bound) and have the structure the program needs. What they compute is checked on a GPU against the
// reference interpreter (scripts/usse_exec_check.py --gpu); validity against the SPIR-V rules by spirv-val (see
// docs/BUILDING.md).

#include "nxtest.h"
#include "psvrecore/gxm/translate.hpp"

using namespace psvrecore;
using namespace psvrecore::usse;

namespace {

constexpr u32 kOpLoopMerge = 246, kOpSwitch = 251, kOpEntryPoint = 15, kOpCapability = 17, kOpExtInstImport = 11;

struct Walk {
    bool ok = true;
    u32 count[512] = {};
};

// Walks the instructions: each length fits the module, and the instructions with a result (from the opcodes
// this translator emits) name an id below the bound.
Walk walk(const nx::vector<u32>& m) {
    Walk w;
    if (m.size() < 5 || m[0] != 0x07230203u || m[1] != 0x00010300u || m[3] == 0 || m[4] != 0) {
        w.ok = false;
        return w;
    }
    const u32 bound = m[3];
    for (usize i = 5; i < m.size();) {
        const u32 n = m[i] >> 16, op = m[i] & 0xFFFF;
        if (n == 0 || i + n > m.size()) {
            w.ok = false;
            return w;
        }
        if (op < 512) ++w.count[op];
        // (result id positions: types and labels at 1; constants, variables and operations at 2)
        u32 at = 0;
        if ((op >= 19 && op <= 33) || op == 248 || op == 54 || op == kOpExtInstImport) at = 1;
        else if ((op >= 41 && op <= 46) || op == 59 || op == 61 || op == 65 || (op >= 124 && op <= 205) || op == 12)
            at = 2;
        if (at && (n <= at || m[i + at] == 0 || m[i + at] >= bound)) {
            w.ok = false;
            return w;
        }
        i += n;
    }
    return w;
}

}  // namespace

TEST_CASE("gxm translate: straight-line programs") {
    // o = a * b + c; t = {0.5, 3, 1.25, -7} * a + frac(b) + rsqrt(abs(c) + 1) (usse interp tests)
    const u64 words[] = {0xFA44070000000000ull, 0xF800094000000000ull, 0x08A51FC49F441101ull, 0x28C45001A0180782ull,
                         0x08A44F840F042FBEull, 0x30800A0900003E82ull, 0x30800A1100203E81ull, 0x30800A1900203E82ull,
                         0x08A44F844F04103Cull, 0x40C00DBCAFB98002ull, 0x18B18981D0910100ull, 0x18B18981D0EEC101ull,
                         0x00A09181A003D084ull, 0x00A09181A04FD0C5ull, 0xFB275000A0200000ull};
    const Gxp gxp;
    const auto m = gxm::translate_compute(gxp, decode_program(words, ProgramKind::Primary));
    REQUIRE(m);
    const Walk w = walk(*m);
    REQUIRE(w.ok);
    CHECK(w.count[kOpEntryPoint] == 1);
    CHECK(w.count[kOpCapability] >= 1);
    CHECK(w.count[kOpExtInstImport] == 1);
}

TEST_CASE("gxm translate: branches become a loop over the blocks") {
    // the primary program of "usse interp: a loop counted by a uniform"
    const u64 primary[] = {0xFA44070000000000ull, 0x50810008E0400480ull, 0x40800D8DF0030102ull, 0x40800D8DF0430102ull,
                           0x4888068130078100ull, 0xFD00004000000008ull, 0x5083000860000600ull, 0x0080918160000000ull,
                           0x5083000860200600ull, 0x0080998160440041ull, 0xD08180042F830101ull, 0xD09080040043017Cull,
                           0xF8000040000FFFF8ull, 0xFB275000A0200000ull};
    const Gxp gxp;
    const auto m = gxm::translate_compute(gxp, decode_program(primary, ProgramKind::Primary));
    REQUIRE(m);
    const Walk w = walk(*m);
    REQUIRE(w.ok);
    CHECK(w.count[kOpLoopMerge] == 1);
    CHECK(w.count[kOpSwitch] == 1);
}

TEST_CASE("gxm translate: unknown instructions fail") {
    const u64 words[] = {0xFFFFFFFFFFFFFFFFull};
    const Gxp gxp;
    const auto program = decode_program(words, ProgramKind::Primary);
    REQUIRE(!program.empty());
    if (program[0].in.op == Op::Invalid) CHECK(!gxm::translate_compute(gxp, program));
}

namespace {

// A fragment program reading the colour (__nativecolor, FRAGCOLOR, 8-bit output) whose last word blends pa0 over
// it: o0.xyz = pa0 * f0 + o0 * f1 and, co-issued, o0.w (sop2), as the shader compiler ends such programs.
struct BlendProgram {
    Gxp gxp;
    nx::vector<Decoded> program;
};

Operand lanes_operand(Bank bank, i32 number, u8 mask) {
    Operand o;
    o.bank = bank;
    o.number = number;
    o.lanes = 4;
    o.mask = mask;
    return o;
}

BlendProgram blend_program(SopTerm colour0, SopTerm colour1, SopTerm alpha0, SopTerm alpha1) {
    BlendProgram p;
    p.gxp.type = ShaderType::Fragment;
    p.gxp.native_color = p.gxp.frag_color = true;
    p.gxp.output_type = ParameterType::U8;
    p.gxp.output_components = 4;
    for (u32 part = 0; part < 2; ++part) {
        Decoded d;
        d.word = 0;
        Instruction& in = d.in;
        in.op = Op::Sop2;
        in.family = 17;
        in.type = Type::Fx8;
        const u8 mask = part ? 8 : 7;
        in.dest = lanes_operand(Bank::Output, 0, mask);
        in.src[0] = lanes_operand(Bank::Primary, 0, mask);
        in.src[1] = lanes_operand(Bank::Output, 0, mask);
        in.src[2] = lanes_operand(Bank::Temp, 0, mask);
        in.src_count = 3;
        in.term[0] = part ? alpha0 : colour0;
        in.term[1] = part ? alpha1 : colour1;
        if (part) in.flags |= kCoissued;
        p.program.push_back(d);
    }
    return p;
}

bool has_capability(const nx::vector<u32>& m, u32 capability) {
    for (usize i = 5; i < m.size(); i += m[i] >> 16) {
        if ((m[i] & 0xFFFF) == kOpCapability && m[i + 1] == capability) return true;
        if ((m[i] >> 16) == 0) break;
    }
    return false;
}

}  // namespace

TEST_CASE("gxm translate: a colour read that is a blend is found as the pipeline's blending") {
    using B = gxm::ColorReadBlend;
    // source alpha over the colour
    const BlendProgram over = blend_program({Factor::AAlpha, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    const auto b = gxm::color_read_blend(over.gxp, over.program);
    REQUIRE(b);
    CHECK(b->color_src == B::kSrcAlpha);
    CHECK(b->color_dst == B::kOneMinusSrcAlpha);
    CHECK(b->alpha_src == B::kSrcAlpha);
    CHECK(b->alpha_dst == B::kOneMinusSrcAlpha);
    CHECK(b->color_op == B::kAdd);
    CHECK(b->alpha_op == B::kAdd);
    CHECK(b->lanes == 15);
    // added to the colour (the colour's factor 1 - 0)
    const BlendProgram add = blend_program({Factor::AAlpha, false}, {Factor::Zero, true}, {Factor::A, false}, {Factor::Zero, true});
    const auto a = gxm::color_read_blend(add.gxp, add.program);
    REQUIRE(a);
    CHECK(a->color_src == B::kSrcAlpha);
    CHECK(a->color_dst == B::kOne);
    CHECK(a->alpha_dst == B::kOne);
}

TEST_CASE("gxm translate: colour reads that are not such a blend are left to the program") {
    // a third register's factor
    BlendProgram s0 = blend_program({Factor::S0, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    CHECK(!gxm::color_read_blend(s0.gxp, s0.program));
    // the colour read again by another instruction
    BlendProgram again = blend_program({Factor::AAlpha, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    Decoded read;
    read.word = 1;
    read.in.op = Op::Mov;
    read.in.type = Type::U32;
    read.in.dest = lanes_operand(Bank::Temp, 2, 15);
    read.in.src[0] = lanes_operand(Bank::Output, 0, 15);
    read.in.src_count = 1;
    again.program.push_back(read);
    CHECK(!gxm::color_read_blend(again.gxp, again.program));
    // a program that does not read the colour
    BlendProgram plain = blend_program({Factor::AAlpha, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    plain.gxp.frag_color = false;
    CHECK(!gxm::color_read_blend(plain.gxp, plain.program));
    // the colour lanes' factor names the source's alpha, which no alpha half writes
    BlendProgram rgb = blend_program({Factor::AAlpha, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    rgb.program.pop_back();
    CHECK(!gxm::color_read_blend(rgb.gxp, rgb.program));
}

TEST_CASE("gxm translate: a colour read left to the blending reads no attachment") {
    constexpr u32 kCapabilityInputAttachment = 40;
    const BlendProgram over = blend_program({Factor::AAlpha, false}, {Factor::AAlpha, true}, {Factor::A, false}, {Factor::A, true});
    gxm::FragmentShader fs;
    const auto reads = gxm::translate_fragment(over.gxp, over.program, fs);
    REQUIRE(reads);
    REQUIRE(walk(*reads).ok);
    CHECK(has_capability(*reads, kCapabilityInputAttachment));
    fs.blend_read = true;
    const auto blends = gxm::translate_fragment(over.gxp, over.program, fs);
    REQUIRE(blends);
    REQUIRE(walk(*blends).ok);
    CHECK(!has_capability(*blends, kCapabilityInputAttachment));
}
