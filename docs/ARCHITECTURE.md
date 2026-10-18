# psvrecore architecture

This document describes the planned design. [STATUS.md](STATUS.md) tracks what exists today; most components below
are not written yet.

## Principles

1. **Static first, not emulation.** Everything that can be known ahead of time is translated ahead of time: code in
   every module the game ships, relocations, import bindings and GPU shader programs. A shipped game contains no
   instruction decoder, no interpreter and no runtime code generator.
2. **The runtime implements system APIs, not hardware.** It reproduces what each API promises the game: its results,
   blocking and timing behavior, memory the game can see, and scheduling guarantees the game can observe. Hardware
   internals are modeled only where the game can observe their effects.
3. **Unknown behavior is never silently accepted.** Each of the following produces a diagnostic with full context
   (address, module, caller, guest registers):
   - an unknown import;
   - an instruction that cannot be translated;
   - an indirect branch to an address that was not discovered;
   - a shader that was not translated.

   By default the program stops there. A game may allow a specific stub, but only by listing it in its configuration
   with a reason. Findings are recorded so the next generation run includes them.
4. **Generic core, game specifics outside.** Nothing about a particular game lives in psvrecore. Games add their
   configuration, function overrides and patches in their own repositories.
5. **Measured performance.** An optimization is kept only if it has correctness tests and reproducible measurements.

## Pipeline

```
user's game files ─► extraction / decryption (keys supplied by the user) ─► executable modules + data
modules ─► analysis (module info, imports/exports, relocations, functions, control flow) ─► C sources
shader programs ─► translation ─► SPIR-V
C sources + runtime + game code ─► Clang ─► native executable (Windows, Linux) or libmain.so (Android)
```

Everything generated stays on the user's machine.

## Components

| Library | Role |
| --- | --- |
| `core` | Errors (`Result`, `Exception`) and log categories |
| `formats` | Packages, file-system images, SELF/ELF, module info, imports/exports, relocations, archives, texture and audio containers |
| `crypto` | Decryption using keys loaded at run time from the user's key directory; no key is compiled in |
| `arm` | ARMv7-A, Thumb-2, VFPv3 and NEON decoding and disassembly |
| `analysis` | Function and basic-block recovery, ARM/Thumb interworking, IT blocks, jump tables, call graph |
| `recomp` | C code generator, plus the guest ABI header the generated code includes |
| `interp` | Reference interpreter, used only by differential tests and never linked into a game |
| `runtime` | Guest memory, module loader, kernel objects and threads, HLE system libraries, files and save data, timing |
| `gxm` | GXM API, texture formats and layouts, shader-program parser and SPIR-V translator |
| `vk` | Vulkan backend (volk, VMA); knows nothing about GXM |
| `platform` | SDL3 window, input and events; audio output; Android integration; ImGui overlay; on-screen controls |
| `frontend` | Program entry point, command line, settings |

## Key design points

- **Guest memory.**
  - The host reserves the whole 32-bit guest address space plus a guard region.
  - A guest address converts to a host pointer as `base + (u32)address`.
  - Only mapped regions are committed. Touching anything else faults, and a fault handler reports the guest address
    and the guest function that touched it.
- **Generated code.**
  - Each guest function becomes a C function.
  - Inside a function, guest registers live in locals so the compiler can allocate them. They are written back at
    calls and returns.
  - Condition flags are separate locals.
  - Indirect branches go through a dense table indexed by guest address. Jump tables become `switch` statements.
  - Floating point is compiled without contraction (`-ffp-contract=off`). The guest's denormal and NaN modes are
    handled explicitly.
- **Threads.**
  - Guest threads run in parallel on host threads.
  - The scheduler keeps the guarantees a game can rely on: how many guest threads run at once, priority order, and
    no time-slicing among threads of equal priority.
  - Exclusive load/store sequences become host compare-and-swap operations, and barriers become host fences.
  - Fibers and other guest stack switches use nx-foundation's fibers.
- **Modules.** Libraries a game ships in its own package are recompiled like its executable. The runtime implements
  the console firmware's system libraries, looked up by NID.
- **Graphics.**
  - GXM scenes map to Vulkan render passes, with load/store operations chosen for tile-based GPUs.
  - Guest surfaces are written back to guest memory only when the game can observe them.
  - Shader programs are translated to SPIR-V ahead of time and specialized for the formats and blending each draw
    uses.
  - Reads of the destination color use framebuffer fetch where the device supports it, with a correct fallback where
    it does not.
  - Higher resolution, anti-aliasing, aspect-ratio changes and higher frame rates are optional and never change game
    logic.
- **Time.** System time, audio playback position, input timestamps and the display's refresh signal all come from one
  host clock with one epoch. Audio APIs block and report positions as the original APIs do. Gameplay that follows the
  music therefore stays in sync at any render frame rate.
