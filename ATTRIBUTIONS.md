# Attributions

Fax is built on other people's work. This file lists what that work is, who did it,
and what it is doing here.

It is a **provisional hand copy**, written 2026-09-25. The fleet's copies are generated
from the master lists in the `stoatworks-backend` repo by `scripts/sync-attributions.py`;
this one will be replaced by that sync when the plugin is registered.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### 5x7 bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The sending machine's header line is printed in graticule's 5x7 font, copied unchanged by way of teletext, each dot four pels wide and half a millimetre of lines tall.

### Effect skeleton, harness, --pipe and verify — Stoatworks teletext

<https://github.com/stoatworks-labs/teletext>  
Licence: MIT  
Copyright: Stoatworks Labs

The read-back-then-CPU plugin shape, the harness's session and parameter plumbing, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1), the verify script and the negative-control pattern are teletext's, which had them from rebate and pitch; the host clock-unit voting is readout's by way of both; the software-renderer switch is wipe's by way of repousse.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly — listed because it is present in the checkout.

## Tools used to check this project, and not shipped

### golang.org/x/image/ccitt

<https://pkg.go.dev/golang.org/x/image/ccitt>  
Licence: BSD-3-Clause  
Copyright: The Go Authors

A CCITT Group 3 / Group 4 decoder that shares no code with this repo. `tools/gocheck` (a development check, not part of the plugin) decodes the plugin's MH and T.6 streams with it and compares the result with the page; its `table.go` is also one of the two transcriptions `tools/check_tables.py` compares `source/T4.h` against, and the code lengths in `tools/fixtures/t4-code-lengths.txt` were extracted from it. None of its code is copied or vendored.

### pdfminer.six

<https://github.com/pdfminer/pdfminer.six>  
Licence: MIT  
Copyright: Yusuke Shinyama and the pdfminer.six contributors

The second independent transcription of the T.4 code tables `tools/check_tables.py` compares against, read from wherever it is installed. Nothing is copied or vendored.

## Standards and published specifications

What the implementation is measured against.

- **ITU-T Recommendation T.4 (07/2003), "Standardization of Group 3 facsimile terminals for document transmission"** — 1728 pels along 215 mm; 3.85, 7.7 and 15.4 lines/mm; the Modified Huffman terminating and make-up codes (Tables 2, 3a and 3b), transcribed by hand; the two-dimensional pass, horizontal and vertical modes (Table 4) and their coding procedure; EOL, the MR tag bit, fill, the minimum scan-line time and RTC; the parameter K. The superfine K of 8 is recalled, not read from the text (AGENTS.md).
- **ITU-T Recommendation T.6, "Facsimile coding schemes and coding control functions for Group 4 facsimile apparatus"** — the all-two-dimensional stream `fxtest --export` writes for the independent decoder; its 2-D coding is T.4's.
- **ITU-T Recommendation T.30** — the negotiated minimum scan-line times (0, 5, 10, 20, 40 ms), of which this uses 10.
- **ITU-R BT.709** — the luma coefficients 0.2126, 0.7152, 0.0722 the scan thresholds.
- **B. E. Bayer, "An optimum method for two-level rendition of continuous-tone pictures" (IEEE ICC, 1973)** — the 8 x 8 ordered-dither matrix of the Halftone mode.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — the pcg_hash output mix behind the line's bit errors, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
