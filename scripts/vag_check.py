"""VAG through NGS against its source: test signals are encoded with the SDK's vagconv2, in its default mode (HE-VAG,
128 filters of up to 4 taps) and its backwards-compatible one (the 5 classic filters), and the runtime test "ngs: VAG
made by vagconv2 plays back its source, ..." plays each file through a player voice and measures it against the
source. A stereo signal (the music on the left, the bright tones on the right) is encoded the same way, and the
SDK's own stereo VAG files are copied next to their sources, for the test "ngs: stereo VAG made by vagconv2 plays back
both channels of its source, ...".

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
TESTS = (
    "ngs: VAG made by vagconv2 plays back its source, HE-VAG at least as well as classic",
    "ngs: stereo VAG made by vagconv2 plays back both channels of its source, from one buffer or streamed",
)


def samples(kind, seconds=4, rate=48000):
    """Music-like tones with a little noise, a quiet chord, or bright high tones (never clipping)."""
    rnd = random.Random(1234)
    out = []
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
        out.append(int(v * 20000))
    return out


def signal(path, kinds, rate=48000):
    """A WAV of one signal per channel."""
    channels = [samples(kind, rate=rate) for kind in kinds]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack(f"<{len(channels)}h", *frame) for frame in zip(*channels)))


def encode(tool, options, source, target):
    """vagconv2 on `source`, its VAG moved to `target`."""
    # (vagconv2 names what it makes after its input: each run gets a folder of its own)
    with tempfile.TemporaryDirectory() as work:
        command = [str(tool), *options, "/1", "/T", work, str(source)]
        result = subprocess.run(command, capture_output=True, text=True)
        vag = Path(work) / (source.stem + ".vag")
        if result.returncode != 0 or not vag.exists():
            sys.stdout.write(result.stdout + result.stderr)
            raise SystemExit(f"failed: {' '.join(command)}")
        shutil.move(str(vag), str(target))


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
        signal(source, [name])
        for mode, options in (("he", []), ("cb", ["/CB"])):
            encode(tool, options, source, out / f"{name}_{mode}.vag")
    signal(out / "duet.wav", ["music", "bright"])
    encode(tool, [], out / "duet.wav", out / "duet_st.vag")
    # (the SDK's stereo VAG, made by vagconv2 from the WAV beside it)
    wave_dir = Path(args.sdk) / "target" / "samples" / "data" / "audio_video" / "sound" / "wave"
    for name in ("stereo_res", "stereo_str"):
        if (wave_dir / f"{name}-LR.vag").exists() and (wave_dir / f"{name}.wav").exists():
            shutil.copyfile(wave_dir / f"{name}-LR.vag", out / f"{name}_st.vag")
            shutil.copyfile(wave_dir / f"{name}.wav", out / f"{name}.wav")
    exe = ROOT / "build" / args.preset / "tests" / ("psvrecore_runtime_tests" + (".exe" if os.name == "nt" else ""))
    env = dict(os.environ, PSVRECORE_VAG_DIR=str(out))
    failed = 0
    for test in TESTS:
        result = subprocess.run([str(exe), f"--test-case={test}", "--verbose"], env=env, capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        failed |= result.returncode
    return failed


if __name__ == "__main__":
    sys.exit(main())
