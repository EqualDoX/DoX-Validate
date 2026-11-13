"""Minimal deterministic PDF writer and tagged-document builder for the test corpus (standard library only).

Every byte of the output follows from the arguments: no timestamps, no random identifiers, no compression (zlib output may
differ between zlib builds). Objects are serialised in the order they were added; the trailer /ID is derived from a caller
supplied seed. Optional RC4 128-bit encryption (standard security handler, V 2, R 3; ISO 32000-1 7.6.3) is implemented here
so that encrypted fixtures are reproducible too.
"""
from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

FONT_DIR = Path(__file__).resolve().parent / "fonts"


# --------------------------------------------------------------------------------------------------------------------------
# COS objects
# --------------------------------------------------------------------------------------------------------------------------
class Name(str):
    """A PDF name (written as /Name)."""


@dataclass(frozen=True)
class Ref:
    num: int


@dataclass
class Str:
    """A PDF string with explicit bytes (literal or hexadecimal form)."""
    data: bytes
    hex: bool = False


@dataclass
class Stream:
    dict: dict
    data: bytes


@dataclass(frozen=True)
class Raw:
    """Verbatim token (used for deliberately malformed values)."""
    text: str


def text_string(s: str) -> Str:
    """A PDF text string: PDFDocEncoding for printable ASCII, else UTF-16BE with BOM (ISO 32000-1 7.9.2.2)."""
    if all(0x20 <= ord(c) < 0x7F for c in s):
        return Str(s.encode("ascii"))
    return Str(b"\xfe\xff" + s.encode("utf-16-be"), hex=True)


def fmt_num(v: float) -> str:
    if isinstance(v, bool):
        raise TypeError("bool is not a number")
    if isinstance(v, int):
        return str(v)
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def _lit(data: bytes) -> bytes:
    out = bytearray(b"(")
    for b in data:
        if b in (0x28, 0x29, 0x5C):
            out += b"\\" + bytes([b])
        elif b == 0x0D:
            out += b"\\r"
        elif b == 0x0A:
            out += b"\\n"
        else:
            out.append(b)
    return bytes(out + b")")


def _name(n: str) -> bytes:
    out = bytearray(b"/")
    for b in n.encode("utf-8"):
        if b < 0x21 or b > 0x7E or b in b"#()<>[]{}/%":
            out += b"#%02X" % b
        else:
            out.append(b)
    return bytes(out)


class Writer:
    """Collects indirect objects and serialises a classic (xref table) PDF file."""

    def __init__(self, version: str = "1.7", seed: str = "corpus"):
        self.version = version
        self.objects: dict[int, Any] = {}
        self.order: list[int] = []
        self.id0 = hashlib.md5(seed.encode()).digest()
        self.encrypt: _Rc4 | None = None
        self.trailer_extra: dict = {}

    def reserve(self) -> Ref:
        n = len(self.order) + 1
        self.order.append(n)
        self.objects[n] = None
        return Ref(n)

    def add(self, obj: Any) -> Ref:
        r = self.reserve()
        self.objects[r.num] = obj
        return r

    def set(self, ref: Ref, obj: Any) -> None:
        self.objects[ref.num] = obj

    def get(self, ref: Ref) -> Any:
        return self.objects[ref.num]

    # serialisation ------------------------------------------------------------------------------------------------------
    def ser(self, v: Any, num: int = 0) -> bytes:
        if v is None:
            return b"null"
        if v is True:
            return b"true"
        if v is False:
            return b"false"
        if isinstance(v, Raw):
            return v.text.encode("latin-1")
        if isinstance(v, Name):
            return _name(v)
        if isinstance(v, Ref):
            return b"%d 0 R" % v.num
        if isinstance(v, (int, float)):
            return fmt_num(v).encode()
        if isinstance(v, str):
            v = text_string(v)
        if isinstance(v, bytes):
            v = Str(v)
        if isinstance(v, Str):
            data = self.encrypt.crypt(num, v.data) if (self.encrypt and num) else v.data
            return b"<" + data.hex().upper().encode() + b">" if (v.hex or self.encrypt and num) else _lit(data)
        if isinstance(v, (list, tuple)):
            return b"[" + b" ".join(self.ser(x, num) for x in v) + b"]"
        if isinstance(v, dict):
            parts = [b"<<"]
            for k, x in v.items():
                parts.append(_name(k) + b" " + self.ser(x, num))
            parts.append(b">>")
            return b" ".join(parts)
        if isinstance(v, Stream):
            data = self.encrypt.crypt(num, v.data) if (self.encrypt and num) else v.data
            d = dict(v.dict)
            d["Length"] = len(data)
            return self.ser(d, num) + b"\nstream\n" + data + b"\nendstream"
        raise TypeError(f"cannot serialise {type(v)}")

    def write(self, root: Ref, info: Ref | None = None, header: bytes | None = None) -> bytes:
        out = bytearray(header if header is not None else b"%PDF-" + self.version.encode() + b"\n%\xe2\xe3\xcf\xd3\n")
        offsets = {}
        encrypt_ref = None
        if self.encrypt:
            encrypt_ref = self.add(self.encrypt.dictionary())
        for n in self.order:
            offsets[n] = len(out)
            num = 0 if (encrypt_ref and n == encrypt_ref.num) else n
            out += b"%d 0 obj\n" % n + self.ser(self.objects[n], num) + b"\nendobj\n"
        xref = len(out)
        size = len(self.order) + 1
        out += b"xref\n0 %d\n0000000000 65535 f \n" % size
        for n in range(1, size):
            out += b"%010d 00000 n \n" % offsets[n]
        trailer = {"Size": size, "Root": root}
        if info:
            trailer["Info"] = info
        if encrypt_ref:
            trailer["Encrypt"] = encrypt_ref
        trailer["ID"] = [Str(self.id0, hex=True), Str(self.id0, hex=True)]
        trailer.update(self.trailer_extra)
        out += b"trailer\n" + self.ser(trailer) + b"\nstartxref\n%d\n%%%%EOF\n" % xref
        return bytes(out)

    def enable_rc4(self, user_pw: bytes, owner_pw: bytes, permissions: int = -3904) -> None:
        self.encrypt = _Rc4(user_pw, owner_pw, permissions, self.id0)


