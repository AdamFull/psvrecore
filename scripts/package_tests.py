#!/usr/bin/env python3
"""Tests of the packaging scripts (sce_sys.py, package.py, android_apk.py's resources) on a made-up game: a
param.sfo written here, a drawn icon0.png and LiveArea (template.xml, an indexed background, a gate), no game files.
Needs Pillow.

    python scripts/package_tests.py [--build <a psvrecore build folder>]
    python scripts/package_tests.py --make-game <folder>   # only writes the made-up game's folder (CI packages it)

With --build (a build with PSVRECORE_SHARED, as CI's), psvrecore_shared_tests is packaged as a game's program for
this computer and run from the package's folder: the runtime library is found beside it (Linux: $ORIGIN).
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import android_apk  # noqa: E402
import sce_sys  # noqa: E402

BUILD = None
SFO_APP = {"TITLE": "Test Game: One", "STITLE": "Test", "TITLE_ID": "TEST00001", "APP_VER": "01.00",
           "CATEGORY": "gd", "TITLE_00": "テストゲーム", "TITLE_02": "Jeu d'essai", "ATTRIBUTE": 1}
SFO_PATCH = {"TITLE": "Test Game: One®", "TITLE_ID": "TEST00001", "APP_VER": "01.04", "CATEGORY": "gp",
             "TITLE_00": "テストゲーム", "TITLE_02": "Jeu d'essai"}


# The made-up LiveArea: as the console's templates are written (a byte order mark, single quotes, a comment, frames).
TEMPLATE = """\ufeff<?xml version='1.0' encoding='utf-8'?>

<livearea style='a1' format-ver='01.00' content-rev='1'>
\t<livearea-background>
\t\t<image>bg0.png</image>
\t</livearea-background>
\t<!-- <gate><startup-image>old.png</startup-image></gate> -->
\t<gate>
\t\t<startup-image>startup.png</startup-image>
\t</gate>
\t<frame id='frame1'>
\t\t<liveitem>
\t\t\t<background>frame1.png</background>
\t\t\t<text align='center' valign='bottom'><str color='#ffffff' size='24' ></str></text>
\t\t</liveitem>
\t</frame>
</livearea>
"""
BG_EDGE, BG_INSIDE, GATE = (20, 60, 140), (90, 30, 30), (240, 240, 240)


def make_livearea(contents):
    """template.xml, bg0.png (840 x 500, indexed: its edge, a darker inside and a transparent square) and startup.png
    (280 x 158)."""
    from PIL import Image
    contents.mkdir(parents=True)
    (contents / "template.xml").write_bytes(TEMPLATE.encode("utf-8"))
    bg = Image.new("P", (840, 500), 0)
    bg.putpalette([*BG_EDGE, *BG_INSIDE, 255, 0, 255] + [0] * (256 - 3) * 3)
    bg.paste(1, (60, 40, 780, 460))
    bg.paste(2, (100, 100, 200, 200))  # (transparent: black on the screen)
    bg.save(contents / "bg0.png", transparency=2)
    Image.new("RGB", (280, 158), GATE).save(contents / "startup.png")


def make_game(folder, patch=True, livearea=True):
    """app/TEST00001/sce_sys (param.sfo, a 128 px opaque icon0.png, with livearea a LiveArea) and, with patch,
    patch/TEST00001/sce_sys."""
    from PIL import Image, ImageDraw
    app = folder / "app" / "TEST00001" / "sce_sys"
    app.mkdir(parents=True)
    (app / "param.sfo").write_bytes(sce_sys.write_sfo(SFO_APP))
    icon = Image.new("RGBA", (128, 128), (200, 40, 90, 255))
    ImageDraw.Draw(icon).rectangle((40, 40, 88, 88), fill=(255, 255, 255, 255))
    icon.save(app / "icon0.png")
    if livearea:
        make_livearea(app / "livearea" / "contents")
    if patch:
        p = folder / "patch" / "TEST00001" / "sce_sys"
        p.mkdir(parents=True)
        (p / "param.sfo").write_bytes(sce_sys.write_sfo(SFO_PATCH))
    return folder


def package(*args):
    result = subprocess.run([sys.executable, str(HERE / "package.py"), *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8")
    if result.returncode != 0:
        raise AssertionError(f"package.py {' '.join(map(str, args))}:\n{result.stdout}{result.stderr}")
    return result.stdout


class Sfo(unittest.TestCase):
    def test_round_trip(self):
        self.assertEqual(sce_sys.parse_sfo(sce_sys.write_sfo(SFO_APP)), SFO_APP)

    def test_errors(self):
        data = sce_sys.write_sfo({"TITLE": "x"})
        with self.assertRaises(ValueError):
            sce_sys.parse_sfo(b"PSF\0" + data[4:])
        with self.assertRaises(ValueError):
            sce_sys.parse_sfo(data[:-4])  # (the value past the end)
        bad = bytearray(data)
        struct.pack_into("<H", bad, 22, 0x0999)  # (the first entry's format)
        with self.assertRaises(ValueError):
            sce_sys.parse_sfo(bytes(bad))


class Find(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="psvrecore_package_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_update_over_game(self):
        meta = sce_sys.find(make_game(self.tmp / "game"))
        self.assertEqual(meta.title, "Test Game: One®")
        self.assertEqual(meta.title_id, "TEST00001")
        self.assertEqual(meta.app_ver, "01.04")
        self.assertEqual(meta.version(), (1, 4))
        self.assertEqual(meta.version_code(), 104)
        self.assertEqual(meta.titles(), {0: "テストゲーム", 2: "Jeu d'essai"})
        self.assertEqual(meta.icon, self.tmp / "game" / "app" / "TEST00001" / "sce_sys" / "icon0.png")

    def test_application_and_sce_sys_folders(self):
        make_game(self.tmp / "game", patch=False)
        app = self.tmp / "game" / "app" / "TEST00001"
        for folder in (app, app / "sce_sys"):
            meta = sce_sys.find(folder)
            self.assertEqual((meta.title, meta.app_ver), ("Test Game: One", "01.00"))

    def test_several_titles(self):
        make_game(self.tmp / "game")
        shutil.copytree(self.tmp / "game" / "app" / "TEST00001", self.tmp / "game" / "app" / "TEST00002")
        with self.assertRaises(SystemExit):
            sce_sys.find(self.tmp / "game")
        self.assertEqual(sce_sys.find(self.tmp / "game", "TEST00002").app_ver, "01.00")


class LiveArea(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="psvrecore_package_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_templates(self):
        self.assertEqual(sce_sys.livearea_pictures(TEMPLATE.encode("utf-8")), ("bg0.png", "startup.png"))
        # (double quotes, a declaration, entities, an element left open, a language's picture and the default)
        text = """<?xml version="1.0"?><!DOCTYPE livearea>
