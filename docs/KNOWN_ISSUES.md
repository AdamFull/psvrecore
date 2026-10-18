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
  in ways the generated functions cannot follow. Those paths trap today; they belong to the runtime (M4).
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

## NID databases

- **NID names from different sources can disagree on aliases.** For example, vita-headers names a NID
  `sceClibMemcpy_safe` where the SDK's stub calls it `sceClibMemcpy`. When several databases are loaded, the last
  one wins.
