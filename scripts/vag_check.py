"""VAG through NGS against its source: test signals are encoded with the SDK's vagconv2, in its default mode (HE-VAG,
128 filters of up to 4 taps) and its backwards-compatible one (the 5 classic filters), and the runtime test "ngs: VAG
made by vagconv2 plays back its source, ..." plays each file through a player voice and measures it against the
source.

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of the runtime tests (--preset). The files vagconv2
makes go to --out, by default build/vag: never commit them.

    python scripts/vag_check.py --sdk C:/path/to/sdk
"""

import argparse
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST = "ngs: VAG made by vagconv2 plays back its source, HE-VAG at least as well as classic"


def signal(path, kind, seconds=4, rate=48000):
    """Music-like tones with a little noise, a quiet chord, or bright high tones (mono, never clipping)."""
    rnd = random.Random(1234)
    frames = []
    for i in range(seconds * rate):
        t = i / rate
        if kind == "music":
            glide = math.sin(2 * math.pi * (200 * t + 900 * t * t))
            chord = sum(math.sin(2 * math.pi * f * t) for f in (440.0, 554.4, 659.3)) / 3
            v = 0.45 * glide + 0.25 * chord + 0.05 * rnd.uniform(-1, 1)
        elif kind == "quiet":
            v = 0.01 * (math.sin(2 * math.pi * 220 * t) + math.sin(2 * math.pi * 330 * t))
        else:
            sweep = 12000 + 3000 * math.sin(2 * math.pi * 0.5 * t)
            v = 0.3 * math.sin(2 * math.pi * 7000 * t) + 0.3 * math.sin(2 * math.pi * sweep * t)
        frames.append(struct.pack("<h", int(v * 20000)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(frames))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset the tests were built with")
    parser.add_argument("--out", default=str(ROOT / "build" / "vag"), help="where to put what vagconv2 makes")
    args = parser.parse_args()
    if not args.sdk:
        raise SystemExit("no SDK: pass --sdk or set SCE_PSP2_SDK_DIR")
    tool = Path(args.sdk) / "host_tools" / "bin" / "vagconv2.exe"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for name in ("music", "quiet", "bright"):
        source = out / f"{name}.wav"
        signal(source, name)
        for mode, options in (("he", []), ("cb", ["/CB"])):
            # (vagconv2 names what it makes after its input: each mode gets a folder of its own)
            with tempfile.TemporaryDirectory() as work:
                command = [str(tool), *options, "/1", "/T", work, str(source)]
                result = subprocess.run(command, capture_output=True, text=True)
                vag = Path(work) / f"{name}.vag"
                if result.returncode != 0 or not vag.exists():
                    sys.stdout.write(result.stdout + result.stderr)
                    raise SystemExit(f"failed: {' '.join(command)}")
                shutil.move(str(vag), str(out / f"{name}_{mode}.vag"))
    exe = ROOT / "build" / args.preset / "tests" / ("psvrecore_runtime_tests" + (".exe" if os.name == "nt" else ""))
    env = dict(os.environ, PSVRECORE_VAG_DIR=str(out))
    result = subprocess.run([str(exe), f"--test-case={TEST}", "--verbose"], env=env, capture_output=True, text=True)
    sys.stdout.write(result.stdout + result.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
