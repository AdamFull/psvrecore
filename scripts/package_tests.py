#!/usr/bin/env python3
"""Tests of the packaging scripts (sce_sys.py, package.py) on a made-up game: a param.sfo written here and a drawn
icon0.png, no game files. Needs Pillow.

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
import sce_sys  # noqa: E402

BUILD = None
SFO_APP = {"TITLE": "Test Game: One", "STITLE": "Test", "TITLE_ID": "TEST00001", "APP_VER": "01.00",
           "CATEGORY": "gd", "TITLE_00": "テストゲーム", "TITLE_02": "Jeu d'essai", "ATTRIBUTE": 1}
SFO_PATCH = {"TITLE": "Test Game: One®", "TITLE_ID": "TEST00001", "APP_VER": "01.04", "CATEGORY": "gp",
             "TITLE_00": "テストゲーム", "TITLE_02": "Jeu d'essai"}


def make_game(folder, patch=True):
    """app/TEST00001/sce_sys (param.sfo, a 128 px opaque icon0.png) and, with patch, patch/TEST00001/sce_sys."""
    from PIL import Image, ImageDraw
    app = folder / "app" / "TEST00001" / "sce_sys"
    app.mkdir(parents=True)
    (app / "param.sfo").write_bytes(sce_sys.write_sfo(SFO_APP))
    icon = Image.new("RGBA", (128, 128), (200, 40, 90, 255))
    ImageDraw.Draw(icon).rectangle((40, 40, 88, 88), fill=(255, 255, 255, 255))
    icon.save(app / "icon0.png")
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
                program = moved / "psvrecore_shared_tests.exe"
            else:
                self.assertTrue(any(n.startswith("libpsvrecore.so.") for n in names), names)
                for n in ("psvrecore_shared_tests.desktop", "install.sh", "icons"):
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
