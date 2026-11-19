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
# after a generator change: regenerate, then compare
python scripts/codegen_metrics.py --synthetic --functions all --compare build/syn-before.json
```

- Flags. The generated-code options come from `psvrecore_generated_code` in `cmake/PsvrecoreCompilerOptions.cmake`,
  read when the script runs, so a change there is measured. The rest are what the release presets give a game's
  generated code: clang-cl `/O2 /Ob1 /DNDEBUG -MD -DPSVRECORE_SHARED` (x64), and the NDK toolchain's `-O2 -DNDEBUG
  -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ffunction-sections -fdata-sections -funwind-tables`
  for `aarch64-none-linux-android29` (arm64). Debug information is left out unless `--debug-info` is given (it
  changes no code). The JSON records the exact command lines.
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

Measured 2026-11-19: x64 with clang-cl 22.1.0 (LLVM), arm64 with the NDK 29.0.14206865's clang, the flags above.
Host instructions per guest instruction (`/g`) and code bytes per guest instruction (`B/g`). The synthetic numbers
are in `docs/codegen_metrics_synthetic.json`, for `--compare`.

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
