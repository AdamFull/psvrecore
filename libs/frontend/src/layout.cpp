#include "psvrecore/frontend/layout.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::frontend {

namespace {

nx::string join(nx::string_view a, nx::string_view b) {
    nx::string out(a);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    out += b;
    return out;
}

bool is_file(const nx::string& path) { return nx::fs::status(nx::fs::path_view(path)) == nx::fs::file_type::regular; }
bool is_directory(const nx::string& path) { return nx::fs::status(nx::fs::path_view(path)) == nx::fs::file_type::directory; }

// Why a file is not found (what the system says: a file there that cannot be read is not the same as none).
nx::string why_missing(const nx::string& path) {
    errno = 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    const int error = errno;
    if (f) std::fclose(f);
    return f ? nx::string("it opens, but is not found") : nx::string(std::strerror(error));
}

}  // namespace

Result<GameLayout> find_layout(const nx::string& folder, nx::string_view title_id) {
    GameLayout layout;
    if (is_file(join(folder, "eboot.bin"))) {
        layout.app = folder;
        return layout;
    }
    const nx::string app = join(join(folder, "app"), title_id);
    if (!is_file(join(app, "eboot.bin"))) {
        if (is_directory(join(folder, "app")))
            return failure(nx::format("{} has no app/{}/eboot.bin ({}): it is not a copy of this game's folders.",
                                      folder, title_id, why_missing(join(app, "eboot.bin"))));
        return failure(nx::format("{} has no eboot.bin ({}): it is not a game's application folder, nor a copy of the "
                                  "console's folders (app/{}/eboot.bin).",
                                  folder, why_missing(join(folder, "eboot.bin")), title_id));
    }
    layout.app = app;
    const nx::string update = join(join(folder, "patch"), title_id);
    if (is_file(join(update, "eboot.bin"))) layout.update = update;
    const nx::string addcont = join(join(folder, "addcont"), title_id);
    if (is_directory(addcont)) layout.addcont = addcont;
    return layout;
}

}  // namespace psvrecore::frontend
