// PlayStation Network as on a console that is offline and signed out: every NP function the surveyed games import
// answers as its library does then (0, a new ID, or the library's "signed out"); the few with real work (NP IDs
// compared, the environment, the party state, an NP ID parsed, trophies' information and icons not available).

#include <cstring>

#include "nxtest.h"
#include "psvrecore/runtime/dialogs.hpp"
#include "runtime_support.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

u32 u(i32 v) { return static_cast<u32>(v); }

// (library, function, the answer: 0, 1 for "a new positive ID", or the error)
struct Answer {
    u32 library, nid, expected;
};
const Answer kAnswers[] = {
    {0xD8835093, 0x29C9B8A5, 0x8055050Au},
    {0xD8835093, 0x7CE94530, 0x8055050Au},
    {0xD8835093, 0x80036A44, 0x8055050Au},
    {0xD8835093, 0x8D7981D4, 0x8055050Au},
    {0xD8835093, 0x9554802B, 0x8055050Au},
    {0xD8835093, 0x96DF1B76, 0x8055050Au},
    {0xD8835093, 0x555EB58C, 0x8055050Au},
    {0xD8835093, 0x8C94C7A5, 0},
    {0xD8835093, 0x65DFB208, 0x8055050Au},
    {0xD8835093, 0xFE835967, 0x8055050Au},
    {0xD8835093, 0x43DC48A1, 0x8055050Au},
    {0xD8835093, 0xD9E6E56C, 0},
    {0xECB62224, 0x40724BEA, 0x80550310u},
    {0xECB62224, 0x48314C6E, 0x80550310u},
    {0xECB62224, 0x650D111B, 0x80550310u},
    {0xECB62224, 0x88B18A5B, 0x80550310u},
    {0xECB62224, 0x9AEF645B, 0x80550310u},
    {0xECB62224, 0xBDCD318E, 0x80550310u},
    {0xECB62224, 0xE426FEE8, 0x80550310u},
    {0xECB62224, 0x14FC18AF, 0},
    {0xECB62224, 0xF93842F0, 0x80550310u},
    {0xECB62224, 0x3377CD37, 0x80550310u},
    {0xECB62224, 0x59608D1C, 0x80550310u},
    {0xECB62224, 0xC1E23E01, 0x80550310u},
    {0xECB62224, 0xE807D0BC, 0x80550601},  // (sceNpSetPlatformType: the "type" here is a pointer, not a type)
    {0x605BA760, 0x1211AE8E, 0x80551D06u},
    {0x605BA760, 0x407E1E6F, 0x80551D06u},
    {0x605BA760, 0x5183A4B5, 0x80551D06u},
    {0x605BA760, 0xEF8A91BC, 0x80551D06u},
    {0x605BA760, 0x364531A8, 0x80551D06u},
    {0x605BA760, 0xFB0F7FDF, 0x80551D06u},
    {0x605BA760, 0x8C90CC09, 0x80551D06u},
    {0x605BA760, 0x3B0A7F47, 0x80551D06u},
    {0x605BA760, 0x3ADE3628, 0},
    {0x605BA760, 0xE02A445C, 0},
    {0x605BA760, 0x7A5020A5, 0x80551D06u},
    {0x605BA760, 0x363FF161, 0},
    {0x605BA760, 0x2A764628, 0},
    {0x75F052DB, 0x00F90E7B, 0x8055050Au},
    {0x75F052DB, 0x4F795659, 0x8055050Au},
    {0x75F052DB, 0x7090ED11, 0x8055050Au},
    {0x75F052DB, 0xDFAD64D3, 0x8055050Au},
    {0x75F052DB, 0xCE416993, 0x8055050Au},
    {0x75F052DB, 0xBAE55B34, 0x8055050Au},
    {0x75F052DB, 0x45CD1D00, 0x8055050Au},
    {0x75F052DB, 0xC45E3FCD, 0x8055050Au},
    {0x75F052DB, 0xC2862B67, 0x8055050Au},
    {0x75F052DB, 0x40573917, 0x8055050Au},
    {0x75F052DB, 0x320C0277, 0x8055050Au},
    {0x75F052DB, 0x5EF44841, 0},
    {0xAE84BDEA, 0x8CA469F4, 0x8055050Au},
    {0xAE84BDEA, 0xEA588F23, 0x8055050Au},
    {0xAE84BDEA, 0xBB5E5E29, 0x8055050Au},
    {0xAE84BDEA, 0x0CFAC523, 0x8055050Au},
    {0xAE84BDEA, 0x883AC44A, 0x8055050Au},
    {0xAE84BDEA, 0x520975C3, 0x8055050Au},
    {0xAE84BDEA, 0xD26B9F4C, 0x8055050Au},
    {0xAE84BDEA, 0xF91887EC, 0x8055050Au},
    {0xAE84BDEA, 0xC003C588, 0x8055050Au},
    {0xAE84BDEA, 0x9FABA1E5, 0x8055050Au},
    {0xAE84BDEA, 0xA61AEDB0, 0x8055050Au},
    {0xAE84BDEA, 0x2E9A7FD0, 0x8055050Au},
    {0xAE84BDEA, 0xC4BB64A7, 0},
    {0xAE84BDEA, 0x44947D07, 0x8055050Au},
    {0xAE84BDEA, 0x518A64B9, 0x8055050Au},
    {0x7B47D9CF, 0x32B068C4, 0},
    {0x7B47D9CF, 0xE0EBFBF6, 0x8055050Au},
    {0x7B47D9CF, 0x081FA13C, 0},
    {0x7B47D9CF, 0x58D92EFD, 0},
    {0x7B47D9CF, 0x027587C4, 0},
    {0x7B47D9CF, 0x6A1BF429, 0x8055050Au},
    {0x7B47D9CF, 0xE5285E0F, 0x8055050Au},
    {0x7B47D9CF, 0xCF956F23, 0x8055050Au},
    {0x7B47D9CF, 0x36DA662E, 0},
    {0x7B47D9CF, 0xA30E4E17, 0x8055050Au},
    {0x7B47D9CF, 0xE956080F, 0x8055050Au},
    {0x7B47D9CF, 0x7183047C, 1},
    {0x7B47D9CF, 0x1075589C, 1},
    {0x7B47D9CF, 0x6690D8E2, 0},
    {0x7B47D9CF, 0xB4E5A189, 0},
    {0x7B47D9CF, 0x01284C6E, 0},
    {0x7B47D9CF, 0xBAE4CD1F, 0x8055050Au},
    {0x7B47D9CF, 0x30606C85, 0x8055050Au},
    {0x7B47D9CF, 0x40EC7271, 0x8055050Au},
    {0x7B47D9CF, 0x9F44CE45, 0},
    {0x7B47D9CF, 0x7D140F70, 0},
    {0x8134120B, 0x7E697F98, 0x80551E0Cu},
    {0x8134120B, 0xC86B6DA7, 0x80551E0Cu},
    {0x8134120B, 0x1DBD26D2, 0x80551E0Cu},
    {0x8134120B, 0x258DA4AC, 0},
    {0x8134120B, 0xF533A73A, 0},
    {0x8134120B, 0x35BE21C5, 0x80551E0Cu},
    {0x8134120B, 0x380230A1, 0},
    {0x1EF501B8, 0x9CF1BC76, 0x80552607u},
    {0x1EF501B8, 0xA537FF90, 0},
    {0x1EF501B8, 0xA3C9041D, 0x80552607u},
    {0x1EF501B8, 0x80557AA0, 0},
    {0x1EF501B8, 0xA71E6847, 0},
    {0xEE59FC02, 0x92FFBDE3, 0x80552705u},
    {0xEE59FC02, 0xF77EF683, 0x80552705u},
    {0xEE59FC02, 0x3B56E6C2, 0},
    {0xEE59FC02, 0xEAA4B1F3, 0},
    {0xEE59FC02, 0x51883EAE, 0x80552705u},
    {0xEE59FC02, 0x83B3DA0A, 0x80552705u},
    {0xEE59FC02, 0x4B6ACF47, 0},
    {0xEE59FC02, 0xBC892D18, 0},
    {0xB3B34742, 0xB6132502, 0},
    {0xB3B34742, 0x0560D9A1, 0x80552506u},
    {0xB3B34742, 0xFB200A6D, 0x80552506u},
    {0xB3B34742, 0x420C30E9, 0x80552506u},
    {0xB3B34742, 0xB304962C, 0},
    {0xB3B34742, 0x324C76F0, 0},
    {0xB3B34742, 0xF524AD2B, 0},
    {0x40120DC3, 0xE7486A60, 0},
    {0x40120DC3, 0x123E55F4, 0x80550F72u},
    {0x40120DC3, 0xFDB39774, 0x80550F72u},
    {0x40120DC3, 0xAEE8D3DF, 0x80550F72u},
    {0x40120DC3, 0xBBDDF866, 0x80550F72u},
    {0x40120DC3, 0x6CD27BD0, 0},
    {0x40120DC3, 0x9CD463F8, 0},
    {0x40120DC3, 0x24D3FE34, 0},
    {0x40120DC3, 0xADF3BD5B, 0},
    {0x40120DC3, 0xA4464754, 0},
    {0x40120DC3, 0x5CF665E0, 0x80550F72u},
    {0x40120DC3, 0xC47CDB59, 0x80550F72u},
    {0x40120DC3, 0x774F88D0, 0x80550F72u},
    {0x40120DC3, 0xB88DB7C2, 0x80550F72u},
    {0x40120DC3, 0x56B80FAF, 0x80550F72u},
    {0x40120DC3, 0xDE45F84F, 0x80550F72u},
    {0x40120DC3, 0x315B9BD0, 0x80550F72u},
    {0x40120DC3, 0x7894DCDF, 0x80550F72u},
    {0x40120DC3, 0xA308B496, 0x80550F72u},
    {0x40120DC3, 0x021DC797, 0x80550F72u},
    {0x40120DC3, 0x0550C016, 0x80550F72u},
    {0x40120DC3, 0x3B7C81A5, 0x80550F72u},
    {0x40120DC3, 0x88D236DF, 0x80550F72u},
    {0x40120DC3, 0xB0AFF6C9, 0x80550F72u},
    {0x40120DC3, 0x363922BA, 0x80550F72u},
    {0x40120DC3, 0xE81B8BAD, 0x80550F72u},
    {0x40120DC3, 0x4DFBCDD6, 0x80550F72u},
    {0x40120DC3, 0x18BDC4BD, 0x80550F72u},
    {0x40120DC3, 0xC594ADC2, 0x80550F72u},
    {0x40120DC3, 0xF6F23623, 0x80550F72u},
    {0x40120DC3, 0xFFC8B18B, 0},
    {0x40120DC3, 0xC73F209A, 0},
    {0x40120DC3, 0x5135EA31, 0},
    {0x40120DC3, 0x29186E27, 0},
    {0x40120DC3, 0x118E1C5E, 0},
    {0x40120DC3, 0x84E56A6D, 0},
    {0x40120DC3, 0x7132EAA5, 0x80550F72u},
    {0x40120DC3, 0xB99958AE, 0},
    {0x7106F303, 0xBB2E7559, 0x80550C2Bu},
    {0x7106F303, 0x506454DE, 0},
    {0x7106F303, 0xADF578E1, 0x80550C2Bu},
    {0x7106F303, 0x818A9499, 0x80550C2Bu},
    {0x7106F303, 0x368AA759, 0},
    {0x7106F303, 0x5BE8B86F, 0x80550C2Bu},
    {0x7106F303, 0xC61C79EE, 0x80550C2Bu},
    {0x7106F303, 0x28C643B7, 0x80550C2Bu},
    {0x7106F303, 0x65C0FEED, 0x80550C2Bu},
    {0x7106F303, 0x78521326, 0x80550C2Bu},
    {0x7106F303, 0xC086B560, 0x80550C2Bu},
    {0x7106F303, 0xEBB1FE74, 0},
    {0x7106F303, 0x33F7D5AE, 0x80550C2Bu},
    {0x7106F303, 0xA8021394, 0x80550C2Bu},
    {0x7106F303, 0xC8B0C9EE, 0x80550C2Bu},
    {0x7106F303, 0xF9E35566, 0},
    {0x7106F303, 0xF486991B, 0},
    {0x7106F303, 0xFA51949B, 0},
    {0x7106F303, 0x1C60BC5B, 0},
    {0x7106F303, 0xD48BAF13, 0x80550C2Bu},
    {0x7106F303, 0xE0BE0510, 0x80550C2Bu},
    {0x7106F303, 0x7B908D99, 0x80550C2Bu},
    {0x7106F303, 0xF3A43C50, 0},
    {0x7106F303, 0x8F88AC7E, 0x80550C2Bu},
    {0x7106F303, 0x94784A8C, 0x80550C2Bu},
    {0x7106F303, 0xD3096C55, 0x80550C2Bu},
    {0x7106F303, 0xF049BEC1, 0x80550C2Bu},
    {0x7106F303, 0x20598618, 0x80550C2Bu},
    {0x7106F303, 0x4E4C55BD, 0x80550C2Bu},
    {0x7106F303, 0x79310806, 0x80550C2Bu},
    {0x7106F303, 0x0124641C, 0},
    {0xE1AF627C, 0xEB3D14CE, 0},
    {0xE1AF627C, 0xEDA9C359, 0},
    {0xE1AF627C, 0x40341759, 1},
    {0xE1AF627C, 0x18CB54B5, 1},
    {0xE1AF627C, 0x6FC7357E, 0x80552907u},
    {0xE1AF627C, 0x2B61B0C7, 1},
    {0xE1AF627C, 0x6099B918, 0},
    {0xE1AF627C, 0x57B2826E, 0},
    {0xE1AF627C, 0xB0500FF4, 0x80552907u},
    {0xE1AF627C, 0xD585FDEA, 0x80552907u},
    {0xE1AF627C, 0x8519EB34, 1},
    {0xE1AF627C, 0xBA75BF6E, 0x80552907u},
    {0xE1AF627C, 0xDB04ECD6, 1},
    {0xE1AF627C, 0xF6B6C994, 1},
    {0xE1AF627C, 0x1E37862A, 0x80552907u},
    {0xE1AF627C, 0x001254C2, 0},
    {0xE1AF627C, 0xC9056736, 0},
    {0xE1AF627C, 0x06D28ADA, 0},
    {0x560D1608, 0x1AA394BA, 0x8010491Eu},
    {0x560D1608, 0x1AE3EC1C, 0x8010491Eu},
    {0x560D1608, 0x49A97D5F, 0x8010491Eu},
    {0x560D1608, 0x69EE6FB3, 0x8010491Eu},
    {0x560D1608, 0x8095FDEF, 0x8010491Eu},
    {0x560D1608, 0x88540CEC, 0x8010491Eu},
    {0x560D1608, 0x9B3F2DCE, 0x8010491Eu},
    {0x560D1608, 0x9FB9277B, 0x8010491Eu},
    {0x560D1608, 0xA412E9CA, 0x8010491Eu},
    {0x560D1608, 0xBF2BBE1F, 0x8010491Eu},
    {0x560D1608, 0xD22CB2DE, 0x8010491Eu},
    {0x560D1608, 0xDDEB6C8C, 0x8010491Eu},
    {0x560D1608, 0xE608000B, 0x8010491Eu},
    {0x560D1608, 0x88A68D20, 0},
    {0x560D1608, 0x2F35C196, 0},
    {0x560D1608, 0x80D6C552, 0x8010491Eu},
    {0x560D1608, 0x8CBEB2DA, 0x8010491Eu},
    {0x560D1608, 0x52021026, 0},
    {0x560D1608, 0xFF3BC581, 0},
    {0x560D1608, 0x3F3F6D92, 0},
    {0x560D1608, 0x773ABEA9, 0x8010491Eu},
    {0x560D1608, 0xEFE6F060, 0x8010491Eu},
    {0x560D1608, 0xA64D9D38, 0x8010491Eu},
    {0x560D1608, 0xDE6F0859, 0x8010491Eu},
    {0x560D1608, 0xDB8BAC35, 0x8010491Eu},
    {0x560D1608, 0xD393578E, 0x8010491Eu},
    {0x560D1608, 0xAEF3022A, 0x8010491Eu},
    {0x560D1608, 0xAD264F5F, 0x8010491Eu},
    {0x560D1608, 0x6C2E610F, 0x8010491Eu},
    {0x560D1608, 0xE75A5A9B, 0x8010491Eu},
    {0x560D1608, 0xBBCA5813, 0},
    {0x560D1608, 0xB22CB01D, 0x8010491Eu},
    {0x560D1608, 0xF3398774, 0x8010491Eu},
    {0x560D1608, 0xF8C15008, 0x8010491Eu},
    {0x560D1608, 0x06807CA3, 0x8010491Eu},
    {0x560D1608, 0x2364D6BD, 0x8010491Eu},
    {0x560D1608, 0xB14145DD, 0x8010491Eu},
    {0x560D1608, 0x2ED36EE2, 0},
    {0x560D1608, 0x146BE236, 0},
};

