#!/usr/bin/env python3
"""Measures the code the generator makes: host instructions and code bytes per guest instruction, for x64 (clang-cl,
the Windows release build's flags) and arm64 (the NDK's clang, the Android release build's flags), overall, by
module, by function class and for named functions. Static counts from the compiled objects, not time.

    # the synthetic corpus (tests/codegen; no game needed): corpusgen from a build of the tests translates it
    python scripts/codegen_metrics.py --synthetic --json before.json
    # a game's generated tree (a game repository, its generated/code folder, or one module's folder)
    python scripts/codegen_metrics.py <game>/generated/code --functions wipeout-hot --json wo.json
    # only the files that hold the named functions (quicker), one target, objects of an existing build
    python scripts/codegen_metrics.py <tree> --functions f_8311A9C8 f_8128DB48 --only-function-files --targets arm64
    python scripts/codegen_metrics.py <tree> --objects-arm64 <build>/CMakeFiles/wo_eboot_code.dir/...
    # before/after: compare with an earlier run's JSON
    python scripts/codegen_metrics.py --synthetic --compare before.json

Guest instructions. The generator writes every guest instruction it translates as one block that starts with its
address and disassembly in a comment, on a line of its own at the function's top level:
    `    { /* 8100001c: adds r0, r1, #1 */`   or   `    if (fl.z) { /* 81000020: moveq r0, #0 */`
(generator.cpp, Emitter::function; the comment is not optional). A function is `void f_<ADDR>(PsvContext* ...)`
to the `}` at column 0 that ends it; its guest instructions are those comment lines between. Code two functions
share (a shared tail) is translated, and counted, in each. Placeholder functions with no code count no guest
instructions and are left out of the ratios.

Classes, from the disassembly in those comments (one class per function, first that applies):
  neon, vfp   at least 10% of its instructions floating point or Advanced SIMD; neon when more of them are Advanced
              SIMD (q registers; vld1-4/vst1-4, permutes, integer or f32 vector arithmetic on d registers, lane
              moves) than scalar floating point (s registers, f64 arithmetic, vldr/vstr, vmrs/vmsr, vcmp...)
  call-heavy  at least one call (bl, blx, conditional forms) per 8 instructions
  integer     the rest
The split of VFP from NEON is by mnemonic, data type and register kinds: approximate, but stable between runs.

Host instructions. Each object is disassembled with llvm-objdump; a function's host instructions and bytes are those
under its symbol (`f_<ADDR>`, which generated code keeps external: clang does not drop or merge them). Out-of-line
helpers from guest.h (PSV_COLD, static) are separate symbols: reported as "helpers", not in the ratios. Trailing
int3/nop padding is not counted.

Flags: the generated-code options are read from cmake/PsvrecoreCompilerOptions.cmake (psvrecore_generated_code), so
changes there are measured; the rest are the release presets' (CMAKE_BUILD_TYPE RelWithDebInfo, the NDK toolchain's
defaults, PSVRECORE_SHARED on Windows) as the build of a game passes them, without debug information (it changes no
code). x64 needs Visual Studio's environment (found with vswhere) for the C headers, arm64 an Android NDK (newest under
ANDROID_HOME/ndk or ANDROID_NDK_HOME). A target whose compiler is missing is skipped with a note.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WIPEOUT_HOT = ["f_8311A9C8", "f_8128DB48", "f_813BAC40", "f_812FC59A", "f_812FC39E", "f_81276E14", "f_8127A712",
               "f_8118C300", "f_813C32DC", "f_812A7EDC"]
FUNCTION_SETS = {"wipeout-hot": WIPEOUT_HOT}

# ---- guest side -------------------------------------------------------------------------------------------------

RE_FUNC = re.compile(r"^void (f_[0-9A-F]{8})\(PsvContext\* (?:PSV_RESTRICT |restrict )?ctx, uint32_t ret\) \{(.*)$")
RE_INSN = re.compile(r"^    (?:if \(.+\) )?\{ /\* ([0-9a-f]{8}): (.*) \*/$")
CONDS = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"}
NEON_ONLY = {
    "vld1", "vld2", "vld3", "vld4", "vst1", "vst2", "vst3", "vst4", "vdup", "vext", "vzip", "vuzp", "vtrn", "vtbl",
    "vtbx", "vrev16", "vrev32", "vrev64", "vswp", "vpadd", "vpadal", "vpaddl", "vpmax", "vpmin", "vqadd", "vqsub",
    "vqdmulh", "vqrdmulh", "vqdmull", "vqdmlal", "vqdmlsl", "vqmovn", "vqmovun", "vqshl", "vqshlu", "vqrshl", "vqshrn",
    "vqshrun", "vqrshrn", "vqrshrun", "vqabs", "vqneg", "vmovl", "vmovn", "vshl", "vshr", "vshll", "vshrn", "vsra",
    "vrshl", "vrshr", "vrshrn", "vrsra", "vsli", "vsri", "vbsl", "vbit", "vbif", "vand", "vorr", "veor", "vbic", "vorn",
    "vmvn", "vcnt", "vclz", "vcls", "vrecpe", "vrsqrte", "vrecps", "vrsqrts", "vceq", "vcge", "vcgt", "vcle", "vclt",
    "vtst", "vaba", "vabal", "vabd", "vabdl", "vaddl", "vaddw", "vaddhn", "vraddhn", "vsubl", "vsubw", "vsubhn",
    "vrsubhn", "vmlal", "vmlsl", "vmull", "vhadd", "vhsub", "vrhadd", "vmax", "vmin", "vacge", "vacgt", "vacle", "vaclt",
}
VFP_ONLY = {"vldr", "vstr", "vpush", "vpop", "vldmia", "vldmdb", "vstmia", "vstmdb", "vldm", "vstm", "vmrs", "vmsr",
            "vcmp", "vcmpe", "vsqrt", "vdiv", "vnmul", "vnmla", "vnmls"}
SHARED = {"vadd", "vsub", "vmul", "vmla", "vmls", "vneg", "vabs", "vmov", "vcvt", "vcvtr", "vcvtb", "vcvtt"}
KNOWN = NEON_ONLY | VFP_ONLY | SHARED
RE_REG = re.compile(r"\b([sdqr])(\d+)\b")


def without_condition(base):
    """vaddeq -> vadd (VFP instructions are conditional in ARM code and in IT blocks); vcge stays vcge."""
    if base in KNOWN:
        return base
    if len(base) > 3 and base[-2:] in CONDS and base[:-2] in KNOWN:
        return base[:-2]
    return base


def guest_kind(text):
    """'neon', 'vfp', 'call' or 'int' for one guest instruction's disassembly (llvm-objdump syntax)."""
    parts = text.split(None, 1)
    if not parts:
        return "int"
    mnem = parts[0].lower()
    ops = parts[1].lower() if len(parts) > 1 else ""
    base = mnem.split(".")[0]
    if base in ("bl", "blx") or (base.startswith("blx") and base[3:] in CONDS) or (base.startswith("bl") and base[2:] in CONDS):
        return "call"
    if not base.startswith("v"):
        return "int"
    base = without_condition(base)
    dts = mnem.split(".")[1:]
    kinds = {k for k, _ in RE_REG.findall(ops)}
    if "q" in kinds:
        return "neon"
    if base in VFP_ONLY:
        return "vfp"
    if base in NEON_ONLY or base[:4] in ("vld1", "vld2", "vld3", "vld4", "vst1", "vst2", "vst3", "vst4"):
        return "neon"
    if "[" in ops and "d" in kinds:  # (a lane: vmov.32 r0, d0[1])
        return "neon"
    if "s" in kinds:
        return "vfp"
    if any(dt == "f64" for dt in dts):
        return "vfp"
    if base == "vmov" and "r" in kinds:
        return "vfp"  # (vmov d0, r0, r1: a transfer)
    if "d" in kinds:
        return "neon"  # (f32 or integer data on d registers, vmov d0, d1: vector operations)
    return "vfp"


