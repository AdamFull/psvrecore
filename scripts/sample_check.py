#!/usr/bin/env python3
"""The SDK's GXM samples on the runtime: each sample of target/samples/sample_code/graphics/api_libgxm is built with the
PS Vita SDK (its Cg shaders with psp2cgc and psp2bin, its C code with psp2snc, linked with the stub libraries its
project names), recompiled by psvrecomp together with the SDK's libc and libfios2, and booted by a runner on the
runtime with the GXM layer. The runner shows nothing: it writes the frames the sample displays as PNG files, and the
sample ends after a number of frames.

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of psvrecomp (--preset). Everything it produces from the
SDK (programs, recompiled code, frames) goes to --out, by default build/samples: never commit it.

    python scripts/sample_check.py --sdk C:/path/to/sdk basic
    python scripts/sample_check.py --sdk ... basic blending --frames 3 --log info
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBC_BASES = "0x83000000,0x83060000"  # (as scripts/program_check.py)
FIOS2_BASES = "0x83100000,0x83180000"


def run(cmd, env=None, cwd=None):
    result = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"failed ({result.returncode}): {' '.join(str(c) for c in cmd)}")
    return result.stdout


def libraries(project):
    """The -l options of a sample's Release configuration (the stub libraries it links)."""
    text = project.read_text(encoding="utf-8", errors="replace")
    found = re.findall(r"<AdditionalDependencies>([^<]*)</AdditionalDependencies>", text)
    libs = []
    for group in found[-1:]:  # (the last one is the Release configuration's)
        for item in group.split(";"):
            item = item.strip()
            if item.startswith("-l") and item not in libs:
                libs.append(item)
    return libs


def project_items(project):
    """A sample project's Release items: its sources, and its custom build steps (command, outputs) for shaders
    and textures."""
    root = ET.parse(project).getroot()
    ns = {"m": root.tag[1:root.tag.index("}")]} if root.tag.startswith("{") else {}
    q = (lambda tag: "m:" + tag) if ns else (lambda tag: tag)
    sources, steps = [], []
    for item in root.iter():
        tag = item.tag.split("}")[-1]
        include = item.get("Include")
        if not include:
            continue
        path = (project.parent / include.replace("\\", "/")).resolve()
        if tag == "ClCompile":
            sources.append(path)
        elif tag == "CustomBuild":
            command = outputs = None
            for child in item:
                cond = child.get("Condition", "")
                if "Release" not in cond:
                    continue
                if child.tag.endswith("Command"):
                    command = child.text
                elif child.tag.endswith("Outputs"):
                    outputs = child.text
            if command:
                steps.append((path, command, outputs or ""))
    return sources, steps


def expand(text, source, sdk, work):
    """MSBuild's item and property macros, as a sample's custom build steps use them."""
    text = text.replace("%(RootDir)%(Directory)", str(source.parent) + "\\")
    text = text.replace("%(Filename)", source.stem).replace("%(Extension)", source.suffix)
    text = text.replace("%(FullPath)", str(source))
    text = text.replace("$(SCE_PSP2_SDK_DIR)", str(sdk)).replace("$(IntDir)", str(work) + "\\")
    return text


