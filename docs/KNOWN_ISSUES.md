# Known issues and open questions

## Build and platforms

- **Linux has not been built or tested.** The development host has no Linux machine and no WSL. The CI workflow
  covers Linux, but it runs only once the repository is pushed.
- **Android has nothing in its default build yet.** Tools and tests are host-only, and there is no runtime yet.
  nx-foundation cross-compiles for arm64. The address-space test has not run on an Android device; it will run once
  there is a device test runner.

## Formats

- **Encrypted SELFs are refused.** Decryption (keys supplied by the user) is a later milestone.
- **Relocation types beyond the common ones are refused.** Every SDK-built binary checked uses only three types:
  ABS32, THM_MOVW_ABS_NC and THM_MOVT_ABS. Also implemented, and unit-tested against hand-computed or
  assembler-produced values, but never seen in a real file: REL32, PREL31, TARGET1, ARM MOVW/MOVT.
  - Branch, GOT-relative and other types are errors.
  - Long entries' second relocation is decoded, and the SDK binaries use it for MOVW/MOVT pairs.
  - Symbol segment 15 has no special meaning here. Whether it means "absolute" in any binary is unknown, so it is
    treated as a segment index (and fails when the module has no such segment).
- **Untested on real files:**
  - the short (0x24-byte) import entry, which no SDK file has;
  - TLS imports and exports;
  - ELF type 0xFE00 (fixed-address executables).

  All of these are covered by synthetic tests only.
## Instruction decoder

- **The reference's behaviour on unpredictable encodings is mirrored, not the architecture's.** Where the ARM ARM
  calls an encoding UNPREDICTABLE, llvm-objdump sometimes rejects it and sometimes decodes it. The decoder follows
  llvm-objdump's choice so the two can be compared, and flags every decoded case `kUnpredictable` for the
  recompiler to refuse. Three examples:
  - nested IT blocks (a pending-condition stack);
  - LDRD with an odd first register;
  - VLDM with too many registers (clamped).
- **Encodings the checker cannot match** (`unmatchable()` in `scripts/arm_check.py`; about 0.01% of random
  encodings, none in real code):
  - **Thumb REV/REV16/RBIT/REVSH/CLZ with different Rm fields:** the reference prints a register that is in neither
    field.
  - **VTBL/VTBX and VLD1/VLD2 register lists running past d31:** the reference names the registers after d31 as
    system registers.
  - **ARM `mov` (register) with a nonzero Rn field:** the reference accepts some and rejects others, with no rule
    this decoder could find. psvdis rejects all of them.
  - **CPS (privileged; never in user code):** the reference's exact field checks are not reproduced.
- **Cortex-A9 instruction set only.** SDIV/UDIV and VFPv4 (VFMA and friends) decode as undefined, as on the
  console's CPU. ARMv8 instructions are out of scope.
- **The checker needs LLVM.** It runs llvm-objdump, so CI runs it on Windows only, where the runner image has LLVM.

## Shader decoder (USSE)

- **Checked only against the SDK's disassembler.** Its listings are taken as the reference for what each word means.
  Where it prints something odd, the decoder prints the same, so a listing match is not proof of the hardware's
  behaviour. The interpreter and rendering tests are what check the semantics.
- **Every word the compiler emitted in the corpora matches; some random words do not.** On words of random bits, these
  differ from the reference (none was seen in compiled shaders):
  - repeats of families 9, 14, 18, 25, 28, 29 and 30 decode as one repetition. Families 15 and 26 execute their
    repetitions and the increments' offsets, which the reference listing does not show (see the interpreter);
  - in repeats, the reference steps immediates, indexed offsets and index-register destinations in printed units, and
    keeps stepped temporaries as temporaries; the decoder does not;
  - small-integer selector layouts of family 8 (pack), u8 variants of family 9 (tests), rare types of family 15;
  - the alpha line of family 16 (blend) when it multiplies, and its stepping;
  - destination-mask edge cases of family 18 (SOP);
  - the complement and alpha forms of families 19 and 25;
  - families 22-24 and 27, for which the reference prints nothing valid.
- **Only what libgxm's program functions report is read from GXP files.** That is the flags, parameters, vertex
  outputs, fragment inputs and output format, all checked against libgxm. Other tables (literals, buffer containers,
  dependent samplers) are not read yet; nothing checks their layout, and the translator will need them.
- **`sceGxmProgramIsEquivalent` is not modelled.**

## Vulkan backend