def function_class(n, neon, vfp, calls):
    if n and (neon + vfp) * 10 >= n and neon + vfp:
        return "neon" if neon > vfp else "vfp"
    if calls and calls * 8 >= n:
        return "call-heavy"
    return "integer"


def scan_source(path):
    """{function: {guest, neon, vfp, calls}} of one generated .c file."""
    out = {}
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if cur is None:
                m = RE_FUNC.match(line)
                if m:
                    cur = out.setdefault(m.group(1), {"guest": 0, "neon": 0, "vfp": 0, "calls": 0})
                    if m.group(2).rstrip().endswith("}"):  # (a placeholder written on one line)
                        cur = None
                continue
            if line.startswith("}"):
                cur = None
                continue
            m = RE_INSN.match(line)
            if m:
                cur["guest"] += 1
                k = guest_kind(m.group(2))
                if k == "neon":
                    cur["neon"] += 1
                elif k == "vfp":
                    cur["vfp"] += 1
                elif k == "call":
                    cur["calls"] += 1
    return out


def find_tree(path):
    """The module folders (<prefix>_<module>, holding *_code_N.c) under a game, a generated/code folder or a module."""
    path = Path(path)
    for cand in (path / "generated" / "code", path):
        if cand.is_dir():
            mods = sorted(d for d in cand.iterdir() if d.is_dir() and any(d.glob("*_code_*.c")))
            if mods:
                return mods
    if any(path.glob("*_code_*.c")):
        return [path]
    raise SystemExit(f"{path}: no generated code (<prefix>_<module>/<prefix>_<module>_code_N.c) found")


