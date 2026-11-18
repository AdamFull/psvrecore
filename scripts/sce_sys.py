#!/usr/bin/env python3
"""A game's sce_sys folder made into what each platform's package needs: its param.sfo (read here: the title, the
localized titles, the title ID, the application's version) and its icon (icon0.png).

Used by scripts/package.py and scripts/android_apk.py; on its own it shows what it reads:

    python scripts/sce_sys.py "game/<the game's folder>" [--title-id PCSB00867]

The game's folder is a copy of the console's folders (app/<title ID>/, patch/<title ID>/), an application folder
(eboot.bin and sce_sys/ at its top), or an sce_sys folder itself. The update's param.sfo, when there is one, is read
over the game's (its APP_VER is the version that runs); icon0.png is the update's if it has one, else the game's.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

# The system languages (SCE_SYSTEM_PARAM_LANG_*: TITLE_00 is Japanese, TITLE_01 English (US), ...) as Android resource
# qualifiers and freedesktop locale names. TITLE (no number) is the default.
LANGUAGES = [
    ("ja", "ja"), ("en", "en_US"), ("fr", "fr"), ("es", "es"), ("de", "de"), ("it", "it"), ("nl", "nl"),
    ("pt-rPT", "pt_PT"), ("ru", "ru"), ("ko", "ko"), ("zh-rTW", "zh_TW"), ("zh-rCN", "zh_CN"), ("fi", "fi"),
    ("sv", "sv"), ("da", "da"), ("nb", "nb"), ("pl", "pl"), ("pt-rBR", "pt_BR"), ("en-rGB", "en_GB"), ("tr", "tr"),
]

# Desktop icon sizes: the Windows .ico's, and the Linux hicolor theme's.
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
HICOLOR_SIZES = [16, 24, 32, 48, 64, 128, 256]

SFO_MAGIC = b"\0PSF"
SFO_UTF8_SPECIAL, SFO_UTF8, SFO_INT32 = 0x0004, 0x0204, 0x0404


def parse_sfo(data):
    """An SFO file's entries: {key: str or int}."""
    if len(data) < 20 or data[:4] != SFO_MAGIC:
        raise ValueError("not an SFO file")
    _, key_table, data_table, count = struct.unpack_from("<4I", data, 4)
    if 20 + count * 16 > len(data):
        raise ValueError("SFO index out of bounds")
    entries = {}
    for i in range(count):
        key_offset, fmt, length, max_length, value_offset = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key_at, value_at = key_table + key_offset, data_table + value_offset
        end = data.find(b"\0", key_at)
        if key_at >= len(data) or end < 0:
            raise ValueError(f"SFO entry {i}: key out of bounds")
        key = data[key_at:end].decode("utf-8")
        if length > max_length or value_at + max_length > len(data):
            raise ValueError(f"SFO entry '{key}': value out of bounds")
        raw = data[value_at:value_at + length]
        if fmt == SFO_INT32:
            if length != 4:
                raise ValueError(f"SFO entry '{key}': integer of {length} bytes")
            entries[key] = struct.unpack("<I", raw)[0]
        elif fmt == SFO_UTF8:
            entries[key] = raw.split(b"\0", 1)[0].decode("utf-8")
        elif fmt == SFO_UTF8_SPECIAL:
            entries[key] = raw.decode("utf-8")
        else:
            raise ValueError(f"SFO entry '{key}': unknown format 0x{fmt:04x}")
    return entries


