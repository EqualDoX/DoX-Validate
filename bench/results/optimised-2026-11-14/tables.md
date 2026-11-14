# PDF/UA-1 tool comparison, cold start (one process per document)

148 documents processed by every variant in all 5 repetitions. Excluded runs (variant, document):
cpdf-ua1 encrypted-user-password (5x), cpdf-ua1 libreoffice-custom-shape-tutorial (5x), cpdf-ua1 libreoffice-draw-guide-268 (5x), cpdf-ua1 malformed-garbage (5x), cpdf-ua1 malformed-header-only (5x), cpdf-ua1 malformed-no-root (5x), cpdf-ua1 malformed-not-pdf (5x), cpdf-ua1 malformed-truncated (5x), cpdf-ua1 malformed-zero-byte (5x), dox edge-zero-pages (5x), dox encrypted-user-password (5x), dox malformed-bad-xref (5x), dox malformed-garbage (5x), dox malformed-header-only (5x), dox malformed-no-root (5x), dox malformed-not-pdf (5x), dox malformed-truncated (5x), dox malformed-zero-byte (5x), dox-base edge-zero-pages (5x), dox-base encrypted-user-password (5x), dox-base malformed-bad-xref (5x), dox-base malformed-garbage (5x), dox-base malformed-header-only (5x), dox-base malformed-no-root (5x), dox-base malformed-not-pdf (5x), dox-base malformed-truncated (5x), dox-base malformed-zero-byte (5x), dox-shared edge-zero-pages (5x), dox-shared encrypted-user-password (5x), dox-shared malformed-bad-xref (5x), dox-shared malformed-garbage (5x), dox-shared malformed-header-only (5x), dox-shared malformed-no-root (5x), dox-shared malformed-not-pdf (5x), dox-shared malformed-truncated (5x), dox-shared malformed-zero-byte (5x), dox-ua1rules edge-zero-pages (5x), dox-ua1rules encrypted-user-password (5x), dox-ua1rules malformed-bad-xref (5x), dox-ua1rules malformed-garbage (5x), dox-ua1rules malformed-header-only (5x), dox-ua1rules malformed-no-root (5x), dox-ua1rules malformed-not-pdf (5x), dox-ua1rules malformed-truncated (5x), dox-ua1rules malformed-zero-byte (5x), outloud-conf encrypted-user-password (5x), outloud-conf malformed-garbage (5x), outloud-conf malformed-header-only (5x), outloud-conf malformed-no-root (5x), outloud-conf malformed-not-pdf (5x), outloud-conf malformed-truncated (5x), outloud-conf malformed-zero-byte (5x), vellum-ua1 encrypted-user-password (5x), vellum-ua1 malformed-bad-xref (5x), vellum-ua1 malformed-garbage (5x), vellum-ua1 malformed-header-only (5x), vellum-ua1 malformed-no-root (5x), vellum-ua1 malformed-not-pdf (5x), vellum-ua1 malformed-truncated (5x), vellum-ua1 malformed-zero-byte (5x), vp-ua1 edge-zero-pages (5x), vp-ua1 encrypted-user-password (5x), vp-ua1 malformed-bad-xref (5x), vp-ua1 malformed-garbage (5x), vp-ua1 malformed-header-only (5x), vp-ua1 malformed-no-root (5x), vp-ua1 malformed-not-pdf (5x), vp-ua1 malformed-truncated (5x), vp-ua1 malformed-zero-byte (5x).

