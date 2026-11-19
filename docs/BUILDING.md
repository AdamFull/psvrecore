# Building psvrecore

## Requirements

| | Windows | Linux | Android |
| --- | --- | --- | --- |
| Compiler | LLVM/Clang (clang-cl, MSVC ABI) with Visual Studio 2022's C++ tools and Windows SDK | Clang | Clang from the NDK (r27 or newer) |
| Build | CMake ≥ 3.27, Ninja (Visual Studio's bundled ones work) | CMake ≥ 3.27, Ninja | CMake ≥ 3.25, Ninja |

Submodules are required: `git submodule update --init --recursive`. That checks out nx-foundation and its own
third-party code, vita-headers (whose NID database the tools read at run time), and the Vulkan side:
Vulkan-Headers, SPIRV-Headers, volk and VulkanMemoryAllocator. Nothing links against a Vulkan SDK; the loader is
opened at run time. The Vulkan SDK's validation layer and `spirv-val` are used by the local checks only. The
media decoders come the same way: LibAtrac9 (MIT) and openh264 (BSD-2-Clause) as submodules; the PacketVideo AAC
decoder (Apache-2.0) is vendored in `third_party/pvaacdec`, minimp3 (CC0) in `third_party/minimp3`, and ScePvf's system fonts, Roboto
and Noto Sans JP, KR, SC and TC (SIL OFL 1.1), in `third_party/roboto` and `third_party/noto-cjk` (their READMEs say
where from). The platform layer's SDL3 (zlib) is a submodule as well, built as a static library, and so is Dear ImGui (MIT), the system UI's overlay, compiled into the platform library. (CMake 3.27 for the shared runtime library on
Windows and Linux; with `PSVRECORE_SHARED=OFF`, 3.25 is enough.)

## Presets

| Preset | Notes |
| --- | --- |
| `windows-x64-debug`, `windows-x64-release` | Need the MSVC environment. `scripts\build.ps1` imports it, and adds LLVM and Visual Studio's CMake/Ninja to `PATH` when they are not already there. |
| `linux-x64-debug`, `linux-x64-release` | `clang`/`clang++` on `PATH`. |
| `android-arm64-debug`, `android-arm64-release` | `ANDROID_NDK_HOME` must point at the NDK. `scripts\build.ps1` falls back to the newest NDK in `%ANDROID_HOME%` or `C:\Android\Sdk`. Tools and tests are off. |

Release presets build `RelWithDebInfo`. Build trees go to `build/<preset>`.

## Commands

Windows (PowerShell):

```
scripts\build.ps1                                    # windows-x64-debug: configure, build, test
scripts\build.ps1 -Preset windows-x64-release -NoTest
scripts\build.ps1 -Preset android-arm64-release
```

Linux:

```
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
ctest --preset linux-x64-release
```

The generator's synthetic corpus (`psvrecore_codegen_tests`, `psvrecore_codegen_masked_tests`) is checked in as
`tests/codegen/corpus_data.inc`. After changing a source in `tests/codegen/corpus/`, rebuild it with
`python scripts/codegen_corpus.py` (LLVM's clang and ld.lld, or the NDK's); `--check` tells whether it is up to
date. `scripts/codegen_metrics.py` measures the generated code ([CODEGEN_METRICS.md](CODEGEN_METRICS.md)).

`psvrecore_gpu_tests` needs a Vulkan device and is skipped without one (CI machines). It turns the validation
layer on when the Vulkan SDK's is installed. `PSVRECORE_TEST_GPU=<index>` picks the device; by default it is a
discrete GPU.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `PSVRECORE_BUILD_TOOLS` | ON (OFF on Android) | Host tools |
| `PSVRECORE_BUILD_TESTS` | ON (OFF on Android) | Unit tests |
| `PSVRECORE_SHARED` | ON (OFF on Android) | The runtime as one shared library games link (`psvrecore.dll`, `libpsvrecore.so.<ABI version>`): see [The shared runtime library](#the-shared-runtime-library). OFF links it into each game statically |
| `PSVRECORE_GUEST_FIBERS` | OFF | Guest threads as fibers on one host worker per console CPU, an experiment ([THREAD_BACKENDS.md](THREAD_BACKENDS.md)); `PSVRECORE_GUEST_THREADS=native` picks native threads in such a build, and `PSVRECORE_DETERMINISTIC=1` the deterministic scheduling (tests). Off, guest threads are OS threads |
| `PSVRECORE_TRACY` | OFF | Tracy (nx-foundation's copy of its client) built in: the runtime's zones, plots and frame marks, the game's CPU and GPU markers, the renderer's GPU zones ([PROFILING.md](PROFILING.md)). Off, the hooks compile to nothing. `PSVRECORE_PROFILER`, its earlier name, still turns it on |
| `PSVRECORE_NID_DB_DIR` | `third_party/vita-headers/db/360` | The NID database the tools load by default |
| `PSVRECORE_FUZZ_SEED`, `PSVRECORE_FUZZ_BLOCKS` | `0x9E3779B97F4A7C15`, `2000` | Corpus of the generator-versus-interpreter test. A larger or different corpus, for example `scripts\build.ps1 -Define PSVRECORE_FUZZ_BLOCKS=20000`, is a stress run |
| `PSVRECORE_PROBES_DIR` | empty | Set by `scripts/probe_check.py` to build the probe runners from the code it generated. Leave it empty otherwise |
| `PSVRECORE_PROGRAMS_DIR` | empty | The same for `scripts/program_check.py` and its program runners |
| `PSVRECORE_GENERATED_CODE_ARCH`, `PSVRECORE_GENERATED_CODE_CPU` | empty | `-march` / `-mcpu` for generated code only (`psvrecore_generated_code`), e.g. `x86-64-v3` or `armv8.2-a`. Empty: the toolchain's baseline (x86-64, ARMv8.0). A build so made runs only on CPUs that have it. Measured in [CODEGEN_METRICS.md](CODEGEN_METRICS.md) |
| `PSVRECORE_GENERATED_CODE_LTO` | OFF | ThinLTO across generated code (clang, with lld or lld-link): inlining between generated files at link time, for a longer link. Measured in [CODEGEN_METRICS.md](CODEGEN_METRICS.md) |
| `PSVRECORE_GENERATED_CODE_CHECK_KEPT` | OFF | Generated code checks, after every direct call where it keeps a register the callee was found to preserve in a local, that the callee left that value in the context (`PSV_CHECK_KEPT_REGISTERS`), and stops with a diagnostic if not: for a game suspected of breaking what the register analysis assumes ([KNOWN_ISSUES.md](KNOWN_ISSUES.md)); `psvrecomp --no-callee-saved` or `--registers-in-context` then generate without it |
| `PSVRECORE_GENERATED_CODE_NATIVE_FLOAT` | OFF | arm64 only, **not yet verified on a device**: generated code's floating point as the host's instructions (`PSV_NATIVE_FLOAT`: one instruction per VFP operation, no checks), the runtime keeping the host's FPCR mirroring the guest's FPSCR ([ARCHITECTURE.md](ARCHITECTURE.md), "VFP registers"; it is in the build signature). Off, the operations are the host's with an exact fallback, as on x64. [Device tests](#device-tests) check it |
| `PSVRECORE_DEVICE_TESTS` | OFF | The [device tests](#device-tests) (`tests/device`): executables to push to an arm64 device and run there |
| `PSVRECORE_HOST_CORPUSGEN` | empty | With `PSVRECORE_DEVICE_TESTS`: a corpusgen built for the build host, which translates the synthetic corpus for the device's codegen test (left out without it) |
| `PSVRECORE_HOST_FUZZGEN` | empty | With `PSVRECORE_DEVICE_TESTS`: a fuzzgen built for the build host, which writes the random-block corpora for the device's fuzz test (left out without it) |
| `PSVRECORE_FUZZ_SIMD_BLOCKS` | 8000 | Blocks in the generator's Advanced SIMD differential corpus (`fuzzgen --simd`, run by `psvrecore_tests`) |
| `PSVRECORE_GENERATED_CODE_DEBUG` | `lines` | Debug information of generated code in builds that have it: `lines` (function names and line tables: debuggers' and profilers' symbols, simpleperf's and Tracy's `f_<address>`, under a third of the size of full debug information), `full` (types and variables too), `none` |

## The shared runtime library

On Windows and Linux the runtime is one shared library, `psvrecore.dll` / `libpsvrecore.so.<ABI version>`, and each
game is an executable linked to it; on Android it is linked into each game's `libmain.so` (`PSVRECORE_SHARED=OFF`).
The library holds the runtime (kernel, system libraries, loader), gxm, vulkan, platform and frontend, nx-foundation and
its allocator, and the third-party code they use (SDL3, openh264, LibAtrac9, the AAC and MP3 decoders, volk, VMA, zlib), which
stays internal. The tests and the tools link the same code as static libraries, as before.

A game links two targets: `psvrecore::game` for its executable (the frontend, nx-foundation's headers) and
`psvrecore::generated_code` for its recompiled modules (`guest.h`). With `PSVRECORE_SHARED=OFF` they are the static
libraries, so a game's CMake is the same either way. The generated code stays in the game's executable; it reaches
the runtime through the functions `guest.h` declares and `psv_membase`.

```cmake
add_subdirectory(psvrecore)
add_executable(mygame src/main.cpp)
target_link_libraries(mygame PRIVATE psvrecore::game)
add_library(mygame_code STATIC ${generated_sources})
target_link_libraries(mygame_code PUBLIC psvrecore::generated_code)
psvrecore_generated_code(mygame_code)
target_link_libraries(mygame PRIVATE mygame_code)
```

The library is written to the top of the build folder, next to the game's executable (`build/<preset>/`), where
Windows finds the DLL and the Linux build's run path points.

What it exports is marked `PSVRECORE_API` (`psvrecore/abi.h`): the frontend's entry, the services generated code
calls, the function registry (`recomp/support.hpp`), whole-file I/O and the few runtime calls a game's own additions
make. nx-foundation's and mimalloc's functions are exported too: a game's executable uses nx-foundation's strings,
containers and log through psvrecore's interface, and the process has one heap and one log. On Windows a module
definition file lists them (`cmake/PsvrecoreExports.cmake`), with SDL_RunApp (the `main` that SDL_main.h gives a
Windows executable calls it); on Linux psvrecore's own code is compiled with `-fvisibility=hidden` and the archives
linked in are hidden (`--exclude-libs`).

**The rule: a game and the psvrecore it runs on are built with the same compiler version and the same settings**
(the same Clang, the same C++ library and C runtime, both release or both debug), from the same psvrecore sources.
The interface is C++ (nx-foundation's types cross it), and only that makes their layouts agree. Building a game in
the same build tree as psvrecore (its submodule, `add_subdirectory`) gives that by construction; a Linux distribution
packaging `libpsvrecore` builds the games with the same toolchain.

**The ABI version.** `PSVRECORE_ABI_VERSION` (in `abi.h`) changes whenever an exported function's parameters, a type
passed through the interface (`frontend::Game`, `PsvContext`, `PsvModuleCode`, ...) or what generated code expects of
the runtime changes. A game's executable carries the version and the build signature (compiler, C++ library, debug
or release) it was compiled with: `frontend::run`, compiled into the executable, hands them to the library before
anything else, and the library refuses to start a game built for another version, or with another compiler or
settings, saying which and what to do (on standard error, and in a message box unless `--hidden`). On Linux the ABI
version is also the library's `SOVERSION`: a game asks for `libpsvrecore.so.<its version>`, so the libraries of
different versions can be installed side by side.

## Probes (needs the PS Vita SDK)

`scripts/probe_check.py` runs the same-source differential tests. It needs the SDK, so it runs locally, not in CI:

```
python scripts/probe_check.py --sdk C:/path/to/sdk               # every probe, -O2
python scripts/probe_check.py --sdk C:/path/to/sdk --opt=-O0 integer float
```

It builds each `tests/probes/*.c` with the SDK's compiler, recompiles it and the SDK's libc with psvrecomp, builds a
runner per probe (with `-Define PSVRECORE_PROBES_DIR=...`, then resets it), and runs them. Each runner compares the
recompiled probe's output with the same source compiled for the host. Everything it produces goes to `build/probes`
(`--out`), which is ignored by git. It is derived from the SDK, so never commit it.

## Shader disassembler check (needs the PS Vita SDK)

`scripts/usse_check.py` compares psvusse's listings with the SDK's disassembler, program by program:

```
python scripts/usse_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk C:/path/to/sdk shaders/
```

The inputs are GXP files, or folders of them: the SDK's sample shaders, or shaders built with its compiler (`psp2cgc
-profile sce_vp_psp2` or `sce_fp_psp2`). It writes nothing. Shaders built from the SDK are derived from it: keep them
outside the repository.

## GXP reader check (needs the PS Vita SDK, Windows)

`scripts/gxp_check.py` compares `psvusse --params` with what libgxm's program functions report. It calls them in the
SDK's host build, `host_tools/lib.x64/gxm_tool.dll`, so it needs a 64-bit Python on Windows:

```
python scripts/gxp_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk C:/path/to/sdk shaders/
```

## Shader interpreter check (needs the PS Vita SDK)

`scripts/usse_exec_check.py` generates random Cg vertex programs, compiles them with the SDK's compiler, runs them
with `psvusse --run`, and compares the outputs with the same expressions evaluated by numpy:

```
python scripts/usse_exec_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk C:/path/to/sdk
python scripts/usse_exec_check.py ... --count 2000 --seed 11 --special int,index --flow none --keep DIR
python scripts/usse_exec_check.py ... --fragment     # fragment programs (f32 and f16 varyings; half sample textures)
python scripts/usse_exec_check.py ... --fragment --textures 1.0   # every fragment program samples textures
python scripts/usse_exec_check.py ... --fixed        # fixed-point fragment programs
```

`--special` and `--flow` limit what is generated. `--keep` keeps the programs, their inputs and their GXPs in a
folder; they are derived from the SDK, so keep that folder outside the repository. To see a program run, use
`psvusse --run DIR/eNNNNN.in --trace DIR/eNNNNN.gxp`.

## Shader translator check (needs the PS Vita SDK and a Vulkan device)

With `--gpu`, each program the interpreter check runs is also translated to SPIR-V and run on a Vulkan device
(`psvusse --run --gpu`), and its result words are compared with the interpreter's:

```
python scripts/usse_exec_check.py ... --gpu --validate             # the default device, validation layer on
python scripts/usse_exec_check.py ... --fragment --gpu --gpu-index 1
psvusse --spirv out.spv shader.gxp && spirv-val --target-env vulkan1.1 out.spv
```

`--gpu-index` picks the device by its Vulkan index (by default a discrete GPU). A disagreement is reported as
`gpu eNNNNN: word N: interpreter ..., GPU ...`.

## Rendering tests (needs the PS Vita SDK and a Vulkan device)

`scripts/render_check.py` compiles each `tests/render/<test>` folder's Cg programs (`v*.cg` vertex, `f*.cg` fragment)
with the SDK's compiler and draws its `scene.txt` with `psvrender` twice: with the reference renderer (the USSE
reference interpreter, rasterized on the CPU) and with the Vulkan backend. It compares the images pixel by pixel:

```
python scripts/render_check.py --sdk C:/path/to/sdk                   # every test, validation layer on
python scripts/render_check.py --sdk C:/path/to/sdk depth blend --gpu-index 0
```

The programs and the images (`reference.png`, `gpu.png`, `diff.png`: red where they differ) go to `build/render`.
They are derived from the SDK: never commit them. The scene format is described in `tools/psvrender/main.cpp`.

## SDK samples (needs the PS Vita SDK and a Vulkan device)

`scripts/sample_check.py` builds the SDK's api_libgxm samples with the SDK (the custom build steps of their
projects included), recompiles them and runs them on the runtime with the GXM layer, saving the frames they show:

```
python scripts/sample_check.py --sdk C:/path/to/sdk basic shadow_mapping --frames 60
```

Name the samples to run: a GXM sample by its name, any other as `category/project/sample` (for example
`audio_video/api_libngs/pcm_player`). The data files a sample names (`app0:...`) are copied from the SDK. The frames go
to `build/samples/<sample>/frames`, with what each audio port played as `audio_port<N>.wav`, and the runner's whole
output to `build/samples/<sample>/run.log`; compare the frames with the screenshot next to each sample's sources.
`--audio-seconds N` ends a sample once a port has played N seconds. Everything there is derived from the SDK: never
commit it.

`--window` runs the sample as a game, through the frontend (below): in a window with its sound and the host's
controls, until the window is closed or `--timeout` seconds have passed. `--input` scripts controls, as items
`<ms> <what> <args>` separated by `;` or new lines (the time from the boot): `key <SDL key name> down|up`,
`pad <button> down|up` (the console's names: cross, circle, square, triangle, up, down, left, right, l, r, start,
select, l3, r3), `axis <SDL gamepad axis> <value>`, `touch <x> <y> down|move|up` (in the screen's 960x544),
`shot <name>` (the next frame shown is saved as `<name>.png`) and `quit`. Two items look at the screen:
`wait <x> <y> <rrggbb> [<tolerance>] [press <button> <ms>]` holds the script until a frame shown has that colour
there (pressing the button every so many ms meanwhile, if asked; the items after it count from then), and
`watch <x> <y> <rrggbb> <tolerance> press <button> [after <ms>]` presses the button each time that point turns
that colour, or so many ms after, until `unwatch` (a player for a rhythm game's lanes). The shots go to the frames
folder; `--frames` counts them:

```
python scripts/sample_check.py --sdk C:/path/to/sdk input_output_devices/api_ctrl/analog_stick --frames 1 \
    --timeout 7 --window --input "1500 shot centre; 2000 axis leftx 32767; 3200 shot moved"
```

## Running a game (the frontend)

A game's program is psvrecore's frontend with the game's recompiled code. `--help` lists its options. Settings come
from `settings.ini` in the user's data folder (`%APPDATA%\psvrecore\<program>\` on Windows,
`~/.local/share/psvrecore/<program>/` on Linux), written with the defaults and a comment on every key the first time;
the command line goes over them (`--set section.key=value`, or the options that stand for one). The game's folder
is `--game <folder>` or `[game] folder`: either its application folder (`eboot.bin` at the top), or a copy of the
console's folders for the title, `app/<title ID>`, `patch/<title ID>` (an update) and `addcont/<title ID>` (add-ons),
as they are under `ux0:`. From a copy, `app0:` shows the update's files over the game's (its `eboot.bin` is the one
that runs and is checked), and `addcont0:` holds the add-ons, each readable once the game opens it
(`sceAppUtilDrmOpen`). Without an update or add-ons those parts are simply missing. ux0: and save data are in
`data/` beside the settings
unless `[game] data` says otherwise, save data as `ux0:user/00/savedata/<title ID>`. The shader cache is `data/cache`
(the game's programs translated, the pipelines made, the driver's pipeline cache): what a run drew with is made before
the next one starts. Deleting it only costs that again. A game whose `param.sfo` asks for the console's extended memory mode (`ATTRIBUTE2` bit 3)
gets its larger budget for memory blocks: 365 MiB of main memory instead of 256. `[display] internal_resolution` (1 to 4) draws the game at
that many times the console's resolution, the surfaces it draws into and reads back as textures included.
`[display] high_frame_rate` (on by default) lets a game's own additions run what the game paces slower than its
menus at 60 frames a second (WipEout 2048's races, 30 on the console), where the game's simulation follows the time
between frames.
`[network] adhoc` plays a game's ad hoc mode with other copies on the same local network (off: offline); see
[ADHOC.md](ADHOC.md).

Keys while playing: arrows (the d-pad), Z Cross, X Circle, A Square, S Triangle, Q L, W R, Enter Start, Right Shift
Select, F11 full screen. Gamepads work as they are plugged in; the mouse touches the front panel. On a touch screen the
controls are drawn over the picture (`[controls] on_screen`: auto, always or never; `opacity`, `scale`): auto shows
them from the start on Android and once the screen is touched elsewhere, and hides them while a gamepad or the keyboard
is used.

## Packaging a game

`scripts/package.py` makes a game's package for one platform from the game's folder, with one command: it reads the
game's `sce_sys` (its own SFO reader, `scripts/sce_sys.py`: `TITLE` and the translated `TITLE_00`..`TITLE_19`,
`TITLE_ID`, `APP_VER`, the update's `param.sfo` over the game's; `icon0.png`), recompiles the game when
`generated/code` is empty (the game's `scripts/generate_code.py`, with psvrecomp from the host's release preset),
builds the platform's release preset (only the game's target; `--preset`, `--build-dir` choose others) and packages
it. A game repository calls it from its own `scripts/package.py`, which gives `--root` (the repository) and `--name`
(the game's program); the game's folder is the one in the repository's `game/` (or `--game <folder>`):

```
python scripts/package.py windows   # build/package/windows/<name>/: the program, psvrecore.dll, install.ps1
python scripts/package.py linux     # build/package/linux/<name>/: the program, libpsvrecore.so.N, <name>.desktop,
                                    #   icons/hicolor/<n>x<n>/apps/<name>.png, install.sh
python scripts/package.py android   # build/package/android/<name>.apk
```

- What is made from `sce_sys` goes to the game repository's `generated/package/` (git-ignored, never committed):
  `<name>.ico` (16 to 256 px), `<name>.rc`, `hicolor/`, `<name>.desktop`, `icon.png` (256 px), `launch.png` (the
  launch screen, below) and `sce_sys.json` (what was read). `python scripts/package.py assets` makes only these.
- The icon: the game's `icon0.png` in the console's round bubble; or the repository's `game/icon.png` when there is
  one, or `--icon <picture>` (a logo: its artwork made square, not rounded).
- Windows: the game's `CMakeLists.txt` calls `psvrecore_game_package(<target>)` (`cmake/PsvrecorePackage.cmake`),
  which compiles `generated/package/<name>.rc` into the program when it is there: the icon (Explorer's and the
  window's: SDL takes the program's first icon) and the version information (the title, `APP_VER` as the file
  version, the title ID). The folder runs wherever it is copied; `install.ps1` adds a Start menu shortcut
  (`-Uninstall` removes it).
- Linux: `psvrecore_game_package` makes the program find `libpsvrecore.so.N` beside itself (`$ORIGIN`), so the
  folder runs wherever it is copied (CI uploads it as an artefact; there it packages a made-up game around the
  shared-library test program, since no game's code reaches CI). The `.desktop` entry is named after the program,
  as SDL names the window (Wayland's application ID, X11's class), with the translated titles; `install.sh` installs
  it and the icons for the user (`$XDG_DATA_HOME`, `~/.local/share`) pointing at the folder (`--uninstall`).
  AppImage or Flatpak: not yet.
- Android: `scripts/android_apk.py --sce-sys <the game's folder>` names the application `@string/app_name` (the
  title, its translations as `values-<language>/strings.xml`), takes `APP_VER` as the version name and code (01.04:
  104) and `icon0.png` as the launcher icon unless `--icon` gives another. `package.py android` passes it, and its
  other options through (`--install`, `--debuggable`).
- The window's title is the game's own at run time too: the frontend reads `sce_sys/param.sfo` from the game's folder
  (`TITLE_xx` in `[system] language`, else `TITLE`); the title the game's program gives is used without one.
- The launch screen, from the game's LiveArea (`sce_sys/livearea/contents/`, the update's over the game's): its
  `template.xml` (read by our own small parser in `sce_sys.py`, tolerant of what the console's templates hold: a byte
  order mark, either quotes, comments, an element left open) names the background (`<livearea-background><image>`,
  `bg0.png` as a rule) and the gate (`<gate><startup-image>`: `startup.png`, `gate.png`, `pic0.png`...). `launch.png`
  is the console's screen, 960 x 544: the background as large as fits (its bars in the mean colour of its edge
  pixels), the gate over its middle as large as the console shows it (280 x 158 of the background's 840 x 500);
  indexed and transparent PNGs are read as they are (transparent: over black). A game without a `livearea` folder
  gets none, and starts as before (black until its first frame).
  - At run time the frontend shows it from the window's first image until the game's first frame, then fades it out
    (250 ms): as large as fits the window, the bars in the colour of its edges (worked out the same way). It is read
    on the window's presenting thread, so the game's start does not wait for it. Setting `[display] launch_screen`
    (on by default; `--set display.launch_screen=false`). The log says when the first frame came (`display: the
    game's first frame ... ms after the window opened`).
  - Windows and Linux: `launch.png` beside the program; `package.py` puts it in the folder, and
    `psvrecore_game_package` copies it beside the program in the build folder when `generated/package/launch.png` is
    there when CMake configures.
  - Android: see "Android applications" below (`android_apk.py`: the picture in the APK's assets, the system splash
    screen's colour).

`python scripts/package_tests.py [--build <build folder>]` tests these scripts on a made-up game (a `param.sfo`
written by the test, a drawn icon); with a build folder it packages `psvrecore_shared_tests` as a game's program and
runs it from the package's folder. Needs Pillow.

## Android applications

`scripts/android_apk.py` packages a `libmain.so` built by an android-arm64 preset as an APK, with the Android SDK's
own tools (aapt2, javac, d8, zipalign, apksigner) and no Gradle: SDL's Java activity, psvrecore's launcher and
activity (`android/java`), and the caller's manifest. It is signed with the Android debug key (made in
`~/.android/debug.keystore` when missing), for development. It needs ANDROID_HOME (or `C:\Android\Sdk`) with
build-tools and a platform, and a JDK.

```
python scripts/android_apk.py --manifest <AndroidManifest.xml> --lib build/android-arm64-release/libmain.so \
    --out game.apk --install
```

With `--sce-sys <the game's folder>` (as `package.py android` passes it) and a game with a LiveArea, or with
`--launch <a picture, or a game's folder>` (`--no-launch`: none; a game repository's
`scripts/android_apk.py` passes its game's folder), the package gets the launch screen:

- `assets/launch.png` (stored), the picture the native side draws until the game's first frame (the frontend opens
  `launch.png`: SDL looks in the application's files, then in the assets).
- `@color/psvrecore_launch`, the colour of the picture's edges, and a theme over the manifest's application theme
  (`PsvrecoreLaunch`, set on `<application>`): on Android 12 and later the system's splash screen is the launcher
  icon (the adaptive one) on that colour (`windowSplashScreenBackground`); before 12 the windows' background is that
  colour with the launcher icon in the middle (`windowBackground`, what shows while the application starts).
- `PsvrecoreActivity` puts the picture over SDL's surface from its first frame (an `ImageView`, the picture as large as
  fits on that colour, read on a thread of its own so nothing waits for it) and takes it away when the native side
  calls `hideLaunchPicture` after its first image (the same picture, drawn the same way). `LauncherActivity` goes on
  to the game without an animation. So: the splash screen's colour, the picture, then the game's first frame fading
  in, with no black between.

On the device, the launcher offers Play, Settings, and the game's application folder: chosen with the system's folder
picker and copied into the application's folder, `Android/data/<package>/files/game`. The settings file and the data
are there too, and standard output goes to `output.txt` there (the three runs before kept as `output.1.txt` to
`output.3.txt`). The game folder is either the application folder
(eboot.bin at its top) or a copy of the console's ux0: folders (`app/<title ID>`, with `patch/` and `addcont/`
beside it). A debuggable build can also be given its files from a computer, into its internal storage
(`files/game`, through `adb exec-in run-as`: run-as reaches nothing else); Play uses that folder when the shared one
holds no game. While playing, the settings open over the game from
the on-screen menu button (at the top in the middle) or Back; the game is suspended meanwhile, and what applies while
it runs (the on-screen controls, filtering, sound, the log's level) applies when it goes on.

`scripts/sample_check.py --android` runs SDK samples on the device adb sees (`--device <serial>` picks one): it builds
them for arm64, packages and installs them (debuggable), puts their files in the application's internal storage with
`run-as`, starts them with `--input`'s script for `--timeout` seconds, and brings the shots and the output back:

```
python scripts/sample_check.py --sdk C:/path/to/sdk input_output_devices/api_touch/trace --android \
    --device <serial> --frames 1 --timeout 10 --input "3000 touch 480 272 down; 4000 shot touching; 6000 quit"
```

## Device tests

Native floating point on arm64 (`PSVRECORE_GENERATED_CODE_NATIVE_FLOAT`) relies on the host's FPCR behaving as the
guest's FPSCR does, which only an arm64 CPU can show; it is off by default until these tests have passed on a device.
The Advanced SIMD helpers' arm64 forms (AArch64 instructions taken to compute ARMv7's results: the estimates, the
conversions, the native floating-point path) can only be checked there too. `tests/device` builds three executables
with generated code's options (native floating point when the option is on):
`psvrecore_float_device_tests` (guest.h's typed operations against the interpreter: every arithmetic operation,
compare and conversion in both precisions, in each FPSCR mode, 10.6 million cases by default, `PSV_FLOAT_CASES=<n>`
for n random ones per operation and mode; the hand-computed cases of `float_tests.cpp`; and `simd_tests.cpp`:
guest_neon.h's single-precision helpers on every pair of edge values, every estimate's exponent and table index, the
products at the smallest normal, with native floating point also on the native path), `psvrecore_codegen_device_tests`
(the synthetic corpus' generated functions against the interpreter, d0-d31 and the FPSCR compared at every exit, the
NEON corpus `neon_locals.s` included; `CODEGEN_RUNS=<n>` runs per function, 48 by default) and
`psvrecore_fuzz_device_tests` (the random-block corpus and the Advanced SIMD corpus of fuzzgen, every NEON family
with lanes at their edges, against the interpreter). The corpora are translated on the build host, so build corpusgen
and fuzzgen for Windows first, then the device tests in a folder of their own (both with the option on, and once
off, for the default operations on the same device):

```
scripts\build.ps1 -Preset windows-x64-release -NoTest -Target corpusgen,fuzzgen
scripts\build.ps1 -Preset android-arm64-release -BuildDir build/android-native-tests -Target psvrecore_float_device_tests,psvrecore_codegen_device_tests,psvrecore_fuzz_device_tests -Define PSVRECORE_DEVICE_TESTS=ON,PSVRECORE_GENERATED_CODE_NATIVE_FLOAT=ON,PSVRECORE_HOST_CORPUSGEN=C:/path/to/psvrecore/build/windows-x64-release/tests/corpusgen.exe,PSVRECORE_HOST_FUZZGEN=C:/path/to/psvrecore/build/windows-x64-release/tests/fuzzgen.exe
scripts\build.ps1 -Preset android-arm64-release -BuildDir build/android-portable-tests -Target psvrecore_float_device_tests,psvrecore_codegen_device_tests,psvrecore_fuzz_device_tests -Define PSVRECORE_DEVICE_TESTS=ON,PSVRECORE_GENERATED_CODE_NATIVE_FLOAT=OFF,PSVRECORE_HOST_CORPUSGEN=C:/path/to/psvrecore/build/windows-x64-release/tests/corpusgen.exe,PSVRECORE_HOST_FUZZGEN=C:/path/to/psvrecore/build/windows-x64-release/tests/fuzzgen.exe
```

Then, with the device connected (`adb devices` lists it; `-s <serial>` after `adb` picks one of several):

```
adb push build/android-native-tests/tests/device/psvrecore_float_device_tests /data/local/tmp/psv_float_native
adb push build/android-native-tests/tests/device/psvrecore_codegen_device_tests /data/local/tmp/psv_codegen_native
adb push build/android-native-tests/tests/device/psvrecore_fuzz_device_tests /data/local/tmp/psv_fuzz_native
adb push build/android-portable-tests/tests/device/psvrecore_float_device_tests /data/local/tmp/psv_float_portable
adb push build/android-portable-tests/tests/device/psvrecore_codegen_device_tests /data/local/tmp/psv_codegen_portable
adb push build/android-portable-tests/tests/device/psvrecore_fuzz_device_tests /data/local/tmp/psv_fuzz_portable
adb shell chmod 755 /data/local/tmp/psv_float_native /data/local/tmp/psv_codegen_native /data/local/tmp/psv_fuzz_native /data/local/tmp/psv_float_portable /data/local/tmp/psv_codegen_portable /data/local/tmp/psv_fuzz_portable
adb shell /data/local/tmp/psv_float_native
adb shell /data/local/tmp/psv_codegen_native
adb shell /data/local/tmp/psv_fuzz_native
adb shell /data/local/tmp/psv_float_portable
adb shell /data/local/tmp/psv_codegen_portable
adb shell /data/local/tmp/psv_fuzz_portable
adb shell PSV_FLOAT_CASES=1000000 /data/local/tmp/psv_float_native
adb shell CODEGEN_RUNS=1000 /data/local/tmp/psv_codegen_native
```

Each prints its cases and disagreements and ends with `N/N test cases passed` (exit status 0); the native runs print
"native floating point" first. Any disagreement names the operation, the FPSCR mode and the operands (the fuzz test:
the block's disassembly and its first state). Passing native runs on the phones the games run on (both of them: their
CPUs differ) are what turning the option on by default waits for; then build a game with
`-Define PSVRECORE_GENERATED_CODE_NATIVE_FLOAT=ON` and compare its scripted run with the default build's.

## ATRAC9 check (needs the PS Vita SDK)

`scripts/at9_check.py` encodes test signals with the SDK's at9tool (mono, stereo, stereo in superframes), decodes them
with at9tool as the reference, and runs the runtime test that plays each through an NGS ATRAC9 voice (from the start
and from a seek) and compares them sample for sample:

```
python scripts/at9_check.py --sdk C:/path/to/sdk
```

What at9tool makes goes to `build/at9`: never commit it.

## VAG check (needs the PS Vita SDK)

`scripts/vag_check.py` encodes test signals with the SDK's vagconv2, in its default HE-VAG mode and its
backwards-compatible one, and runs the runtime test that plays each through an NGS player voice and measures it
against its source:

```
python scripts/vag_check.py --sdk C:/path/to/sdk
```

What vagconv2 makes goes to `build/vag`: never commit it.

## Test programs (needs the PS Vita SDK)

`scripts/program_check.py` runs whole SDK-built programs on the runtime:

```
python scripts/program_check.py --sdk C:/path/to/sdk                  # every program once
python scripts/program_check.py --sdk C:/path/to/sdk threads --runs 10
```

Each `tests/programs/*.c` is built with the SDK's compiler (with the SDK's start-up code), put into an application
folder with the SDK's `libc.suprx` and `libfios2.suprx` under `sce_module/`, and recompiled with them. A runner boots
it as a game is booted: `app0:` is the application folder, `ux0:` an empty writable folder. What it prints must
match `tests/programs/<name>.expected`, and it must exit with 0. The runner logs to stderr; set `PSVRECORE_LOG=debug`
(or `info`) to see more. Everything goes to `build/programs` (`--out`), which is derived from the SDK: never commit
it.

## Debugging switches

Environment variables the runtime reads, for finding what a game does; they cost nothing while unset. Where the
environment cannot be given (an Android activity, started with `am start ... --esa args ...`), the frontend's
`--env NAME=VALUE` sets one before the runtime starts.

| Variable | What it does |
|---|---|
| `PSVRECORE_FAULT_DUMP=1` | A guest memory fault also prints the 64 bytes at each register's address. |
| `PSVRECORE_WATCH=<address>[:<size>],<function>,...` | Up to four hardware watchpoints (Windows, x86-64): a write to a guest address (1, 2, 4 or 8 bytes, 4 by default) prints the value now there, the host function that wrote, the guest thread's registers, the text registers point at, and the words at its stack pointer; a call of a host function by name (a generated `f_<address>`, called through a pointer: one called directly may be inlined) prints the time and the guest registers it gets. `f_<address>@r<n>+<offset>+...` (hex) also prints the words at those offsets from the register, as numbers and floats; `@r<n>[<offset>]...` reads through the pointer at that offset first (`[a][b]` through two), and more `@` groups read more (eight words in all): `f_8101EA62@r0[8]+1dc` prints a field of the structure the first argument's third word points to at every call. |
| `PSVRECORE_WATCH_SHOTS=1` | Each press of an input script's `watch` also saves a shot (`w<ms>_<button>.png`), to see what the watch saw. |
| `PSVRECORE_THREADS=<seconds>` | Every so many seconds, each guest thread's state, priority, link register and what it waits for. Without it, the same goes to the log as warnings whenever the picture stands still for 3 s with the application in front (again at 10 s), with the renderer's scene counts and the host threads using the CPU. |
| `PSVRECORE_STALL_SNAPSHOT=<file>` | The first time the picture stands still (the warnings above), every mapped page of guest memory written to the file, for reading the game's state offline: records of a guest address and a byte count (u32 each, little-endian) followed by the bytes, one record for each run of mapped pages (about 340 MiB for WipEout 2048). On Android a file in the application's folder (`/sdcard/Android/data/<package>/files/...`), pulled with adb. |
| `PSVRECORE_HLE_ERRORS=1` | Every HLE call that answers with an error code (0x80000000-0x80FFFFFF), with its first four arguments and the guest address it returns to: where a game gives up on something the system refused. |
| `PSVRECORE_HLE_COUNTS=<seconds>` | Every so many seconds, the HLE functions called since the last report, how often and the time spent in them (waits included: what blocks shows where the game waits), and the CPU time each host thread used meanwhile (Windows). |
| `PSVRECORE_FRAME_STATS=<seconds>` | Every so many seconds, the frames the game displayed, their rate, the frame times' median, 99th percentile and worst, and the hitches (frames half again as long as the median); each hitch logged as it comes. With a frame over 50 ms after the one before, why: how long after the frame before the game queued it, how long the display queue took to take it, and how long its scenes then took to finish (waiting for the renderer's lock, for the GPU, writing back). |
| `PSVRECORE_FRAME_TIMES=<file>` | Every frame time (the time between two frames the game displayed, in microseconds), a line each, written to the file: for comparing runs. |
| `PSVRECORE_AUDIO_RECORD=<folder>` | What each audio port plays, as the game queues it, to `<folder>/audio_port<N>.wav` (16-bit PCM at the port's rate and channels), and to the speaker as well: for hearing or analysing what the game made, apart from the device. The sizes in the headers are set when the program ends (one stopped from outside leaves them 0; the samples are there). On Android a folder the application can write, such as its own (`/data/user/0/<package>/files/...`, read back with `run-as`). |
| `PSVRECORE_GUEST_THREADS=native` | In a build with `PSVRECORE_GUEST_FIBERS=ON`: guest threads as OS threads after all, for comparing in one build. |
| `PSVRECORE_DETERMINISTIC=1` | In a build with `PSVRECORE_GUEST_FIBERS=ON`: the same scheduling order run after run. One host worker runs the console's CPUs in turn, switching at each kernel call, and the kernel's clock is its own (a microsecond each kernel call; when nothing can run, the next deadline). For tests: host threads (the display's, audio) do not follow it. |
| `PSVRECORE_THREAD_STATS=1` | The guest threads started, how many a second, and how long each took from the start call to running on its host thread (mean, median, 99th percentile, worst), with the thread backend and the host threads it made: in the log when the game ends. |
| `PSVRECORE_HITCH_TRACE=1` | With the above: at each hitch, what every guest thread did since the frame before (its long calls, the long stretches of its own code), and the console's CPUs it held and waited for. |
| `PSVRECORE_SHOTS_FROM_WINDOW=1` | An input script's shots are what the window presented (the overlays over the picture, at the window's size), not the frame in guest memory. |
| `PSVRECORE_GXM_KEEP_CPU=1` | A scene's end keeps the game thread's emulated CPU while the host renders the scene (by default the CPU is lent to the game's other threads meanwhile and taken back after). For comparing. |
| `PSVRECORE_GXM_SCENE_THREAD=1` | Scenes rendered on the renderer's own thread (one scene behind the game at most), not on the game's thread when they end. Slower so far: a song on a Pixel 8a ran at 27.3 frames a second, 28.1 without. |
| `PSVRECORE_GXM_PROFILE=1` | Every 200 scenes, the time each part of rendering a scene took (loading the colour surface, the draws, the textures, recording, the GPU, writing back), the GPU's own time on the scene's commands (timestamps), the texture cache's size, and the textures' lookups, hashes, copies from surfaces and new ones; every 50th new texture described; and at once each scene that took over 16 ms (its parts, new textures, shaders and pipelines) and each new texture that took 2 ms or more (its format and size), each shader and each secondary program's compute pipeline that took 5 ms or more; at the start, each graphics pipeline the shader cache made again, with the time it took and its shaders' sizes. |
| `PSVRECORE_PIPELINE_THREADS=<n>` | Threads that compile pipelines at once: the start's warm-up from the shader cache, and a scene's new pipelines before it is drawn (1: one after another). The same threads translate a scene's new shaders and decode its new textures. By default half the logical cores, 1 to 4: a phone's driver takes up to a tenth of a second for a pipeline whose vertex shader is new. |
| `PSVRECORE_HALF=integer` | Translated programs round their half-precision results in integer arithmetic, without checking the device's own conversion first. By default the renderer, psvrender and psvusse `--gpu` run the device's conversion over about a billion float values and use it where it gives the interpreter's bits (with NaNs fixed up where only those differ): far fewer instructions for a phone's driver to compile. The game keeps the answer with its shader cache (`half.bin`) for that device and driver. |
| `PSVRECORE_YUV_CPU=1` | Movies' YUV420 frames converted to RGBA on the CPU (as they were) instead of by a compute shader in the scene's commands. |
| `PSVRECORE_TARGET_TEXTURES=0` / `verify` / `buffers` | `0`: render targets read as textures (an HDR colour surface as U2F10F10F10, a depth surface as F32, F32M or U16) read back from guest memory and decoded on the CPU, as before, instead of made on the GPU from the surface's image (`gxm/target_texture.hpp`). `verify`: each such texture also decoded on the CPU when it is made (or, from a pending scene's surface, once that scene is written back) and compared with the GPU's, differences logged. `buffers`: an HDR surface's texture made through buffers, as before (its image copied into one, converted, copied into the texture), not from the image directly (`target_image_shader`). |
| `PSVRECORE_WRITE_WATCH=0` / `verify` | Whether guest memory still holds what a scene wrote back of a colour or depth surface is known by watching its pages for writes (`GuestMemory::watch_writes`) after the write-back. `0`: compared with a copy, as before; `verify`: compared too, each change the watch did not see logged. |
| `PSVRECORE_LAZY_WRITEBACK=0` | A scene's colour surface, and its depth surface's store (without a stencil), are not written into guest memory when the scene is done: their pages are deferred (`GuestMemory::defer`: no access) and the bytes read back kept, written when something first touches those pages (the game's code, the renderer, the display, a file or socket the OS writes from there); a surface drawn again first is never written. `0`: written when the scene is done, as before (also off with `PSVRECORE_WRITE_WATCH=0`). |
| `PSVRECORE_GXM_SCENES_IN_FLIGHT=<1-64>` | How many scenes the GPU may have in flight before the renderer waits for the oldest (8). |
| `PSVRECORE_DUMP=<address>:<bytes>,...` | Guest memory, in words, with each report (every 10 seconds without the two above). |
| `PSVRECORE_GXM_TRACE=<first>[-<last>]` (or `1`: all) | Each scene in that range logged before it renders: its surfaces, and each draw's programs, primitive and textures; each vertex program, the first time, with its attributes (names, formats, streams) and uniforms. |
| `PSVRECORE_GXM_TRACE_PROGRAMS=<folder>` | With the trace: each traced vertex program's GXP, and its fragment program's, written there (for `psvusse` and `scripts/usse_check.py`). |
| `PSVRECORE_GXM_SKIP=<address>[,<address>...]` | The draws whose vertex or fragment program is at one of these addresses (as the trace gives them) left out: which draw makes what. |
| `PSVRECORE_GXM_CAPTURE=<scene>:<draw>:<folder>` | That draw (by index, or the first whose vertex program is at that address, in that scene or a later one) written to the folder as a `psvrender` scene (its programs, vertices, indices, uniforms and uniform buffers), to compare the translation's drawing with the reference renderer's. |
| `PSVRECORE_RENDER_SCALE=<1-4>` | The SDK sample runner's internal resolution. |
| `PSVRECORE_GPU_LABELS=1` | The renderer's and the game's debug labels on the GPU commands (`VK_EXT_debug_utils`) for a GPU tool that does not turn them on itself (Nsight, Radeon GPU Profiler); on anyway with the validation layer, under RenderDoc and in a Tracy build ([PROFILING.md](PROFILING.md)). |