# ---- host side --------------------------------------------------------------------------------------------------

def cmake_generated_flags():
    """psvrecore_generated_code's options from cmake/PsvrecoreCompilerOptions.cmake: (MSVC list, other list)."""
    text = (ROOT / "cmake" / "PsvrecoreCompilerOptions.cmake").read_text()
    m = re.search(r"function\(psvrecore_generated_code target\)(.*?)endfunction\(\)", text, re.S)
    if not m:
        raise SystemExit("cmake/PsvrecoreCompilerOptions.cmake: psvrecore_generated_code not found")
    body = m.group(1)
    branches = re.split(r"^\s*else\(\)\s*$", body, maxsplit=1, flags=re.M)
    if len(branches) != 2 or "if(MSVC)" not in branches[0]:
        raise SystemExit("psvrecore_generated_code: expected an if(MSVC) ... else() ... endif()")

    def options(chunk):
        flags = []
        for opt in re.finditer(r"target_compile_options\(\$\{target\}\s+PRIVATE(.*?)\)\s*$", chunk, re.S | re.M):
            for tok in opt.group(1).split():
                g = re.fullmatch(r"\$<\$<CONFIG:([^>]+)>:(.*)>", tok)
                if g:
                    if "RelWithDebInfo" in g.group(1).split(","):
                        flags.append(g.group(2))
                    continue
                if tok.startswith("$<"):
                    print(f"note: generator expression {tok} in psvrecore_generated_code not understood; left out")
                    continue
                flags.append(tok)
        return flags

    # (the else branch whole: its nested if()s hold options for some hosts, arm64 among them)
    return options(branches[0]), options(branches[1])


def vs_environment():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                         "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    if not vs:
        return None
    vcvars = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    env_text = subprocess.run(f'cmd /c ""{vcvars}" >nul 2>nul && set"', capture_output=True, text=True, shell=True).stdout
    env = dict(os.environ)
    for line in env_text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env


