# Generated-code metrics

How much host code the generator's C becomes, per guest instruction, and the baseline every generator change is
measured against. These are static counts from compiled objects (host instructions and code bytes under each
`f_<address>` symbol), not time: cold paths count as much as hot ones. They show what a change does to the code; a
profile shows whether it matters.

## Running it

`scripts/codegen_metrics.py` reads a generated tree, compiles it (or takes the objects of an existing build) for x64
and arm64 with the release builds' flags, disassembles the objects and prints tables; `--json` writes them for a later
`--compare`.

```
# the synthetic corpus (tests/codegen; no game needed). corpusgen comes from a build of the tests
python scripts/codegen_metrics.py --synthetic --functions all --json build/syn-before.json
# a game's tree: everything, or only the files that hold the named functions
python scripts/codegen_metrics.py <game>/generated/code --functions wipeout-hot --only-function-files --json build/wo-before.json
# after a generator change: regenerate, then compare (docs/codegen_metrics_synthetic.json: the current numbers)
python scripts/codegen_metrics.py --synthetic --functions all --compare docs/codegen_metrics_synthetic.json
# a compiler option on top of the generated code's (the cache options: PSVRECORE_GENERATED_CODE_ARCH ...)
python scripts/codegen_metrics.py --synthetic --flags-x64=/clang:-march=x86-64-v3 --flags-arm64=-march=armv8.2-a
# a linked arm64 library instead of objects (link-time optimization leaves no per-file code)
python scripts/codegen_metrics.py <tree> --targets arm64 --binary-arm64 <build>/libmain.so
```

