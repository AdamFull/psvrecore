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
| `analysis`: code discovery (functions, ARM/Thumb interworking, IT blocks, jump tables, functions that never return, imported variables) | Tested | Checked on SDK files: **0 discovery errors** in libc (1604 functions), libfios2 (588), libult (160) and a 24 MB executable (55915 functions, 4.86M instructions, 677 jump tables, 79 of them sized by inference). Every imported variable's reference table in the SDK modules decodes: 29 variables, ~19.7k MOVW/MOVT pairs, 69 data words |
| `recomp`: C code generator and guest ABI header (integer, VFPv3, Advanced SIMD) | Tested | Differential tests against the reference interpreter and same-source probes (below). Checked on SDK files: libc, libfios2 and libult generate and compile with clang -O2. The only untranslated instructions are genuine `bkpt` (3, 70 and 604). The 24 MB executable generates in 19 s with 82 `bkpt`, 3 `mcrr` and 3 `mrc p15` untranslated; its C has not been compiled yet |
| `interp`: reference interpreter (tests only) | Tested | Written from the ARM ARM pseudocode, independently of the generator. It shares only the decoder and the register layout, and has its own integer soft float with every rounding mode. Unit test vectors for integer, VFP and Advanced SIMD instructions |
| Generator vs interpreter on random code | Tested | `psvrecore_tests` generates random ARM and Thumb blocks (integer, VFP, Advanced SIMD). It runs each block as generated C and in the interpreter from the same random state, then compares registers, flags and memory. **0 disagreements** on the CI corpus (2000 blocks) and on 20000-block corpora with two other seeds |
| Same-source probes (`scripts/probe_check.py`) | Tested (local, needs the SDK) | C probes for integer arithmetic, floating point, control flow and libc calls. Each is built with the SDK's compiler, recompiled together with the SDK's libc, and compared with the same source built for the host. At -O0 and at -O2, **4 of 4 agree word for word** (~30k output words). Not in CI, because it needs the SDK |
| `psvrecomp` tool | Tested | `analyze` and `generate`, exercised by the checks above |
| `runtime`: guest memory (blocks, protection, fault reports) | Tested (Windows) | Unit tests. Faults in guest memory are reported with the guest address and thread (seen while developing; no automated test, since the process then ends) |
| `runtime`: kernel (threads, scheduling, semaphores, event flags, mutexes, condition variables, lightweight mutexes and condition variables, callbacks, signals, delays, TLS) | Tested (Windows) | 19 unit tests, through the system library functions as recompiled code calls them, including scheduling order on one CPU, no time slicing, three CPUs at once, timeouts and deletion under waiters. 10 runs in a row without a failure |
| `runtime`: files (mounted devices, descriptors, directories, stat, tty) | Tested (Windows) | Unit tests on a temporary folder: case-insensitive names, `..` kept inside the device, read-only devices |
| `runtime`: loader and process start-up | Tested (Windows, local, needs the SDK) | Boots SDK-built programs with the SDK's libc and libfios2 (below) |
| `runtime`: system libraries (thread manager, memory blocks, files, process manager, libc support, sceClib, module loading, RTC, libfios2 overlays) | Tested (Windows) | Unit tests for the calling-convention adapter, formatted output, memory blocks and files; the programs below |
| Test programs (`scripts/program_check.py`) | Tested (Windows, local, needs the SDK) | Four SDK-built programs (threads and synchronisation, files through libc and the kernel, the libc heap and memory blocks, standard streams) recompiled with libc and libfios2 and booted as a game is: **4 of 4 print exactly what is expected and exit with 0**: built at -O2 (10 runs each) and at -O0 |
| `usse`: GXP container (header, primary and secondary programs) | Tested | Unit test. Checked on SDK files: every GXP below parses |
| `usse`: USSE decoder and formatter, `psvusse` tool | Tested (local, needs the SDK) | Differential check against the SDK's disassembler (`psp2shaderperf -disasm`), `scripts/usse_check.py`: **identical listings** for 181 of 181 programs of the SDK's sample shaders, 705 of 705 programs compiled from the SDK's Cg sources with its compiler (vertex and fragment), and 1874 of 1874 programs of generated shaders. Unit tests for every family the compiler emits, repeats, branch targets and the constant table. Random words that the compiler never emits are not all matched (see [KNOWN_ISSUES.md](KNOWN_ISSUES.md)) |
| `crypto` | Not started | |
| `gxm`, `vk`, `platform`, `frontend` | Not started | |