| group | tool | docs | pages | median wall | IQR | docs/s | pages/s | median peak RSS | max peak RSS | time ÷ dox-validate (full) |
|---|---|---|---|---|---|---|---|---|---|---|
| all | dox-validate (full catalogue) | 148 | 14000 | 1.71 ms | 1.51 ms–2.31 ms | 23 | 2,176 | 8.0 MB | 183 MB | 1 (reference) |
| all | dox-validate, default build (shared libqpdf) | 148 | 14000 | 3.81 ms | 3.63 ms–4.5 ms | 21 | 1,985 | 15.9 MB | 190 MB | 2.2× (1.97×) |
| all | dox-validate before the optimisation (729a5c5) | 148 | 14000 | 5.32 ms | 4.72 ms–5.88 ms | 10.6 | 1,000 | 20.6 MB | 722 MB | 3.07× (2.77×) |
| all | dox-validate --rules EQX-UA1- | 148 | 14000 | 1.65 ms | 1.5 ms–2.24 ms | 34.2 | 3,234 | 9.3 MB | 161 MB | 0.955× (0.914×) |
| all | VellumPdf 2.3.3 -p ua1 | 148 | 14000 | 3.5 ms | 3.24 ms–5.23 ms | 3.42 | 323.1 | 13.3 MB | 241 MB | 2.13× (2.39×) |
| all | outloud 0.2.1 --layer conformance | 148 | 14000 | 301 ms | 298 ms–306 ms | 1.12 | 106 | 78.4 MB | 537 MB | 175× (117×) |
| small | dox-validate (full catalogue) | 127 | 166 | 1.68 ms | 1.45 ms–1.87 ms | 442 | 577.2 | 7.9 MB | 14 MB | 1 (reference) |
| small | dox-validate, default build (shared libqpdf) | 127 | 166 | 3.77 ms | 3.56 ms–3.99 ms | 229 | 299.1 | 15.8 MB | 22 MB | 2.23× (2.17×) |
| small | dox-validate before the optimisation (729a5c5) | 127 | 166 | 5.24 ms | 4.63 ms–5.49 ms | 168 | 220.1 | 20.3 MB | 29 MB | 3.13× (2.91×) |
| small | dox-validate --rules EQX-UA1- | 127 | 166 | 1.61 ms | 1.45 ms–1.84 ms | 486 | 635 | 9.3 MB | 15 MB | 0.963× (0.957×) |
| small | VellumPdf 2.3.3 -p ua1 | 127 | 166 | 3.43 ms | 3.17 ms–4.08 ms | 186 | 243.2 | 13.1 MB | 39 MB | 2.1× (2.15×) |
| small | outloud 0.2.1 --layer conformance | 127 | 166 | 300 ms | 298 ms–304 ms | 3.29 | 4.304 | 78.4 MB | 93 MB | 179× (162×) |
| medium | dox-validate (full catalogue) | 11 | 988 | 34.2 ms | 22.7 ms–89.2 ms | 12.2 | 1,096 | 15.5 MB | 140 MB | 1 (reference) |
| medium | dox-validate, default build (shared libqpdf) | 11 | 988 | 39 ms | 26.1 ms–94 ms | 11.2 | 1,010 | 23.4 MB | 147 MB | 1.1× (1.15×) |
| medium | dox-validate before the optimisation (729a5c5) | 11 | 988 | 64.2 ms | 39.5 ms–134 ms | 6.36 | 571.7 | 33.9 MB | 242 MB | 1.77× (1.83×) |
| medium | dox-validate --rules EQX-UA1- | 11 | 988 | 28.2 ms | 17.7 ms–59.5 ms | 16.9 | 1,515 | 15.7 MB | 128 MB | 0.745× (0.726×) |
| medium | VellumPdf 2.3.3 -p ua1 | 11 | 988 | 186 ms | 65.8 ms–495 ms | 1.17 | 104.7 | 45.6 MB | 198 MB | 5.3× (4.8×) |
| medium | outloud 0.2.1 --layer conformance | 11 | 988 | 659 ms | 535 ms–1.39 s | 0.857 | 76.99 | 97.7 MB | 298 MB | 16.9× (20.3×) |
| large | dox-validate (full catalogue) | 10 | 12846 | 433 ms | 154 ms–961 ms | 1.91 | 2,449 | 61.5 MB | 183 MB | 1 (reference) |
| large | dox-validate, default build (shared libqpdf) | 10 | 12846 | 461 ms | 167 ms–990 ms | 1.81 | 2,328 | 69.6 MB | 190 MB | 1.08× (1.07×) |
| large | dox-validate before the optimisation (729a5c5) | 10 | 12846 | 1.07 s | 396 ms–1.85 s | 0.868 | 1,116 | 247.8 MB | 722 MB | 2.39× (2.33×) |
| large | dox-validate --rules EQX-UA1- | 10 | 12846 | 292 ms | 100 ms–620 ms | 2.93 | 3,761 | 59.7 MB | 161 MB | 0.652× (0.654×) |
| large | VellumPdf 2.3.3 -p ua1 | 10 | 12846 | 1.29 s | 538 ms–6.32 s | 0.301 | 386.8 | 97.2 MB | 241 MB | 3.68× (4.19×) |
| large | outloud 0.2.1 --layer conformance | 10 | 12846 | 3.9 s | 2.39 s–16 s | 0.124 | 159.3 | 278.1 MB | 537 MB | 13.8× (13.4×) |

## Further tools in the same run

Each tool on the documents of the comparison above that it processed in every repetition; dox-validate (full catalogue) on the same documents.