<livearea style="a5"><livearea-background><image lang="ja">bg_ja.png</image><image>b&amp;g.png</image>
<gate><startup-image> gate.png </startup-image></gate></livearea>"""
        self.assertEqual(sce_sys.livearea_pictures(text), ("b&g.png", "gate.png"))
        self.assertEqual(sce_sys.livearea_pictures("<livearea><gate><startup-image>g.png</startup-image></gate>"
                                                   "</livearea>"), (None, "g.png"))
        self.assertEqual(sce_sys.livearea_pictures("<livearea><frame id='frame1'/></livearea>"), (None, None))
        self.assertEqual(sce_sys.livearea_pictures("not xml at all"), (None, None))
        self.assertEqual(sce_sys.livearea_pictures(
            "<livearea><livearea-background><image><![CDATA[bg1.png]]></image></livearea-background>"
            "</livearea>"), ("bg1.png", None))

    def test_folders(self):
        game = make_game(self.tmp / "game")
        app = game / "app" / "TEST00001" / "sce_sys" / "livearea" / "contents"
        meta = sce_sys.find(game)
        self.assertEqual(meta.livearea(), (app / "bg0.png", app / "startup.png"))
        # (the update's template over the game's; its pictures where they are, the update's first)
        patch = game / "patch" / "TEST00001" / "sce_sys" / "livearea" / "contents"
        patch.mkdir(parents=True)
        (patch / "template.xml").write_text("<livearea><livearea-background><image>bg0.png</image>"
                                            "</livearea-background><gate><startup-image>../../param.sfo"
                                            "</startup-image></gate></livearea>", encoding="utf-8")
        self.assertEqual(sce_sys.find(game).livearea(), (app / "bg0.png", None))
        shutil.copy(app / "bg0.png", patch / "bg0.png")
        self.assertEqual(sce_sys.find(game).livearea(), (patch / "bg0.png", None))
        # (no livearea folder: no pictures)
        self.assertEqual(sce_sys.find(make_game(self.tmp / "plain", livearea=False)).livearea(), (None, None))
        self.assertIsNone(sce_sys.launch_picture(None, None))

    def test_launch_picture(self):
        game = make_game(self.tmp / "game")
        picture, colour = sce_sys.launch_picture(*sce_sys.find(game).livearea())
        self.assertEqual(picture.size, sce_sys.LAUNCH_SIZE)
        self.assertEqual(picture.mode, "RGB")
        # (840 x 500 fitted: 914 x 544 from x 23; bars of the edge's colour; the gate in the middle, 305 x 172)
        self.assertEqual(colour, sce_sys.ring_colour(picture))
        self.assertTrue(all(abs(a - b) <= 2 for a, b in zip(colour, BG_EDGE)), colour)
        self.assertEqual(picture.getpixel((5, 272)), BG_EDGE)
        self.assertEqual(picture.getpixel((480, 272)), GATE)
        self.assertEqual(picture.getpixel((480 - 140, 272)), GATE)
        self.assertEqual(picture.getpixel((480 - 170, 272)), BG_INSIDE)
        self.assertEqual(picture.getpixel((23 + 163, 163)), (0, 0, 0))  # (the transparent square)

    def test_android_resources(self):
        res = self.tmp / "res"
        manifest = ('<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="x">\n'
                    '    <application android:label="X" android:theme="@android:style/Theme.Black.NoTitleBar">\n'
                    '    </application>\n</manifest>\n')
        (res / "mipmap-anydpi-v26").mkdir(parents=True)
        text = android_apk.write_launch(res, (20, 60, 140), manifest)
        self.assertIn('android:theme="@style/PsvrecoreLaunch"', text)
        self.assertNotIn("Theme.Black.NoTitleBar", text)
        values = (res / "values" / "psvrecore_launch.xml").read_text(encoding="utf-8")
        self.assertIn('<color name="psvrecore_launch">#143C8C</color>', values)
        self.assertIn('parent="@android:style/Theme.Black.NoTitleBar"', values)
        self.assertIn("@drawable/psvrecore_launch_window", values)
        self.assertNotIn("windowSplashScreen", values)
        v31 = (res / "values-v31" / "psvrecore_launch.xml").read_text(encoding="utf-8")
        self.assertIn('<item name="android:windowSplashScreenBackground">@color/psvrecore_launch</item>', v31)
        window = (res / "drawable" / "psvrecore_launch_window.xml").read_text(encoding="utf-8")
        self.assertIn("@mipmap/ic_launcher", window)
        # (a manifest without a theme gets one)
        plain = android_apk.write_launch(self.tmp / "res2", (0, 0, 0), manifest.replace(
            ' android:theme="@android:style/Theme.Black.NoTitleBar"', ""))
        self.assertIn('<application android:theme="@style/PsvrecoreLaunch" android:label="X">', plain)


class Assets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="psvrecore_package_"))
        cls.game = make_game(cls.tmp / "game")
        cls.root = cls.tmp / "repo"
        cls.root.mkdir()
        package("assets", "--root", cls.root, "--name", "testgame", "--game", cls.game)
        cls.out = cls.root / "generated" / "package"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_icons(self):
        from PIL import Image
        with Image.open(self.out / "testgame.ico") as ico:
            self.assertEqual(sorted(ico.info["sizes"]), [(s, s) for s in sce_sys.ICO_SIZES])
        for size in sce_sys.HICOLOR_SIZES:
            with Image.open(self.out / "hicolor" / f"{size}x{size}" / "apps" / "testgame.png") as png:
                self.assertEqual(png.size, (size, size))
        # (the console's round bubble: transparent corners, the picture's colour inside)
        with Image.open(self.out / "icon.png") as f:
            big = f.convert("RGBA")
        self.assertEqual(big.getpixel((0, 0))[3], 0)
        self.assertEqual(big.getpixel((128, 20)), (200, 40, 90, 255))

    def test_desktop_entry(self):
        text = (self.out / "testgame.desktop").read_text(encoding="utf-8")
        lines = text.splitlines()
        self.assertEqual(lines[0], "[Desktop Entry]")
        for line in ("Type=Application", "Name=Test Game: One®", "Name[ja]=テストゲーム", "Name[fr]=Jeu d'essai",
                     "Exec=testgame", "Icon=testgame", "StartupWMClass=testgame", "Categories=Game;"):
            self.assertIn(line, lines)
        validate = shutil.which("desktop-file-validate")
        if validate:
            result = subprocess.run([validate, str(self.out / "testgame.desktop")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_windows_resources(self):
        rc = (self.out / "testgame.rc").read_text(encoding="utf-8")
        self.assertIn("#pragma code_page(65001)", rc)
        self.assertIn("FILEVERSION 1,4,0,0", rc)
        self.assertIn('VALUE "ProductName", "Test Game: One®"', rc)
        self.assertIn('VALUE "OriginalFilename", "testgame.exe"', rc)
        self.assertIn("testgame.ico", rc)

    def test_android_labels(self):
        res = self.tmp / "res"
        sce_sys.write_android_labels(sce_sys.find(self.game), res)
        self.assertIn(">Test Game: One®<", (res / "values" / "strings.xml").read_text(encoding="utf-8"))
        self.assertIn(">テストゲーム<", (res / "values-ja" / "strings.xml").read_text(encoding="utf-8"))
        self.assertIn(">Jeu d\\'essai<", (res / "values-fr" / "strings.xml").read_text(encoding="utf-8"))

    def test_own_picture(self):
        from PIL import Image
        logo = self.tmp / "logo.png"
        picture = Image.new("RGBA", (300, 100), (0, 0, 0, 0))
        picture.paste((10, 200, 30, 255), (100, 20, 220, 80))
        picture.save(logo)
        root = self.tmp / "repo2"
        root.mkdir()
        package("assets", "--root", root, "--name", "testgame", "--game", self.game, "--icon", logo)
        with Image.open(root / "generated" / "package" / "icon.png") as f:
            big = f.convert("RGBA")
        # (the artwork, 120x60, made square: centred, not rounded)
        self.assertEqual(big.getpixel((128, 128)), (10, 200, 30, 255))
        self.assertEqual(big.getpixel((2, 128)), (10, 200, 30, 255))
        self.assertEqual(big.getpixel((128, 2))[3], 0)

    def test_launch_screen(self):
        from PIL import Image
        with Image.open(self.out / sce_sys.LAUNCH_FILE) as f:
            self.assertEqual(f.size, sce_sys.LAUNCH_SIZE)
        import json
        info = json.loads((self.out / "sce_sys.json").read_text(encoding="utf-8"))
        self.assertEqual(info["launch"]["file"], sce_sys.LAUNCH_FILE)
        self.assertTrue(info["livearea"]["background"].endswith("bg0.png"))
        # (a game without a LiveArea: none, and an earlier one's removed)
        root = self.tmp / "repo3"
        (root / "generated" / "package").mkdir(parents=True)
        (root / "generated" / "package" / sce_sys.LAUNCH_FILE).write_bytes(b"old")
        package("assets", "--root", root, "--name", "testgame", "--game",
                make_game(self.tmp / "plain", livearea=False))
        self.assertFalse((root / "generated" / "package" / sce_sys.LAUNCH_FILE).exists())
        info = json.loads((root / "generated" / "package" / "sce_sys.json").read_text(encoding="utf-8"))
        self.assertIsNone(info["launch"])


class Program(unittest.TestCase):
    """psvrecore_shared_tests packaged as a game's program, and run from the package's folder."""

    def setUp(self):
        if not os.environ.get("PSVRECORE_PACKAGE_BUILD"):
            self.skipTest("no --build")

    def test_package_runs(self):
        build = Path(os.environ["PSVRECORE_PACKAGE_BUILD"]).resolve()
        platform = "windows" if os.name == "nt" else "linux"
        with tempfile.TemporaryDirectory(prefix="psvrecore_package_") as tmp:
            tmp = Path(tmp)
            game = make_game(tmp / "game")
            root = tmp / "repo"
            root.mkdir()
            package(platform, "--root", root, "--name", "psvrecore_shared_tests", "--game", game, "--no-build",
                    "--build-dir", build, "--out", tmp / "out")
            folder = tmp / "out" / "psvrecore_shared_tests"
            moved = tmp / "elsewhere"  # (the folder copied away from the build: nothing found through it)
            shutil.move(str(folder), str(moved))
            names = sorted(p.name for p in moved.iterdir())
            if platform == "windows":
                self.assertIn("psvrecore.dll", names)
                self.assertIn("install.ps1", names)
                self.assertIn(sce_sys.LAUNCH_FILE, names)
                program = moved / "psvrecore_shared_tests.exe"
            else:
                self.assertTrue(any(n.startswith("libpsvrecore.so.") for n in names), names)
                for n in ("psvrecore_shared_tests.desktop", "install.sh", "icons", sce_sys.LAUNCH_FILE):
                    self.assertIn(n, names)
                program = moved / "psvrecore_shared_tests"
                data = tmp / "data"
                env = dict(os.environ, XDG_DATA_HOME=str(data))
                subprocess.run(["sh", str(moved / "install.sh")], check=True, env=env, capture_output=True)
                entry = (data / "applications" / "psvrecore_shared_tests.desktop").read_text(encoding="utf-8")
                self.assertIn(f'Exec="{moved}/psvrecore_shared_tests"', entry)
                self.assertTrue((data / "icons" / "hicolor" / "48x48" / "apps" / "psvrecore_shared_tests.png").is_file())
                subprocess.run(["sh", str(moved / "install.sh"), "--uninstall"], check=True, env=env,
                               capture_output=True)
                self.assertFalse((data / "applications" / "psvrecore_shared_tests.desktop").exists())
            env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
            result = subprocess.run([str(program)], cwd=tmp, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", help="a psvrecore build folder with PSVRECORE_SHARED (psvrecore_shared_tests)")
    ap.add_argument("--make-game", help="write the made-up game's folder there and stop")
    args, rest = ap.parse_known_args()
    if args.make_game:
        make_game(Path(args.make_game))
        sys.exit(0)
    if args.build:
        os.environ["PSVRECORE_PACKAGE_BUILD"] = args.build
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
