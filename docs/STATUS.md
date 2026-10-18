# psvrecore status

Each component has one of these states: **Not started** · **Implemented** · **Builds successfully** · **Tested** ·
**Integrated** · **Verified in-game**.

A state is claimed only once it has been checked. Anything unverified is listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

| Component | State | Notes |
| --- | --- | --- |
| Build system (presets, compiler options, build script) | Tested (Windows) | Windows debug and release built and passed their tests locally. Android: the preset configures with NDK 29.0 and nx-foundation cross-compiles. Linux is checked only by CI, which has not run yet |
| nx-foundation integration | Tested (Windows) | Submodule at 5489a63, built with clang-cl; also builds for Android arm64 |
| `core` (errors, log categories) | Tested | Unit tests |
| Guest address-space reservation | Tested (Windows) | Reserves 4 GiB plus a guard region; pages at both ends of the guest space can be committed and reached |
| CI workflow | Implemented | Not run yet, because the repository has not been pushed |
| `formats`, `crypto`, `arm`, `analysis`, `recomp`, `interp` | Not started | |
| `runtime`, `gxm`, `vk`, `platform`, `frontend` | Not started | |
