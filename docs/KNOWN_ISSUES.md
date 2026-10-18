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
- **NID names from different sources can disagree on aliases.** For example, vita-headers names a NID
  `sceClibMemcpy_safe` where the SDK's stub calls it `sceClibMemcpy`. When several databases are loaded, the last
  one wins.
