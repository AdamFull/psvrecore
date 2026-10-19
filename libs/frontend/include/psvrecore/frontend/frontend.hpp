#pragma once

// The program a game becomes. A game repository describes the game (Game) and calls run() from its main(); run()
// reads the settings and the command line, opens the window and the audio device (psvrecore::platform), boots the
// game's recompiled code on the runtime with the GXM layer, and runs it until the window is closed, the game exits,
// or a time limit passes.
//
// Where things are kept: the settings file and the data folder (ux0:) are in the user's data folder for the program
// (SDL_GetPrefPath: %APPDATA%\psvrecore\<name>\ on Windows, ~/.local/share/psvrecore/<name>/ on Linux), unless the
// settings say otherwise. Save data is ux0:user/00/savedata/<title ID>, as on the console, mounted as savedata0:.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/frontend/settings.hpp"
#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::frontend {

struct Game {
    nx::string_view name;      // the program's: its command line, its user data folder ("p4danrecomp")
    nx::string_view title;     // the window's
    nx::string_view title_id;  // the game's (its save data is kept under it)
    std::span<const PsvModuleCode* const> code;  // its recompiled modules
    nx::string_view main = "app0:eboot.bin";
    std::span<const char* const> modules;  // started before the main thread ("app0:sce_module/libc.suprx", ...)
    // Checks the folder of the eboot.bin that runs (the update's, when there is one: see layout.hpp) before anything
    // starts, that it holds a version the code was generated from; an error is shown and the program ends.
    Result<void> (*check)(const nx::string& executable_folder) = nullptr;
};

// What the command line asks for besides settings.
struct CommandLine {
    nx::string settings_file;              // --settings (empty: the user's data folder's settings.ini)
    nx::vector<nx::string> assignments;    // section.key=value, from --set and the options that stand for one
    nx::string input;                      // --input: scripted controls, or @file
    nx::string out;                        // --out: where shots go
    u32 timeout_s = 0;                     // --timeout: end after this long (0: never)
    bool list_gpus = false;
    bool hidden = false;                   // --hidden: no window on screen, no message boxes (tests)
    bool help = false;                     // --help: `help_text` holds it
    nx::string help_text;
};

Result<CommandLine> parse_command_line(const Game& game, int argc, const char* const* argv);

// The settings: defaults, then the settings file, then the command line. A missing settings file is written with
// the defaults when it is the default one (so it can be edited); one named on the command line must exist.
Result<Settings> resolve_settings(const CommandLine& command_line, const nx::string& default_file,
                                  nx::string* file_used);

// Runs the game; returns the program's exit code (the process may also end in it: guest threads are not joined).
int run(const Game& game, int argc, char** argv);

}  // namespace psvrecore::frontend
