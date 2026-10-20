# Building psvrecore

## Requirements

| | Windows | Linux | Android |
| --- | --- | --- | --- |
| Compiler | LLVM/Clang (clang-cl, MSVC ABI) with Visual Studio 2022's C++ tools and Windows SDK | Clang | Clang from the NDK (r27 or newer) |
| Build | CMake ≥ 3.25, Ninja (Visual Studio's bundled ones work) | CMake ≥ 3.25, Ninja | CMake ≥ 3.25, Ninja |

Submodules are required: `git submodule update --init --recursive`. That checks out nx-foundation and its own
third-party code, vita-headers (whose NID database the tools read at run time), and the Vulkan side:
Vulkan-Headers, SPIRV-Headers, volk and VulkanMemoryAllocator. Nothing links against a Vulkan SDK; the loader is
opened at run time. The Vulkan SDK's validation layer and `spirv-val` are used by the local checks only. The
media decoders come the same way: LibAtrac9 (MIT) and openh264 (BSD-2-Clause) as submodules; the PacketVideo AAC
decoder (Apache-2.0) is vendored in `third_party/pvaacdec` (its README says where from). The platform layer's
SDL3 (zlib) is a submodule as well, built as a static library.

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

`psvrecore_gpu_tests` needs a Vulkan device and is skipped without one (CI machines). It turns the validation
layer on when the Vulkan SDK's is installed. `PSVRECORE_TEST_GPU=<index>` picks the device; by default it is a
discrete GPU.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `PSVRECORE_BUILD_TOOLS` | ON (OFF on Android) | Host tools |
| `PSVRECORE_BUILD_TESTS` | ON (OFF on Android) | Unit tests |
| `PSVRECORE_PROFILER` | OFF | Builds Tracy into nx-foundation and compiles profiler zones |
| `PSVRECORE_NID_DB_DIR` | `third_party/vita-headers/db/360` | The NID database the tools load by default |
| `PSVRECORE_FUZZ_SEED`, `PSVRECORE_FUZZ_BLOCKS` | `0x9E3779B97F4A7C15`, `2000` | Corpus of the generator-versus-interpreter test. A larger or different corpus, for example `scripts\build.ps1 -Define PSVRECORE_FUZZ_BLOCKS=20000`, is a stress run |
| `PSVRECORE_PROBES_DIR` | empty | Set by `scripts/probe_check.py` to build the probe runners from the code it generated. Leave it empty otherwise |
| `PSVRECORE_PROGRAMS_DIR` | empty | The same for `scripts/program_check.py` and its program runners |

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
the next one starts. Deleting it only costs that again. `[display] internal_resolution` (1 to 4) draws the game at
that many times the console's resolution, the surfaces it draws into and reads back as textures included.

Keys while playing: arrows (the d-pad), Z Cross, X Circle, A Square, S Triangle, Q L, W R, Enter Start, Right Shift
Select, F11 full screen. Gamepads work as they are plugged in; the mouse touches the front panel. On a touch screen the
controls are drawn over the picture (`[controls] on_screen`: auto, always or never; `opacity`, `scale`): auto shows
them from the start on Android and once the screen is touched elsewhere, and hides them while a gamepad or the keyboard
is used.

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

On the device, the launcher offers Play, Settings, and the game's application folder: chosen with the system's folder
picker and copied into the application's folder, `Android/data/<package>/files/game`. The settings file and the data
are there too, and standard output goes to `output.txt` there. The game folder is either the application folder
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
| `PSVRECORE_THREADS=<seconds>` | Every so many seconds, each guest thread's state, priority, link register and what it waits for. |
| `PSVRECORE_HLE_COUNTS=<seconds>` | Every so many seconds, the HLE functions called since the last report, how often and the time spent in them (waits included: what blocks shows where the game waits), and the CPU time each host thread used meanwhile (Windows). |
| `PSVRECORE_GXM_PROFILE=1` | Every 200 scenes, the time each part of rendering a scene took (loading the colour surface, the draws, the textures, recording, the GPU, writing back), the GPU's own time on the scene's commands (timestamps), the texture cache's size, and the textures' lookups, hashes, copies from surfaces and new ones; every 50th new texture described. |
| `PSVRECORE_DUMP=<address>:<bytes>,...` | Guest memory, in words, with each report (every 10 seconds without the two above). |
| `PSVRECORE_GXM_TRACE=<first>[-<last>]` (or `1`: all) | Each scene in that range logged before it renders: its surfaces, and each draw's programs, primitive and textures; each vertex program, the first time, with its attributes (names, formats, streams) and uniforms. |
| `PSVRECORE_GXM_TRACE_PROGRAMS=<folder>` | With the trace: each traced vertex program's GXP, and its fragment program's, written there (for `psvusse` and `scripts/usse_check.py`). |
| `PSVRECORE_GXM_SKIP=<address>[,<address>...]` | The draws whose vertex or fragment program is at one of these addresses (as the trace gives them) left out: which draw makes what. |
| `PSVRECORE_GXM_CAPTURE=<scene>:<draw>:<folder>` | That draw (by index, or the first whose vertex program is at that address, in that scene or a later one) written to the folder as a `psvrender` scene (its programs, vertices, indices, uniforms and uniform buffers), to compare the translation's drawing with the reference renderer's. |
| `PSVRECORE_RENDER_SCALE=<1-4>` | The SDK sample runner's internal resolution. |