// An SceNpId with this online ID.
u32 np_id(const char* id) {
    const u32 at = scratch(36);
    std::memset(current_runtime()->memory().host(at), 0, 36);
    std::memcpy(current_runtime()->memory().host(at), id, std::strlen(id));
    return at;
}

}  // namespace

TEST_CASE("psn: every NP function the surveyed games import answers as a signed-out console's does") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 out = scratch(256);
    for (const Answer& a : kAnswers) {
        // (arguments: pointers to writable memory, so a check of them passes)
        const u32 r = u(call32(ctx, a.library, a.nid, {out, out + 64, out + 128, out + 192}));
        if (a.expected == 1) CHECK(static_cast<i32>(r) > 0);
        else CHECK(r == a.expected);
        if (a.expected == 1 ? static_cast<i32>(r) <= 0 : r != a.expected)
            std::fprintf(stderr, "library 0x%08x function 0x%08x: 0x%08x\n", a.library, a.nid, r);
    }
}

TEST_CASE("psn: NP IDs compared, the environment, the party state, an NP ID parsed, trophies without their file") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceNpCommon = 0xECB62224, kSceNpPartyGameUtil = 0xB3B34742, kSceNpWebApi = 0xE1AF627C,
                  kSceNpTrophy = 0x4332C10D, kSceNpActivity = 0xFEF492ED;
    const u32 a = np_id("alpha"), a2 = np_id("alpha"), b = np_id("beta"), order = scratch(4);
    CHECK(call32(ctx, kSceNpCommon, 0xFB8D82E5 /*sceNpCmpNpId*/, {a, a2}) == 0);
    CHECK(u(call32(ctx, kSceNpCommon, 0xFB8D82E5, {a, b})) == 0x80550609u);
    CHECK(u(call32(ctx, kSceNpCommon, 0xFB8D82E5, {a, 0})) == 0x80550601u);
    REQUIRE(call32(ctx, kSceNpCommon, 0x6BC8150A /*sceNpCmpNpIdInOrder*/, {a, b, order}) == 0);
    CHECK(static_cast<i32>(psv_ld32(order)) == -1);
    REQUIRE(call32(ctx, kSceNpCommon, 0x6BC8150A, {b, a, order}) == 0);
    CHECK(static_cast<i32>(psv_ld32(order)) == 1);
    REQUIRE(call32(ctx, kSceNpCommon, 0x6BC8150A, {a, a2, order}) == 0);
    CHECK(psv_ld32(order) == 0);
    const u32 env = scratch(16);
    REQUIRE(call32(ctx, kSceNpCommon, 0xAD6403E0 /*sceNpCommonGetNpEnviroment*/, {env}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(env))) == "np");
    REQUIRE(call32(ctx, kSceNpPartyGameUtil, 0x9F99ADF7 /*sceNpPartyGetState*/, {order}) == 0);
    CHECK(psv_ld32(order) == 2);  // (not in a party)
    const u32 parsed = scratch(36);
    REQUIRE(call32(ctx, kSceNpWebApi, 0x98CBE11F /*sceNpWebApiUtilityParseNpId*/, {guest_string("gamma"), parsed}) == 0);
    CHECK(call32(ctx, kSceNpCommon, 0xFB8D82E5, {parsed, np_id("gamma")}) == 0);
    CHECK(u(call32(ctx, kSceNpWebApi, 0x98CBE11F, {guest_string("an-online-id-too-long"), parsed})) == 0x80552902u);
    CHECK(call32(ctx, kSceNpTrophy, 0xD55C6F4C /*sceNpTrophyAbortHandle*/, {1}) == 0);
    for (u32 nid : {0xBA2B7F2Au, 0x087B0535u, 0xA4ADDD91u}) CHECK(u(call32(ctx, kSceNpTrophy, nid, {1, 1, 0, 0})) == 0x8055161Bu);
    for (u32 nid : {0xFE382529u, 0x1B8C3192u, 0x94BAB8D0u}) CHECK(u(call32(ctx, kSceNpTrophy, nid, {1, 1, 0, 0})) == 0x80551618u);
    CHECK(u(call32(ctx, kSceNpActivity, 0x6BE90416 /*sceNpActivityPostAppStartupStatus*/, {0})) == 0x80552306u);
}

