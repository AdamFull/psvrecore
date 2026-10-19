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
| `libs/formats` | SELF and ELF, module information, imports/exports, relocations, SFO, CPK archives, NID databases |
| `libs/arm` | ARMv7-A decoder (ARM, Thumb-2, VFPv3, Advanced SIMD) and disassembler |
| `libs/analysis` | Code discovery: functions, interworking, IT blocks, jump tables, imported variables |
| `libs/recomp` | C code generator, the guest ABI header generated code includes, and its support library |
| `libs/interp` | Reference interpreter, for tests only |
| `libs/runtime` | What recompiled programs run on: guest memory, the kernel (threads, scheduling, synchronisation), files, the module loader, and the system libraries implemented on the host |
| `libs/nx-foundation` | [nx-foundation](https://github.com/AdamFull/nx-foundation) (submodule): containers, strings, threads, files, logging, tests |
| `third_party/vita-headers` | [vita-headers](https://github.com/vitasdk/vita-headers) (submodule, MIT): its NID database names system library functions |
| `tools/psvinfo` | Inspects SELF/ELF modules, SFO files and CPK archives |
| `tools/psvdis` | Disassembles raw ARM or Thumb code |
| `tools/psvrecomp` | Finds a module's code and translates it to C |
| `scripts/arm_check.py` | Checks psvdis against llvm-objdump over generated instruction corpora or a code file |
| `scripts/probe_check.py` | Same-source differential tests: SDK-built C probes, recompiled, against the host build (needs the SDK) |
| `scripts/program_check.py` | Whole SDK-built test programs, recompiled with the SDK's libc and libfios2 and booted on the runtime (needs the SDK) |
| `tests` | Unit tests, the generator-versus-interpreter test and the runtime's tests (no game data or SDK files needed); `tests/probes` and `tests/programs` (sources for the SDK-built checks) |
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

`psvinfo` inspects a module (SELF or ELF), an SFO file or a CPK archive:

```
psvinfo info eboot.bin                  # container, segments, module info, imports/exports, relocations
psvinfo imports eboot.bin               # every imported function and variable, with names (tab-separated)
psvinfo exports module.suprx --db my-nids.tsv
psvinfo relocs module.suprx             # every relocation
psvinfo elf module.suprx module.elf     # the ELF inside a SELF
psvinfo stubs <sdk>/target/lib nids.tsv # NID database from SDK stub libraries (keep it local)
psvinfo cpk data.cpk                    # a CPK's header, totals and files by extension and folder
psvinfo cpk --list data.cpk             # every file: path, ID, offset, stored and extracted size
```

Names come from vita-headers' database (`third_party/vita-headers/db/360`) unless `--no-default-db` is given;
`--db` adds more databases (`.yml`, `.tsv`, or folders of them). The `info` command also checks every relocation
against the module's linked contents.

`psvdis` disassembles raw code in the syntax llvm-objdump prints:

```
psvdis --mode thumb --base 0x81000000 code.bin
python scripts/arm_check.py --psvdis build/windows-x64-release/tools/psvdis/psvdis.exe   # needs LLVM
```

`psvrecomp` finds a module's code and translates it to C:

```
psvrecomp analyze module.suprx --diagnostics           # functions, jump tables, what discovery could not resolve
psvrecomp generate module.suprx out/ --name mymodule    # C files: functions, module table, imports
psvrecomp generate eboot.bin out/ --name game --base 0x81000000,0x81400000
```

The generated C includes `libs/recomp/include/psvrecore/recomp/guest.h` and must be compiled with the options in
`psvrecore_generated_code()` (`cmake/PsvrecoreCompilerOptions.cmake`). Generated code is derived from the module, so
keep it local.

## License

MIT; see [LICENSE](LICENSE). Third-party components keep their own licenses.
