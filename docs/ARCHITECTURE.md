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
C sources + game code ─► Clang ─► native executable on the runtime library psvrecore.dll / libpsvrecore.so.N
                                  (Windows, Linux), or libmain.so with the runtime linked in (Android)
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
| `runtime` | Guest memory, module loader, kernel objects and threads, HLE system libraries, files and save data, timing (one clock), audio output, NGS, the video player (MP4 demuxer and decoders), input sampling |
| `usse` | Shader programs: the GXP container (flags, parameters, vertex outputs, fragment inputs) and the USSE instruction set (decoding into an instruction model, listings in the reference disassembler's syntax) |
| `gxm` | GXM API, texture formats and layouts, SPIR-V translation of shader programs |
| `vulkan` | Vulkan backend (volk, VMA): device, memory, buffers, dispatches; knows nothing about GXM |
| `platform` | SDL3: a window and its Vulkan swapchain (the frames a game displays, scaled to fit), the audio device (a stream per port), and gamepads, keyboard, mouse and touch for the runtime's input; on Android, landscape full screen; the on-screen controls; the system UI's overlay (Dear ImGui, drawn after the picture, the controls routed to it while it shows something) |
| `frontend` | The program a game becomes: the game's description (`frontend::Game`), the settings file and the command line over it, scripted controls for smoke tests, the system's screens (the dialogs a game opens, the keyboards, notices, the Home menu; `runtime/dialogs.hpp` is their boundary with the system libraries), and the run loop (window, sound, runtime, GXM) |

One shared library, `psvrecore` (`psvrecore.dll`, `libpsvrecore.so.<ABI version>`), holds `core`, `formats`, `arm`,
`analysis`, `recomp`'s support code, `runtime`, `usse`, `gxm`, `vulkan`, `platform` and `frontend`, with
nx-foundation and the third-party code inside it; each game is an executable with its recompiled code on it. On
Android (and with `PSVRECORE_SHARED=OFF`) the same libraries are linked statically. The tests and tools link the
static libraries. See [BUILDING.md](BUILDING.md#the-shared-runtime-library).

## Key design points

- **The runtime library's interface.** What a game's executable and its generated code call is exported
  (`PSVRECORE_API`, `psvrecore/abi.h`): the frontend's entry, `guest.h`'s services and `psv_membase`, the function
  registry, whole-file I/O, and nx-foundation (its strings, containers, log and allocator: one heap and one log in
  the process); nothing else. The interface is C++, kept sound by one rule: game and runtime are built with the same
  compiler version and settings. A versioned check enforces it at start-up: the executable passes the ABI version
  and build signature it was compiled with, and the library refuses a mismatch with a message saying what to
  rebuild. State that must exist once (the HLE registry, the kernel, the function registry, the log) is in the
  library, so it exists once as before. If the C++ boundary proves fragile, the exported surface can become plain C.
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
  - The base (`psv_membase`) is set before guest code runs and never changes. Each generated function reads it once,
    into a local (`uint8_t* const mb`), and passes it to the accessors (`psv_mld32(mb, a)`...): read from the global
    at each access, the C compiler would load it again after every guest store (generated code has no strict
    aliasing). The runtime's own code uses the accessors that read the global (`psv_ld32(a)`...).
- **Analysis.** Function starts come from the module's exports, the exception index table and call targets.
  - Discovery repeats until the set of functions and the set of functions that never return stop changing. The
    second set starts from imports known by name never to return (`abort`, `exit`, the C++ throw helpers and
    others).
  - Jump tables are read from their bounds checks. When a table has no bounds check, it is taken to end at the
    first code after it. Tables of halfwords and bytes after TBB/TBH, of addresses after `ldr pc, [pc, rX, lsl #2]`
    or at the address `adr rB` puts in a register just before `ldr pc, [rB, rX, lsl #2]`, and ARM's branch tables
    after `add pc, pc, rX, lsl #2`.
  - Each imported variable's reference table lists the places that need its address. Those places are resolved at
    load time, never at generation time.
- **Generated code** (`recomp/guest.h` is the contract between the generator, the runtime and the tests).
  - Each guest function becomes `void f_<address>(PsvContext*, uint32_t ret)`. `ret` is the guest return address
    the caller expects.
  - A call sets LR and calls the C function. A return is an indirect branch whose target equals `ret`. A tail call
    passes its own `ret` on.
  - Other indirect branches look the target up among the registered modules' functions. An unknown target stops
    with a diagnostic. Jump tables become `switch` statements on the index register (the tables are in read-only
    memory the loader does not patch, so each index's target is known). An index outside the table goes where the
    entry the guest reads there leads, if that is one of the table's targets, and traps otherwise.
  - Generated code is compiled with its own options (`psvrecore_generated_code` in
    `cmake/PsvrecoreCompilerOptions.cmake`): wrapping arithmetic, no strict aliasing, no floating-point contraction,
    unwind tables; no stack protector; its symbols hidden (ELF) and bound inside the shared
    library (`-Bsymbolic` on Android), so calls between generated functions are direct; inline atomics on arm64;
    full inlining (`/Ob2`) with clang-cl; line tables only as debug information. Options for the instruction set,
    ThinLTO and debug information are in [BUILDING.md](BUILDING.md#options).
  - Registers live in the context, and the C compiler keeps them in host registers where it can. The condition
    flags live in a local of each function and are computed only where an instruction reads them (below).
  - IT blocks become per-instruction conditions.
  - Loads from read-only data are folded into constants, except words the loader patches.
  - Imports go through a per-module table the loader fills, and so do imported variables' addresses.
  - Floating point is compiled without contraction (`-ffp-contract=off`, no fast math). Flush-to-zero, default NaN
    and the ARM NaN rules are implemented explicitly, binary32 by way of binary64 with exact underflow detection.
  - Advanced SIMD always uses its standard FP mode.
  - Exclusive loads and stores become a host compare-and-swap.
- **Control-flow graph** (`analysis/cfg.hpp`). Each function's instructions as the generator translates their
  control flow: per instruction its condition (where control goes when it does not execute), its call (direct, import,
  through the runtime, svc) and its edges, to another instruction or out of the function (return, indirect jump, tail
  call, import tail call, trap); and basic blocks over them (a call ends a block). A `bx rN` just after `ldr rN, =f` or
  `movw`/`movt`, and `ldr pc, =f`, are taken as tail calls of `f` (a veneer's jump), though the generated code still
  jumps through the runtime. It is what dataflow over guest state runs on (the flags now; registers next).
- **Condition flags** (`analysis/flags.hpp`). N, Z, C and V are a local `fl` of each generated function. Q and GE
  stay in the context.
  - Each instruction's reads and writes are known (`flag_effect`): its condition; carry in (adc, sbc, rsc, rrx, rrx
    offsets); what S forms write, the carry of a logical operation only when the shifter gives one; a shift by a
    register writes the carry without ending the old one's life (a shift by 0 keeps it); mrs reads all four, msr
    APSR_nzcvq and vmrs APSR_nzcv write them. A conditional instruction's writes end no flag's life.
  - Liveness runs backwards over each function's graph, twice at once: with its returns reading no flag and with
    them reading all four. Per flag only its own bit at the returns matters, so any return set R gives
    `live(none) | (live(all) & R)` everywhere. Each function's summary: `need` (read at entry before written, itself
    or in what it calls) and `pass` (live at entry when its returns read all four). Summaries are a least fixed point
    over the module's calls and tail calls; then `live_out` (what each function's returns leave in the context) is
    propagated from callers: the flags a caller reads after the call, the `live_out` of functions that tail-call it
    or run into it.
  - A flag-setting instruction computes only the flags it writes that are live after it (`cmp` + `beq`: Z only); a
    dead one is plain arithmetic.
  - Traps show the flags the function computed: before a trap the four locals are written to the context, and the
    liveness keeps every flag live on the way to a trap back to the function's entry or its last call (no further:
    making a trap's flags exact through callers would make every function's flags live at its entry). The rest are
    unspecified at a trap. Traps are fatal; the runtime's handler (`recomp/support.cpp`) logs r0-r14 and aborts,
    and no handler reads the flags. vmsr (it traps on an FPSCR setting not implemented) and svc are such places.
  - **The contract.** Where control goes and what the context must hold there:
    - Function entry: the flags in `live_in` are read from the context (`need`, `pass & live_out`, and what a trap
      may show); the others start at 0.
    - A direct call or tail call to function G of the module (and running into G): written to the context before it
      are `need(G) | (A & pass(G))`, A the flags live after the call (for a tail call, the caller's `live_out`); after
      the call A is read back. G's `live_out` includes A.
    - A return (a return-form branch to `ret`: bx lr, pop/ldm/ldr pc from the stack, mov pc, lr): the function's
      `live_out` is written first.
    - Code the module does not know: imports, indirect calls (`psv_call`), indirect jumps (`psv_jump`), svc. Written
      before: the module's `given_at_entry` and what is live after (or the function's `live_out` for a jump); read
      back after a call: what is live after it. `given_at_entry` is the union of every function's `need` (any
      function may be entered through the function table) and `--flags-given-at-entry`. Flags read after an indirect
      call or jump returns make the module's `needed_at_return`; every function's `live_out` includes the module's
      `given_at_return` (that, and `--flags-given-at-return`).
    - Exports (functions of the module's non-main export libraries) leave all four flags at return: other modules
      call them through imports, and compiler helpers like `__aeabi_cdcmple` return their result in the flags. Their
      callers outside the module are not seen, so an import call's continuation needs nothing from the callee's module
      (an HLE implementation leaves the context's flags as they were). What an export passes on from an indirect call
      or jump it ends in is that code's: the module's `given_at_return` (an export's four flags do not make the
      module's `needed_at_return`).
    - A return-form branch to an address other than `ret` (longjmp, exception unwinding, a hand-made context switch)
      goes through the runtime with only the function's `live_out` in the context, and the code it reaches is taken
      to need no more at entry and to leave nothing its caller would read. AAPCS leaves the flags undefined there;
      this is assumed, not checked.
    - HLE entry (`call_guest`: callbacks, thread and fiber entries, `module_start`): the runtime reads no flag after a
      guest function returns, and a function that reads flags at entry reads what the context holds, as on the console
      it reads what the system left. Fibers save and restore the whole context at their switches (imports, where the
      context holds what the import call wrote).
    - `override_function` replaces a function for table lookups only: a replacement is entered with
      `given_at_entry` and what its caller reads after it in the context, and must leave in the context the flags the
      original's module gives at return (all four for an export). Host code that does not touch the flags leaves them
      as its caller wrote them.
    - mrs and msr read and write the locals (`psv_read_apsr_fl`, `psv_write_apsr_fl`); Q and GE stay in the context.
  - **Module boundaries.** Each module's `PsvModuleCode` records `flags_needed_at_entry`, `flags_given_at_entry`,
    `flags_needed_at_return` and `flags_given_at_return` (`flag_contract` 1; 0 is code generated before, which has all
    four in the context everywhere and is taken to read all four). `recomp::register_module` checks every pair of
    registered modules (what each reads, every other must give) and the loader refuses to load a program whose modules
    do not fit, naming the psvrecomp option that fixes it. `psvrecomp generate` prints each module's contract.
  - `psvrecomp --flags-everywhere` turns the liveness off: every flag computed and in the context at every call, jump
    and return (as before), for checking a suspected liveness bug.
- **Reference interpreter** (`interp`, tests only). It is written from the ARM ARM pseudocode, independently of the
  generator, and shares only the decoder and the context layout. It has its own integer soft float. Two kinds of
  test compare generated code against it:
  - random blocks run on both from the same state (in CI);
  - whole functions of a synthetic corpus (`tests/codegen`: loops, calls, returns, tables, flags across calls, VFP,
    NEON) run on both from random states, through the production memory path and through folded memory (in CI);
  - SDK-built C programs, recompiled and compared with the same source built for the host (`scripts/probe_check.py`,
    local).
- **Generated-code size** is measured by `scripts/codegen_metrics.py` (host instructions and bytes per guest
  instruction, x64 and arm64, on the synthetic corpus and on a game's tree); a change to the generator reports it
  before and after against the baseline in [CODEGEN_METRICS.md](CODEGEN_METRICS.md).
- **Runtime tests.**
  - Unit tests call the system library functions as recompiled code does, from stand-in guest functions (in CI).
  - Whole SDK-built programs (start-up code, libc, threads, files, memory) are recompiled and booted from an
    application folder, and their output is compared with what is expected (`scripts/program_check.py`, local).
- **Threads** (`runtime/kernel.hpp`).
  - Each guest thread runs on a host context of its own, but only while it holds one of the three CPUs a game has.
    The thread backend (`src/kernel/backend.hpp`) is the boundary: start a thread, park the calling one, wake one,
    give one a CPU, host work that may block, the end of a run. The kernel's objects and its scheduler use only that.
    The native backend makes an OS thread for each start, named after the guest thread, and ends it with the run
    (nothing is reused); the fiber backend ([THREAD_BACKENDS.md](THREAD_BACKENDS.md), `PSVRECORE_GUEST_FIBERS`) is
    an option: each guest thread a fiber, run by one host worker per CPU while it holds that CPU and by helper
    threads while it has none (the kernel's work after a wake, host work that blocks), with a deterministic mode
    for tests.
  - The kernel hands CPUs out as the console's scheduler does:
    - fixed priorities, FIFO among equal ones;
    - no time slicing;
    - affinity masks;
    - a thread that becomes ready takes a free CPU, or the CPU of the lowest-priority thread running below it.
  - A thread that loses its CPU stops at its next kernel call. A host thread cannot be stopped between two guest
    instructions.
  - One lock guards the kernel's state. Each thread waits on a parker of its own, which gives microsecond deadlines
    where a Windows futex timeout rounds to the timer tick.
  - A start makes an OS thread: 0.1-0.9 ms from the start call to the thread running (P4D starts 27 in a song).
  - The runtime's own state of a guest thread is `GuestLocal<T>` (per guest thread's run), not `thread_local`.
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
  - Four forms. The compute form is the test form: one instance per invocation, checked on GPUs against the
    interpreter. The secondary form runs a secondary program once per draw as a compute dispatch, on the draw's
    secondary attributes in a storage buffer. The vertex and fragment forms are the draw's shaders: attributes in
    their formats, outputs and varyings at fixed locations, the GXM viewport applied in the vertex shader, textures
    as combined image samplers found through the program's sampler slots, the colour read in the program's declared
    output type (the attachment quantizes it); a read that is only the program's final blend of the colour with
    another register (`color_read_blend`) is left to the pipeline's blending instead, the blend's other source
    written as the output. A fragment program whose secondary program reads per-tile registers
    runs that secondary first, in the fragment shader.
  - Guest memory the programs read (uniform buffers) is copied into one storage buffer per scene; the addresses in
    the secondary attributes point into it. Stores go to it and are copied back.
- **Vulkan backend** (`vulkan/`). It knows nothing about GXM.
  - Vulkan 1.1 is the minimum and 1.3 is used where the driver has it. Dynamic rendering comes from core 1.3 or
    `VK_KHR_dynamic_rendering`; without it, drawing goes through render passes and framebuffers, made once and
    cached. A pass whose shaders read the colour attachment always uses a render pass, with the attachment as an
    input attachment and a self-dependency.
  - A pipeline description is a padding-free struct, hashed and compared as bytes. Pipelines are made once and
    kept, and the driver's pipeline cache can be saved and loaded. Viewport, scissor, stencil values, depth bias
    and blend constants are dynamic state.
  - Frames in flight each have a command buffer, a fence, a stream of host-visible memory for their draws' data
    and a list of objects to release once the GPU is done with them.
  - Every use of the queue holds one lock, since the GXM layer and the display may use it from different
    threads.
- **GXM layer** (`gxm/src/hle`). The GXM API on the runtime: its objects live on the host, the guest sees handles
  (the memory it gave for them).
  - A scene is recorded as draws (each with a copy of the state and uniform data it used) and rendered when it
    ends: the colour surface is loaded from guest memory and written back, depth and stencil are loaded or stored
    when the surface asks, and the draws go into one render pass. sceGxmEndScene returns with the results in guest
    memory, so transfers, the display and the CPU see them in order.
  - Textures are decoded on the CPU from their control words and cached by those words and a hash of their data.
  - Deferred contexts record draws the same way; a command list is those draws, appended to the scene that executes
    it. Precomputed draws and states are records in the game's memory, read when a draw is made. Transfers run on the
    CPU when called. Visibility tests are occlusion queries.
- **Reference renderer** (`gxm/reference.hpp`). Draws with the interpreter and a CPU rasterizer: the rendering tests'
  oracle against the Vulkan backend (`psvrender`). Never linked into a game.
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