TEST_CASE("psn: a trophy unlocked twice answers already unlocked") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceNpTrophy = 0x4332C10D, kUnlock = 0xB397AA24;
    const u32 platinum = scratch(4);
    REQUIRE(call32(ctx, kSceNpTrophy, kUnlock, {1, 1, 7, platinum}) == 0);
    CHECK(u(call32(ctx, kSceNpTrophy, kUnlock, {1, 1, 7, platinum})) == 0x8055160Fu);
}

TEST_CASE("psn: a trophy set's details come from the game's trophy pack when its TROP.SFM is plain") {
    // (a folder with app0:sce_sys/trophy/NPWR00001_00/TROPHY.TRP: the pack's header, one entry, TROP.SFM)
    nx::string base = "/tmp";
    if (auto v = nx::os::environment_variable("TEMP")) base = *v;
    else if (auto v2 = nx::os::environment_variable("TMPDIR")) base = *v2;
    const nx::string dir = nx::format("{}/psvrecore_trophy_test_{}", base, nx::os::process_id());
    const nx::string trophies = dir + "/sce_sys/trophy/NPWR00001_00";
    (void)nx::fs::remove_all(nx::fs::path_view(dir));
    REQUIRE(nx::fs::create_directories(nx::fs::path_view(trophies)).has_value());
    const nx::string xml =
        "<!--Sce-Np-Trophy-Signature: 00-->\n<trophyconf version=\"1.1\" platform=\"psp2\">\n"
        " <title-name>A Game &amp; More</title-name>\n <title-detail>Its trophies</title-detail>\n"
        " <group id=\"001\">\n  <name>Pack 1</name>\n  <detail>The first pack</detail>\n </group>\n"
        " <trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>All</name><detail>Every one</detail></trophy>\n"
        " <trophy id=\"001\" hidden=\"yes\" ttype=\"B\" pid=\"000\"><name>First</name><detail>One</detail></trophy>\n"
        " <trophy id=\"002\" hidden=\"no\" ttype=\"G\" pid=\"000\" gid=\"001\"><name>Packed</name><detail>Two</detail></trophy>\n"
        "</trophyconf>\n";
    nx::vector<u8> pack(0x80, 0);
    auto put32 = [&](usize at, u32 v) {
        for (int i = 0; i < 4; ++i) pack[at + i] = static_cast<u8>(v >> (24 - 8 * i));
    };
    put32(0, 0xDCA24D00u);
    put32(4, 2);
    put32(0x10, 1);     // (one entry)
    put32(0x14, 0x40);  // (of 64 bytes)
    std::memcpy(&pack[0x40], "TROP.SFM", 8);
    put32(0x40 + 36, 0x80);                           // (its offset, u64)
    put32(0x40 + 44, static_cast<u32>(xml.size()));  // (its size, u64)
    pack.insert(pack.end(), xml.begin(), xml.end());
    REQUIRE(nx::fs::file_write(nx::fs::path_view(trophies + "/TROPHY.TRP"), std::span<const u8>(pack)).has_value());

    TestRuntime t;
    t.rt.files().mount({"app0", dir, false});
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceNpTrophy = 0x4332C10D, kCreateContext = 0xC49FD33F, kGameInfo = 0xBA2B7F2A,
                  kGroupInfo = 0x087B0535, kTrophyInfo = 0xA4ADDD91;
    const u32 comm_id = scratch(12), context = scratch(4);
    std::memset(t.rt.memory().host(comm_id), 0, 12);
    std::memcpy(t.rt.memory().host(comm_id), "NPWR00001", 9);
    REQUIRE(call32(ctx, kSceNpTrophy, kCreateContext, {context, comm_id, 0, 0, 0}) == 0);
    const u32 c = psv_ld32(context);
    const u32 details = scratch(0x1C + 1152), data = scratch(0x20);
    REQUIRE(call32(ctx, kSceNpTrophy, kGameInfo, {c, 1, details, data}) == 0);
    CHECK(psv_ld32(details + 4) == 1);   // (groups besides the base game)
    CHECK(psv_ld32(details + 8) == 3);   // (trophies)
    CHECK(psv_ld32(details + 12) == 1);  // (platinum)
    CHECK(psv_ld32(details + 16) == 1);  // (gold)
    CHECK(psv_ld32(details + 24) == 1);  // (bronze)
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(details + 0x1C))) == "A Game & More");
    CHECK(psv_ld32(data + 4) == 0);  // (none unlocked)
    REQUIRE(call32(ctx, kSceNpTrophy, kGroupInfo, {c, 1, 1, details, data}) == 0);
    CHECK(psv_ld32(details + 8) == 1);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(details + 0x1C))) == "Pack 1");
    REQUIRE(call32(ctx, kSceNpTrophy, kGroupInfo, {c, 1, 0xFFFFFFFFu, details, data}) == 0);  // (the base game)
    CHECK(psv_ld32(details + 8) == 2);
    CHECK(u(call32(ctx, kSceNpTrophy, kGroupInfo, {c, 1, 5, details, data})) == 0x8055160Du);
    REQUIRE(call32(ctx, kSceNpTrophy, kTrophyInfo, {c, 1, 1, details, data}) == 0);
    CHECK(psv_ld32(details + 4) == 1);
    CHECK(psv_ld32(details + 8) == 4);           // (bronze)
    CHECK(psv_ld32(details + 12) == 0xFFFFFFFFu);  // (the base game)
    CHECK(psv_ld32(details + 16) == 1);          // (hidden)
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(details + 0x14))) == "First");
    CHECK(u(call32(ctx, kSceNpTrophy, kTrophyInfo, {c, 1, 9, details, data})) == 0x8055160Eu);
    REQUIRE(call32(ctx, kSceNpTrophy, 0xB397AA24 /*sceNpTrophyUnlockTrophy*/, {c, 1, 2, scratch(4)}) == 0);
    REQUIRE(call32(ctx, kSceNpTrophy, kTrophyInfo, {c, 1, 2, details, data}) == 0);
    CHECK(psv_ld32(details + 12) == 1);  // (group 001)
    CHECK(psv_ld32(data + 8) == 1);      // (unlocked)
    REQUIRE(call32(ctx, kSceNpTrophy, kGameInfo, {c, 1, details, data}) == 0);
    CHECK(psv_ld32(data + 4) == 1);
    CHECK(psv_ld32(data + 0x18) == 33);  // (per cent)
    (void)nx::fs::remove_all(nx::fs::path_view(dir));
}

