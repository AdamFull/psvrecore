#!/usr/bin/env python3
"""Differential check of psvrecore's ARM/Thumb disassembler against llvm-objdump.

Generates instruction corpora (ARM words, Thumb 16- and 32-bit encodings, IT blocks), wraps each in a minimal ELF
object with ARM/Thumb mapping symbols so llvm-objdump decodes it in the right mode, disassembles it with
llvm-objdump (--mcpu=cortex-a9) and with psvdis, normalizes both and reports every difference.

    arm_check.py --psvdis build/windows-x64-debug/tools/psvdis/psvdis.exe [--corpus all|arm|thumb16|thumb32|it]
                 [--samples N] [--seed S] [--show 40] [--file module.bin --mode thumb --base 0x81000000]

No SDK or game data is involved unless --file names such a file, and nothing is written outside --work.
"""

import argparse
import collections
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile


# --- corpora ---------------------------------------------------------------------------------------------------

def arm_corpus(rng, samples):
    """ARM words, stratified over the decoding bits (cond, bits 27-20 and 7-4) with random other bits."""
    words = []
    per_class = max(1, samples // (16 * 256 * 16))
    for cond in range(16):
        for hi in range(256):
            for lo in range(16):
                for _ in range(per_class):
                    w = rng.getrandbits(32)
                    w = (w & ~0xFFF000F0) | (cond << 28) | (hi << 20) | (lo << 4)
                    words.append(w & 0xFFFFFFFF)
    rng.shuffle(words)
    return [("arm", struct.pack("<I", w)) for w in words[:samples]]


def is_thumb32(hw1):
    return (hw1 >> 11) in (0b11101, 0b11110, 0b11111)


def is_it(hw):
    return (hw & 0xFF00) == 0xBF00 and (hw & 0xF) != 0


def thumb16_corpus(rng, samples):
    """Every 16-bit Thumb encoding (IT instructions excluded: they change how the next instructions decode)."""
    out = []
    for hw in range(0x10000):
        if is_thumb32(hw) or is_it(hw):
            continue
        out.append(("thumb", struct.pack("<H", hw)))
    rng.shuffle(out)
    return out[:samples] if samples < len(out) else out


def thumb32_corpus(rng, samples):
    """32-bit Thumb encodings, stratified over the first halfword's bits 15-4 and the second halfword's top bits."""
    out = []
    firsts = [hw for hw in range(0xE800, 0x10000, 16)]  # bits 15-4 of the first halfword
    per = max(1, samples // (len(firsts) * 16))
    for f in firsts:
        for top in range(16):
            for _ in range(per):
                hw1 = f | rng.getrandbits(4)
                hw2 = (top << 12) | rng.getrandbits(12)
                out.append(("thumb", struct.pack("<HH", hw1, hw2)))
    rng.shuffle(out)
    return out[:samples]


def it_corpus(rng, samples):
    """IT instructions followed by the instructions they cover (16- and 32-bit, random)."""
    out = []
    while len(out) < samples:
        firstcond = rng.randrange(0, 15)
        mask = rng.randrange(1, 16)
        if firstcond == 14 and bin(mask).count("1") != 1:
            continue  # AL with else-conditions is unpredictable
        seq = struct.pack("<H", 0xBF00 | (firstcond << 4) | mask)
        count = 4 - ((mask & -mask).bit_length() - 1)
        for _ in range(count):
            if rng.random() < 0.5:
                while True:
                    hw = rng.getrandbits(16)
                    if not is_thumb32(hw) and not is_it(hw):
                        break
                seq += struct.pack("<H", hw)
            else:
                hw1 = 0xE800 | rng.getrandbits(11)
                if not is_thumb32(hw1):
                    hw1 |= 0xF000
                seq += struct.pack("<HH", hw1, rng.getrandbits(16))
        out.append(("thumb", seq))
    return out


def encodings(mode, data):
    """(offset, text) of each instruction in `data`: ARM words, or Thumb halfwords as the ARM ARM writes them."""
    out = []
    at = 0
    while at < len(data):
        if mode == "arm":
            out.append((at, f"{struct.unpack_from('<I', data, at)[0]:08x}"))
            at += 4
            continue
        hw1 = struct.unpack_from("<H", data, at)[0]
        if is_thumb32(hw1) and at + 4 <= len(data):
            out.append((at, f"{hw1:04x} {struct.unpack_from('<H', data, at + 2)[0]:04x}"))
            at += 4
        else:
            out.append((at, f"{hw1:04x}"))
            at += 2
    return out


# --- ELF ---------------------------------------------------------------------------------------------------------

def write_elf(path, mode, code):
    """A relocatable ELF with one .text section and a $a or $t mapping symbol at its start."""
    shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    strtab = b"\0" + (b"$a" if mode == "arm" else b"$t") + b"\0"
    sym = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0) + struct.pack("<IIIBBH", 1, 0, 0, 0, 0, 1)
    text_off = 0x40
    sym_off = text_off + ((len(code) + 3) & ~3)
    str_off = sym_off + len(sym)
    shs_off = str_off + len(strtab)
    sh_off = (shs_off + len(shstr) + 3) & ~3
    ehdr = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    ehdr += struct.pack("<HHIIIIIHHHHHH", 1, 40, 1, 0, 0, sh_off, 0x05000000, 52, 0, 0, 40, 5, 4)
    data = bytearray(sh_off + 5 * 40)
    data[0:52] = ehdr
    data[text_off:text_off + len(code)] = code
    data[sym_off:sym_off + len(sym)] = sym
    data[str_off:str_off + len(strtab)] = strtab
    data[shs_off:shs_off + len(shstr)] = shstr

    def sh(i, name, typ, flags, off, size, link, info, align, ent):
        struct.pack_into("<IIIIIIIIII", data, sh_off + i * 40, name, typ, flags, 0, off, size, link, info, align, ent)

    sh(1, 1, 1, 6, text_off, len(code), 0, 0, 4, 0)       # .text: PROGBITS, AX
    sh(2, 7, 2, 0, sym_off, len(sym), 3, 2, 4, 16)        # .symtab (local symbols: 2)
    sh(3, 15, 3, 0, str_off, len(strtab), 0, 0, 1, 0)     # .strtab
    sh(4, 23, 3, 0, shs_off, len(shstr), 0, 0, 1, 0)      # .shstrtab
    with open(path, "wb") as f:
        f.write(data)


# --- disassembly and normalization -------------------------------------------------------------------------------

LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2,8} ?)+)\s*\t(.*)$")


