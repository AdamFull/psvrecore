# psvrecore status

Each component has one of these states: **Not started** · **Implemented** · **Builds successfully** · **Tested** ·
**Integrated** · **Verified in-game**.

A state is claimed only once it has been checked. Anything unverified is listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

"Checked on SDK files" means the check ran on the development machine against plaintext binaries from a PS Vita
SDK. Those files and the outputs derived from them are not in the repository, so CI cannot repeat that check.

| Component | State | Notes |
| --- | --- | --- |
| Build system (presets, compiler options, build script) | Tested (Windows) | Windows debug and release build and pass their tests. Android: the preset configures with NDK 29.0 and nx-foundation cross-compiles. Linux runs only in CI, which has not run yet |
| nx-foundation integration | Tested (Windows) | Submodule at 5489a63, built with clang-cl. Also builds for Android arm64 |
| `core`: errors, logs, binary reader, files, text, CLI | Tested | Unit tests |
| Guest address-space reservation | Tested (Windows) | 4 GiB plus a guard region can be reserved; pages at both ends of the guest space can be committed and reached |
| `formats`: ELF, `ar` archives | Tested | Unit tests. Checked on SDK files: 34 modules and executables |
| `formats`: SELF → ELF (stored and zlib segments) | Tested | Unit tests. Checked on SDK files: program headers and every segment are byte-identical to the SDK's plain ELF of the same executable (24 MB). Compressed SELFs made by the SDK's `make_fself -c` (libult, the 24 MB executable) decompress to identical segments. Encrypted SELFs are refused |
| `formats`: module information, exports, imports | Tested | Unit tests, including both import entry sizes. Checked on SDK files: every module parses, and `module_info` in the module's own export equals the information's address |
| `formats`: SCE relocations (decode, apply, check) | Tested | Unit tests, with MOVW/MOVT encodings taken from clang's assembler. Checked on SDK files: all ~2.7 million relocations in 34 binaries match the linked contents |
| `formats`: SFO | Tested | Unit tests. Checked on SDK files: a sample's `param.sfo` |
| `formats`: NID database (vita-headers YAML, TSV, SDK stub archives) | Tested | Unit tests. Checked on SDK files: names from vita-headers and from the SDK's stub libraries agree on every NID both name, apart from 2 aliases |
| `psvinfo` tool | Tested | Exercised by the SDK-file checks above |
| `arm`: ARM, Thumb-2 (with IT blocks), VFPv3, Advanced SIMD decoder and formatter | Tested | Differential check against llvm-objdump 22.1 (`--mcpu=cortex-a9`), `scripts/arm_check.py`. **0 differences**, apart from the documented unmatchable cases, on random corpora of ~2M ARM words, every 16-bit Thumb encoding, ~2M 32-bit Thumb encodings and ~0.85M IT-block sequences, over several seeds. Checked on SDK files: all 7.1M instructions in the code of libc, libfios2, libult, libsmart, a sample and a 24 MB executable match. Unit tests for the instruction model (ARM and Thumb forms decode alike; conditions, IT, branch targets, memory operands) |
| `psvdis` tool | Tested | Decodes and formats about 2.5M instructions/s (24 MB of Thumb code in 2.4 s, release build, Core Ultra 7 155H) |
| CI workflow | Implemented | Includes the disassembler check (Windows, release). Not run yet, because the repository has not been pushed |
| `crypto`, `analysis`, `recomp`, `interp` | Not started | |
| `runtime`, `gxm`, `vk`, `platform`, `frontend` | Not started | |
