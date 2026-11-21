// Host implementations of guest functions in the generator (recomp::Options::replacements, psvrecomp --replace): the
// text it writes for codegen/replace_module.hpp's synthetic module. Every way of reaching a replaced function calls
// the host function (a direct call, a tail branch, a table case; an indirect call finds f_<address>, which calls it),
// none through an internal entry or an inlined copy; with a guard, f_<address> calls the guest code's translation
// (fo_<address>) where the guard does not hold. psvrecore_host_functions_tests runs the same module's translation.

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "nxtest.h"
#include "psvrecore/recomp/generator.hpp"
#include "replace_module.hpp"

using namespace psvrecore;
using namespace psvrecore::replace_test;

namespace {

struct Generated {
    nx::string text;  // every file's
    recomp::Stats stats;
};

Generated generate(nx::vector<recomp::Options::Replacement> replacements) {
    const auto image = replace_image();
    const auto program = replace_program(image);
    recomp::Options options;
    options.name = "rep_mod";
    options.replacements = std::move(replacements);
    auto output = recomp::generate(program, options);
    Generated g;
    for (const auto& f : output.files) g.text += f.text;
    g.stats = std::move(output.stats);
    return g;
}

bool has(const Generated& g, nx::string_view s) { return g.text.find(s) != nx::string::npos; }

}  // namespace

TEST_CASE("recomp: replace_module's functions are reached by internal entries and inlined copies without replacements") {
    const auto g = generate({});
    // (the premise of the test below: rep_store is a small function with an internal entry, inlined into its callers)
    CHECK(has(g, "fi_8100000E"));
    CHECK(g.stats.calls_inlined > 0);
    CHECK(!has(g, kStoreHost));
    CHECK(g.stats.replaced == 0);
    CHECK(g.stats.replaced_calls == 0);
    CHECK(g.stats.replaced_tails == 0);
}

TEST_CASE("recomp: every call, tail branch and table case of a replaced function calls the host function") {
    const auto g = generate({{kRepStore, kStoreHost, ""}, {kRepLoad, kLoadHost, kLoadGuard}});
    CHECK(g.stats.replaced == 2);
    CHECK(g.stats.replaced_guarded == 1);
    // No internal entry, no inlined copy, no guest code of the unguarded one.
    CHECK(!has(g, "fi_8100000E"));
    CHECK(!has(g, "fi_8100003A"));
    CHECK(!has(g, "fo_8100000E"));
    CHECK(!has(g, "/* 8100000e: str"));
    // f_<address> (the function table's, psv_call's): the host function.
    CHECK(has(g, "void f_8100000E(PsvContext* PSV_RESTRICT ctx, uint32_t ret) { test_host_store(ctx, ret); }"));
    // Direct calls (c_direct, c_relies), with the return address the guest's call has.
    CHECK(has(g, "test_host_store(ctx, 0x8100001Fu);"));
    CHECK(has(g, "test_host_store(ctx, 0x81000037u);"));
    // A tail branch (c_tail) and a table case (c_table): returning where the caller would.
    CHECK(has(g, "        ctx->r[1] = g1;\n        test_host_store(ctx, ret);\n        return;"));
    CHECK(has(g, "case 1u: test_host_store(ctx, ret); return;"));
    // An indirect call (c_indirect) goes through the module's lookup, to f_8100000E.
    CHECK(has(g, "rep_mod_call(ctx, target1, 0x8100002Du);"));
    CHECK(g.stats.replaced_calls == 3);  // (c_direct, c_relies; c_guarded's of the guarded one)
    CHECK(g.stats.replaced_tails == 2);
    // The guarded one: the guard, else the guest code's translation; its callers call f_<address>.
    CHECK(has(g, "void fo_8100003A(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {\n"));
    CHECK(has(g, "    if (test_host_load_guard(ctx)) { test_host_load(ctx, ret); return; }"));
    CHECK(has(g, "    fo_8100003A(ctx, ret);\n}"));
    CHECK(has(g, "f_8100003A(ctx, 0x81000047u);"));
    // The header declares what the module calls.
    CHECK(has(g, "void test_host_store(PsvContext* PSV_RESTRICT ctx, uint32_t ret);"));
    CHECK(has(g, "void test_host_load(PsvContext* PSV_RESTRICT ctx, uint32_t ret);"));
    CHECK(has(g, "int test_host_load_guard(const PsvContext* ctx);"));
    CHECK(has(g, "void fo_8100003A(PsvContext* PSV_RESTRICT ctx, uint32_t ret);"));
    // c_relies reads r2 after the call: what the guest's code leaves there, which the host function need not (AAPCS).
    CHECK(g.stats.replaced_relying == 1);
    bool listed = false;
    for (const auto& n : g.stats.replacement_notes) listed |= n.find("0x81000032") != nx::string::npos && n.find("r2") != nx::string::npos;
    CHECK(listed);
}

