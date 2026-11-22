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
    compiler keep a value across a guest spin loop. On x86-64 and arm64 they compile to ordinary moves. Their
    remaining cost, the weaker models measured and why they are not used: [CODEGEN_METRICS.md](CODEGEN_METRICS.md),
    "Guest memory access model". The runtime test of generated code waiting for another thread's store guards it.
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
    the caller expects. A function the module's code calls directly also gets an internal entry `fi_<address>` that
    takes r0-r3, sp and lr as C arguments and returns r0/r1 by value; direct calls between the module's functions use
    it, and `f_<address>` becomes a wrapper around it (below, "Internal entries").
  - A call sets LR and calls the C function. A return is an indirect branch whose target equals `ret`. A tail call
    passes its own `ret` on.
  - Other indirect branches look the target up among the registered modules' functions: a call first in its own
    module's table (below, "Indirect calls"), then through the runtime. An unknown target stops with a diagnostic. Jump tables become `switch` statements on the index register (the tables are in read-only
    memory the loader does not patch, so each index's target is known). An index outside the table goes where the
    entry the guest reads there leads, if that is one of the table's targets, and traps otherwise.
  - Generated code is compiled with its own options (`psvrecore_generated_code` in
    `cmake/PsvrecoreCompilerOptions.cmake`): wrapping arithmetic, no strict aliasing, no floating-point contraction,
    unwind tables; no stack protector; its symbols hidden (ELF) and bound inside the shared
    library (`-Bsymbolic` on Android), so calls between generated functions are direct; inline atomics on arm64;
    full inlining (`/Ob2`) with clang-cl; line tables only as debug information. Options for the instruction set,
    ThinLTO and debug information are in [BUILDING.md](BUILDING.md#options).
  - The core registers live in locals of each function and are in the context only where code outside the
    function may read them (below, "Registers"). The condition flags live in a local of each function and are
    computed only where an instruction reads them (below). The floating-point registers and the FPSCR live in typed
    locals (`float`, `double`) and are in the context only where code outside the function may read them (below,
    "VFP registers").
  - IT blocks become per-instruction conditions.
  - Loads from read-only data are folded into constants, except words the loader patches.
  - Imports go through a per-module table the loader fills, and so do imported variables' addresses.
  - Floating point is compiled without contraction (`-ffp-contract=off`, no fast math, no errno). Each VFP operation
    is the host's, checked where its result could differ from the VFP's (a NaN; with flush-to-zero, denormal operands
    and results at the smallest normal), the rest by exact helpers that implement flush-to-zero, default NaN and the ARM
    NaN rules explicitly, binary32 by way of binary64 with exact underflow detection; on arm64 with native floating
    point the host's instruction alone (below, "VFP registers").
  - Advanced SIMD always uses its standard FP mode.
  - Exclusive loads and stores become a host compare-and-swap.
- **Control-flow graph** (`analysis/cfg.hpp`). Each function's instructions as the generator translates their
  control flow: per instruction its condition (where control goes when it does not execute), its call (direct, import,
  through the runtime, svc) and its edges, to another instruction or out of the function (return, indirect jump, tail
  call, import tail call, trap); and basic blocks over them (a call ends a block). A `bx rN` just after `ldr rN, =f` or
  `movw`/`movt`, and `ldr pc, =f`, are taken as tail calls of `f` (a veneer's jump), though the generated code still
  jumps through the runtime. Each node also says which of its edges is the continuation the generator writes after
  the instruction's own translation. It is what dataflow over guest state runs on (the flags and the registers).
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
- **Registers** (`analysis/registers.hpp`). r0-r12, sp and lr are locals `g0`-`g14` of each generated function.
  - What each instruction's translation reads and writes is recorded by a first translation of the function (its
    reads and writes of registers go through the emitter), and checked against the analysis' own model
    (`register_effect`): psvrecomp lists every instruction where they differ (none in WipEout 2048's and P4D's
    modules), and corpusgen fails on one. The analysis uses the translation's.
  - A forward pass gives every register two symbolic values at each point, the guest's (the local) and the
    context's: unknown, a register's value at entry, the entry's sp plus a constant, or what the context held after
    a given call (or a store before it; a call's symbols are forgotten when it runs again). It also follows the stack
    words the function writes at known offsets from the entry's sp (push, str, strd, stm, vpush and vstm, which only
    make words unknown). A register is dirty where its two values are not known to be the same (a pop that restores
    the value the context still holds is clean). A backward pass gives the live registers: read by an instruction,
    or written to the context at a call, an exit or a trap before being written. Every register a store writes
    from its local is live there, so the local always holds the guest's value where it is written to the context.
  - **The contract.** Where control goes and what the context holds there (the same as before registers were
    locals, except where said):
    - Function entry (every way in: a direct call or tail call, `psv_call`, `psv_jump`, `call_guest` with callbacks,
      thread and fiber entries, `module_start`, a function watch, `override_function`'s replacement): the context
      holds every register. The function reads into its locals those it reads before writing them; the others start
      at 0 and are never read before written.
    - A direct call to a function of the module without an internal entry (bl/blx to its entry): before it, the
      context holds every register (each dirty one written). After it, the registers the callee preserves (below) stay
      in the caller's locals, which equal the context's; the others live after the call are read back. LR is written as
      before (`lr = next`), and the callee gets `ret = next`. A call of an internal entry: below, "Internal entries".
    - An indirect call (`psv_call`), an import (`psv_call_import`: a system library or another module's export),
      svc: before it, the context holds every register. After it, every live register is read back, except sp in a
      function that has direct callers and preserves something (they rely on it): there sp stays in the local and
      the generated code checks that the context holds the same, and traps otherwise (one `psv_trap` per function,
      `PSV_TRAP_STACK_POINTER` at the function's address: "a call to code outside the module returned with another
      stack pointer"). `--no-sp-check` assumes it without the check (sp read back), `--no-callee-saved` generates
      without the reliance.
    - A return (a return-form branch to `ret`), any jump through the runtime (`psv_jump`, a return-form branch to
      another address included), a tail call, an import's tail call, running into another function: the context
      holds every register before it. So the context is exact when a function returns, as before, and the
      differential tests compare every register at every exit.
    - A trap (`psv_trap`: undefined or untranslated instructions, a table index outside its table, a place
      discovery did not follow, an FPSCR setting vmsr does not implement, a no-return call returning): every
      register that differs from the context at the trapping instruction's start and those it has written before
      the trap are written first, so the handler's dump (r0-r14) shows the guest's registers there.
    - Host code that reads the context while generated code runs without calling it: the guest memory fault report
      and the memory write watch (`PSVRECORE_WATCH=<address>`) print `t->ctx`, which holds the registers as of the
      function's last call, exit or trap; later values are in locals. Before, the C compiler could also keep values
      in host registers past the stores; now the window is the whole stretch between calls.
    - System libraries: the HLE adapter reads r0-r3 and the stack through sp from an exact context and writes r0
      (and r1); `call_guest` writes r0-r3 and lr, moves sp for stack arguments and puts it back; fibers save and
      restore the whole context at their switches. All of this happens inside an import call, after which every
      live register but a checked sp is read back. `sceKernelExitThread` ends a thread by a C++ exception through the
      generated frames: their locals are dropped, the context was exact at the import call.
    - mrs, msr, vmrs, vmsr: the flags and FPSCR, no register beyond the instruction's own operands.
    - Guest memory: unchanged. Pushes store the same values at the same addresses, so the guest's C++ exception
      unwinding (which reads saved registers from guest frames, and still traps where it leaves a function by a
      non-local path, KNOWN_ISSUES) sees the frames it did.
  - **Callee-saved registers.** A function preserves r4-r11 and sp that are, at each of its returns, the values they
    had at entry, when it is entered with LR holding the address it returns to (a direct call): shown by its own code
    (each such register still its entry value, or loaded back from the word it was pushed to; sp moved back, through
    a frame pointer as well), and through its calls (after a direct call the callee's preserved set keeps its symbolic
    values; after any other call only sp, by the check above). Every return must branch to the LR value the function
    was entered with (else it is a jump through the runtime to code that may do anything: nothing preserved), a tail
    call must pass that LR on and the callee preserve the register, an import's tail call preserves nothing. The
    sets are the greatest fixed point over the module's direct calls and tail calls: a function that calls itself
    keeps what its returning paths keep (by induction over the length of a returning execution). A caller keeps the
    callee's preserved registers in its locals across direct calls to it (`--keep-registers all`, the default), only
    across those inside a loop of the caller (`loops`), or nowhere (`none`: read back, though known the same), and
    reads back the rest.
  - The proof is cross-checked against the module's exception index table (`.ARM.exidx`, ARM EHABI: the compact
    models and the generic one): a function found to preserve a register it writes should have that register
    restored by its unwind opcodes. psvrecomp prints the counts. WipEout 2048's and P4D's ten modules: every function
    with unwind opcodes that describe a frame agrees; the entries that describe none (only "finish", what the
    toolchain gives a function that calls nothing, though it may push registers) are not compared. What is assumed
    and not checked is in KNOWN_ISSUES: the saved words are changed by nothing but the function's own stores.
    Builds with `PSV_CHECK_KEPT_REGISTERS` (`PSVRECORE_GENERATED_CODE_CHECK_KEPT`) check after every such call that
    the context holds what the locals kept (`PSV_KEPT`, `PSV_TRAP_KEPT_REGISTER`; the masked corpus tests are built
    so).
  - **Module boundaries.** Nothing is assumed of another module's code beyond the checked stack pointer: calls into
    it (imports, indirect calls) keep nothing else, and other modules' calls into this one (through their imports)
    get an exact context and give one. So `PsvModuleCode` records nothing for the registers, and code generated
    before (which has every register in the context at every call and return) and after mix.
  - `override_function` replaces a function for table lookups only (`psv_call`, `psv_jump`, `call_guest`, thread
    entries); direct calls stay calls of the generated function, as before. A replacement is entered with an exact
    context and must leave the registers in the context; nothing is assumed of what it preserves, as nothing that
    reaches it through the runtime keeps a register across it.
  - `psvrecomp --registers-in-context` generates the code from before, every register in the context (byte for
    byte what the generator gave before: checked on WipEout 2048's four modules); `--no-callee-saved` keeps the
    locals and reads every live register back after every call (stage A alone), for chasing a suspected bug in a
    game.
- **Internal entries** (`analysis/registers.hpp`, `EntryKind`). The context is the calling convention of
  `f_<address>`: every register is in it at a call and back in it at the return. Direct calls between the module's
  functions need not pay that: they use an internal entry.
  - **Which functions.** A function with direct callers (bl/blx), that may return, whose every way out returns to
    the LR it was entered with (`returns_to_lr`, shown with sp kept across calls to code outside the module's view, which
    such a function then checks after them, as callers of a preserving function rely on), and that ends in no tail
    call (no branch, table entry, veneer or running into another function, no import's tail call). Not one listed by
    `psvrecomp --no-inline` (a function a game stands in for by renaming its definition), and not one replaced by a
    host implementation (`--replace`, "Host implementations of guest functions" below).
    WipEout 2048's executable: 6224 of its 14792 functions; 7219 have no direct caller, 1338 may return elsewhere than
    their LR (most of them: a store at a register offset from sp, which ends what the analysis knows of the frame and
    so of the saved LR, or a call of such a function, which then loses sp), 11 never return.
  - **Its signature.** `RT fi_<address>(PsvContext* ctx, uint32_t g<k>...)`: the registers of r0-r3, sp and lr it
    reads at entry, in that order (`kArgumentRegisters`; its liveness, a least fixed point over the module's calls:
    what its callees read at entry is read at the call). It reads r4-r12 from the context. RT returns r0 and r1 where
    it may change them: `uint64_t` (r0 | r1 << 32: one register on x64 and arm64), `uint32_t` or `void`. What it may
    change (`modified`) is what its code and its callees' may write (`writes`, everything when it reaches code outside
    the module's view) less what it preserves (r4-r11 and sp, the callee-saved proof). The returns compare nothing
    with `ret`: every way out goes back to the LR it was entered with, which its caller set.
  - **Two kinds.** `Inside`: nothing it may reach is outside the generated code's view: no import, no call through the
    runtime, no svc, nothing that may trap, no call of a function without an internal entry, transitively over its
    direct calls. `Flushed`: the others.
  - **The contract at a direct call of G** (what the context holds; the condition flags and the floating-point
    registers keep their contracts, through the context, unchanged):
    - G `Inside`: the caller writes to the context only its dirty r4-r12 (not r0-r3, sp, lr: no `ctx->r[14] = next`),
      passes G's arguments from its locals (lr = the return address when G reads it: it pushes it, so the guest's
      stack holds what it always did), and takes the results. At G's entry r4-r12 are exact in the context, r0-r3, sp
      and lr hold whatever the caller's context held: G's analysis starts with them dirty (`caller_ctx`).
    - G `Flushed`: the caller writes every dirty register first, lr included, as before; G's entry finds the context
      exact. It passes the arguments as well (G need not read them again).
    - G's returns: the results by value; every other register G may change written to the context where its local
      differs (so the context holds G's value for it); a register G does not change but whose context copy a callee
      changed (one G saved and restored) put back; the argument registers G neither may change nor had changed left as
      they are (the caller has them in locals). After the call the caller has the results in locals, reads back from
      the context the other registers G may change that are live, and keeps the rest in its locals (no reload of
      r0-r3, sp or lr from a context that does not hold them).
    - `f_<address>` (the wrapper; its definition is `void f_<address>(PsvContext* PSV_RESTRICT ctx, uint32_t ret)`
      with the brace on the next line): reads the arguments from the context, calls the internal entry, writes the
      results back, and when it was entered with LR other than `ret` makes the jump through the runtime to LR that the
      function's returns made before (`psv_jump(ctx, lr, ret, <function>)`: the diagnostic's address is the function's
      now, not the return instruction's). Where a return is a branch through another register than lr (a copy of the
      LR: an indirect jump in the graph), the internal entry leaves the flags such a jump through the runtime would (the
      module's `given_at_entry` too), as the returns did on that path before. So entering a
      function through `f_<address>` (the function table, `psv_call`, `psv_jump`, `call_guest`, thread entries,
      imports from other modules, `override_function`'s callers) sees what it saw before: an exact context in, an
      exact context out; the differential tests compare every register at every function's exit through it.
  - **Why code outside the view still sees an exact context.** Callers up the host stack keep registers in locals
    across internal calls, so the context alone is no longer exact at any depth. What makes it exact where something
    reads it: r4-r12 are written before every internal call where dirty (as before); r0-r3, sp and lr enter an
    `Inside` entry dirty, so wherever its code writes registers to the context (a trap, a call through the runtime,
    an import: places an `Inside` entry has none of, by its kind, but the property does not rest on that) it writes
    them from its arguments, which its liveness then makes it take; and every call of a `Flushed` entry or a function
    without one writes everything first. So the kinds are a choice of where to write, not a proof obligation: taking
    a function that reaches an import to be `Inside` (checked: a mutation that does so passes every test, and why)
    only moves the writes down to where the import is called. The test that checks it: the corpus' `obs_*` functions,
    reached only through `psv_call` at the bottom of chains of internal calls, have the whole context compared with
    the interpreter's at their entry (`codegen_tests.cpp`).
  - Guest memory: unchanged. The pushes store the same words (lr is the return address its caller passed); returns
    read the saved LR from the frame as before (and do not use it: it is the address the caller gave, by the proof).
  - What is assumed and not checked (KNOWN_ISSUES): the frame's saved LR is changed by nothing but the function's own
    stores, the assumption the callee-saved proof already makes; before, a return to an overwritten saved LR became a
    jump through the runtime (usually a trap: no function there), now it returns to the caller.
  - Builds with `PSV_CHECK_KEPT_REGISTERS` check after a call of an `Inside` entry only r4-r12 against the context.
  - `override_function` replaces a function for table lookups only, as before: direct calls (internal entries,
    inlined copies) are compiled in. A game that stands in for a function by renaming its generated definition (a
    script matching `void f_<address>(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {`) must give it
    `psvrecomp --no-inline <address>`: the script then finds the definition as before; without it the script finds
    nothing and stops, rather than direct calls silently going around the stand-in.
  - Watches and fault reports (`PSVRECORE_WATCH`, the guest memory fault report) print the context, which now holds
    a register as of the last time any frame on the host stack wrote it (an internal call writes nothing of r0-r3, sp
    and lr), no longer of the current function's last call.
  - `psvrecomp --no-internal-calls` (with `--no-call-table`) gives the code from before byte for byte (checked on
    WipEout 2048's four modules and P4D's six). Statistics: `psvrecomp generate` prints the kinds, why functions have
    no internal entry, the direct calls of each kind and why calls write the context; `PSVRECOMP_ENTRIES=<file>` lists
    every function's entry.
- **Inlining.** An internal entry of at most `--inline-limit` instructions (8 by default; 0: none) with no jump table
  is `static inline` and has a copy in each .c file whose functions call it (with those it calls), so that the C
  compiler can inline it where it is called across files; its wrapper stays in its own file. Not a function given
  `--no-inline`. The other internal entries are declared `noinline`: the C compiler would otherwise copy them into
  their wrapper and the callers in their own file while keeping the out-of-line copy the other files call (measured: a
  larger program for no measured gain; `--inline-in-file` allows it). What the copies cost and buy is in CODEGEN_METRICS.md; ThinLTO
  (`PSVRECORE_GENERATED_CODE_LTO`) inlines across files as well, at a link time unacceptable as a default.
- **Host implementations of guest functions** (task 8: `psvrecomp generate --replace <target>=<symbol>[/<guard>]`,
  `--replace-table <file>`; `recomp::Options::replacements`; the functions in `recomp/src/host_functions.cpp`, the
  table `tools/psvrecomp/host_functions.tsv`, installed as `share/psvrecore/host_functions.tsv`). A target is an
  address or `nid:[<library NID>:]<function NID>`, resolved against the module's own export; a table line is
  `<module fingerprint or *> <target> <symbol> [<guard>]`, the fingerprint the SHA-256 of the module's ELF
  (`Image::fingerprint`), so another build of the same library is another module and gets none of its lines.
  - **What the analyses see.** The program is analysed as it is: the replaced function stays a function, its
    callers' calls stay direct calls of it. It gets no internal entry and no inlined copy (`no_internal`), and the
    register solver gives its callers a contract in place of what its code proves (`RegisterOptions::hosts`,
    `kHostWrites`/`kHostPreserved`): it writes r0-r3, r12 and lr, keeps r4-r11 and sp (with a guard: also what the
    guest code writes, and of r4-r11/sp only what that code is shown to keep), and is called as a Flushed callee (every
    dirty register in the context before the call; a caller keeps r4-r11 and sp in its locals across it). It is not
    "outside" for that: its callers are not made to read the context back as after a call of unknown code. Flags and
    VFP registers follow the guest code at its calls (the flag analysis' `into`), which the host function's contract
    covers: the flags are dead after an AAPCS call (a caller that reads them is counted, below). A function whose code
    reads flags at entry (`FunctionFlags::need`) is not a C function and is not replaced, nor one at an address no
    function starts at, nor an import's stub; each is listed (`replacement_notes`), and the module is translated as
    without it. (An earlier version made the entry an import's stub; P4D libc measured both ways in CODEGEN_METRICS.md,
    task 8: the stub made every caller write the whole context at the call and lose r4-r11 across it.)
  - **What it writes.** Where the guest's code reaches the function (a direct call, a tail branch, a table case,
    running into it), a call of the host function `void S(PsvContext*, uint32_t ret)` with the return address the guest's call has (a
    tail branch and a table case pass their own `ret` and return). `f_<address>`, the module table's entry, is
    `{ S(ctx, ret); }`: indirect calls, `psv_call`, and other modules' imports resolved to the function's export find
    it there. With a guard `int G(const PsvContext*)`, callers call `f_<address>`, which is
    `if (G(ctx)) { S(ctx, ret); return; } fo_<address>(ctx, ret);`, `fo_<address>` the guest code's translation.
    The generated header declares S, G and fo_. A jump to a constant address (`ldr pc, =A`) goes through `psv_jump`
    to `f_<address>`; in `fo_`, the guest code's branch to its own entry is a loop and its call of itself calls
    `f_<address>` (the guard again); calls of inlined callees are inlined as in any function. Tests:
    `tests/replace_tests.cpp` (the text) and `psvrecore_host_functions_tests` (replace_module.hpp's ten callers, run
    against the interpreter).
  - **The host function's contract.** It leaves what a C caller may read after the call as the guest's code does:
    r0 (and r1 of a 64-bit result), r4-r11, sp, d8-d15, memory, and for the atomics the exclusive monitor as the
    guest's strex leaves it. r1-r3, r12, lr, the flags and the other VFP registers are the callee's by the AAPCS and
    need not hold what the guest's code leaves there. A caller that reads one of those after the call anyway (hand
    assembly, or a compiler that knew the callee) is counted and listed (`replaced_relying`; a forward scan of each
    call's return along the CFG to the next call, return or jump): none of P4D libc's 267 calls of its replaced
    functions. It is not checked across modules (another module's call through an import is not scanned).
  - **The table's lines.** Only modules whose functions were read instruction by instruction and compared with the
    host function on random inputs in the interpreter (`PSVRECORE_TEST_LIBC`, CODEGEN_METRICS.md, task 8): P4D's
    libc.suprx (memcpy where the forward copy gives memmove's result: `psv_host_memcpy_exact`; memmove, memset,
    memcmp, strlen, strcmp, strncpy, wcslen; `_Atomic_fetch_add_4`/`_sub_4` for memory orders 0-5:
    `psv_host_atomic_order_valid`, the guest's code asserts beyond). That comparison is local only (the module is
    Sony's and is not in the repository; without the variable the test case is skipped): `PSVRECORE_TEST_LIBC=<path
    to libc.suprx> build/windows-x64-release/tests/psvrecore_host_functions_tests.exe`; last run 2026-11-21 with P4D's
    libc.suprx: 14906 runs, 0 failures. psvrecomp prints `N of M table lines apply to this module (fingerprint ...)`
    whenever a table is passed, and rejects a `psv_host_*` symbol or guard the runtime does not define
    (`recomp::kRuntimeHostFunctions`, checked against guest.h by a test).
- **Indirect calls** (`guest.h`, `PsvDispatch`, `psv_call_in`). Each module has a table that registering it fills
  (`recomp::register_module`): by the halfword of code from its lowest Thumb function, the u16 index of the Thumb
  function starting there, and an array of what the registry gives for each (a module of 65535 functions or more gets
  u32 entries, which only `psv_call` reads). Generated code's indirect calls (`blx rN`, `bl` to an address no function
  starts at) call the module's `<name>_call` (in its `_module.c`), which looks in the table and calls what it finds,
  without a call into the runtime; anything else (ARM code, other modules, import stubs, addresses no function starts
  at) goes to `psv_call`, which looks in every registered module's table, then
  in the registry's map and the import stubs. What is called is what `psv_call` would call: the context's contract
  at an indirect call (flags, registers) is unchanged. `override_function` updates the tables (an atomic store) and
  `clear_modules` empties them; a module registered while guest code runs (one the game loads itself) publishes its
  table complete before its span. Memory: two (four) bytes per halfword of the module's Thumb code span plus eight
  per Thumb function (WipEout 2048's executable: 5.3 MB of code, below). `psvrecomp --call-table-inline` puts the
  lookup at each call site instead (`psv_call_in`): as fast on the corpus, but Minecraft's 85704 sites made its
  program 14% larger (x64), against a call of the shared function; with the shared one, every indirect call of a module
  goes through one indirect branch in `<name>_call`, whose prediction on a game with many virtual call targets is not
  measured. `psvrecomp --no-call-table` gives the calls through `psv_call` alone. A per-site inline cache was not added: no game with many virtual calls runs here to
  measure one (Minecraft has never been built).
- **VFP registers** (`analysis/vfp.hpp`). The floating-point registers and the FPSCR are typed locals of each generated
  function: d0-d15 as two `float` locals `s<2n>`, `s<2n+1>` or as one `double` `d<n>` (by how the function's
  instructions access them: a word at a time at least as often as whole, two floats), d16-d31 as doubles, the FPSCR as
  `fpscr` (bits 27-0) and `fpnzcv` (bits 31-28, what vcmp writes). A unit is one such local; an access to part of a
  unit (an S register of a double-held register: `psv_vlo32`, `psv_vsetlo64`...; a whole D register of float-held
  ones: `psv_vpack64`; a byte or halfword lane) reads the whole unit. Moves, loads and stores copy bits: a float or
  double copy keeps every bit pattern, signalling NaNs and payloads included (no host operation that canonicalises
  NaNs is involved; `tests/vfp_typed_tests.cpp` checks the moves, the corpus' `nan_moves` the whole path).
  - What each instruction's translation reads and writes is recorded by the first translation (as for the core
    registers), in words of the register file. A forward pass gives the units that may be dirty, a backward pass the
    live ones: read by an instruction, or written to the context at a call, an Advanced SIMD instruction, an exit or a
    trap, before being written. Every unit a store writes from its local is live there.
  - **The contract.** Where control goes and what the context holds there:
    - Function entry (every way in): the context holds every register and the FPSCR. The function reads into its
      locals the units it reads before writing them; the others start at 0 and are never read before written.
    - A direct call to a function G of the module: before it, the context holds every unit G may read or write (each
      dirty one of them written); after it, the live units G may write are read back, the others stay in the caller's
      locals. What G may read and write is its summary: the words and FPSCR parts its own instructions touch and its
      callees' (direct calls and tail calls), the least fixed point over the module; everything when G calls an
      import, calls or jumps through the runtime, makes svc, ends in an import's tail call, or when its returns are
      not shown to go back to the LR it was entered with (`FunctionRegisters::returns_to_lr`: a return to another
      address is a jump through the runtime to code that may do anything). No calling convention is assumed: a callee
      that saves d8 with vpush and restores it with vpop still writes d8 (its caller reads d8 back); one that touches
      no floating-point register (most integer helpers) leaves every local alone.
    - Imports, indirect calls (`psv_call`), svc: before them the context holds every unit; after them every live unit
      is read back.
    - Advanced SIMD instructions are translated on the locals' values like any other instruction (below, "Advanced
      SIMD"). Only one without a translation on values, and every one with `psvrecomp --neon-in-context`, works on the
      context's registers (guest_simd.h's helpers): before it, the dirty units of the D registers it names (lists: more
      than it names, never fewer) and the FPSCR's rest (QC, AHP) are written; after it, the live ones are read back.
    - A return, a jump through the runtime, a tail call, an import's tail call, running into another function: every
      dirty unit is written first. The context is exact when a function returns; the differential tests compare
      d0-d31 and the FPSCR at every exit.
    - A trap: the dirty units are written first (on the trapping path), as the core registers are.
    - Host code that reads the context while generated code runs without calling it (the memory fault report, the
      write watch): it prints the core registers; the floating-point registers there are as of the function's last
      call, exit or trap.
    - System libraries: the HLE adapter reads float arguments from s0-s15/d0-d7 and writes results to s0/d0 in an
      exact context (an import call), after which every live unit is read back. Fibers save and restore the whole
      context, FPSCR included, inside their imports.
    - vmsr: the new FPSCR in both locals (`psv_vmsr`; a setting not implemented traps, with the dirty units written
      first on that path); vmrs: from the locals.
  - **Compares.** A vcmp/vcmpe and the vmrs APSR_nzcv after it in the same block (with no other compare, vmsr, vmrs
    or call between) keep the compared operands in `fc32a/b` (`fc64a/b`), flushed in flush-to-zero mode
    (`psv_vflush*`); the vmsr computes only the flags something reads after the conditions that follow it, and those
    conditions (up to an instruction that writes or otherwise reads the flags, a call or the block's end) compare the
    operands instead: each condition code is one ordered comparison or its negation (`gt`: `a > b`, `ls`: `a <= b`,
    `lt`: `!(a >= b)`, `vs`: unordered...). `vcmp; vmrs; bgt` is one host compare and branch. The FPSCR's NZCV is still
    computed (`psv_vnzcv*`) where something may read it (a vmrs to a core register, a call, an exit).
  - **Exactness.** The operations are guest.h's typed ones (`psv_vadd32`, `psv_vcvt_s32_f64`...). On x64 (and arm64
    by default) each is the host's operation and a check: with FPSCR.FZ clear the host's IEEE result in round to
    nearest is the VFP's unless it is a NaN (the VFP's operand order, signalling NaNs first, its positive default NaN,
    DN), so a NaN result takes the exact helper; with FZ set an inline check also sends denormal operands and results
    at or below the smallest normal (tininess before rounding) there (one unsigned minimum over each value's
    magnitude bits less one, times two; an operand at the smallest normal goes there too, needlessly but exactly).
    Only the helpers are out of line; both checks are inline because a call in a loop costs the spills of every live
    floating-point local, and clang threads the FZ test through a function (two copies of a run of operations, one
    per mode). Conversions to integers use x86's conversion and take the exact helper for its out-of-range answer
    (0x80000000), conversions from integers and fixed point are exact by construction (`guest.h` says why),
    float/double conversions check like the arithmetic.
    Multiply-accumulates stay two rounded operations (`-ffp-contract=off`), negation and absolute value are sign-bit
    operations. Rounding is always to nearest (a vmsr selecting another mode traps), and the cumulative exception
    flags are not computed, in the generated code and in the interpreter alike.
  - **Native floating point (arm64, `PSVRECORE_GENERATED_CODE_NATIVE_FLOAT`, off by default, not yet verified on a
    device).** `PSV_NATIVE_FLOAT`: each operation is the host's instruction (volatile inline assembly: `fadd`, `fmul`,
    `fdiv`, `fsqrt`, `fnmul`, `fcvt`, `fcmp` + `mrs nzcv`; conversions to integers `fcvt[zn][su]`, which saturate and
    give 0 for a NaN as VFP does), relying on the host's FPCR having the guest FPSCR's FZ and DN: AArch64's
    flush-to-zero (inputs and results, tininess before rounding), default NaN (positive) and NaN propagation (first
    signalling, then quiet, the first operand first) are ARMv7 VFP's. The assembly keeps the operand order (the
    compiler would swap an addition's operands, which picks the other NaN), keeps constants from being folded under the
    default environment, and keeps operations from moving across a write of the FPCR (a vmsr, a call). Comparisons are
    C, and the generated code is built with `-fdenormal-fp-math=dynamic` (the compiler assumes nothing about how
    denormals compare). The runtime keeps the mirror (`runtime/float_env.hpp`): set at a guest thread's start, by
    `call_guest` (and back to what its caller had after), after a sceFiber switch (the resumed fiber's FPSCR), carried
    by each guest fiber of the fiber backend across its host threads; vmsr updates it. HLE code runs in it, except the
    imports of libraries registered `host_float` (`hle::Library`: JSON, NGS, SAS, the audio, video, JPEG and codec
    engines, AvPlayer, PVF fonts, motion, system gestures, voice, SceLibKernel (printf's `%f`), SceAudio, SceGxm,
    SceDisplay), which run in the host's default environment (`HostFloatScope`, a trampoline per bound function).
    `tests/device` has the tests that check it on a device ([BUILDING.md](BUILDING.md#device-tests)).
  - `psvrecomp --vfp-in-context` generates the code from before, every floating-point register and the FPSCR in the
    context (byte for byte what the generator gave before: checked on WipEout 2048's four modules);
    `--vfp-calls-conservative` keeps the locals and takes every call to read and write everything (no summaries), for
    chasing a suspected bug in a game.
- **Advanced SIMD** (`recomp/src/emit_neon.cpp`, `recomp/guest_neon.h`). NEON instructions work on the values of the
  floating-point locals above: each becomes a call of a guest_neon.h helper on `PsvD` (8-byte) and `PsvQ` (16-byte)
  vector values, its operands read from and its results written to whatever local holds each register.
  - **Q registers as vector locals.** Besides the S and D views, q<p> (d<2p> and d<2p+1>, any p < 16) can be one
    `PsvQ` local `q<p>`: chosen when the function's instructions access the Q register whole (recorded as for the other
    views) at least half as often as its D and S registers apart. A Q-held unit is all four words: a D register of it
    is its low or high half (`psv_qlo`, `psv_qsethi`), an S register a lane (`psv_vqf`, `psv_vqsetf`), the context's
    copy written and read as 16 bytes (`psv_ctx_qput`, `psv_ctx_qget`). The analysis is the VFP registers' with the
    new unit (`FunctionVfp::q_view`, `units()`), so the contract above holds unchanged for Q-held registers: entry,
    direct calls with the callees' summaries, imports, indirect calls, svc, exits and traps. NEON instructions are no
    longer places where the context is synced: they read and write locals, and are in the summaries through the words
    they read and write. The FPSCR's QC is a bit of the `fpscr` local, which the saturating helpers OR into
    (`uint32_t* fpscr`).
  - **Helpers.** Each family is element-wise vector code in clang's vector extensions (`vector_size`,
    `__builtin_shufflevector`, `__builtin_convertvector`, `__builtin_elementwise_*`), which compiles to NEON on arm64 and
    SSE2 on x64: integer arithmetic, compares, min/max/abs/neg, halving, saturating (`__builtin_elementwise_add_sat`;
    QC from a compare with the wrapping result), shifts by an immediate and by a register (rounding, saturating; counts
    clamped before every host shift, so lanes never shift by their size or more; no `__int128`), inserts, logic and
    the bit selects, long/wide/narrow forms by `__builtin_convertvector`, pairwise forms, doubling multiplies,
    polynomial multiplies (arm64: PMUL/PMULL), counts (arm64: CNT/CLS/CLZ; x64: SWAR), permutes and VEXT/VREV/VDUP
    lanes as constant shuffles (macros, so the indices are constants), VTBL/VTBX (arm64: TBL; x64 with SSSE3: PSHUFB;
    SSE2: the eight bytes one by one), estimates and conversions. The only lane-by-lane code left: VTBL on an SSE2-only
    host, the half-precision conversions (four lanes through guest.h's exact helpers; x64's baseline has no
    half-precision instructions), and the conversions to integers on a host that is neither x64 nor arm64.
  - **Floating point** (single precision; Advanced SIMD always uses the standard mode: flush-to-zero, default NaN,
    round to nearest, whatever the FPSCR says). Inputs are flushed (a denormal lane to a zero of its sign) and results
    fixed (a NaN lane to 0x7FC00000, a lane below 2^-126 to a zero of its sign). With no denormal operand the host's
    IEEE result in round to nearest is ARM's in every lane except for NaNs (ARM's is always the default NaN where IEEE
    gives a NaN) and results whose exact value is below 2^-126 (ARM flushes them; the host gives a denormal or zero of
    the same sign, or rounds up to 2^-126), so the fix makes it exact, with one exception: a product at exactly 2^-126
    may come from an exact value below it (a sum cannot: a sum of floats below 2^-126 is exact). A vector with such a
    lane decides it inline in a rare branch (no call: x64 computes the exact products in double precision, arm64 the
    sign of a fused residual scaled by 2^48). VMLA/VMLS round the product and flush it before the sum (ARMv7 has no
    fused NEON form; AArch64's FRECPS/FRSQRTS are fused, so VRECPS/VRSQRTS are a product and a difference, with
    infinity times zero taken as +0). Compares, max/min, pairwise forms work on flushed operands. Every case,
    NaN operands, infinities, invalid operations and denormals included, is computed inline: unlike task 5's VFP
    operations, NaN-heavy inputs take no slower path. This holds under any FZ/DN setting of the host's MXCSR or FPCR
    (only round to nearest is assumed), so the same code is exact on x64, on arm64 by default, and on arm64 with
    native floating point when the guest's FPSCR is not in the standard mode.
    - Operands known to hold no denormal are not flushed again: the emitter follows, within a straight stretch of code
      (forgotten at labels, calls, conditional instructions and any other write), the D registers last written by a
      floating-point helper (whose results never hold one), a copy, a constant without a denormal lane, VDUP of such a
      lane, VABS/VNEG of one, or a conversion to single precision, and passes `known` bits.
    - Estimates: arm64's FRECPE, FRSQRTE, URECPE and URSQRTE compute ARMv7's 8-bit estimates (ARMv8's RecipEstimate and
      RecipSqrtEstimate are defined to; FRECPE/FRSQRTE on a flushed operand, the result fixed: with FPCR.FZ clear a
      reciprocal of |x| >= 2^126 comes out denormal, which ARM flushes). On x64 the estimate is computed in single
      precision from the table index (a division for the reciprocal, a square root for the other), which equals the
      integer definition for all 256 indices of each table (checked by `simd_tests.cpp`, and IEEE single precision is
      the same on every host).
    - Conversions to integers: arm64's FCVTZS/FCVTZU (saturating, NaN to 0: ARM's); x64's CVTTPS2DQ corrected
      (2^31 and above to 0x7FFFFFFF, NaN to 0; unsigned by subtracting 2^31 from large values, saturating at 2^32 and
      at -1). Fixed point: the flushed value times 2^fraction (exact, or an infinity that saturates as the exact value
      would), then the conversion; back, the integer rounded, then scaled exactly.
    - **arm64 with native floating point** (`PSV_NATIVE_FLOAT`, task 5's option, off by default, not verified on a
      device). NEON needs the FPCR in the standard mode (FZ and DN set), but task 5 mirrors the guest's FPSCR into the
      FPCR (normally FZ = DN = 0). Resolution: each floating-point helper tests the `fpscr` local; when the guest's
      FPSCR has FZ and DN set (so the FPCR has them), the operation is the host's instruction alone (volatile assembly:
      FADD, FMUL, FABD, FCMxx, FACGx, FMAX/FMIN, FADDP/FMAXP/FMINP, FRECPE, FRSQRTE; VMLA as FMUL + FADD), otherwise the
      checked code above, which is exact under any FPCR. Nothing changes the FPCR for NEON, so no new ordering between
      FPCR writes and plain C floating-point code (task 5's compares) arises. The alternative of switching the FPCR to
      the standard mode around runs of NEON floating-point instructions was measured statically and not taken: WipEout
      2048 has 16402 such instructions in 2536 runs (6.5 per run; P4D 10.5), so it would cost two FPCR writes per 6.5
      operations (glibc avoids FPCR writes as slow; their latency is unmeasured here), and 87% of WipEout's NEON
      floating-point instructions are in functions that also do VFP compares or arithmetic in C, which clang may move
      across the FPCR writes. Games that never set FZ and DN (WipEout and P4D were not seen to) run the checked code.
  - **Loads and stores.** VLD1-4 / VST1-4 move whole registers: each 16 bytes (two registers) or 8 bytes by one host
    access of that size (`psv_mldq`, `psv_mldd`: volatile, so not merged, split or removed, as the relaxed atomic
    scalar accesses), never touching a byte the instruction does not; structure forms (de)interleave by constant byte
    shuffles of those chunks; single lanes and all-lane forms load each element once. An element is single-copy atomic
    where the host's access keeps it so (an aligned 8-byte part of it on x64 and arm64). With `PSV_ADDRESS_MASK`
    (tests) each element is folded on its own, as before.
  - `psvrecomp --neon-in-context` generates the code from before (byte for byte task 5's: checked on WipEout 2048's four
    modules), every NEON instruction in the context form; `--no-q-locals` keeps NEON on values but no Q register as one
    vector (two D locals).
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
