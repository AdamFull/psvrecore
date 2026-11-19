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
neither the tests nor the metrics need an ARM toolchain. Six modules, one per source file:

| Module | What it holds |
| --- | --- |
| `integer` | loops over bytes, halfwords and words, 64-bit arithmetic, selects (IT blocks), TBB and TBH switches, a read-only table, an exclusive-access loop, bit fields |
| `integer_arm` | the same kinds of code in the ARM instruction set: conditional execution, ldm/stm, an address-table switch |
| `calls` | getters called in a loop, recursion, more than four arguments, C++-style virtual calls, callbacks, tail calls, Thumb-ARM interworking |
| `vfp` | single and double precision: loops, compares that branch, conversions, square roots, division |
| `neon` | VLD1/VST1 of every element size at any alignment, VLD2-4/VST2-4, float and integer lanes, widening, narrowing, saturation, permutes, tables, estimates, fixed-point conversion |
| `control` | hand-written: flags live at entry and returned by a callee, flags and a carry across calls, `pop {pc}` and `ldm` returns, tail branches, hand-made TBB/TBH, 4-instruction IT blocks, cbz/cbnz, conditional calls and returns in ARM code |

The same corpus is the generator's whole-function differential test (`psvrecore_codegen_tests`,
`psvrecore_codegen_masked_tests`; see [STATUS.md](STATUS.md)).

## Baseline (generator at b45df9a)

Measured 2026-11-19: x64 with clang-cl 22.1.0 (LLVM), arm64 with the NDK 29.0.14206865's clang, the flags of the
time (the release presets' and `-fwrapv -fno-strict-aliasing -ffp-contract=off -fno-fast-math -w -funwind-tables`).
Host instructions per guest instruction (`/g`) and code bytes per guest instruction (`B/g`). These synthetic numbers
are in git history's `docs/codegen_metrics_synthetic.json` (before the change below); the file now holds the
numbers [after it](#the-memory-base-in-a-local-jump-tables-on-their-index-the-generated-codes-options).

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

The synthetic numbers after them are in `docs/codegen_metrics_synthetic.json`, the baseline for the next change.

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