# --------------------------------------------------------------------------------------------------------------------------
# RC4 128-bit standard security handler (ISO 32000-1 7.6.3.3, algorithms 2, 3, 5)
# --------------------------------------------------------------------------------------------------------------------------
_PAD = bytes.fromhex("28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


class _Rc4:
    def __init__(self, user_pw: bytes, owner_pw: bytes, p: int, id0: bytes):
        pad = lambda pw: (pw + _PAD)[:32]  # noqa: E731
        h = hashlib.md5(pad(owner_pw or user_pw)).digest()
        for _ in range(50):
            h = hashlib.md5(h).digest()
        o = rc4(h, pad(user_pw))
        for i in range(1, 20):
            o = rc4(bytes(b ^ i for b in h), o)
        self.o, self.p = o, p
        k = hashlib.md5(pad(user_pw) + o + struct.pack("<i", p) + id0).digest()
        for _ in range(50):
            k = hashlib.md5(k[:16]).digest()
        self.key = k[:16]
        u = rc4(self.key, hashlib.md5(_PAD + id0).digest())
        for i in range(1, 20):
            u = rc4(bytes(b ^ i for b in self.key), u)
        self.u = u + bytes(16)

    def dictionary(self) -> dict:
        return {"Filter": Name("Standard"), "V": 2, "R": 3, "Length": 128, "P": self.p,
                "O": Str(self.o, hex=True), "U": Str(self.u, hex=True)}

    def crypt(self, num: int, data: bytes) -> bytes:
        k = hashlib.md5(self.key + struct.pack("<I", num)[:3] + b"\x00\x00").digest()
        return rc4(k[:16], data)


# --------------------------------------------------------------------------------------------------------------------------
# Fonts
# --------------------------------------------------------------------------------------------------------------------------
_FONT_META = json.loads((FONT_DIR / "CorpusSans.json").read_text())


def font_meta(style: str = "Regular") -> dict:
    return _FONT_META[style]


def text_width(text: str, size: float, style: str = "Regular") -> float:
    m = font_meta(style)
    return sum(m["widths"][ord(c) - 32] if 32 <= ord(c) < 127 else m["widths"][0] for c in text) * size / 1000.0


TOUNICODE_SIMPLE = b"""/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /Adobe-Identity-UCS def
/CMapType 2 def
1 begincodespacerange
<00> <FF>
endcodespacerange
1 beginbfrange
<20> <7E> <0020>
endbfrange
endcmap
CMapName currentdict /CMap defineresource pop
end
end
"""


def tounicode_cid(meta: dict) -> bytes:
    lines = [f"<{g:04X}> <{c:04X}>" for c, g in zip(range(meta["first_char"], 127), meta["gids"])]
    body = []
    for i in range(0, len(lines), 100):
        chunk = lines[i:i + 100]
        body.append(f"{len(chunk)} beginbfchar\n" + "\n".join(chunk) + "\nendbfchar")
    return ("/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n" + "\n".join(body) +
            "\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n").encode()


def font_descriptor(w: Writer, style: str, embed: bool = True, flags: int = 32) -> Ref:
    m = font_meta(style)
    fd = {"Type": Name("FontDescriptor"), "FontName": Name(m["postscript_name"]), "Flags": flags,
          "FontBBox": m["bbox"], "ItalicAngle": m["italic_angle"], "Ascent": m["ascent"], "Descent": m["descent"],
          "CapHeight": m["cap_height"], "StemV": 80 if style == "Regular" else 140, "FontWeight": m["weight"]}
    if embed:
        data = (FONT_DIR / m["file"]).read_bytes()
        fd["FontFile2"] = w.add(Stream({"Length1": len(data)}, data))
    return w.add(fd)


def simple_truetype(w: Writer, style: str = "Regular", *, embed: bool = True, tounicode: bool = True,
                    widths_delta: int = 0, encoding: Any = "WinAnsiEncoding", flags: int = 32) -> Ref:
    m = font_meta(style)
    f = {"Type": Name("Font"), "Subtype": Name("TrueType"), "BaseFont": Name(m["postscript_name"]),
         "FirstChar": 32, "LastChar": 126, "Widths": [x + widths_delta for x in m["widths"]],
         "FontDescriptor": font_descriptor(w, style, embed, flags)}
    if encoding is not None:
        f["Encoding"] = Name(encoding) if isinstance(encoding, str) else encoding
    if tounicode:
        f["ToUnicode"] = w.add(Stream({}, TOUNICODE_SIMPLE))
    return w.add(f)


def type0_identity(w: Writer, style: str = "Regular", *, tounicode: bool = True) -> Ref:
    """Composite font, Identity-H, CIDFontType2 with CIDToGIDMap /Identity: codes are glyph ids (2 bytes)."""
    m = font_meta(style)
    cid = {"Type": Name("Font"), "Subtype": Name("CIDFontType2"), "BaseFont": Name(m["postscript_name"]),
           "CIDSystemInfo": {"Registry": "Adobe", "Ordering": "Identity", "Supplement": 0},
           "FontDescriptor": font_descriptor(w, style, True, 32), "DW": 1000,
           "W": [0, m["gid_widths"]], "CIDToGIDMap": Name("Identity")}
    f = {"Type": Name("Font"), "Subtype": Name("Type0"), "BaseFont": Name(m["postscript_name"]),
         "Encoding": Name("Identity-H"), "DescendantFonts": [w.add(cid)]}
    if tounicode:
        f["ToUnicode"] = w.add(Stream({}, tounicode_cid(m)))
    return w.add(f)


def standard14(w: Writer, base: str = "Helvetica") -> Ref:
    return w.add({"Type": Name("Font"), "Subtype": Name("Type1"), "BaseFont": Name(base),
                  "Encoding": Name("WinAnsiEncoding")})


def pdf_text(s: str) -> bytes:
    return _lit(s.encode("latin-1"))


def cid_text(s: str, style: str = "Regular") -> bytes:
    m = font_meta(style)
    return b"<" + b"".join(b"%04X" % m["gids"][ord(c) - 32] for c in s) + b">"


# --------------------------------------------------------------------------------------------------------------------------
# Tagged document builder
# --------------------------------------------------------------------------------------------------------------------------
@dataclass
class MCR:
    page: int
    mcid: int


@dataclass
class OBJR:
    page: int
    annot: Ref


@dataclass
class SE:
    """A structure element; kids are SE, MCR or OBJR. `extra` entries are copied into the dictionary verbatim."""
    role: str
    kids: list = field(default_factory=list)
    extra: dict = field(default_factory=dict)
    parent: "SE | None" = None
    ref: Ref | None = None
    page: int | None = None
    omit_parent: bool = False

    def add(self, kid: "SE") -> "SE":
        kid.parent = self
        self.kids.append(kid)
        return kid


@dataclass
class Annot:
    page: int
    dict: dict
    se: SE | None
    ref: Ref | None = None
    struct_parent: bool = True


def xmp(title: str | None, ua_part: int | None, *, lang_alt: str = "x-default", ua_rev: int | None = None,
        extra_desc: str = "", wtpdf: str | None = None) -> bytes:
    desc = ['<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/"'
            ' xmlns:pdfuaid="http://www.aiim.org/pdfua/ns/id/"'
            ' xmlns:pdf="http://ns.adobe.com/pdf/1.3/"'
            ' xmlns:xmp="http://ns.adobe.com/xap/1.0/">']
    if title is not None:
        desc.append(f'<dc:title><rdf:Alt><rdf:li xml:lang="{lang_alt}">{title}</rdf:li></rdf:Alt></dc:title>')
    if ua_part is not None:
        desc.append(f"<pdfuaid:part>{ua_part}</pdfuaid:part>")
    if ua_rev is not None:
        desc.append(f"<pdfuaid:rev>{ua_rev}</pdfuaid:rev>")
    desc.append("<pdf:Producer>dox-validate test corpus generator</pdf:Producer>")
    desc.append(extra_desc)
    desc.append("</rdf:Description>")
    if wtpdf:
        items = "".join(f'<rdf:li rdf:parseType="Resource"><pdfd:conformsTo>{u}</pdfd:conformsTo></rdf:li>'
                        for u in wtpdf.split())
        desc.append('<rdf:Description rdf:about="" xmlns:pdfd="http://pdfa.org/declarations/">'
                    f"<pdfd:declarations><rdf:Bag>{items}</rdf:Bag></pdfd:declarations></rdf:Description>")
    body = ('<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>\n'
            '<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
            + "".join(desc) + "</rdf:RDF></x:xmpmeta>\n" + " " * 64 + '\n<?xpacket end="w"?>')
    return body.encode("utf-8")


PAGE_W, PAGE_H, MARGIN = 612, 792, 72


class Page:
    def __init__(self, doc: "TaggedDoc", index: int):
        self.doc, self.index = doc, index
        self.ops: list[bytes] = []
        self.mcid = 0
        self.y = PAGE_H - MARGIN
        self.annots: list[Annot] = []
        self.xobjects: dict[str, Ref] = {}
        self.extra: dict = {}
        self.mc_owners: list[SE | None] = []   # per MCID: the element whose K holds it (None: absent from ParentTree)

    # marked content --------------------------------------------------------------------------------------------------
    def _mc(self, se: SE | None, body: bytes, tag: str | None = None, props: str = "") -> MCR | None:
        if not self.doc.tagged or se is None:
            self.ops.append(body)
            return None
        m = MCR(self.index, self.mcid)
        self.mcid += 1
        self.mc_owners.append(se)
        t = tag or se.role
        self.ops.append(b"/%s <</MCID %d%s>> BDC\n" % (t.encode(), m.mcid, props.encode()) + body + b"EMC\n")
        se.kids.append(m)
        if se.page is None:
            se.page = self.index
        return m

    def artifact(self, body: bytes, kind: str = "Pagination") -> None:
        self.ops.append(b"/Artifact <</Type /%s>> BDC\n" % kind.encode() + body + b"EMC\n")

    def text_ops(self, text: str, x: float, y: float, size: float, font: str = "F1", color: float | tuple = 0.0,
                 tr: int = 0) -> bytes:
        col = (b"%s g " % fmt_num(color).encode()) if not isinstance(color, tuple) else \
            (b"%s %s %s rg " % tuple(fmt_num(c).encode() for c in color))
        enc = cid_text(text) if self.doc.fonts.get(font, (None, ""))[1] == "cid" else pdf_text(text)
        trs = b"%d Tr " % tr if tr else b""
        return (b"BT " + col + trs + b"/%s %s Tf %s %s Td " % (font.encode(), fmt_num(size).encode(),
                fmt_num(x).encode(), fmt_num(y).encode()) + enc + b" Tj ET\n")

    def lines(self, text: str, size: float, style: str = "Regular") -> list[str]:
        words, out, cur = text.split(), [], ""
        for wd in words:
            cand = (cur + " " + wd).strip()
            if cur and text_width(cand, size, style) > PAGE_W - 2 * MARGIN:
                out.append(cur)
                cur = wd
            else:
                cur = cand
        if cur:
            out.append(cur)
        return out

    # blocks -----------------------------------------------------------------------------------------------------------
    def text_block(self, se: SE | None, text: str, size: float = 11, font: str = "F1", color: float | tuple = 0.0,
                   tag: str | None = None, x: float = MARGIN, gap: float = 6) -> list[MCR]:
        style = "Bold" if font == "F2" else "Regular"
        mcrs = []
        body = b""
        for ln in self.lines(text, size, style):
            self.y -= size * 1.25
            body += self.text_ops(ln, x, self.y, size, font, color)
        m = self._mc(se, body, tag)
        if m:
            mcrs.append(m)
        self.y -= gap
        return mcrs

    def heading(self, level: int | None, text: str, parent: SE | None = None, role: str | None = None) -> SE:
        se = self.doc.elem(role or (f"H{level}" if level else "H"), parent)
        self.text_block(se, text, size=18 - 2 * min(level or 1, 4), font="F2", gap=8)
        self.doc.headings.append((se, text, self.index, self.y))
        return se

    def para(self, text: str, parent: SE | None = None, role: str = "P", **kw) -> SE:
        se = self.doc.elem(role, parent)
        self.text_block(se, text, **kw)
        return se

    def untagged_text(self, text: str, size: float = 11) -> None:
        self.y -= size * 1.25
        self.ops.append(self.text_ops(text, MARGIN, self.y, size))
        self.y -= 6

    def artifact_text(self, text: str, y: float = 40) -> None:
        self.artifact(self.text_ops(text, MARGIN, y, 9))

    def figure(self, alt: str | None, parent: SE | None = None, *, bbox: bool = True, actual_text: str | None = None,
               w: float = 200, h: float = 100, fill: float = 0.2, role: str = "Figure") -> SE:
        se = self.doc.elem(role, parent)
        x0, y0 = MARGIN, self.y - h
        if alt is not None:
            se.extra["Alt"] = alt
        if actual_text is not None:
            se.extra["ActualText"] = actual_text
        if bbox:
            se.extra["A"] = {"O": Name("Layout"), "Placement": Name("Block"), "BBox": [x0, y0, x0 + w, y0 + h]}
        body = (b"q %s g %s %s %s %s re f 1 g %s %s %s %s re f Q\n" % (
            fmt_num(fill).encode(), fmt_num(x0).encode(), fmt_num(y0).encode(), fmt_num(w).encode(), fmt_num(h).encode(),
            fmt_num(x0 + 20).encode(), fmt_num(y0 + 20).encode(), fmt_num(w / 2).encode(), fmt_num(h / 3).encode()))
        self._mc(se, body)
        self.y = y0 - 12
        return se

    def image(self, name: str, ref: Ref, x: float, y: float, w: float, h: float, se: SE | None = None) -> None:
        self.xobjects[name] = ref
        body = b"q %s 0 0 %s %s %s cm /%s Do Q\n" % (fmt_num(w).encode(), fmt_num(h).encode(), fmt_num(x).encode(),
                                                    fmt_num(y).encode(), name.encode())
        if se is None and self.doc.tagged:
            self.artifact(body, "Layout")
        else:
            self._mc(se, body)

    def table(self, rows: list[list[str]], parent: SE | None = None, *, header_rows: int = 1, scope: bool = True,
              row_parent_role: str | None = "TR", header_role: str = "TH", tbody: bool = False) -> SE:
        doc = self.doc
        tse = doc.elem("Table", parent)
        holder = doc.elem("TBody", tse) if tbody else tse
        ncols = max(len(r) for r in rows)
        colw = (PAGE_W - 2 * MARGIN) / ncols
        for ri, row in enumerate(rows):
            self.y -= 16
            tr = doc.elem(row_parent_role, holder) if row_parent_role else holder
            for ci, cell in enumerate(row):
                is_head = ri < header_rows
                cse = doc.elem(header_role if is_head else "TD", tr)
                if is_head and scope:
                    cse.extra["A"] = {"O": Name("Table"), "Scope": Name("Column")}
                font = "F2" if is_head else "F1"
                self._mc(cse, self.text_ops(cell, MARGIN + ci * colw + 2, self.y, 10, font))
        self.y -= 12
        return tse

    def list(self, items: list[str], parent: SE | None = None) -> SE:
        lse = self.doc.elem("L", parent)
        lse.extra["A"] = {"O": Name("List"), "ListNumbering": Name("Disc")}
        for it in items:
            li = self.doc.elem("LI", lse)
            lbl = self.doc.elem("Lbl", li)
            body = self.doc.elem("LBody", li)
            self.y -= 14
            self._mc(lbl, self.text_ops("-", MARGIN + 6, self.y, 11))
            self._mc(body, self.text_ops(it, MARGIN + 20, self.y, 11))
        self.y -= 8
        return lse

    def link(self, text: str, uri: str, parent: SE | None = None, *, contents: str | None = "", tagged: bool = True,
             color: tuple | float = (0, 0, 0.6), underline: bool = True, prefix: str = "", border: list | None = None) -> SE | None:
        doc = self.doc
        p = doc.elem("P", parent)
        self.y -= 14
        x = MARGIN
        if prefix:
            self._mc(p, self.text_ops(prefix, x, self.y, 11))
            x += text_width(prefix + " ", 11)
        lse = doc.elem("Link", p) if tagged else None
        w = text_width(text, 11)
        body = self.text_ops(text, x, self.y, 11, color=color)
        if underline:
            body += b"q 0 0 0.6 RG 0.75 w %s %s m %s %s l S Q\n" % (fmt_num(x).encode(), fmt_num(self.y - 2).encode(),
                                                                       fmt_num(x + w).encode(), fmt_num(self.y - 2).encode())
        self._mc(lse if tagged else p, body)
        a = {"Type": Name("Annot"), "Subtype": Name("Link"), "Rect": [x, self.y - 3, x + w, self.y + 11],
             "Border": border if border is not None else [0, 0, 0], "A": {"S": Name("URI"), "URI": Str(uri.encode())}}
        if contents is not None:
            a["Contents"] = contents if contents else f"Link to {uri}"
        an = Annot(self.index, a, lse)
        self.annots.append(an)
        if lse is not None:
            lse.kids.append(an)
        self.y -= 8
        return lse

    def text_field(self, name: str, label: str, parent: SE | None = None, *, tu: str | None = "", in_form: bool = True,
                   alt_on_form: str | None = None, mk: dict | None = None, contents: str | None = "") -> SE | None:
        doc = self.doc
        self.para(label, parent)
        rect = [MARGIN, self.y - 22, MARGIN + 200, self.y]
        ap = doc.w.add(Stream({"Type": Name("XObject"), "Subtype": Name("Form"), "BBox": [0, 0, 200, 22],
                               "Resources": {}}, b"/Tx BMC q 0 G 1 w 0.5 0.5 199 21 re S Q EMC\n"))
        a = {"Type": Name("Annot"), "Subtype": Name("Widget"), "FT": Name("Tx"), "T": name, "Rect": rect, "F": 4,
             "DA": Str(b"/F1 11 Tf 0 g"), "AP": {"N": ap}}
        if tu is not None:
            a["TU"] = tu if tu else label
        if contents is not None:
            a["Contents"] = contents if contents else label
        if mk is not None:
            a["MK"] = mk
        fse = doc.elem("Form", doc.elem("P", parent)) if in_form else None
        if fse is not None and alt_on_form:
            fse.extra["Alt"] = alt_on_form
        an = Annot(self.index, a, fse)
        self.annots.append(an)
        if fse is not None:
            fse.kids.append(an)
        doc.fields.append(an)
        self.y -= 30
        return fse


class TaggedDoc:
    """A one-column document of headings, paragraphs, figures, tables, lists, links and form fields.

    The defaults produce what PDF/UA-1 requires of the parts that a machine can check (ISO 14289-1 clauses 6 and 7):
    a structure tree whose leaves own every piece of real content (MCIDs) and a ParentTree that maps back, artifacts for
    page furniture, /Lang, embedded fonts with ToUnicode, XMP with dc:title and pdfuaid:part, DisplayDocTitle, MarkInfo,
    /Tabs /S on pages with annotations, Alt on figures, TU on fields and Contents on links. Arguments switch parts off.
    """

    def __init__(self, seed: str, *, version: str = "1.7", lang: str | None = "en-US", title: str | None = "Corpus test document",
                 tagged: bool = True, ua_part: int | None = 1, ua_rev: int | None = None, metadata: bool = True,
                 xmp_title: bool = True, display_doc_title: bool | None = True, mark_info: dict | None = None,
                 outline: bool = True, info_title: bool = True, tabs: str | None = "S", root_role: str | None = "Document",
                 fonts: str = "embedded", wtpdf: str | None = None, pdf2_namespace: bool = False):
        self.w = Writer(version, seed)
        self.version = version
        self.tagged = tagged
        self.lang, self.title, self.ua_part, self.ua_rev = lang, title, ua_part, ua_rev
        self.metadata, self.xmp_title, self.display_doc_title = metadata, xmp_title, display_doc_title
        self.mark_info = {"Marked": True} if mark_info is None else mark_info
        self.outline, self.info_title, self.tabs = outline, info_title, tabs
        self.wtpdf = wtpdf
        self.pdf2_namespace = pdf2_namespace
        self.pages: list[Page] = []
        self.headings: list = []
        self.fields: list[Annot] = []
        self.role_map: dict = {}
        self.catalog_extra: dict = {}
        self.struct_root_extra: dict = {}
        self.parent_tree = True
        self.drop_parent_tree_pages: set[int] = set()
        self.drop_struct_parents: set[int] = set()
        self.elements: list[SE] = []
        self.root = SE("StructTreeRoot")
        self.top = SE(root_role) if root_role else self.root
        if root_role:
            self.root.add(self.top)
        self.fonts: dict[str, tuple[Ref, str]] = {}
        if fonts == "embedded":
            self.fonts["F1"] = (simple_truetype(self.w, "Regular"), "simple")
            self.fonts["F2"] = (simple_truetype(self.w, "Bold"), "simple")
        elif fonts == "standard14":
            self.fonts["F1"] = (standard14(self.w, "Helvetica"), "simple")
            self.fonts["F2"] = (standard14(self.w, "Helvetica-Bold"), "simple")
        elif fonts == "type0-no-tounicode":
            self.fonts["F1"] = (type0_identity(self.w, "Regular", tounicode=False), "cid")
            self.fonts["F2"] = (simple_truetype(self.w, "Bold"), "simple")
        elif fonts == "type0":
            self.fonts["F1"] = (type0_identity(self.w, "Regular"), "cid")
            self.fonts["F2"] = (simple_truetype(self.w, "Bold"), "simple")
        else:
            raise ValueError(fonts)
        self.hooks: list[Callable[["TaggedDoc", dict], None]] = []

    def elem(self, role: str, parent: SE | None = None) -> SE:
        se = SE(role)
        (parent or self.top).add(se)
        self.elements.append(se)
        return se

    def page(self, *, furniture: bool = True) -> Page:
        p = Page(self, len(self.pages))
        self.pages.append(p)
        if furniture:
            p.artifact_text(f"Page {p.index + 1}")
        return p

    # assembly ---------------------------------------------------------------------------------------------------------
    def build(self) -> bytes:
        w = self.w
        pages_ref = w.reserve()
        page_refs = [w.reserve() for _ in self.pages]
        st_root_ref = w.reserve() if self.tagged else None
        # structure element refs
        ns_ref = None
        if self.tagged:
            self._assign_refs(self.root, st_root_ref)
            if self.pdf2_namespace:
                ns_ref = w.add({"Type": Name("Namespace"), "NS": Str(b"http://iso.org/pdf2/ssn")})
        # annotations
        sp_next = len(self.pages)
        parent_tree_nums: list = []
        for p in self.pages:
            for an in p.annots:
                an.dict["P"] = page_refs[p.index]
                if self.tagged and an.struct_parent and an.se is not None:
                    an.dict["StructParent"] = sp_next
                    parent_tree_nums.append((sp_next, an.se.ref))
                    sp_next += 1
                an.ref = w.add(an.dict)
        # pages
        font_res = {k: v[0] for k, v in self.fonts.items()}
        for p in self.pages:
            content = w.add(Stream({}, b"".join(p.ops)))
            res = {"Font": font_res}
            if p.xobjects:
                res["XObject"] = dict(p.xobjects)
            pd = {"Type": Name("Page"), "Parent": pages_ref, "MediaBox": [0, 0, PAGE_W, PAGE_H], "Resources": res,
                  "Contents": content}
            if self.tagged and p.index not in self.drop_struct_parents:
                pd["StructParents"] = p.index
            if p.annots:
                pd["Annots"] = [an.ref for an in p.annots]
                if self.tabs:
                    pd["Tabs"] = Name(self.tabs)
            pd.update(p.extra)
            w.set(page_refs[p.index], pd)
        w.set(pages_ref, {"Type": Name("Pages"), "Kids": page_refs, "Count": len(page_refs)})
        catalog: dict = {"Type": Name("Catalog"), "Pages": pages_ref}
        if self.tagged:
            nums = []
            for p in self.pages:
                if p.index in self.drop_parent_tree_pages:
                    continue
                nums += [p.index, [se.ref if se is not None else None for se in p.mc_owners]]
            for k, ref in parent_tree_nums:
                nums += [k, ref]
            st = {"Type": Name("StructTreeRoot"), "K": self._kids(self.root, page_refs)}
            if self.parent_tree:
                st["ParentTree"] = w.add({"Nums": nums})
                st["ParentTreeNextKey"] = sp_next
            if ns_ref:
                st["Namespaces"] = [ns_ref]
            if self.role_map:
                st["RoleMap"] = {k: Name(v) for k, v in self.role_map.items()}
            st.update(self.struct_root_extra)
            w.set(st_root_ref, st)
            for se in self._walk(self.root):
                if se is self.root:
                    continue
                d = {"Type": Name("StructElem"), "S": Name(se.role)}
                if ns_ref:
                    d["NS"] = ns_ref
                if not se.omit_parent:
                    d["P"] = se.parent.ref if se.parent is not None else st_root_ref
                if se.page is not None:
                    d["Pg"] = page_refs[se.page]
                d["K"] = self._kids(se, page_refs)
                d.update(se.extra)
                w.set(se.ref, d)
            catalog["StructTreeRoot"] = st_root_ref
        if self.mark_info is not False and (self.tagged or self.mark_info):
            if self.tagged or "Marked" in self.mark_info:
                catalog["MarkInfo"] = dict(self.mark_info)
        if self.lang is not None:
            catalog["Lang"] = self.lang
        if self.display_doc_title is not None:
            catalog["ViewerPreferences"] = {"DisplayDocTitle": self.display_doc_title}
        if self.metadata:
            catalog["Metadata"] = w.add(Stream({"Type": Name("Metadata"), "Subtype": Name("XML")},
                                               xmp(self.title if self.xmp_title else None, self.ua_part,
                                                   ua_rev=self.ua_rev, wtpdf=self.wtpdf)))
        if self.outline and self.headings:
            catalog["Outlines"] = self._outline(page_refs)
            catalog["PageMode"] = Name("UseOutlines")
        if self.fields:
            catalog["AcroForm"] = {"Fields": [an.ref for an in self.fields],
                                   "DA": Str(b"/F1 11 Tf 0 g"), "DR": {"Font": {"F1": self.fonts["F1"][0]}}}
        catalog.update(self.catalog_extra)
        info = None
        if self.info_title and self.title:
            info = w.add({"Title": self.title, "Producer": "dox-validate test corpus generator"})
        root = w.add(catalog)
        ctx = {"catalog": catalog, "page_refs": page_refs, "root": root}
        for h in self.hooks:
            h(self, ctx)
        return w.write(root, info)

    def _assign_refs(self, se: SE, st_root_ref: Ref) -> None:
        se.ref = st_root_ref if se is self.root else self.w.reserve()
        for k in se.kids:
            if isinstance(k, SE):
                self._assign_refs(k, st_root_ref)

    def _walk(self, se: SE):
        yield se
        for k in se.kids:
            if isinstance(k, SE):
                yield from self._walk(k)

    def _kids(self, se: SE, page_refs: list[Ref]):
        out = []
        for k in se.kids:
            if isinstance(k, SE):
                out.append(k.ref)
            elif isinstance(k, MCR):
                out.append(k.mcid if k.page == se.page else {"Type": Name("MCR"), "Pg": page_refs[k.page], "MCID": k.mcid})
            elif isinstance(k, Annot):
                out.append({"Type": Name("OBJR"), "Obj": k.ref, "Pg": page_refs[k.page]})
        if se is self.root:
            return out[0] if len(out) == 1 else out
        return out

    def _outline(self, page_refs: list[Ref]) -> Ref:
        w = self.w
        root = w.reserve()
        items = [w.reserve() for _ in self.headings]
        for i, (se, text, pg, y) in enumerate(self.headings):
            d = {"Title": text, "Parent": root, "Dest": [page_refs[pg], Name("XYZ"), MARGIN, round(y + 30), 0]}
            if i > 0:
                d["Prev"] = items[i - 1]
            if i + 1 < len(items):
                d["Next"] = items[i + 1]
            w.set(items[i], d)
        w.set(root, {"Type": Name("Outlines"), "First": items[0], "Last": items[-1], "Count": len(items)})
        return root


def scanned_image(w: Writer, width: int = 240, height: int = 320, seed: int = 7) -> Ref:
    """A grey 8-bit raster that looks like a scanned text page: dark horizontal strokes in 'lines' (no text objects)."""
    rows = []
    x = seed
    for y in range(height):
        row = bytearray(b"\xf0" * width)
        if 20 <= y < height - 20 and (y // 6) % 2 == 0 and (y % 6) in (1, 2, 3):
            for c in range(20, width - 20):
                x = (x * 1103515245 + 12345) & 0x7FFFFFFF
                if (x >> 16) % 7 != 0:
                    row[c] = 0x30
        rows.append(bytes(row))
    data = b"".join(rows)
    return w.add(Stream({"Type": Name("XObject"), "Subtype": Name("Image"), "Width": width, "Height": height,
                         "ColorSpace": Name("DeviceGray"), "BitsPerComponent": 8}, data))