TEST_CASE("dialogs: the PSN, store and photo dialogs open and the player backs out; aborts end a running dialog") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceCommonDialog = 0xE537816C, kSceNpWebApiCommonDialog = 0x883CFD5C, kUpdate = 0x90530F2F;
    constexpr u32 kNotRunning = 0x80020404u, kNotInUse = 0x80020411u;
    struct Set {
        u32 library, init, status, result, term;
    };
    const Set sets[] = {
        {kSceCommonDialog, 0x4535A358, 0x2A0D060F, 0x7EC95C61, 0x7AB50F63},  // NP message
        {kSceCommonDialog, 0x5E0AFDF8, 0xFDFE6042, 0x749CAFFC, 0x04B63D6F},  // NP profile
        {kSceCommonDialog, 0x52ECD8A5, 0x7004BB2E, 0x07ED1E26, 0xB787F4B0},  // store checkout
        {kSceCommonDialog, 0x93FCFEC6, 0x1FD5D373, 0xD29FE607, 0x4A880C6A},  // friend list
        {kSceCommonDialog, 0x6821F09B, 0x147650E8, 0xA8682304, 0xDE1F3928},  // Facebook
        {kSceCommonDialog, 0x73EE7C9C, 0x032206D8, 0xD855414C, 0x7FE5BD77},  // photo import
        {kSceNpWebApiCommonDialog, 0x23C9840E, 0x114F831A, 0xC2B39806, 0xE183B9F3},  // game custom data
        {kSceNpWebApiCommonDialog, 0x8723A5F2, 0x5B735F60, 0xEA14FB1E, 0x4136382C},  // invitation
    };
    const u32 param = scratch(256), result = scratch(64), update = scratch(64);
    for (const Set& s : sets) {
        CHECK(call32(ctx, s.library, s.status, {}) == 0);  // (none)
        REQUIRE(call32(ctx, s.library, s.init, {param}) == 0);
        CHECK(call32(ctx, s.library, s.status, {}) == 1);  // (running)
        CHECK(u(call32(ctx, s.library, s.term, {})) == 0x80020410u);  // (not finished)
        REQUIRE(call32(ctx, kSceCommonDialog, kUpdate, {update}) == 0);
        CHECK(call32(ctx, s.library, s.status, {}) == 2);  // (finished)
        REQUIRE(call32(ctx, s.library, s.result, {result}) == 0);
        CHECK(psv_ld32(result) == 1);  // (user canceled)
        CHECK(call32(ctx, s.library, s.term, {}) == 0);
        CHECK(u(call32(ctx, s.library, s.term, {})) == kNotInUse);
    }
    // Aborts: a running dialog ends, aborted; one that does not run cannot be.
    for (u32 abort : {0x47AB6D04u, 0x2B02BE3Fu, 0x58FA2062u}) CHECK(u(call32(ctx, kSceCommonDialog, abort, {})) == kNotRunning);
    REQUIRE(call32(ctx, kSceCommonDialog, 0x4535A358, {param}) == 0);
    REQUIRE(call32(ctx, kSceCommonDialog, 0x47AB6D04 /*sceNpMessageDialogAbort*/, {}) == 0);
    REQUIRE(call32(ctx, kSceCommonDialog, 0x7EC95C61, {result}) == 0);
    CHECK(psv_ld32(result) == 2);  // (aborted)
    CHECK(call32(ctx, kSceCommonDialog, 0x7AB50F63, {}) == 0);
    std::memset(t.rt.memory().host(param), 0, 256);
    REQUIRE(call32(ctx, kSceCommonDialog, 0xA38A4A0D /*sceNetCheckDialogInit*/, {param}) == 0);
    REQUIRE(call32(ctx, kSceCommonDialog, 0x2D8EDF09 /*sceNetCheckDialogAbort*/, {}) == 0);
    CHECK(call32(ctx, kSceCommonDialog, 0x8027292A /*GetStatus*/, {}) == 2);
    CHECK(call32(ctx, kSceCommonDialog, 0x8BE51C15 /*Term*/, {}) == 0);
    for (u32 abort : {0x2D8EDF09u, 0xDC346979u, 0x013E7F74u, 0xC296D396u})
        CHECK(u(call32(ctx, kSceCommonDialog, abort, {})) == kNotRunning);
    for (u32 nid : {0x33626E2Cu, 0xB55CF0E3u, 0xB7D4C911u, 0xBEBAEF25u, 0xD1858A78u})
        CHECK(u(call32(ctx, kSceCommonDialog, nid, {})) == kNotInUse);
}

