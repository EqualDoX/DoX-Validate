#!/usr/bin/env python3
"""Builds the small test fonts committed next to this script (run once; the generator only reads the results).

Input: Liberation Sans 2.1.5 Regular and Bold (SIL Open Font License 1.1, see OFL.txt), e.g. from the Arch Linux package
ttf-liberation 2.1.5 (/usr/share/fonts/liberation). Output: CorpusSans-{Regular,Bold}.ttf, subsets restricted to printable
ASCII (U+0020..U+007E) without hinting, renamed "Corpus Sans" because the OFL reserves the names "Liberation", "Arimo",
"Tinos" and "Cousine" for unmodified versions, and CorpusSans.json with the metrics the PDF generator needs.

usage: make_subset.py REGULAR.ttf BOLD.ttf   (requires fontTools, see tests/requirements.txt)
"""
import hashlib
import json
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
CODES = list(range(0x20, 0x7F))
EXPECTED_SHA256 = {
    "LiberationSans-Regular.ttf": "baccc64becc3eb7d104b7c84d99f5314a0a1f896e2b3ea6c2f22fc08d2003bee",
    "LiberationSans-Bold.ttf": "769673c4355020b1e28a14c366a152da410ab6b16239fe883ebc35b73624835b",
}


def build(src: Path, style: str) -> dict:
    digest = hashlib.sha256(src.read_bytes()).hexdigest()
    if EXPECTED_SHA256.get(src.name) != digest:
        sys.exit(f"{src}: unexpected sha256 {digest} (expected Liberation Sans 2.1.5)")
    font = TTFont(str(src))
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.glyph_names = True
    opts.notdef_outline = True
    opts.recalc_timestamp = False
    opts.drop_tables += ["GPOS", "GSUB", "GDEF", "kern", "DSIG"]
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=CODES)
    sub.subset(font)
    family, ps = "Corpus Sans", f"CorpusSans-{style}"
    for rec in font["name"].names:
        if rec.nameID in (1, 16):
            rec.string = family
        elif rec.nameID == 4:
            rec.string = f"{family} {style}"
        elif rec.nameID == 6:
            rec.string = ps
        elif rec.nameID == 3:
            rec.string = f"{ps} (test subset of Liberation Sans 2.1.5)"
        elif rec.nameID in (0,):
            rec.string = rec.toUnicode() + " Subset and renamed for a test corpus."
    font["head"].created = font["head"].modified = 0
    out = HERE / f"{ps}.ttf"
    font.save(str(out))
    font = TTFont(str(out))
    upem = font["head"].unitsPerEm
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    scale = 1000.0 / upem
    widths = [round(hmtx[cmap[c]][0] * scale) for c in CODES]
    glyph_order = font.getGlyphOrder()
    head, os2, post, hhea = font["head"], font["OS/2"], font["post"], font["hhea"]
    return {
        "file": out.name,
        "sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
        "postscript_name": ps,
        "units_per_em": upem,
        "first_char": CODES[0],
        "widths": widths,
        "gids": [glyph_order.index(cmap[c]) for c in CODES],
        "gid_widths": [round(hmtx[g][0] * scale) for g in glyph_order],
        "bbox": [round(v * scale) for v in (head.xMin, head.yMin, head.xMax, head.yMax)],
        "ascent": round(hhea.ascent * scale),
        "descent": round(hhea.descent * scale),
        "cap_height": round(getattr(os2, "sCapHeight", 0) * scale),
        "italic_angle": post.italicAngle,
        "weight": os2.usWeightClass,
    }


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    meta = {"source": "Liberation Sans 2.1.5 (SIL OFL 1.1), subset to U+0020..U+007E and renamed",
            "Regular": build(Path(sys.argv[1]), "Regular"), "Bold": build(Path(sys.argv[2]), "Bold")}
    (HERE / "CorpusSans.json").write_text(json.dumps(meta, indent=1) + "\n")


if __name__ == "__main__":
    main()
