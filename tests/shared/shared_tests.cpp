// A program built as a game's executable is (psvrecore::game) on the shared runtime library: the start-up check, and
// what crosses the library's interface works both ways (strings and errors made on one side and freed on the other,
// the registry and the guest memory base generated code reads, the log, SDL's main on Windows). Built and run only
// with PSVRECORE_SHARED. No window, no game.

#include <SDL3/SDL_main.h>

#include <cstdio>

#include "psvrecore/abi.h"
#include "psvrecore/frontend/frontend.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/recomp/support.hpp"
#include "core/foundation/strings/format.h"

namespace {

using namespace psvrecore;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    if (!ok) ++g_failures;
}

void replacement(PsvContext*, uint32_t) {}

}  // namespace

int main(int argc, char** argv) {
    const char* const hidden[] = {"psvrecore_shared_tests", "--hidden"};
    check(psvrecore_abi_version() == PSVRECORE_ABI_VERSION, "the library is the ABI version the headers say");
    check(psvrecore_check_abi(PSVRECORE_ABI_VERSION, PSVRECORE_BUILD_SIGNATURE, 2, hidden) == 1,
          "the library accepts this program");
    check(psvrecore_check_abi(PSVRECORE_ABI_VERSION + 1, PSVRECORE_BUILD_SIGNATURE, 2, hidden) == 0,
          "the library refuses another ABI version");
    check(psvrecore_check_abi(PSVRECORE_ABI_VERSION, "another compiler", 2, hidden) == 0,
          "the library refuses another compiler");

    // A command line parsed in the library: its strings made there, freed here.
    frontend::Game game;
    game.name = "psvrecore_shared_tests";
    game.title = "psvrecore shared library tests";
    const char* const args[] = {"psvrecore_shared_tests", "--set", "display.internal_resolution=2", "--hidden",
                                "--log", "info"};
    {
        auto cl = frontend::parse_command_line(game, 6, args);
        check(cl && cl->hidden && cl->assignments.size() == 2 && cl->assignments[1] == "display.internal_resolution=2",
              "a command line parsed by the library");
    }
    {
        const char* const bad[] = {"psvrecore_shared_tests", "--no-such-option"};
        auto cl = frontend::parse_command_line(game, 2, bad);
        check(!cl && !cl.error().message.empty(), "an error made by the library, freed here");
    }

    // Files: a string made here written by the library, read back into a string the library made.
    const nx::string path = nx::format("{}.txt", args[0]);
    const nx::string text = nx::format("written by {} through psvrecore", args[0]);
    check(bool(io::write_text_file(nx::fs::path_view(path), text.view())), "a file written by the library");
    {
        auto back = io::read_text_file(nx::fs::path_view(path));
        check(back && *back == text, "the file read back by the library");
    }
    check(!io::read_file(nx::fs::path_view(nx::string("no/such/file"))), "a missing file reported");

    // The registry and the guest memory base: the library's, seen from here.
    check(psv_membase == nullptr, "no guest memory before a runtime starts (psv_membase imported)");
    check(recomp::override_function(0x81000001, replacement) == nullptr, "a function put in the registry");
    check(recomp::find_function(0x81000001) == replacement, "and found there");
    recomp::clear_modules();

    nx::logi(logs::frontend, "shared library tests: {} failed", g_failures);

    // The frontend's entry, as a game calls it (--help: it ends before opening anything).
    const char* const help[] = {"psvrecore_shared_tests", "--help"};
    check(frontend::run(game, 2, const_cast<char**>(help)) == 0, "the frontend's entry");

    (void)argc, (void)argv;
    std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