- Flags. The generated-code options come from `psvrecore_generated_code` in `cmake/PsvrecoreCompilerOptions.cmake`,
  read when the script runs, so a change there is measured (its `target_compile_options`, those under the host
  conditions inside its non-MSVC branch included; the cache options' function `_psvrecore_generated_code_options` is
  not read: give those with `--flags-x64` / `--flags-arm64`). The rest are what the release presets give a game's
  generated code: clang-cl `/O2 /Ob1 /DNDEBUG -MD -DPSVRECORE_SHARED` (x64), and the NDK toolchain's `-O2 -DNDEBUG
  -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -funwind-tables`
  for `aarch64-none-linux-android29` (arm64), each before the generated code's own (which override them: `/Ob2`,
  `-fno-stack-protector`). Debug information is left out unless `--debug-info` is given (it changes no code). The
  JSON records the exact command lines.
- Tools. x64 needs clang-cl and Visual Studio's C environment (found with vswhere); arm64 the newest Android NDK
  under `ANDROID_HOME/ndk` (or `ANDROID_NDK_HOME`). A target without its compiler is skipped with a note.
- Objects are cached under `build/codegen-metrics/` and rebuilt when the source or the flags change.
- Guest instructions are counted from the generated C: the generator writes each guest instruction as a block that
  starts with a comment holding its address and disassembly (`    { /* 8100001c: adds r0, r1, #1 */`). Code that
  several functions share is translated, and counted, in each.
- Classes (one per function): `neon` or `vfp` when at least 10% of its guest instructions are floating point or
  Advanced SIMD (whichever has more), `call-heavy` with at least one call per 8 instructions, `integer` otherwise.
- Out-of-line helpers (`PSV_COLD` functions of `guest.h`) are reported apart and are not in the ratios.

## The synthetic corpus

`tests/codegen/corpus/`: C (Thumb-2 and ARM) and hand-written assembly, compiled for the Cortex-A9 by
`scripts/codegen_corpus.py` with LLVM's clang and ld.lld and checked in as `tests/codegen/corpus_data.inc`, so
neither the tests nor the metrics need an ARM toolchain. Seven modules, one per source file:

| Module | What it holds |
| --- | --- |
| `integer` | loops over bytes, halfwords and words, 64-bit arithmetic, selects (IT blocks), TBB and TBH switches, a read-only table, an exclusive-access loop, bit fields |
| `integer_arm` | the same kinds of code in the ARM instruction set: conditional execution, ldm/stm, an address-table switch |
| `calls` | getters called in a loop, recursion, more than four arguments, C++-style virtual calls, callbacks, tail calls, Thumb-ARM interworking |
| `vfp` | single and double precision: loops, compares that branch, conversions, square roots, division |
| `neon` | VLD1/VST1 of every element size at any alignment, VLD2-4/VST2-4, float and integer lanes, widening, narrowing, saturation, permutes, tables, estimates, fixed-point conversion |
| `neon_locals` | hand-written (task 6): one Q register as Q, D and S registers and VFP arithmetic on its lanes, NEON values across calls (q4-q7 clobbered or saved by callees, an indirect call), every VLD/VST form with writeback and at the end of the corpus' memory, VFP and NEON on the same registers in a loop, a 4x4 matrix product, vec4 normalisation (vrsqrte + Newton steps), a saturating s16 mix, an RGBA-to-luma conversion (widening multiplies, narrowing shifts), a vld1/vst1 copy |
| `control` | hand-written: flags live at entry and returned by a callee, flags and a carry across calls, `pop {pc}` and `ldm` returns, tail branches, hand-made TBB/TBH, 4-instruction IT blocks, cbz/cbnz, conditional calls and returns in ARM code |
| `flags` | hand-written, for the flag liveness: flags returned by a vcmp+vmrs helper and through a veneer (`ldr pc, =f`), read at entry through a direct call, an indirect call, a tail call and an indirect jump, setters under a condition at joins (Thumb and ARM), a carry through a loop, the shifter's carry read later, flags through a jump table, mrs/msr, a call under a condition to a function that never returns, an address-table switch reached with another table in its base register |

The same corpus is the generator's whole-function differential test (`psvrecore_codegen_tests`,
`psvrecore_codegen_masked_tests`; see [STATUS.md](STATUS.md)).

## Baseline (generator at b45df9a)

Measured 2026-11-19: x64 with clang-cl 22.1.0 (LLVM), arm64 with the NDK 29.0.14206865's clang, the flags of the
time (the release presets' and `-fwrapv -fno-strict-aliasing -ffp-contract=off -fno-fast-math -w -funwind-tables`).
Host instructions per guest instruction (`/g`) and code bytes per guest instruction (`B/g`). These synthetic numbers
are in git history's `docs/codegen_metrics_synthetic.json` (before the change below); the file then held the
numbers [after it](#the-memory-base-in-a-local-jump-tables-on-their-index-the-generated-codes-options), and now those
[after the flag liveness](#condition-flags-computed-only-where-they-are-read).

### Synthetic corpus

| | Functions | Guest instructions | x64 /g | x64 B/g | arm64 /g | arm64 B/g |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 96 | 1694 | **10.45** | 41.83 | **8.30** | 33.22 |
| Module `calls` | 31 | 377 | 6.54 | 23.57 | 5.98 | 23.94 |
| Module `control` | 16 | 134 | 8.93 | 29.73 | 7.54 | 30.18 |
| Module `integer` | 18 | 605 | 5.25 | 19.25 | 4.30 | 17.19 |
| Module `integer_arm` | 6 | 120 | 6.18 | 21.88 | 5.33 | 21.33 |
| Module `neon` | 13 | 194 | 23.71 | 104.28 | 17.50 | 69.98 |
| Module `vfp` | 12 | 264 | 20.92 | 89.00 | 15.78 | 63.12 |
| Class integer | 57 | 770 | 5.80 | 19.79 | 5.09 | 20.36 |
| Class vfp | 12 | 264 | 20.92 | 89.00 | 15.78 | 63.12 |
| Class neon | 20 | 533 | 12.62 | 53.86 | 9.70 | 38.81 |
| Class call-heavy | 7 | 127 | 7.72 | 26.98 | 6.39 | 25.54 |

Out-of-line helpers, not in the ratios: 1177 instructions (x64), 941 (arm64). The `neon` class is larger than the
`neon` module: clang vectorized five integer loops (`fill_words`, `popcount_words`, `mac64`, `widen_halves`,
`saturate_add`), and `virtual_calls` and `stack_args` use NEON too.

A selection of functions:

| Function | Class | Guest | x64 /g | arm64 /g |
| --- | --- | ---: | ---: | ---: |
| `sum_bytes` (byte loop) | integer | 22 | 4.36 | 4.23 |
| `insertion_sort` | integer | 27 | 4.93 | 4.78 |
| `large_switch` (TBH) | integer | 164 | 3.79 | 2.92 |
| `crc32_nibbles` (read-only table) | integer | 17 | 5.00 | 3.82 |
| `arm_checksum` (ARM) | integer | 21 | 6.29 | 4.86 |
| `vec_sum` (getters in a loop) | call-heavy | 34 | 6.91 | 5.74 |
| `fib` (recursion) | integer | 13 | 8.62 | 7.08 |
| `fold_all` (callbacks) | call-heavy | 24 | 4.33 | 4.42 |
| `flags_across_call` | call-heavy | 16 | 12.19 | 8.25 |
| `tbh_table` (hand-made TBH) | integer | 13 | 10.69 | 9.15 |
| `dot3_f32` | vfp | 23 | 14.78 | 12.96 |
| `clamp_f32` (compares) | vfp | 32 | 5.78 | 4.75 |
| `dot_f64` | vfp | 10 | 33.20 | 22.50 |
| `mat2_f64` | vfp | 51 | 35.84 | 26.02 |
| `neon_add_f32` | neon | 12 | 13.17 | 10.92 |
| `neon_mat4` (vmla by lane) | neon | 18 | 25.06 | 21.28 |
| `neon_blend_u8` (widening, narrowing) | neon | 17 | 17.41 | 11.35 |
| `neon_u64` (64-bit lanes) | neon | 13 | 5.15 | 4.92 |
| `neon_deinterleave` (VLD3/VST4) | neon | 11 | 23.09 | 23.36 |
| `neon_permute` (zip, tbl, ext) | neon | 21 | 21.33 | 16.24 |
| `neon_normalize` (estimates) | neon | 19 | 60.74 | 41.32 |

### WipEout 2048

Its four modules as WipEout 2048's `generated/code` holds them (regenerated 2026-11-19 by this generator: byte for
byte what `psvrecomp` at b45df9a gives). Game code is not in this repository; the numbers come from
`python scripts/codegen_metrics.py <WipEout_2048>/generated/code --functions wipeout-hot` (about 15 minutes for
both targets with 8 compiles at a time).

| | Functions | Guest instructions | x64 /g | x64 B/g | arm64 /g | arm64 B/g |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16658 | 1478980 | **4.91** | 19.50 | **4.40** | 17.59 |
| Module `wo_eboot` | 14792 | 1350406 | 4.92 | 19.61 | 4.42 | 17.68 |
| Module `wo_libc` | 1201 | 65672 | 5.95 | 23.22 | 5.04 | 20.14 |
| Module `wo_libfios2` | 520 | 39042 | 3.72 | 13.76 | 3.50 | 14.02 |
| Module `wo_libult` | 145 | 23860 | 3.06 | 12.59 | 2.94 | 11.74 |
| Class integer | 10290 | 757452 | 3.63 | 13.63 | 3.40 | 13.59 |
| Class vfp | 1444 | 224442 | 6.47 | 26.48 | 5.61 | 22.43 |
| Class neon | 663 | 122161 | 12.86 | 57.08 | 10.24 | 40.95 |
| Class call-heavy | 4261 | 374925 | 3.96 | 14.95 | 3.80 | 15.19 |

Out-of-line helpers, not in the ratios: 55465 instructions (x64), 44588 (arm64).

The hot functions (`--functions wipeout-hot`; from the simpleperf profiles of WipEout's main thread):

| Function | Class | Guest | x64 insns | x64 /g | x64 B/g | arm64 insns | arm64 /g | arm64 B/g |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `f_8311A9C8` (libfios2's inflate) | integer | 383 | 1524 | 3.98 | 14.18 | 1212 | 3.16 | 12.66 |
| `f_8128DB48` | neon | 443 | 4460 | 10.07 | 43.59 | 3894 | 8.79 | 35.16 |
| `f_813BAC40` | vfp | 408 | 1779 | 4.36 | 16.38 | 1577 | 3.87 | 15.46 |
| `f_812FC59A` | neon | 1125 | 10694 | 9.51 | 42.39 | 9285 | 8.25 | 33.01 |
| `f_812FC39E` | neon | 117 | 5118 | 43.74 | 216.16 | 3824 | 32.68 | 130.74 |
| `f_81276E14` | neon | 112 | 2304 | 20.57 | 93.05 | 1750 | 15.62 | 62.50 |
| `f_8127A712` | neon | 123 | 1840 | 14.96 | 65.39 | 1458 | 11.85 | 47.41 |
| `f_8118C300` | neon | 716 | 11037 | 15.41 | 70.02 | 8891 | 12.42 | 49.67 |
| `f_813C32DC` | integer | 31 | 148 | 4.77 | 16.42 | 137 | 4.42 | 17.68 |
| `f_812A7EDC` | neon | 475 | 2986 | 6.29 | 25.68 | 2591 | 5.46 | 21.82 |
| **The ten together** | | 3933 | 41890 | 10.65 | | 34619 | 8.80 | |

`--only-function-files` measures only the 7 files that hold them (about a minute): 140553 guest instructions,
x64 5.99 and arm64 5.22 per guest instruction, the same numbers for the ten.

## The memory base in a local, jump tables on their index, the generated code's options

Measured 2026-11-19 with the same compilers. Three changes, none to what the guest code does:

- **The memory base in a local.** Each generated function reads `psv_membase` once (`uint8_t* const mb =
  psv_membase;`) and passes it to the accessors (`psv_mld32(mb, a)`, `psv_mst32(mb, a, v)`...). Before, every access
  read the global, and as generated code has no strict aliasing the compiler loaded it again after each guest store:
  through the import table on Windows (two loads), the GOT on Android. The alternative, a field of the context
  (`ctx->membase`), was measured too: the context escapes at every call, so the base is loaded again after each one.
- **Jump tables switch on the index register,** not on the target loaded through it: no guest memory access and no
  compares of the loaded target on the way. An index outside the table still loads the entry the guest reads there,
  out of line, and goes there if it is one of the table's targets (else it traps, as before). Code discovery also
  recognizes the table whose address `adr rB` (or ARM's `add rB, pc, #K`) puts in a register just before `ldr pc,
  [rB, rX, lsl #2]`, and ARM compares against a modified immediate as bounds: the synthetic corpus' `arm_switch`
  (clang's ARM switch) now runs, and WipEout 2048's executable has one table more (464), whose cases (380
  instructions) are translated now.
- **Compiler options of generated code** (`psvrecore_generated_code`): `/GS-` and `/Ob2` with clang-cl;
  `-fno-stack-protector`, `-fvisibility=hidden`, `-mno-outline-atomics` (arm64) elsewhere, and `-Bsymbolic` when
  linking on Android; line tables only as debug information. Each is measured below.

The synthetic numbers after them were `docs/codegen_metrics_synthetic.json` at 925071c (on the six-module corpus); the file
now holds the numbers [after the flag liveness](#condition-flags-computed-only-where-they-are-read).

### Synthetic corpus

| | Functions | Guest instructions | x64 before | x64 after | | arm64 before | arm64 after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 96 | 1725 | 10.45 | 10.40 | -0.5% | 8.30 | 8.00 | -3.7% |
| Module `calls` | 31 | 377 | 6.54 | 6.52 | -0.4% | 5.98 | 5.72 | -4.3% |
| Module `control` | 16 | 134 | 8.93 | 9.81 | +9.9% | 7.54 | 7.69 | +2.0% |
| Module `integer` | 18 | 605 | 5.25 | 5.29 | +0.8% | 4.30 | 4.15 | -3.4% |
| Module `integer_arm` | 6 | 151 | 6.18 | 6.66 | +7.7% | 5.33 | 5.21 | -2.4% |
| Module `neon` | 13 | 194 | 23.71 | 22.94 | -3.3% | 17.50 | 16.70 | -4.6% |
| Module `vfp` | 12 | 264 | 20.92 | 20.86 | -0.3% | 15.78 | 15.42 | -2.3% |
| Class integer | 57 | 801 | 5.80 | 6.14 | +5.8% | 5.09 | 5.04 | -1.1% |
| Class vfp | 12 | 264 | 20.92 | 20.86 | -0.3% | 15.78 | 15.42 | -2.3% |
| Class neon | 20 | 533 | 12.62 | 12.22 | -3.2% | 9.70 | 9.21 | -5.1% |
| Class call-heavy | 7 | 127 | 7.72 | 7.80 | +1.1% | 6.39 | 6.16 | -3.6% |

The corpus has 31 guest instructions more: `arm_switch`'s cases (51 and 50 host instructions as an indirect jump,
282 and 243 now). Without it: x64 10.46 -> 10.46, arm64 8.30 -> 8.03 (-3.3%). The rises are in small functions: the
tables' out-of-line path (on x64 `tbb_table` 157 -> 177, `tbh_table` 139 -> 157, `small_switch` 209 -> 231; about
the same on arm64), and on x64 `/Ob2` inlining callees into their callers (`tail_branch` 26 -> 77, `tail_dispatch`
55 -> 91, `thumb_to_arm` 52 -> 75: calls gone, code copied). The corpus is too small to show the memory base well:
WipEout's numbers below are the ones to go by.

### WipEout 2048

The modules regenerated with this generator (`psvrecomp` with the game's own module list and entries, into a
folder outside the game's repository), against the baseline above.

| | Functions | Guest instructions | x64 before | x64 after | | arm64 before | arm64 after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16658 | 1479360 | 4.91 | 4.73 | -3.6% | 4.40 | 4.18 | -5.0% |
| Module `wo_eboot` | 14792 | 1350786 | 4.92 | 4.74 | -3.7% | 4.42 | 4.20 | -5.0% |
| Module `wo_libc` | 1201 | 65672 | 5.95 | 5.87 | -1.4% | 5.04 | 4.85 | -3.7% |
| Module `wo_libfios2` | 520 | 39042 | 3.72 | 3.53 | -5.1% | 3.50 | 3.29 | -6.0% |
| Module `wo_libult` | 145 | 23860 | 3.06 | 2.91 | -4.8% | 2.94 | 2.77 | -5.5% |
| Class integer | 10290 | 757832 | 3.63 | 3.44 | -5.0% | 3.40 | 3.19 | -6.0% |
| Class vfp | 1444 | 224442 | 6.47 | 6.30 | -2.6% | 5.61 | 5.41 | -3.5% |
| Class neon | 663 | 122161 | 12.86 | 12.61 | -1.9% | 10.24 | 9.97 | -2.6% |
| Class call-heavy | 4261 | 374925 | 3.96 | 3.82 | -3.7% | 3.80 | 3.55 | -6.5% |

| Function | Class | Guest | x64 insns before | after | x64 /g before | after | arm64 insns before | after | arm64 /g before | after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `f_8311A9C8` | integer | 383 | 1524 | 1480 | 3.98 | 3.86 | 1212 | 1175 | 3.16 | 3.07 |
| `f_8128DB48` | neon | 443 | 4460 | 4349 | 10.07 | 9.82 | 3894 | 3745 | 8.79 | 8.45 |
| `f_813BAC40` | vfp | 408 | 1779 | 1687 | 4.36 | 4.13 | 1577 | 1481 | 3.87 | 3.63 |
| `f_812FC59A` | neon | 1125 | 10694 | 10408 | 9.51 | 9.25 | 9285 | 9017 | 8.25 | 8.02 |
| `f_812FC39E` | neon | 117 | 5118 | 5092 | 43.74 | 43.52 | 3824 | 3763 | 32.68 | 32.16 |
| `f_81276E14` | neon | 112 | 2304 | 2248 | 20.57 | 20.07 | 1750 | 1705 | 15.62 | 15.22 |
| `f_8127A712` | neon | 123 | 1840 | 1862 | 14.96 | 15.14 | 1458 | 1376 | 11.85 | 11.19 |
| `f_8118C300` | neon | 716 | 11037 | 10832 | 15.41 | 15.13 | 8891 | 8721 | 12.42 | 12.18 |
| `f_813C32DC` | integer | 31 | 148 | 137 | 4.77 | 4.42 | 137 | 128 | 4.42 | 4.13 |
| `f_812A7EDC` | neon | 475 | 2986 | 3007 | 6.29 | 6.33 | 2591 | 2479 | 5.46 | 5.22 |
| **Together** | | 3933 | 41890 | 41102 | 10.65 | 10.45 | 34619 | 33590 | 8.80 | 8.54 |

Out-of-line helpers: 55545 instructions (x64), 44923 (arm64), `psv_table_find` new among them.

### Each part, on the files that hold the hot functions

The 7 files of `--only-function-files --functions wipeout-hot` (140553 guest instructions), host instructions:

| | x64 | | arm64 | |
| --- | ---: | ---: | ---: | ---: |
| Before | 841535 | | 733243 | |
| Tables on their index (the base still the global) | 841930 | +0.05% | 733955 | +0.10% |
| ... and the base in a local | 811851 | -3.5% | 701060 | -4.4% |
| ... or instead the base in the context (`ctx->membase`) | 835795 | -0.7% | 726599 | -0.9% |
| The base in a local, with the compiler options | 813242 | -3.4% | 701489 | -4.3% |

The base in a local wins on both hosts, by a wide margin, so that is the one made (the integer functions of these
files: x64 -5.7%, arm64 -6.1%; with the field in the context -1.1% and -1.4%). The options one at a time, each
against the full set (the difference is what the option does):

| Option | Host instructions | What else |
| --- | --- | --- |
| clang-cl `/Ob2` (instead of RelWithDebInfo's `/Ob1`) | x64 +2035 (+0.25%) | direct calls to generated functions 7278 -> 7009 (-3.7%): small callees inlined in their file |
| clang-cl `/GS-` | x64 -644 (-0.08%) | |
| `-fno-stack-protector` | arm64 -747 (-0.11%) | `__stack_chk_fail` was called from 6 of the 7 objects |
| `-mno-outline-atomics` (arm64) | arm64 +741 (+0.11%) | no call to `__aarch64_cas4_acq_rel` (5 of the 7 objects had them) for STREX and swaps: an inline LL/SC loop instead of a call that checks for LSE each time |
| `-fvisibility=hidden` | arm64 0 | at link time: the 7 files' objects linked into a shared library have 659 PLT slots with default visibility, 609 hidden (all those left are for functions of other files, undefined there); in a whole game, none for generated code |
| `-Bsymbolic` (Android link) | | with default visibility 666 -> 616 PLT slots, as with hidden; it also binds what generated code calls in the runtime (`psv_call`, `psv_jump`, `psv_trap`...) when the runtime is in the same library (Android) |
| `-U_FORTIFY_SOURCE` | arm64 +436 (+0.06%) | **left out**: no `_chk` function is called with or without it (the difference is inlining noise) |
| `-fno-semantic-interposition` | arm64 0 | **left out**: hidden symbols are not interposable anyway |

The opt-in instruction sets (`PSVRECORE_GENERATED_CODE_ARCH`), against the full set:

| `-march` | The 7 WipEout files | Synthetic corpus |
| --- | --- | --- |
| `x86-64-v2` | x64 947476 (+16.5%; neon class +35.8%) | 19846 (+10.7%) |
| `x86-64-v3` | x64 843136 (+3.7%; neon +8.7%, vfp +2.7%) | 18721 (+4.4%; vfp -6.0%) |
| `armv8.2-a` | arm64 700721 (-0.11%: LSE atomics inline) | 13793 (-0.04%) |

More code on x64 with either: the compiler vectorizes the helpers' per-lane loops with SSE4.1 or AVX2 and inlines
rounding; static counts cannot say whether that is faster, and a build so made needs such a CPU. They stay opt-in.
armv8.2-a changes almost nothing.

### Debug information

Generated code carries line tables only (`PSVRECORE_GENERATED_CODE_DEBUG=lines`) in builds with debug information.
One WipEout file (`wo_eboot_code_10.c`) with clang-cl: the object 4.08 MB with `/Zi`, 1.20 MB with line tables only,
0.47 MB without; compiled in 15, 12 and 11 s. The line-tables object has the same 152 CodeView procedure records (the
`f_<address>` names a PDB gives debuggers, Tracy's symbol resolution and other symbolizers) and the same 39819 line
entries. arm64, the 7 files: objects 72.3 MB with `-g`, 21.2 MB with `-gline-tables-only`, 3.9 MB without; linked
into a shared library 36.9, 11.0 and 3.0 MB. `llvm-symbolizer` names an address inside `f_8128DB48` from each library
(from the symbol table, which the package's `--strip-debug` keeps and simpleperf reads), with its file and line from
the first two. Hidden symbols stay in the symbol table (as local symbols). WipEout 2048's Windows build: its PDB
203 MB before, 63 MB with line tables (the executable, with all of the changes, 30.2 and 29.3 MB; the link 2.4 and
2.0 s); the PDB has a
public symbol and a procedure record for each of the 16513 generated functions linked in.

### ThinLTO

`PSVRECORE_GENERATED_CODE_LTO=ON` (off by default). arm64: WipEout's three linked modules (76 files) compiled with
the generated-code flags by the NDK's clang, 8 at a time, and linked into a shared library with lld (8 ThinLTO
jobs), on a machine other builds were using too: without LTO 216 s to compile and 1 s to link; with ThinLTO 121 s and
92 s (the optimization moves to the link, and each change to any file links again with it). Measured on the two
libraries (`--binary-arm64`):

| | arm64 /g without | with ThinLTO | |
| --- | ---: | ---: | ---: |
| **All** | 4.20 | 4.23 | +0.6% |
| Class integer | 3.21 | 3.22 | +0.5% |
| Class vfp | 5.41 | 5.44 | +0.6% |
| Class neon | 9.97 | 9.92 | -0.4% |
| Class call-heavy | 3.55 | 3.61 | +1.7% |
| The ten hot functions | 8.54 (33590) | 8.48 (33363) | -0.7% |

Every function keeps its symbol (the function table refers to each). The extra code is callees from other files
inlined into their callers: calls gone, code copied, which a static count charges as code. Whether that is worth a
link of a minute and a half is for the call-cost work to settle against its own inlining.

x64: WipEout 2048's Windows build with the option (clang-cl `-flto=thin`, the objects LLVM bitcode, archived by
llvm-lib and linked by lld-link): the executable links in 116 s instead of 2 s; its code section is 0.3% smaller
(28.18 MB instead of 28.26; the executable has no symbols per function, so the per-function metric is arm64's). It
runs WipEout's race script into the race as the build without it does.

### In the games

WipEout 2048 and Persona 4 Dancing All Night, each regenerated with this generator and built for Windows on it
(game worktrees outside the games' checkouts), against the same game built on the psvrecore before the change with
its generated code as it was, run hidden with the same input and data:

- **WipEout 2048**, its race script (the title, the menus by touch, the first event, R held from 140 s, shots at
  100, 150, 170 and 199 s, quit at 200 s): both builds reach the race and are at the same point of it at 199 s (lap 1
  of 3, total time 1:47.64, position 8, shield 89%), no trap, the same warnings; 11830 and 11787 frames, the race
  at 59.1 and 58.8 frames a second (150-199 s). A first pair of runs, the new build's while two other builds ran,
  fell to 5-16 frames a second in the new build's run; run again in the other order on a quieter machine, both ran
  as above.
- **Persona 4 Dancing All Night**: its script for a song played by the automatic player (`run/script_auto.txt`)
  never gets past one of its waits for a colour on the screen with either build, nor with the game's own build in
  its checkout (no shot, the time limit reached at 190 s): that run does not work here. With shots at fixed times
  instead, both builds boot through the logos into the opening movie to 55 s (1655 and 1661 frames).

## Condition flags computed only where they are read

Measured 2026-11-19 with the same compilers and options. The generator computes a condition flag only where an
instruction reads it, keeps the flags in a local, and writes them to the context only where code outside the function
may read them ([ARCHITECTURE.md](ARCHITECTURE.md), "Condition flags"). Before, every call and return wrote all four
flags to the context and read them back, and the compiler could drop a flag only between them. "Before" is the
generator at 925071c. The synthetic corpus has a module more (`flags`; the modules after it in file order moved by
0x8000), so both columns are measured on the new corpus, "before" with 925071c's corpusgen built on it;
`docs/codegen_metrics_synthetic.json` now holds the numbers after, the baseline for the next change.

### Synthetic corpus

| | Functions | Guest instructions | x64 before | x64 after | | arm64 before | arm64 after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 115 | 1871 | 10.22 | 9.18 | -10.1% | 7.92 | 6.79 | -14.3% |
| Module `calls` | 31 | 377 | 6.52 | 5.24 | -19.6% | 5.72 | 4.62 | -19.2% |
| Module `control` | 16 | 134 | 9.81 | 6.82 | -30.4% | 7.69 | 5.49 | -28.6% |
| Module `flags` | 19 | 146 | 8.14 | 6.86 | -15.7% | 6.93 | 5.65 | -18.5% |
| Module `integer` | 18 | 605 | 5.29 | 4.31 | -18.4% | 4.15 | 3.20 | -22.9% |
| Module `integer_arm` | 6 | 151 | 6.66 | 5.60 | -15.9% | 5.21 | 3.99 | -23.3% |
| Module `neon` | 13 | 194 | 22.94 | 22.32 | -2.7% | 16.70 | 15.30 | -8.3% |
| Module `vfp` | 12 | 264 | 20.86 | 20.86 | -0.0% | 15.42 | 14.72 | -4.6% |
| Class integer | 70 | 909 | 6.30 | 4.86 | -23.0% | 5.19 | 3.91 | -24.6% |
| Class vfp | 14 | 280 | 20.26 | 20.21 | -0.2% | 14.99 | 14.30 | -4.6% |
| Class neon | 20 | 533 | 12.22 | 11.69 | -4.4% | 9.21 | 8.26 | -10.3% |
| Class call-heavy | 11 | 149 | 8.08 | 5.91 | -26.8% | 6.62 | 4.96 | -25.0% |

Out-of-line helpers: 1203 (x64) and 976 (arm64) instructions, unchanged. Some functions (x64, arm64): `vec_sum`
(getters in a loop) 7.03 -> 3.50, 5.59 -> 2.91; `rect_area` 12.00 -> 5.67, 11.33 -> 6.00; `fib` 8.31 -> 5.69,
6.77 -> 5.54; `flags_across_call` 10.75 -> 7.56, 7.94 -> 5.44; `large_switch` 3.74 -> 2.75, 2.99 -> 2.32;
`sum_bytes` 4.23 -> 3.68, 4.18 -> 3.04; `neon_u64` 5.38 -> 3.69, 5.00 -> 3.00. A few float functions grow a little
on x64 (`clamp_f32` 5.66 -> 5.78, `dot_f64` 33.10 -> 33.30; on arm64 -7.9% and -11.6%).

The corpus' hand-written functions that read flags at entry (`flags_at_entry`, `reads_entry_flags`, `carry_loop`)
make every one of its modules put all four flags in the context before each call and jump to code it does not know:
corpusgen gives each module what the others read (`given_at_entry` nzcv), as a game's modules would need. The games'
modules need nothing of the kind (below).

### WipEout 2048

Regenerated with this generator into the game worktree's `generated/code` (outside the game's repository), against
the generator before (`build/t2/wo-final.json`: the same code, byte for byte, as 925071c's psvrecomp gives).

| | Functions | Guest instructions | x64 before | x64 after | | arm64 before | arm64 after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16658 | 1479360 | 4.73 | 4.05 | -14.3% | 4.18 | 3.68 | -12.0% |
| Module `wo_eboot` | 14792 | 1350786 | 4.74 | 4.04 | -14.9% | 4.20 | 3.68 | -12.4% |
| Module `wo_libc` | 1201 | 65672 | 5.87 | 5.47 | -6.7% | 4.85 | 4.52 | -6.8% |
| Module `wo_libfios2` | 520 | 39042 | 3.53 | 3.04 | -13.8% | 3.29 | 2.96 | -10.0% |
| Module `wo_libult` | 145 | 23860 | 2.91 | 2.66 | -8.7% | 2.77 | 2.65 | -4.3% |
| Class integer | 10290 | 757832 | 3.44 | 2.86 | -17.0% | 3.19 | 2.75 | -13.8% |
| Class vfp | 1444 | 224442 | 6.30 | 5.88 | -6.8% | 5.41 | 5.08 | -6.0% |
| Class neon | 663 | 122161 | 12.61 | 12.35 | -2.1% | 9.97 | 9.75 | -2.1% |
| Class call-heavy | 4261 | 374925 | 3.82 | 2.67 | -29.9% | 3.55 | 2.74 | -22.8% |

| Function | Class | Guest | x64 insns before | after | x64 /g before | after | arm64 insns before | after | arm64 /g before | after |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `f_8311A9C8` | integer | 383 | 1480 | 1310 | 3.86 | 3.42 | 1175 | 1032 | 3.07 | 2.69 |
| `f_8128DB48` | neon | 443 | 4349 | 4384 | 9.82 | 9.90 | 3745 | 3766 | 8.45 | 8.50 |
| `f_813BAC40` | vfp | 408 | 1687 | 1844 | 4.13 | 4.52 | 1481 | 1433 | 3.63 | 3.51 |
| `f_812FC59A` | neon | 1125 | 10408 | 10390 | 9.25 | 9.24 | 9017 | 8968 | 8.02 | 7.97 |
| `f_812FC39E` | neon | 117 | 5092 | 5089 | 43.52 | 43.50 | 3763 | 3759 | 32.16 | 32.13 |
| `f_81276E14` | neon | 112 | 2248 | 2246 | 20.07 | 20.05 | 1705 | 1698 | 15.22 | 15.16 |
| `f_8127A712` | neon | 123 | 1862 | 1936 | 15.14 | 15.74 | 1376 | 1335 | 11.19 | 10.85 |
| `f_8118C300` | neon | 716 | 10832 | 10669 | 15.13 | 14.90 | 8721 | 8690 | 12.18 | 12.14 |
| `f_813C32DC` | integer | 31 | 137 | 127 | 4.42 | 4.10 | 128 | 116 | 4.13 | 3.74 |
| `f_812A7EDC` | neon | 475 | 3007 | 2928 | 6.33 | 6.16 | 2479 | 2494 | 5.22 | 5.25 |
| **Together** | | 3933 | 41102 | 40923 | 10.45 | 10.40 | 33590 | 33291 | 8.54 | 8.46 |

Out-of-line helpers: 55534 instructions (x64), 44923 (arm64). The hot functions are float and NEON code, where the
flags were a small part. `f_813BAC40` and `f_8127A712` grow on x64 although their C has fewer flag computations and
no flag stores (`f_813BAC40`: 104 flag writes instead of 109 helper calls and 23 stores or reloads); clang-cl
allocates and schedules the rest differently. Both shrink on arm64.

What psvrecomp reports for the modules (its `flags` lines):

| Module | Flag-setting instructions | that compute a flag | Calls | writing flags before | reading them after | Returns leaving flags | Functions reading a flag at entry | Functions leaving flags at return |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `wo_eboot` | 349556 | 76579 (21.9%) | 119629 | 0 | 0 | 0 of 17433 | 0 | 0 |
| `wo_libc` | 20175 | 7961 (39.5%) | 3542 | 113 | 577 | 1018 of 1441 | 0 | 900 |
| `wo_libfios2` | 11664 | 3521 (30.2%) | 2803 | 17 | 218 | 199 of 676 | 0 | 189 |
| `wo_libult` | 6773 | 3068 (45.3%) | 1227 | 8 | 37 | 88 of 165 | 0 | 72 |
| **WipEout 2048** | 388168 | 91129 (23.5%) | 127201 | 138 | 832 | 1305 of 19715 | 0 | 1161 |
| **Persona 4 Dancing All Night** (6 modules) | 288000 | 79649 (27.7%) | 83588 | 139 | 491 | 1398 of 16672 | 0 | 1410 |

Before, a flag-setting instruction computed all four flags whenever a call or return came before the next setter, and
every call and return wrote and read all four. The executables' code reads no flag across a call, at a function's
entry or after a return. The libraries' flags at return are their exported functions' (all four, for callers in
other modules) and what those exports' callees then leave. Every module's contract is empty (no flag read at entry or
after an indirect call or jump), so the modules fit each other.

## Core registers in locals

Measured 2026-11-19 with the same compilers and options. r0-r14 are locals of each generated function, in the context
only where code outside it may read them ([ARCHITECTURE.md](ARCHITECTURE.md), "Registers"). Two stages: **A**, the
locals with no callee-saved knowledge (`psvrecomp --no-callee-saved`: every live register read back after every call);
**B**, the default, a caller keeping the registers a callee preserves in its locals across direct calls to it, sp
checked after calls to code outside the module where callers rely on it. "Before" is the generator at f05a38a. The
corpus has a module more (`regs`, between `neon` and `vfp` in file order: `vfp` moved by 0x8000), so the synthetic
"before" is f05a38a's corpusgen built on the new corpus; WipEout's "before" is `build/t3/wo-after.json`, the same code
byte for byte as f05a38a's psvrecomp gives (checked) with the same compilers and flags. `docs/codegen_metrics_synthetic.json`
now holds the numbers of B, the baseline for the next change.

### Synthetic corpus

| | Guest | x64 before | x64 A | x64 B | arm64 before | arm64 A | arm64 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 2156 | 9.07 | 9.19 (+1.3%) | 9.25 (+2.0%) | 6.86 | 6.52 (-4.9%) | 6.56 (-4.3%) |
| Module `syn_calls` | 377 | 5.24 | 5.17 (-1.4%) | 5.30 (+1.2%) | 4.62 | 4.22 (-8.7%) | 4.25 (-8.1%) |
| Module `syn_control` | 134 | 6.82 | 7.03 (+3.1%) | 7.01 (+2.7%) | 5.49 | 5.29 (-3.7%) | 5.27 (-4.1%) |
| Module `syn_flags` | 146 | 6.86 | 7.02 (+2.4%) | 7.01 (+2.3%) | 5.65 | 5.47 (-3.1%) | 5.47 (-3.1%) |
| Module `syn_integer` | 605 | 4.31 | 4.38 (+1.5%) | 4.38 (+1.5%) | 3.20 | 2.75 (-13.9%) | 2.75 (-13.9%) |
| Module `syn_integer_arm` | 151 | 5.60 | 5.36 (-4.4%) | 5.36 (-4.4%) | 3.99 | 3.38 (-15.4%) | 3.38 (-15.4%) |
| Module `syn_neon` | 194 | 22.32 | 22.95 (+2.8%) | 22.95 (+2.8%) | 15.30 | 15.44 (+0.9%) | 15.44 (+0.9%) |
| Module `syn_regs` | 285 | 8.35 | 8.22 (-1.5%) | 8.53 (+2.1%) | 7.34 | 6.90 (-6.1%) | 7.17 (-2.4%) |
| Module `syn_vfp` | 264 | 20.86 | 21.40 (+2.6%) | 21.40 (+2.6%) | 14.72 | 14.50 (-1.5%) | 14.50 (-1.5%) |
| Class integer | 1071 | 5.11 | 5.01 (-1.9%) | 5.03 (-1.6%) | 4.17 | 3.75 (-10.0%) | 3.77 (-9.6%) |
| Class vfp | 287 | 19.85 | 20.34 (+2.4%) | 20.34 (+2.4%) | 14.07 | 13.84 (-1.6%) | 13.84 (-1.6%) |
| Class neon | 533 | 11.69 | 12.00 (+2.7%) | 12.03 (+2.9%) | 8.26 | 7.96 (-3.7%) | 7.96 (-3.6%) |
| Class call-heavy | 265 | 8.17 | 8.39 (+2.8%) | 8.77 (+7.3%) | 7.13 | 6.93 (-2.9%) | 7.17 (+0.6%) |

Static counts understate this change on loops: with the registers out of memory the compiler unrolls loops it could
not before (`sum_bytes`, `copy_bytes`, `fill_words`, `neon_u64` on x64, unrolled by 2: more instructions in all, fewer
per iteration, no context stores in the loop), and inlines callees in the same file differently (`loop_kept`, three
calls of `next_value` inlined and the loop unrolled). The same, compiled with `-fno-unroll-loops` on both hosts (only
to compare like with like; the builds unroll):

| | Guest | x64 before | x64 A | x64 B | arm64 before | arm64 A | arm64 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 2156 | 8.78 | 8.69 (-1.0%) | 8.74 (-0.4%) | 6.90 | 6.56 (-4.9%) | 6.59 (-4.5%) |
| Class integer | 1071 | 4.99 | 4.66 (-6.7%) | 4.68 (-6.2%) | 4.14 | 3.73 (-10.0%) | 3.74 (-9.7%) |
| Class vfp | 287 | 19.85 | 20.34 (+2.4%) | 20.34 (+2.4%) | 14.07 | 13.84 (-1.6%) | 13.84 (-1.6%) |
| Class neon | 533 | 10.72 | 10.67 (-0.5%) | 10.69 (-0.3%) | 8.45 | 8.15 (-3.5%) | 8.15 (-3.5%) |
| Class call-heavy | 265 | 8.17 | 8.39 (+2.8%) | 8.66 (+6.0%) | 7.13 | 6.93 (-2.9%) | 7.07 (-0.9%) |

What the code does in time, the corpus' functions run by the differential test's harness (`CODEGEN_BENCH=<rounds>`:
2000 calls per round from the first random state, the fastest round; `build/t4/bench2.py`: the three builds' test
programs alternately, 6 rounds each, pinned to one logical CPU of a performance core at high priority, the fastest of
all rounds per function). This machine was running other builds, so the numbers move by a few percent from run to run:
the same program run as two builds differed by 1.9% in the geometric mean, and functions whose code is identical in A
and B (no calls) differed by up to 8%. Geometric means over the 114 functions: before 48.5 ns, A 49.7 ns (+2.3%), B
47.2 ns (-2.7%): within that noise. Loops over memory without calls gain clearly in both: `arm_mla` 83 -> 53 ns
(-36%), `copy_bytes` 64 -> 42/39, `arm_checksum` 296 -> 214, `insertion_sort` 103 -> 86/80, `fill_words` 76 -> 60/56,
`bounded_strlen` 124 -> 96/89, `carry_loop` 38 -> 29, `step_particles` 52 -> 44 (A/B where they differ). An earlier
run of the same programs without pinning gave B at -30%, an artifact of the cores the programs landed on.

### WipEout 2048

Regenerated with this generator (`build/t4/wo-a`, `build/t4/wo-b2`; the game build's code is the latter, byte for byte).

| | Guest | x64 before | x64 A | x64 B | arm64 before | arm64 A | arm64 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 1479360 | 4.05 | 4.00 (-1.4%) | 4.02 (-0.8%) | 3.68 | 3.45 (-6.2%) | 3.48 (-5.4%) |
| Module `wo_eboot` | 1350786 | 4.04 | 3.97 (-1.6%) | 3.99 (-1.1%) | 3.68 | 3.45 (-6.1%) | 3.47 (-5.5%) |
| Module `wo_libc` | 65672 | 5.47 | 5.58 (+2.0%) | 5.58 (+2.0%) | 4.52 | 4.30 (-4.9%) | 4.47 (-1.2%) |
| Module `wo_libfios2` | 39042 | 3.04 | 2.98 (-2.0%) | 3.05 (+0.1%) | 2.96 | 2.59 (-12.6%) | 2.62 (-11.6%) |
| Module `wo_libult` | 23860 | 2.66 | 2.69 (+1.1%) | 2.80 (+5.2%) | 2.65 | 2.50 (-5.9%) | 2.64 (-0.6%) |
| Class integer | 757832 | 2.86 | 2.77 (-3.1%) | 2.82 (-1.2%) | 2.75 | 2.42 (-12.2%) | 2.50 (-9.0%) |
| Class vfp | 224442 | 5.88 | 5.84 (-0.6%) | 5.86 (-0.3%) | 5.08 | 4.99 (-1.9%) | 5.01 (-1.5%) |
| Class neon | 122161 | 12.35 | 12.35 (-0.0%) | 12.33 (-0.1%) | 9.75 | 9.77 (+0.2%) | 9.74 (-0.1%) |
| Class call-heavy | 374925 | 2.67 | 2.65 (-0.9%) | 2.62 (-1.9%) | 2.74 | 2.56 (-6.4%) | 2.50 (-8.6%) |

| | Guest | x64 before | x64 A | x64 B | arm64 before | arm64 A | arm64 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `f_8311A9C8` | 383 | 3.42 | 3.47 (+1.5%) | 3.52 (+2.9%) | 2.69 | 2.35 (-13.0%) | 2.42 (-10.3%) |
| `f_8128DB48` | 443 | 9.90 | 9.53 (-3.7%) | 9.62 (-2.8%) | 8.50 | 8.18 (-3.8%) | 8.23 (-3.2%) |
| `f_813BAC40` | 408 | 4.52 | 5.13 (+13.5%) | 5.16 (+14.2%) | 3.51 | 3.06 (-12.8%) | 3.35 (-4.6%) |
| `f_812FC59A` | 1125 | 9.24 | 9.18 (-0.7%) | 9.46 (+2.4%) | 7.97 | 8.42 (+5.6%) | 8.36 (+4.9%) |
| `f_812FC39E` | 117 | 43.50 | 43.20 (-0.7%) | 43.20 (-0.7%) | 32.13 | 32.67 (+1.7%) | 32.67 (+1.7%) |
| `f_81276E14` | 112 | 20.05 | 20.08 (+0.1%) | 20.08 (+0.1%) | 15.16 | 14.96 (-1.3%) | 14.96 (-1.3%) |
| `f_8127A712` | 123 | 15.74 | 16.17 (+2.7%) | 16.52 (+5.0%) | 10.85 | 11.94 (+10.0%) | 12.16 (+12.1%) |
| `f_8118C300` | 716 | 14.90 | 14.71 (-1.3%) | 14.77 (-0.9%) | 12.14 | 13.12 (+8.1%) | 12.63 (+4.1%) |
| `f_813C32DC` | 31 | 4.10 | 4.58 (+11.8%) | 4.84 (+18.1%) | 3.74 | 3.35 (-10.3%) | 3.65 (-2.6%) |
| `f_812A7EDC` | 475 | 6.16 | 6.55 (+6.3%) | 6.53 (+6.0%) | 5.25 | 5.70 (+8.5%) | 5.87 (+11.7%) |

The integer and call-heavy code shrinks on arm64 (-9 to -12%), little on x64. Of the hot ten, the integer and VFP
ones shrink on arm64 and grow on x64 (`f_813BAC40` +14%, `f_813C32DC` +18%; not looked into), and the NEON ones grow
on arm64 by up to 12%. They call out-of-line helpers; probably the locals live across those calls take callee-saved
host registers or spills, where before the values were loaded from the context again after each call (not checked).

What each part costs, whole program (B's variants are psvrecomp options):

| | x64 /g | | arm64 /g | |
| --- | ---: | ---: | ---: | ---: |
| before (f05a38a) | 4.05 | | 3.68 | |
| A | 4.00 | -1.4% | 3.45 | -6.2% |
| B (the default) | 4.02 | -0.8% | 3.48 | -5.4% |
| B, sp checked by a trap at each call | 4.04 | -0.4% | 3.49 | -5.0% |
| B, sp not checked (`--no-sp-check`) | 3.99 | -1.5% | 3.45 | -6.1% |
| B, sp checked by a trap at each call, kept in loops only (`--keep-registers loops`) | 4.04 | -0.4% | 3.50 | -5.0% |
| B, sp checked by a trap at each call, kept nowhere (`--keep-registers none`) | 4.03 | -0.6% | 3.48 | -5.4% |

The check of sp after calls to code outside the module (B against B without it) costs 0.7% on x64 and 0.7% on arm64,
down from 1.1% and 1.2% with a trap at each call: one trap per function now, the check a load, a compare and a branch.
Keeping registers across calls costs about as much as reading them back in static counts (kept nowhere, or only in
loops, against kept at every call, all three with the trap at each call: 0.2% on x64, 0.4% on arm64); A and B without
the check are within 0.1%. The default keeps the check (a trap where the analysis'
one assumption about other code fails, instead of wrong values) and keeps registers at every direct call (fewer loads
in loops that call, which a static count does not see).

Context traffic as the generated C writes it (`build/t4/ctxcount.py`: `ctx->r[N]` read and written, all four modules,
the stores before calls, exits and traps, the reads at entry and after calls; the C compiler removes some of either):

| | Reads of `ctx->r[]` | of them at function entry | Writes |
| --- | ---: | ---: | ---: |
| before | 1231271 | | 1039554 |
| A | 469729 | 98949 | 633872 |
| B | 327332 | 98950 | 631333 |

Registers read back after calls (`build/t4/reloads.py`), WipEout's four modules:

| | Direct calls (104956) | of them r4-r11 | sp | Imports (16748) | Indirect (5497) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A | 296460 | 157192 | 42530 | 52087 | 22233 |
| B | 150066 | 47618 | 5710 | 56187 | 22129 |

(After imports B reads more than A: sp stays in the local after 7706 of them, checked, and what that frees is spent
elsewhere; the rest are the registers the import may change.) psvrecomp's counts for the executable: 12436 of its
14792 functions preserve r4, 10917 r5, 8087 r8, 5941 r11, 12478 sp, 5870 all of r4-r11 and sp; 82209 of its 104956
direct calls keep 212227 registers in locals; 6225 functions check sp after their calls to code outside the module
(7706 checks). The exception index cross-check: 11518 of its functions have unwind opcodes that describe a frame, and
all 11518 agree (every register a function is found to preserve and writes, its unwinding restores); 2296 restore a
register the analysis does not show preserved (a frame the proof does not follow: an indirect jump, an unknown stack
pointer); 3829 entries describe no frame or do not unwind. No internal entries
(stage C, arguments in C arguments) are generated: not done.

Generated source: 242.8 MB before, 247.7 MB with A (+2.0%), 253.3 MB with B (+4.3%; the `PSV_KEPT` lines a
`PSV_CHECK_KEPT_REGISTERS` build checks, one per call that keeps a register, are most of the difference).

The numbers above were taken before a bug in `vmsr` was fixed (its register stores, which belong on the way to its
trap only, were on every path; found by WipEout's first run and the `vmsr_restore` corpus test since): the fixed
generator's WipEout code differs in two libc files, 4 lines each, at two `vmsr` sites.

Compile time (`build/t4/compile_time.py`: three of WipEout's files, before and B, one at a time, alternating, twice
each, the fastest kept): x64 31.5 -> 31.9 s (+1.3%; `wo_eboot_code_10.c` 5.7 -> 5.9, `_40.c` 14.2 -> 14.5,
`wo_libc_code_2.c` 11.6 -> 11.5), arm64 40.9 -> 39.8 s (-2.8%; 8.6 -> 7.8, 17.6 -> 17.1, 14.7 -> 14.9). Generating
WipEout's four modules takes psvrecomp (with the script around it) 19 s, 9-10 s with `--registers-in-context` (the
same program: the probe pass and the analysis are the difference). With `--registers-in-context` the generator writes
the code of f05a38a byte for byte (all of WipEout's files compared).

### Games

Hidden runs (`--hidden`), each with its own data, settings and shots, the build of f05a38a's generator and this one
back to back, Windows x64 release.

- WipEout 2048, the race script, twice: first base then new (the new build still with the `vmsr` bug), then new then
  base (fixed). At the 199 s shot all four runs are on lap 1/3 in 8th place with 89% shield, total time 1:47.60 /
  1:47.60 and 1:47.65 (new) / 1:47.58 (base); the same four warnings (system module 0x002d twice, one untranslated
  GXP program), no traps. Frames 11814 / 11658 and 11767 / 11825 (base / new, new / base); race 150-199 s 58.3 / 56.9
  and 58.4 (new) / 58.9 (base) fps. The time and the frames move together (the game counts frames). The new build
  was the slower in both pairs (by 2.4% and 0.8% over 150-199 s), but the machine was building and testing in the
  background (in the second pair, the debug tests during the new run and the start of the base run), so these runs
  do not tell a speed difference either way; the game holds 60 fps most of the race in all four.
- Persona 4 Dancing All Night, the fixed-time run (shots at 3-55 s), base then new: both reach the opening movie and
  are at the same scenes at 40 s and 55 s (a few frames apart, the 8 s shot identical), the same single gxm warning,
  no traps, 1658 / 1656 frames.

## Floating-point registers in locals, typed operations

Measured 2026-11-19 with the same compilers and options (the generated code's now with `-fno-math-errno`). The S and D
registers and the FPSCR are typed locals of each function ([ARCHITECTURE.md](ARCHITECTURE.md), "VFP registers"); each
operation is guest.h's typed one: on x64, and on arm64 by default, the host's operation with a check of its result and
an exact fallback out of line; on arm64 with `PSVRECORE_GENERATED_CODE_NATIVE_FLOAT` the host's instruction alone
(measured with `--flags-arm64=-DPSV_NATIVE_FLOAT --flags-arm64=-fdenormal-fp-math=dynamic`; the option is off by
default and not yet verified on a device). "Before" is the generator at 75635f5 with its own flags. The corpus has a
module more (`vfp_locals`, last in file order) and its data moved to 0x82050000, so the synthetic "before" is
75635f5's corpusgen built on the new corpus, measured by 75635f5's script; WipEout's "before" is 75635f5's
psvrecomp's tree. `docs/codegen_metrics_synthetic.json` now holds the x64 and default arm64 numbers of this change, the
baseline for the next one.

### Synthetic corpus

| | Functions | Guest | x64 before | x64 after | | arm64 before | arm64 after | | arm64 native | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 175 | 2458 | 10.84 | 9.51 | -12.3% | 7.73 | 7.64 | -1.1% | 5.04 | -34.8% |
| Module `syn_calls` | 31 | 377 | 5.30 | 5.34 | +0.7% | 4.25 | 4.25 | +0.1% | 4.25 | +0.1% |
| Module `syn_control` | 16 | 134 | 7.01 | 7.01 | +0.0% | 5.27 | 5.27 | +0.0% | 5.27 | +0.0% |
| Module `syn_flags` | 19 | 146 | 7.01 | 6.97 | -0.6% | 5.47 | 5.40 | -1.4% | 5.24 | -4.3% |
| Module `syn_integer` | 18 | 605 | 4.38 | 4.38 | +0.0% | 2.75 | 2.75 | +0.0% | 2.75 | +0.0% |
| Module `syn_integer_arm` | 6 | 151 | 5.36 | 5.34 | -0.3% | 3.38 | 3.38 | +0.0% | 3.38 | +0.0% |
| Module `syn_neon` | 13 | 194 | 22.95 | 22.61 | -1.5% | 15.44 | 15.45 | +0.1% | 15.63 | +1.2% |
| Module `syn_regs` | 44 | 291 | 8.49 | 8.49 | +0.0% | 7.16 | 7.14 | -0.2% | 7.17 | +0.1% |
| Module `syn_vfp` | 12 | 264 | 21.40 | 14.18 | -33.7% | 14.50 | 12.87 | -11.2% | 2.58 | -82.2% |
| Module `syn_vfp_locals` | 16 | 296 | 22.48 | 18.05 | -19.7% | 16.25 | 17.02 | +4.7% | 4.52 | -72.2% |
| Class integer | 98 | 1074 | 5.02 | 5.02 | -0.0% | 3.77 | 3.77 | +0.0% | 3.77 | +0.0% |
| Class vfp | 30 | 568 | 21.27 | 15.85 | -25.5% | 14.94 | 14.74 | -1.4% | 3.62 | -75.8% |
| Class neon | 21 | 551 | 12.43 | 12.07 | -2.8% | 8.29 | 8.12 | -2.0% | 7.96 | -3.9% |
| Class call-heavy | 26 | 265 | 8.77 | 8.76 | -0.1% | 7.17 | 7.17 | -0.1% | 7.17 | -0.1% |

Out-of-line helpers: 1590 -> 3104 instructions (x64), 1286 -> 2322 (arm64), 849 (arm64 native): the typed
operations' exact fallbacks. A static count weighs the fallbacks and the flush-to-zero checks like the fast path: each
operation's check is inline in both modes (FZ clear: is the result a NaN; FZ set: also an operand or the result
denormal or at the smallest normal, one unsigned minimum of three keys), and clang threads the FZ test through a
function, so the code holds two copies of each run of operations, one per mode. Without the FZ checks inline (an
earlier version: FZ handled in the out-of-line helper) the static numbers were lower (on the corpus of then, class vfp
x64 -44.8%, arm64 -35.7%), but every operation in FZ mode became a call, and the float functions of the timing run
took up to 2.3 times as long. VFP functions (x64 before / after, arm64 before / after / native, per guest instruction): `dot_f64` 35.4 ->
16.5, 19.8 -> 14.2 / 3.6; `newton_f64` 34.7 -> 15.1, 23.1 -> 12.2 / 2.2; `mat2_f64` 39.1 -> 17.3, 25.1 -> 21.7 / 2.4;
`saves_d8` 35.8 -> 10.3, 23.5 -> 9.8 / 3.5; `partial_d` 60.8 -> 28.3, 37.5 -> 23.3 / 4.0; `clamp_f32` 5.8 -> 4.9,
4.2 -> 3.5 / 2.4; `cmp_loop` 13.4 -> 9.2, 4.7 -> 4.3 / 3.5. Single precision grows in static size where its fast
path was already inline (`dot3_f32` 14.0 -> 14.7, 11.4 -> 14.8 / 2.4; `lerp_f32`, `div_abs_f32`, `arith_all` on
arm64 89.5 from 56.9): the duplicated FZ copy.

Per guest operation (`build/t5/ops`: a module of functions that each repeat one operation 8 times in a dependent
chain, less an empty function; the compare rows less the same branch on an integer compare). First the static count
(every path), then the fall-through path the compiler laid out as the likely one (FZ clear, no NaN: `hot_path.py`, from
the entry with every conditional branch not taken; it includes each function's frame over 8 operations, about 2.5
instructions per operation on x64):

| Guest operation | x64 before | x64 after | arm64 before | arm64 after | arm64 native |
| --- | ---: | ---: | ---: | ---: | ---: |
| vadd.f32 | 43.8 | 44.6 | 32.5 | 37.1 | 1.1 |
| vmul.f32 | 43.8 | 44.6 | 32.5 | 37.1 | 1.1 |
| vdiv.f32 | 43.8 | 44.6 | 32.5 | 37.1 | 1.1 |
| vsqrt.f32 | 49.9 | 33.0 | 32.6 | 25.1 | 1.1 |
| vmla.f32 | 77.2 | 87.4 | 54.6 | 77.0 | 2.2 |
| vadd.f64 | 93.1 | 44.2 | 69.9 | 37.1 | 1.1 |
| vmul.f64 | 110.8 | 44.2 | 82.0 | 37.1 | 1.1 |
| vdiv.f64 | 123.0 | 44.2 | 91.0 | 37.1 | 1.1 |
| vsqrt.f64 | 30.1 | 33.5 | 20.6 | 25.1 | 1.1 |
| vmla.f64 | 221.0 | 86.6 | 151.6 | 77.0 | 2.2 |
| vneg.f32 (with a vadd.f32, less the vadd) | 4.0 | 1.6 | 1.0 | 3.4 | 1.6 |
| vcvt.s32.f32 + vcvt.f32.s32 | 30.5 | 17.8 | 22.2 | 2.5 | 2.5 |
| vcvtr.s32.f32 + vcvt.f32.s32 | 30.5 | 17.8 | 22.2 | 2.5 | 2.5 |
| vcvt.s32.f64 + vcvt.f64.s32 | 33.2 | 16.0 | 21.5 | 2.5 | 2.5 |
| vcvt.f64.f32 + vcvt.f32.f64 | 73.8 | 62.5 | 55.0 | 54.2 | 2.5 |
| vcmp.f32 + vmrs + b (less an integer cmp + b) | 33.6 | 14.1 | 27.2 | 20.1 | 5.1 |
| vcmpe.f64 + vmrs + b (less an integer cmp + b) | 47.6 | 17.4 | 32.5 | 19.6 | 4.8 |

Fall-through path:

| Guest operation | x64 before | x64 after | arm64 before | arm64 after | arm64 native |
| --- | ---: | ---: | ---: | ---: | ---: |
| vadd.f32 | 30.5 | 6.8 | 21.0 | 4.8 | 1.4 |
| vmul.f32 | 30.5 | 6.8 | 21.0 | 4.8 | 1.4 |
| vdiv.f32 | 30.5 | 6.8 | 21.0 | 4.8 | 1.4 |
| vsqrt.f32 | 38.4 | 6.0 | 13.9 | 4.5 | 1.4 |
| vmla.f32 | 57.6 | 12.1 | 41.2 | 8.4 | 2.5 |
| vadd.f64 | 51.9 | 7.0 | 35.0 | 4.8 | 1.4 |
| vmul.f64 | 62.8 | 7.0 | 35.0 | 4.8 | 1.4 |
| vdiv.f64 | 79.9 | 7.0 | 44.6 | 4.8 | 1.4 |
| vsqrt.f64 | 23.6 | 6.2 | 13.9 | 4.5 | 1.4 |
| vmla.f64 | 77.4 | 12.1 | 69.4 | 8.4 | 2.5 |
| vneg.f32 (with a vadd.f32, less the vadd) | 3.2 | 3.2 | 3.0 | 2.5 | 1.9 |
| vcvt.s32.f32 + vcvt.f32.s32 | 26.8 | 8.5 | 21.2 | 2.5 | 2.5 |
| vcvtr.s32.f32 + vcvt.f32.s32 | 26.8 | 8.5 | 21.2 | 2.5 | 2.5 |
| vcvt.s32.f64 + vcvt.f64.s32 | 20.8 | 8.8 | 20.8 | 3.0 | 3.0 |
| vcvt.f64.f32 + vcvt.f32.f64 | 47.5 | 10.5 | 37.5 | 8.8 | 3.0 |
| vcmp.f32 + vmrs + b (less an integer cmp + b) | 64.4 | 9.1 | 47.0 | 7.9 | 1.0 |
| vcmpe.f64 + vmrs + b (less an integer cmp + b) | 28.4 | 13.0 | 29.4 | 11.6 | 1.4 |

On x64 the fast path of an addition is `movaps; addss; ucomiss; jp` (the copy is SSE's two-operand form), on arm64
`fadd; fcmp; b.vs`, native `fadd` alone; before, each operation loaded its operands from the context, checked both
exponents and the result's, and stored the result. Conversions to integers are one SSE conversion and a compare with
0x80000000 on x64, one FCVTZS/FCVTNS on arm64 (its saturation and NaN-to-0 are VFP's) in both arm64 builds. vcmp +
vmrs + branch is one ucomiss (fcmp) and branch; the remaining instructions in that row compute the FPSCR's NZCV, which
stays in the context at the function's exit (KNOWN_ISSUES.md).

What the code does in time (x64 only; `build/t4/bench2.py`, the corpus' functions run by the differential test's
harness, both programs alternately, 6 rounds each, pinned to one logical CPU at high priority, twice on two CPUs). The
machine was running other sessions' work (another program busy on two cores, four processes spinning a core each);
integer functions, whose code did not change, moved by -2.5% to +3.8% between the programs, which is the noise here.
Geometric means:

| | Functions | CPU 2: before | after | | CPU 8: before | after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| All | 126 | 42.93 ns | 41.97 ns | -2.2% | 42.96 ns | 39.72 ns | -7.5% |
| Class integer | 55 | 26.45 | 27.45 | +3.8% | 26.44 | 25.84 | -2.3% |
| Class call-heavy | 25 | 26.64 | 27.51 | +3.3% | 26.57 | 25.90 | -2.5% |
| Class vfp | 26 | 98.90 | 79.93 | -19.2% | 99.33 | 76.33 | -23.1% |
| Class neon | 20 | 99.76 | 99.03 | -0.7% | 100.07 | 94.55 | -5.5% |

The float functions that gain most (both runs, after / before): `partial_d` 0.42-0.46, `cond_all` 0.43-0.47,
`newton_f64` 0.48-0.50, `cvt_all` 0.47-0.51, `clamp_f32` 0.55-0.57, `convert_f32` 0.57, `vmsr_modes` 0.55-0.58,
`neon_fixed` (NEON with VFP conversions) 0.47-0.48. Three are slower: `dot3_f32` 1.86-1.95, `length_f32` 1.50-1.56,
`arith_all` 1.47-1.59. Their start states hold NaNs and infinities (the corpus' random memory words and "any"
registers include special values), their sums become NaN early, and from then on every operation calls its
out-of-line exact helper, which in a loop also costs the spills of the live float locals; 75635f5 handled NaN
operands with the same kind of call but had nothing in registers to spill. The FZ checks were out of line in an
earlier version of this change; with random FPSCRs half the start states have FZ set, and `mixed_neon`, `dot3_f32`,
`arith_all`, `length_f32` and `lerp_f32` took 1.3-2.3 times as long: hence the inline checks.

### WipEout 2048

Regenerated with this generator into the game worktree's `generated/code` (the game build's code); "before" is
75635f5's psvrecomp's tree (`build/wo-game-base`).

| | Functions | Guest | x64 before | x64 after | | arm64 before | arm64 after | | arm64 native | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16658 | 1479360 | 4.02 | 4.00 | -0.4% | 3.48 | 3.53 | +1.5% | 3.11 | -10.6% |
| Module `wo_eboot` | 14792 | 1350786 | 3.99 | 4.03 | +1.0% | 3.47 | 3.56 | +2.5% | 3.15 | -9.4% |
| Module `wo_libc` | 1201 | 65672 | 5.58 | 4.44 | -20.5% | 4.47 | 3.83 | -14.3% | 2.89 | -35.2% |
| Module `wo_libfios2` | 520 | 39042 | 3.05 | 3.04 | -0.1% | 2.62 | 2.62 | -0.1% | 2.62 | -0.2% |
| Module `wo_libult` | 145 | 23860 | 2.80 | 2.80 | +0.0% | 2.64 | 2.64 | +0.0% | 2.64 | +0.0% |
| Class integer | 10290 | 757832 | 2.82 | 2.83 | +0.2% | 2.50 | 2.51 | +0.3% | 2.48 | -1.0% |
| Class vfp | 1444 | 224442 | 5.86 | 5.61 | -4.2% | 5.01 | 5.21 | +4.1% | 3.00 | -40.0% |
| Class neon | 663 | 122161 | 12.33 | 12.56 | +1.9% | 9.74 | 9.93 | +2.0% | 9.16 | -5.9% |
| Class call-heavy | 4261 | 374925 | 2.62 | 2.63 | +0.0% | 2.50 | 2.51 | +0.1% | 2.49 | -0.8% |
| `f_8311A9C8` (integer) | 1 | 383 | 3.52 | 3.52 | +0.0% | 2.42 | 2.42 | +0.0% | 2.42 | +0.0% |
| `f_8128DB48` (neon) | 1 | 443 | 9.62 | 9.70 | +0.9% | 8.23 | 8.45 | +2.7% | 7.32 | -11.1% |
| `f_813BAC40` (vfp) | 1 | 408 | 5.16 | 5.41 | +4.8% | 3.35 | 4.16 | +24.1% | 3.73 | +11.3% |
| `f_812FC59A` (neon) | 1 | 1125 | 9.46 | 9.66 | +2.2% | 8.36 | 8.71 | +4.2% | 7.22 | -13.7% |
| `f_812FC39E` (neon) | 1 | 117 | 43.20 | 43.48 | +0.6% | 32.67 | 32.40 | -0.8% | 33.29 | +1.9% |
| `f_81276E14` (neon) | 1 | 112 | 20.08 | 20.48 | +2.0% | 14.96 | 14.80 | -1.1% | 14.71 | -1.7% |
| `f_8127A712` (neon) | 1 | 123 | 16.52 | 17.41 | +5.4% | 12.16 | 12.41 | +2.1% | 10.98 | -9.8% |
| `f_8118C300` (neon) | 1 | 716 | 14.77 | 14.82 | +0.4% | 12.63 | 12.09 | -4.3% | 11.45 | -9.3% |
| `f_813C32DC` (integer) | 1 | 31 | 4.84 | 4.84 | +0.0% | 3.65 | 3.65 | +0.0% | 3.65 | +0.0% |
| `f_812A7EDC` (neon) | 1 | 475 | 6.53 | 6.91 | +5.8% | 5.87 | 6.21 | +5.9% | 3.92 | -33.3% |

Out-of-line helpers: 55500 -> 68062 instructions (x64), 44903 -> 53687 (arm64), 35843 (arm64 native). In static
counts the whole program hardly moves on x64 (-0.4%) and grows on arm64 (+1.5%); libc's float code shrinks (-20.5%,
-14.3%: its doubles had no fast path before), the executable's single-precision code grows on arm64 (class vfp +4.1%),
where 75635f5's binary32 operations were already inline with a short check and each operation now has its FZ copy
(above). Native floating point takes the VFP class to 3.00 instructions per guest instruction (-40%), the whole
program -10.6%. The NEON class grows by 2% (the floating-point locals written to the context before an Advanced SIMD
instruction that reads them and read back after it: 9174 such reloads in the executable). Of the hot ten, the NEON
ones grow by up to 6% (`f_812A7EDC` +5.8% on x64, +5.9% on arm64, -33% native), and `f_813BAC40` (VFP) grows by 4.8%
(x64) and 24% (arm64), 11% even native (not looked into: register pressure of the locals across its many helper calls
is the likely cause).

What psvrecomp reports (`vfp` lines):

| Module | Functions with VFP locals | D registers as two floats / as a double | Read at entry | Store places | Reloads (after Advanced SIMD) | Calls keeping a live local | Trap stores | Fused vcmp + vmrs | Fused conditions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `wo_eboot` | 2482 | 8952 / 1995 | 11251 | 14058 | 15156 (9174) | 6160 | 76 | 3812 | 3692 |
| `wo_libc` | 276 | 200 / 703 | 1042 | 1218 | 915 (411) | 561 | 1 | 595 | 595 |
| `wo_libfios2` | 50 | 1 / 57 | 1 | 52 | 48 (0) | 2 | 0 | 0 | 0 |
| `wo_libult` | 0 | | | | | | | | |

Generated source: 243 MB before, 246 MB after. Context traffic in the generated C: `ctx->vfp` writes 75196 -> 61110,
FPSCR references 38053 -> 17103. `psvrecomp --vfp-in-context` writes 75635f5's code byte for byte (WipEout's four
modules compared).

### Games

Hidden runs (`--hidden`), each with its own data, settings and shots, the build of 75635f5's generator and this one
back to back, Windows x64 release (the runtime and generated code of each built from its own tree).

- WipEout 2048, the race script, three pairs: base then new, new then base (both with an earlier version of the typed
  operations), and new then base with the final code. At the 199 s shot all six runs are on lap 1/3 in 8th place.
  Race time and shield: base 1:47.71 / 89%, 1:46.80 / 90%, 1:47.41 / 89%; new 1:46.81 / 90%, 1:47.66 / 89%,
  1:47.65 / 89% (frames drawn: base 11913, 11548, 11754; new 11677, 11887, 11892). The script's inputs come at fixed
  wall-clock times, so a run that drew more or fewer frames meets them at another point of the track: both builds
  give both outcomes, and the time follows the frames. The same four warnings in every run (system module 0x002d
  twice, one untranslated GXP program), no traps. The machine was running other sessions' work (another program
  busy on two cores, four processes spinning a core each), so these runs do not show a speed difference.
- Persona 4 Dancing All Night, the fixed-time run (shots at 3-55 s), two pairs (base then new with the earlier
  version, new then base with the final code): all four reach the opening movie and are at the same scenes at 40 s
  and 55 s, a few frames apart (the 8 s shot identical in both pairs), the same single gxm warning (the mask bit of
  DF32M_S8 surfaces), no traps; frames 1660 / 1657 and 1661 (new) / 1658 (base).

## Advanced SIMD on values, Q registers in locals

Measured 2026-11-19 and 20 with the same compilers and options. NEON instructions work on the floating-point locals'
values (guest_neon.h's vector helpers), a Q register can be one vector local, and single-precision lanes are exact by a
flush of the inputs and a fix of the results ([ARCHITECTURE.md](ARCHITECTURE.md), "Advanced SIMD"). "Before" is the
generator at 10acc8b with its own headers and script. The corpus has one more module (`neon_locals`, between `neon` and
`regs` in file order, so the later modules move), and the synthetic "before" is 10acc8b's corpusgen built on the new
corpus. WipEout's and P4D's "before" are the trees from 10acc8b's psvrecomp (`build/wo-game-base`,
`build/p4d-game-base`). The "native" columns are arm64 with `PSV_NATIVE_FLOAT` (`--flags-arm64=-DPSV_NATIVE_FLOAT
--flags-arm64=-fdenormal-fp-math=dynamic`), before and after. That option stays off by default and has not been
verified on a device. `docs/codegen_metrics_synthetic.json` now holds this change's x64 and default arm64 numbers, the
baseline for the next change.

The order of the work came from a static histogram of the NEON instructions in task 5's trees (`build/t6/neon_hist.py`).
WipEout 2048 has 51044 NEON instructions in 1479360 (3.5%). 45408 of them are in 719 functions where NEON is at least
10% of the code (30.7% there). By form: `vorr d` as a move 5828, `vmul.f32 q` 4719, `vdup.32 d` 3788, `vld1.8` of two
registers 3570, `vst1.8` of two 3191, `vmla.f32 q` 2464, `vbif d` 1664, `vorr q` (move) 1548, `vdup.32 q` 1423,
`vld1.32` to all lanes 1401, `vadd.f32 d` 1066, `vmul.f32 d` 1006, `vrsqrts.f32 q` 906, `vpadd.f32` 854, `vst1.32`
lane 662, `vcgt.f32 d` 629. After those come the immediates, `vrsqrte`, `vext`, `vmls`, `vsli.64`, `vrecpe` and
`vtrn`. Integer arithmetic (`psv_n3`) has 134 instructions in all; there are no long or narrow forms and no `vtbl`.
P4D has 20147 NEON instructions, led by the same forms (`vorr d`, `vmul.f32 q`, `vmla.f32 q`, `vdup.32 q`, `vbif`,
`vadd.f32 q`), plus 127 long forms and 39 `vtbl`. In task 5 the context was synced around each of these instructions:
19409 stores of a VFP local before them or reloads after them in WipEout's code. So the order was: locals and the
register moves first, then single-precision arithmetic, loads and stores, duplication, and then the rest, each family
as vector code.

### Synthetic corpus

| | Functions | Guest | x64 before | x64 after | | arm64 before | arm64 after | | native before | native after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 189 | 2659 | 11.36 | 9.21 | -18.9% | 9.12 | 7.06 | -22.6% | 6.66 | 4.61 | -30.7% |
| Module `syn_calls` | 31 | 377 | 5.34 | 4.99 | -6.5% | 4.25 | 3.73 | -12.3% | 4.25 | 3.73 | -12.3% |
| Module `syn_control` | 16 | 134 | 7.01 | 7.01 | +0.0% | 5.27 | 5.27 | +0.0% | 5.27 | 5.27 | +0.0% |
| Module `syn_flags` | 19 | 146 | 6.97 | 6.97 | +0.0% | 5.40 | 5.40 | +0.0% | 5.24 | 5.24 | +0.0% |
| Module `syn_integer` | 18 | 605 | 4.38 | 3.93 | -10.2% | 2.75 | 2.46 | -10.7% | 2.75 | 2.46 | -10.7% |
| Module `syn_integer_arm` | 6 | 151 | 5.34 | 5.29 | -1.1% | 3.38 | 3.35 | -0.8% | 3.38 | 3.35 | -0.8% |
| Module `syn_neon` | 13 | 194 | 22.61 | 11.06 | -51.1% | 15.45 | 5.32 | -65.6% | 15.63 | 5.67 | -63.7% |
| Module `syn_neon_locals` | 14 | 201 | 34.00 | 18.91 | -44.4% | 27.12 | 11.53 | -57.5% | 26.41 | 11.16 | -57.7% |
| Module `syn_regs` | 44 | 291 | 8.49 | 8.49 | +0.0% | 7.14 | 7.14 | +0.0% | 7.17 | 7.17 | +0.0% |
| Module `syn_vfp` | 12 | 264 | 14.18 | 14.12 | -0.4% | 12.87 | 12.86 | -0.1% | 2.58 | 2.52 | -2.6% |
| Module `syn_vfp_locals` | 16 | 296 | 18.05 | 17.97 | -0.4% | 17.02 | 17.05 | +0.1% | 4.52 | 4.41 | -2.5% |
| Class integer | 99 | 1076 | 5.02 | 5.00 | -0.3% | 3.77 | 3.76 | -0.2% | 3.77 | 3.76 | -0.2% |
| Class vfp | 32 | 585 | 16.20 | 16.05 | -0.9% | 14.98 | 14.92 | -0.4% | 3.95 | 3.79 | -4.0% |
| Class neon | 32 | 733 | 17.74 | 10.08 | -43.2% | 12.99 | 5.57 | -57.1% | 12.87 | 5.59 | -56.5% |
| Class call-heavy | 26 | 265 | 8.76 | 8.76 | +0.0% | 7.17 | 7.17 | +0.0% | 7.17 | 7.17 | +0.0% |

Out-of-line helpers went from 4366 to 2490 instructions on x64, from 3340 to 1826 on arm64, and from 1653 to 44 with
native floating point. Before, the out-of-line part held guest_simd.h's per-lane loops. Now it holds only the VFP
operations' exact fallbacks. The integer and call modules move because clang, compiling the corpus' C for the
Cortex-A9, vectorised some of their loops (`popcount_words` uses `vcnt`, `mac64` uses `vmlal.s32`, and `stack_args`
copies with `vld1`/`vst1`).

Per guest instruction (`build/t6/ops`: one function per row repeats the instruction 8 times, minus an empty function;
a second instruction in a row keeps the chain dependent). The first table is the static count over every path. The
second is the fall-through path that the compiler laid out as the likely one (`build/t6/ops/hot_ops.py`). Before,
several helpers were out-of-line loops, so their fall-through path is only the call. Those "before" cells
(`vrsqrte`/`vrsqrts`, `vqadd`, `vqdmulh`, `vclz`, `vzip`, `vtbl` on x64) understate what the code did.

| Guest instructions (8 times each) | x64 before | x64 after | arm64 before | arm64 after | native before | native after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `vorr q0, q1, q1; vorr q1, q0, q0` | 2.0 | 0.1 | 2.0 | 0.1 | 2.0 | 0.1 |
| `vmul.f32 q0, q0, q1` | 58.4 | 48.4 | 47.4 | 32.0 | 49.4 | 33.8 |
| `vmla.f32 q0, q1, q2` | 91.8 | 49.4 | 64.4 | 33.0 | 66.4 | 35.8 |
| `vmla.f32 q0, q1, d4[1]` | 92.8 | 49.1 | 66.1 | 33.2 | 68.1 | 39.0 |
| `vadd.f32 d0, d0, d1` | 59.2 | 17.1 | 47.4 | 7.5 | 49.4 | 9.1 |
| `vmul.f32 d0, d0, d1` | 59.2 | 36.8 | 47.4 | 32.0 | 49.4 | 34.6 |
| `vsub.f32 q0, q0, q1` | 58.5 | 15.6 | 47.4 | 7.5 | 49.4 | 9.1 |
| `vpadd.f32 d0, d0, d1` | 71.5 | 30.4 | 53.2 | 14.8 | 53.2 | 16.4 |
| `vcgt.f32 d0, d0, d1` | 19.0 | 4.6 | 14.6 | 2.2 | 14.6 | 3.9 |
| `vmax.f32 q0, q0, q1` | 109.9 | 22.6 | 79.6 | 11.9 | 79.6 | 13.5 |
| `vrsqrte.f32 q0, q0` | 325.5 | 70.6 | 85.0 | 7.1 | 85.0 | 8.8 |
| `vrsqrts.f32 q0, q0, q1` | 128.2 | 27.5 | 103.9 | 15.9 | 103.9 | 15.9 |
| `vrecpe.f32 d0, d0` | 72.1 | 52.4 | 59.1 | 7.1 | 59.1 | 8.8 |
| `vcvt.s32.f32 d0, d0; vcvt.f32.s32 d0, d0` | 23.8 | 3.3 | 24.4 | 1.2 | 24.4 | 1.2 |
| `vdup.32 q0, d2[1]; vdup.32 q1, d0[0]` | 0.6 | 0.2 | 0.2 | 0.2 | 0.2 | 0.2 |
| `vbsl q0, q1, q2` | 4.4 | 4.4 | 1.5 | 1.5 | 1.5 | 1.5 |
| `vext.32 d0, d0, d1, #1` | 3.6 | 0.4 | 3.5 | 0.5 | 3.5 | 0.5 |
| `vzip.8 q0, q1` | 54.8 | 8.1 | 41.2 | 1.4 | 41.2 | 1.4 |
| `vtbl.8 d0, {d1, d2}, d0` | 82.2 | 41.0 | 74.5 | 3.9 | 74.5 | 3.9 |
| `vadd.i32 q0, q0, q1` | 2.5 | 0.6 | 2.1 | 0.6 | 2.1 | 0.6 |
| `vqadd.s16 q0, q0, q1` | 146.0 | 11.9 | 45.6 | 7.8 | 45.6 | 7.8 |
| `vshl.s16 q0, q0, q1` (by register) | 231.4 | 36.2 | 201.4 | 7.0 | 201.4 | 7.0 |
| `vshr.u16 q0, q0, #3; vadd.i16 q0, q0, q1` | 8.7 | 1.2 | 8.4 | 1.1 | 8.4 | 1.1 |
| `vmull.s16 q0, d2, d3; vmovn.i32 d2, q0` | 5.4 | 1.1 | 3.2 | 0.7 | 3.2 | 0.7 |
| `vqmovn.s32 d0, q1; vmovl.s16 q1, d0` | 5.3 | 1.8 | 5.1 | 1.6 | 5.1 | 1.6 |
| `vqdmulh.s16 q0, q0, q1` | 208.2 | 25.5 | 44.5 | 13.0 | 44.5 | 13.0 |
| `vabd.u8 q0, q0, q1` | 43.0 | 4.4 | 32.0 | 1.4 | 32.0 | 1.4 |
| `vpaddl.u8 q0, q0` | 24.0 | 4.4 | 16.2 | 4.1 | 16.2 | 4.1 |
| `vcnt.8 d0, d0` | 82.1 | 20.6 | 38.1 | 1.4 | 38.1 | 1.4 |
| `vclz.i32 q0, q0` | 126.8 | 40.9 | 37.0 | 0.4 | 37.0 | 0.4 |
| `vld1.8 {d0, d1}, [r0]!; veor q1, q1, q0` | 5.6 | 1.9 | 4.9 | 1.9 | 4.9 | 1.9 |
| `vst1.8 {d2, d3}, [r0]!; vadd.i8 q1, q1, q2` | 20.9 | 1.9 | 16.9 | 1.9 | 16.9 | 1.9 |
| `vld1.32 {d0[]}, [r0]!; vadd.i32 d1, d1, d0` | 7.3 | 2.6 | 4.3 | 2.1 | 4.3 | 2.1 |
| `vst1.32 {d0[1]}, [r0]!; vadd.i32 d0, d0, d1` | 3.3 | 2.9 | 2.5 | 2.4 | 2.5 | 2.4 |
| `vld2.16 {d0, d1}, [r0]!; vadd.i16 q1, q1, q0` | 29.6 | 5.9 | 24.8 | 2.6 | 24.8 | 2.6 |
| `vld4.8 {d0, d1, d2, d3}, [r0]!; vadd.i8 q2, q2, q0` | 66.7 | 19.6 | 63.1 | 3.8 | 63.1 | 3.8 |
| `vst3.8 {d0, d1, d2}, [r0]!; vadd.i8 d0, d0, d3` | 57.1 | 17.3 | 51.9 | 5.2 | 51.9 | 5.2 |

Fall-through path:

| Guest instructions (8 times each) | x64 before | x64 after | arm64 before | arm64 after | native after |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vmul.f32 q0, q0, q1` | 40.6 | 23.2 | 31.6 | 14.1 | 14.6 |
| `vmla.f32 q0, q1, q2` | 61.5 | 25.6 | 40.8 | 14.8 | 15.2 |
| `vadd.f32 d0, d0, d1` | 41.5 | 16.5 | 31.6 | 7.4 | 8.0 |
| `vsub.f32 q0, q0, q1` | 40.8 | 15.1 | 31.6 | 7.4 | 8.0 |
| `vpadd.f32 d0, d0, d1` | 58.2 | 29.5 | 41.6 | 14.6 | 15.2 |
| `vcgt.f32 d0, d0, d1` | 18.4 | 4.4 | 14.8 | 2.1 | 2.8 |
| `vmax.f32 q0, q0, q1` | 99.8 | 22.1 | 71.8 | 11.8 | 12.4 |
| `vrsqrte.f32 q0, q0` | 244.8 | 69.2 | 6.1 | 7.0 | 7.6 |
| `vrsqrts.f32 q0, q0, q1` | 16.5 | 26.5 | 12.1 | 15.8 | 15.8 |
| `vrecpe.f32 d0, d0` | 35.6 | 51.0 | 25.2 | 7.0 | 7.6 |
| `vcvt.s32.f32 d0, d0; vcvt.f32.s32 d0, d0` | 22.2 | 3.3 | 21.2 | 1.1 | 1.1 |
| `vzip.8 q0, q1` | 6.2 | 7.6 | 41.1 | 1.5 | 1.5 |
| `vtbl.8 d0, {d1, d2}, d0` | 27.9 | 40.2 | 42.8 | 3.8 | 3.8 |
| `vqadd.s16 q0, q0, q1` | 81.1 | 10.9 | 4.8 | 7.9 | 7.9 |
| `vshl.s16 q0, q0, q1` | 51.8 | 35.5 | 46.0 | 6.9 | 6.9 |
| `vqdmulh.s16 q0, q0, q1` | 193.0 | 24.5 | 5.1 | 13.1 | 13.1 |
| `vabd.u8 q0, q0, q1` | 41.8 | 4.4 | 32.0 | 1.2 | 1.2 |
| `vcnt.8 d0, d0` | 81.4 | 20.1 | 38.2 | 1.2 | 1.2 |
| `vclz.i32 q0, q0` | 15.6 | 40.5 | 37.1 | 0.2 | 0.2 |
| `vst1.8 {d2, d3}, [r0]!; vadd.i8 q1, q1, q2` | 20.3 | 1.9 | 16.8 | 2.0 | 2.0 |
| `vld4.8 {d0, d1, d2, d3}, [r0]!; vadd.i8 q2, q2, q0` | 66.2 | 19.1 | 63.1 | 3.9 | 3.9 |

Moves, `vneg`, `vrev`, `vtrn`, `vdup` and the logic operations fold into their neighbours in both versions (0.0 to
1.0 per instruction). Weighted by WipEout's 24 most frequent forms (37819 of its NEON instructions), the static count
per NEON instruction goes from 30.2 to 14.5 on x64 and from 21.5 to 9.0 on arm64.

Single precision is the expensive part. Each operation flushes denormal input lanes (a compare of the absolute value
with 2^-126 and a mask, skipped for operands that an earlier NEON float operation in the same straight stretch already
produced) and fixes the result (NaN lanes to the default NaN, lanes below 2^-126 to a signed zero). A multiply also
tests for a product that rounds to exactly 2^-126, where flush-to-zero before rounding differs from the host's after
rounding. That test is about 4 inline instructions, plus a never-taken branch of about 15 instructions on x64 and 8 on
arm64 that the static count includes (`vmul.f32 d` 36.8 vs `vadd.f32 d` 17.1 on x64). Native floating point does not
lower the static count on its own: unless the guest's FPSCR is known, each operation holds both the host instruction
(taken when FZ and DN are set) and the checked code (taken otherwise). The estimates are FRECPE/FRSQRTE (and
URECPE/URSQRTE) on arm64 and about 50 to 70 instructions of exact integer formulas on x64. Conversions are
FCVTZS/FCVTZU on arm64 and cvttps2dq corrected for out-of-range lanes on x64.

What the code does in time (x64 only). `build/t4/bench2.py` runs the corpus' functions through the differential test's
harness, both programs alternately, 6 rounds each, pinned to one logical CPU at high priority. This was done twice, on
two CPUs, while this session ran nothing else. The geometric means:

| | Functions | CPU 2: before | after | | CPU 8: before | after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| All | 136 | 40.92 ns | 32.40 ns | -20.8% | 41.48 ns | 33.09 ns | -20.2% |
| Class integer | 55 | 23.49 | 23.04 | -1.9% | 23.70 | 23.64 | -0.2% |
| Class call-heavy | 25 | 24.24 | 24.01 | -0.9% | 24.71 | 24.61 | -0.4% |
| Class vfp | 27 | 67.08 | 65.64 | -2.2% | 68.06 | 66.42 | -2.4% |
| Class neon | 29 | 116.24 | 41.49 | -64.3% | 118.13 | 42.24 | -64.2% |

The integer and call-heavy classes, whose code barely changed, moved by -1.9% to -0.2%, which is the noise here.
Single unchanged functions moved by up to about +13% (`stack_args` 32 to 36-37 ns, `cvt_all` 52 to 57-58 ns). NEON
functions, CPU 2 (CPU 8 within 3%): `neon_mat4` 4813 -> 946 ns, `nl_mat4x4` 2771 -> 477, `nl_normalize` 1780 -> 594,
`neon_add_f32` 552 -> 46, `neon_complex` 507 -> 39, `popcount_words` 613 -> 49, `nl_color` 342 -> 92, `neon_fixed`
328 -> 34, `nl_calls` 274 -> 60, `nl_alias` 142 -> 29, `neon_threshold` 94 -> 17, `nl_copy` 55 -> 30, `mixed_neon`
57 -> 38. Slower: `mac64` 31 -> 36 ns. Its loop's `vmlal.s32` has no SSE2 instruction (PMULDQ is SSE4.1), so clang
builds each 64-bit product from three PMULUDQ and sign corrections, where 10acc8b's per-lane loop used scalar
multiplies (KNOWN_ISSUES.md). `neon_permute` 10.7 -> 11.6 ns.

### WipEout 2048

Regenerated with this generator into this worktree's game build (`build/wo-game`). "Before" is 10acc8b's tree.

| | Functions | Guest | x64 before | x64 after | | arm64 before | arm64 after | | native before | native after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16658 | 1479360 | 4.00 | 3.56 | -11.2% | 3.53 | 3.17 | -10.2% | 3.11 | 2.74 | -11.8% |
| Module `wo_eboot` | 14792 | 1350786 | 4.03 | 3.54 | -12.2% | 3.56 | 3.16 | -11.1% | 3.15 | 2.74 | -12.8% |
| Module `wo_libc` | 1201 | 65672 | 4.44 | 4.43 | -0.1% | 3.83 | 3.83 | +0.2% | 2.89 | 2.89 | -0.1% |
| Module `wo_libfios2` | 520 | 39042 | 3.04 | 3.04 | +0.0% | 2.62 | 2.62 | +0.0% | 2.62 | 2.62 | +0.0% |
| Module `wo_libult` | 145 | 23860 | 2.80 | 2.80 | +0.0% | 2.64 | 2.64 | +0.0% | 2.64 | 2.64 | +0.0% |
| Class integer | 10290 | 757832 | 2.83 | 2.82 | -0.4% | 2.51 | 2.50 | -0.4% | 2.48 | 2.47 | -0.3% |
| Class vfp | 1444 | 224442 | 5.61 | 5.24 | -6.6% | 5.21 | 5.00 | -4.1% | 3.00 | 2.67 | -11.1% |
| Class neon | 663 | 122161 | 12.56 | 7.87 | -37.4% | 9.93 | 6.00 | -39.6% | 9.16 | 5.35 | -41.6% |
| Class call-heavy | 4261 | 374925 | 2.63 | 2.62 | -0.0% | 2.51 | 2.51 | +0.0% | 2.49 | 2.49 | +0.0% |
| `f_8311A9C8` (integer) | 1 | 383 | 3.52 | 3.52 | +0.0% | 2.42 | 2.42 | +0.0% | 2.42 | 2.42 | +0.0% |
| `f_8128DB48` (neon) | 1 | 443 | 9.70 | 6.70 | -31.0% | 8.45 | 5.18 | -38.7% | 7.32 | 4.06 | -44.5% |
| `f_813BAC40` (vfp) | 1 | 408 | 5.41 | 4.83 | -10.9% | 4.16 | 3.56 | -14.4% | 3.73 | 3.18 | -14.6% |
| `f_812FC59A` (neon) | 1 | 1125 | 9.66 | 7.63 | -21.0% | 8.71 | 5.61 | -35.6% | 7.22 | 4.04 | -44.0% |
| `f_812FC39E` (neon) | 1 | 117 | 43.48 | 24.48 | -43.7% | 32.40 | 17.02 | -47.5% | 33.29 | 18.71 | -43.8% |
| `f_81276E14` (neon) | 1 | 112 | 20.48 | 12.14 | -40.7% | 14.80 | 7.48 | -49.5% | 14.71 | 7.69 | -47.7% |
| `f_8127A712` (neon) | 1 | 123 | 17.41 | 11.44 | -34.3% | 12.41 | 8.20 | -33.9% | 10.98 | 6.67 | -39.3% |
| `f_8118C300` (neon) | 1 | 716 | 14.82 | 8.33 | -43.8% | 12.09 | 6.40 | -47.0% | 11.45 | 7.13 | -37.8% |
| `f_813C32DC` (integer) | 1 | 31 | 4.84 | 4.84 | +0.0% | 3.65 | 3.65 | +0.0% | 3.65 | 3.65 | +0.0% |
| `f_812A7EDC` (neon) | 1 | 475 | 6.91 | 6.25 | -9.6% | 6.21 | 5.74 | -7.7% | 3.92 | 2.90 | -25.8% |

Out-of-line helpers went from 68062 to 30732 instructions on x64, from 53687 to 23513 on arm64, and from 35843 to 707
native. The NEON class drops by 37-42%. The executable drops by 11-13%. The VFP class drops by 4-11% because the
float functions with a few NEON instructions no longer sync their locals around them. Of the hot ten, the eight NEON
and VFP functions shrink by 8-50%. In `f_8118C300` (a 716-instruction mix of vmul/vmla.f32 on Q registers, vbif and
compares) the arm64 code still spends most of its instructions on the checked path's masks: of 4591 instructions,
554 are BIC, 237 FACGT, 213 FMUL, 203 DUP and 244 AND.

What psvrecomp reports for `wo_eboot` (before is task 5's psvrecomp, from that section):

| | Functions with VFP locals | D registers as two floats / as a double | Read at entry | Store places | Reloads (after Advanced SIMD) | Calls keeping a live local | Advanced SIMD on values / in the context form | Q registers as one vector |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| before | 2482 | 8952 / 1995 | 11251 | 14058 | 15156 (9174) | 6160 | 0 / 50590 | 0 |
| after | 2625 | 6158 / 3316 | 11920 | 11490 | 10004 (0) | 8046 | 50590 / 0 | 3942 |

`wo_libc` translates 454 NEON instructions on values (8 Q registers held as one vector). In every module of WipEout
and P4D, every NEON instruction is translated on values. `psvrecomp --neon-in-context` writes 10acc8b's code byte for
byte (WipEout's four modules compared). The generated source grows from 256.9 MB to 258.1 MB, and the game's
executable shrinks from 26.9 MB to 24.0 MB.

`f_813BAC40` grew by 4.8% (x64) and 24% (arm64) in task 5. The cause, from objects compiled with and without inlining
at each step: most of its code is clang inlining its same-file callee `f_813BB144` (5 call sites) once that callee
became small enough (task 3 on x64, task 5 on arm64). Loop peeling in task 4 duplicated three of the inlined sites.
Task 4's full register flush at each of its 9 calls added 213 instructions, and task 5 added the VFP write-backs at
calls and the FZ copy of each typed operation with its two cold calls. Without inlining it is 1687 / 1624 / 1642 /
1773 x64 instructions and 1481 / 1433 / 1367 / 1485 arm64 instructions at tasks 2 / 3 / 4 / 5. So the metric counts
the callee's inlined body in the caller (`/Ob2`). The fixes belong to the per-callee live-in summaries (calls) and the
typed operations' FZ duplication, not this change. Here it shrinks to 4.83 (x64) and 3.56 (arm64).

### Persona 4 Dancing All Night

Regenerated into `build/p4d-game`. "Before" is 10acc8b's tree. No native columns.

| | Functions | Guest | x64 before | x64 after | | arm64 before | arm64 after | |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **All** | 16704 | 1164693 | 5.33 | 5.10 | -4.5% | 5.04 | 4.81 | -4.4% |
| Module `p4d_eboot` | 12440 | 741511 | 5.40 | 5.08 | -5.9% | 4.95 | 4.64 | -6.1% |
| Module `p4d_libc` | 1554 | 83418 | 4.34 | 4.35 | +0.1% | 3.79 | 3.80 | +0.1% |
| Module `p4d_libface` | 451 | 29920 | 6.80 | 6.58 | -3.2% | 6.54 | 6.32 | -3.4% |
| Module `p4d_libfios2` | 577 | 47768 | 3.26 | 3.26 | +0.0% | 2.81 | 2.81 | +0.0% |
| Module `p4d_libsmart` | 1522 | 241328 | 5.88 | 5.73 | -2.5% | 6.16 | 6.05 | -1.9% |
| Module `p4d_libult` | 160 | 20748 | 3.32 | 3.32 | +0.0% | 3.02 | 3.02 | +0.0% |
| Class integer | 9093 | 568416 | 3.27 | 3.27 | -0.0% | 2.82 | 2.82 | -0.0% |
| Class vfp | 2723 | 421903 | 7.57 | 7.37 | -2.7% | 7.71 | 7.52 | -2.6% |
| Class neon | 147 | 28863 | 18.04 | 11.43 | -36.6% | 14.62 | 8.52 | -41.8% |
| Class call-heavy | 4741 | 145511 | 4.38 | 4.38 | +0.0% | 4.03 | 4.03 | +0.0% |

Out-of-line helpers went from 49685 to 33363 instructions on x64 and from 38276 to 25234 on arm64. psvrecomp: the
executable has 16517 NEON instructions on values and 1228 Q registers held as one vector, libsmart has 2824 and 122,
libface 468 and 34, and libc 338 and 3. The executable is 20.96 MB before and 19.88 MB after.

### Compile time

Compile time for one file, the one holding the hot NEON functions (`wo_eboot_code_49.c`; `_41.c` in parentheses), on
one core:

- x64 `/O2`: 5.2 -> 4.1 s (4.9 -> 3.8 s).
- x64 `/Od`: 10.4 -> 2.2 s (9.9 -> 2.4 s), objects 20.0 -> 2.4 MB (19.0 -> 2.7 MB). Before, every NEON instruction
  expanded inline into lane loops over the context.
- arm64 `-O2`: 7.0 -> 4.9 s; native 6.5 -> 5.0 s.
- arm64 `-O0`: 25.1 -> 2.8 s (objects 22.8 -> 2.0 MB).

With `PSVRECORE_GENERATED_CODE_ARCH` (helpers compiled alone, instructions) at x86-64-v2 / v3 against plain x86-64:
`vtbl` 50 -> 27 / 24 (PSHUFB), `vqdmull` 70 -> 38 / 26, a shift by register 150 -> 126 / 65, `vmull` 7 -> 4, the
narrowing forms 31 -> 18 / 16, `vclz` 43 -> 43 / 34, `vrsqrte` (integer) 42 -> 26 / 29. Single precision hardly
changes, because v3's FMA is not used for these unfused operations. Plain x86-64 objects contain no instruction later
than SSE2 (PMAXUB and PMINUB are SSE2).

### Checks

- Differential: fuzzgen's Advanced SIMD corpus (`psvrecore_fuzz_simd_code`, 8000 blocks of 1-3 NEON instructions,
  96000 runs from states with lanes at their edges) gave 0 disagreements. The main fuzz corpus (2000 blocks, 8000
  runs) gave 0. `simd_tests.cpp` gave 0 on every pair of 46 single-precision edge values through each helper (39556
  runs), every estimate index (131072 runs) and 50913 products near 2^-126. The synthetic corpus gave 0 in 6576 runs
  in each of the three configurations, with a faulting page after the corpus' memory in the production one.
- Faults put in by hand, each caught. Swapped zip lanes: 71 fuzz disagreements. No saturation in vqadd: 1047. No QC
  from vqmovn: 1333. No product test at 2^-126: 15428 edge products and 93 helper pairs. The fix threshold off by one:
  225 helper pairs and 2 fuzz runs. An integer result marked "known flushed": 9 fuzz runs (0 in the corpus). No
  write-back of a Q local before a call: 1395 corpus runs and 32119 fuzz runs. A vld3 reading 8 bytes past its
  operands: an access violation on the corpus' guard page.

### Games

Hidden runs (`--hidden`), each with its own data, settings and shots. The build from 10acc8b's generator and this one
ran back to back as Windows x64 release builds (the runtime and generated code of each built from its own tree). The
CPU time of each thread was sampled at the shot mark (`build/t6/run_game_cpu.ps1`).

- WipEout 2048, the race script, two pairs (base then new, new then base). All four runs are on lap 1/3 in 8th place
  at the 199 s shot, shield 89%. Race times are 1:47.69 and 1:47.68 (base) and 1:47.69 and 1:47.67 (new), and frames
  drawn are 11908 and 11910 (base) and 11916 and 11904 (new). The main game thread's CPU time at 199 s is 35.0 s and
  32.7 s for base and 23.2 s and 22.7 s for new (about -32%). The render thread is unchanged (44.5-46.7 s). Every run
  has the same four distinct warnings (audio port behind, an untranslated GXP program, depthf, system module 0x002d
  twice) and no traps.
- Persona 4 Dancing All Night, the fixed-time run (shots at 3-55 s), two pairs. All four runs reach the opening movie.
  The 8 s shot is identical in all four, the 55 s shot is identical in base-1 and new-1, and the others show the same
  scenes a few frames apart. Frames drawn are 1661 and 1660 (base) and 1659 and 1659 (new). The same two warnings
  appear (audio, the mask bit of DF32M_S8) and there are no traps. In this phase the game's main thread uses about
  0.5 s of CPU (the movie decoder thread about 7 s), so the run shows no CPU difference.
