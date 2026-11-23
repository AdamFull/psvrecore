# Known issues and open questions

## Build and platforms

- **Linux is built and tested only in CI** (Ubuntu 24.04, clang; the GPU tests on Mesa's lavapipe). The development
  host has no Linux machine, so the frontend has not been run with a real window, sound or a game there.
- **Android: one device so far.** SDK samples run as applications on a Pixel 10 Pro (Android 17); no other device,
  GPU or Android version has been tried. The unit tests have not run on a device (they are host-only; there is no
  device test runner).

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
- **Reading the colour attachment in dynamic rendering.** On an NVIDIA RTX 4060 (no rasterization-order access), a
  draw that read the colour attachment as an input attachment in a dynamic rendering pass (`VK_KHR_dynamic_rendering_local_read`)
  saw the colour from before the pass, not the earlier draws' writes, with the attachment and the input attachment in
  GENERAL and a by-region barrier between the draws (the validation layer reports nothing; render passes were correct).
  Killzone's depth-of-field pass read its own near masks as stale and blurred the whole level. Such passes keep the
  colour in `RENDERING_LOCAL_READ`, which reads the writes. `PSVRECORE_LOCAL_READ=0` uses render passes instead.
- **No swapchain yet.** Presenting comes with the platform layer (SDL3).
- **Rasterization-order attachment access** is enabled where a driver has it (`VK_EXT_` or `VK_ARM_`); neither GPU
  tested here does, so draws that read the colour attachment are separated by barriers, which only holds between
  draws, not between overlapping triangles of one draw (whose fragments then lose each other's writes: a light cone
  of layers drawn in one draw came out blotchy). Programs whose only read of the colour is a final blend of it with
  another register (sop2 or blend, factors Vulkan has: every such program P4D draws) leave that blend to the
  pipeline's fixed-function blending, which is ordered, and read no attachment; other colour reads still race.

## Shader translator (USSE to SPIR-V)

- **Four forms.** Compute is the test form (one instance per invocation, registers in storage buffers, textures and
  memory in the interpreter's test model). Secondary programs run as a compute dispatch per draw, except a fragment
  program's secondary that reads the per-tile special registers, which runs at the start of the fragment shader.
  Vertex and fragment forms are what the GXM layer draws with.
- **The GPU's precision, not the interpreter's.** `log2`, `exp2`, `inversesqrt` and division are the GPU's
  (Vulkan allows a few ULP), and f32 denormals may be flushed. Results agree within 2e-5 relative, except where an
  expression cancels the error out of proportion: 1 of 3864 random programs on the RTX 4060 (`exp2(x) - a` near
  zero, then `sqrt`).
- **Where the interpreter fails, the translation clamps or reads zero:** an indexed register outside its bank is
  clamped to the bank; a load outside the memory buffer or a sample of a missing texture gives zeros; a run of more
  than 1,000,000 blocks stops and is reported.
- **Not implemented** (as in the interpreter): the per-instance predicate, `rlp`, per-lane predicate tests, o8
  packs, loads and stores other than absolute ones, texture queries other than `.irsd` of a 2D texture
  with an f32 result (and that one has no oracle: see below).
- **Depth output (`depthf`) is the draw's fragment form's only**: the register's f32, clamped to 0-1, replaces the
  fragment's depth (FragDepth, DepthReplacing); a predicated one that does not run leaves the fragment's own depth.
  The compute form, the one compared with the interpreter, has no depth: it ignores `depthf`, which the interpreter
  refuses, so this has no oracle.
- **Stores** of 1 or 2 bytes read, modify and write their word, not atomically: instances storing bytes of one word
  can race. Stores of 4 bytes are assumed aligned. A store outside the memory buffer is dropped.
- **Implicit levels of detail** need derivatives, which fragment shaders have only outside non-uniform control
  flow: a program with branches samples at level 0.
- **64-bit integers.** Integer tests and 32-bit multiply-adds are computed in 64 bits, as the interpreter does;
  the module then needs `shaderInt64`, which some mobile GPUs lack. Programs without those instructions do not.

## GXM layer (HLE)

- **A program and state seen for the first time is still translated (and its pipeline compiled) when it is first
  drawn.** The shader cache makes everything an earlier run drew with before the game starts, but nothing can be
  made ahead of that: the programs are in the user's game files (not there when the program is built, and never in
  the repository), and what a translation depends on (the vertex formats, the output format, blending, depth) is
  known only when the game draws. The first run of each scene pays once, on each device. The cache's SPIR-V would do
  for another device (the driver's pipeline cache would not, and is dropped there), but copying it has not been
  tried.
- **The shader cache only grows.** Nothing is evicted; a translator whose sources changed starts it again (its
  header holds a hash of them, made when CMake runs: it runs again when they change).
- **The internal resolution (`Options::render_scale`, `[display] internal_resolution`)** scales colour surfaces,
  and depth-only scenes whose depth is stored (shadow maps) when the surface is DF32 or DF32M. A texture reads the
  higher-resolution image only when it is that surface as it was drawn: same address and size, one level, its texels
  what the image holds (8-bit RGBA colour; one float of depth), not the scene's own target, and guest memory
  unchanged since. Otherwise (mip levels, other formats, a surface the CPU changed, depth with stencil) it reads what
  guest memory holds, at the surface's size. The CPU's drawing over a surface (debug text, a cleared area) reaches
  the scaled picture as the surface's pixels, blown up. Lines are not widened (points are). Surfaces of formats the device
  cannot blit with filtering stay at their own size, as do frames that are not 4 bytes a pixel.
- **A frame drawn at a higher resolution is read back to the CPU to be shown** (about 8 MB at 2x), then uploaded by
  the window: a copy between the two GPU images would do without it.
- **A scene waits on the game's thread** for its fragment sync object while the display holds it (the console's GPU
  waits instead, and the CPU runs on). A game that draws into a buffer the display queue holds, and only then queues
  the entry that lets it go, would wait for ever; GXM's display buffering does not do that.
- **Scenes render synchronously** when they end (one submission and wait each): simple and ordered with transfers and
  the CPU, but slow (multithreaded_rendering: about 40 ms a frame).
- **Not implemented:** multisampling, the DF32M_S8
  mask bit (DF32M's is the depth image's stencil aspect), YUV transfers, the VDM buffer and region clip checks of
  command lists.
- **Uniform data is copied when a draw is made** (the hardware reads it when the scene runs). A game that writes a
  buffer between the draw and the end of the scene would see a difference; that is a race on the hardware too.
- **Assumed, unverified:** the cull winding (CW culls Vulkan's back faces), the background mask set by default,
  F32M textures without their mask bit, the level-of-detail bias in eighths, the transfer layouts of tiled surfaces
  (as tiled textures), tiled and swizzled colour surfaces laid out as textures of those types are (a tiled surface's
  tiles in rows of its stride; a swizzled one in Morton order over its power-of-two size), the downscale's rounding.
- **Visibility testing** uses the front face's settings for both faces, writes every count to core 0's buffer (the
  game sums the cores'), and counts samples exactly only where the device has `occlusionQueryPrecise`.
- **Uniform buffers** are copied once per scene and shared by the draws that use them; buffers at different
  addresses that overlap get separate copies.
- **The mask update program** has no `SceGxmProgram`: `sceGxmFragmentProgramGetProgram` returns 0 for it.
- **Texture formats not done:** X8S8S8U8, U32, S32, U32U32, SE5M9M9M9, X8U24 in a swizzle other than its first. The mirror-clamp addressing modes clamp.
- **YUV textures are converted to RGBA on the CPU** when they are read, with the colour space profile set then. A
  profile changed later (`sceGxmSetYuvProfile`) does not reach textures already converted and cached. Only their top
  level is read.

## Shader interpreter (USSE)

- **Textures are behind an interface** (`ShaderTextures`). The check uses nearest sampling of RGBA8 at texel centres;
  filtering, mipmaps, bias and level of detail are the texture model's, not the interpreter's. Texture queries (`.rsd`,
  `.info`, `.minp`) are not implemented.
- **Not implemented:** the per-instance predicate, `rlp`, per-lane predicate tests, o8 packs,
  loads and stores other than absolute ones, depth output. These fail with "not implemented". Derivatives read as zero
  (one instance, no neighbours); special registers read what `Machine::specials` holds (zero by default).
- **Fixed point is the least certain part.** fx10 is a signed 10-bit value with 1.0 = 255. x, y and z are in bits
  0-9, 10-19 and 20-29 of a register, w in the next one; this layout is read from the compiler's literals, and an fx10
  varying takes two registers. Products round to nearest (assumed). 7 of 600 random fixed-point programs disagree,
  unexplained.
- **Semantics marked "(assumed)" in `interp.cpp` are not covered by the same-source check.** These are the
  index-register destination numbering, the 16-bit integer multiply-add without a byte part, and unfused
  multiply-add rounding.
- **Inferred from compiled programs, without an oracle** (each consistent with every compiled program examined, but
  not measured on hardware):
  - Loads and stores: an immediate offset counts elements, a register offset's low half is bytes (its high half, 1 in
    every program, does not move the address), a burst (`.fetchN`) starts one element further. The element size of
    16- and 64-bit bursts is assumed.
  - A float test with a register destination writes all ones or zero for a single lane (every one in the corpora
    feeds a `cmov8`), 1.0 or 0.0 per lane for a four-lane destination.
  - Scaled conversions to integers truncate.
  - GXP header flags 17 and 18 mark the vertex and instance indices, varyings word 7 their registers.
  - `.irsd`: four texels (f32) then four f16 weights, texel k's at index 3 - k.
  - `pixelX`, `pixelY` and `tileXY` describe 16 x 16 tiles.
- **auxiliary_surface's export programs** write a pixel index computed with `mad.i16` into an internal register, then
  read the whole word with `mad.u32`. With 16-bit lanes as halves of a word (what a unit test, checked on compiled
  code, establishes for a lane-x write), lane y lands in the high half and every tile row but the first is lost; the
  hardware must lay out or read these lanes differently. Not resolved.
- **2 of 293 random vertex programs (seed 11) disagree with the host**, interpreter and GPU agreeing: loops over
  float-to-int conversions of negative values (an off-by-one through the compiler's 16-bit conversion sequence).
  Unexplained.
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
- **A table of addresses reached through a register is recognized only right after the instruction that makes its
  address.** `adr rB, table` (or ARM's `add`/`sub rB, pc, #K`) must be the instruction just before `ldr pc, [rB, rX,
  lsl #2]`; with anything between them the branch stays an indirect jump (which stops with "branch to an address no
  recompiled function starts at" if reached). The generated code checks that rB holds the table's address before it
  switches on the index; another path to the branch with another value in rB goes where the word it loads says if that
  is one of the table's targets, and traps otherwise, as any table branch does for an index outside its table.
- **The condition flags are guaranteed only where the contract says** (ARCHITECTURE.md, "Condition flags"). What
  is assumed and not checked:
  - a return-form branch (bx lr, pop/ldm/ldr pc from the stack) to an address other than the function's return
    address is a non-local exit: the code it reaches gets only the function's `live_out` flags at entry and is not
    relied on for any flag at its return;
  - an exported function leaves all four flags at return except those that come out of an indirect call or jump it
    ends in (those are its module's `given_at_return`): an export that hands a flag result on from a function
    pointer to another module's caller would lose it;
  - a function replaced with `override_function` must itself leave the flags its module gives at return;
  - at a trap the context holds the flags the trapping function computed since its entry or last call, not those of
    its callers.
  Across modules the contract is checked: the loader refuses modules that do not fit (`psvrecomp
  --flags-given-at-entry`/`--flags-given-at-return` fix it). `psvrecomp --flags-everywhere` generates without flag
  liveness, for checking a suspected liveness bug against a game.
- **The core registers are in the context only where the contract says** (ARCHITECTURE.md, "Registers"). What is
  assumed and not checked:
  - a word a function wrote at a known offset of its stack frame (the registers it pushes, words it stores through
    sp or a frame pointer) is changed only by its own later stores there: not by a function it calls (one writing
    to its incoming stack arguments may, but no compiler restores a register from those), not by a store through a
    pointer the analysis does not know to be a stack address (an array overrun into the saved registers), not by
    another thread. A function that restores a register from such a word is taken to preserve it, and its direct
    callers keep their copy of the register instead of reading the context. The generated code built with
    `PSV_CHECK_KEPT_REGISTERS` checks this after every call where it is relied on (the masked corpus tests are),
    a game's build does not; `psvrecomp --no-callee-saved` generates without the reliance;
  - what a function preserves is known only for entries where LR holds its return address (direct calls); entered
    otherwise (a tail call that changed LR, `psv_jump`), nothing relies on it;
  - host code that reads `t->ctx` while generated code runs, without a call in between (the guest memory fault
    report, the memory write watch), sees the registers as of the function's last call, exit or trap;
  - the callee-saved proof comes from the code alone; the exception index table only cross-checks it (where its
    entries describe a frame);
  - with `psvrecomp --no-sp-check`, code outside the module's view (imports, function pointers) is taken to return
    with the stack pointer it was called with, unchecked;
  Checked: what each instruction's translation reads and writes against the analysis' model (psvrecomp lists every
  difference, corpusgen fails on one; none in WipEout 2048's or P4D's modules); the stack pointer after a call to
  code outside the module's view, where a function's callers rely on it (a trap, `PSV_TRAP_STACK_POINTER`: a game
  with an import or a function pointer that returns with another sp, legal only outside AAPCS, needs
  `--no-callee-saved`). `psvrecomp --registers-in-context` generates the code from before (every register in the
  context), for checking a suspected register analysis bug.
- **Internal entries, inlining and the indirect-call tables (task 7): what is assumed and not checked**
  (ARCHITECTURE.md, "Internal entries", "Inlining", "Indirect calls"):
  - an internal entry's returns go back to its caller without comparing the saved LR they read with the return
    address: that they are the same rests on the callee-saved proof's assumption above (the frame's saved words are
    changed only by the function's own stores). Before, a return whose saved LR had been overwritten (an array overrun
    into the frame) went through the runtime to that address, which usually stopped the program (no function there);
    now it returns to its caller as if not overwritten. Nothing checks it, not even `PSV_CHECK_KEPT_REGISTERS`;
  - a function entered through `f_<address>` with LR other than `ret` jumps through the runtime to LR after its
    internal entry returns, which is what its returns did before; the diagnostic of a jump to an unknown address
    names the function's address now, not the return instruction's;
  - after a call of an `Inside` internal entry, `PSV_CHECK_KEPT_REGISTERS` checks only r4-r12 (the context's r0-r3,
    sp and lr are the caller's business there);
  - host code that reads `t->ctx` while generated code runs (the fault report, the write watch, a watch of a host
    function by name) sees r0-r3, sp and lr as of the last time any frame on the host stack wrote them: an internal
    call writes none of them;
  - a game that stands in for a generated function by renaming its definition in the generated C (`HOOKS` in Gravity
    Rush's and LittleBigPlanet's `scripts/generate_code.py`) gets direct calls around its stand-in unless the function
    has no internal entry: psvrecomp writes such a function's `f_<address>` in another form, so the script finds
    nothing and stops, and `psvrecomp --no-inline <address>` restores the form. Checked with this generator: Gravity
    Rush's two hooked functions (0x81048988, 0x81155A40) get internal entries, so its script stops until it passes
    `--no-inline` for them; LittleBigPlanet's (0x8101F266) has none (it may return elsewhere than its LR), so its
    script works as before. Neither game's script was changed (other sessions' repositories);
  - a module registered while guest code runs (one the game loads itself) publishes its indirect-call table before
    its code runs; the registry's map (`find_function`, the slow path) is still not locked against that, as before;
  - the indirect-call tables cost memory: two bytes per halfword of a module's Thumb code span (four with 65535
    functions or more) and eight per Thumb function: 4.4 MiB for WipEout 2048's four modules, 11.8 MiB for
    Minecraft's, 8.4 MiB for Call of Duty's;
  - there is no per-site inline cache for indirect calls: whether one would pay (Minecraft's 86.6K virtual call
    sites) is unmeasured, as no such game runs here. The table lookup is one function per module (`<name>_call`), so
    all of a module's indirect calls share one host indirect branch; on the corpus that is as fast as a lookup at each
    site (`--call-table-inline`), on a game with many polymorphic call sites it is not measured.
  Checked: every function entered through `f_<address>` leaves the context exact (the corpus tests compare r0-r14 at
  every exit); the whole context at calls through `psv_call` at the bottom of chains of internal calls
  (`obs_*`); the tables against the registry (`codegen_tests.cpp`: Thumb and ARM targets, other modules, import
  stubs, addresses no function starts at, overrides, an emptied registry). `psvrecomp --no-internal-calls
  --no-call-table` gives the code from before byte for byte (WipEout 2048's four modules, P4D's six).
- **Host implementations of guest functions (task 8): what is assumed and not checked.**
  - a host function leaves r1-r3, r12, lr, the flags and d0-d7/d16-d31 as it likes (the AAPCS's); a caller reading
    one after the call gets something else than from the guest's code. Calls inside the replaced module are scanned
    for that (listed by psvrecomp); calls from other modules through an import are not;
  - callers keep r4-r11 and sp in their locals across a host function's call on the contract's word
    (`kHostPreserved`; the same `PSV_KEPT` check as any call whose callee is shown to keep them), not on a proof
    from the host function's code; with a guard, only what the guest code is also shown to keep;
  - the comparison with the module's own code (`PSVRECORE_TEST_LIBC`) runs only where the module is at hand, not
    in CI (ARCHITECTURE.md, "Host implementations of guest functions");
  - the table's lines hold for one build of a module (its fingerprint); WipEout 2048's libc (another memcpy, which
    returns through r12 and copies with vext) and the other games' were not checked and get none. A game opts in by
    passing `--replace-table` to psvrecomp; no game's `generate_code.py` does yet;
  - a host function reads memory through `PSV_HOST` with no fault of its own: where the guest's code would fault at
    an unmapped address the host function faults the same way (the memory is the same), but strlen and memcmp read
    in 8-byte aligned units that stay in the page of the last byte the guest's code reads;
  - the atomics are host `__atomic` read-modify-writes on the guest's memory; guest ldrex/strex is a compare-and-swap
    (above), so a host atomic and another thread's ldrex/strex loop interleave as two guest threads' loops would
    (and share the compare-and-swap's ABA blind spot);
  - WipEout 2048's libfios2 inflate (`f_8311A9C8`, called by its zlib wrapper 0x8311A94C) is not replaced: it is a
    hand-tuned raw deflate decoder, `int inflate(u8* dst, u32 capacity, const u8* src, const u8** end)` (r3 may be 0)
    returning the bytes written or 0x80560100 (bad data)/0x80560101 (output full), and it is not byte-equivalent to a
    host inflate: a match at distance 4 or more is copied in words and may write up to 3 bytes past its end (inside
    the capacity, later overwritten, but left after the last one), output written before an error stays, its table
    building and limits (HLIT/HDIST at most 29, code-length repeats with no previous length) are its own, and `*end`
    is the byte after the last bit read. A replacement would be a port of this decoder checked by fuzzing, not zlib
    (CODEGEN_METRICS.md, task 8).
- **Floating-point registers in locals (task 5): what is assumed and not checked.**
  - The host's floating-point environment on x64 (and arm64 without native floating point) is the default one while
    generated code runs: round to nearest, no flush-to-zero or denormals-are-zero (MXCSR's FTZ/DAZ clear). The typed
    operations' fast paths take the host's IEEE result where it equals the VFP's under that environment (as the
    binary32 fast paths before them did); host code that changed MXCSR on a guest thread would break them. Nothing in
    the runtime does; a driver or library that did would not be noticed.
  - A vcvtr on x64 rounds by MXCSR's mode (to nearest, as above), the exact fallback by `rint` (the same).
  - Advanced SIMD instructions in the context form (`--neon-in-context`, or one without a translation on values: none in
    WipEout 2048, P4D or the corpora) are taken to read and write only the D registers their operands name (register
    lists: up to twice their count, four at least) and the FPSCR's bits 27-0 (QC, AHP), not its NZCV: guest_simd.h's
    helpers do so today. A helper that touched another register would see a stale context register or have its write
    lost.
  - Summaries take a callee to read and write only the registers its own instructions name, its callees' and, through
    anything outside the module's view, everything. Code the module does not know that reaches a function's callers'
    frames another way than through a call or a return to the LR it was given (a longjmp into the middle of a
    caller, an exception unwound by guest code through a frame without a call) would find stale locals; the core
    registers assume the same.
  - The FPSCR's NZCV (what vcmp writes) is in the context at every call and exit where the function may have
    changed it, so a loop that compares and branches still computes it each time round when the loop ends in a return
    (x64: a few instructions; the branch itself is fused). AAPCS leaves it undefined at calls and returns; using that
    would need a whole-program liveness like the condition flags' (not done).
  - Operations whose result is a NaN (and, in flush-to-zero mode, whose operands or result are denormal or at the
    smallest normal) call an out-of-line exact helper; in a loop that call costs the spills of the live
    floating-point locals. Code that keeps computing on NaNs is slower than before: in the corpus' timing run the
    functions whose random operands include NaNs and infinities (`dot3_f32`, `length_f32` over random memory words,
    whose sums become NaN early; `arith_all`) take 1.5-2 times as long, while the other float functions take from 5%
    more to 58% less (27-30% less in the geometric mean). Games are not expected to run hot loops on NaNs; not measured
    in one.
  Checked: the differential corpus (`vfp_locals.s`: aliasing, calls, NaN payloads, vmsr, fused compares, Advanced
  SIMD next to VFP) in all three configurations, and the typed operations against the interpreter
  (`vfp_typed_tests.cpp`). `psvrecomp --vfp-in-context` generates the code from before, `--vfp-calls-conservative`
  without the summaries.
- **Native floating point on arm64 (`PSVRECORE_GENERATED_CODE_NATIVE_FLOAT`) is not verified on a device.** It
  compiles for android-arm64-release and its disassembly was inspected (one host instruction per guest operation), but
  the device tests that compare it with the interpreter (`tests/device`, [BUILDING.md](BUILDING.md#device-tests)) have
  not been run: the option stays off by default. Assumed and unchecked until then:
  - AArch64's FPCR.FZ flushes denormal inputs and results with tininess detected before rounding, and its NaN
    propagation and default NaN (FPCR.DN) are ARMv7 VFP's (the ARM ARM's FPRound and FPProcessNaNs say so; cores with
    FEAT_AFP keep it while FPCR.AH is 0, which the runtime never sets);
  - FCVT between single and double precision honours FZ and DN as VFP's vcvt.f64.f32 / vcvt.f32.f64 do;
  - the C comparisons the generated code branches on compile to FCMP (which flushes by FPCR.FZ) with
    `-fdenormal-fp-math=dynamic`, and nothing folds them under another assumption;
  - the FPCR is per host thread and kept by the OS across preemption; nx::fiber's context switches leave it alone (the
    fiber backend carries each guest fiber's FPCR itself);
  - HLE code outside the `host_float` libraries (hle::Library) is unaffected by FZ/DN in what it computes. Libraries
    not marked and possibly affected: none known to do floating-point arithmetic (SceCtrl, SceAppUtil, SceNet...);
    marked as a precaution, perhaps needlessly: SceLibKernel (only printf's `%f` formats doubles), SceGxm and
    SceDisplay (drivers' own arithmetic). Host threads of their own (the audio device's, the renderer's) never run in
    guest mode.
  - Only where the guest sets FZ or DN does any of this differ from the host's default (the FPSCR starts at 0, so
    the mirror is the default FPCR until a vmsr); WipEout's libc has two vmsr instructions, not seen executed.
- **Advanced SIMD on values (task 6): what is assumed and not checked.**
  - Round to nearest in the host's floating-point environment while generated code runs (MXCSR's rounding on x64,
    FPCR.RMode on arm64), as for the VFP operations. The checked single-precision code is exact under any FZ/DN (or
    FTZ/DAZ) setting otherwise; nothing else of the environment is assumed.
  - arm64's FRECPE, FRSQRTE, URECPE and URSQRTE give ARMv7's 8-bit estimates (ARMv8's pseudocode defines them to; the
    device tests compare every exponent and table index with the interpreter, not run yet), and FCVTZS/FCVTZU and
    SCVTF/UCVTF are ARM's conversions. x64 uses no instruction later than SSE2 unless the build's
    `PSVRECORE_GENERATED_CODE_ARCH` enables it (PSHUFB for VTBL with SSSE3).
  - With native floating point (arm64, off by default): when the guest's FPSCR is in the standard mode the NEON
    floating-point operations are the host's instructions, relying on the FPCR mirror (task 5) having FZ and DN then:
    AArch64's FMUL, FADD, FSUB, FABD, FMAX, FMIN, the pairwise forms, the compares and the estimates are ARMv7 NEON's in
    that mode (flush-to-zero with tininess before rounding, the default NaN). Not run on a device.
  - Element single-copy atomicity of VLD/VST: each register (or pair of registers) is one host access of 8 or 16
    bytes, so an element is atomic where that access keeps it (an aligned 8-byte part on x64 and arm64; a 16-byte
    access aligned to 16 bytes on x64 CPUs with AVX). An element the guest made aligned but that straddles an 8-byte
    boundary of an unaligned 16-byte host access is not guaranteed atomic. Before, an aligned 8-byte register was
    atomic and an unaligned one not either. Two guest threads racing on NEON loads and stores of one element are not
    known in games.
  - The known-no-denormal analysis (which skips flushing operands) is local to straight stretches of code and
    forgets at labels, calls, conditional instructions and writes by anything but the listed operations; a wrong
    entry would make an operand's denormal lane count as itself. The differential corpora run chains of NEON
    floating-point instructions from random states with denormal lanes; a mutation that marks an integer result
    "known" is caught (task 6's mutation checks, CODEGEN_METRICS.md).
  - The Q view is chosen by a count of the function's whole-register and part accesses; any choice is correct, only
    the code's size differs. Functions that mix VFP scalar arithmetic on lanes of a Q-held register pay a lane extract
    or insert for each.
  - Static code size: each single-precision multiply carries its rare branch for a product at 2^-126 (inline, about
    15 instructions on x64 and 8 on arm64, never taken by ordinary values), which the static metric counts like the
    fast path.
  - Slower than before on x64 with plain SSE2: the signed 32 x 32 -> 64 multiplies (VMULL/VMLAL/VMLSL.S32,
    VQDMULL.S32 and its accumulating forms) are vector code that SSE2 has no instruction for (PMULDQ is SSE4.1), so
    clang builds each from three PMULUDQ and sign corrections; the corpus' `mac64` loop (one vmlal.s32 per two
    elements) takes 36 ns instead of 31. With `PSVRECORE_GENERATED_CODE_ARCH` at x86-64-v2 or later it is PMULDQ.
  Checked: fuzzgen's Advanced SIMD corpus (8000 blocks of one to three NEON instructions, 12 states each with lanes at
  their edges, against the interpreter), the helpers against the interpreter on every pair of 46 single-precision edge
  values and every estimate table index (`simd_tests.cpp`), the corpus' `neon_locals.s` (aliasing, calls, every
  load/store form, the end of mapped memory, game loops) in all three configurations.
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
- **Timing on the development machine.** Starting an OS thread there took 30-100 ms in October 2026, with other
  threads' timed wake-ups 10-15 ms late meanwhile, so guest threads ran on pooled host threads until 2026-11-18.
  Now a start takes 0.1-0.9 ms (a median of 0.34 ms in P4D) and costs no frame time, and each start makes an OS
  thread of its own ([THREAD_BACKENDS.md](THREAD_BACKENDS.md)). Timed waits (delays, timeouts) end within a
  millisecond of their deadline.
- **Fiber backend (`PSVRECORE_GUEST_FIBERS`, off by default) is an experiment.** It runs the kernel tests, the SDK
  test programs and P4D ([THREAD_BACKENDS.md](THREAD_BACKENDS.md)); it has not run WipEout 2048 or DIVA F 2nd
  through a race or a song, nor run on a phone. A thread that loops without kernel calls holds its host worker. While
  a preempted thread runs on to its next kernel call, its CPU's new holder runs on a helper thread, so more than three
  host threads may run guest code then, as with native threads. Debuggers show the workers and helpers:
  `PSVRECORE_THREADS` lists where each guest thread is. Deterministic scheduling (`PSVRECORE_DETERMINISTIC=1`) is for
  tests: one host thread runs every guest thread, so a thread that spins until another goes on never ends, and host
  threads (the display's, audio) do not follow its clock.
- **libfios2's schedulers are woken every 100 ms.** The SDK's file library loses a wakeup of its I/O schedulers when
  a PSARC chunk's decompression ends while the scheduler that started it has not put it back in its queue yet: the
  chunk is ready but counted among the busy ones, and every reader of the game's files waits for good (WipEout 2048
  froze in its menus on an AYN Odin 3, often within a few minutes; the analysis is in the kernel's
  `wait_cond`). A wait on a condition named `SceFiosSchedulerWakeup` with no timeout therefore ends after 100 ms as
  if signalled, and the scheduler looks at its queue again. It works around the library, recognised by that name:
  another library with the same flaw would need its own.
- **Memory budgets are fixed defaults.** Main memory 256 MiB and CDRAM 128 MiB for memory blocks, set in
  `Runtime::Config`. The console's figures for a game are not reproduced exactly.
- **Run on Windows and Android.** The POSIX host layer (faults, files, decommit) runs on Android arm64 (the SDK
  samples on a Pixel 10 Pro: the whole guest address space reserved, guest threads, files); on Linux it runs the unit
  and runtime tests in CI. A guest memory fault has not been seen on Android.

## Audio (AudioOut, NGS)

- **The AudioOut timing tests need an idle host.** They check that each block starts exactly a grain after the one
  before and that calls return within 4 ms. With the host at ~90% CPU from other builds, the test thread wakes late,
  so the port's timeline restarts after a gap and the checks fail. On an idle host they pass.
- **HE-VAG's predictors are measured, not documented.** The 128 filters of HE-VAG (a 7-bit filter index, the SDK's
  vagconv2 default "Standard" mode) are not in the documentation: they were measured against vagconv2, as the
  coefficients that make its output decode closest to its input. The 2-tap filters (5-28) land exactly on a grid of
  1/8192ths. The 4-tap ones (29-127) are the grid points nearest the fit and may be a step off, which changes nothing
  measurable: the test files' SNR is the same to 0.01 dB with the unrounded fit.
- **VAG prediction is not rounded.** vagconv2 encodes for a decoder that keeps the fraction of the prediction (the
  PlayStation ADPCM decoders round it to an integer, which drifts a few steps from what vagconv2 meant). Whether the
  console rounds is unknown.
- **NGS effects are close, not exact.** Filters, the parametric EQ, envelope curves, the compressor, delay, pitch shift
  and the I3DL2 reverb follow their documented behaviour with the usual designs; the console's arithmetic is unknown.
  The reverb presets are the I3DL2 environment presets. The player's resampling between rates is linear.
- **Some NGS defaults are assumptions.** Which modules a template voice starts with bypassed (the player, and in
  template 1 the mixer, are active; the other effects of source voices bypassed; buss effects active), the modules'
  default parameters, and the default patch volumes (each channel to the same channel, mono to and from both).
- **A loop starts VAG decoding again; a buffer swap does not.** A swap goes on from the samples before, so a stream
  split over buffers (as the SDK's adpcm_stream sample streams) decodes as one. Whether the console also clears the
  predictor's history when a buffer loops is unknown (vagconv2 did not make a file that shows it).
- **A voice cued to play reports itself active and pending** until the next update starts it (as we read the
  documentation, active covers a voice waiting to start as well as one playing). CRI's sound engine relies on it: a
  voice it finds neither active nor finalizing it takes for stopped, and its streams stall.
- **Scream's voice definitions are stood in for** by template 1 and the ATRAC9 voice: their layout is not documented.
- **Callbacks run on the thread that calls `sceNgsSystemUpdate`**, after it processed every voice, as documented.
  Parameter error callbacks are never raised: out-of-range parameters are refused when they are set.

## Video (AvPlayer)

- **Decoding happens in the calls that ask for frames**, on the caller's thread, not on player threads of its own.
  `sceAvPlayerGetVideoData` decodes as far as the clock needs (at most 16 samples a call).
- **Events are delivered from the call that causes them** (`sceAvPlayerAddSource`, `Start`, `Pause`, `Resume`,
  `Stop`, or the frame calls at the end), not from a player thread.
- **Not modelled:** trick play (only normal speed), timed text streams (listed, never delivered), more than one
  video or audio stream played at once, and encrypted content.
- **openh264 is built without its assembly.** Decoding a 960x544 picture takes 2-4 ms on the development machine.
- **Pictures are rounded up to 16 pixels** with the picture's last column and row repeated; the crop offsets of
  `sceAvPlayerGetVideoDataEx` say what is the picture.

## Input

- **Input comes from the platform layer** (SDL3); its limits are under Platform layer below.
- **Wireless controllers (ports 1-4) read as idle; touch force reads as 0.**

## Platform layer (SDL3)

- **In the background the game is suspended** (Kernel::suspend): its threads stop at their next kernel call and its
  clock stands still. A thread in a loop that makes no kernel call runs on. Host work already started (a GXM scene
  being rendered, a file read) finishes first. The application is not told it was suspended, as on the console. If
  Android ends the process meanwhile, what the game had not saved is lost.
- **On desktops nothing is suspended** when the window is minimized: the game runs on (and plays) unseen.
- **Android: the swapchain is not pre-rotated.** The compositor rotates, and `VK_SUBOPTIMAL_KHR` is ignored for that
  reason. The window is full screen and landscape only.
- **One Vulkan device a process.** volk's entry points are loaded for the last device made, so a second device breaks
  the first one's calls (it crashed). With a window, the GXM layer renders with the window's device
  (`gxm::Options::shared_device`).
- **Frames are copied:** from guest memory on the game's thread (about 2 MB at 960x544), then uploaded and drawn on
  the window's thread. Scaling is bilinear (nearest at 1:1); there is no integer scaling. The guest's bytes reach the
  display as they are (a UNORM swapchain), as on the console.
- **A frame set for the next vertical blank is shown when the game next waits for a blank or sets a frame** after that
  blank. A game that sets a frame and then does neither does not get it shown.
- **Audio:** the device runs at 48 kHz stereo and SDL resamples ports at other rates. A port more than 200 ms behind
  drops what it holds (logged).
- **Controls are fixed:** no remapping, no rear touch panel (the mouse and fingers touch the front one), no motion
  sensors, no rumble. The on-screen controls have one layout (size and opacity are settings): no L3/R3, no moving
  them, no haptic feedback; a finger on a face button presses one button, not two at once. Keyboard: arrows, Z
  Cross, X Circle, A Square, S Triangle, Q L, W R, Enter Start, Right Shift Select; F11 toggles full screen.
- **On-screen controls and gamepads coming and going** (auto mode) are unit-tested with SDL's events, not yet with a
  real controller connected to the phone and taken away.
- **Memory block alignment:** an alignment smaller than the type's own is raised to it (the SDK's samples ask CDRAM
  for 256 bytes and run on the console); one that is not a power of two is refused. Inferred from the samples, not
  measured.

## Frontend

- **The process ends with `_Exit`** when the window is closed or the game exits: guest threads cannot be stopped and
  joined, so nothing is torn down (the window, the audio device, the log are flushed first). A game's own save data
  is written by the time its calls return, so nothing is lost that the game wrote.
- **Two settings screens.** Android's own (`SettingsActivity`, from Back or the on-screen menu button) and, on every
  platform, the Home menu's (Guide on a gamepad; Escape and the on-screen menu button on a desktop). The Home menu
  shows the text and folder settings but does not edit them (the file or the command line does). F11's full screen
  lasts for the run.
- **Some settings apply only at the next start** (marked so on the screen while playing): the display's vsync, the
  sound's latency, the console's settings (the game reads them once), the log file. Sound switched on while playing
  plays only if it was on when the game started (the device is opened then).
- **Android's Back opens the settings** over the game; the game ends from there ("Quit the game") or from the
  system's recent apps.
- **Android: the game's files are copied in.** The application may read only files it made: what adb or a computer
  puts in its folder (`Android/data/<package>/files`) belongs to someone else and cannot be read (it was refused). So
  the launcher copies the folder the user picks (the system's folder picker) into `game/` there, which needs the space
  twice over while it copies. The copy is not checked beyond `eboot.bin` being there.
- **Android: standard output and error go to `output.txt`** in that folder (the three runs before kept as
  `output.1.txt` to `output.3.txt`); the log goes to logcat as well.
- **The command line is the frontend's** on every platform; on Android it is the intent's `args` array (empty from
  the home screen).
- **nx-foundation warns on Android** (`platform/process.cpp`: an unused function `keep`) since the frontend links it
  whole; it is nx-foundation's to fix.

## System UI (dialogs, keyboards, notices, the Home menu)

- **Drawn over the picture, not into the game's surfaces.** The console draws a dialog into the frame the game
  passes to `sceCommonDialogUpdate`; here it is drawn by the window after the game's frame, so a game reading its
  own frame back (none seen) would not find it there.
- **The system's wording is English only** (our own, not the console's), whatever `[system] language` says; the
  dialogs' buttons and messages from the game are its own.
- **The save slots' own icons are not shown** (a numbered tile instead): there is no PNG reader in the runtime yet.
- **The on-screen keyboard has no kana or kanji conversion**, and no layouts but Latin letters, symbols and numbers.
  A desktop's own input method types any text into it.
- **The console's own fonts, pictures and sounds are never used**, and loading them from the user's firmware files
  (optional in the plan) is not done.
- **The PSN, store and photo dialogs are not drawn**: they are answered at once as before (signed out).
- **api_libime (SDK sample) stops in its own sprite drawing** (the guest's stack protector, with or without the
  keyboard drawn), so the in-game keyboard is checked by unit tests only.
- **With an sRGB swapchain the overlay's colours are slightly off** (its vertex colours are not converted); the
  window picks a UNORM format wherever there is one.

## Network (ad hoc over a local network)

See [ADHOC.md](ADHOC.md).

- **Only other psvrecore copies can join.** A Vita's ad hoc network is a Wi-Fi network of its own (IBSS), which
  phones and PCs cannot join; the traffic is carried over the local network instead, with psvrecore's own header on
  P2P datagrams.
- **One copy per device on a real network**: the ad hoc port (3658) is bound once. Copies on one machine need
  loopback addresses of their own (`adhoc_interface = 127.0.0.2`).
- **A P2P stream socket's virtual port is not carried**: it is a plain TCP socket on its port, so two of them on one
  port with different virtual ports would clash (no game seen uses them).
- **SceNetCtl's ad hoc peer list is empty**: peers are known only from what they send. SceNetAdhocMatching and the
  PSP-style ad hoc library still find nobody (no game seen needs them with ad hoc over the network).
- **A guest thread waiting on a socket wakes in slices** (20 ms; 4 ms for a P2P port shared by several sockets, whose
  datagrams one reader hands to the others), so a datagram for a P2P socket whose port another thread is reading
  may wait up to 4 ms more.
- **Windows asks to let the program through its firewall** the first time it listens on the network; until allowed,
  the other players' datagrams do not arrive.

## Save data and the application utility

- **The save data dialog is not implemented**, so the SDK's save data samples cannot run.
- **Other titles' save data (`savedata1:`) cannot be mounted**, nor other titles' add-ons (`addcont1:`,
  `sceAppUtilAddcontMount`).
- **Add-ons are always owned and installed.** Every folder in `addcont0:` opens; there are no entitlement checks,
  downloads or store. A file in an add-on folder that is not open is reported missing (`ENOENT`): what the console
  answers there is not documented.
- **The file system's free space is not checked before a save**: a host disk filling up shows as an error while
  writing.
- **On case-sensitive hosts, saves write names as given.** Saving `SAVE.DAT` after `save.dat` makes a second file.

## NID databases

- **NID names from different sources can disagree on aliases.** For example, vita-headers names a NID
  `sceClibMemcpy_safe` where the SDK's stub calls it `sceClibMemcpy`. When several databases are loaded, the last
  one wins.
