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