def build_sample(name, sample_dir, work, sdk, env, opt):
    snc = sdk / "host_tools" / "build" / "bin" / "psp2snc.exe"
    project = next(sample_dir.glob("*.vcxproj"))
    sources, steps = project_items(project)
    objects = []
    for source, command, outputs in steps:
        result = subprocess.run(expand(command, source, sdk, work).strip(), shell=True, env=env, cwd=work,
                                capture_output=True, text=True)
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f"failed: the custom build step of {source.name}")
        for out in expand(outputs, source, sdk, work).split(";"):
            out = out.strip()
            if out.lower().endswith(".obj") and Path(out).exists():
                objects.append(Path(out))
    for source in sources:
        obj = work / f"{source.parent.name}_{source.stem}.o"
        run([snc, opt, "-c", source, "-o", obj, f"-I{sample_dir.as_posix()}"], env=env)
        objects.append(obj)
    app = work / "app"
    (app / "sce_module").mkdir(parents=True)
    self_file = app / "eboot.bin"
    run([snc, *objects, "-o", self_file, *libraries(project), f"-Wl,--Map={(work / (name + '.map')).as_posix()}"], env=env)
    return app, self_file


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("samples", nargs="*", help="sample names (default: basic)")
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset to build with")
    parser.add_argument("--out", default=str(ROOT / "build" / "samples"), help="where to put what the SDK produces")
    parser.add_argument("--opt", default="-O2", help="psp2snc optimization option")
    parser.add_argument("--frames", type=int, default=2, help="frames to show before the sample is ended")
    parser.add_argument("--timeout", type=int, default=120, help="seconds a sample may take")
    parser.add_argument("--log", help="the runtime's log level (PSVRECORE_LOG: debug, info, warn, error)")
    parser.add_argument("--no-build", action="store_true", help="run what was built last time")
    args = parser.parse_args()
    if not args.sdk:
        raise SystemExit("give --sdk (or set SCE_PSP2_SDK_DIR)")
    sdk = Path(args.sdk).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SCE_PSP2_SDK_DIR=str(sdk))
    exe = ".exe" if os.name == "nt" else ""
    psvrecomp = ROOT / "build" / args.preset / "tools" / "psvrecomp" / f"psvrecomp{exe}"
    samples_root = sdk / "target" / "samples" / "sample_code" / "graphics" / "api_libgxm"
    names = args.samples or ["basic"]

    if not args.no_build:
        if not psvrecomp.exists():
            raise SystemExit(f"{psvrecomp} not found: build the {args.preset} preset first")
        libc_self = sdk / "target" / "sce_module" / "libc.suprx"
        fios2_self = sdk / "target" / "sce_module" / "libfios2.suprx"
        libc_gen, fios2_gen = out / "sdk_libc", out / "sdk_libfios2"
        if not libc_gen.exists():
            run([psvrecomp, "generate", libc_self, libc_gen, "--name", "libc", "--base", LIBC_BASES])
        if not fios2_gen.exists():
            run([psvrecomp, "generate", fios2_self, fios2_gen, "--name", "libfios2", "--base", FIOS2_BASES])
        lines = [f'set(PSVRECORE_SAMPLE_LIBC_GENERATED "{libc_gen.as_posix()}")',
                 f'set(PSVRECORE_SAMPLE_FIOS2_GENERATED "{fios2_gen.as_posix()}")']
        for name in names:
            work = out / name
            shutil.rmtree(work, ignore_errors=True)
            work.mkdir(parents=True)
            app, self_file = build_sample(name, samples_root / name, work, sdk, env, args.opt)
            shutil.copy(libc_self, app / "sce_module" / "libc.suprx")
            shutil.copy(fios2_self, app / "sce_module" / "libfios2.suprx")
            run([psvrecomp, "generate", self_file, work / "gen", "--name", f"sample_{name}"])
            lines.append(f'psvrecore_add_sample(NAME {name} GENERATED "{(work / "gen").as_posix()}")')
            print(f"{name}: built and recompiled")
        (out / "samples.cmake").write_text("\n".join(lines) + "\n")
        build = ROOT / "scripts" / "build.ps1"
        targets = ",".join(f"sample_{n}" for n in names)
        try:
            run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset,
                 "-Target", targets, "-Define", f"PSVRECORE_SAMPLES_DIR={out.as_posix()}"])
        finally:
            run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset,
                 "-Target", "psvrecomp", "-Define", "PSVRECORE_SAMPLES_DIR="])

    failed = []
    for name in names:
        runner = ROOT / "build" / args.preset / "tests" / "samples" / f"sample_{name}{exe}"
        work = out / name
        data = work / "data"
        shutil.rmtree(data, ignore_errors=True)
        data.mkdir(parents=True)
        frames = work / "frames"
        shutil.rmtree(frames, ignore_errors=True)
        frames.mkdir()
        run_env = dict(os.environ)
        if args.log:
            run_env["PSVRECORE_LOG"] = args.log
        try:
            result = subprocess.run([runner, work / "app", data, frames, str(args.frames), str(args.timeout)],
                                    capture_output=True, text=True, timeout=args.timeout + 30, env=run_env)
        except subprocess.TimeoutExpired:
            print(f"{name}: did not end")
            failed.append(name)
            continue
        shown = sorted(frames.glob("*.png"))
        good = result.returncode == 0 and len(shown) >= args.frames
        print(f"{name}: {'showed' if good else 'FAILED after'} {len(shown)} frame(s) (exit {result.returncode})"
              + (f"; {shown[-1]}" if shown else ""))
        if not good or args.log:
            sys.stdout.write(result.stdout[-4000:])
            sys.stdout.write(result.stderr[-8000:])
        if not good:
            failed.append(name)
    print(f"{len(names) - len(failed)} of {len(names)} samples ran" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
