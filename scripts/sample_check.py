#!/usr/bin/env python3
"""The SDK's samples on the runtime: each sample (a folder of target/samples/sample_code with a project, named
category/project/sample, or just its name for the GXM samples of graphics/api_libgxm) is built with the PS Vita SDK
(its Cg shaders with psp2cgc and psp2bin, its C code with psp2snc, linked with the stub libraries its project names),
recompiled by psvrecomp together with the SDK's libc and libfios2, and booted by a runner on the runtime with the GXM
layer. The data files its sources name (app0:...) are copied into its application folder. The runner shows and plays
nothing: it writes the frames the sample displays as PNG files and what its audio ports play as WAV files, and ends
the sample after a number of frames and seconds of audio (or when it exits).

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of psvrecomp (--preset). Everything it produces from the
SDK (programs, recompiled code, frames) goes to --out, by default build/samples: never commit it.

    python scripts/sample_check.py --sdk C:/path/to/sdk basic
    python scripts/sample_check.py --sdk ... basic blending --frames 3 --log info
    python scripts/sample_check.py --sdk ... audio_video/api_audioout/stream --audio-seconds 10
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


def include_dirs(project, sample_dir, sdk):
    """The include folders a sample's Release configuration adds (beyond the SDK's own, which psp2snc knows)."""
    root = ET.parse(project).getroot()
    dirs = []
    for group in root.iter():
        if not group.tag.endswith("ItemDefinitionGroup") or "Release" not in group.get("Condition", ""):
            continue
        for item in group.iter():
            if not item.tag.endswith("AdditionalIncludeDirectories") or not item.text:
                continue
            for entry in item.text.split(";"):
                entry = entry.strip()
                if not entry or entry.startswith("%("):
                    continue
                entry = entry.replace("$(SCE_PSP2_SDK_DIR)", str(sdk))
                entry = entry.replace("$(SCE_PSP2_SAMPLE_DIR)", str(sdk / "target" / "samples"))
                path = (sample_dir / entry.replace("\\", "/")).resolve()
                if path.is_dir() and path not in dirs:
                    dirs.append(path)
    return dirs


def expand(text, source, sdk, work):
    """MSBuild's item and property macros, as a sample's custom build steps use them."""
    text = text.replace("%(RootDir)%(Directory)", str(source.parent) + "\\")
    text = text.replace("%(Filename)", source.stem).replace("%(Extension)", source.suffix)
    text = text.replace("%(FullPath)", str(source))
    text = text.replace("$(SCE_PSP2_SDK_DIR)", str(sdk)).replace("$(IntDir)", str(work) + "\\")
    return text


def sample_dir_of(sdk, name):
    """A sample's folder: category/project/sample under sample_code, or a GXM sample's name."""
    code = sdk / "target" / "samples" / "sample_code"
    return code / name if "/" in name else code / "graphics" / "api_libgxm" / name


def target_id(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def sample_data(sdk, sample_dir, path):
    """A data file the SDK keeps under target/samples/data (by its name, in the sample's category if several)."""
    data = sdk / "target" / "samples" / "data"
    name = Path(path).name
    found = [f for f in data.rglob(name) if f.is_file() and f.as_posix().endswith(path)] if data.is_dir() else []
    if len(found) > 1:
        category = sample_dir.relative_to(sdk / "target" / "samples" / "sample_code").parts[0]
        found = [f for f in found if category in f.relative_to(data).parts] or found
    return found[0] if found else None


def copy_data(sample_dir, sources, sdk, app):
    """The files the sample's sources name as app0:... (the SDK's samples keep them under target/)."""
    texts = set(sources)
    for pattern in ("*.c", "*.cpp", "*.h"):
        texts.update(sample_dir.glob(pattern))
    copied = []
    for text_file in texts:
        try:
            text = text_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for path in re.findall(r'"app0:/?([^"%]+)"', text):
            src = sdk / "target" / path
            if not src.exists():
                src = sample_data(sdk, sample_dir, path) or src
            dst = app / path
            if src.is_file() and not dst.exists():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(src, dst)
                copied.append(path)
            elif src.is_dir() and not dst.exists():
                shutil.copytree(src, dst)
                copied.append(path + "/")
    return copied


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
    includes = [f"-I{d.as_posix()}" for d in [sample_dir, *include_dirs(project, sample_dir, sdk)]]
    for source in sources:
        obj = work / f"{source.parent.name}_{source.stem}.o"
        run([snc, opt, "-c", source, "-o", obj, *dict.fromkeys(includes)], env=env)
        objects.append(obj)
    app = work / "app"
    (app / "sce_module").mkdir(parents=True)
    self_file = app / "eboot.bin"
    run([snc, *objects, "-o", self_file, *libraries(project), f"-Wl,--Map={(work / 'sample.map').as_posix()}"], env=env)
    copy_data(sample_dir, sources, sdk, app)
    return app, self_file


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("samples", nargs="*", help="sample names (default: basic)")
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset to build with")
    parser.add_argument("--out", default=str(ROOT / "build" / "samples"), help="where to put what the SDK produces")
    parser.add_argument("--opt", default="-O2", help="psp2snc optimization option")
    parser.add_argument("--frames", type=int, default=None,
                        help="frames to show before the sample is ended (default: 2 for GXM samples, else 0)")
    parser.add_argument("--audio-seconds", type=int, default=0,
                        help="seconds of audio a port must play before the sample is ended (default 0)")
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
            tid = target_id(name)
            work = out / tid
            shutil.rmtree(work, ignore_errors=True)
            work.mkdir(parents=True)
            app, self_file = build_sample(name, sample_dir_of(sdk, name), work, sdk, env, args.opt)
            shutil.copy(libc_self, app / "sce_module" / "libc.suprx")
            shutil.copy(fios2_self, app / "sce_module" / "libfios2.suprx")
            run([psvrecomp, "generate", self_file, work / "gen", "--name", f"sample_{tid}"])
            lines.append(f'psvrecore_add_sample(NAME {tid} GENERATED "{(work / "gen").as_posix()}")')
            print(f"{name}: built and recompiled")
        (out / "samples.cmake").write_text("\n".join(lines) + "\n")
        build = ROOT / "scripts" / "build.ps1"
        targets = ",".join(f"sample_{target_id(n)}" for n in names)
        try:
            run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset,
                 "-Target", targets, "-Define", f"PSVRECORE_SAMPLES_DIR={out.as_posix()}"])
        finally:
            run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", build, "-Preset", args.preset,
                 "-Target", "psvrecomp", "-Define", "PSVRECORE_SAMPLES_DIR="])

    failed = []
    for name in names:
        tid = target_id(name)
        runner = ROOT / "build" / args.preset / "tests" / "samples" / f"sample_{tid}{exe}"
        work = out / tid
        wanted = args.frames if args.frames is not None else (0 if "/" in name else 2)
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
            result = subprocess.run([runner, work / "app", data, frames, str(wanted), str(args.timeout),
                                     str(args.audio_seconds)],
                                    capture_output=True, text=True, timeout=args.timeout + 30, env=run_env)
        except subprocess.TimeoutExpired:
            print(f"{name}: did not end")
            failed.append(name)
            continue
        shown = sorted(frames.glob("*.png"))
        tracks = sorted(frames.glob("*.wav"))
        good = result.returncode == 0 and len(shown) >= wanted
        print(f"{name}: {'showed' if good else 'FAILED after'} {len(shown)} frame(s) (exit {result.returncode})"
              + (f"; {shown[-1]}" if shown else "") + "".join(f"; {t}" for t in tracks))
        if not good or args.log:
            sys.stdout.write(result.stdout[-4000:])
            sys.stdout.write(result.stderr[-8000:])
        if not good:
            failed.append(name)
    print(f"{len(names) - len(failed)} of {len(names)} samples ran" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
