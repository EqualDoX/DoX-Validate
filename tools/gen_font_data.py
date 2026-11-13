"""Generates the font data tables of dox-validate from the veraPDF 1.30.2 parser sources.

    python3 -I tools/gen_font_data.py VERAPDF_SRC [OUT_DIR]

VERAPDF_SRC is the root of the veraPDF 1.30.2 source tree (the directory that contains parser/). The
script only *reads* files there (Java sources and resource files); nothing is executed. OUT_DIR defaults
to src/fonts/generated. Outputs (deterministic, commit them):

  font_tables.cpp   Adobe Glyph List (veraPDF's own parser/font/AdobeGlyphList.txt, which differs from the
                    AGL of fontTools: veraPDF has no uniXXXX/uXXXX rule and maps .notdef to U+0000),
                    the simple-font encodings and Mac glyph tables (org.verapdf.pd.font.truetype.
                    TrueTypePredefined), the CFF predefined strings/encodings/charsets (cff.CFFPredefined),
                    the Symbol / ZapfDingbats glyph-name sets (type1.SymbolSet, type1.ZapfDingbats) and the
                    standard-14 metrics (parser/font/stdmetrics/*.afm, read like stdmetrics.AFMParser).
  font_cmaps.cpp    every predefined CMap resource of veraPDF (parser/font/cmap/*), pre-tokenized into a
                    compact operation stream (one zlib-compressed record per CMap) that the C++ CMap builder
                    replays with the same semantics as org.verapdf.pd.font.cmap.CMapParser.

The data are Adobe resources (Adobe Glyph List, CMap resources, Core 14 AFM files) as redistributed with
veraPDF; their notices are kept in the generated files.
"""
from __future__ import annotations

import re
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


# ----------------------------------------------------------------------------------------- Java arrays
def java_string_array(src: str, name: str) -> list[str | None]:
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit(f"array {name} not found")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out: list[str | None] = []
    for tok in re.finditer(r'"((?:[^"\\]|\\.)*)"|\bnull\b', body):
        if tok.group(0) == "null":
            out.append(None)
        else:
            out.append(bytes(tok.group(1), "latin-1").decode("unicode_escape"))
    return out


def java_int_array(src: str, name: str) -> list[int]:
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit(f"array {name} not found")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(x) for x in re.findall(r"-?\d+", body)]


def java_map_puts(src: str, name: str) -> list[tuple[str, int]]:
    return [(k, int(v)) for k, v in re.findall(re.escape(name) + r'\.put\("((?:[^"\\]|\\.)*)",\s*(-?\d+)\)', src)]


def java_set_adds(src: str) -> list[str]:
    return re.findall(r'glyphNames\.add\("((?:[^"\\]|\\.)*)"\)', src)


# ----------------------------------------------------------------------------------------- C++ output
def cstr(s: str | None) -> str:
    if s is None:
        return "nullptr"
    out = []
    for ch in s.encode("latin-1"):
        if ch in (0x22, 0x5C):
            out.append("\\" + chr(ch))
        elif 0x20 <= ch < 0x7F and ch != 0x3F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    return '"' + "".join(out) + '"'


def blob_literal(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 64):
        lines.append(indent + '"' + "".join("\\%03o" % b for b in data[i:i + 64]) + '"')
    return "\n".join(lines) if lines else indent + '""'


def string_table(name: str, items: list[str | None]) -> str:
    rows = ",\n".join("    " + cstr(s) for s in items)
    return f"const char* const {name}[{len(items)}] = {{\n{rows}\n}};\n"


# ----------------------------------------------------------------------------------------- AFM
def parse_afm(path: Path) -> dict:
    """stdmetrics.AFMParser: FontName, EncodingScheme, CharacterSet, FontBBox and 'C code ; WX w ; N name'
    lines (widths by glyph name; a later line with the same name replaces the earlier one)."""
    res = {"widths": {}, "codes": [], "font_name": None, "encoding_scheme": None, "charset": None}
    for line in path.read_text("latin-1").splitlines():
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if key == "FontName":
            res["font_name"] = line[len("FontName"):].strip()
        elif key == "EncodingScheme":
            res["encoding_scheme"] = line[len("EncodingScheme"):].strip()
        elif key == "CharacterSet":
            res["charset"] = line[len("CharacterSet"):].strip()
        elif key == "C":
            fields = {}
            for seg in line.split(";"):
                seg = seg.split()
                if len(seg) >= 2:
                    fields[seg[0]] = seg[1]
            code = int(fields["C"])
            width = int(fields["WX"])
            name = fields["N"]
            res["widths"][name] = width
            res["codes"].append((code, name, width))
    return res


