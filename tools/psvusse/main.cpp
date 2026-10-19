// psvusse — disassembles the programs of GXP files (compiled Vita shaders) in the reference disassembler's syntax:
// "Secondary program:" and "Primary program:" sections of "N: text" lines (scripts/usse_check.py compares them).

#include <cstdio>

#include "psvrecore/cli.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/usse/format.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

Result<void> run(nx::fs::path_view path) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    const auto gxp = usse::parse_gxp(*data);
    if (!gxp) return nx::err(gxp.error());
    nx::string out = "Secondary program:\n";
    out += usse::format_listing(usse::decode_program(gxp->secondary, usse::ProgramKind::Secondary),
                                usse::ProgramKind::Secondary);
    out += "Primary program:\n";
    out += usse::format_listing(usse::decode_program(gxp->primary, usse::ProgramKind::Primary), usse::ProgramKind::Primary);
    std::fputs(out.c_str(), stdout);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::vector<nx::string> files;
    nx::cli::Parser cli("psvusse", "Disassembles the shader programs of GXP files.");
    cli.root().positional_many("files", files, "GXP files");
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;
    int status = 0;
    for (const nx::string& file : files) {
        if (files.size() > 1) std::printf("== %s\n", file.c_str());
        if (const auto done = run(file); !done) {
            nx::loge(logs::tool, "psvusse: {}: {}", file, done.error().message);
            status = 1;
        }
    }
    return status;
}
