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
| CI workflow | Implemented | Not run yet, because the repository has not been pushed |
| `crypto`, `arm`, `analysis`, `recomp`, `interp` | Not started | |
| `runtime`, `gxm`, `vk`, `platform`, `frontend` | Not started | |