| group | tool | docs | median wall | pages/s | median peak RSS | max peak RSS | dox-validate median wall | dox-validate pages/s | time ÷ dox-validate |
|---|---|---|---|---|---|---|---|---|---|
| all | veraPDF 1.30.4 -f ua1 | 148 of 148 | 629 ms | 92.34 | 238.5 MB | 5,164 MB | 1.71 ms | 2,176 | 371× (223×) |
| all | cpdf 2.9.3 -verify PDF/UA-1(matterhorn) | 146 of 148 | 5.25 ms | 464.6 | 13.2 MB | 1,161 MB | 1.71 ms | 2,276 | 3.12× (3.14×) |
| small | veraPDF 1.30.4 -f ua1 | 127 of 127 | 617 ms | 2.093 | 237.2 MB | 280 MB | 1.68 ms | 577.2 | 379× (331×) |
| small | cpdf 2.9.3 -verify PDF/UA-1(matterhorn) | 127 of 127 | 5.2 ms | 198 | 13.1 MB | 22 MB | 1.68 ms | 577.2 | 3.12× (3.09×) |
| medium | veraPDF 1.30.4 -f ua1 | 11 of 11 | 1.29 s | 51.59 | 571.9 MB | 4,096 MB | 34.2 ms | 1,096 | 36.5× (33.9×) |
| medium | cpdf 2.9.3 -verify PDF/UA-1(matterhorn) | 10 of 11 | 106 ms | 217.9 | 39.9 MB | 354 MB | 34.1 ms | 1,054 | 3.33× (3.51×) |
| large | veraPDF 1.30.4 -f ua1 | 10 of 10 | 4.36 s | 241.7 | 2799.1 MB | 5,164 MB | 433 ms | 2,449 | 10.6× (11.8×) |
| large | cpdf 2.9.3 -verify PDF/UA-1(matterhorn) | 9 of 10 | 1.09 s | 513.6 | 208.0 MB | 1,161 MB | 461 ms | 2,582 | 2.84× (3.47×) |

## Medium and large documents

Median wall time / peak RSS per document.

| document | pages | dox-validate before the optimisation (729a5c5) | dox-validate (full catalogue) | dox-validate, default build (shared libqpdf) | VellumPdf 2.3.3 -p ua1 | outloud 0.2.1 --layer conformance |
|---|---|---|---|---|---|---|
| nist-sp800-53Ar5 | 733 | 2.58 s / 722 MB | 1.12 s / 183 MB | 1.17 s / 190 MB | 10.7 s / 241 MB | 25.2 s / 392 MB |
| govinfo-uscode-2022-title26 | 4215 | 2.14 s / 446 MB | 1.01 s / 36 MB | 1.05 s / 44 MB | 5.34 s / 79 MB | 15.8 s / 537 MB |
| govinfo-budget-2025-app | 1298 | 1.75 s / 415 MB | 945 ms / 55 MB | 971 ms / 63 MB | 9.24 s / 99 MB | 16.7 s / 470 MB |
| govinfo-fr-2024-12-30 | 758 | 1.22 s / 335 MB | 461 ms / 68 MB | 495 ms / 76 MB | 1.33 s / 126 MB | 4.34 s / 456 MB |
| nist-sp800-53r5 | 492 | 1.2 s / 268 MB | 661 ms / 142 MB | 715 ms / 150 MB | 3.65 s / 218 MB | 7.38 s / 297 MB |
| irs-p17-2023 | 142 | 956 ms / 242 MB | 433 ms / 140 MB | 468 ms / 147 MB | 7.19 s / 198 MB | 4.86 s / 298 MB |
| gen-large-2000-complex | 2000 | 940 ms / 227 MB | 342 ms / 127 MB | 368 ms / 135 MB | 716 ms / 114 MB | 2.86 s / 259 MB |
| libreoffice-draw-guide-268 | 350 | 904 ms / 152 MB | 406 ms / 88 MB | 427 ms / 96 MB | 1.25 s / 95 MB | 3.46 s / 192 MB |
| gen-large-2000-simple | 2000 | 455 ms / 87 MB | 180 ms / 18 MB | 194 ms / 26 MB | 655 ms / 54 MB | 2.95 s / 145 MB |
| gen-large-500-complex | 500 | 217 ms / 74 MB | 78.4 ms / 38 MB | 85 ms / 45 MB | 189 ms / 59 MB | 927 ms / 122 MB |
| gao-yellow-book-2024 | 260 | 148 ms / 57 MB | 83.6 ms / 25 MB | 89.7 ms / 33 MB | 495 ms / 49 MB | 1.39 s / 105 MB |
| libreoffice-custom-shape-tutorial | 132 | 134 ms / 34 MB | 89.2 ms / 15 MB | 94 ms / 23 MB | 562 ms / 73 MB | 1.42 s / 107 MB |
| arxiv-2204.02311v5-palm | 87 | 131 ms / 43 MB | 91 ms / 25 MB | 98.6 ms / 34 MB | 197 ms / 52 MB | 937 ms / 116 MB |
| gen-large-500-simple | 500 | 114 ms / 36 MB | 46 ms / 10 MB | 51 ms / 18 MB | 170 ms / 38 MB | 953 ms / 95 MB |
| nist-sp800-63-3 | 76 | 106 ms / 42 MB | 69.5 ms / 22 MB | 74 ms / 30 MB | 430 ms / 58 MB | 1.14 s / 127 MB |
| cdc-mmwr-7301-h | 26 | 64.2 ms / 38 MB | 34.1 ms / 20 MB | 39 ms / 28 MB | 135 ms / 45 MB | 578 ms / 98 MB |
| gao-green-book-2014 | 86 | 61.9 ms / 33 MB | 34.2 ms / 16 MB | 37.7 ms / 23 MB | 186 ms / 46 MB | 659 ms / 89 MB |
| gen-medium-100-complex | 100 | 42.3 ms / 30 MB | 16.1 ms / 14 MB | 19.4 ms / 22 MB | 44.5 ms / 38 MB | 431 ms / 87 MB |
| nist-cswp29 | 32 | 39.5 ms / 29 MB | 22.7 ms / 13 MB | 26.1 ms / 21 MB | 120 ms / 39 MB | 559 ms / 83 MB |
| arxiv-2302.13971v1-llama | 27 | 36 ms / 25 MB | 24.7 ms / 12 MB | 27.2 ms / 20 MB | 65.8 ms / 39 MB | 535 ms / 95 MB |
| gen-medium-20-simple | 20 | 8.3 ms / 20 MB | 3.12 ms / 8 MB | 5.25 ms / 16 MB | 13.9 ms / 29 MB | 323 ms / 78 MB |

