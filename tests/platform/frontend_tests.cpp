// The frontend: settings (the file's text both ways, values refused), the command line over the settings file,
// scripted controls, and where a game's files are in its folder.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <optional>

#include "nxtest.h"
#include "psvrecore/frontend/frontend.hpp"
#include "psvrecore/frontend/layout.hpp"
#include "psvrecore/frontend/script.hpp"
#include "shader_cache.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::frontend;

namespace {

nx::string temp_file(const char* name) {
    nx::string base = "/tmp";
    if (auto t = nx::os::environment_variable("TEMP")) base = *t;
    return nx::format("{}/psvrecore_{}_{}", base, nx::os::process_id(), name);
}

frontend::Game test_game() {
    frontend::Game g;
    g.name = "test";
    g.title = "Test";
    g.title_id = "TEST00000";
    return g;
}

Result<CommandLine> parse(std::initializer_list<const char*> args) {
    nx::vector<const char*> argv{"test"};
    argv.insert(argv.end(), args.begin(), args.end());
    return parse_command_line(test_game(), static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("settings: the file's text reads back as the settings it was written from") {
    Settings s;
    s.game_folder = "C:/Games/P4D; copy #2";  // (what the grammar would read as comments: quoted)
    s.data_folder = "";
    s.fullscreen = true;
    s.scale = 3;
    s.vsync = false;
    s.smooth = false;
    s.gpu = 1;
    s.audio = false;
    s.audio_latency_ms = 120;
    s.language = 18;
    s.user_name = "Ren \"Joker\"";
    s.date_format = 0;
    s.time_format = 0;
    s.time_zone_auto = false;
    s.time_zone_minutes = -300;
    s.summer_time = true;
    s.log_level = "debug";
    s.log_file = "logs/p4d.log";
    const nx::string text = file_text(s);
    Settings r;
    const auto applied = apply_file_text(&r, text.view());
    if (!applied) std::printf("%s\n%s\n", applied.error().message.c_str(), text.c_str());
    REQUIRE(applied);
    CHECK(r.game_folder == s.game_folder);
    CHECK(r.data_folder.empty());
    CHECK(r.fullscreen);
    CHECK(r.scale == 3);
    CHECK_FALSE(r.vsync);
    CHECK_FALSE(r.smooth);
    CHECK(r.gpu == 1);
    CHECK_FALSE(r.audio);
    CHECK(r.audio_latency_ms == 120);
    CHECK(r.language == 18);
    CHECK(r.user_name == s.user_name);
    CHECK(r.date_format == 0);
    CHECK(r.time_format == 0);
    CHECK_FALSE(r.time_zone_auto);
    CHECK(r.time_zone_minutes == -300);
    CHECK(r.summer_time);
    CHECK(r.log_level == "debug");
    CHECK(r.log_file == "logs/p4d.log");
    // The defaults' file gives the defaults.
    Settings d;
    REQUIRE(apply_file_text(&d, file_text(Settings{}).view()));
    CHECK(d.language == 1);
    CHECK(d.time_zone_auto);
    CHECK(d.scale == 1);
    CHECK(get(d, "system", "language") == "english_us");
    CHECK(get(d, "system", "date_format") == "dd/mm/yyyy");
}

TEST_CASE("settings: unknown keys and values a key does not take are refused, naming them") {
    Settings s;
    auto r = set(&s, "display", "colour", "red");
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "no setting display.colour");
    r = set(&s, "display.scale=0");
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "display.scale = 0: expected 1 to 8");
    CHECK_FALSE(set(&s, "display.scale=two"));
    CHECK_FALSE(set(&s, "display.fullscreen=maybe"));
    CHECK_FALSE(set(&s, "system.language=klingon"));
    CHECK_FALSE(set(&s, "system.time_zone=900"));
    CHECK_FALSE(set(&s, "system.user_name="));
    CHECK_FALSE(set(&s, "log.level=verbose"));
    CHECK_FALSE(set(&s, "scale=2"));  // (no section)
    CHECK(s.scale == 1);              // (nothing refused changed anything)
    REQUIRE(set(&s, "display.scale=2"));
    CHECK(s.scale == 2);
    REQUIRE(set(&s, "system.time_zone=60"));
    CHECK_FALSE(s.time_zone_auto);
    CHECK(s.time_zone_minutes == 60);
    REQUIRE(set(&s, "game.folder=D:/a=b"));  // (the first '=' ends the key)
    CHECK(s.game_folder == "D:/a=b");
    // In a file: the bad line's key is named.
    Settings f;
    r = apply_file_text(&f, "[display]\nscale = 2\n[audio]\nenabled = sometimes\n");
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "audio.enabled = sometimes: expected true or false");
    CHECK_FALSE(apply_file_text(&f, "[display\nscale = 2\n"));  // (does not parse)
}

