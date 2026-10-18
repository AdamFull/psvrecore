// psvdis — disassembles ARM or Thumb code from a raw file: one line per instruction, "<address>\t<text>", in the
// syntax llvm-objdump prints (scripts/arm_check.py compares the two). Thumb is decoded linearly, following IT
// blocks.

#include <cstdio>
#include <cstring>

#include "psvrecore/arm/decode.hpp"
#include "psvrecore/arm/format.hpp"
#include "psvrecore/cli.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/text.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

Result<void> run(nx::fs::path_view path, nx::string_view mode, u32 base) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    const u8* p = data->data();
    const usize size = data->size();
    nx::string out;
    out.reserve(size * 12);
    const auto line = [&](u32 address, const arm::Instruction& in) {
        out += nx::format("{:08x}\t{}\n", address, arm::format(in));
        if (out.size() > (1u << 20)) {
            std::fputs(out.c_str(), stdout);
            out.clear();
        }
    };
    if (mode == "arm") {
        for (usize at = 0; at + 4 <= size; at += 4) {
            u32 w;
            std::memcpy(&w, p + at, 4);
            line(base + static_cast<u32>(at), arm::decode_arm(w, base + static_cast<u32>(at)));
        }
    } else if (mode == "thumb") {
        arm::ItState it;
        for (usize at = 0; at + 2 <= size;) {
            u16 hw1, hw2 = 0;
            std::memcpy(&hw1, p + at, 2);
            const bool wide = arm::thumb_is_32bit(hw1);
            if (wide && at + 4 > size) {  // a 32-bit instruction cut off by the end of the data
                out += nx::format("{:08x}\t<unknown>\n", base + static_cast<u32>(at));
                break;
            }
            if (wide) std::memcpy(&hw2, p + at + 2, 2);
            const u32 address = base + static_cast<u32>(at);
            const auto in = arm::decode_thumb(hw1, hw2, address, it);
            line(address, in);
            at += in.size;
        }
        if (size % 2) out += nx::format("{:08x}\t<unknown>\n", base + static_cast<u32>(size - 1));  // a last odd byte
    } else {
        return failure(nx::format("unknown mode '{}' (arm or thumb)", mode));
    }
    std::fputs(out.c_str(), stdout);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::string file, mode = "thumb", base = "0";
    nx::cli::Parser cli("psvdis", "Disassembles raw ARM or Thumb code.");
    cli.root().positional("file", file, "raw code")
        .option("mode", mode, "arm or thumb", {.value_name = "MODE"})
        .option("base", base, "address of the first byte (default 0)", {.value_name = "ADDRESS"});
    if (const int code = parse_command_line(cli, argc, argv); code >= 0) return code;
    const auto address = parse_u64(base, 0);
    if (!address || *address > 0xFFFFFFFFu) {
        nx::loge(logs::tool, "psvdis: bad base address '{}'", base);
        return 2;
    }
    if (const auto done = run(file, mode, static_cast<u32>(*address)); !done) {
        nx::loge(logs::tool, "psvdis: {}", done.error().message);
        return 1;
    }
    return 0;
}