def find_ndk():
    if os.environ.get("ANDROID_NDK_HOME") and Path(os.environ["ANDROID_NDK_HOME"]).is_dir():
        return Path(os.environ["ANDROID_NDK_HOME"])
    sdk = Path(os.environ.get("ANDROID_HOME", r"C:\Android\Sdk"))
    ndks = [d for d in (sdk / "ndk").glob("*") if d.is_dir()]

    def version(d):
        try:
            return tuple(int(x) for x in d.name.split("."))
        except ValueError:
            return (0,)

    return max(ndks, key=version) if ndks else None


def llvm_tool(name):
    exe = name + (".exe" if os.name == "nt" else "")
    for d in [os.environ.get("LLVM_DIR"), str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin")]:
        if d and (Path(d) / exe).exists():
            return str(Path(d) / exe)
    return shutil.which(name)


class Target:
    def __init__(self, name, compile_cmd, env, obj_suffix, objdump, flags, compiler):
        self.name, self.compile_cmd, self.env, self.obj_suffix = name, compile_cmd, env, obj_suffix
        self.objdump, self.flags, self.compiler = objdump, flags, compiler
        self.key = hashlib.sha256(" ".join(flags).encode()).hexdigest()[:16]


def make_targets(names, debug_info, extra_x64=(), extra_arm64=()):
    msvc_flags, gnu_flags = cmake_generated_flags()
    includes = [ROOT / "libs" / "recomp" / "include", ROOT / "libs" / "core" / "include"]
    targets, notes = [], []
    objdump = llvm_tool("llvm-objdump")
    if "x64" in names:
        clang_cl = llvm_tool("clang-cl")
        env = vs_environment() if os.name == "nt" else None
        if not clang_cl or env is None or not objdump:
            notes.append("x64: skipped (needs clang-cl, llvm-objdump and Visual Studio's C environment)")
        else:
            flags = ["/nologo", "-DPSVRECORE_SHARED", "/DWIN32", "/D_WINDOWS", "/O2", "/Ob1", "/DNDEBUG", "-MD"]
            flags += ["-Zi"] if debug_info else []
            flags += msvc_flags + list(extra_x64) + ["/utf-8"] + [f"-I{p}" for p in includes]

            def cmd(src, obj, moddir, flags=flags, clang_cl=clang_cl):
                return [clang_cl, *flags, f"-I{moddir}", "-c", f"/Fo{obj}", "--", str(src)]

            targets.append(Target("x64", cmd, env, ".obj", objdump, flags, clang_cl))
    if "arm64" in names:
        ndk = find_ndk()
        bins = list((ndk / "toolchains" / "llvm" / "prebuilt").glob("*/bin")) if ndk else []
        exe = ".exe" if os.name == "nt" else ""
        clang = next((b / f"clang{exe}" for b in bins if (b / f"clang{exe}").exists()), None)
        if not clang:
            notes.append("arm64: skipped (no Android NDK: set ANDROID_NDK_HOME)")
        else:
            sysroot = clang.parent.parent / "sysroot"
            ndk_objdump = clang.parent / f"llvm-objdump{exe}"
            flags = ["--target=aarch64-none-linux-android29", f"--sysroot={sysroot}", "-DANDROID", "-fdata-sections",
                     "-ffunction-sections", "-funwind-tables", "-fstack-protector-strong", "-no-canonical-prefixes",
                     "-D_FORTIFY_SOURCE=2", "-Wformat", "-Werror=format-security", "-O2", "-DNDEBUG", "-fPIC"]
            flags += ["-g"] if debug_info else []
            flags += gnu_flags + list(extra_arm64) + ["-pthread"] + [f"-I{p}" for p in includes]

            def cmd(src, obj, moddir, flags=flags, clang=clang):
                return [str(clang), *flags, f"-I{moddir}", "-c", "-o", str(obj), str(src)]

            targets.append(Target("arm64", cmd, None, ".o", str(ndk_objdump) if ndk_objdump.exists() else objdump, flags,
                                  f"{clang} (NDK {ndk.name})"))
    return targets, notes


def up_to_date(target, src, obj):
    stamp = obj.with_suffix(obj.suffix + ".key")
    return obj.exists() and stamp.exists() and stamp.read_text() == target.key and obj.stat().st_mtime >= src.stat().st_mtime


def compile_one(target, src, obj, moddir):
    stamp = obj.with_suffix(obj.suffix + ".key")
    if up_to_date(target, src, obj):
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(target.compile_cmd(src, obj, moddir), capture_output=True, text=True, env=target.env)
    if r.returncode != 0:
        return f"{target.name}: {src.name}: compile failed\n{r.stdout}{r.stderr}"
    stamp.write_text(target.key)
    return None


RE_SYM = re.compile(r"^[0-9a-fA-F]+ <(.+)>:\s*$")
RE_LINE = re.compile(r"^\s*[0-9a-fA-F]+:\s+([0-9a-fA-F]{2}(?: [0-9a-fA-F]{2})*|[0-9a-fA-F]{8})\s+(\S+)")


def disassemble(target, obj):
    """{symbol: [instructions, bytes]} of one object."""
    out = {}
    cmd = [target.objdump, "-d", str(obj)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors="replace")
    cur = None
    pad_n = pad_b = 0
    for line in proc.stdout:
        m = RE_SYM.match(line)
        if m:
            if cur is not None:
                cur[0] -= pad_n
                cur[1] -= pad_b
            cur = out.setdefault(m.group(1), [0, 0])
            pad_n = pad_b = 0
            continue
        if cur is None:
            continue
        m = RE_LINE.match(line)
        if not m:
            continue
        nbytes = len(m.group(1).replace(" ", "")) // 2
        mnem = m.group(2)
        cur[0] += 1
        cur[1] += nbytes
        if mnem in ("int3", "nop") or mnem.startswith("nop"):
            pad_n += 1
            pad_b += nbytes
        else:
            pad_n = pad_b = 0
    if cur is not None:
        cur[0] -= pad_n
        cur[1] -= pad_b
    if proc.wait() != 0:
        raise SystemExit(f"{' '.join(cmd)} failed")
    return out


# ---- report -----------------------------------------------------------------------------------------------------

def ratio(host, guest):
    return round(host / guest, 3) if guest else None


def summarize(rows):
    g = sum(r["guest"] for r in rows)
    out = {"functions": len(rows), "guest": g}
    for t in ("x64", "arm64"):
        hs = [r for r in rows if t in r]
        if hs:
            hi = sum(r[t][0] for r in hs)
            hb = sum(r[t][1] for r in hs)
            out[t] = {"host_instructions": hi, "host_bytes": hb, "per_guest": ratio(hi, g), "bytes_per_guest": ratio(hb, g)}
    return out


def fmt_row(label, s, targets):
    cells = [f"{label:<36}", f"{s['functions']:>7}", f"{s['guest']:>9}"]
    for t in targets:
        v = s.get(t)
        cells.append(f"{v['host_instructions']:>10} {v['per_guest']:>6.2f} {v['bytes_per_guest']:>6.2f}" if v and v["per_guest"] is not None
                     else f"{'-':>10} {'-':>6} {'-':>6}")
    return " ".join(cells)


def print_table(title, summaries, targets):
    print(f"\n{title}")
    head = f"{'':<36} {'funcs':>7} {'guest':>9}"
    for t in targets:
        head += f" {t + ' insns':>10} {'/g':>6} {'B/g':>6}"
    print(head)
    for label, s in summaries:
        print(fmt_row(label, s, targets))


def compare(old, new, targets):
    print("\nComparison with the earlier run (host instructions per guest instruction, old -> new, change)")

    def rows(doc):
        r = {"overall": doc["overall"]}
        for k, v in doc.get("modules", {}).items():
            r[f"module {k}"] = v
        for k, v in doc.get("classes", {}).items():
            r[f"class {k}"] = v
        for k, v in doc.get("functions", {}).items():
            r[k] = v
        return r

    a, b = rows(old), rows(new)
    for label in b:
        if label not in a:
            continue
        cells = []
        for t in targets:
            x, y = a[label].get(t), b[label].get(t)
            if x and y and x.get("per_guest") and y.get("per_guest"):
                cells.append(f"{t} {x['per_guest']:.2f} -> {y['per_guest']:.2f} ({100 * (y['per_guest'] / x['per_guest'] - 1):+.1f}%)")
        if cells:
            print(f"  {label:<28} " + "   ".join(cells))


def run_corpusgen(corpusgen, out, per_file):
    if not corpusgen:
        for preset in ("windows-x64-release", "windows-x64-debug", "linux-x64-release", "linux-x64-debug"):
            for name in ("corpusgen.exe", "corpusgen"):
                p = ROOT / "build" / preset / "tests" / name
                if p.exists():
                    corpusgen = p
                    break
            if corpusgen:
                break
    if not corpusgen or not Path(corpusgen).exists():
        raise SystemExit("corpusgen not found: build the tests (scripts/build.ps1) or give --corpusgen")
    shutil.rmtree(out, ignore_errors=True)
    cmd = [str(corpusgen), str(out)] + ([str(per_file)] if per_file else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"corpusgen failed:\n{r.stdout}{r.stderr}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tree", nargs="?", help="a game folder, its generated/code folder, or one module's folder")
    ap.add_argument("--synthetic", action="store_true", help="measure the synthetic corpus (translated by corpusgen)")
    ap.add_argument("--corpusgen", help="corpusgen executable (default: build/<windows preset>/tests/corpusgen.exe)")
    ap.add_argument("--instructions-per-file", type=int, help="(--synthetic) corpusgen's file size")
    ap.add_argument("--targets", default="x64,arm64", help="x64, arm64 or both (default)")
    ap.add_argument("--functions", nargs="*", default=[],
                    help=f"functions to list (f_XXXXXXXX, or the synthetic corpus' source names), a set ({', '.join(FUNCTION_SETS)}), or all")
    ap.add_argument("--only-function-files", action="store_true", help="compile only the files holding the named functions")
    ap.add_argument("--objects-x64", help="use the objects of an existing build (searched by name) instead of compiling")
    ap.add_argument("--objects-arm64", help="the same for arm64")
    ap.add_argument("--binary-arm64", help="a linked ELF binary (a shared library with its symbol table) holding the "
                    "code instead of objects: for link-time optimization, which leaves no per-file objects (functions "
                    "it inlined everywhere have no symbol left; their guest instructions count where they went)")
    ap.add_argument("--work", help="where objects are compiled (default: build/codegen-metrics/<tree name>)")
    ap.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 4, 12)), help="parallel compiles")
    ap.add_argument("--debug-info", action="store_true", help="compile with the builds' debug information (-g, -Zi)")
    ap.add_argument("--flags-x64", action="append", default=[], metavar="FLAG",
                    help="another compiler option for x64 (clang-cl syntax; repeatable; give it as --flags-x64=/clang:-march=x86-64-v3)")
    ap.add_argument("--flags-arm64", action="append", default=[], metavar="FLAG",
                    help="another compiler option for arm64 (repeatable; e.g. --flags-arm64=-march=armv8.2-a)")
    ap.add_argument("--json", help="write the results here")
    ap.add_argument("--all-functions", action="store_true", help="every function's numbers in the JSON")
    ap.add_argument("--compare", help="an earlier run's JSON to compare with")
    args = ap.parse_args()

    names = [f.strip() for f in args.targets.split(",") if f.strip()]
    wanted = []
    for f in args.functions:
        wanted += FUNCTION_SETS.get(f, [f])

    if args.synthetic:
        work = Path(args.work) if args.work else ROOT / "build" / "codegen-metrics" / "synthetic"
        tree = run_corpusgen(args.corpusgen, work / "generated" / "code", args.instructions_per_file)
        label = "synthetic corpus"
    else:
        if not args.tree:
            ap.error("give a generated tree or --synthetic")
        tree = Path(args.tree)
        label = str(tree)
        work = Path(args.work) if args.work else ROOT / "build" / "codegen-metrics" / re.sub(r"[^\w.-]+", "_", tree.resolve().as_posix()[-60:])
    modules = find_tree(tree)

    # Guest side.
    files = []  # (module name, module dir, source)
    functions = {}  # name -> row
    for moddir in modules:
        for src in sorted(moddir.glob("*_code_*.c"), key=lambda p: int(re.search(r"_code_(\d+)\.c$", p.name).group(1))):
            files.append((moddir.name, moddir, src))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        scans = list(pool.map(lambda f: scan_source(f[2]), files))
    file_of = {}
    for (mod, moddir, src), scan in zip(files, scans):
        for name, row in scan.items():
            row.update(module=mod, file=src.name)
            row["class"] = function_class(row["guest"], row["neon"], row["vfp"], row["calls"])
            functions[f"{mod}:{name}"] = row
            file_of.setdefault(name, []).append((mod, moddir, src))
    # (the synthetic corpus' source names: corpusgen's table)
    aliases, label_of = {}, {}
    for d in {m.parent for m in modules}:
        tsv = d / "corpus_functions.tsv"
        if tsv.exists():
            for line in tsv.read_text().splitlines()[1:]:
                mod, cname, symbol = line.split("\t")
                aliases[symbol] = cname
                label_of[cname] = symbol
    if "all" in wanted:
        wanted = [w for w in wanted if w != "all"] + sorted(file_of)
    wanted = list(dict.fromkeys(aliases.get(f, f) for f in wanted))
    missing = [f for f in wanted if f not in file_of]
    if missing:
        print(f"note: not in the tree: {' '.join(missing)}")
    if args.only_function_files:
        keep = {src for f in wanted for (_, _, src) in file_of.get(f, [])}
        files = [f for f in files if f[2] in keep]
        scope = f"the {len(files)} files holding the named functions"
    else:
        scope = f"all {len(files)} files"
    in_scope = {(mod, src.name) for mod, _, src in files}

    # Host side.
    targets, notes = make_targets(names, args.debug_info, args.flags_x64, args.flags_arm64)
    for n in notes:
        print(f"note: {n}")
    helpers = {}
    for target in targets:
        binary = getattr(args, f"binary_{target.name}", None)
        if binary:
            by_name = defaultdict(list)
            for key, r in functions.items():
                if (r["module"], r["file"]) in in_scope:
                    by_name[key.split(":", 1)[1]].append(key)
            hi = hb = 0
            for sym, (n, b) in disassemble(target, Path(binary)).items():
                keys = by_name.get(sym, [])
                if len(keys) == 1:
                    functions[keys[0]][target.name] = [n, b]
                elif not sym.startswith("f_"):
                    hi += n
                    hb += b
            helpers[target.name] = {"host_instructions": hi, "host_bytes": hb}
            gone = sum(1 for k, r in functions.items() if (r["module"], r["file"]) in in_scope and target.name not in r)
            print(f"{target.name}: {binary}: {gone} functions have no symbol in it (inlined, or not linked)")
            continue
        given = getattr(args, f"objects_{target.name}")
        objects = []
        if given:
            by_name = {}
            for p in Path(given).rglob(f"*.c{target.obj_suffix}"):
                by_name.setdefault(p.name, p)
            for mod, moddir, src in files:
                obj = by_name.get(src.name + target.obj_suffix)
                if not obj:
                    raise SystemExit(f"{target.name}: no object for {src.name} under {given}")
                if obj.stat().st_mtime < src.stat().st_mtime:
                    print(f"warning: {obj} is older than {src}: it may not be compiled from this source")
                objects.append((mod, src, obj))
        else:
            jobs = []
            for mod, moddir, src in files:
                obj = work / target.name / mod / (src.name + target.obj_suffix)
                objects.append((mod, src, obj))
                jobs.append((src, obj, moddir))
            stale = sum(1 for src, obj, _ in jobs if not up_to_date(target, src, obj))
            print(f"{target.name}: {len(jobs)} files, {stale} to compile ({args.jobs} at a time) with {target.compiler}", flush=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                errors = [e for e in pool.map(lambda j: compile_one(target, *j), jobs) if e]
            if errors:
                raise SystemExit("\n".join(errors))
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            dis = list(pool.map(lambda o: disassemble(target, o[2]), objects))
        hi = hb = 0
        for (mod, src, obj), syms in zip(objects, dis):
            for sym, (n, b) in syms.items():
                key = f"{mod}:{sym}"
                if key in functions and functions[key]["file"] == src.name:
                    functions[key][target.name] = [n, b]
                elif not sym.startswith("f_"):
                    hi += n
                    hb += b
        helpers[target.name] = {"host_instructions": hi, "host_bytes": hb}
        lost = [k for k, r in functions.items() if (r["module"], r["file"]) in in_scope and target.name not in r]
        if lost:
            print(f"warning: {target.name}: {len(lost)} functions have no symbol in the objects (first: {lost[0]})")

    tnames = [t.name for t in targets]
    rows = [r for r in functions.values() if (r["module"], r["file"]) in in_scope and r["guest"] > 0]
    here = lambda s: s.replace(str(ROOT), "<psvrecore>")  # (the record says where the checkout was only as this)
    result = {"tree": label, "scope": scope,
              "targets": {t.name: {"compiler": t.compiler, "flags": [here(f) for f in t.flags]} for t in targets},
              "overall": summarize(rows), "helpers": helpers}
    result["modules"] = {m: summarize([r for r in rows if r["module"] == m]) for m in sorted({r["module"] for r in rows})}
    result["classes"] = {c: summarize([r for r in rows if r["class"] == c]) for c in ("integer", "vfp", "neon", "call-heavy")}
    named = {}
    for f in wanted:
        for mod, _, src in file_of.get(f, []):
            r = functions[f"{mod}:{f}"]
            if (r["module"], r["file"]) in in_scope:
                s = summarize([r])
                s.update(module=mod, **{"class": r["class"]}, neon=r["neon"], vfp=r["vfp"], calls=r["calls"])
                key = f if len(file_of[f]) == 1 else f"{mod}:{f}"
                named[f"{label_of[f]} ({key})" if f in label_of else key] = s
    result["functions"] = named
    if args.all_functions:
        result["all_functions"] = {k: {kk: vv for kk, vv in r.items() if kk != "file"} for k, r in functions.items() if (r["module"], r["file"]) in in_scope}

    print(f"\n{label}: {scope}; host instructions, per guest instruction (/g) and code bytes per guest instruction (B/g)")
    print_table("Overall", [("all", result["overall"])], tnames)
    print_table("By module", list(result["modules"].items()), tnames)
    print_table("By class", [(c, s) for c, s in result["classes"].items() if s["functions"]], tnames)
    if named:
        print_table("Named functions", [(f"{k} ({v['class']})", v) for k, v in named.items()], tnames)
    for t, h in helpers.items():
        print(f"{t}: out-of-line helpers and other symbols: {h['host_instructions']} instructions, {h['host_bytes']} bytes (not in the ratios)")
    if args.json:
        with open(args.json, "w", newline="\n") as f:
            json.dump(result, f, indent=1)
        print(f"wrote {args.json}")
    if args.compare:
        compare(json.loads(Path(args.compare).read_text()), result, tnames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
