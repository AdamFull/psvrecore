#include "psvrecore/frontend/settings.hpp"

#include <charconv>
#include <ctime>

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

bool parse_bool(nx::string_view v, bool* out) {
    if (v == "true" || v == "on" || v == "yes" || v == "1") return *out = true, true;
    if (v == "false" || v == "off" || v == "no" || v == "0") return *out = false, true;
    return false;
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
        const nx::string value = k.get(settings);
        // (quoted when it has what the grammar would read otherwise: a path with ';' or '#', spaces at the ends)
        const bool quote = value.view().find_first_of(";#\"\\") != nx::string_view::npos ||
                           (!value.empty() && (value.front() == ' ' || value.back() == ' '));
        if (quote) {
            nx::string escaped;
            for (char c : value) {
                if (c == '"' || c == '\\') escaped.push_back('\\');
                escaped.push_back(c);
            }
            text += nx::format("{} = \"{}\"\n", k.key, escaped);
        } else {
            text += nx::format("{} = {}\n", k.key, value);
        }
    }
    return text;
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
