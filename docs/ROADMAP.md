# psvrecore roadmap

These are the toolkit milestones. Most of them can be developed and verified without any game data. Game-specific
milestones are in each game's repository.

| # | Milestone | Done when |
| --- | --- | --- |
| M0 | Repository, nx-foundation, presets, tests, CI, docs | Windows and Linux build and pass tests; Android preset configures |
| M1 | `formats`: SFO, SELF→ELF, module info, imports/exports, relocations; NID database; module inspector tool | Unit tests pass on synthetic ELFs; reports match the module's import stubs |
| M2 | ARMv7-A / Thumb-2 / VFPv3 / NEON decoder and disassembler | Output matches `llvm-objdump` on an encoding corpus |
| M3 | Analysis, code generator, test-only interpreter | Same-source differential tests agree (see below) |
| M4 | Runtime core: memory, loader, threads, synchronisation, files | Multithreaded test programs give the expected results natively on Windows and Linux |
| M5 | GXM, shader translation, Vulkan backend | Rendering tests, one per feature, match reference images |
| M6 | Audio, video, input, save data | Sample-accurate audio position tests and input timestamp tests pass |

A same-source differential test compiles one source program twice:
- built for the guest, then recompiled by psvrecore;
- built natively for the host.

Both builds must give the same results.

The first game, [p4danrecomp](https://github.com/AdamFull/p4danrecomp), drives the later milestones: game boot, full
gameplay, Android, enhancements and validation.
