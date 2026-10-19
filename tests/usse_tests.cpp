// psvrecore::usse: decoding into the instruction model and formatting. The expected texts are what the reference
// disassembler (psp2shaderperf) prints for each word; the decoder's agreement with it over whole shader corpora is
// checked separately (scripts/usse_check.py).

#include <cstring>

#include "nxtest.h"
#include "psvrecore/usse/format.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::usse;

namespace {

nx::string squeeze(const nx::string& s) {
    nx::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        out += c;
        space = false;
    }
    return out;
}

// The lines one word decodes to (outside a program: no increments set, branches unresolved).
nx::vector<nx::string> lines(u64 word, ProgramKind kind = ProgramKind::Primary) {
    DecodeState state;
    nx::vector<Decoded> out;
    decode_word(word, 0, kind, state, out);
    nx::vector<nx::string> v;
    for (const Decoded& d : out)
        if (d.in.prints() || d.in.op == Op::Invalid) v.push_back(squeeze(format(d.in, kind)));
    return v;
}

nx::string one(u64 word, ProgramKind kind = ProgramKind::Primary) {
    const auto v = lines(word, kind);
    return v.size() == 1 ? v[0] : nx::string("<lines: ") + nx::format("{}", v.size()) + ">";
}

}  // namespace

TEST_CASE("usse: arithmetic families") {
    CHECK(one(0x00A10F80DF03C2CCull) == "mad.f32 i0.xyzw, i0.xyzw, sa22.xxxx, {0.5, 0.5, 0.5, 0.5}");
    CHECK(one(0x04800884AF041004ull) == "mad.f16 i0.x, pa2.y, pa0.x, pa8.x");
    CHECK(one(0x08A40F84EF0450C0ull) == "min.f32 i0.xyzw, sa6.xyzw, pa0.xxxx");
    CHECK(one(0x10C2F8826024F3C0ull) == "dot.f16 pa0.x, {1, 1, 1, 0}, pa0.xyz1");
    CHECK(one(0x18818880EF7E4401ull) == "dot.f32 i1.x, sa2.xz0, i2.xy1");
    CHECK(one(0x18B18F80DF51210Dull) == "mad.f32 i1.xyzw, sa26.xyzw, i1.xyzw, i2.xyzw");
    CHECK(one(0x3020009080000001ull) == "rcp.f16 r0.x, pa0.--z");
    CHECK(one(0x308008000F800001ull) == "rcp.f32 i0.x, r0.x");
}

TEST_CASE("usse: moves, packs and tests") {
    CHECK(one(0x38C34C8655040022ull) == "cmov.lezero.f16 pa2.xyzw, pa0.xyzw, {0, 0, 0, 0}, {1, 1, 1, 1}");
    CHECK(one(0x388202004FF40300ull) == "mov.i32 i1.xyzw, 1056964608");
    CHECK(one(0x40C00DBC0F998002ull) == "mov.f32 i0.xyzw, r0.xyzw");
    CHECK(one(0x40C30D9C5FC18002ull) == "mov.f32 i2.xyz, ({0}, {1})");
    CHECK(one(0x40810FC4A0800001ull) == "mov.fx10 r4.x, pa0.y");
    CHECK(one(0x40810B7EE0198001ull) == "mov.f16 pa0.xyzw, sa0.yyzw");
    // packing f32 to integers: the lane selects go to the written lanes in order
    CHECK(one(0x40810C7120033C80ull) == "pack.s8.f32 o0.--zw, i0.--zw");
    CHECK(one(0x40810C3120010000ull) == "pack.u8.f32 o0.--zw, r0.--xy");
    CHECK(one(0x4809C91110038080ull) == "cmp.ge.f32 p0, r2.y, {0}");
    CHECK(one(0x488C0A81000A2240ull) == "cmp.gt.u8 p0, r4.x, r0.x");
    CHECK(one(0x78408A80C0338080ull) == "cmp.gt.f32 r2.x, sa2.xyzw, r0.xyzw");
}

