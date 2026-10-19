# Building psvrecore

## Requirements

| | Windows | Linux | Android |
| --- | --- | --- | --- |
| Compiler | LLVM/Clang (clang-cl, MSVC ABI) with Visual Studio 2022's C++ tools and Windows SDK | Clang | Clang from the NDK (r27 or newer) |
| Build | CMake ≥ 3.25, Ninja (Visual Studio's bundled ones work) | CMake ≥ 3.25, Ninja | CMake ≥ 3.25, Ninja |

Submodules are required: `git submodule update --init --recursive`. That checks out nx-foundation and its own
third-party code, and vita-headers (whose NID database the tools read at run time).

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