TEST_CASE("psn: online IDs compared, the platform type kept in an NP ID, statistics of memory never used") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceNpCommon = 0xECB62224, kSceNpMatching2 = 0x7106F303;
    const u32 a = scratch(36), b = scratch(36), info = scratch(24);
    std::memset(t.rt.memory().host(a), 0, 36);
    std::memset(t.rt.memory().host(b), 0, 36);
    std::memcpy(t.rt.memory().host(a), "player", 6);
    std::memcpy(t.rt.memory().host(b), "player", 6);
    CHECK(call32(ctx, kSceNpCommon, 0x8327C26F /*sceNpCmpOnlineId*/, {a, b}) == 0);
    t.rt.memory().host(b)[0] = 'q';
    CHECK(u(call32(ctx, kSceNpCommon, 0x8327C26F, {a, b})) == 0x80550609u);
    CHECK(call32(ctx, kSceNpCommon, 0xE807D0BC /*sceNpSetPlatformType*/, {a, 2}) == 0);
    CHECK(call32(ctx, kSceNpCommon, 0xE9A003DE /*sceNpGetPlatformType*/, {a}) == 2);
    CHECK(call32(ctx, kSceNpCommon, 0xE9A003DE, {b}) == 0);
    std::memset(t.rt.memory().host(info), 0xAA, 24);
    CHECK(call32(ctx, kSceNpMatching2, 0x7BD39E50 /*sceNpMatching2GetMemoryInfo*/, {info}) == 0);
    CHECK(psv_ld32(info + 8) == 0);
}