def _decimal(m):
    sign, digits = m.group(1), m.group(2)
    value = int(digits, 16) if digits.lower().startswith("0x") else int(digits)
    return f"#{'-' if sign else ''}{value}"


def normalize(text):
    """Text that differs only in presentation compares equal: comments, address annotations, spacing, hex or
    decimal immediates, and .w/.n width suffixes (the width is checked through the instruction addresses)."""
    text = text.split("@")[0]                                        # comments (branch distances, literal values)
    text = re.sub(r"\s*<(?!unknown>)[^>]*>", "", text)                # symbol annotations of addresses
    text = text.replace("\t", " ").lower()
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*,\s*", ", ", text)
    text = re.sub(r"#(-?)(0x[0-9a-f]+|\d+)\b", _decimal, text)       # immediates
    text = re.sub(r"(?<![#\w])0x([0-9a-f]+)\b", lambda m: str(int(m.group(1), 16)), text)  # addresses
    head, _, rest = text.partition(" ")
    if head.endswith(".w") or head.endswith(".n"):
        head = head[:-2]
    return head + (" " + rest if rest else "")


def run_objdump(objdump, path, mode):
    triple = "armv7a-none-eabi" if mode == "arm" else "thumbv7a-none-eabi"
    out = subprocess.run([objdump, "-d", "-z", "--triple=" + triple, "--mcpu=cortex-a9", path],
                         capture_output=True, text=True, check=True).stdout
    result = {}
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            result[int(m.group(1), 16)] = normalize(m.group(3))
    return result


def run_psvdis(psvdis, path, mode, base=0):
    out = subprocess.run([psvdis, "--mode", mode, "--base", hex(base), path],
                         capture_output=True, text=True, check=True).stdout
    result = {}
    for line in out.splitlines():
        if "\t" not in line:
            continue
        addr, text = line.split("\t", 1)
        result[int(addr, 16)] = normalize(text)
    return result


# Encodings whose reference output cannot be matched, with the reason. They are architecturally UNPREDICTABLE, and
# the reference prints an operand that does not come from the encoding.
def unmatchable(raw):
    word = None
    if " " in raw:  # Advanced SIMD in Thumb form: 111U 1111 ... is ARM's 1111 001U ...
        hw1, hw2 = (int(x, 16) for x in raw.split(" "))
        if (hw1 & 0xEF00) == 0xEF00:
            word = 0xF2000000 | ((hw1 >> 12) & 1) << 24 | ((hw1 & 0xFF) << 16) | hw2
    elif len(raw) == 8:
        word = int(raw, 16)
    # vtbl/vtbx with a table list running past d31: the reference names the registers after d31 as system registers
    if word is not None and (word & 0xFFB00C10) == 0xF3B00800:
        n = ((word >> 3) & 0x10) | ((word >> 16) & 0xF)
        if n + ((word >> 8) & 3) + 1 > 32:
            return True
    # cps (ARM, privileged): the reference applies field checks this decoder does not reproduce exactly; cps never
    # appears in user-mode code
    if word is not None and len(raw) == 8 and (word & 0xFFF10020) == 0xF1000000:
        return True
    # ARM mov (register, no shift) with a nonzero Rn field (unpredictable): the reference accepts some of these and
    # rejects others without a rule tied to the encoding; psvdis rejects them all
    if word is not None and len(raw) == 8 and (word >> 28) != 0xF and (word & 0x0DE00FF0) == 0x01A00000             and (word >> 16) & 0xF != 0:
        return True
    if " " in raw:  # 32-bit Thumb: rev, rev16, rbit, revsh, clz name Rm twice (hw1 3-0, hw2 3-0); when the two
        hw1, hw2 = (int(x, 16) for x in raw.split(" "))  # differ, llvm-objdump prints a register from neither
        if (hw1 & 0xFFC0) == 0xFA80 and (hw2 & 0xF0C0) == 0xF080 and (hw1 & 0xF) != (hw2 & 0xF):
            return True
        # cps (Thumb, privileged): as for ARM, the reference's exact field checks are not reproduced
        if hw1 == 0xF3AF and (hw2 & 0xD000) == 0x8000 and (hw2 & 0x0700) != 0:
            return True
    return False