def write_sfo(entries):
    """An SFO file from {key: str or int} (for tests: the format parse_sfo reads, keys in order)."""
    keys, values, index = b"", b"", b""
    for key in sorted(entries):
        value = entries[key]
        if isinstance(value, int):
            fmt, raw, size = SFO_INT32, struct.pack("<I", value), 4
        else:
            raw = value.encode("utf-8") + b"\0"
            fmt, size = SFO_UTF8, (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), fmt, len(raw), size, len(values))
        keys += key.encode("utf-8") + b"\0"
        values += raw.ljust(size, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_table = 20 + len(index)
    return SFO_MAGIC + struct.pack("<4I", 0x0101, key_table, key_table + len(keys), len(entries)) + index + keys + values


class SceSys:
    """What a game's sce_sys folders give: its param.sfo values (the update's over the game's) and its icon."""

    def __init__(self, sfo, icon, folders):
        self.sfo = sfo
        self.icon = icon  # icon0.png (None: none)
        self.folders = folders  # the sce_sys folders read, the game's first

    @property
    def title(self):
        return self.sfo.get("TITLE") or self.sfo.get("STITLE") or self.title_id

    @property
    def title_id(self):
        return self.sfo.get("TITLE_ID", "")

    @property
    def app_ver(self):
        return self.sfo.get("APP_VER", "01.00")

    def titles(self):
        """The localized titles that differ from the default: {language index: title}."""
        out = {}
        for i in range(len(LANGUAGES)):
            t = self.sfo.get(f"TITLE_{i:02d}")
            if t and t != self.title:
                out[i] = t
        return out

    def version(self):
        """APP_VER ("01.04") as numbers: (1, 4)."""
        parts = [int(p) if p.isdigit() else 0 for p in self.app_ver.split(".")]
        return tuple((parts + [0, 0])[:2])

    def version_code(self):
        """An increasing integer for the version (Android's versionCode): 01.04 -> 104."""
        major, minor = self.version()
        return major * 100 + minor

    def describe(self):
        return {"title": self.title, "titles": {LANGUAGES[i][1]: t for i, t in self.titles().items()},
                "title_id": self.title_id, "app_ver": self.app_ver, "icon": str(self.icon) if self.icon else None,
                "sce_sys": [str(f) for f in self.folders]}


def find(game, title_id=None):
    """The game's sce_sys folders (see the module's description). title_id picks the title when app/ holds several;
    without it, the only one there."""
    game = Path(game)
    if (game / "param.sfo").is_file():
        folders = [game]
    elif (game / "sce_sys" / "param.sfo").is_file():
        folders = [game / "sce_sys"]
    elif (game / "app").is_dir():
        if not title_id:
            ids = sorted(p.name for p in (game / "app").iterdir() if (p / "sce_sys" / "param.sfo").is_file())
            if len(ids) != 1:
                raise SystemExit(f"{game}/app: {'no title' if not ids else 'several titles: ' + ', '.join(ids)} "
                                 f"with sce_sys/param.sfo (give the title ID)")
            title_id = ids[0]
        folders = [game / sub / title_id / "sce_sys" for sub in ("app", "patch")]
        folders = [f for f in folders if (f / "param.sfo").is_file()]
        if not folders:
            raise SystemExit(f"{game}: no app/{title_id}/sce_sys/param.sfo")
    else:
        raise SystemExit(f"{game}: no sce_sys/param.sfo, nor app/<title ID>/sce_sys/param.sfo")
    sfo = {}
    for folder in folders:
        sfo.update(parse_sfo((folder / "param.sfo").read_bytes()))
    icons = [f / "icon0.png" for f in reversed(folders) if (f / "icon0.png").is_file()]
    return SceSys(sfo, icons[0] if icons else None, folders)


# --- Pictures ---------------------------------------------------------------------------------------------------

def _pil():
    try:
        from PIL import Image, ImageChops, ImageDraw
    except ImportError:
        raise SystemExit("the icons need Pillow (pip install pillow)")
    return Image, ImageChops, ImageDraw


def square_icon(picture, bubble):
    """A square RGBA picture for desktop icons. bubble: the console's icon (icon0.png), shown as it shows it, in a
    round bubble (a picture already round, with transparent corners, stays as it is). Otherwise (a logo): its
    artwork (what is not transparent, or differs from its corner's colour) in the middle of a transparent square."""
    Image, ImageChops, ImageDraw = _pil()
    with Image.open(picture) as opened:
        src = opened.convert("RGBA")
    if bubble:
        side = max(src.size)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(src, ((side - src.width) // 2, (side - src.height) // 2))
        if square.getpixel((0, 0))[3] == 255:
            # (the circle drawn 4x larger and reduced: smooth edges)
            mask = Image.new("L", (side * 4, side * 4), 0)
            ImageDraw.Draw(mask).ellipse((0, 0, side * 4 - 1, side * 4 - 1), fill=255)
            mask = mask.resize((side, side), Image.LANCZOS)
            square.putalpha(ImageChops.multiply(square.getchannel("A"), mask))
        return square
    corner = src.getpixel((0, 0))
    if corner[3] < 255:
        box = src.getchannel("A").point(lambda a: 255 if a > 16 else 0).getbbox()
    else:
        flat = Image.new("RGBA", src.size, corner)
        box = ImageChops.difference(src, flat).convert("L").point(lambda d: 255 if d > 24 else 0).getbbox()
    art = src.crop(box or (0, 0, *src.size))
    side = max(art.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(art, ((side - art.width) // 2, (side - art.height) // 2))
    return square


def resized(square, size):
    Image, _, _ = _pil()
    return square.resize((size, size), Image.LANCZOS)


def write_ico(square, path):
    """A Windows .ico of every size in ICO_SIZES, each a PNG (as Windows Vista and later read them)."""
    import io
    images = []
    for size in ICO_SIZES:
        buf = io.BytesIO()
        resized(square, size).save(buf, "PNG", optimize=True)
        images.append((size, buf.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for size, png in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset + len(blobs))
        blobs += png
    path.write_bytes(header + entries + blobs)


def write_hicolor(square, folder, name):
    """The icon in the hicolor theme's layout: <folder>/<n>x<n>/apps/<name>.png."""
    for size in HICOLOR_SIZES:
        out = folder / f"{size}x{size}" / "apps"
        out.mkdir(parents=True, exist_ok=True)
        resized(square, size).save(out / f"{name}.png", optimize=True)


# --- Text files ---------------------------------------------------------------------------------------------------

def _desktop_string(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")


def desktop_entry(meta, name):
    """A freedesktop .desktop entry for the program <name> (in the same folder; install.sh writes its path)."""
    lines = ["[Desktop Entry]", "Type=Application", "Version=1.5", f"Name={_desktop_string(meta.title)}"]
    for i, t in meta.titles().items():
        lines.append(f"Name[{LANGUAGES[i][1]}]={_desktop_string(t)}")
    lines += [f"Comment={_desktop_string(f'{meta.title} ({meta.title_id}, version {meta.app_ver}), recompiled for this computer')}",
              f"Exec={name}", f"Icon={name}", "Terminal=false", "Categories=Game;",
              f"StartupWMClass={name}", "PrefersNonDefaultGPU=true", f"X-Psvrecore-TitleId={meta.title_id}"]
    return "\n".join(lines) + "\n"


def _rc_string(s):
    return s.replace("\\", "\\\\").replace('"', '""')


def windows_rc(meta, name, ico):
    """A Windows resource script: the icon (the window's and Explorer's: SDL takes the program's first icon) and the
    version information, in UTF-8 (#pragma code_page)."""
    major, minor = meta.version()
    fields = {
        "FileDescription": meta.title,
        "ProductName": meta.title,
        "ProductVersion": f"{meta.app_ver} ({meta.title_id})",
        "FileVersion": f"{major}.{minor}.0.0",
        "InternalName": name,
        "OriginalFilename": f"{name}.exe",
        "Comments": f"{meta.title_id} {meta.app_ver}, recompiled with psvrecore",
    }
    values = "\n".join(f'            VALUE "{k}", "{_rc_string(v)}"' for k, v in fields.items())
    icon = str(Path(ico).resolve()).replace("\\", "/")
    return f"""// Made by psvrecore's scripts/package.py from the game's sce_sys: never committed.
#pragma code_page(65001)
#include <winver.h>

1 ICON "{_rc_string(icon)}"

VS_VERSION_INFO VERSIONINFO
FILEVERSION {major},{minor},0,0
PRODUCTVERSION {major},{minor},0,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
{values}
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"""


def _xml_text(s):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    # (Android's string resources: quotes and apostrophes escaped, and a leading @ or ? is not a reference)
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("'", "\\'")
    return "\\" + s if s[:1] in ("@", "?") else s


def write_android_labels(meta, res):
    """@string/app_name: the title, and the localized ones as values-<language>/strings.xml."""
    def write(folder, text):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "strings.xml").write_text(
            f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <string name="app_name">{_xml_text(text)}'
            f'</string>\n</resources>\n', encoding="utf-8")
    write(res / "values", meta.title)
    for i, t in meta.titles().items():
        write(res / f"values-{LANGUAGES[i][0]}", t)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="the game's folder, an application folder or an sce_sys folder")
    ap.add_argument("--title-id", help="the title, when app/ holds several")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(find(args.game, args.title_id).describe(), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
