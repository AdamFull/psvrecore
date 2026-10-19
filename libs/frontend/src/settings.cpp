#include "psvrecore/frontend/settings.hpp"

#include <charconv>
#include <ctime>

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/serialization/ini.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::frontend {

namespace {

// SCE_SYSTEM_PARAM_LANG_* by index.
constexpr const char* kLanguages[] = {"japanese",   "english_us", "french",        "spanish",    "german",
                                      "italian",    "dutch",      "portuguese_pt", "russian",    "korean",
                                      "chinese_t",  "chinese_s",  "finnish",       "swedish",    "danish",
                                      "norwegian",  "polish",     "portuguese_br", "english_gb", "turkish"};
constexpr const char* kDateFormats[] = {"yyyy/mm/dd", "dd/mm/yyyy", "mm/dd/yyyy"};  // SCE_SYSTEM_PARAM_DATE_FORMAT_*
constexpr const char* kTimeFormats[] = {"12h", "24h"};                             // SCE_SYSTEM_PARAM_TIME_FORMAT_*
constexpr const char* kLogLevels[] = {"debug", "info", "warn", "error"};
constexpr const char* kOnScreen[] = {"auto", "always", "never"};

bool parse_bool(nx::string_view v, bool* out) {
    if (v == "true" || v == "on" || v == "yes" || v == "1") return *out = true, true;
    if (v == "false" || v == "off" || v == "no" || v == "0") return *out = false, true;
    return false;
}

bool parse_float(nx::string_view v, f32* out, f32 lo, f32 hi) {
    f32 parsed = 0;
    const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), parsed);
    if (error != std::errc{} || end != v.data() + v.size() || parsed < lo || parsed > hi) return false;
    *out = parsed;
    return true;
}

template <typename T>
bool parse_int(nx::string_view v, T* out) {
    T parsed{};
    const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), parsed);
    if (error != std::errc{} || end != v.data() + v.size()) return false;
    *out = parsed;
    return true;
}

template <usize N>
i32 index_of(const char* const (&names)[N], nx::string_view v) {
    for (usize i = 0; i < N; ++i)
        if (v == names[i]) return static_cast<i32>(i);
    return -1;
}

template <usize N>
nx::string choices(const char* const (&names)[N]) {
    nx::string s;
    for (usize i = 0; i < N; ++i) {
        if (i) s += ", ";
        s += names[i];
    }
    return s;
}

const char* yes_no(bool b) { return b ? "true" : "false"; }

// A key: its spelling, what it is, and how its value is read and written.
struct Key {
    const char* section;
    const char* key;
    const char* help;
    // (false: the value is not one the key takes; `expected` says what is)
    bool (*set)(Settings& s, nx::string_view v);
    nx::string (*get)(const Settings& s);
    const char* expected;
};

