#!/usr/bin/env python3
"""Packages an Android application from a game's (or sample's) libmain.so with the Android SDK's own tools: no Gradle.

The application is SDL's Java activity (third_party/SDL/android-project, compiled here) under the caller's subclass,
the caller's manifest and resources, and the native library (built by the android-arm64 presets; debug information
stripped for the package). It is signed with the Android debug key (~/.android/debug.keystore, made with keytool when
missing), so it installs for development only.

    python scripts/android_apk.py --manifest android/AndroidManifest.xml --java android/java \\
        --lib build/android-arm64-release/libmain.so --out build/android-arm64-release/game.apk [--install]

Needs ANDROID_HOME (or ANDROID_SDK_ROOT, or C:/Android/Sdk) with build-tools and a platform, the NDK (for llvm-strip),
and a JDK (javac, keytool) on PATH or in JAVA_HOME.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", required=True, help="AndroidManifest.xml (its package names the application)")
    parser.add_argument("--java", action="append", default=[], help="a folder of Java sources (the activity)")
    parser.add_argument("--res", help="a resources folder (res/)")
    parser.add_argument("--lib", required=True, help="libmain.so (arm64-v8a)")
    parser.add_argument("--out", required=True, help="the .apk to write")
    parser.add_argument("--version-code", type=int, default=1)
    parser.add_argument("--version-name", default="0.1")
    parser.add_argument("--keep-debug", action="store_true", help="keep the library's debug information")
    parser.add_argument("--debuggable", action="store_true", help="debuggable (run-as, a debugger)")
    parser.add_argument("--install", action="store_true", help="install it on the device adb sees")
    args = parser.parse_args()

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
        gen = tmp / "gen"
        gen.mkdir()
        base = tmp / "base.apk"
        run([build_tools / f"aapt2{EXE}", "link", "-I", android_jar, "--manifest", args.manifest,
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
