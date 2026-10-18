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
| `libs/core` | Error handling, log categories, binary readers, file and text helpers shared by every library |
| `libs/formats` | SELF and ELF, module information, imports/exports, relocations, SFO, NID databases |
| `libs/nx-foundation` | [nx-foundation](https://github.com/AdamFull/nx-foundation) (submodule): containers, strings, threads, files, logging, tests |
| `third_party/vita-headers` | [vita-headers](https://github.com/vitasdk/vita-headers) (submodule, MIT): its NID database names system library functions |
| `tools/psvinfo` | Inspects SELF/ELF modules and SFO files |
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

## Tools

`psvinfo` inspects a module (SELF or ELF) or an SFO file:

```
psvinfo info eboot.bin                  # container, segments, module info, imports/exports, relocations
psvinfo imports eboot.bin               # every imported function and variable, with names (tab-separated)
psvinfo exports module.suprx --db my-nids.tsv
psvinfo relocs module.suprx             # every relocation
psvinfo elf module.suprx module.elf     # the ELF inside a SELF
psvinfo stubs <sdk>/target/lib nids.tsv # NID database from SDK stub libraries (keep it local)
```

Names come from vita-headers' database (`third_party/vita-headers/db/360`) unless `--no-default-db` is given;
`--db` adds more databases (`.yml`, `.tsv`, or folders of them). The `info` command also checks every relocation
against the module's linked contents.

## License

MIT; see [LICENSE](LICENSE). Third-party components keep their own licenses.
