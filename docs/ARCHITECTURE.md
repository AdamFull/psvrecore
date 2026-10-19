# psvrecore architecture

This document describes the design. [STATUS.md](STATUS.md) tracks what exists today; the graphics, audio and platform
components (M5 and later) are not written yet.

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
| `usse` | Shader programs: the GXP container (flags, parameters, vertex outputs, fragment inputs) and the USSE instruction set (decoding into an instruction model, listings in the reference disassembler's syntax) |
| `gxm` | GXM API, texture formats and layouts, SPIR-V translation of shader programs |
| `vulkan` | Vulkan backend (volk, VMA): device, memory, buffers, dispatches; knows nothing about GXM |
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

- **Guest memory** (`runtime/memory.hpp`).
  - The host reserves the whole 32-bit guest address space plus a guard region.
  - A guest address converts to a host pointer as `base + (u32)address`.
  - Only mapped blocks are committed, each with its protection: module segments where their code was generated for,
    memory blocks, thread stacks and TLS areas, the runtime's own data.
  - Touching anything else faults. A fault handler reports the guest address and the guest thread that touched it.
  - Generated code accesses guest memory with relaxed atomic loads and stores. Plain C accesses would let the C
    compiler keep a value across a guest spin loop. On x86-64 and arm64 they compile to ordinary moves.
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
- **Runtime tests.**
  - Unit tests call the system library functions as recompiled code does, from stand-in guest functions (in CI).
  - Whole SDK-built programs (start-up code, libc, threads, files, memory) are recompiled and booted from an
    application folder, and their output is compared with what is expected (`scripts/program_check.py`, local).
- **Threads** (`runtime/kernel.hpp`).
  - Each guest thread runs on a host thread of its own, but only while it holds one of the three CPUs a game has.
  - The kernel hands CPUs out as the console's scheduler does:
    - fixed priorities, FIFO among equal ones;
    - no time slicing;
    - affinity masks;
    - a thread that becomes ready takes a free CPU, or the CPU of the lowest-priority thread running below it.
  - A thread that loses its CPU stops at its next kernel call. A host thread cannot be stopped between two guest
    instructions.
  - One lock guards the kernel's state. Each thread waits on a parker of its own, which gives microsecond deadlines
    where a Windows futex timeout rounds to the timer tick.
  - Guest threads start on pooled host threads, so a start takes microseconds on every host.
  - Wait queues are FIFO or priority-ordered. Timeouts write back the time left, and objects deleted under a waiter
    wake it with an error.
  - `sceKernelExitThread` from any depth ends the thread by unwinding its host stack (a C++ exception through the
    generated C frames, which have unwind tables).
  - Host work that blocks, such as file I/O, gives the CPU to other threads meanwhile.
  - Exclusive load/store sequences become host compare-and-swap operations, and barriers become host fences.
- **System libraries** (`runtime/hle.hpp`, `src/hle/`). The firmware's libraries are host functions registered by
  library and NID. An adapter reads each one's arguments from the guest calling convention: r0-r3, then the stack,
  with 64-bit values in aligned pairs and variadic arguments read in order. The result goes back in r0 (and r1).
  Every call ends at a scheduling checkpoint.
- **Modules** (`runtime/loader.hpp`).
  - Libraries a game ships in its own package (`sce_module/`) are recompiled like its executable, and all their
    generated code is linked into the program.
  - Loading a file finds its code by fingerprint. A module that differs from the one generated from is refused.
  - The loader maps the segments where the code expects them and links imports: to other modules' exports first,
    then to the system libraries.
  - It patches the data that holds imported variables' addresses.
  - Start-up mirrors the console's: the main module, then the modules the system loads for it, each started with
    `module_start` on a thread of its own. Then the main thread runs the main module's start entry with the
    parameters from its process parameter block.
- **Files** (`runtime/filesystem.hpp`). Device names (`app0:`, `ux0:`, ...) are mounted on host folders, read-only or
  writable. File names are matched without regard to case, and `..` never leaves a device's folder. The standard
  streams go to a sink.
- **Shader instruction model** (`usse/instruction.hpp`). A GXP file holds a primary program, run per vertex or fragment, and an
  optional secondary program, run once per draw. Their 64-bit USSE words decode into instructions with an operation, a
  type, a predicate and typed operands (bank, register number, lanes, swizzle, view, modifiers). The translator works
  from that model, not from the encodings.
  - A word that holds two operations decodes to two instructions; the second is marked co-issued.
  - A repeated instruction decodes to one instruction per repetition, with its registers stepped as the hardware
    steps them: by the increments the program last set, or by the family's own rule.
  - Words that only set state (the increments, the program prologue) decode to instructions that print nothing.
  - The formatter prints the reference disassembler's syntax, so the decoder can be checked against it word for word
    (`scripts/usse_check.py`).
- **Shader interpreter** (`usse/interp.hpp`, test and tool use only). It runs decoded programs on one instance.
  - Lanes are read by data type: words, halves or bytes of the 32-bit registers.
  - The internal registers hold four 32-bit lanes. f16 values are held there as f32, narrower integers zero-extended.
  - Before a draw, the secondary attributes are filled as the driver fills them: the default uniform buffer, uniform
    buffer addresses with their bias, and literals (`load_secondary`). Then the secondary program runs, then the
    primary one.
  - It is the oracle for the SPIR-V translator's tests.
- **Shader translator** (`gxm/translate.hpp`). It translates a primary program instruction by instruction, with the
  interpreter's semantics, so what the interpreter's checks establish carries over.
  - The banks become function-scope arrays of 32-bit words. Each lane is read and written as the interpreter does:
    the word, its bit range, and for internal registers the f32 widening of f16 values.
  - Results are written after an instruction's sources are read, and after both halves of a dual-issue word.
  - An instruction under a predicate computes its result and writes it with a select, so blocks have no branches
    inside them. Programs with branches become a loop around a switch over their blocks.
  - Secondary programs are not translated to SPIR-V. They run once per draw on the CPU (in the tests, in the
    interpreter; how the game runs them is decided with the GXM layer), and their results are the primary program's
    secondary attributes.
  - The compute form is the test form: one instance per invocation, checked on GPUs against the interpreter.
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
