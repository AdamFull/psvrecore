#!/usr/bin/env python3
"""Test programs on the runtime: each tests/programs/*.c is built with the PS Vita SDK's compiler into a whole program
(the SDK's start-up code, libc), recompiled by psvrecomp together with the SDK's libc module, and booted by a runner on
the runtime, from an application folder laid out as a game's (eboot.bin, sce_module/libc.suprx). What it prints must
match tests/programs/<name>.expected, and it must exit with 0.

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of psvrecomp (--preset). Everything it produces from the
SDK (programs, recompiled code, runners) goes to --out, by default build/programs: never commit it.

    python scripts/program_check.py --sdk C:/path/to/sdk
    python scripts/program_check.py --sdk ... threads --runs 5     # one program, five times
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBC_BASES = "0x83000000,0x83060000"  # libc moved off 0x81000000, where every program is linked
FIOS2_BASES = "0x83100000,0x83180000"  # and libfios2 (the other module the system preloads)


def run(cmd, env=None, cwd=None):
    result = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"failed ({result.returncode}): {' '.join(str(c) for c in cmd)}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("programs", nargs="*", help="program names (default: every tests/programs/*.c)")
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset to build with")
    parser.add_argument("--out", default=str(ROOT / "build" / "programs"), help="where to put what the SDK produces")
    parser.add_argument("--opt", default="-O2", help="psp2snc optimization option")
    parser.add_argument("--runs", type=int, default=1, help="run each program this many times")
    parser.add_argument("--timeout", type=int, default=120, help="seconds a program may take")
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
    names = args.programs or sorted(p.stem for p in (ROOT / "tests" / "programs").glob("*.c"))

    libc_self = sdk / "target" / "sce_module" / "libc.suprx"
    libc_gen = out / "sdk_libc"
    shutil.rmtree(libc_gen, ignore_errors=True)
    run([psvrecomp, "generate", libc_self, libc_gen, "--name", "libc", "--base", LIBC_BASES])

    fios2_self = sdk / "target" / "sce_module" / "libfios2.suprx"
    fios2_gen = out / "sdk_libfios2"
    shutil.rmtree(fios2_gen, ignore_errors=True)
    run([psvrecomp, "generate", fios2_self, fios2_gen, "--name", "libfios2", "--base", FIOS2_BASES])

    lines = [f'set(PSVRECORE_PROGRAM_LIBC_GENERATED "{libc_gen.as_posix()}")',
             f'set(PSVRECORE_PROGRAM_FIOS2_GENERATED "{fios2_gen.as_posix()}")']
    for name in names:
        source = ROOT / "tests" / "programs" / f"{name}.c"
        work = out / name
        shutil.rmtree(work, ignore_errors=True)
        app = work / "app"
        (app / "sce_module").mkdir(parents=True)
        obj, self_file = work / f"{name}.o", app / "eboot.bin"
        run([snc, args.opt, "-c", source, "-o", obj, f"-I{(ROOT / 'tests' / 'programs').as_posix()}"], env=env)
        run([snc, obj, "-o", self_file, f"-Wl,--Map={(work / (name + '.map')).as_posix()}"], env=env)
        shutil.copy(libc_self, app / "sce_module" / "libc.suprx")
        shutil.copy(fios2_self, app / "sce_module" / "libfios2.suprx")
        run([psvrecomp, "generate", self_file, work / "gen", "--name", f"program_{name}"])
        lines.append(f'psvrecore_add_program(NAME {name} GENERATED "{(work / "gen").as_posix()}")')
        print(f"{name}: built and recompiled")
    (out / "programs.cmake").write_text("\n".join(lines) + "\n")

    build = ROOT / "scripts" / "build.ps1"
    targets = ",".join(f"program_{n}" for n in names)
    try:
        run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset, "-Target", targets,
             "-Define", f"PSVRECORE_PROGRAMS_DIR={out.as_posix()}"])
    finally:
        run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset, "-Target",
             "psvrecomp", "-Define", "PSVRECORE_PROGRAMS_DIR="])

    failed = []
    for name in names:
        runner = ROOT / "build" / args.preset / "tests" / "programs" / f"program_{name}{exe}"
        expected = (ROOT / "tests" / "programs" / f"{name}.expected").read_text().replace("\r\n", "\n")
        for attempt in range(args.runs):
            data = out / name / "data"
            shutil.rmtree(data, ignore_errors=True)
            data.mkdir(parents=True)
            try:
                result = subprocess.run([runner, out / name / "app", data, str(args.timeout)], capture_output=True, text=True,
                                        timeout=args.timeout + 30)
            except subprocess.TimeoutExpired:
                print(f"{name}: did not end")
                failed.append(name)
                break
            output = result.stdout.replace("\r\n", "\n")
            good = result.returncode == 0 and output == expected
            label = f"{name}" + (f" (run {attempt + 1})" if args.runs > 1 else "")
            print(f"{label}: {'as expected' if good else 'DIFFERENT'} (exit {result.returncode})")
            if not good:
                sys.stdout.write(output)
                sys.stdout.write(result.stderr[-4000:])
                failed.append(name)
                break
    print(f"{len(names) - len(set(failed))} of {len(names)} programs as expected" +
          (f"; failed: {', '.join(sorted(set(failed)))}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