TEST_CASE("recomp: replacements the generator cannot make are listed, and the module is translated as without them") {
    const auto plain = generate({});
    const auto g = generate({{kReplaceBase + 0x10 + 1, "test_nowhere", ""}});  // (inside rep_store: no function starts there)
    CHECK(g.stats.replaced == 0);
    REQUIRE(g.stats.replacement_notes.size() == 1);
    CHECK(g.stats.replacement_notes[0].find("no function starts there") != nx::string::npos);
    CHECK(!has(g, "test_nowhere"));
    CHECK(g.text == plain.text);
}

TEST_CASE("recomp: replaced functions' callers keep r4-r11 and sp across the call (the AAPCS's contract)") {
    const auto g = generate({{kRepStore, kStoreHost, ""}});
    // c_direct: r4 kept in its local across the host call (not read back), every dirty register in the context first.
    CHECK(has(g, "        ctx->r[4] = g4; ctx->r[13] = g13; ctx->r[14] = g14;\n        test_host_store(ctx, 0x8100001Fu);\n"
                 "        PSV_KEPT(ctx, 0x8100001Au, ctx->r[4] == g4 && ctx->r[13] == g13);\n        g0 = ctx->r[0];\n"));
}

TEST_CASE("recomp: every replacement of replace_module: guest code with inlined callees, loops, calls of itself; run-into, a constant jump") {
    nx::vector<recomp::Options::Replacement> all;
    for (const auto& r : kReplacements) all.push_back({r.code, r.symbol, r.guard});
    const auto g = generate(all);
    CHECK(g.stats.replaced == 6);
    CHECK(g.stats.replaced_guarded == 4);
    CHECK(g.stats.replacement_notes.size() == 1);  // (c_relies' read of r2)
    // rep_add3's guest code (fo_) calls add3's internal entry: its inlined copy is in the file.
    CHECK(has(g, "static inline uint32_t fi_8100004A(PsvContext* PSV_RESTRICT ctx, uint32_t g0) {"));
    CHECK(has(g, "        g0 = fi_8100004A(ctx, g0);\n"));
    // c_into runs into rep_inc: its host function, with ret.
    CHECK(has(g, "    /* runs into the next function */\n        ctx->r[0] = g0;\n        test_host_inc(ctx, ret);\n        return;"));
    // c_ldrpc jumps to rep_inc's address through the runtime (f_81000062: the host function).
    CHECK(has(g, "        const uint32_t target1 = 0x81000063u;\n        if (target1 == ret) return;\n        psv_jump(ctx, target1, ret, 0x81000066u);"));
    CHECK(has(g, "void f_81000062(PsvContext* PSV_RESTRICT ctx, uint32_t ret) { test_host_inc(ctx, ret); }"));
    // rep_dbl's guest code branches to its own entry: a loop in fo_, not a call.
    CHECK(has(g, "void fo_81000070(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {\n    PsvFlags fl = {0u, 0u, 0u, 0u};\n"
                 "    uint32_t g0 = ctx->r[0], g1 = ctx->r[1], g14 = ctx->r[14];\nL_81000070:;"));
    CHECK(has(g, "        goto L_81000070;"));
    // rep_tri's guest code calls itself: through f_<address>, the guard.
    CHECK(has(g, "        f_81000086(ctx, 0x81000093u);"));
    // (calls: c_direct, c_relies, c_guarded, c_add3, c_dbl, c_tri, rep_tri's own; tails: c_tail, c_table's case, c_into)
    CHECK(g.stats.replaced_calls == 7);
    CHECK(g.stats.replaced_tails == 3);
}

TEST_CASE("recomp: a function whose code reads flags at entry is not replaced") {
    // (f_flag reads C at entry; rep_load is replaced as usual)
    const auto g = generate({{kFlagFn, "test_nowhere", ""}, {kRepLoad, "test_nowhere", "test_nowhere_guard"}});
    CHECK(g.stats.replaced == 1);
    REQUIRE(g.stats.replacement_notes.size() == 1);
    CHECK(g.stats.replacement_notes[0] == "0x810000a7 (test_nowhere): its code reads flags --c- at entry; not replaced");
    CHECK(!has(g, "fo_810000A6"));
}

TEST_CASE("recomp: kRuntimeHostFunctions is guest.h's psv_host_* functions and guards") {
    std::ifstream in(PSVRECORE_GUEST_H);
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();
    const std::string guest_h = text.str();
    std::set<std::pair<std::string, bool>> declared, listed;
    const std::regex declaration(R"(PSVRECORE_API (void|int) (psv_host_\w+)\()");
    for (auto it = std::sregex_iterator(guest_h.begin(), guest_h.end(), declaration); it != std::sregex_iterator(); ++it)
        declared.insert({(*it)[2].str(), (*it)[1].str() == "int"});
    for (const auto& h : recomp::kRuntimeHostFunctions) listed.insert({h.name, h.guard});
    CHECK(declared.size() == 12);
    CHECK(declared == listed);
}
