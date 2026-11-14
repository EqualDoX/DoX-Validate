# PDF/UA-1 tool comparison, cold start (one process per document)

148 documents processed by every variant in all 5 repetitions. Excluded runs (variant, document):
dox edge-zero-pages (5x), dox encrypted-user-password (5x), dox malformed-bad-xref (5x), dox malformed-garbage (5x), dox malformed-header-only (5x), dox malformed-no-root (5x), dox malformed-not-pdf (5x), dox malformed-truncated (5x), dox malformed-zero-byte (5x), dox-ua1rules edge-zero-pages (5x), dox-ua1rules encrypted-user-password (5x), dox-ua1rules malformed-bad-xref (5x), dox-ua1rules malformed-garbage (5x), dox-ua1rules malformed-header-only (5x), dox-ua1rules malformed-no-root (5x), dox-ua1rules malformed-not-pdf (5x), dox-ua1rules malformed-truncated (5x), dox-ua1rules malformed-zero-byte (5x), outloud-conf encrypted-user-password (5x), outloud-conf malformed-garbage (5x), outloud-conf malformed-header-only (5x), outloud-conf malformed-no-root (5x), outloud-conf malformed-not-pdf (5x), outloud-conf malformed-truncated (5x), outloud-conf malformed-zero-byte (5x), vellum-ua1 encrypted-user-password (5x), vellum-ua1 malformed-bad-xref (5x), vellum-ua1 malformed-garbage (5x), vellum-ua1 malformed-header-only (5x), vellum-ua1 malformed-no-root (5x), vellum-ua1 malformed-not-pdf (5x), vellum-ua1 malformed-truncated (5x), vellum-ua1 malformed-zero-byte (5x).

| group | tool | docs | pages | median wall | IQR | docs/s | pages/s | median peak RSS | max peak RSS | time ÷ dox-validate (full) |
|---|---|---|---|---|---|---|---|---|---|---|
| all | dox-validate (full catalogue) | 148 | 14000 | 6.51 ms | 5.97 ms–7.63 ms | 9.01 | 852.4 | 20.6 MB | 722 MB | 1 (reference) |
| all | dox-validate --rules EQX-UA1- | 148 | 14000 | 6.34 ms | 5.84 ms–7.35 ms | 12.9 | 1,223 | 20.0 MB | 689 MB | 0.96× (0.927×) |
| all | VellumPdf 2.3.3 -p ua1 | 148 | 14000 | 4.55 ms | 4.1 ms–6.59 ms | 2.96 | 279.7 | 12.9 MB | 241 MB | 0.717× (0.876×) |
| all | outloud 0.2.1 --layer conformance | 148 | 14000 | 397 ms | 357 ms–424 ms | 0.931 | 88.04 | 87.5 MB | 546 MB | 58.1× (43.2×) |
| small | dox-validate (full catalogue) | 127 | 166 | 6.34 ms | 5.84 ms–6.94 ms | 137 | 178.8 | 20.3 MB | 29 MB | 1 (reference) |
| small | dox-validate --rules EQX-UA1- | 127 | 166 | 6.21 ms | 5.76 ms–6.74 ms | 145 | 189.2 | 20.0 MB | 27 MB | 0.98× (0.966×) |
| small | VellumPdf 2.3.3 -p ua1 | 127 | 166 | 4.38 ms | 4.07 ms–5.09 ms | 151 | 197.1 | 12.8 MB | 39 MB | 0.697× (0.754×) |
| small | outloud 0.2.1 --layer conformance | 127 | 166 | 393 ms | 353 ms–408 ms | 2.58 | 3.366 | 87.5 MB | 102 MB | 59.5× (57×) |
| medium | dox-validate (full catalogue) | 11 | 988 | 74.3 ms | 48.2 ms–147 ms | 5.71 | 513.3 | 33.4 MB | 243 MB | 1 (reference) |
| medium | dox-validate --rules EQX-UA1- | 11 | 988 | 61.2 ms | 37.2 ms–116 ms | 7.67 | 688.7 | 32.0 MB | 223 MB | 0.771× (0.771×) |
| medium | VellumPdf 2.3.3 -p ua1 | 11 | 988 | 224 ms | 77.8 ms–584 ms | 1.04 | 93.17 | 45.1 MB | 198 MB | 2.67× (2.59×) |
| medium | outloud 0.2.1 --layer conformance | 11 | 988 | 837 ms | 676 ms–1.62 s | 0.747 | 67.09 | 106.4 MB | 307 MB | 11.2× (11.2×) |
| large | dox-validate (full catalogue) | 10 | 12846 | 1.29 s | 465 ms–2.12 s | 0.737 | 946.6 | 246.6 MB | 722 MB | 1 (reference) |
| large | dox-validate --rules EQX-UA1- | 10 | 12846 | 893 ms | 321 ms–1.53 s | 1.09 | 1,406 | 216.5 MB | 689 MB | 0.698× (0.676×) |
| large | VellumPdf 2.3.3 -p ua1 | 10 | 12846 | 1.51 s | 617 ms–7.35 s | 0.259 | 332.7 | 96.6 MB | 241 MB | 1.47× (1.78×) |
| large | outloud 0.2.1 --layer conformance | 10 | 12846 | 4.3 s | 2.74 s–18.3 s | 0.105 | 135.3 | 286.6 MB | 546 MB | 6.34× (5.64×) |