// ---- the system's dialogs drawn by a host (runtime/dialogs.hpp)

namespace {

constexpr u32 kDialogs = 0xE537816C, kUpdate = 0x90530F2F;
constexpr u32 kMsgInit = 0x755FF270, kMsgStatus = 0x4107019E, kMsgResult = 0xBB3BFC89, kMsgTerm = 0x81ACF695,
              kMsgClose = 0xC296D396, kBarSetValue = 0x9CDA5E0D, kBarInc = 0x7BE0E08B;

// (the host's side for one test: drawn while it lasts, answered at once again after)
struct Drawn {
    Drawn() { dialogs::set_drawn(true); }
    ~Drawn() { dialogs::set_drawn(false); }
};

// A message dialog's parameters: user message `buttons` and `text`, or a progress bar (mode 4).
u32 message_param(u32 mode, u32 buttons, const char* text) {
    const u32 p = scratch(160), user = scratch(48), bar = scratch(96);
    std::memset(current_runtime()->memory().host(p), 0, 160);
    std::memset(current_runtime()->memory().host(user), 0, 48);
    std::memset(current_runtime()->memory().host(bar), 0, 96);
    psv_st32(p + 80, mode);
    psv_st32(p + 84, user);
    psv_st32(p + 96, bar);
    psv_st32(user, buttons);
    psv_st32(user + 4, guest_string(text));
    psv_st32(bar + 44, guest_string(text));
    return p;
}

}  // namespace