TEST_CASE("command line: options stand for settings, --set has the last word, the file is under both") {
    auto c = parse({"--game", "G", "--fullscreen", "--scale", "2", "--no-audio", "--language", "french", "--set",
                    "display.scale=4", "--log", "info", "--timeout", "30", "--input", "100 quit", "--hidden"});
    REQUIRE(c);
    CHECK(c->timeout_s == 30);
    CHECK(c->hidden);
    CHECK(c->input == "100 quit");
    // (a settings file of its own, saying otherwise)
    const nx::string file = temp_file("settings.ini");
    REQUIRE(nx::fs::file_write_text_atomic(nx::fs::path_view(file),
                                           "[game]\nfolder = F\n[display]\nscale = 3\nvsync = false\n"));
    c->settings_file = file;
    nx::string used;
    auto s = resolve_settings(*c, "", &used);
    REQUIRE(s);
    CHECK(used == file);
    CHECK(s->game_folder == "G");  // (the command line over the file)
    CHECK(s->scale == 4);          // (--set over --scale and the file)
    CHECK_FALSE(s->vsync);         // (the file's, the command line saying nothing)
    CHECK(s->fullscreen);
    CHECK_FALSE(s->audio);
    CHECK(s->language == 2);
    CHECK(s->log_level == "info");
    std::remove(file.c_str());

    // A settings file named that is not there is an error; the default one is written instead.
    auto missing = parse({"--settings", "no/such/settings.ini"});
    REQUIRE(missing);
    CHECK_FALSE(resolve_settings(*missing, "", &used));
    const nx::string default_file = temp_file("default.ini");
    auto plain = parse({"--windowed"});
    REQUIRE(plain);
    s = resolve_settings(*plain, default_file, &used);
    REQUIRE(s);
    CHECK_FALSE(s->fullscreen);
    REQUIRE(nx::fs::exists(nx::fs::path_view(default_file)));
    auto written = nx::fs::file_read_text(nx::fs::path_view(default_file));
    REQUIRE(written);
    CHECK(*written == file_text(Settings{}));
    std::remove(default_file.c_str());

    // Errors: a value the option does not take, options at odds, unknown options; help.
    auto bad = parse({"--scale", "0"});
    REQUIRE(bad);  // (checked as a setting)
    CHECK_FALSE(resolve_settings(*bad, "", &used));
    CHECK_FALSE(parse({"--fullscreen", "--windowed"}));
    CHECK_FALSE(parse({"--frobnicate"}));
    CHECK_FALSE(parse({"--timeout", "soon"}));
    auto help = parse({"--help"});
    REQUIRE(help);
    CHECK(help->help);
    CHECK(help->help_text.view().find("--game") != nx::string_view::npos);
}

TEST_CASE("script: items by time, comments, and what each takes") {
    auto items = parse_script(
        "2000 key z up; 1000 key z down  # Cross\n"
        "1500 pad cross down;1500 axis leftx -32768;1600 pad dpup up\n"
        "# a line of comment\n"
        "  3000 touch 480 272 down ; 3100 touch 500.5 272 move; 3200 touch 500 272 up\n"
        "4000 shot menu\n5000 quit\n");
    if (!items) std::printf("%s\n", items.error().message.c_str());
    REQUIRE(items);
    REQUIRE(items->size() == 10);
    const auto& v = *items;
    CHECK(v[0].at_ms == 1000);
    CHECK(v[0].what == ScriptItem::What::Key);
    CHECK(v[0].down);
    CHECK(v[1].what == ScriptItem::What::Pad);
    CHECK(v[1].code == SDL_GAMEPAD_BUTTON_SOUTH);  // (the console's name)
    CHECK(v[2].what == ScriptItem::What::Axis);
    CHECK(v[2].value == -32768);
    CHECK(v[3].code == SDL_GAMEPAD_BUTTON_DPAD_UP);  // (SDL's)
    CHECK(v[4].at_ms == 2000);
    CHECK_FALSE(v[4].down);
    CHECK(v[5].what == ScriptItem::What::Touch);
    CHECK(v[5].phase == 0);
    CHECK(v[6].x == 500.5f);
    CHECK(v[6].phase == 1);
    CHECK(v[7].phase == 2);
    CHECK(v[8].what == ScriptItem::What::Shot);
    CHECK(v[8].name == "menu");
    CHECK(v[9].what == ScriptItem::What::Quit);
    CHECK(parse_script("")->empty());
    CHECK(parse_script(" ; # nothing\n;")->empty());
    for (const char* bad : {"key z down", "100 key nosuchkey down", "100 key z sideways", "100 pad nosuch down",
                            "100 axis leftx 40000", "100 touch 1 2", "100 touch 1 2 hover", "100 shot ../up",
                            "100 jump", "100 quit now"}) {
        auto r = parse_script(bad);
        CHECK_FALSE(r);
    }
    auto r = parse_script("100 key z down; 200 jump");
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "input '200 jump': expected key, pad, axis, touch, shot, wait, watch, unwatch or quit");
}

