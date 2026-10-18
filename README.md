# psvrecore

A toolkit for static recompilation of PS Vita software into native programs for Windows, Linux (x86-64) and
Android (ARM64). The toolkit has two parts:

- **Tools.** They read a game's executable modules (SELF/ELF), recover their ARMv7/Thumb-2 code and translate it
  ahead of time into C. They also translate the game's GPU shader programs into SPIR-V.
- **Runtime.** It is what the recompiled code runs on: the system libraries the game imports (kernel, files, save
  data, audio, input, GXM graphics on Vulkan) and a platform layer.

It is not an emulator. Code and shaders are translated ahead of time. The runtime implements the system APIs the game
calls; it does not model the console's hardware.

Each game lives in its own repository that uses psvrecore as a git submodule and adds what is specific to that game.
For example, [p4danrecomp](https://github.com/AdamFull/p4danrecomp) does this for *Persona 4: Dancing All Night*.

**This repository contains no game code or assets, no keys, and no PS Vita SDK code or documentation.**
Games are processed from the user's own files on the user's machine. Nothing derived from them is committed.

## Layout

| Folder | Contents |
| --- | --- |
| `libs/core` | Error handling and log categories shared by every library |
| `libs/nx-foundation` | [nx-foundation](https://github.com/AdamFull/nx-foundation) (submodule): containers, strings, threads, files, logging, tests |
| `tests` | Unit tests (no game data or SDK files needed) |
| `cmake`, `scripts` | Build helpers |
| `docs` | [Architecture](docs/ARCHITECTURE.md) · [Roadmap](docs/ROADMAP.md) · [Status](docs/STATUS.md) · [Known issues](docs/KNOWN_ISSUES.md) · [Building](docs/BUILDING.md) |

## Building

See [docs/BUILDING.md](docs/BUILDING.md). In short:

```
git submodule update --init --recursive
scripts\build.ps1                      # Windows: configure, build, test (windows-x64-debug)
cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug && ctest --preset linux-x64-debug
```

## License

MIT; see [LICENSE](LICENSE). Third-party components keep their own licenses.