TEST_CASE("dialogs: a message is answered at once without a host; drawn, it waits for the host's answer") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 update = scratch(64), result = scratch(48);
    // (no host: Yes, at its first update)
    REQUIRE(call32(ctx, kDialogs, kMsgInit, {message_param(1, 1, "Save?")}) == 0);
    REQUIRE(call32(ctx, kDialogs, kUpdate, {update}) == 0);
    CHECK(call32(ctx, kDialogs, kMsgStatus, {}) == 2);
    REQUIRE(call32(ctx, kDialogs, kMsgResult, {result}) == 0);
    CHECK(psv_ld32(result + 8) == 1);
    REQUIRE(call32(ctx, kDialogs, kMsgTerm, {}) == 0);
    DialogView v;
    CHECK_FALSE(dialogs::current(&v));

    Drawn drawn;
    REQUIRE(call32(ctx, kDialogs, kMsgInit, {message_param(1, 1, "Save?")}) == 0);
    for (int i = 0; i < 3; ++i) REQUIRE(call32(ctx, kDialogs, kUpdate, {update}) == 0);
    CHECK(call32(ctx, kDialogs, kMsgStatus, {}) == 1);  // (still up: the player has not answered)
    CHECK(dialogs::showing() == DialogView::Kind::Message);
    REQUIRE(dialogs::current(&v));
    CHECK(v.text == "Save?");
    CHECK(v.buttons == DialogView::Buttons::YesNo);
    dialogs::answer(v.serial + 1, DialogAnswer{0, 1});  // (not this dialog's: ignored)
    CHECK(call32(ctx, kDialogs, kMsgStatus, {}) == 1);
    dialogs::answer(v.serial, DialogAnswer{0, 2});  // (No)
    CHECK(call32(ctx, kDialogs, kMsgStatus, {}) == 2);
    CHECK_FALSE(dialogs::current(&v));
    REQUIRE(call32(ctx, kDialogs, kMsgResult, {result}) == 0);
    CHECK(psv_ld32(result + 4) == 0);
    CHECK(psv_ld32(result + 8) == 2);
    REQUIRE(call32(ctx, kDialogs, kMsgTerm, {}) == 0);

    // A progress bar: its value as the game sets it, closed by the game.
    REQUIRE(call32(ctx, kDialogs, kMsgInit, {message_param(4, 0, "Copying")}) == 0);
    REQUIRE(call32(ctx, kDialogs, kBarSetValue, {0, 40}) == 0);
    REQUIRE(call32(ctx, kDialogs, kBarInc, {0, 30}) == 0);
    CHECK(static_cast<u32>(call32(ctx, kDialogs, kBarInc, {1, 30})) == 0x80100A01u);  // (no such bar)
    REQUIRE(dialogs::current(&v));
    CHECK(v.progress);
    CHECK(v.percent == 70);
    CHECK(v.text == "Copying");
    CHECK(v.buttons == DialogView::Buttons::None);
    REQUIRE(call32(ctx, kDialogs, kMsgClose, {}) == 0);
    CHECK(call32(ctx, kDialogs, kMsgStatus, {}) == 2);
    REQUIRE(call32(ctx, kDialogs, kMsgTerm, {}) == 0);
}

TEST_CASE("dialogs: drawn, a save data list answers the slot chosen and the text input dialog the text typed") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Drawn drawn;
    constexpr u32 kSaveInit = 0xBF5248FA, kSaveStatus = 0x6E258046, kSaveSub = 0xBA0542CA, kSaveResult = 0xB2FF576E,
                  kSaveTerm = 0x2192A10A;
    const u32 p = scratch(160), list = scratch(64), slots = scratch(48), result = scratch(64);
    std::memset(t.rt.memory().host(p), 0, 160);
    std::memset(t.rt.memory().host(list), 0, 64);
    std::memset(t.rt.memory().host(slots), 0, 48);
    std::memset(t.rt.memory().host(result), 0, 64);
    for (u32 i = 0; i < 3; ++i) psv_st32(slots + 16 * i, 10 + i);
    psv_st32(p + 80, 2);  // (list)
    psv_st32(p + 84, 1);  // (save)
    psv_st32(p + 92, list);
    psv_st32(list, slots);
    psv_st32(list + 4, 3);
    psv_st32(list + 8, 8);  // (focus: by ID)
    psv_st32(list + 12, 11);
    psv_st32(list + 16, guest_string("Pick a slot"));
    REQUIRE(call32(ctx, kDialogs, kSaveInit, {p}) == 0);
    REQUIRE(call32(ctx, kDialogs, kUpdate, {scratch(64)}) == 0);
    CHECK(call32(ctx, kDialogs, kSaveSub, {}) == 1);  // (shown, not answered)
    DialogView v;
    REQUIRE(dialogs::current(&v));
    CHECK(v.kind == DialogView::Kind::SaveData);
    CHECK(v.title == "Pick a slot");
    REQUIRE(v.slots.size() == 3);
    CHECK(v.focus == 1);
    CHECK(v.slots[2].id == 12);
    CHECK(v.slots[2].title == "New Saved Data");
    dialogs::answer(v.serial, DialogAnswer{0, 1, 12});
    CHECK(call32(ctx, kDialogs, kSaveStatus, {}) == 2);
    REQUIRE(call32(ctx, kDialogs, kSaveResult, {result}) == 0);
    CHECK(psv_ld32(result) == 2);
    CHECK(psv_ld32(result + 4) == 0);
    CHECK(psv_ld32(result + 12) == 12);
    REQUIRE(call32(ctx, kDialogs, kSaveTerm, {}) == 0);

    // The text input dialog: its initial text shown, the typed text (UTF-8 to UTF-16, cut to its length) stored.
    constexpr u32 kImeInit = 0x1E7043BF, kImeStatus = 0xCF0431FD, kImeResult = 0x2EB3D046, kImeTerm = 0x838A3AF4;
    const u32 ip = scratch(0xA8), initial = scratch(16), buffer = scratch(32), title = scratch(16);
    std::memset(t.rt.memory().host(ip), 0, 0xA8);
    const char16_t kInitial[] = u"abc", kTitle[] = u"Name";
    std::memcpy(t.rt.memory().host(initial), kInitial, sizeof kInitial);
    std::memcpy(t.rt.memory().host(title), kTitle, sizeof kTitle);
    psv_st32(ip + 0x28, title);
    psv_st32(ip + 0x2C, 6);
    psv_st32(ip + 0x30, initial);
    psv_st32(ip + 0x34, buffer);
    REQUIRE(call32(ctx, kDialogs, kImeInit, {ip}) == 0);
    REQUIRE(call32(ctx, kDialogs, kUpdate, {scratch(64)}) == 0);
    CHECK(call32(ctx, kDialogs, kImeStatus, {}) == 1);
    REQUIRE(dialogs::current(&v));
    CHECK(v.kind == DialogView::Kind::Ime);
    CHECK(v.title == "Name");
    CHECK(v.ime_text == "abc");
    CHECK(v.max_length == 6);
    dialogs::answer(v.serial, DialogAnswer{0, 2, 0, "h\xC3\xA9llo world"});  // (cut to 6 units)
    CHECK(call32(ctx, kDialogs, kImeStatus, {}) == 2);
    const char16_t kTyped[] = u"héllo ";
    CHECK(std::memcmp(t.rt.memory().host(buffer), kTyped, sizeof kTyped) == 0);
    REQUIRE(call32(ctx, kDialogs, kImeResult, {result}) == 0);
    CHECK(psv_ld32(result) == 0);
    CHECK(psv_ld32(result + 4) == 2);
    REQUIRE(call32(ctx, kDialogs, kImeTerm, {}) == 0);
}