## Start-up

One process each; median over the repetitions (min–max).

| tool | call | runs | wall | min–max | CPU (user+sys) | peak RSS |
|---|---|---|---|---|---|---|
| dox-validate (full catalogue) | --version | 10 | 0.654 ms | 0.569 ms–0.736 ms | 0.581 ms | 4.1 MB |
| dox-validate, default build (shared libqpdf) | --version | 10 | 2.6 ms | 2.2 ms–2.81 ms | 2.55 ms | 12.1 MB |
| dox-validate before the optimisation (729a5c5) | --version | 10 | 2.4 ms | 2.25 ms–2.96 ms | 2.33 ms | 13.3 MB |
| VellumPdf 2.3.3 -p ua1 | --version | 10 | 2.17 ms | 1.99 ms–2.36 ms | 2.14 ms | 9.9 MB |
| outloud 0.2.1 --layer conformance | --version | 10 | 189 ms | 186 ms–196 ms | 188 ms | 42.5 MB |
| dox-validate (full catalogue) | smallest corpus file | 10 | 1.32 ms | 1.27 ms–1.39 ms | 1.24 ms | 8.6 MB |
| dox-validate, default build (shared libqpdf) | smallest corpus file | 10 | 3.18 ms | 2.94 ms–3.35 ms | 3.11 ms | 16.7 MB |
| dox-validate before the optimisation (729a5c5) | smallest corpus file | 10 | 3.86 ms | 3.76 ms–4.37 ms | 3.8 ms | 19.3 MB |
| VellumPdf 2.3.3 -p ua1 | smallest corpus file | 10 | 3.11 ms | 2.84 ms–3.28 ms | 3.08 ms | 12.4 MB |
| outloud 0.2.1 --layer conformance | smallest corpus file | 10 | 308 ms | 302 ms–453 ms | 306 ms | 76.5 MB |

## Batch: one process per tool over the whole corpus

Wall time of the whole process, all passes; median over the repetitions (min–max).

| tool | runs | documents per run | wall | min–max | pages/s | CPU | peak RSS | time ÷ dox-validate |
|---|---|---|---|---|---|---|---|---|
| dox-validate (full catalogue) | 3 | 471 | 18.5 s | 18.4 s–18.5 s | 2,274 | 18.4 s | 185 MB | 1 (reference) |
| dox-validate before the optimisation (729a5c5) | 3 | 471 | 40 s | 39.8 s–40 s | 1,050 | 39.9 s | 820 MB | 2.17× |
| VellumPdf 2.3.3 -p ua1 | 3 | 471 | 123 s | 122 s–124 s | 341 | 126 s | 575 MB | 6.67× |
| outloud 0.2.1 --layer conformance | 3 | 471 | 267 s | 266 s–269 s | 157.1 | 267 s | 1,328 MB | 14.5× |

