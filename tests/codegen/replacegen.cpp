// Translates replace_module.hpp's synthetic module with its functions replaced by host implementations (kReplacements:
// rep_store by test_host_store, rep_load by test_host_load where test_host_load_guard holds, ...), for
// psvrecore_host_functions_tests: replacegen <out-dir>. Writes <out-dir>/rep_mod/rep_mod_*.

#include <cstdio>
#include <iterator>

#include "psvrecore/io/file.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "replace_module.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::replace_test;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: replacegen <out-dir>\n");
        return 2;
    }
    const auto image = replace_image();
    const auto program = replace_program(image);
    for (const auto& d : program.diagnostics)
        if (d.severity == analysis::Severity::Error) {
            std::fprintf(stderr, "replacegen: 0x%08x: %s\n", d.address, d.message.c_str());
            return 1;
        }
    recomp::Options gen;
    gen.name = "rep_mod";
    usize guarded = 0;
    for (const auto& r : kReplacements) gen.replacements.push_back({r.code, r.symbol, r.guard}), guarded += r.guard[0] != 0;
    const auto output = recomp::generate(program, gen);
    if (output.stats.replaced != std::size(kReplacements) || output.stats.replaced_guarded != guarded) {
        std::fprintf(stderr, "replacegen: %zu functions replaced (%zu with a guard), not %zu (%zu)\n", static_cast<size_t>(output.stats.replaced),
                     static_cast<size_t>(output.stats.replaced_guarded), std::size(kReplacements), static_cast<size_t>(guarded));
        for (const auto& n : output.stats.replacement_notes) std::fprintf(stderr, "replacegen: %s\n", n.c_str());
        return 1;
    }
    const nx::string sub = nx::fs::path_view(argv[1]) / nx::fs::path_view("rep_mod");
    if (!nx::fs::create_directories(nx::fs::path_view(sub))) {
        std::fprintf(stderr, "replacegen: cannot create %s\n", sub.c_str());
        return 1;
    }
    for (const auto& f : output.files)
        if (auto ok = io::write_text_file(nx::fs::path_view(sub) / nx::fs::path_view(f.name), f.text); !ok) {
            std::fprintf(stderr, "replacegen: %s\n", ok.error().message.c_str());
            return 1;
        }
    return 0;
}
