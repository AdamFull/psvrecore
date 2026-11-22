// Translates spin_module.hpp's synthetic module (guest threads waiting for each other through memory) for
// psvrecore_runtime_tests: spingen <out-dir>. Writes <out-dir>/spin_mod/spin_mod_*.

#include <cstdio>

#include "psvrecore/io/file.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "spin_module.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::spin_test;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: spingen <out-dir>\n");
        return 2;
    }
    const auto image = spin_image();
    const auto program = spin_program(image);
    for (const auto& d : program.diagnostics)
        if (d.severity == analysis::Severity::Error) {
            std::fprintf(stderr, "spingen: 0x%08x: %s\n", d.address, d.message.c_str());
            return 1;
        }
    recomp::Options gen;
    gen.name = "spin_mod";
    const auto output = recomp::generate(program, gen);
    const nx::string sub = nx::fs::path_view(argv[1]) / nx::fs::path_view("spin_mod");
    if (!nx::fs::create_directories(nx::fs::path_view(sub))) {
        std::fprintf(stderr, "spingen: cannot create %s\n", sub.c_str());
        return 1;
    }
    for (const auto& f : output.files)
        if (auto ok = io::write_text_file(nx::fs::path_view(sub) / nx::fs::path_view(f.name), f.text); !ok) {
            std::fprintf(stderr, "spingen: %s\n", ok.error().message.c_str());
            return 1;
        }
    return 0;
}