TEST_CASE("usse: bitwise, fixed point, integer") {
    CHECK(one(0x50810008E1000680ull) == "or.u32 r8.x, sa13.x, 0x0");
    CHECK(one(0x6071062029CA862Dull) == "shl.u32 r78.x, r12.x, 0x2d000095");
    CHECK(one(0x9080118008C02340ull) == "mul.fx10 r6.x, r6.x, r0.x");
    CHECK(one(0x9010B20008C02346ull) == "sub.fx10 r6.xy, 0, r6.xy");
    CHECK(one(0x9080118008C02340ull ^ (1ull << 35)) ==
          "add.fx10 r6.x, r0.x * r6.x, min(r6.www,1.0 - r0.www) * r0.x");
    CHECK(one(0xC880080009122046ull) == "mad.sat.fx10 r8.xyzw, r0.xyzw, r6.xyzw, r8.xyzw");
    CHECK(one(0xA8820882B001440Dull) == "mad.i32 pa0.x, r5.lo16, 0x8.lo16, sa13.x");
    CHECK(one(0xA0010900E0A10601ull) == "mad.i16 r5.xy, r4.xy, sa12, 0x1.lo8");
    CHECK(one(0xD0820006B1041011ull) == "mad.u32.s0 pa8.x, pa16.x, 0x20, sa17.x");
}

TEST_CASE("usse: two operations in one word") {
    const auto dual = lines(0x24411C02200584FEull);
    REQUIRE(dual.size() == 2);
    CHECK(dual[0] == "add.f32 pa0.x, i1.x, i2.y");
    CHECK(dual[1] == "+add.f32 i2.x, -i0.x, i2.x");
    const auto v4 = lines(0x28C401108FB74080ull);
    REQUIRE(v4.size() == 2);
    CHECK(v4[0] == "mad.f32 i0.x--w, i0.x--w, i0.x--w, i1.x--w");
    CHECK(v4[1] == "+mov.f32 i1.xyzw, pa0.xyzw");
    const auto sop = lines(0x88809106F810264Eull);
    REQUIRE(sop.size() == 2);
    CHECK(sop[0] == "add.fx10 pa0.xyz, pa0.xyz * sa12.xyz, sa14.xyz");
    CHECK(sop[1] == "+add.fx10 pa0.---w, pa0.---w * sa12.---w, sa14.---w");
    const auto blend = lines(0x809082DD90000000ull);
    REQUIRE(blend.size() == 2);
    CHECK(blend[0] == "add.fx8 o0.xyz, pa0.www * pa0.xyz, (1.0 - pa0.www) * o0.xyz");
    CHECK(blend[1] == "+add.fx8 o0.---w, pa0.---w * pa0.---w, (1.0 - pa0.---w) * o0.---w");
}

TEST_CASE("usse: texture, memory, control") {
    CHECK(one(0xE001C408E2C00080ull) == "tex2D.f32 r22, r0.xy, sa2");
    CHECK(one(0xE000C500E0800101ull) == "tex2DBias.f32 r4, r0.xy, sa4, pa2");
    CHECK(one(0xE8A30084AB400080ull, ProgramKind::Secondary) == "lda32 sa90, [sa0, 0x1]");
    CHECK(one(0xEE16737A7ED4D57Bull, ProgramKind::Secondary) == "!p1 lda64.fetch8 r118, [o83, 67110912]");
    CHECK(one(0xFC63003000503127ull) == "mov.u32 r2.x, 0xc0703127");
    CHECK(one(0xF9300406F0000000ull) == "!p1 kill");
    CHECK(one(0xFB300400F000060Cull) == "p1 depthf r0");  // (its own predicate table)
    CHECK(one(0xFB380000F0005830ull) == "depthf o1");
    CHECK(one(0xF800094000000000ull) == "nop");
    CHECK(lines(0xFA44070000000000ull).empty());  // (the program prologue prints nothing)
}