TEST_CASE("dialogs: drawn, a trophy unlocked is a notice for the host; without a host it is only logged") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceNpTrophy = 0x4332C10D, kUnlock = 0xB397AA24;
    SystemNotice n;
    while (dialogs::take_notice(&n)) {
    }
    REQUIRE(call32(ctx, kSceNpTrophy, kUnlock, {1, 1, 3, scratch(4)}) == 0);
    CHECK_FALSE(dialogs::has_notice());
    Drawn drawn;
    REQUIRE(call32(ctx, kSceNpTrophy, kUnlock, {1, 1, 4, scratch(4)}) == 0);
    REQUIRE(dialogs::take_notice(&n));
    CHECK(n.title == "Trophy unlocked");
    CHECK(n.text == "Trophy #4");
    CHECK_FALSE(dialogs::take_notice(&n));
}

namespace {
nx::vector<u32> g_keyboard_events, g_keyboard_carets, g_keyboard_changes;
void keyboard_handler(PsvContext* ctx, u32) {
    const u32 e = ctx->r[1];
    g_keyboard_events.push_back(psv_ld32(e));
    if (psv_ld32(e) == 1) g_keyboard_carets.push_back(psv_ld32(e + 12)), g_keyboard_changes.push_back(psv_ld32(e + 24));
}
}  // namespace

TEST_CASE("dialogs: drawn, the in-game keyboard's text as the player types it, then Enter, reach the game's handler") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Drawn drawn;
    constexpr u32 kSceIme = 0xD2553017, kOpen = 0x0E050613, kUpdate = 0x71D6898A, kSetText = 0xF3BD9A76,
                  kClose = 0x889A8421;
    GuestCode code;
    const u32 handler = code.add(&keyboard_handler);
    code.install();
    g_keyboard_events.clear(), g_keyboard_carets.clear(), g_keyboard_changes.clear();
    const u32 param = scratch(64), work = scratch(20 * 1024), buffer = scratch(2 * 48), initial = scratch(8);
    std::memset(t.rt.memory().host(param), 0, 64);
    const char16_t kHi[] = u"Hi";
    std::memcpy(t.rt.memory().host(initial), kHi, sizeof kHi);
    psv_st32(param + 28, work);
    psv_st32(param + 36, handler);
    psv_st32(param + 44, initial);
    psv_st32(param + 48, 16);
    psv_st32(param + 52, buffer);
    REQUIRE(call32(ctx, kSceIme, kOpen, {param}) == 0);
    DialogView v;
    REQUIRE(dialogs::current(&v));
    CHECK(v.kind == DialogView::Kind::InGameIme);
    CHECK(v.ime_text == "Hi");
    CHECK(v.caret == 2);
    CHECK(dialogs::showing() == DialogView::Kind::InGameIme);
    REQUIRE(call32(ctx, kSceIme, kUpdate, {}) == 0);  // (open)
    REQUIRE(call32(ctx, kSceIme, kUpdate, {}) == 0);  // (nothing typed yet: nothing)
    dialogs::edit(v.serial, "Hiya", 4);
    REQUIRE(call32(ctx, kSceIme, kUpdate, {}) == 0);
    const char16_t kHiya[] = u"Hiya";
    CHECK(std::memcmp(t.rt.memory().host(buffer), kHiya, sizeof kHiya) == 0);
    // (the game sets its text itself: the host takes it again)
    const char16_t kNew[] = u"Yo";
    const u32 text = scratch(sizeof kNew);
    std::memcpy(t.rt.memory().host(text), kNew, sizeof kNew);
    REQUIRE(call32(ctx, kSceIme, kSetText, {text, 2}) == 0);
    REQUIRE(dialogs::current(&v));
    CHECK(v.ime_text == "Yo");
    CHECK(v.revision == 1);
    dialogs::answer(v.serial, DialogAnswer{0, 2});  // (Enter)
    REQUIRE(call32(ctx, kSceIme, kUpdate, {}) == 0);
    REQUIRE(g_keyboard_events.size() == 3);
    CHECK(g_keyboard_events[0] == 0);  // (open)
    CHECK(g_keyboard_events[1] == 1);  // (the text updated)
    CHECK(g_keyboard_carets[0] == 4);
    CHECK(g_keyboard_changes[0] == 2);
    CHECK(g_keyboard_events[2] == 5);  // (Enter)
    CHECK_FALSE(dialogs::current(&v));  // (nothing shown after Enter, until it is opened again)
    CHECK(call32(ctx, kSceIme, kClose, {}) == 0);
}
