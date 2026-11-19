#pragma once

#include <optional>

// Where a game's files are in its folder. Either the application folder itself (eboot.bin at its top), or a copy of
// the console's ux0: folders for the title:
//
//   <folder>/app/<title ID>/        the game (eboot.bin, sce_module/, its files)
//   <folder>/patch/<title ID>/      an update (optional): its files over the game's in app0:
//   <folder>/addcont/<title ID>/    the add-ons (optional): one folder each, addcont0:
//
// The runtime mounts app0: as the update's folder over the game's, and addcont0: on the add-ons' folder (an empty
// device when there is none).

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::frontend {

struct GameLayout {
    nx::string app;      // the game's application folder
    nx::string update;   // the update's (empty: none)
    nx::string addcont;  // the add-ons' (empty: none)
    // The folder whose eboot.bin runs: the update's, when there is one.
    const nx::string& executable_folder() const { return update.empty() ? app : update; }
};

// The layout of `folder` for the game `title_id`, or an error saying what is missing.
Result<GameLayout> find_layout(const nx::string& folder, nx::string_view title_id);

// The game's title from its sce_sys/param.sfo (the update's, else the game's): TITLE_<language> (the console's
// language, SCE_SYSTEM_PARAM_LANG_*: TITLE_00 Japanese, TITLE_01 English, ...) when it has one, else TITLE. Empty
// when neither has a title.
nx::string game_title(const GameLayout& layout, i32 language);

// An integer of the game's sce_sys/param.sfo (the update's, else the game's): ATTRIBUTE2, ... Empty when neither
// has it.
std::optional<u32> game_sfo_integer(const GameLayout& layout, nx::string_view key);

}  // namespace psvrecore::frontend