TEST_CASE("usse: repeats follow the increments") {
    // 4 repetitions of a mov with the default increments, then with src1 in swizzle mode (offsets 1, 2, 0, 0).
    const u64 rep = 0x38803521C3000000ull;
    {
        const u64 words[] = {rep};
        const auto p = decode_program(words, ProgramKind::Primary);
        REQUIRE(p.size() == 4);
        CHECK(squeeze(format(p[3].in, ProgramKind::Primary)) == "mov.f32 o6.xy, sa6.xy");
    }
    {
        const u64 words[] = {0xFA10000201010901ull, rep};
        const auto p = decode_program(words, ProgramKind::Primary);
        REQUIRE(p.size() == 5);
        CHECK(!p[0].in.prints());
        CHECK(squeeze(format(p[1].in, ProgramKind::Primary)) == "mov.f32 o0.xy, sa2.xy");
        CHECK(squeeze(format(p[2].in, ProgramKind::Primary)) == "mov.f32 o2.xy, sa4.xy");
        CHECK(squeeze(format(p[3].in, ProgramKind::Primary)) == "mov.f32 o4.xy, sa0.xy");
    }
    // Family 3 repeats its own way: a dot product writes the next lane and reads the next vec4.
    const u64 dot[] = {0x18903881C03DA200ull | (3ull << 44)};
    const auto p = decode_program(dot, ProgramKind::Primary);
    REQUIRE(p.size() == 4);
    CHECK(squeeze(format(p[3].in, ProgramKind::Primary)) == "dot.f32 o0.---w, sa12.xyzw, i0.xyz1");
}

TEST_CASE("usse: listing numbers and branch targets") {
    // prologue (no line), nop, branch two words on (to the mov), nop, mov
    const u64 words[] = {0xFA44070000000000ull, 0xF800094000000000ull, 0xF800004000000002ull, 0xF800094000000000ull,
                         0x40C00DBC0F998002ull};
    const auto p = decode_program(words, ProgramKind::Primary);
    const nx::string listing = format_listing(p, ProgramKind::Primary);
    CHECK(squeeze(listing) == squeeze("0: nop\n1: br #3\n2: nop\n#3: mov.f32 i0.xyzw, r0.xyzw\n"));
}

TEST_CASE("usse: the constant table reads as f32 or f16 lanes") {
    // index 20 with an xyzw swizzle reads words 40-43: e, 1/e, sqrt(2), 1/sqrt(2)
    const u64 word = (0x00A10F80DF03C2CCull & ~0x3Full & ~(7ull << 45)) | 20 | (4ull << 45);
    CHECK(one(word) == "mad.f32 i0.xyzw, i0.xyzw, sa22.xxxx, {2.7183, 0.36788, 1.4142, 0.70711}");
    CHECK(one(word | (1ull << 58)) == "mad.f16 i0.xyzw, i0.xyzw, sa22.xxxx, {-35456, 2.0879, 214.25, 1.6836}");
    CHECK(constants()[40] == 0x402DF854u);
}

TEST_CASE("usse: GXP container") {
    // A minimal GXP: header, a primary program of two words, no secondary program.
    u8 data[0x60] = {};
    std::memcpy(data, "GXP", 4);
    auto put32 = [&](u32 at, u32 v) { std::memcpy(data + at, &v, 4); };
    put32(0x08, sizeof data);
    put32(0x1C, 1);              // fragment
    put32(0x3C, 2);              // primary: 2 words
    put32(0x40, 0x50 - 0x40);    // at 0x50
    const u64 w0 = 0xFA44070000000000ull, w1 = 0xF800094000000000ull;
    std::memcpy(data + 0x50, &w0, 8);
    std::memcpy(data + 0x58, &w1, 8);
    const auto g = parse_gxp(data);
    REQUIRE(g);
    CHECK(g->type == ShaderType::Fragment);
    REQUIRE(g->primary.size() == 2);
    CHECK(g->primary[1] == w1);
    CHECK(g->secondary.empty());
    data[0] = 'X';
    CHECK(!parse_gxp(data));
}
