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
