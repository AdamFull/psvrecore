"""ATRAC9 through NGS against the SDK's own decoder: test signals are encoded with the SDK's at9tool (mono, stereo,
stereo in superframes), decoded with at9tool as the reference, and the runtime test "ngs: ATRAC9 voices play what
at9tool decodes, ..." plays each file through an NGS ATRAC9 voice (from the start, and from a seek) and compares.

Needs a PS Vita SDK (--sdk, or SCE_PSP2_SDK_DIR) and a build of the runtime tests (--preset). The files at9tool
makes go to --out, by default build/at9: never commit them.

    python scripts/at9_check.py --sdk C:/path/to/sdk
"""

import argparse
import math
import os
import random
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST = "ngs: ATRAC9 voices play what at9tool decodes, from the start and from a seek"


def signal(path, channels, seconds=4, rate=48000):
    """Tones gliding up, a chord, and some noise: content in most bands, never clipping."""
    rnd = random.Random(1234)
    frames = []
    for i in range(seconds * rate):
        t = i / rate
        glide = math.sin(2 * math.pi * (200 * t + 900 * t * t))
        chord = sum(math.sin(2 * math.pi * f * t) for f in (440.0, 554.4, 659.3)) / 3
        noise = rnd.uniform(-1, 1)
        left = 0.45 * glide + 0.25 * chord + 0.05 * noise
        right = 0.45 * chord + 0.25 * glide - 0.05 * noise
        values = (left, right)[:channels]
        frames.append(struct.pack("<" + "h" * channels, *(int(v * 20000) for v in values)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(frames))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", default=os.environ.get("SCE_PSP2_SDK_DIR"), help="the PS Vita SDK folder")
    parser.add_argument("--preset", default="windows-x64-release", help="the CMake preset the tests were built with")
    parser.add_argument("--out", default=str(ROOT / "build" / "at9"), help="where to put what at9tool makes")
    args = parser.parse_args()
    if not args.sdk:
        raise SystemExit("no SDK: pass --sdk or set SCE_PSP2_SDK_DIR")
    tool = Path(args.sdk) / "host_tools" / "bin" / "at9tool.exe"
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    variants = [("mono", 1, []), ("stereo", 2, []), ("stereo_superframe", 2, ["-supframeon"])]
    for name, channels, options in variants:
        source = out / f"{name}_source.wav"
        signal(source, channels)
        at9 = out / f"{name}.at9"
        for command in ([str(tool), "-e", *options, str(source), str(at9)], [str(tool), "-d", str(at9), str(out / f"{name}.wav")]):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                sys.stdout.write(result.stdout + result.stderr)
                raise SystemExit(f"failed: {' '.join(command)}")
        source.unlink()
    exe = ROOT / "build" / args.preset / "tests" / ("psvrecore_runtime_tests" + (".exe" if os.name == "nt" else ""))
    env = dict(os.environ, PSVRECORE_AT9_DIR=str(out))
    result = subprocess.run([str(exe), f"--test-case={TEST}", "--verbose"], env=env, capture_output=True, text=True)
    sys.stdout.write(result.stdout + result.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
