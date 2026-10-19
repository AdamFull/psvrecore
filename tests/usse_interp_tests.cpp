// psvrecore::usse reference interpreter: short programs (instruction words the shader compiler emits) with their
// inputs set by hand, against values computed by hand. Whole compiled shaders are checked against the host by
// scripts/usse_exec_check.py.

#include <bit>
#include <cmath>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/usse/interp.hpp"

using namespace psvrecore;
using namespace psvrecore::usse;

namespace {

u32 bits(f32 v) { return std::bit_cast<u32>(v); }
f32 value(u32 w) { return std::bit_cast<f32>(w); }

void set4(u32* bank, u32 at, f32 x, f32 y, f32 z, f32 w) {
    bank[at] = bits(x), bank[at + 1] = bits(y), bank[at + 2] = bits(z), bank[at + 3] = bits(w);
}

bool near(f32 got, f32 want) { return std::fabs(got - want) <= 1e-6f * (1.0f + std::fabs(want)); }

Result<void> run_both(std::span<const u64> secondary, std::span<const u64> primary, Machine& m) {
    const auto s = decode_program(secondary, ProgramKind::Secondary);
    if (auto r = run(s, ProgramKind::Secondary, m); !r) return r;
    return run(decode_program(primary, ProgramKind::Primary), ProgramKind::Primary, m);
}

class TestMemory : public ShaderMemory {
public:
    u32 base = 0;
    u32 words[128] = {};
    bool read(u32 address, void* out, u32 size) override {
        if (address < base || address + size > base + sizeof words) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words) + (address - base), size);
        return true;
    }
    bool write(u32, const void*, u32) override { return false; }
};

}  // namespace