## Parallel: one pass over the corpus with N workers

native = `--jobs N` threads in one process; pool = N concurrent one-document processes. Median over the repetitions. Peak RSS: largest single process, and the sampled sum over all live processes of the run.

| mode | tool | N | runs | wall | min–max | pages/s | scaling vs N=1 | CPU | max process RSS | peak tree RSS |
|---|---|---|---|---|---|---|---|---|---|---|
| native | dox-validate (full catalogue) | 1 | 5 | 6.19 s | 6.17 s–6.2 s | 2,261 | 1.00× | 6.18 s | 184 MB | 186 MB |
| native | dox-validate (full catalogue) | 2 | 5 | 3.27 s | 3.25 s–3.28 s | 4,282 | 1.89× | 6.48 s | 321 MB | 324 MB |
| native | dox-validate (full catalogue) | 4 | 5 | 2.05 s | 2.05 s–2.07 s | 6,817 | 3.01× | 6.49 s | 427 MB | 430 MB |
| native | dox-validate (full catalogue) | 8 | 5 | 1.75 s | 1.72 s–1.79 s | 7,986 | 3.53× | 9.19 s | 757 MB | 759 MB |
| native | dox-validate before the optimisation (729a5c5) | 1 | 5 | 13 s | 12.9 s–13 s | 1,080 | 1.00× | 12.9 s | 743 MB | 745 MB |
| native | dox-validate before the optimisation (729a5c5) | 2 | 5 | 7.54 s | 7.52 s–7.55 s | 1,857 | 1.72× | 14.1 s | 1,189 MB | 1,192 MB |
| native | dox-validate before the optimisation (729a5c5) | 4 | 5 | 4.96 s | 4.96 s–5.02 s | 2,821 | 2.61× | 14.3 s | 1,761 MB | 1,763 MB |
| native | dox-validate before the optimisation (729a5c5) | 8 | 5 | 4.13 s | 4.02 s–4.2 s | 3,390 | 3.14× | 20.3 s | 2,627 MB | 2,630 MB |
| pool | dox-validate (full catalogue) | 1 | 5 | 6.49 s | 6.46 s–6.5 s | 2,158 | 1.00× | 6.34 s | 182 MB | 184 MB |
| pool | dox-validate (full catalogue) | 2 | 5 | 3.25 s | 3.25 s–3.26 s | 4,304 | 1.99× | 6.34 s | 182 MB | 311 MB |
| pool | dox-validate (full catalogue) | 4 | 5 | 2.01 s | 2.01 s–2.04 s | 6,963 | 3.23× | 6.35 s | 182 MB | 312 MB |
| pool | dox-validate (full catalogue) | 8 | 5 | 1.67 s | 1.66 s–1.71 s | 8,362 | 3.88× | 8.81 s | 182 MB | 620 MB |
| pool | dox-validate before the optimisation (729a5c5) | 1 | 5 | 14 s | 14 s–14.1 s | 996.8 | 1.00× | 13.9 s | 722 MB | 724 MB |
| pool | dox-validate before the optimisation (729a5c5) | 2 | 5 | 7.37 s | 7.34 s–7.38 s | 1,901 | 1.91× | 14 s | 722 MB | 945 MB |
| pool | dox-validate before the optimisation (729a5c5) | 4 | 5 | 4.79 s | 4.78 s–4.85 s | 2,926 | 2.94× | 14.2 s | 722 MB | 1,143 MB |
| pool | dox-validate before the optimisation (729a5c5) | 8 | 5 | 3.88 s | 3.87 s–3.91 s | 3,609 | 3.62× | 20 s | 723 MB | 1,730 MB |
| pool | VellumPdf 2.3.3 -p ua1 | 1 | 5 | 43.2 s | 42.6 s–43.7 s | 323.8 | 1.00× | 44.2 s | 240 MB | 242 MB |
| pool | VellumPdf 2.3.3 -p ua1 | 2 | 5 | 24.2 s | 23.9 s–24.3 s | 577.4 | 1.78× | 46 s | 240 MB | 437 MB |
| pool | VellumPdf 2.3.3 -p ua1 | 4 | 5 | 15.8 s | 15.7 s–16 s | 884.2 | 2.73× | 49.6 s | 240 MB | 661 MB |
| pool | VellumPdf 2.3.3 -p ua1 | 8 | 5 | 13.8 s | 13.5 s–14.1 s | 1,014 | 3.13× | 58.8 s | 242 MB | 927 MB |