const Key kKeys[] = {
    {"game", "folder", "The game's application folder: eboot.bin, sce_module/ and the game's files.",
     [](Settings& s, nx::string_view v) { return s.game_folder = nx::string(v), true; },
     [](const Settings& s) { return s.game_folder; }, "a folder"},
    {"game", "data", "Where the game's ux0: and save data are kept (empty: the user's data folder).",
     [](Settings& s, nx::string_view v) { return s.data_folder = nx::string(v), true; },
     [](const Settings& s) { return s.data_folder; }, "a folder"},

    {"display", "fullscreen", "Full screen (F11 switches while playing).",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.fullscreen); },
     [](const Settings& s) { return nx::string(yes_no(s.fullscreen)); }, "true or false"},
    {"display", "scale", "The window's size, in screens of 960 x 544.",
     [](Settings& s, nx::string_view v) { return parse_int(v, &s.scale) && s.scale >= 1 && s.scale <= 8; },
     [](const Settings& s) { return nx::format("{}", s.scale); }, "1 to 8"},
    {"display", "vsync", "Present at the display's refresh (the game's pace is its own either way).",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.vsync); },
     [](const Settings& s) { return nx::string(yes_no(s.vsync)); }, "true or false"},
    {"display", "smooth", "Filter the picture when it is scaled (false: sharp pixels).",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.smooth); },
     [](const Settings& s) { return nx::string(yes_no(s.smooth)); }, "true or false"},
    {"display", "gpu", "The Vulkan device by index (-1: a discrete GPU first; --list-gpus lists them).",
     [](Settings& s, nx::string_view v) { return parse_int(v, &s.gpu) && s.gpu >= -1; },
     [](const Settings& s) { return nx::format("{}", s.gpu); }, "-1 or a device's index"},
    {"display", "validation", "Vulkan's validation layer, when installed (slow; for development).",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.validation); },
     [](const Settings& s) { return nx::string(yes_no(s.validation)); }, "true or false"},

    {"audio", "enabled", "Play the game's sound.",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.audio); },
     [](const Settings& s) { return nx::string(yes_no(s.audio)); }, "true or false"},
    {"audio", "latency_ms", "How far the sound may fall behind before it is dropped to catch up, in milliseconds.",
     [](Settings& s, nx::string_view v) {
         return parse_int(v, &s.audio_latency_ms) && s.audio_latency_ms >= 20 && s.audio_latency_ms <= 2000;
     },
     [](const Settings& s) { return nx::format("{}", s.audio_latency_ms); }, "20 to 2000"},

    {"controls", "on_screen",
     "The controls drawn on a touch screen: auto shows them once it is touched, until a gamepad or a key is used.",
     [](Settings& s, nx::string_view v) {
         const i32 i = index_of(kOnScreen, v);
         return i >= 0 && (s.on_screen = i, true);
     },
     [](const Settings& s) { return nx::string(kOnScreen[s.on_screen]); }, nullptr},
    {"controls", "opacity", "How solid they are drawn.",
     [](Settings& s, nx::string_view v) { return parse_float(v, &s.controls_opacity, 0.0f, 1.0f); },
     [](const Settings& s) { return nx::format("{}", s.controls_opacity); }, "0 to 1"},
    {"controls", "scale", "Their size.",
     [](Settings& s, nx::string_view v) { return parse_float(v, &s.controls_scale, 0.5f, 2.0f); },
     [](const Settings& s) { return nx::format("{}", s.controls_scale); }, "0.5 to 2"},

    {"system", "language", "The console's language.",
     [](Settings& s, nx::string_view v) {
         const i32 i = index_of(kLanguages, v);
         return i >= 0 && (s.language = i, true);
     },
     [](const Settings& s) { return nx::string(kLanguages[s.language]); }, nullptr},
    {"system", "user_name", "The user's name (up to 16 bytes of UTF-8 reach the game).",
     [](Settings& s, nx::string_view v) { return !v.empty() && (s.user_name = nx::string(v), true); },
     [](const Settings& s) { return s.user_name; }, "a name"},
    {"system", "date_format", "How dates are written.",
     [](Settings& s, nx::string_view v) {
         const i32 i = index_of(kDateFormats, v);
         return i >= 0 && (s.date_format = i, true);
     },
     [](const Settings& s) { return nx::string(kDateFormats[s.date_format]); }, nullptr},
    {"system", "time_format", "How times are written.",
     [](Settings& s, nx::string_view v) {
         const i32 i = index_of(kTimeFormats, v);
         return i >= 0 && (s.time_format = i, true);
     },
     [](const Settings& s) { return nx::string(kTimeFormats[s.time_format]); }, nullptr},
    {"system", "time_zone", "Minutes ahead of GMT, or auto: the host's time zone and summer time.",
     [](Settings& s, nx::string_view v) {
         if (v == "auto") return s.time_zone_auto = true, true;
         i32 m = 0;
         if (!parse_int(v, &m) || m < -12 * 60 || m > 14 * 60) return false;
         s.time_zone_auto = false, s.time_zone_minutes = m;
         return true;
     },
     [](const Settings& s) { return s.time_zone_auto ? nx::string("auto") : nx::format("{}", s.time_zone_minutes); },
     "auto or minutes from -720 to 840"},
    {"system", "summer_time", "Summer time (when time_zone is not auto).",
     [](Settings& s, nx::string_view v) { return parse_bool(v, &s.summer_time); },
     [](const Settings& s) { return nx::string(yes_no(s.summer_time)); }, "true or false"},

    {"log", "level", "How much is logged.",
     [](Settings& s, nx::string_view v) { return index_of(kLogLevels, v) >= 0 && (s.log_level = nx::string(v), true); },
     [](const Settings& s) { return s.log_level; }, nullptr},
    {"log", "file", "A file the log is written to as well (empty: none).",
     [](Settings& s, nx::string_view v) { return s.log_file = nx::string(v), true; },
     [](const Settings& s) { return s.log_file; }, "a file"},
};

