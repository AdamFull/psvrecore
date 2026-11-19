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