# ----------------------------------------------------------------------------------------- CMaps
TOK = re.compile(rb"\s*(?:(%[^\r\n]*)|(<[0-9A-Fa-f\s]*>)|(\[)|(\])|(/[^\s/<>\[\]{}()%]*)|(\((?:[^)\\]|\\.)*\))|"
                 rb"(\{)|(\})|(<<)|(>>)|([^\s/<>\[\]{}()%]+))")


def tokens(data: bytes):
    pos = 0
    while pos < len(data):
        m = TOK.match(data, pos)
        if not m:
            if data[pos:].strip() == b"":
                return
            raise SystemExit(f"cannot tokenize CMap at {pos}: {data[pos:pos + 40]!r}")
        pos = m.end()
        if m.group(1) is not None:
            continue
        if m.group(2) is not None:
            h = re.sub(rb"\s", b"", m.group(2)[1:-1])
            if len(h) % 2:
                h += b"0"
            yield ("hex", bytes.fromhex(h.decode()))
        elif m.group(3) is not None:
            yield ("[", None)
        elif m.group(4) is not None:
            yield ("]", None)
        elif m.group(5) is not None:
            yield ("name", m.group(5)[1:].decode("latin-1"))
        elif m.group(6) is not None:
            yield ("str", m.group(6))
        elif m.group(7) is not None or m.group(8) is not None or m.group(9) is not None or m.group(10) is not None:
            yield ("punct", m.group(0).strip())
        else:
            w = m.group(11).decode("latin-1")
            if re.fullmatch(r"[+-]?\d+", w):
                yield ("int", int(w))
            elif re.fullmatch(r"[+-]?\d*\.\d*", w):
                yield ("real", float(w))
            else:
                yield ("kw", w)


def varint(n: int) -> bytes:
    if n < 0:
        raise SystemExit("negative varint")
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def vbytes(b: bytes) -> bytes:
    return varint(len(b)) + b


def zz(n: int) -> bytes:
    """zigzag varint of a signed delta"""
    return varint((n << 1) if n >= 0 else ((-n) << 1) - 1)


def num(b: bytes) -> int:
    r = 0
    for x in b:
        r = (r << 8) | x
    return r & 0xFFFFFFFF


# Operation codes replayed by src/fonts/cmap.cpp (CMapBuilder) - keep in sync.
OP_CODESPACE, OP_CIDRANGE, OP_CIDCHAR, OP_NOTDEFRANGE, OP_NOTDEFCHAR, OP_BFCHAR, OP_BFRANGE_ARRAY, \
    OP_BFRANGE, OP_USECMAP, OP_END = range(1, 11)
U_HEX, U_NAME, U_HEX2 = 0, 1, 2   # U_HEX2: 2-byte hex string as a zigzag delta of the previous one


