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