TEST_CASE("usse interp: half-precision conversions") {
    CHECK(to_half(1.0f) == 0x3C00);
    CHECK(to_half(-2.0f) == 0xC000);
    CHECK(to_half(65504.0f) == 0x7BFF);
    CHECK(to_half(1e6f) == 0x7C00);                           // overflow: infinity
    CHECK(to_half(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);   // a tie: to even
    CHECK(to_half(1.0f + 3 * std::ldexp(1.0f, -11)) == 0x3C02);
    CHECK(to_half(std::ldexp(1.0f, -24)) == 0x0001);          // the smallest subnormal
    CHECK(from_half(0x3555) == 0.333251953125f);
    CHECK(from_half(0x0001) == std::ldexp(1.0f, -24));
    CHECK(std::isnan(from_half(0x7E00)));
}

TEST_CASE("usse interp: f32 arithmetic and dual issue") {
    // o = a * b + c; t = {0.5, 3, 1.25, -7} * a + frac(b) + rsqrt(abs(c) + 1), the literal in sa0-sa3
    const u64 words[] = {0xFA44070000000000ull, 0xF800094000000000ull, 0x08A51FC49F441101ull, 0x28C45001A0180782ull,
                         0x08A44F840F042FBEull, 0x30800A0900003E82ull, 0x30800A1100203E81ull, 0x30800A1900203E82ull,
                         0x08A44F844F04103Cull, 0x40C00DBCAFB98002ull, 0x18B18981D0910100ull, 0x18B18981D0EEC101ull,
                         0x00A09181A003D084ull, 0x00A09181A04FD0C5ull, 0xFB275000A0200000ull};
    Machine m;
    set4(m.primary, 0, 1, 2, 3, 4);
    set4(m.primary, 4, 0.5f, 1.5f, 2.25f, -3.75f);
    set4(m.primary, 8, 1, -2, 3, -4);
    set4(m.secondary, 0, 0.5f, 3, 1.25f, -7);
    REQUIRE(run(decode_program(words, ProgramKind::Primary), ProgramKind::Primary, m));
    const f32 a[4] = {1, 2, 3, 4}, b[4] = {0.5f, 1.5f, 2.25f, -3.75f}, c[4] = {1, -2, 3, -4}, k[4] = {0.5f, 3, 1.25f, -7};
    for (int i = 0; i < 4; ++i) {
        CHECK(near(value(m.output[i]), a[i] * b[i] + c[i]));
        CHECK(near(value(m.output[4 + i]), k[i] * a[i] + (b[i] - std::floor(b[i])) + 1 / std::sqrt(std::fabs(c[i]) + 1)));
    }
}

TEST_CASE("usse interp: f16 results in internal registers are f32") {
    // secondary: sa0-sa1 = f16(u.yxwz); primary: i1 = mad.f16(a, sa0, a) widened, o = a * i1 (f32)
    const u64 secondary[] = {0x40840D7EA010C003ull};
    const u64 primary[] = {0xFA44070000000000ull, 0xF800094000000000ull, 0x40C00DBCAF998002ull, 0x04A09F80CF43C03Cull,
                           0x18E181811011813Cull, 0x0880B181004D0F3Dull, 0xFB275000A0200000ull};
    Machine m;
    set4(m.secondary, 0, 1.5f, 2, -0.5f, 4);
    set4(m.primary, 0, 1, -2, 0.25f, 3);
    REQUIRE(run_both(secondary, primary, m));
    // a * u.yxwz + a = (3, -5, 1.25, 1.5); times a
    CHECK(value(m.output[0]) == 3.0f);
    CHECK(value(m.output[1]) == 10.0f);
    CHECK(value(m.output[2]) == 0.3125f);
    CHECK(value(m.output[3]) == 4.5f);
}

TEST_CASE("usse interp: a loop counted by a uniform") {
    // r = 1; for (i < (int)min(abs(u.y), 5)) r = r * 0.5 + a: the count goes through the compiler's float-to-int
    // sequence (a register-destination test, truncating packs, cmov8), the loop through predicates, a branch and
    // the 32 x 16 multiply-adds.
    const u64 secondary[] = {0x08A41896A0045140ull, 0x08A40884AF046004ull, 0x78C489028010817Cull, 0x18E388804F018053ull,
                             0x08A408840F442F3Cull, 0x18A18146001C003Dull, 0x18E380825041804Bull, 0x40810CC6A0400100ull,
                             0x40810D0AA0400001ull, 0x388082A6AF0002C2ull, 0x3886050A43040040ull};
    const u64 primary[] = {0xFA44070000000000ull, 0x50810008E0400480ull, 0x40800D8DF0030102ull, 0x40800D8DF0430102ull,
                           0x4888068130078100ull, 0xFD00004000000008ull, 0x5083000860000600ull, 0x0080918160000000ull,
                           0x5083000860200600ull, 0x0080998160440041ull, 0xD08180042F830101ull, 0xD09080040043017Cull,
                           0xF8000040000FFFF8ull, 0xFB275000A0200000ull};
    Gxp gxp;  // (the program's tables: the uniform in sa0-sa3, its constants from sa4)
    gxp.containers = {Container{kDefaultUniformContainer, 0, 4}, Container{kConstantsContainer, 4, 9}};
    const u32 literals[9] = {0x4F000000, 0x4F000000, 0x4F000000, 0x4F000000, 0xCF000000, 0, 0x40A00000, 0x7FFFFFFF, 1};
    for (u32 i = 0; i < 9; ++i) gxp.literals.push_back(Literal{i, literals[i]});
    for (const f32 uy : {3.75f, -1.5f, 0.5f, 9.0f}) {
        Machine m;
        const u32 uniforms[4] = {0, bits(uy), 0, 0};
        REQUIRE(load_secondary(gxp, uniforms, {}, m));
        set4(m.primary, 0, 1, -2, 0.5f, 4);
        REQUIRE(run_both(secondary, primary, m));
        const int n = static_cast<int>(std::fmin(std::fabs(uy), 5.0f));
        const f32 a[4] = {1, -2, 0.5f, 4};
        for (int i = 0; i < 4; ++i) {
            f32 r = 1;
            for (int k = 0; k < n; ++k) r = r * 0.5f + a[i];
            CHECK(value(m.output[i]) == r);
        }
    }
}

TEST_CASE("usse interp: the default uniform buffer read from memory") {
    // o = v[(int)a.x] + p, with p in sa0-sa3 and v (64 elements) only in memory: sa4 holds the buffer's address
    // plus its bias (4 * the first word not in a register - 4), sa5 a literal 0x10000 (one element, the bias back)
    const u64 primary[] = {0xFA44070000000000ull, 0x40810CC6A0000000ull, 0xA8820886B0000805ull, 0xE8853004A0010000ull,
                           0xF920000000000000ull, 0x08A4418530041000ull, 0x08C0418930441041ull, 0xFB275000A0200000ull};
    Gxp gxp;
    gxp.containers = {Container{kDefaultUniformContainer, 0, 4}, Container{kConstantsContainer, 4, 2}};
    gxp.literals = {Literal{1, 0x10000}};
    gxp.buffer_slots = {BufferSlot{kDefaultUniformContainer, 0, 12}};
    TestMemory memory;
    memory.base = 0x10000;
    set4(memory.words, 0, 0.5f, 1, 2, 4);  // p
    for (u32 i = 0; i < 7; ++i) set4(memory.words, 4 + 4 * i, static_cast<f32>(i), 10, 20, 30);  // v[i]
    u32 buffers[15] = {};
    buffers[kDefaultUniformContainer] = memory.base;
    Machine m;
    m.memory = &memory;
    REQUIRE(load_secondary(gxp, std::span<const u32>(memory.words, 4), buffers, m));
    CHECK(m.secondary[4] == memory.base + 12);
    set4(m.primary, 0, 5.75f, 0, 0, 0);
    REQUIRE(run(decode_program(primary, ProgramKind::Primary), ProgramKind::Primary, m));
    CHECK(value(m.output[0]) == 5.5f);
    CHECK(value(m.output[1]) == 11.0f);
    CHECK(value(m.output[2]) == 22.0f);
    CHECK(value(m.output[3]) == 34.0f);
}

TEST_CASE("usse interp: failures are reported") {
    Machine m;
    const u64 texture[] = {0xE001C408E2C00080ull};  // (texture sampling is not implemented yet)
    CHECK(!run(decode_program(texture, ProgramKind::Primary), ProgramKind::Primary, m));
    const u64 loop[] = {0xF800004000000000ull};  // br to itself
    RunLimits limits;
    limits.max_steps = 1000;
    CHECK(!run(decode_program(loop, ProgramKind::Primary), ProgramKind::Primary, m, limits));
}
