#!/usr/bin/env python3
"""Regenerate src/assets/pixel-ui.ttf, the pixel font embedded in BatteryScope.

The source is Cubic 11 (俐方體11號) by ACh, SIL Open Font License 1.1:
https://github.com/ACh-K/Cubic-11 (fonts/ttf/Cubic_11.ttf, version 1.451).

Only the characters BatteryScope's own UI text can contain are kept: every
string literal in src/i18n.h and src/app.cpp (Chinese and English), printable
ASCII, and a few symbols. Text outside this set (process and service names,
system messages) is drawn with the system font instead. Re-run this script
after adding or changing UI strings; the panel self-test fails otherwise.

Usage: python tools/subset-pixel-font.py path/to/Cubic_11.ttf
Requires: pip install fonttools
"""
import pathlib
import re
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = [ROOT / "src" / "i18n.h", ROOT / "src" / "app.cpp"]
OUTPUT = ROOT / "src" / "assets" / "pixel-ui.ttf"
FAMILY = "BatteryScope Pixel"
EXTRA = "·…—–↓↑→←%×°：，、。；（）「」／＋－？！"
ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "0": "\0", "\\": "\\", '"': '"', "'": "'"}


def literal_characters(text: str) -> set[str]:
    characters: set[str] = set()
    for match in re.finditer(r'L"((?:[^"\\]|\\.)*)"', text):
        body = re.sub(r"\\(.)", lambda m: ESCAPES.get(m.group(1), m.group(1)), match.group(1))
        characters.update(c for c in body if c.isprintable())
    return characters


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    characters = {chr(c) for c in range(0x20, 0x7F)} | set(EXTRA)
    for source in SOURCES:
        characters |= literal_characters(source.read_text(encoding="utf-8"))
    font = TTFont(sys.argv[1])
    cmap = font.getBestCmap()
    wanted = sorted(ord(c) for c in characters)
    missing = [chr(c) for c in wanted if c not in cmap and c > 0x20]
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[c for c in wanted if c in cmap])
    subsetter.subset(font)
    # A modified OFL font is a derivative: give it its own name, keep the
    # original copyright (ID 0) and license (IDs 13 and 14).
    names = font["name"]
    for record in list(names.names):
        if record.nameID in (1, 3, 4, 6, 16, 17, 21, 22):
            names.removeNames(nameID=record.nameID)
    for platform, encoding, language in ((3, 1, 0x409), (1, 0, 0)):
        names.setName(FAMILY, 1, platform, encoding, language)
        names.setName(FAMILY + " Regular; subset of Cubic 11 1.451", 3, platform, encoding, language)
        names.setName(FAMILY, 4, platform, encoding, language)
        names.setName("BatteryScopePixel-Regular", 6, platform, encoding, language)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    font.save(OUTPUT)
    print(f"{OUTPUT.relative_to(ROOT)}: {len(wanted) - len(missing)} characters, {OUTPUT.stat().st_size} bytes")
    if missing:
        print("Not in Cubic 11 (system font fallback):", "".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