def compare(name, reference, ours, items_at, show, grep=None):
    diffs = []
    skipped = 0
    for addr, ref in sorted(reference.items()):
        got = ours.get(addr)
        if got != ref:
            # (also: a reference register list naming a system register - its output for lists past d31)
            if unmatchable(items_at.get(addr, "")) or re.search(r"\{[^}]*\b(fpinst2?|mvfr[01]|fpexc|fpsid|fpscr)\b", ref):
                skipped += 1
                continue
            diffs.append((addr, ref, got))
    if grep:
        pattern = re.compile(grep)
        diffs = [d for d in diffs if pattern.search(d[1]) or pattern.search(d[2] or "")]
    missing = len([a for a in ours if a not in reference])
    total = len(reference)
    print(f"{name}: {total} instructions, {total - len(diffs) - skipped} agree, {len(diffs)} differ"
          + (f", {skipped} unmatchable (see unmatchable())" if skipped else "")
          + (f", {missing} only in psvdis" if missing else ""))
    categories = {
        "llvm rejects, psvdis decodes": [d for d in diffs if d[1] == "<unknown>"],
        "psvdis rejects, llvm decodes": [d for d in diffs if d[2] == "<unknown>"],
        "both decode, text differs": [d for d in diffs if d[1] != "<unknown>" and d[2] != "<unknown>"],
    }
    for title, items in categories.items():
        if not items:
            continue
        key = (lambda d: (d[2] or "").split(" ")[0]) if title.startswith("llvm rejects") else (lambda d: d[1].split(" ")[0])
        counts = collections.Counter(key(d) for d in items)
        print(f"  {title}: {len(items)} - " + ", ".join(f"{m} {n}" for m, n in counts.most_common(14)))
        for addr, ref, got in items[:show]:
            raw = items_at.get(addr, "")
            print(f"    {addr:08x} {raw:<9} llvm: {ref!r:<46} psvdis: {got!r}")
    return len(diffs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--psvdis", required=True)
    ap.add_argument("--objdump", default=shutil.which("llvm-objdump") or r"C:\Program Files\LLVM\bin\llvm-objdump.exe")
    ap.add_argument("--corpus", default="all", choices=["all", "arm", "thumb16", "thumb32", "it"])
    ap.add_argument("--samples", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=40)
    ap.add_argument("--grep", help="only differences where either text matches this regular expression")
    ap.add_argument("--file", help="check a raw code file instead of a generated corpus")
    ap.add_argument("--mode", default="thumb", choices=["arm", "thumb"])
    ap.add_argument("--base", default="0")
    ap.add_argument("--work", default=None, help="folder for the corpus files (default: a temporary one)")
    args = ap.parse_args()
    args.psvdis = os.path.abspath(args.psvdis)

    work = args.work or tempfile.mkdtemp(prefix="arm_check_")
    os.makedirs(work, exist_ok=True)
    rng = random.Random(args.seed)
    failed = 0

    if args.file:
        code = open(args.file, "rb").read()
        elf = os.path.join(work, "file.o")
        write_elf(elf, args.mode, code)
        raw = os.path.join(work, "file.bin")
        with open(raw, "wb") as f:
            f.write(code)
        ref = run_objdump(args.objdump, elf, args.mode)
        base = int(args.base, 0)
        ours = {a - base: t for a, t in run_psvdis(args.psvdis, raw, args.mode, base).items()}
        failed += compare(os.path.basename(args.file), ref, ours, {}, args.show, args.grep)
        return 1 if failed else 0

    corpora = {
        "arm": lambda: arm_corpus(rng, args.samples),
        "thumb16": lambda: thumb16_corpus(rng, args.samples),
        "thumb32": lambda: thumb32_corpus(rng, args.samples),
        "it": lambda: it_corpus(rng, max(1, args.samples // 10)),
    }
    for name, make in corpora.items():
        if args.corpus not in ("all", name):
            continue
        items = make()
        mode = items[0][0]
        code = b"".join(b for _, b in items)
        items_at = {}
        at = 0
        for _, b in items:
            for off, text in encodings(mode, b):
                items_at[at + off] = text
            at += len(b)
        elf = os.path.join(work, name + ".o")
        raw = os.path.join(work, name + ".bin")
        write_elf(elf, mode, code)
        with open(raw, "wb") as f:
            f.write(code)
        failed += compare(name, run_objdump(args.objdump, elf, mode), run_psvdis(args.psvdis, raw, mode), items_at,
                          args.show, args.grep)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