def cmap_ops(data: bytes) -> bytes:
    """Pre-tokenizes one predefined CMap resource. The resources are well-formed Adobe CMaps; anything the
    simple reader below does not understand aborts the generation (no silent approximation)."""
    toks = list(tokens(data))
    out = bytearray()
    user: dict[str, object] = {}
    i = 0
    stack: list = []
    st = {"code": 0, "cid": 0}   # delta-coding state (codes: previous code/end; cids: next expected cid)

    def dcode(v: int) -> bytes:
        d = zz(v - st["code"])
        st["code"] = v
        return d

    def dcid(v: int, span: int) -> bytes:
        d = zz(v - st["cid"])
        st["cid"] = v + span + 1
        return d

    def ustr(t) -> bytes:
        kind, v = t
        if kind == "hex" and len(v) == 2:
            u = (v[0] << 8) | v[1]
            d = zz(u - st.get("u", 0))
            st["u"] = u
            return bytes([U_HEX2]) + d
        if kind == "hex":
            return bytes([U_HEX]) + vbytes(v)
        if kind == "name":
            return bytes([U_NAME]) + vbytes(v.encode("latin-1"))
        raise SystemExit(f"bad unicode token {t}")

    while i < len(toks):
        kind, v = toks[i]
        if kind == "int" and i + 1 < len(toks) and toks[i + 1][0] == "kw" and toks[i + 1][1].startswith("begin") \
                and toks[i + 1][1] != "begincmap":
            n, key = v, toks[i + 1][1][5:]
            i += 2
            for _ in range(n):
                if key == "codespacerange":
                    (k1, b), (k2, e) = toks[i], toks[i + 1]
                    assert k1 == k2 == "hex"
                    out += bytes([OP_CODESPACE]) + vbytes(b) + vbytes(e)
                    i += 2
                elif key in ("cidrange", "notdefrange"):
                    (k1, b), (k2, e), (k3, c) = toks[i], toks[i + 1], toks[i + 2]
                    assert k1 == k2 == "hex" and k3 == "int"
                    out += bytes([OP_CIDRANGE if key == "cidrange" else OP_NOTDEFRANGE]) + dcode(num(b)) \
                        + dcode(num(e)) + dcid(c, num(e) - num(b))
                    i += 3
                elif key in ("cidchar", "notdefchar"):
                    (k1, b), (k3, c) = toks[i], toks[i + 1]
                    assert k1 == "hex" and k3 == "int"
                    out += bytes([OP_CIDCHAR if key == "cidchar" else OP_NOTDEFCHAR]) + dcode(num(b)) + dcid(c, 0)
                    i += 2
                elif key == "bfchar":
                    (k1, b) = toks[i]
                    assert k1 == "hex"
                    out += bytes([OP_BFCHAR]) + dcode(num(b)) + ustr(toks[i + 1])
                    i += 2
                elif key == "bfrange":
                    (k1, b), (k2, e) = toks[i], toks[i + 1]
                    assert k1 == k2 == "hex"
                    if toks[i + 2][0] == "[":
                        j = i + 3
                        vals = []
                        while toks[j][0] != "]":
                            vals.append(ustr(toks[j]))
                            j += 1
                        assert len(vals) == num(e) - num(b) + 1
                        out += bytes([OP_BFRANGE_ARRAY]) + vbytes(b) + vbytes(e) + varint(len(vals)) + b"".join(vals)
                        i = j + 1
                    else:
                        k3, s = toks[i + 2]
                        assert k3 == "hex"
                        out += bytes([OP_BFRANGE]) + vbytes(b) + vbytes(e) + vbytes(s)
                        i += 3
                else:
                    raise SystemExit(f"unknown list {key}")
            # CMapParser.processList reads exactly the declared number of entries and only logs a missing
            # "end<key>": surplus entries (Adobe-Japan1-UCS2 declares 41 bfranges and lists 42) are then
            # ordinary PostScript operands and are ignored.
            if toks[i] == ("kw", "end" + key):
                i += 1
            continue
        if kind == "kw" and v == "usecmap":
            assert stack and stack[-1][0] == "name"
            out += bytes([OP_USECMAP]) + vbytes(stack[-1][1].encode("latin-1"))
            stack.pop()
            i += 1
            continue
        if kind == "kw" and v == "def":
            if len(stack) >= 2 and stack[-2][0] == "name":
                user[stack[-2][1]] = stack[-1]
            stack = stack[:-2]
            i += 1
            continue
        if kind in ("int", "real", "name", "str", "hex"):
            stack.append((kind, v))
        i += 1
    wmode = user.get("WMode", ("int", 0))
    assert wmode[0] == "int"
    name = user.get("CMapName", ("name", ""))
    out += bytes([OP_END]) + varint(wmode[1]) + vbytes(name[1].encode("latin-1"))
    return bytes(out)