- **Clearing an attachment the shaders read.** On an Intel Arc (Windows driver 101.8801), a colour attachment that
  is also the pass's input attachment reads back wrong values where a draw wrote it, when the pass began with the
  CLEAR load operation (the validation layer reports nothing; NVIDIA is correct). Such passes are cleared with a
  transfer and loaded instead, so their images need `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.
- **No swapchain yet.** Presenting comes with the platform layer (SDL3).
- **Rasterization-order attachment access** is enabled where a driver has it (`VK_EXT_` or `VK_ARM_`); neither GPU
  tested here does, so draws that read the colour attachment are separated by barriers, which only holds between
  draws, not between overlapping triangles of one draw.

## Shader translator (USSE to SPIR-V)

- **Only the compute form exists.** It is the translator's test form: one instance per invocation, registers in
  storage buffers, textures and memory in the interpreter's test model. The vertex and fragment shaders GXM draws
  with come next, from the same instruction translation.
- **The GPU's precision, not the interpreter's.** `log2`, `exp2`, `inversesqrt` and division are the GPU's
  (Vulkan allows a few ULP), and f32 denormals may be flushed. Results agree within 2e-5 relative, except where an
  expression cancels the error out of proportion: 1 of 3864 random programs on the RTX 4060 (`exp2(x) - a` near
  zero, then `sqrt`).
- **Where the interpreter fails, the translation clamps or reads zero:** an indexed register outside its bank is
  clamped to the bank; a load outside the memory buffer or a sample of a missing texture gives zeros; a run of more
  than 1,000,000 blocks stops and is reported.
- **Not implemented** (as in the interpreter): the per-instance predicate, `rlp`, per-lane predicate tests, o8
  packs, texture queries, depth output, stores, and loads other than absolute ones.
- **64-bit integers.** Integer tests and 32-bit multiply-adds are computed in 64 bits, as the interpreter does;
  the module then needs `shaderInt64`, which some mobile GPUs lack. Programs without those instructions do not.

## Shader interpreter (USSE)

- **Textures are behind an interface** (`ShaderTextures`). The check uses nearest sampling of RGBA8 at texel centres;
  filtering, mipmaps, bias and level of detail are the texture model's, not the interpreter's. Texture queries (`.rsd`,
  `.info`, `.minp`) are not implemented.
- **Not implemented:** the per-instance predicate, `rlp`, per-lane predicate tests, o8 packs,
  loads and stores other than absolute ones, depth output. These fail with "not implemented". Derivatives and the
  special registers read as zero (one instance, no neighbours).
- **Fixed point is the least certain part.** fx10 is a signed 10-bit value with 1.0 = 255. x, y and z are in bits
  0-9, 10-19 and 20-29 of a register, w in the next one; this layout is read from the compiler's literals, and an fx10
  varying takes two registers. Products round to nearest (assumed). 7 of 600 random fixed-point programs disagree,
  unexplained.
- **Semantics marked "(assumed)" in `interp.cpp` are not covered by the same-source check.** These are the
  index-register destination numbering, the 16-bit integer multiply-add without a byte part, rounding in scaled
  (normalized) conversions, and unfused multiply-add rounding.
- **8 of 981 random vertex programs disagree, unexplained.** Most involve integers that go through the compiler's
  16-bit conversion shortcuts, or integer arithmetic on indexed array elements. It is not known whether the
  interpreter or the compiler is wrong there.
- **The SDK's shader compiler (psp2cgc) miscompiles some expressions.** The check avoids these patterns, or reports
  them separately, since they say nothing about the interpreter:
  - `abs(floor(x)) + c` becomes `c - floor(x)`;
  - `abs(x * y)` is sometimes moved onto one factor;
  - `frac` of constants folds as `x - trunc(x)`, while `frac` of variables compiles as `x - floor(x)`;
  - a uniform-only value used both by `clamp`/`min` and directly can lose its register in the secondary program (one
    case dropped a declared output). Such programs agree once their uniforms are made attributes, and the check
    reports them as "disagree only with uniforms".
  - Deep expressions crash the compiler (status 0xC0000409).
  - In fixed point, `x - y.w` (subtracting a scalar broadcast) compiles to `sub t, 0, y` (a negation) followed by
    `x - t`. That is only right if the first `sub` does not negate, yet `-y` compiles to the same instruction. The
    interpreter negates, as the instruction reads, and the check reports such programs separately ("subtract a
    negated value").
  - `lerp(x, constant, t)` with a constant t > 1 folds with the wrong sign in fixed point.
  - A half vector cast to int is converted in one lane, which is then read for every lane.
- **The compiler converts float to int by flooring** (`(int)-2.5` is -3). Cg says truncation; the check follows the
  compiler.

## Analysis and code generation

- **The console's default FPSCR is unknown.** The SDK material does not say what the floating-point control register
  holds when a thread starts. Nothing in crt0, libc's startup or a 24 MB SDK-built executable writes it. The runtime
  starts at 0 (IEEE: no flush-to-zero, no default NaN, round to nearest) until hardware can confirm it.
- **Floating-point exception flags are not modeled.** The cumulative flags in FPSCR (IOC, DZC and the others) stay
  clear. Advanced SIMD sets QC on saturation, as the architecture specifies.
- **Unsupported FPSCR modes trap.** A write that selects directed rounding, a nonzero vector LEN/STRIDE or
  alternative half precision stops with a diagnostic. No SDK-built code seen does this. The interpreter implements
  every rounding mode, so the generator can follow if a game needs them.
- **Exclusive loads and stores are a compare-and-swap.** STREX succeeds if memory still holds the value LDREX read.
  A store of the same value in between (ABA) therefore goes unnoticed, which real hardware would catch. Lock-free
  code built from LDREX/STREX does not depend on that difference.
- **Non-local control flow is not handled yet.** setjmp/longjmp and C++ exception unwinding change the stack and PC
  in ways the generated functions cannot follow. Those paths trap today. (A thread's own end, sceKernelExitThread
  from any depth, is handled: the runtime unwinds the host stack.)
- **Some jump tables are sized by inference.** When a TBB/TBH or computed branch has no bounds check that analysis
  can read, the table is taken to end at the first code after it (79 of 677 tables in a 24 MB executable). A wrong
  size would show up as a trap on an unknown target, not as a wrong branch.
- **A few system instructions are not translated.** In the 24 MB executable these are 3 `mcrr` and 3 `mrc p15`
  (coprocessor registers other than the thread ID registers). Each traps with a diagnostic.
- **Imported variables need the SDK's reference-table format.** Analysis reads each imported variable's table of uses
  (MOVW/MOVT pairs and data words). The format was worked out from SDK-built modules, and every one of them decodes.
  An unknown entry is an error, not a guess.
- **Probes need the SDK and run locally.** `scripts/probe_check.py` builds C programs with the SDK's compiler, so CI
  cannot run it. The generator-versus-interpreter test runs in CI without any SDK file.

## Runtime

- **Preemption takes effect at the preempted thread's next kernel call.** When a higher-priority thread becomes
  ready and takes the CPU of a running lower-priority one, the kernel cannot stop the host thread between two guest
  instructions. Until that thread's next system library call, both run, so one CPU is briefly shared by two threads.
  A thread that never calls the kernel (a pure computation loop) is never stopped. The console would stop it at
  once.
- **CPU assignment is simpler than the console's.** There are three CPUs, priorities and affinity masks, but not
  the console's split into per-CPU queues (priorities 64-127) and a shared queue (128-191). Every ready thread is in
  one priority-ordered queue that each CPU takes from, as far as its affinity mask allows. For the combinations the
  SDK recommends (one CPU for 64-127, all CPUs for 128-191), the result is the same.
- **Not implemented yet:** reader/writer locks, message pipes, timers, simple events, thread events (accepted, never
  called), mutex priority ceilings, `sceKernelGetModuleInfoByAddr`, `sceKernelLibcMktime`, unloading modules, and
  libfios2 overlays a game adds (the process has none: a path resolves to itself). A call to anything that is not
  implemented stops the program with the library and NID.
- **Not checked against hardware.** Several behaviours follow the SDK's documentation where it is clear and the SDK
  libraries' own code where it is not. Two examples:
  - `sceKernelCallAbortHandler`, which libc calls with an event code at exit and elsewhere and then carries on, is
    implemented as a note.
  - The main thread gets one argument, the main module's path.

  None of this has been compared with a console.
- **Unresolved variable imports have address 0.** As on the console for loosely linked imports, an access then
  faults (and is reported). libc imports one (`SceLibcParam`) that nothing provides; it checks the address first.
- **The console's stdout and stderr are one stream.** libc writes both to the same tty, and the runtime sends that
  tty to the host's standard output.
- **Guest threads that never end keep the process alive.** At shutdown, threads blocked in the kernel end, but one
  running guest code without kernel calls cannot be stopped. Its host thread is left to the process exit, and the
  kernel's state stays allocated for it.
- **Timing on the development machine.** Starting an OS thread there takes 30-100 ms, and while one starts, other
  threads' timed wake-ups are late by about 10-15 ms. Guest threads start on pooled host threads, so this only
  happens when the pool grows. Elsewhere, timed waits (delays, timeouts) end within a millisecond of their deadline.
- **Memory budgets are fixed defaults.** Main memory 256 MiB and CDRAM 128 MiB for memory blocks, set in
  `Runtime::Config`. The console's figures for a game are not reproduced exactly.
- **Run on Windows only.** The runtime's POSIX host layer (faults, files, decommit) compiles for Android arm64 with
  the NDK, without warnings, but has not run there or on Linux (no device test runner, no Linux machine; CI covers
  Linux once the repository is pushed).

## NID databases

- **NID names from different sources can disagree on aliases.** For example, vita-headers names a NID
  `sceClibMemcpy_safe` where the SDK's stub calls it `sceClibMemcpy`. When several databases are loaded, the last
  one wins.