// What each key takes, for settings screens: its kind, choices or range, whether it applies while the game runs, and
// whether it means anything on a phone.
struct Schema {
    const char* section;
    const char* key;
    SettingInfo::Kind kind;
    f32 lo, hi;
    bool live, desktop_only;
};

using K = SettingInfo::Kind;
const Schema kSchema[] = {
    {"game", "folder", K::Folder, 0, 0, false, true},       {"game", "data", K::Folder, 0, 0, false, true},
    {"display", "fullscreen", K::Bool, 0, 0, false, true},  {"display", "scale", K::Int, 1, 8, false, true},
    {"display", "vsync", K::Bool, 0, 0, false, false},      {"display", "smooth", K::Bool, 0, 0, true, false},
    {"display", "gpu", K::Int, -1, 8, false, true},         {"display", "validation", K::Bool, 0, 0, false, true},
    {"audio", "enabled", K::Bool, 0, 0, true, false},       {"audio", "latency_ms", K::Int, 20, 2000, false, false},
    {"controls", "on_screen", K::Choice, 0, 0, true, false}, {"controls", "opacity", K::Float, 0, 1, true, false},
    {"controls", "scale", K::Float, 0.5f, 2, true, false},  {"system", "language", K::Choice, 0, 0, false, false},
    {"system", "user_name", K::Text, 0, 0, false, false},   {"system", "date_format", K::Choice, 0, 0, false, false},
    {"system", "time_format", K::Choice, 0, 0, false, false}, {"system", "time_zone", K::Text, 0, 0, false, false},
    {"system", "summer_time", K::Bool, 0, 0, false, false}, {"log", "level", K::Choice, 0, 0, true, false},
    {"log", "file", K::Text, 0, 0, false, true},
};

template <usize N>
nx::vector<nx::string> list(const char* const (&names)[N]) {
    nx::vector<nx::string> out;
    for (const char* n : names) out.push_back(n);
    return out;
}

// A value as the file must spell it: quoted when it has what the grammar would read otherwise (a path with ';' or
// '#', spaces at the ends).
nx::string file_value(const nx::string& value) {
    const bool quote = value.view().find_first_of(";#\"\\") != nx::string_view::npos ||
                       (!value.empty() && (value.front() == ' ' || value.back() == ' '));
    if (!quote) return value;
    nx::string escaped = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') escaped.push_back('\\');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

const Key* find_key(nx::string_view section, nx::string_view key) {
    for (const Key& k : kKeys)
        if (section == k.section && key == k.key) return &k;
    return nullptr;
}

nx::string expected(const Key& k) {
    if (k.expected) return nx::string(k.expected);
    if (nx::string_view(k.key) == "language") return "one of " + choices(kLanguages);
    if (nx::string_view(k.key) == "date_format") return "one of " + choices(kDateFormats);
    if (nx::string_view(k.key) == "time_format") return "one of " + choices(kTimeFormats);
    if (nx::string_view(k.key) == "on_screen") return "one of " + choices(kOnScreen);
    return "one of " + choices(kLogLevels);
}

}  // namespace

Result<void> set(Settings* settings, nx::string_view section, nx::string_view key, nx::string_view value) {
    const Key* k = find_key(section, key);
    if (!k) return failure(nx::format("no setting {}.{}", section, key));
    Settings changed = *settings;
    if (!k->set(changed, value))
        return failure(nx::format("{}.{} = {}: expected {}", section, key, value, expected(*k)));
    *settings = std::move(changed);
    return {};
}

