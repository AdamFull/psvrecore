#!/usr/bin/env python3
"""Same-source differential tests: each probe in tests/probes/*.c is built with the PS Vita SDK's compiler, recompiled
by psvrecomp together with the SDK's libc module, and run by the probe runner, which compares its output with the
same probe compiled for the host.

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of psvrecomp (--preset). Everything it produces from the
SDK (the probe modules, the recompiled code, the runners) goes to --out, by default build/probes: never commit it.

    python scripts/probe_check.py --sdk C:/path/to/sdk
    python scripts/probe_check.py --sdk ... integer float     # some probes only
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBC_BASES = "0x83000000,0x83060000"  # libc moved off 0x81000000, where every probe is linked


def run(cmd, env=None, cwd=None):
    result = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"failed ({result.returncode}): {' '.join(str(c) for c in cmd)}")
    return result.stdout


def map_symbol(map_file, name):
    """The address of a symbol in a psp2ld map file (code addresses keep the Thumb bit)."""
    pattern = re.compile(r"^([0-9a-fA-F]{8}) [0-9a-fA-F]{8} +\d+ +" + re.escape(name) + r"\s*$")
    for line in Path(map_file).read_text(errors="replace").splitlines():
        m = pattern.match(line.strip())
        if m:
            return int(m.group(1), 16)
    raise SystemExit(f"{name} not found in {map_file}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("probes", nargs="*", help="probe names (default: every tests/probes/*.c)")
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset to build with")
    parser.add_argument("--out", default=str(ROOT / "build" / "probes"), help="where to put what the SDK produces")
    parser.add_argument("--opt", default="-O2", help="psp2snc optimization option")
    args = parser.parse_args()
    if not args.sdk:
        raise SystemExit("give --sdk (or set SCE_PSP2_SDK_DIR)")
    sdk = Path(args.sdk)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SCE_PSP2_SDK_DIR=str(sdk))
    snc = sdk / "host_tools" / "build" / "bin" / "psp2snc.exe"
    exe = ".exe" if os.name == "nt" else ""
    psvrecomp = ROOT / "build" / args.preset / "tools" / "psvrecomp" / f"psvrecomp{exe}"
    if not psvrecomp.exists():
        raise SystemExit(f"{psvrecomp} not found: build the {args.preset} preset first")

    names = args.probes or sorted(p.stem for p in (ROOT / "tests" / "probes").glob("*.c"))

    # libc, recompiled once.
    libc_self = sdk / "target" / "sce_module" / "libc.suprx"
    libc_gen = out / "sdk_libc"  # (not "libc": a probe has that name)
    shutil.rmtree(libc_gen, ignore_errors=True)
    run([psvrecomp, "generate", libc_self, libc_gen, "--name", "libc", "--base", LIBC_BASES])

    lines = [
        f'set(PSVRECORE_PROBE_LIBC_SELF "{libc_self.as_posix()}")',
        f'set(PSVRECORE_PROBE_LIBC_BASES "{LIBC_BASES}")',
        f'set(PSVRECORE_PROBE_LIBC_GENERATED "{libc_gen.as_posix()}")',
    ]
    for name in names:
        source = ROOT / "tests" / "probes" / f"{name}.c"
        work = out / name
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        obj, self_file, map_file = work / f"{name}.o", work / f"{name}.self", work / f"{name}.map"
        # (a main for the SDK's startup code to call; probe_main is what the runner calls)
        main_c = work / "main.c"
        main_c.write_text("int main(void) { return 0; }\n")
        run([snc, args.opt, "-c", source, "-o", obj, f"-I{(ROOT / 'tests' / 'probes').as_posix()}"], env=env)
        run([snc, "-c", main_c, "-o", work / "main.o"], env=env)
        run([snc, obj, work / "main.o", "-o", self_file, f"-Wl,--Map={map_file}"], env=env)
        entry = map_symbol(map_file, "probe_main")
        run([psvrecomp, "generate", self_file, work / "gen", "--name", f"probe_{name}"])
        lines.append(f'psvrecore_add_probe(NAME {name} SOURCE "{source.as_posix()}" GENERATED "{(work / "gen").as_posix()}" '
                     f'SELF "{self_file.as_posix()}" BASES "" ENTRY 0x{entry:08X})')
        print(f"{name}: probe_main at 0x{entry:08x}")
    (out / "probes.cmake").write_text("\n".join(lines) + "\n")

    # Build the runners in the preset's build folder, then take the probes out of its configuration again.
    build = ROOT / "scripts" / "build.ps1"
    targets = ",".join(f"probe_{n}" for n in names)
    try:
        run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset, "-Target", targets,
             "-Define", f"PSVRECORE_PROBES_DIR={out.as_posix()}"])
    finally:
        run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset, "-Target",
             "psvrecomp", "-Define", "PSVRECORE_PROBES_DIR="])

    failed = []
    for name in names:
        runner = ROOT / "build" / args.preset / "tests" / "probes" / f"probe_{name}{exe}"
        result = subprocess.run([runner], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            sys.stdout.write(result.stderr[-4000:])
            failed.append(name)
    print(f"{len(names) - len(failed)} of {len(names)} probes agree" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
