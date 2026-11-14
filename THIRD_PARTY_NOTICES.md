# Third-party notices

This file lists the third-party material contained in or used by this repository: where it comes from, which notices
and terms accompany it at its source, and which files or parts of files contain it. It is a factual record. The full
license texts are in [LICENSES/](LICENSES).

The material listed here is not covered by the EqualDoX Attribution License in [LICENSE](LICENSE); it keeps the
terms of its source. Everything else in the repository is EqualDoX's own work.

## Overview

| Files | Material | License |
|---|---|---|
| `src/fonts/ps.h`, `ps.cpp`, `cmap.h`, `cmap.cpp`, `programs.h`, `programs.cpp`, `charstrings.h`, `charstrings.cpp`, `truetype.cpp`, `cff.cpp`, `type1.cpp`, `model.h`, `pdfont.cpp` | C++ translation of veraPDF 1.30.2 font parsers and font model | MPL-2.0 |
| `src/fonts/generated/font_tables.cpp` | font data tables extracted from veraPDF 1.30.2 (Adobe, Apple and ISO data) | MPL-2.0 (precaution) AND BSD-3-Clause AND Adobe AFM terms |
| `src/fonts/generated/font_cmaps.cpp` | Adobe predefined CMap resources | BSD-3-Clause |
| `src/rules/document/xmp.h`, `xmp.cpp`; parts of `src/rules/document/xml_dom.cpp` | C++ translation of the Adobe XMP Toolkit for Java (veraPDF xmp-core) | BSD-3-Clause |
| `src/rules/containment/containment_table.inc` | adapted from the veraPDF validation profiles | CC BY 4.0 |
| part of `src/rules/pac_quality/jpx_reduce.cpp`; coefficients in `src/rules/pac_quality/pac_contrast.cpp` | from pdf.js | Apache-2.0 |
| `catalog/matrix.json` (Matterhorn failure conditions) | Matterhorn Protocol 1.1 | CC BY 4.0 |
| `catalog/wcag2.json` | WCAG 2.2 data | W3C Document License |
| `src/rules/document/lang_registry_data.cpp` | IANA Language Subtag Registry | CC0 1.0 |
| `tests/corpus/fonts/` | Corpus Sans, a subset of Liberation Sans 2.1.5 | SIL OFL 1.1 |

The files in the first rows carry the license in an SPDX header or, for parts of files, a notice that marks the
part.

## veraPDF 1.30.2 code (MPL-2.0)

