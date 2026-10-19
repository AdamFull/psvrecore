// psvusse — disassembles the programs of GXP files (compiled Vita shaders) in the reference disassembler's syntax:
// "Secondary program:" and "Primary program:" sections of "N: text" lines (scripts/usse_check.py compares them).
// With --params it prints the program's properties and parameters instead, one "key=value" line each
// (scripts/gxp_check.py compares them with libgxm's).

#include <cstdio>

#include "psvrecore/cli.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/usse/format.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

nx::string params(const usse::Gxp& g) {
    nx::string out = nx::format(
        "program type={} size={} discard={} depth_replace={} sprite_coord={} native_color={} frag_color={} "
        "default_uniform_buffer={} vertex_outputs={} fragment_inputs={}",
        static_cast<u32>(g.type), g.size, g.discard ? 1 : 0, g.depth_replace ? 1 : 0, g.sprite_coord ? 1 : 0,
        g.native_color ? 1 : 0, g.frag_color ? 1 : 0, g.default_uniform_buffer_size, g.vertex_outputs,
        g.fragment_inputs);
    if (g.type == usse::ShaderType::Fragment)
        out += nx::format(" output={},{}", static_cast<u32>(g.output_type), static_cast<u32>(g.output_components));
    out += '\n';
    for (usize i = 0; i < g.parameters.size(); ++i) {
        const usse::Parameter& p = g.parameters[i];
        out += nx::format(
            "param {} offset={} category={} name={} semantic={} semantic_index={} type={} components={} array={} "
            "resource={} container={} cube={} reg_format={}\n",
            i, p.offset, static_cast<u32>(p.category), p.named ? p.name : nx::string("-"),
            static_cast<u32>(p.semantic), static_cast<u32>(p.semantic_index), static_cast<u32>(p.type),
            static_cast<u32>(p.components), p.array_size, p.resource, static_cast<u32>(p.container),
            p.sampler_cube ? 1 : 0, p.reg_format ? 1 : 0);
    }
    return out;
}

Result<void> run(nx::fs::path_view path, bool show_params) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    const auto gxp = usse::parse_gxp(*data);
    if (!gxp) return nx::err(gxp.error());
    nx::string out;
    if (show_params) {
        out = params(*gxp);
    } else {
        out = "Secondary program:\n";
        out += usse::format_listing(usse::decode_program(gxp->secondary, usse::ProgramKind::Secondary),
                                    usse::ProgramKind::Secondary);
        out += "Primary program:\n";
        out += usse::format_listing(usse::decode_program(gxp->primary, usse::ProgramKind::Primary),
                                    usse::ProgramKind::Primary);
    }
    std::fputs(out.c_str(), stdout);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::vector<nx::string> files;
    bool show_params = false;
    nx::cli::Parser cli("psvusse", "Disassembles the shader programs of GXP files.");
    cli.root()
        .positional_many("files", files, "GXP files")
        .flag("params", show_params, "print the program's properties and parameters instead");
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;
    int status = 0;
    for (const nx::string& file : files) {
        if (files.size() > 1) std::printf("== %s\n", file.c_str());
        if (const auto done = run(file, show_params); !done) {
            nx::loge(logs::tool, "psvusse: {}: {}", file, done.error().message);
            status = 1;
        }
    }
    return status;
}
