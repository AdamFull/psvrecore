#!/usr/bin/env python3
"""Builds the code generator's synthetic corpus: the C and assembly sources in tests/codegen/corpus/ compiled for the
Vita's CPU (armv7-a, Cortex-A9, VFPv3 + NEON, hard float) with LLVM's clang and ld.lld, each file linked on its own
at a fixed address, into tests/codegen/corpus_data.inc: each module's bytes, its function symbols and the
differential tests' argument specifications. The .inc is checked in, so the tests and the metrics need no ARM
toolchain; run this after changing a source.

    python scripts/codegen_corpus.py            # rewrite tests/codegen/corpus_data.inc
    python scripts/codegen_corpus.py --check    # fail if it is not what the sources give
    python scripts/codegen_corpus.py --llvm "C:/Program Files/LLVM/bin"

Modules: one per source file, `<name>.c` (Thumb-2), `<name>_arm.c` (ARM) or `<name>.s` (its own .thumb/.arm), at
0x82000000 + 0x8000 * index in sorted file order, at most 32 KiB each, code and read-only data only (no .data or
.bss). Functions call only functions of their own file.

Test specifications: a comment line `@test <function> <reg>=<kind> ...` in the source names a function the
differential test (tests/codegen/codegen_tests.cpp) runs from random states, and how the arguments are drawn:
  rN=any          random (the default for every register not named; s/d registers are always random)
  rN=int:LO:HI    an integer in [LO, HI] (loop counts: every test must terminate)
  rN=ptr[:A]      a pointer into a 4 KiB region of random bytes of its own, aligned to A bytes (default: any byte)
  rN=fptr         the same, the region filled with interesting binary32 values, 4-byte aligned
  rN=dptr         the same with binary64 values, 8-byte aligned
  sN=any, dN=any  (documentation only: floating-point registers are always random)
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / "tests" / "codegen" / "corpus"
OUTPUT = ROOT / "tests" / "codegen" / "corpus_data.inc"
BASE = 0x82000000
STRIDE = 0x8000

CFLAGS = ["--target=armv7a-none-eabihf", "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=hard", "-O2", "-ffreestanding",
          "-fno-builtin", "-fno-math-errno", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fno-exceptions",
          "-fomit-frame-pointer", "-ffp-contract=off", "-fno-pic", "-fno-stack-protector"]
TEST_RE = re.compile(r"@test\s+(\w+)((?:\s+[rsd]\d+=[\w:]+)*)")
SPEC_RE = re.compile(r"^(r\d+)=(any|int:-?\d+:-?\d+|ptr(?::\d+)?|fptr|dptr)$|^[sd]\d+=any$")


def find_tool(name, llvm):
    exe = name + (".exe" if os.name == "nt" else "")
    dirs = [Path(llvm)] if llvm else []
    if os.environ.get("LLVM_DIR"):
        dirs.append(Path(os.environ["LLVM_DIR"]))
    dirs.append(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM" / "bin")
    for d in dirs:
        if (d / exe).exists():
            return str(d / exe)
    found = shutil.which(name)
    if found:
        return found
    sdk = Path(os.environ.get("ANDROID_HOME", "C:/Android/Sdk"))
    for ndk in sorted((sdk / "ndk").glob("*"), reverse=True):
        for d in (ndk / "toolchains" / "llvm" / "prebuilt").glob("*/bin"):
            if (d / exe).exists():
                return str(d / exe)
    raise SystemExit(f"{name} not found: give --llvm <folder of clang and ld.lld>")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"failed: {' '.join(cmd)}")
    return r.stdout


def read_elf(path):
    """The one loadable segment (address, bytes) and the function symbols {name: code address} of a linked ELF."""
    data = Path(path).read_bytes()
    assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "a 32-bit little-endian ELF"
    e_phoff, e_shoff = struct.unpack_from("<II", data, 0x1C)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", data, 0x2A)
    loads = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz, p_flags))
    if len(loads) != 1:
        raise SystemExit(f"{path}: {len(loads)} loadable segments (one expected: no .data or .bss in the corpus)")
    vaddr, offset, filesz, memsz, flags = loads[0]
    if memsz != filesz or flags & 2:
        raise SystemExit(f"{path}: a writable or zero-filled segment (no .data or .bss in the corpus)")
    sections = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    symbols = {}
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for at in range(sh[4], sh[4] + sh[5], 16):
            st_name, st_value, _, st_info, _, st_shndx = struct.unpack_from("<IIIBBH", data, at)
            if st_info & 0xF != 2 or st_shndx == 0:  # STT_FUNC, defined
                continue
            end = data.index(b"\0", strtab[4] + st_name)
            symbols[data[strtab[4] + st_name:end].decode()] = st_value
    return vaddr, data[offset:offset + filesz], symbols


def build(llvm):
    clang, lld = find_tool("clang", llvm), find_tool("ld.lld", llvm)
    version = run([clang, "--version"]).splitlines()[0].strip()
    sources = sorted(p for p in SOURCES.iterdir() if p.suffix in (".c", ".s"))
    modules, tests = [], []
    with tempfile.TemporaryDirectory() as tmp:
        for index, src in enumerate(sources):
            name = src.stem
            base = BASE + STRIDE * index
            obj, elf, script = Path(tmp) / f"{name}.o", Path(tmp) / f"{name}.elf", Path(tmp) / f"{name}.ld"
            mode = [] if src.suffix == ".s" else (["-marm"] if name.endswith("_arm") else ["-mthumb"])
            run([clang, *CFLAGS, *mode, "-Wno-unused-command-line-argument", "-c", str(src), "-o", str(obj)])
            script.write_text("PHDRS { code PT_LOAD; }\n"
                              f"SECTIONS {{ . = 0x{base:08X}; .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :code\n"
                              "  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) } }\n")
            run([lld, "-T", str(script), "-nostdlib", "-static", "-e", "0", "--no-undefined", str(obj), "-o", str(elf)])
            vaddr, code, symbols = read_elf(elf)
            if vaddr != base or len(code) > STRIDE:
                raise SystemExit(f"{src.name}: linked at 0x{vaddr:08X}, {len(code)} bytes (0x{base:08X}, at most {STRIDE})")
            modules.append((name, base, code, symbols))
            for line in src.read_text().splitlines():
                m = TEST_RE.search(line)
                if not m:
                    continue
                fn, spec = m.group(1), m.group(2).split()
                if fn not in symbols:
                    raise SystemExit(f"{src.name}: @test {fn}: no such function")
                for s in spec:
                    if not SPEC_RE.match(s):
                        raise SystemExit(f"{src.name}: @test {fn}: bad specification '{s}'")
                tests.append((name, fn, symbols[fn], " ".join(spec)))
    out = ["// Generated by scripts/codegen_corpus.py from tests/codegen/corpus/: do not edit.",
           f"// {version}", f"// {' '.join(CFLAGS)}", ""]
    for name, base, code, symbols in modules:
        out.append(f"static const uint8_t kCorpusBytes_{name}[] = {{")
        for i in range(0, len(code), 16):
            out.append("    " + " ".join(f"0x{b:02X}," for b in code[i:i + 16]))
        out.append("};")
        out.append(f"static const CorpusFunction kCorpusFunctions_{name}[] = {{")
        for sym, value in sorted(symbols.items(), key=lambda kv: kv[1]):
            out.append(f"    {{\"{sym}\", 0x{value:08X}u}},")
        out.append("};")
    out.append("")
    out.append("static const CorpusModule kCorpusModules[] = {")
    for name, base, code, symbols in modules:
        out.append(f"    {{\"{name}\", 0x{base:08X}u, kCorpusBytes_{name}, sizeof kCorpusBytes_{name}, kCorpusFunctions_{name},"
                   f" sizeof kCorpusFunctions_{name} / sizeof kCorpusFunctions_{name}[0]}},")
    out.append("};")
    out.append("")
    out.append("static const CorpusTest kCorpusTests[] = {")
    for module, fn, address, spec in tests:
        out.append(f"    {{\"{module}\", \"{fn}\", 0x{address:08X}u, \"{spec}\"}},")
    out.append("};")
    return "\n".join(out) + "\n", len(modules), len(tests)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--llvm", help="folder of clang and ld.lld (default: LLVM_DIR, Program Files/LLVM, PATH, the NDK)")
    ap.add_argument("--check", action="store_true", help="compare with the checked-in file instead of writing it")
    args = ap.parse_args()
    text, n_modules, n_tests = build(args.llvm)
    if args.check:
        current = OUTPUT.read_text() if OUTPUT.exists() else ""
        # (the compiler version line may differ between machines; the bytes must not)
        strip = lambda t: "\n".join(l for l in t.splitlines() if not l.startswith("// "))
        if strip(current) != strip(text):
            was, now = (t.splitlines()[1] if len(t.splitlines()) > 1 else "" for t in (current, text))
            why = f" (built by another compiler: {was[3:]}; this is {now[3:]})" if was != now else ""
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} is not what the sources give{why}: run scripts/codegen_corpus.py")
        print(f"{OUTPUT.relative_to(ROOT)}: up to date ({n_modules} modules, {n_tests} tests)")
        return 0
    with open(OUTPUT, "w", newline="\n") as f:
        f.write(text)
    print(f"{OUTPUT.relative_to(ROOT)}: {n_modules} modules, {n_tests} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
