#pragma once

// A game's settings: where the game and its data are, the window, sound, the console's system settings and logging.
// They come from defaults, then the settings file (INI: `[section]` and `key = value`), then the command line, each
// over the one before. Every key has one spelling in the file and on the command line (`--set section.key=value`).

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::frontend {

struct Settings {
    // [game]
    nx::string game_folder;  // the game's application folder (eboot.bin at its top), or a copy of the console's
                             // app/, patch/ and addcont/ folders for it (see layout.hpp)
    nx::string data_folder;  // ux0: and save data (empty: the user's data folder)
    // [display]
    bool fullscreen = false;
    u32 scale = 1;  // the window's size in screens (960 x 544 each)
    bool vsync = true;
    bool smooth = true;  // linear filtering when scaled
    bool fps_overlay = false;  // the frame rate and a graph of the frame times over the picture
    bool launch_screen = true;  // the game's LiveArea picture (launch.png) while it starts, until its first frame
    u32 render_scale = 1;  // the internal resolution: the game's surfaces drawn at this many times their size
    i32 gpu = -1;        // the Vulkan device's index (-1: a discrete GPU first)
    bool validation = false;
    u32 capture_frame = 0;  // a RenderDoc capture of this frame of the game's, under RenderDoc (0: none; F12: the next)
    // [audio]
    bool audio = true;
    u32 audio_latency_ms = 200;  // the most a port may fall behind before what it holds is dropped
    // [controls] (on a touch screen)
    i32 on_screen = 0;          // 0 auto (once the screen is touched, until a gamepad or a key is used), 1 always, 2 never
    f32 controls_opacity = 0.5f;
    f32 controls_scale = 1.0f;
    // [system] (what the game reads as the console's settings)
    i32 language = 1;  // SCE_SYSTEM_PARAM_LANG_* (English, US)
    nx::string user_name = "User";
    i32 date_format = 1;  // SCE_SYSTEM_PARAM_DATE_FORMAT_* (day, month, year)
    i32 time_format = 1;  // SCE_SYSTEM_PARAM_TIME_FORMAT_* (24 hours)
    bool time_zone_auto = true;  // the host's (else time_zone_minutes)
    i32 time_zone_minutes = 0;
    bool summer_time = false;
    i32 dialogs = 0;  // the system's dialogs: 0 auto (drawn; answered at once in --input and --hidden runs), 1 drawn,
                      // 2 answered at once, as a player answers most often (runtime/dialogs.hpp)
    // [network]
    bool adhoc = true;           // the game's ad hoc mode over the host's local network (else offline)
    nx::string adhoc_interface;  // the interface it uses: empty auto, a name (wlan0, Wi-Fi), an IPv4 address, or a
                                 // loopback address (127.0.0.2: instances on one machine)
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

// What a settings screen needs to know of each setting, in the file's order.
struct SettingInfo {
    enum class Kind : u8 { Bool, Int, Float, Choice, Text, Folder };
    nx::string section, key, help;
    Kind kind = Kind::Text;
    nx::vector<nx::string> choices;  // (Choice)
    f32 lo = 0, hi = 0;              // (Int, Float)
    bool live = false;               // applies while the game runs (else at its next start)
    bool desktop_only = false;       // means nothing on a phone (the window's size, the GPU, the log file, ...)
    nx::string value;                // as the file spells it
};
nx::vector<SettingInfo> describe(const Settings& settings);

// One value changed in a settings file, every other line kept as it was (comments included); a missing file is made
// with the defaults first. A refused value, or a file that does not read, is an error and changes nothing.
Result<void> set_in_file(const nx::string& path, nx::string_view section, nx::string_view key, nx::string_view value);

// The host's time zone, in minutes ahead of GMT (without summer time), and whether summer time is on now.
i32 host_time_zone_minutes();
bool host_summer_time();

}  // namespace psvrecore::frontend
