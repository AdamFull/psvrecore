#pragma once

// A game's settings: where the game and its data are, the window, sound, the console's system settings and logging.
// They come from defaults, then the settings file (INI: `[section]` and `key = value`), then the command line, each
// over the one before. Every key has one spelling in the file and on the command line (`--set section.key=value`).

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::frontend {

struct Settings {
    // [game]
    nx::string game_folder;  // the game's application folder (app0:): eboot.bin, sce_module/, the game's files
    nx::string data_folder;  // ux0: and save data (empty: the user's data folder)
    // [display]
    bool fullscreen = false;
    u32 scale = 1;  // the window's size in screens (960 x 544 each)
    bool vsync = true;
    bool smooth = true;  // linear filtering when scaled
    i32 gpu = -1;        // the Vulkan device's index (-1: a discrete GPU first)
    bool validation = false;
    // [audio]
    bool audio = true;
    u32 audio_latency_ms = 200;  // the most a port may fall behind before what it holds is dropped
    // [system] (what the game reads as the console's settings)
    i32 language = 1;  // SCE_SYSTEM_PARAM_LANG_* (English, US)
    nx::string user_name = "User";
    i32 date_format = 1;  // SCE_SYSTEM_PARAM_DATE_FORMAT_* (day, month, year)
    i32 time_format = 1;  // SCE_SYSTEM_PARAM_TIME_FORMAT_* (24 hours)
    bool time_zone_auto = true;  // the host's (else time_zone_minutes)
    i32 time_zone_minutes = 0;
    bool summer_time = false;
    // [log]
    nx::string log_level = "warn";  // debug, info, warn or error
    nx::string log_file;            // also written there (empty: not)
};

// One value, by section and key; an error names what was wrong.
Result<void> set(Settings* settings, nx::string_view section, nx::string_view key, nx::string_view value);
// `section.key=value`.
Result<void> set(Settings* settings, nx::string_view assignment);
// A settings file's values over `settings` (an unknown key or a bad value is an error naming it).
Result<void> apply_file_text(Settings* settings, nx::string_view text);
// A settings file of `settings`, every key with a comment saying what it is and what it takes.
nx::string file_text(const Settings& settings);
// One value as the settings file spells it.
nx::string get(const Settings& settings, nx::string_view section, nx::string_view key);

// The host's time zone, in minutes ahead of GMT (without summer time), and whether summer time is on now.
i32 host_time_zone_minutes();
bool host_summer_time();

}  // namespace psvrecore::frontend
