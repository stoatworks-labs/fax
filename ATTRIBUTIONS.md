# Attributions

Fax is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Effect skeleton, harness, --pipe and verify — Stoatworks teletext

<https://github.com/stoatworks-labs/teletext>  
Licence: MIT  
Copyright: Stoatworks Labs

The read-back-then-CPU plugin shape, the harness's session and parameter plumbing, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1), the verify script and the negative-control pattern are teletext's, which had them from rebate and pitch; the host clock-unit voting is readout's by way of both; the software-renderer switch is wipe's by way of repousse.

### 5x7 bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The sending machine's header line is printed in graticule's 5x7 font, copied unchanged by way of teletext, each dot four pels wide and half a millimetre of lines tall.

### PassBuffer, the sweep and CI — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer, tools/sweep.py and the shape of the CI workflow are tinsel's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

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

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### golang.org/x/image/ccitt — The Go Authors

<https://pkg.go.dev/golang.org/x/image/ccitt>  
Licence: BSD-3-Clause  
Copyright: The Go Authors

A CCITT Group 3 / Group 4 decoder that shares no code with this repo. tools/gocheck (a development check, not part of the plugin) decodes the plugin's MH and T.6 streams with it and gets the page back exactly; its table.go is one of the two transcriptions tools/check_tables.py compares source/T4.h against, bit for bit, and the code-length fixture was extracted from it. None of its code is copied or vendored.

### pdfminer.six — Yusuke Shinyama and the pdfminer.six contributors

<https://github.com/pdfminer/pdfminer.six>  
Licence: MIT  
Copyright: Yusuke Shinyama and the pdfminer.six contributors

The second independent transcription of the T.4 code tables (its ccitt.py) that tools/check_tables.py compares source/T4.h against, read from wherever it is installed: 204 codes, 408 comparisons, none differ. Nothing is copied or vendored.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The Group 3 fax machine

The streak, the wedge, the repeated line, the squashed standard-resolution page and the header line along the top are what a 1980s fax did. No code, firmware or binaries from any fax machine, modem or fax software were used or examined.

## Standards and published specifications

What the implementation is measured against.

- **ITU-T Recommendation T.4 (07/2003), "Standardization of Group 3 facsimile terminals for document transmission"** — 1728 pels along 215 mm; 3.85, 7.7 and 15.4 lines/mm; the Modified Huffman terminating and make-up codes (Tables 2, 3a and 3b), transcribed by hand; the two-dimensional pass, horizontal and vertical modes (Table 4) and their coding procedure; EOL, the MR tag bit, fill, the minimum scan-line time and RTC; and the parameter K (4.2.1.1: 2 at standard, 4 at 200 and 8 at 400 lines/25.4 mm, which the note to Table 2 makes 7.7 and 15.4 lines/mm).
- **ITU-T Recommendation T.6, "Facsimile coding schemes and coding control functions for Group 4 facsimile apparatus"** — The all-two-dimensional stream fxtest --export writes for the independent decoder; its 2-D coding is T.4's.
- **ITU-T Recommendation T.30** — The negotiated minimum scan-line times (0, 5, 10, 20, 40 ms), of which this uses 10.
- **ITU-R BT.709** — The luma coefficients 0.2126, 0.7152, 0.0722 the scan thresholds.
- **B. E. Bayer, "An optimum method for two-level rendition of continuous-tone pictures" (IEEE ICC, 1973)** — The 8 x 8 ordered-dither matrix of the Halftone mode.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix behind the line's bit errors, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