[veraPDF](https://verapdf.org) (sources: <https://github.com/veraPDF>, tag `v1.30.2`) is Copyright (c) 2015-2026,
veraPDF Consortium <info@verapdf.org>, and offered under the GNU General Public License, version 3 or later, or the
Mozilla Public License, version 2.0 or later. The files below are used and distributed under the **Mozilla Public
License 2.0** ([LICENSES/MPL-2.0.txt](LICENSES/MPL-2.0.txt)); each carries the MPL notice (Exhibit A) and the
veraPDF copyright line in its header.

- `src/fonts/ps.h`, `src/fonts/ps.cpp`: PostScript tokenizer and interpreter for CMaps and Type 1 programs
  (veraPDF Parser: `org.verapdf.parser.*`, `org.verapdf.parser.postscript.*`).
- `src/fonts/cmap.h`, `src/fonts/cmap.cpp`: CMaps (`org.verapdf.pd.font.cmap.*`).
- `src/fonts/truetype.cpp`, `src/fonts/cff.cpp`, `src/fonts/type1.cpp`, `src/fonts/charstrings.h`,
  `src/fonts/charstrings.cpp`, `src/fonts/programs.h`, `src/fonts/programs.cpp`: TrueType, CFF and Type 1 programs
  and charstring widths (`org.verapdf.pd.font.{truetype,cff,type1,opentype}.*`).
- `src/fonts/model.h`, `src/fonts/pdfont.cpp`: PD font objects (`org.verapdf.pd.font.PDFont` and subclasses) and the
  font properties of the validation model.
- `src/fonts/generated/font_tables.cpp` (as a precaution, see "Font data" below).

**Obligations when distributing.** These files stay under the MPL-2.0 in every copy and modified version. Whoever
distributes DoX-Validate in executable form must make the source code of these files available (this repository
does so) and tell recipients how to obtain it, for example with a reference to this file. The files may be combined
with code under other licenses into a larger work (MPL-2.0 section 3.3).

The other modules of DoX-Validate reproduce the *results* of veraPDF's validation where its rules are the reference
(compatible behaviour, observed and documented), but their code is EqualDoX's own; comments that name veraPDF
classes or methods identify the behaviour being matched.

## veraPDF validation profiles (CC BY 4.0)

`src/rules/containment/containment_table.inc` is adapted from the veraPDF validation profiles 1.30.2 (PDF/UA-1,
PDF/UA-2, WTPDF 1.0; <https://github.com/veraPDF/veraPDF-validation-profiles>), (c) 2015 veraPDF Consortium, licensed
under the [Creative Commons Attribution 4.0 International License](https://creativecommons.org/licenses/by/4.0/)
([LICENSES/CC-BY-4.0.txt](LICENSES/CC-BY-4.0.txt)). Changes: the test expressions of the containment rules were
converted mechanically into a C++ rule table. The material is provided as-is, without warranties.

The rule ids `EQX-UA1-*`, `EQX-UA2-*`, `EQX-TS32005-*` and `EQX-WTPDF-*` and the `sources` entries of the catalogue
refer to the clause-based rule numbers of these profiles. The rule titles are EqualDoX's own wording.

## Adobe XMP Toolkit for Java, as in veraPDF xmp-core (BSD-3-Clause)

`src/rules/document/xmp.h`, `src/rules/document/xmp.cpp` and the parts of `src/rules/document/xml_dom.cpp` marked
"BSD-3-Clause portion" (`bytebuffer_encoding`, `latin1_convert`, `is_control`, `fix_controls`) are a C++ translation
of the Adobe XMP Toolkit for Java (XMPCore) as distributed in veraPDF 1.30.2 (`verapdf-xmp-core`), and of veraPDF's
additions to it.

```
Copyright 2006 Adobe Systems Incorporated. All Rights Reserved.
veraPDF additions: Copyright (c) 2015-2026, veraPDF Consortium.
```

They are licensed under the BSD 3-Clause License ([LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt)), the
license that `verapdf-xmp-core` 1.30.2 declares in its Maven metadata and under which Adobe publishes the XMP
Toolkit. (The `LICENSE.txt` in veraPDF's `xmp-core` directory is a BSD-3-Clause text that names another copyright
holder; the Java sources refer to "the Adobe license agreement accompanying" them.)

## Adobe resources (as redistributed with veraPDF)

### Predefined CMap resources (BSD-3-Clause)

`src/fonts/generated/font_cmaps.cpp` contains the 118 predefined CMap resources shipped with veraPDF 1.30.2 in a
pre-tokenised, compressed form. They are Adobe's CMap resources (<https://github.com/adobe-type-tools/cmap-resources>
and the UCS2 maps of <https://github.com/adobe-type-tools/mapping-resources-pdf>). The copyright lines of the source
files are "Copyright 1990-2009 Adobe Systems Incorporated" and "Copyright 1990-2019 / 2020 / 2022 / 2023 Adobe. All
rights reserved." All of them carry the following terms (BSD-3-Clause). The text below is quoted from the newer
files; the older files name "Adobe Systems Incorporated" instead of "Adobe".

```
Copyright 1990-2023 Adobe. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

Neither the name of Adobe nor the names of its contributors may be used to
endorse or promote products derived from this software without specific
prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

### Core 14 AFM font metrics

`src/fonts/generated/font_tables.cpp` contains the glyph widths and the encoding, character-set and code data of
the 14 standard Type 1 fonts. These were extracted from the Core 14 AFM files shipped with veraPDF
(Courier, Helvetica and Times in four styles each, Symbol, ZapfDingbats). Each file states
"Copyright (c) 1985–1997 Adobe Systems Incorporated. All Rights Reserved." (the years differ per file). The
trademark notices in the files are: "Helvetica is a trademark of Linotype-Hell AG and/or its subsidiaries.",
"Times is a trademark of Linotype-Hell AG and/or its subsidiaries." and "ITC Zapf Dingbats is a registered
trademark of International Typeface Corporation."

The ReadMe that accompanies the files reads:

> This file and the 14 PostScript(R) AFM files it accompanies may be used, copied, and distributed for any purpose
> and without charge, with or without modification, provided that all copyright notices are retained; that the AFM
> files are not distributed without this file; that all modifications to this file or any of the AFM files are
> prominently noted in the modified file(s); and that this paragraph is not modified. Adobe Systems has no
> responsibility or obligation to support the use of the AFM files.

This repository does not contain the AFM files themselves, only data extracted from them by
`tools/gen_font_data.py`.

### Adobe Glyph List (BSD-3-Clause)

`src/fonts/generated/font_tables.cpp` contains the glyph-name-to-Unicode mapping of veraPDF's
`AdobeGlyphList.txt`: the Adobe Glyph List 2.0 (4,281 entries, <https://github.com/adobe-type-tools/agl-aglfn>,
"Copyright 2002-2019 Adobe", BSD-3-Clause as quoted above with "Adobe" as the holder) plus one veraPDF entry
(`.notdef` to U+0000).

### Font data: sources and veraPDF-specific entries

`tools/gen_font_data.py` reads the data from the veraPDF 1.30.2 parser sources (Java array literals and resource
files); regenerating from tag `v1.30.2` of veraPDF-parser reproduces both generated files byte for byte. Apart from
the Adobe resources above, the tables are the simple-font encodings of ISO 32000-1 Annex D (Standard, MacRoman,
WinAnsi, MacExpert), the Macintosh standard glyph names of the TrueType `post` table, the Macintosh OS Roman mapping
and the CFF standard strings, encodings and charsets of Adobe Technical Note 5176. Three entries follow veraPDF
rather than the primary source and are kept because the validation results depend on them: the Macintosh glyph-name
table differs from Apple's list at 11 positions, the AGL has the `.notdef` entry, and the Adobe-Japan1-UCS2 CMap
declares 41 instead of 42 `bfrange` entries in one block.

## pdf.js (Apache-2.0)

[pdf.js](https://github.com/mozilla/pdf.js) is Copyright 2012 Mozilla Foundation and licensed under the Apache
License, Version 2.0 ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); the repository has no NOTICE file.

- `src/rules/pac_quality/jpx_reduce.cpp`, the part between the markers "[Apache-2.0 portion" and "[end of
  Apache-2.0 portion]": the MQ arithmetic decoder and the code-block decoder with its context-label tables are a C++
  translation of `src/core/arithmetic_decoder.js` and the `BitModel` class of `src/core/jpx.js` (v4.0.379),
  modified (translated to C++ and adapted to this decoder). The rest of the file is EqualDoX's own work.
- `src/rules/pac_quality/pac_contrast.cpp` (`pac_cmyk_paint`) uses the polynomial coefficients for converting
  DeviceCMYK to RGB that pdf.js publishes in `src/core/colorspace.js` (`DeviceCmykCS`).

## Standards data in the catalogue

- **Matterhorn Protocol 1.1** (CC BY 4.0). `catalog/matrix.json` lists the failure conditions of the Matterhorn
  Protocol 1.1 with their ids (prefixed `MH-`) and texts, and rules reference the ids. Source: "Matterhorn Protocol
  1.1", Copyright (c) 2021 PDF Association,
  <https://pdfa.org/wp-content/uploads/2021/04/Matterhorn-Protocol-1-1.pdf>, licensed under the
  [Creative Commons Attribution 4.0 International License](https://creativecommons.org/licenses/by/4.0/). Changes: the
  ids were prefixed and EqualDoX's coverage classification was added; the texts are unchanged. Provided as-is,
  without warranties.
- **WCAG 2.x** (W3C Document License). `catalog/wcag2.json` lists the success criterion numbers, handles and levels
  per WCAG version, taken from the W3C WCAG 2.2 data (<https://www.w3.org/WAI/WCAG22/wcag.json>) of the W3C
  Recommendation [Web Content Accessibility Guidelines (WCAG) 2.2](https://www.w3.org/TR/WCAG22/). Copyright (c)
  2020-2024 World Wide Web Consortium. [W3C Document License](https://www.w3.org/copyright/document-license/).
- **EN 301 549 V3.2.1** (c) ETSI, CEN and CENELEC 2021. `catalog/matrix.json` uses the clause numbers and the short
  titles of chapter 10 as references; no requirement text is reproduced.
- **ISO 14289-1, ISO 14289-2, ISO 32000-1/-2, ISO/TS 32005** (c) ISO, and **WTPDF 1.0** (c) 2024 PDF Association
  (CC BY 4.0). The catalogue refers to their clause numbers and to the WTPDF declaration URIs; rule titles and
  descriptions are EqualDoX's own wording.
- **PAC 2026** (26.1.0.0, axes4). The `EQX-PAC-*` and `EQX-PACQ-*` rules check conditions that PAC reports; their
  behaviour was determined by black-box testing of PAC. The rule ids and the `check` / `condition` entries of the
  rule sources are PAC's check and condition identifiers, used as cross-references; the rule titles are EqualDoX's
  own wording. PAC is not included or executed.

## IANA Language Subtag Registry (CC0 1.0)

`src/rules/document/lang_registry_data.cpp` lists the language subtags and grandfathered tags of the
[IANA Language Subtag Registry](https://www.iana.org/assignments/language-subtag-registry/language-subtag-registry)
(File-Date 2026-09-17; the file records the source URL and the SHA-256 of the copy used). It is generated by
`tools/gen_lang_registry.py`. IANA dedicates its registries to the public domain (CC0 1.0,
<https://www.iana.org/help/licensing-terms>).

## Test material

- `tests/corpus/fonts/`: Corpus Sans, an ASCII subset of Liberation Sans 2.1.5 ("Digitized data copyright (c) 2010
  Google Corporation with Reserved Font Arimo, Tinos and Cousine. Copyright (c) 2012 Red Hat, Inc. with Reserved
  Font Name Liberation."), renamed as the SIL Open Font License 1.1 requires for modified versions; it stays under
  the OFL (`tests/corpus/fonts/OFL.txt`).
- `tests/corpus/public/manifest.json` lists public documents with their licenses (among them the veraPDF test corpus,
  CC BY 4.0). Only the manifest is committed; `fetch_public.py` downloads the files.

## Libraries linked at build time

These libraries are not part of this repository. They are found on the build system through `pkg-config` and
linked into the `dox-validate` binary; binary distributions must include their notices:

| Library | Version | Use | License (as published by the project) |
|---|---|---|---|
| [qpdf](https://github.com/qpdf/qpdf) | ≥ 12 | PDF object layer, filters, encryption, repair | Apache License 2.0 |
| [pugixml](https://pugixml.org/) | any packaged version | annotation rich text (`/RC`) | MIT License |
| [zlib](https://zlib.net/) | any packaged version | Flate decoding, compressed generated data | zlib License |

qpdf in turn uses a crypto provider chosen when qpdf is built (its native implementation, OpenSSL or GnuTLS) and
libjpeg. Their notices come with the qpdf package of the build system.

A build configured with `-DDOX_STATIC_DEPS=ON` does not use the build system's qpdf and pugixml. It builds
qpdf 12.4.2 (Apache License 2.0, with its native crypto provider) and pugixml 1.16 (MIT License) from their upstream
release archives and links them into the binary, together with the compiler's C++ runtime (libstdc++ and libgcc:
GPL-3.0-or-later with the GCC Runtime Library Exception). zlib and libjpeg stay shared libraries of the system. A
binary distribution of such a build embeds these libraries and must include their notices.