Result<void> set(Settings* settings, nx::string_view assignment) {
    const usize dot = assignment.find('.');
    const usize equals = assignment.find('=');
    if (dot == nx::string_view::npos || equals == nx::string_view::npos || dot > equals)
        return failure(nx::format("'{}': expected section.key=value", assignment));
    return set(settings, assignment.substr(0, dot), assignment.substr(dot + 1, equals - dot - 1),
               assignment.substr(equals + 1));
}

Result<void> apply_file_text(Settings* settings, nx::string_view text) {
    auto document = nx::ini::parse(text);
    if (!document)
        return failure(nx::format("line {}: {}", document.error().line, document.error().message()));
    for (const nx::ini::Entry& e : document->entries()) {
        if (auto r = set(settings, e.section.view(), e.key.view(), e.value.view()); !r) return r;
    }
    return {};
}

nx::string file_text(const Settings& settings) {
    nx::string text = "; Settings. The command line goes over them: --set section.key=value, or the options --help lists.\n";
    const char* section = "";
    for (const Key& k : kKeys) {
        if (nx::string_view(section) != k.section) {
            section = k.section;
            text += nx::format("\n[{}]\n", section);
        }
        nx::string_view help(k.help);
        if (!help.empty() && help.back() == '.') help = help.substr(0, help.size() - 1);
        text += nx::format("; {}: {}.\n", help, expected(k));
        text += nx::format("{} = {}\n", k.key, file_value(k.get(settings)));
    }
    return text;
}

nx::vector<SettingInfo> describe(const Settings& settings) {
    nx::vector<SettingInfo> out;
    for (const Key& k : kKeys) {
        SettingInfo info;
        info.section = k.section, info.key = k.key, info.help = k.help;
        info.value = k.get(settings);
        for (const Schema& s : kSchema) {
            if (nx::string_view(s.section) != k.section || nx::string_view(s.key) != k.key) continue;
            info.kind = s.kind, info.lo = s.lo, info.hi = s.hi, info.live = s.live, info.desktop_only = s.desktop_only;
        }
        const nx::string_view key(k.key);
        if (key == "language") info.choices = list(kLanguages);
        else if (key == "date_format") info.choices = list(kDateFormats);
        else if (key == "time_format") info.choices = list(kTimeFormats);
        else if (key == "on_screen") info.choices = list(kOnScreen);
        else if (key == "level") info.choices = list(kLogLevels);
        out.push_back(std::move(info));
    }
    return out;
}

Result<void> set_in_file(const nx::string& path, nx::string_view section, nx::string_view key, nx::string_view value) {
    const nx::fs::path_view file(path);
    nx::string text;
    if (nx::fs::exists(file)) {
        auto read = nx::fs::file_read_text(file);
        if (!read) return failure(nx::format("{}: cannot be read", path));
        text = std::move(*read);
    } else {
        text = file_text(Settings{});
    }
    Settings settings;
    if (auto r = apply_file_text(&settings, text.view()); !r) return failure(nx::format("{}: {}", path, r.error().message));
    if (auto r = set(&settings, section, key, value); !r) return r;
    auto changed = nx::ini::assign(text.view(), section, key, value);  // (it quotes what needs it)
    if (!changed) return failure(nx::format("{}: {}", path, changed.error().message()));
    if (!nx::fs::file_write_text_atomic(file, changed->view())) return failure(nx::format("{}: cannot be written", path));
    return {};
}

nx::string get(const Settings& settings, nx::string_view section, nx::string_view key) {
    const Key* k = find_key(section, key);
    return k ? k->get(settings) : nx::string();
}

i32 host_time_zone_minutes() {
    const std::time_t now = std::time(nullptr);
    std::tm utc = *std::gmtime(&now);
    utc.tm_isdst = 0;  // (the UTC fields read as local standard time: the difference is the standard offset)
    return static_cast<i32>(std::difftime(now, std::mktime(&utc)) / 60.0);
}

bool host_summer_time() {
    const std::time_t now = std::time(nullptr);
    return std::localtime(&now)->tm_isdst > 0;
}

}  // namespace psvrecore::frontend
