# psvrecore architecture

This document describes the design. [STATUS.md](STATUS.md) tracks what exists today; components from the runtime on
(M4 and later) are not written yet.

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

- **Instruction model.** The decoder turns ARM and Thumb encodings into one description: an operation, a condition,
  flags and up to six typed operands. The same operation decodes to the same description whichever instruction set
  and encoding it comes from, so the code generator implements each operation once.
  - Encodings the architecture leaves undefined decode to `Op::Undefined`.
  - UNPREDICTABLE encodings decode with a flag, so analysis can refuse them.
  - The target is the Cortex-A9's ARMv7-A: VFPv3 with half-precision conversions, and Advanced SIMD. Integer divide
    and VFPv4 fused multiply-add are not implemented by that core, so they are undefined.
- **Disassembly** prints llvm-objdump's syntax. That makes the decoder checkable against an independent
  implementation over whole encoding spaces (`scripts/arm_check.py`).

- **Guest memory.**
  - The host reserves the whole 32-bit guest address space plus a guard region.
  - A guest address converts to a host pointer as `base + (u32)address`.
  - Only mapped regions are committed. Touching anything else faults, and a fault handler reports the guest address
    and the guest function that touched it.
- **Analysis.** Function starts come from the module's exports, the exception index table and call targets.
  - Discovery repeats until the set of functions and the set of functions that never return stop changing. The
    second set starts from imports known by name never to return (`abort`, `exit`, the C++ throw helpers and
    others).
  - Jump tables are read from their bounds checks. When a table has no bounds check, it is taken to end at the
    first code after it.
  - Each imported variable's reference table lists the places that need its address. Those places are resolved at
    load time, never at generation time.
- **Generated code** (`recomp/guest.h` is the contract between the generator, the runtime and the tests).
  - Each guest function becomes `void f_<address>(PsvContext*, uint32_t ret)`. `ret` is the guest return address
    the caller expects.
  - A call sets LR and calls the C function. A return is an indirect branch whose target equals `ret`. A tail call
    passes its own `ret` on.
  - Other indirect branches look the target up among the registered modules' functions. An unknown target stops
    with a diagnostic. Jump tables become `switch` statements.
  - Registers and flags live in the context, and the C compiler keeps them in host registers where it can.
  - IT blocks become per-instruction conditions.
  - Loads from read-only data are folded into constants, except words the loader patches.
  - Imports go through a per-module table the loader fills, and so do imported variables' addresses.
  - Floating point is compiled without contraction (`-ffp-contract=off`, no fast math). Flush-to-zero, default NaN
    and the ARM NaN rules are implemented explicitly, binary32 by way of binary64 with exact underflow detection.
  - Advanced SIMD always uses its standard FP mode.
  - Exclusive loads and stores become a host compare-and-swap.
- **Reference interpreter** (`interp`, tests only). It is written from the ARM ARM pseudocode, independently of the
  generator, and shares only the decoder and the context layout. It has its own integer soft float. Two kinds of
  test compare generated code against it:
  - random blocks run on both from the same state (in CI);
  - SDK-built C programs, recompiled and compared with the same source built for the host (`scripts/probe_check.py`,
    local).
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