TEST_CASE("script: a wait holds what follows until a frame shows its colour; their times count from then") {
    // (written out of order: each stretch is sorted, and a wait ends its own)
    auto items = parse_script("200 shot b; 100 shot a; 300 wait 480 272 ff8000 10; 50 shot d; 20 shot c; 0 wait 1 2 000000");
    if (!items) std::printf("%s\n", items.error().message.c_str());
    REQUIRE(items);
    REQUIRE(items->size() == 6);
    const auto& v = *items;
    CHECK(v[0].name == "a");
    CHECK(v[1].name == "b");
    CHECK(v[2].what == ScriptItem::What::Wait);
    CHECK(v[2].x == 480.0f);
    CHECK(v[2].rgb == 0xFF8000u);
    CHECK(v[2].tolerance == 10);
    CHECK(v[3].name == "c");
    CHECK(v[5].tolerance == 24);
    for (const char* bad : {"1 wait 1 2", "1 wait 1 2 ff80", "1 wait 1 2 gg8000", "1 wait 1 2 ff8000 300",
                            "1 wait 1 2 ff8000 press", "1 wait 1 2 ff8000 press nosuch 500", "1 wait 1 2 ff8000 press cross 50",
                            "1 wait 1 2 ff8000 10 press cross 500 more"})
        CHECK_FALSE(parse_script(bad));
    auto pressing = parse_script("0 wait 1 2 ff8000 press cross 500; 0 wait 1 2 ff8000 8 press circle 300");
    REQUIRE(pressing);
    CHECK((*pressing)[0].press == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK((*pressing)[0].press_every == 500);
    CHECK((*pressing)[1].tolerance == 8);
    CHECK((*pressing)[1].press == SDL_GAMEPAD_BUTTON_EAST);

    Script script(*items);
    REQUIRE(script.probes().size() == 2);
    nx::vector<nx::string> shots;
    std::optional<u32> colour;  // (the probed pixel: none until a frame is shown)
    auto shot = [&](const nx::string& name) { shots.push_back(name); };
    auto pixel = [&](f32, f32) { return colour; };
    CHECK(script.play(250, nullptr, shot, pixel));
    CHECK(shots.size() == 2);  // a, b
    CHECK(script.play(1000, nullptr, shot, pixel));  // (no frame yet: held)
    colour = 0xF08A05u;  // (not within 10 of ff8000)
    CHECK(script.play(2000, nullptr, shot, pixel));
    CHECK(shots.size() == 2);
    colour = 0xFA7F08u;
    CHECK(script.play(3000, nullptr, shot, pixel));  // met at 3000: c is due at 3020, d at 3050
    CHECK(shots.size() == 2);
    CHECK(script.play(3030, nullptr, shot, pixel));
    REQUIRE(shots.size() == 3);
    CHECK(shots[2] == "c");
    CHECK(script.play(3060, nullptr, shot, pixel));
    CHECK(shots.size() == 4);
    CHECK_FALSE(script.finished());  // (the last wait: 000000 not shown)
    colour = 0x101010u;
    CHECK(script.play(3070, nullptr, shot, pixel));
    CHECK(script.finished());
}

TEST_CASE("script: watches parse, and look at their points alongside the rest") {
    auto items = parse_script("100 watch 790 121 ffdc31 30 press triangle; 100 watch 135 272 ffdc31 30 press left; 5000 unwatch");
    if (!items) std::printf("%s\n", items.error().message.c_str());
    REQUIRE(items);
    REQUIRE(items->size() == 3);
    CHECK((*items)[0].what == ScriptItem::What::Watch);
    CHECK((*items)[0].press == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK((*items)[1].press == SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    CHECK((*items)[2].what == ScriptItem::What::Unwatch);
    for (const char* bad : {"1 watch 1 2 ffdc31 30", "1 watch 1 2 ffdc31 press cross", "1 watch 1 2 ffdc31 30 press nosuch",
                            "1 unwatch now"})
        CHECK_FALSE(parse_script(bad));
    Script script(*items);
    CHECK(script.probes().size() == 2);
    // (without a window or SDL's events running, the presses go nowhere: the script just goes on)
    auto pixel = [&](f32, f32) -> std::optional<u32> { return 0xFFDC31u; };
    auto shot = [&](const nx::string&) {};
    CHECK(script.play(200, nullptr, shot, pixel));
    CHECK(script.play(6000, nullptr, shot, pixel));
    CHECK(script.finished());
}

TEST_CASE("settings: described for a settings screen, every key with what it takes") {
    Settings s;
    s.language = 18;
    const auto all = describe(s);
    REQUIRE(all.size() >= 20);
    usize live = 0, phone = 0;
    for (const SettingInfo& i : all) {
        CHECK_FALSE(i.help.empty());
        if (i.kind == SettingInfo::Kind::Choice) {
            CHECK_FALSE(i.choices.empty());
            CHECK(std::find(i.choices.begin(), i.choices.end(), i.value) != i.choices.end());
        }
        if (i.kind == SettingInfo::Kind::Int || i.kind == SettingInfo::Kind::Float) CHECK(i.hi > i.lo);
        // (each value is one the key takes)
        Settings t;
        CHECK(set(&t, i.section.view(), i.key.view(), i.value.view()));
        live += i.live;
        phone += !i.desktop_only;
    }
    CHECK(live >= 5);
    CHECK(phone >= 12);
    auto language = std::find_if(all.begin(), all.end(), [](const SettingInfo& i) { return i.key == "language"; });
    REQUIRE(language != all.end());
    CHECK(language->value == "english_gb");
}

TEST_CASE("settings: one value changed in the file, its comments and other lines kept") {
    const nx::string file = temp_file("edit.ini");
    const nx::string text = "; mine\n[display]\nsmooth = true ; keep this\nvsync = false\n\n[system]\nlanguage = french\n";
    REQUIRE(nx::fs::file_write_text_atomic(nx::fs::path_view(file), text));
    REQUIRE(set_in_file(file, "display", "smooth", "false"));
    REQUIRE(set_in_file(file, "controls", "opacity", "0.25"));  // (a section not there yet)
    REQUIRE(set_in_file(file, "system", "user_name", "Rise; #1"));
    auto now = nx::fs::file_read_text(nx::fs::path_view(file));
    REQUIRE(now);
    CHECK(now->view().find("; mine") != nx::string_view::npos);
    CHECK(now->view().find("keep this") != nx::string_view::npos);
    CHECK(now->view().find("vsync = false") != nx::string_view::npos);
    Settings s;
    REQUIRE(apply_file_text(&s, now->view()));
    CHECK_FALSE(s.smooth);
    CHECK_FALSE(s.vsync);
    CHECK(s.language == 2);
    CHECK(s.controls_opacity == 0.25f);
    CHECK(s.user_name == "Rise; #1");
    // Refused: nothing written.
    auto bad = set_in_file(file, "display", "scale", "9");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message == "display.scale = 9: expected 1 to 8");
    CHECK(*nx::fs::file_read_text(nx::fs::path_view(file)) == *now);
    std::remove(file.c_str());
    // A missing file: made with the defaults, then changed.
    REQUIRE(set_in_file(file, "audio", "enabled", "false"));
    Settings d;
    REQUIRE(apply_file_text(&d, nx::fs::file_read_text(nx::fs::path_view(file))->view()));
    CHECK_FALSE(d.audio);
    CHECK(d.scale == 1);
    std::remove(file.c_str());
}

TEST_CASE("shader cache: kept from one run to the next; another translator's, or a record cut short, not used") {
    using namespace psvrecore::gxm::hle;
    const nx::string folder = temp_file("shader_cache");
    (void)nx::fs::remove_all(nx::fs::path_view(folder));
    const u8 program_a[] = {1, 2, 3}, program_b[] = {1, 2, 4};
    const ShaderKey a = shader_key({program_a}), b = shader_key({program_b});
    CHECK(a != b);
    CHECK(shader_key({program_a}) == a);
    // (the parts' borders count: {1, 2} + {3} is not {1} + {2, 3})
    const u8 p12[] = {1, 2}, p3[] = {3}, p1[] = {1}, p23[] = {2, 3};
    CHECK(shader_key({p12, p3}) != shader_key({p1, p23}));
    const u32 words_a[] = {0x07230203u, 1, 2, 3}, words_b[] = {0x07230203u, 9};
    {
        ShaderCache c;
        c.open(folder);
        CHECK(c.spirv(a) == nullptr);
        c.add_spirv(a, words_a);
        c.add_spirv(b, words_b);
        c.add_spirv(a, words_b);  // (a key already there: kept as it was)
        ShaderCache::Graphics g{};
        g.desc = vulkan::GraphicsPipelineDesc::defaults();
        g.desc.depth_test = 1;
        g.vertex = a, g.fragment = b;
        c.add_graphics(g);
        c.add_graphics(g);
        c.add_compute(b);
    }
    {
        ShaderCache c;
        c.open(folder);
        REQUIRE(c.spirv(a));
        CHECK(*c.spirv(a) == nx::vector<u32>(std::begin(words_a), std::end(words_a)));
        REQUIRE(c.spirv(b));
        CHECK(c.spirv(b)->size() == 2);
        REQUIRE(c.graphics().size() == 1);
        CHECK(c.graphics()[0].desc.depth_test == 1);
        CHECK(c.graphics()[0].vertex == a);
        CHECK(c.compute().size() == 1);
    }
    // A record cut short (the process ended while writing): what came before it is kept.
    const nx::string file = folder + "/shaders.bin";
    {
        std::FILE* f = std::fopen(file.c_str(), "ab");
        REQUIRE(f);
        const u8 partial[] = {1, 200, 0, 0, 0, 7};
        std::fwrite(partial, 1, sizeof partial, f);
        std::fclose(f);
        ShaderCache c;
        c.open(folder);
        CHECK(c.spirv(a));
        CHECK(c.spirv_count() == 2);
    }
    // Another translator's file (its header's id differs): dropped, the cache starts again.
    {
        std::FILE* f = std::fopen(file.c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 8, SEEK_SET);
        std::fputc('#', f);
        std::fclose(f);
        ShaderCache c;
        c.open(folder);
        CHECK(c.spirv(a) == nullptr);
        CHECK(c.spirv_count() == 0);
        CHECK(c.graphics().empty());
    }
    (void)nx::fs::remove_all(nx::fs::path_view(folder));
}

TEST_CASE("layout: an application folder, or a copy of the console's app/, patch/ and addcont/ folders") {
    const nx::string root = temp_file("layout");
    (void)nx::fs::remove_all(nx::fs::path_view(root));
    const auto write = [](const nx::string& path) {
        (void)nx::fs::create_directories(nx::fs::path_view(nx::fs::path::parent_path(path.view())));
        (void)nx::fs::file_write_text(nx::fs::path_view(path), "x");
    };
    // The application folder itself.
    write(root + "/plain/eboot.bin");
    auto plain = find_layout(root + "/plain", "TEST00000");
    REQUIRE(plain.has_value());
    CHECK(plain->app == root + "/plain");
    CHECK(plain->update.empty());
    CHECK(plain->addcont.empty());
    CHECK(plain->executable_folder() == root + "/plain");

    // A copy of the console's folders: the game only, then with an update and add-ons.
    write(root + "/copy/app/TEST00000/eboot.bin");
    auto game_only = find_layout(root + "/copy", "TEST00000");
    REQUIRE(game_only.has_value());
    CHECK(game_only->app == root + "/copy/app/TEST00000");
    CHECK(game_only->update.empty());
    CHECK(game_only->addcont.empty());
    write(root + "/copy/patch/TEST00000/eboot.bin");
    write(root + "/copy/addcont/TEST00000/ADDCONTDLC00001/dlc.cpk");
    auto full = find_layout(root + "/copy", "TEST00000");
    REQUIRE(full.has_value());
    CHECK(full->update == root + "/copy/patch/TEST00000");
    CHECK(full->addcont == root + "/copy/addcont/TEST00000");
    CHECK(full->executable_folder() == full->update);

    // Another title's copy, an update without an executable, a folder that is neither.
    auto other = find_layout(root + "/copy", "OTHR00000");
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().message.view().find("app/OTHR00000/eboot.bin") != nx::string_view::npos);
    write(root + "/half/app/TEST00000/eboot.bin");
    (void)nx::fs::create_directories(nx::fs::path_view(root + "/half/patch/TEST00000"));
    auto half = find_layout(root + "/half", "TEST00000");
    REQUIRE(half.has_value());
    CHECK(half->update.empty());
    (void)nx::fs::create_directories(nx::fs::path_view(root + "/empty"));
    auto none = find_layout(root + "/empty", "TEST00000");
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().message.view().find("has no eboot.bin") != nx::string_view::npos);
    (void)nx::fs::remove_all(nx::fs::path_view(root));
}