# ----------------------------------------------------------------------------------------- main
def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    root = Path(argv[1])
    out_dir = Path(argv[2]) if len(argv) > 2 else HERE.parent / "src/fonts/generated"
    out_dir.mkdir(parents=True, exist_ok=True)
    parser = root / "parser"
    fontpkg = parser / "org/verapdf/pd/font"
    ttp = (fontpkg / "truetype/TrueTypePredefined.java").read_text("utf-8")
    cffp = (fontpkg / "cff/CFFPredefined.java").read_text("utf-8")

    enc = {n: java_string_array(ttp, n) for n in
           ("MAC_ROMAN_ENCODING", "WIN_ANSI_ENCODING", "STANDARD_ENCODING", "MAC_EXPERT_ENCODING")}
    for n, a in enc.items():
        assert len(a) == 256, (n, len(a))
    mac_names = java_string_array(ttp, "MAC_INDEX_TO_GLYPH_NAME")
    mac_os_roman = java_map_puts(ttp, "MAC_OS_ROMAN_ENCODING_MAP")
    # HashMap put: a later put of the same name replaces the earlier value.
    mor: dict[str, int] = {}
    for k, v in mac_os_roman:
        mor[k] = v

    cff_std_strings = java_string_array(cffp, "STANDARD_STRINGS")
    cff_std_enc = java_int_array(cffp, "STANDARD_ENCODING")
    cff_exp_enc = java_int_array(cffp, "EXPERT_ENCODING")
    cff_iso = java_string_array(cffp, "ISO_ADOBE_CHARSET")
    cff_exp = java_string_array(cffp, "EXPERT_CHARSET")
    cff_exps = java_string_array(cffp, "EXPERT_SUBSET_CHARSET")

    symbol = sorted(set(java_set_adds((fontpkg / "type1/SymbolSet.java").read_text("utf-8"))))
    zapf = sorted(set(java_set_adds((fontpkg / "type1/ZapfDingbats.java").read_text("utf-8"))))

    # Adobe Glyph List: "name hex [hex...]" lines, HashMap -> the last line of a name wins.
    agl: dict[str, list[int]] = {}
    for line in (parser / "font/AdobeGlyphList.txt").read_text("latin-1").splitlines():
        words = line.split(" ")
        if len(words) < 2:
            continue
        agl[words[0]] = [int(w, 16) for w in words[1:]]
    agl_names = sorted(agl, key=lambda s: s.encode("latin-1"))

    afm_dir = parser / "font/stdmetrics"
    metrics = {p.stem: parse_afm(p) for p in sorted(afm_dir.glob("*.afm"))}

    # ---------------------------------------------------------------------------- font_tables.cpp
    o = []
    o.append("// GENERATED by tools/gen_font_data.py from the veraPDF 1.30.2 parser sources. Do not edit.\n"
             "// SPDX-License-Identifier: MPL-2.0 AND BSD-3-Clause AND LicenseRef-Adobe-Core14-AFM\n"
             "// Data: the Adobe Glyph List (Copyright 2002-2019 Adobe, BSD-3-Clause, in veraPDF's version), the\n"
             "// metrics of the Adobe Core 14 AFM files (Copyright 1985-1997 Adobe Systems Incorporated, terms of\n"
             "// Adobe's AFM ReadMe), the simple-font encodings of ISO 32000-1 Annex D, the Macintosh standard glyph\n"
             "// names (veraPDF's version) and the CFF predefined tables of Adobe TN 5176, as tabled in veraPDF\n"
             "// 1.30.2 (Copyright 2015-2026 veraPDF Consortium; used under MPL-2.0, chosen from GPL-3.0-or-later or\n"
             "// MPL-2.0-or-later, as a precaution). See THIRD_PARTY_NOTICES.md.\n")
    o.append('#include "fonts/data.h"\n\nnamespace eqx::fontdata {\n')
    for n, a in enc.items():
        cname = {"MAC_ROMAN_ENCODING": "kMacRoman", "WIN_ANSI_ENCODING": "kWinAnsi",
                 "STANDARD_ENCODING": "kStandard", "MAC_EXPERT_ENCODING": "kMacExpert"}[n]
        o.append(string_table(cname, a))
    o.append(string_table("kMacGlyphNames", mac_names))
    o.append(f"const std::size_t kMacGlyphNamesCount = {len(mac_names)};\n")
    rows = ",\n".join(f"    {{{cstr(k)}, {mor[k]}}}" for k in sorted(mor, key=lambda s: s.encode('latin-1')))
    o.append(f"const NameCode kMacOsRoman[] = {{\n{rows}\n}};\nconst std::size_t kMacOsRomanCount = {len(mor)};\n")
    o.append(string_table("kCffStdStrings", cff_std_strings))
    o.append(f"const std::size_t kCffStdStringsCount = {len(cff_std_strings)};\n")
    for cname, arr in (("kCffStdEncoding", cff_std_enc), ("kCffExpertEncoding", cff_exp_enc)):
        o.append(f"const std::uint16_t {cname}[{len(arr)}] = {{" + ",".join(str(x) for x in arr) + "};\n")
        o.append(f"const std::size_t {cname}Count = {len(arr)};\n")
    for cname, arr in (("kCffIsoAdobeCharset", cff_iso), ("kCffExpertCharset", cff_exp),
                       ("kCffExpertSubsetCharset", cff_exps)):
        o.append(string_table(cname, arr))
        o.append(f"const std::size_t {cname}Count = {len(arr)};\n")
    o.append(string_table("kSymbolSetNames", symbol))
    o.append(f"const std::size_t kSymbolSetCount = {len(symbol)};\n")
    o.append(string_table("kZapfDingbatsNames", zapf))
    o.append(f"const std::size_t kZapfDingbatsCount = {len(zapf)};\n")
    # AGL: names sorted bytewise; code points in one pool.
    pool: list[int] = []
    arows = []
    for name in agl_names:
        cps = agl[name]
        arows.append(f"    {{{cstr(name)}, {len(pool)}, {len(cps)}}}")
        pool.extend(cps)
    o.append("const std::uint32_t kAglPool[] = {" + ",".join(str(x) for x in pool) + "};\n")
    o.append("const AglEntry kAgl[] = {\n" + ",\n".join(arows) + "\n};\n")
    o.append(f"const std::size_t kAglCount = {len(agl_names)};\n")
    # standard-14 metrics
    for stem, m in metrics.items():
        ident = "kAfm_" + re.sub(r"[^A-Za-z0-9]", "_", stem)
        names = sorted(m["widths"], key=lambda s: s.encode("latin-1"))
        o.append(f"const NameWidth {ident}W[] = {{\n" + ",\n".join(
            f"    {{{cstr(n)}, {m['widths'][n]}}}" for n in names) + "\n};\n")
        o.append(f"const CodeName {ident}C[] = {{\n" + ",\n".join(
            f"    {{{c}, {cstr(n)}}}" for c, n, _ in m["codes"] if c >= 0) + "\n};\n")
    o.append("const StdMetrics kStdMetrics[] = {\n" + ",\n".join(
        "    {%s, %s, %s, %sW, %d, %sC, %d}" % (
            cstr(stem), cstr(m["encoding_scheme"]), cstr(m["charset"]),
            "kAfm_" + re.sub(r"[^A-Za-z0-9]", "_", stem), len(m["widths"]),
            "kAfm_" + re.sub(r"[^A-Za-z0-9]", "_", stem), sum(1 for c, _, _ in m["codes"] if c >= 0))
        for stem, m in sorted(metrics.items())) + "\n};\n")
    o.append(f"const std::size_t kStdMetricsCount = {len(metrics)};\n")
    o.append("\n}  // namespace eqx::fontdata\n")
    (out_dir / "font_tables.cpp").write_text("".join(o), "utf-8")

    # ---------------------------------------------------------------------------- font_cmaps.cpp
    cmap_dir = parser / "font/cmap"
    blob = bytearray()
    entries = []
    for p in sorted(cmap_dir.iterdir(), key=lambda q: q.name.encode()):
        if not p.is_file():
            continue
        try:
            ops = cmap_ops(p.read_bytes())
        except AssertionError as e:
            raise SystemExit(f"{p.name}: {e}")
        comp = zlib.compress(ops, 9)
        entries.append((p.name, len(blob), len(comp), len(ops)))
        blob += comp
    c = ["// GENERATED by tools/gen_font_data.py from the predefined CMap resources of veraPDF 1.30.2\n"
         "// (parser/font/cmap/*). Do not edit.\n"
         "// SPDX-License-Identifier: BSD-3-Clause\n"
         "// Data: Adobe CMap resources, Copyright 1990-2023 Adobe (Systems Incorporated), BSD-3-Clause; the notice\n"
         "// and license text are in THIRD_PARTY_NOTICES.md. Each record is a zlib-compressed operation stream\n"
         "// replayed by src/fonts/cmap.cpp.\n",
         '#include "fonts/data.h"\n\nnamespace eqx::fontdata {\n\n',
         "static const char kCMapBlob[] =\n", blob_literal(bytes(blob)), ";\n\n",
         "const PredefinedCMap kPredefinedCMaps[] = {\n",
         ",\n".join(f"    {{{cstr(n)}, kCMapBlob + {off}, {ln}, {raw}}}" for n, off, ln, raw in entries),
         "\n};\n", f"const std::size_t kPredefinedCMapsCount = {len(entries)};\n",
         "\n}  // namespace eqx::fontdata\n"]
    (out_dir / "font_cmaps.cpp").write_text("".join(c), "utf-8")
    print(f"font_tables.cpp: agl {len(agl_names)}, afm {len(metrics)}; font_cmaps.cpp: {len(entries)} CMaps, "
          f"{len(blob)} compressed bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
