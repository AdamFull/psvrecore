#!/usr/bin/env python3
"""Packages an Android application from a game's (or sample's) libmain.so with the Android SDK's own tools: no Gradle.

The application is SDL's Java activity (third_party/SDL/android-project, compiled here) under the caller's subclass,
the caller's manifest and resources (and its launcher icon, made from a picture: --icon), and the native library (built by the android-arm64 presets; debug information
stripped for the package). It is signed with the Android debug key (~/.android/debug.keystore, made with keytool when
missing), so it installs for development only.

    python scripts/android_apk.py --manifest android/AndroidManifest.xml --java android/java \\
        --lib build/android-arm64-release/libmain.so --out build/android-arm64-release/game.apk [--install]

With --sce-sys <the game's folder>, the application is named from the game's param.sfo (its TITLE as the label,
@string/app_name, with the TITLE_xx translations; APP_VER as the version) and its icon is icon0.png unless --icon
gives another (scripts/sce_sys.py reads them; scripts/package.py passes it).

Needs ANDROID_HOME (or ANDROID_SDK_ROOT, or C:/Android/Sdk) with build-tools and a platform, the NDK (for llvm-strip),
and a JDK (javac, keytool) on PATH or in JAVA_HOME.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sce_sys  # noqa: E402
SDL_JAVA = ROOT / "third_party" / "SDL" / "android-project" / "app" / "src" / "main" / "java"
PSVRECORE_JAVA = ROOT / "android" / "java"  # (PsvrecoreActivity)
MIN_SDK = 29  # (the android-arm64 presets' ANDROID_PLATFORM)
EXE = ".exe" if os.name == "nt" else ""
BAT = ".bat" if os.name == "nt" else ""


def run(cmd, **kwargs):
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"failed ({result.returncode}): {' '.join(str(c) for c in cmd)}")
    return result.stdout


def newest(folder):
    """The highest-versioned entry of a folder (build-tools/36.0.0, platforms/android-36, ndk/29...)."""
    def key(p):
        return [int(x) if x.isdigit() else 0 for x in p.name.replace("android-", "").split(".")]
    entries = [p for p in folder.iterdir() if p.is_dir()] if folder.is_dir() else []
    return max(entries, key=key) if entries else None


def jdk_tool(name):
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / f"{name}{EXE}").exists():
        return Path(home) / "bin" / f"{name}{EXE}"
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"no {name}: install a JDK (17 or newer) or set JAVA_HOME")
    return Path(found)


# The launcher icon (@mipmap/ic_launcher): the legacy icon (48 dp) at each density, and the adaptive icon (two 108 dp
# layers, of which a mask shows the middle 72 dp at most).
DENSITIES = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
ICON_SPAN = 64  # (dp: the picture's artwork in the adaptive icon, a round logo's ends inside the masks' 66 dp safe zone)
LEGACY_SPAN = 44  # (dp, of the legacy icon's 48)


def write_icon(picture, res, background=None):
    """The launcher icon from a picture (a game's icon0.png, or a logo): its artwork (what differs from its corner's
    colour, or is not transparent) made square, over the corner's colour (or the background given; white for a
    transparent picture), in the middle of the legacy icon and of the adaptive icon's layers."""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        raise SystemExit("--icon needs Pillow (pip install pillow)")
    src = Image.open(picture).convert("RGBA")
    corner = src.getpixel((0, 0))
    if corner[3] < 255:
        box = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox()
        fill = (255, 255, 255)
    else:
        flat = Image.new("RGBA", src.size, corner)
        box = ImageChops.difference(src, flat).convert("L").point(lambda d: 255 if d > 24 else 0).getbbox()
        fill = corner[:3]
    if background:
        fill = tuple(int(background.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    art = src.crop(box or (0, 0, *src.size))
    side = max(art.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(art, ((side - art.width) // 2, (side - art.height) // 2))
    for name, scale in DENSITIES.items():
        folder = res / f"mipmap-{name}"
        folder.mkdir(parents=True)
        legacy, span = round(48 * scale), round(LEGACY_SPAN * scale)
        icon = Image.new("RGBA", (legacy, legacy), fill + (255,))
        piece = square.resize((span, span), Image.LANCZOS)
        icon.alpha_composite(piece, ((legacy - span) // 2, (legacy - span) // 2))
        icon.save(folder / "ic_launcher.png")
        layer, span = round(108 * scale), round(ICON_SPAN * scale)
        foreground = Image.new("RGBA", (layer, layer), (0, 0, 0, 0))
        foreground.alpha_composite(square.resize((span, span), Image.LANCZOS), ((layer - span) // 2, (layer - span) // 2))
        foreground.save(folder / "ic_launcher_foreground.png")
    write_adaptive(res, "#%02X%02X%02X" % fill, "@mipmap/ic_launcher_foreground")


def write_placeholder_icon(res):
    """An icon for a manifest that names @mipmap/ic_launcher when there is no picture for it: a light triangle (play)
    on dark grey, as a vector (every device the package installs on takes adaptive icons)."""
    drawable = res / "drawable"
    drawable.mkdir(parents=True)
    (drawable / "ic_launcher_foreground.xml").write_text(
        '<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="108dp" '
        'android:height="108dp" android:viewportWidth="108" android:viewportHeight="108">\n'
        '    <path android:fillColor="#E0E0E0" android:pathData="M44,36 L74,54 L44,72 Z" />\n'
        '</vector>\n', encoding="utf-8")
    write_adaptive(res, "#202124", "@drawable/ic_launcher_foreground")


def write_adaptive(res, background, foreground):
    values = res / "values"
    values.mkdir(parents=True, exist_ok=True)
    (values / "ic_launcher.xml").write_text(
        f'<resources>\n    <color name="ic_launcher_background">{background}</color>\n</resources>\n',
        encoding="utf-8")
    folder = res / "mipmap-anydpi-v26"
    folder.mkdir(parents=True)
    (folder / "ic_launcher.xml").write_text(
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        f'    <foreground android:drawable="{foreground}" />\n'
        '</adaptive-icon>\n', encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", required=True, help="AndroidManifest.xml (its package names the application)")
    parser.add_argument("--java", action="append", default=[], help="a folder of Java sources (the activity)")
    parser.add_argument("--res", help="a resources folder (res/)")
    parser.add_argument("--icon", help="a picture (a game's icon0.png, a logo) made into the launcher icon, "
                                       "@mipmap/ic_launcher (without it, a manifest naming that gets a plain "
                                       "placeholder; needs Pillow)")
    parser.add_argument("--icon-background", help="the icon's background colour, #RRGGBB (default: the picture's "
                                                  "corner's)")
    parser.add_argument("--lib", required=True, help="libmain.so (arm64-v8a)")
    parser.add_argument("--out", required=True, help="the .apk to write")
    parser.add_argument("--sce-sys", metavar="GAME", help="the game's folder: the label, version and icon from its "
                                                           "sce_sys (param.sfo, icon0.png)")
    parser.add_argument("--title-id", help="with --sce-sys: the title, when the game's folder holds several")
    parser.add_argument("--version-code", type=int, help="default: from APP_VER with --sce-sys (01.04: 104), else 1")
    parser.add_argument("--version-name", help="default: APP_VER with --sce-sys, else 0.1")
    parser.add_argument("--keep-debug", action="store_true", help="keep the library's debug information")
    parser.add_argument("--debuggable", action="store_true", help="debuggable (run-as, a debugger)")
    parser.add_argument("--install", action="store_true", help="install it on the device adb sees")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    meta = sce_sys.find(args.sce_sys, args.title_id) if args.sce_sys else None
    if args.version_code is None:
        args.version_code = meta.version_code() if meta else 1
    if args.version_name is None:
        args.version_name = meta.app_ver if meta else "0.1"
    if meta and not args.icon and meta.icon:
        args.icon = str(meta.icon)

    sdk = Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "C:/Android/Sdk")
    build_tools = newest(sdk / "build-tools")
    platform = newest(sdk / "platforms")
    ndk = Path(os.environ["ANDROID_NDK_HOME"]) if os.environ.get("ANDROID_NDK_HOME") else newest(sdk / "ndk")
    if not build_tools or not platform:
        raise SystemExit(f"no build-tools or platform in {sdk}")
    android_jar = platform / "android.jar"
    target_sdk = int(platform.name.replace("android-", ""))
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="psvrecore_apk_") as tmp:
        tmp = Path(tmp)
        # Resources and the manifest: the base package, and R.java.
        flat = []
        if args.res:
            run([build_tools / f"aapt2{EXE}", "compile", "--dir", args.res, "-o", tmp / "res.zip"])
            flat = [tmp / "res.zip"]
        icon = tmp / "icon"
        if args.icon:
            write_icon(Path(args.icon), icon, args.icon_background)
        elif "@mipmap/ic_launcher" in Path(args.manifest).read_text(encoding="utf-8") and not (
                args.res and any(Path(args.res).glob("mipmap*/ic_launcher.*"))):
            write_placeholder_icon(icon)
        manifest = Path(args.manifest)
        if meta:
            # (the application's label: @string/app_name, the title and its translations)
            sce_sys.write_android_labels(meta, icon)
            text = manifest.read_text(encoding="utf-8")
            application = re.search(r"<application\b[^>]*>", text)
            if not application:
                raise SystemExit(f"{manifest}: no <application> element")
            element = application.group(0)
            if re.search(r'android:label="[^"]*"', element):
                element = re.sub(r'android:label="[^"]*"', 'android:label="@string/app_name"', element)
            else:
                element = element.replace("<application", '<application android:label="@string/app_name"', 1)
            manifest = tmp / "AndroidManifest.xml"
            manifest.write_text(text[:application.start()] + element + text[application.end():], encoding="utf-8")
            print(f"label: {meta.title} ({len(meta.titles())} translations); version {args.version_name} "
                  f"({args.version_code})")
        if icon.is_dir():
            run([build_tools / f"aapt2{EXE}", "compile", "--dir", icon, "-o", tmp / "icon.zip"])
            flat.append(tmp / "icon.zip")
        gen = tmp / "gen"
        gen.mkdir()
        base = tmp / "base.apk"
        run([build_tools / f"aapt2{EXE}", "link", "-I", android_jar, "--manifest", manifest,
             "--min-sdk-version", MIN_SDK, "--target-sdk-version", target_sdk,
             "--version-code", args.version_code, "--version-name", args.version_name,
             "--java", gen, "-o", base, *(["--debug-mode"] if args.debuggable else []), *flat])

        # The Java: SDL's activity and the caller's, compiled against the platform and turned into dex.
        sources = [p for d in [SDL_JAVA, PSVRECORE_JAVA, *args.java, gen] for p in Path(d).rglob("*.java")]
        classes = tmp / "classes"
        classes.mkdir()
        run([jdk_tool("javac"), "-nowarn", "-Xlint:-options", "-source", "8", "-target", "8", "-encoding", "UTF-8",
             "-bootclasspath", android_jar, "-d", classes, *sources])
        jar = tmp / "classes.jar"
        with zipfile.ZipFile(jar, "w") as z:
            for c in classes.rglob("*.class"):
                z.write(c, c.relative_to(classes).as_posix())
        dex = tmp / "dex"
        dex.mkdir()
        run([build_tools / f"d8{BAT}", "--release", "--min-api", MIN_SDK, "--lib", android_jar, "--output", dex, jar],
            shell=(os.name == "nt"))

        # The native library, stripped of its debug information.
        lib = tmp / "libmain.so"
        if args.keep_debug or not ndk:
            shutil.copyfile(args.lib, lib)
        else:
            strip = next((ndk / "toolchains" / "llvm" / "prebuilt").iterdir()) / "bin" / f"llvm-strip{EXE}"
            run([strip, "--strip-debug", "-o", lib, args.lib])

        # The package: stored native code (loaded from the APK in place, 16 KiB aligned), then signed.
        unaligned = tmp / "unaligned.apk"
        shutil.copyfile(base, unaligned)
        with zipfile.ZipFile(unaligned, "a") as z:
            z.write(dex / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
            z.write(lib, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_STORED)
        aligned = tmp / "aligned.apk"
        run([build_tools / f"zipalign{EXE}", "-f", "-P", "16", "4", unaligned, aligned])
        keystore = Path.home() / ".android" / "debug.keystore"
        if not keystore.exists():
            keystore.parent.mkdir(parents=True, exist_ok=True)
            run([jdk_tool("keytool"), "-genkeypair", "-keystore", keystore, "-storepass", "android", "-alias",
                 "androiddebugkey", "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                 "-dname", "CN=Android Debug,O=Android,C=US"])
        run([build_tools / f"apksigner{BAT}", "sign", "--ks", keystore, "--ks-pass", "pass:android",
             "--key-pass", "pass:android", "--out", out, aligned], shell=(os.name == "nt"))
    print(f"{out} ({out.stat().st_size // 1024} KiB; target SDK {target_sdk}, build-tools {build_tools.name})")

    if args.install:
        print(run(["adb", "install", "-r", out]).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
