#!/usr/bin/env python3
"""Generates, builds and packages a game for one platform, named and iconed from its own sce_sys folder (param.sfo's
title, localized titles, title ID and version; icon0.png; the LiveArea's pictures as the launch screen). A game
repository calls it from its scripts/package.py, which gives --root and --name; the user gives the platform and the
game's folder:

    python scripts/package.py windows [--game "game/<the game's folder>"]   # build/package/windows/<name>/
    python scripts/package.py linux   [--game "game/<the game's folder>"]   # build/package/linux/<name>/
    python scripts/package.py android [--game "game/<the game's folder>"]   # build/package/android/<name>.apk

Without --game, the one folder in <root>/game/ that holds app/ (a copy of the console's folders) is the game's.

Steps:
  1. sce_sys: the pictures and files made from the game's sce_sys go to <root>/generated/package/ (never committed):
     <name>.ico (16-256 px) and <name>.rc (the icon and the version information) for Windows, hicolor/ and
     <name>.desktop for Linux, launch.png (the launch screen: the LiveArea's background and gate, when the game has a
     sce_sys/livearea folder), sce_sys.json (what was read). The game's CMakeLists.txt calls
     psvrecore_game_package(<name>), which compiles <name>.rc into the Windows program and copies launch.png beside
     the program in the build folder.
  2. code: when <root>/generated/code is empty (or --generate always), psvrecomp is built (the host's release preset)
     and the game's scripts/generate_code.py recompiles the game.
  3. build: the platform's preset (release unless --preset), only the game's target.
  4. package:
     - windows: a folder to copy anywhere: the program, psvrecore.dll, launch.png, install.ps1 (a Start menu shortcut;
       -Uninstall removes it).
     - linux: a folder (what CI uploads): the program, libpsvrecore.so.N (found next to the program: $ORIGIN), the
       .desktop entry, the hicolor icons and launch.png next to them, install.sh (installs the entry and icons for this user,
       pointing at the folder; --uninstall).
     - android: the APK (scripts/android_apk.py: the label and its translations, the version, the launcher icon, the
       launch screen and the system's splash screen in its colours).

The icon is the game's icon0.png, shown in the console's round bubble; or a picture of your own: --icon, or
<root>/game/icon.png when there is one (made square from its artwork, not rounded).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sce_sys  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
HOST = "windows" if os.name == "nt" else "linux"


def run(cmd, **kwargs):
    print("+ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], **kwargs)
    if result.returncode != 0:
        raise SystemExit(f"failed ({result.returncode}): {cmd[0]}")


def build(root, preset, build_dir, targets):
    """Configures and builds a preset of the game's repository into build_dir (Windows: through build.ps1, which
    brings in the Visual Studio environment)."""
    if os.name == "nt":
        run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", HERE / "build.ps1", "-Root", root,
             "-Preset", preset, "-BuildDir", build_dir, "-NoTest", "-Target", ",".join(targets)])
    else:
        run(["cmake", "--preset", preset, "-B", build_dir], cwd=root)
        run(["cmake", "--build", build_dir, "--target", *targets], cwd=root)


# --- 1. sce_sys -----------------------------------------------------------------------------------------------------

def make_assets(meta, name, picture, bubble, out):
    out.mkdir(parents=True, exist_ok=True)
    square = sce_sys.square_icon(picture, bubble) if picture else None
    if square:
        sce_sys.resized(square, 256).save(out / "icon.png", optimize=True)
        sce_sys.write_ico(square, out / f"{name}.ico")
        shutil.rmtree(out / "hicolor", ignore_errors=True)
        sce_sys.write_hicolor(square, out / "hicolor", name)
    else:
        for stale in (out / "icon.png", out / f"{name}.ico"):
            stale.unlink(missing_ok=True)
        shutil.rmtree(out / "hicolor", ignore_errors=True)
    rc = sce_sys.windows_rc(meta, name, out / f"{name}.ico")
    if not square:
        rc = "\n".join(line for line in rc.splitlines() if not line.startswith("1 ICON")) + "\n"
    # (written only when it changes: the build recompiles it then)
    rc_path = out / f"{name}.rc"
    if not rc_path.is_file() or rc_path.read_text(encoding="utf-8") != rc:
        rc_path.write_text(rc, encoding="utf-8")
    (out / f"{name}.desktop").write_text(sce_sys.desktop_entry(meta, name), encoding="utf-8", newline="\n")
    # (the launch screen: the LiveArea's background and gate; none without a livearea folder)
    launch = sce_sys.launch_picture(*meta.livearea())
    if launch:
        launch[0].save(out / sce_sys.LAUNCH_FILE, optimize=True)
    else:
        (out / sce_sys.LAUNCH_FILE).unlink(missing_ok=True)
    info = meta.describe()
    info["picture"] = str(picture) if picture else None
    info["launch"] = {"file": sce_sys.LAUNCH_FILE, "colour": "#%02X%02X%02X" % launch[1]} if launch else None
    (out / "sce_sys.json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"sce_sys: {meta.title} ({meta.title_id} {meta.app_ver}); icon {picture or 'none'}; launch screen "
          f"{'from the LiveArea' if launch else 'none (no LiveArea)'} -> {out}")


# --- 4. packages ----------------------------------------------------------------------------------------------------

def shortcut_name(title):
    """A file name from the title (no characters Windows refuses: "Persona 4: X" -> "Persona 4 - X")."""
    name = title.replace(": ", " - ")
    name = "".join(c for c in name if c not in '<>:"/\\|?*' and ord(c) >= 32).strip(" .")
    return name or "game"


INSTALL_PS1 = """# Adds a Start menu shortcut to {name}.exe in this folder (-Uninstall removes it). Made by psvrecore's
# scripts/package.py.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$programs = [Environment]::GetFolderPath('Programs')
$link = Join-Path $programs '{shortcut}.lnk'
if ($Uninstall) {{
    if (Test-Path -LiteralPath $link) {{ Remove-Item -LiteralPath $link }}
    Write-Output "removed $link"
    exit 0
}}
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($link)
$s.TargetPath = Join-Path $PSScriptRoot '{name}.exe'
$s.WorkingDirectory = $PSScriptRoot
$s.Description = '{title}'
$s.Save()
Write-Output "added $link"
"""

INSTALL_SH = """#!/bin/sh
# Installs {name}'s launcher entry and icons for this user ($XDG_DATA_HOME, ~/.local/share), pointing at the program
# in this folder; --uninstall removes them. Made by psvrecore's scripts/package.py.
set -e
here=$(cd "$(dirname "$0")" && pwd)
data=${{XDG_DATA_HOME:-$HOME/.local/share}}
entry="$data/applications/{name}.desktop"
if [ "$1" = "--uninstall" ]; then
    rm -f "$entry"
    for icon in "$data"/icons/hicolor/*/apps/{name}.png; do rm -f "$icon"; done
else
    (cd "$here/icons" && find hicolor -name '{name}.png') | while read -r icon; do
        mkdir -p "$data/icons/$(dirname "$icon")"
        cp "$here/icons/$icon" "$data/icons/$icon"
    done
    mkdir -p "$data/applications"
    # (Exec: the program's path quoted, with the characters a quoted argument escapes)
    program=$(printf '%s' "$here/{name}" | sed 's/[\\\\"`$]/\\\\\\\\&/g')
    grep -v '^Exec=' "$here/{name}.desktop" > "$entry"
    printf 'Exec="%s"\\nPath=%s\\n' "$program" "$here" >> "$entry"
fi
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$data/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$data/icons/hicolor" || true
echo "${{1:-installed}}: $entry"
"""


def copy_launch(assets, folder):
    """The launch screen's picture beside the program, where the frontend looks for it."""
    if (assets / sce_sys.LAUNCH_FILE).is_file():
        shutil.copy2(assets / sce_sys.LAUNCH_FILE, folder)


def package_windows(meta, name, build_dir, assets, out):
    folder = out / name
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    exe = build_dir / f"{name}.exe"
    if not exe.is_file():
        raise SystemExit(f"{exe}: not built")
    shutil.copy2(exe, folder)
    for dll in build_dir.glob("*.dll"):
        shutil.copy2(dll, folder)
    copy_launch(assets, folder)
    title = meta.title.replace("'", "''")
    (folder / "install.ps1").write_text(
        INSTALL_PS1.format(name=name, shortcut=shortcut_name(meta.title).replace("'", "''"), title=title),
        encoding="utf-8-sig")  # (Windows PowerShell reads a file without a byte order mark as ANSI)
    return folder


def package_linux(meta, name, build_dir, assets, out):
    folder = out / name
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    program = build_dir / name
    if not program.is_file():
        raise SystemExit(f"{program}: not built")
    shutil.copy2(program, folder)
    libraries = sorted(p for p in build_dir.glob("libpsvrecore.so.*") if p.name.count(".") == 2)
    for lib in libraries:  # (the SONAME's file: libpsvrecore.so.<ABI version>)
        shutil.copy2(lib, folder / lib.name, follow_symlinks=True)
    shutil.copy2(assets / f"{name}.desktop", folder)
    copy_launch(assets, folder)
    if (assets / "hicolor").is_dir():
        shutil.copytree(assets / "hicolor", folder / "icons" / "hicolor")
    script = folder / "install.sh"
    script.write_text(INSTALL_SH.format(name=name), encoding="utf-8", newline="\n")
    script.chmod(0o755)
    return folder


def package_android(meta, name, root, build_dir, game, title_id, picture, out, extra):
    lib = build_dir / "libmain.so"
    if not lib.is_file():
        raise SystemExit(f"{lib}: not built")
    apk = out / f"{name}.apk"
    cmd = [sys.executable, HERE / "android_apk.py", "--manifest", root / "android" / "AndroidManifest.xml",
           "--lib", lib, "--out", apk, "--sce-sys", game]
    if title_id:
        cmd += ["--title-id", title_id]
    if picture:
        cmd += ["--icon", picture]
    run(cmd + extra)
    return apk


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("platform", choices=["windows", "linux", "android", "assets"],
                    help="what to make (assets: step 1 only)")
    ap.add_argument("--root", required=True, help="the game's repository")
    ap.add_argument("--name", required=True, help="the game's program (its CMake target and frontend name)")
    ap.add_argument("--game", help="the game's folder (app/<title ID>/, patch/<title ID>/); default: the one in "
                                   "<root>/game/")
    ap.add_argument("--title-id", help="the title, when the game's folder holds several")
    ap.add_argument("--icon", help="a picture of your own for the icons (default: <root>/game/icon.png when there, "
                                   "else the game's icon0.png)")
    ap.add_argument("--preset", help="the build's preset (default: <platform>-x64-release, android-arm64-release)")
    ap.add_argument("--build-dir", help="the build's folder (default: the preset's, build/<preset>)")
    ap.add_argument("--out", help="where the package goes (default: <root>/build/package/<platform>)")
    ap.add_argument("--generate", choices=["auto", "always", "never"], default="auto",
                    help="recompile the game first: auto, when generated/code is empty")
    ap.add_argument("--no-build", action="store_true", help="package what is built (no code generation either)")
    args, extra = ap.parse_known_args()
    if extra and args.platform != "android":
        ap.error(f"unrecognized arguments: {' '.join(extra)}")
    sys.stdout.reconfigure(encoding="utf-8")

    root = Path(args.root).resolve()
    if args.game:
        game = Path(args.game).resolve()
    else:
        found = sorted(p for p in (root / "game").glob("*") if (p / "app").is_dir())
        if len(found) != 1:
            raise SystemExit(f"{root / 'game'}: {'no game folder' if not found else 'several game folders'} "
                             f"(app/<title ID>/ inside): give --game")
        game = found[0]
    meta = sce_sys.find(game, args.title_id)
    title_id = args.title_id or meta.title_id
    picture, bubble = None, False
    if args.icon:
        picture = Path(args.icon)
    elif (root / "game" / "icon.png").is_file():
        picture = root / "game" / "icon.png"
    elif meta.icon:
        picture, bubble = meta.icon, True
    assets = root / "generated" / "package"
    make_assets(meta, args.name, picture, bubble, assets)
    if args.platform == "assets":
        return 0
    if args.platform in ("windows", "linux") and args.platform != HOST:
        raise SystemExit(f"{args.platform}: build it on {args.platform} (this is {HOST})")

    preset = args.preset or ("android-arm64-release" if args.platform == "android" else f"{args.platform}-x64-release")
    build_dir = Path(args.build_dir).resolve() if args.build_dir else root / "build" / preset
    code = root / "generated" / "code"
    empty = not (code.is_dir() and any(code.iterdir()))
    if not args.no_build and (args.generate == "always" or (args.generate == "auto" and empty)):
        host_preset = f"{HOST}-x64-release"
        build(root, host_preset, root / "build" / host_preset, ["psvrecomp"])
        run([sys.executable, root / "scripts" / "generate_code.py", "--game", game, "--preset", host_preset])
    if not args.no_build:
        build(root, preset, build_dir, [args.name])

    out = Path(args.out).resolve() if args.out else root / "build" / "package" / args.platform
    out.mkdir(parents=True, exist_ok=True)
    if args.platform == "windows":
        result = package_windows(meta, args.name, build_dir, assets, out)
    elif args.platform == "linux":
        result = package_linux(meta, args.name, build_dir, assets, out)
    else:
        result = package_android(meta, args.name, root, build_dir, game, title_id,
                                 picture if not bubble else None, out, extra)
    print(f"{args.platform}: {result}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
