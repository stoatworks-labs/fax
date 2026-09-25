# fax

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The fax is not asserted but
> measured: an offline harness drives the real plugin class in a headless GL context and
> reads each claim back — the 204 code-table entries agree bit for bit with two
> independent transcriptions of ITU-T T.4 and every run length round-trips at exactly the
> standard's bits; an independent decoder (Go's `x/image/ccitt`) reads the plugin's own
> streams back to its page exactly; with a clean line the received page is the sent page,
> bit for bit, in MH and MR; one bit error in an MH line damages that line from the error
> on and nothing else; one in MR runs down no further than the next one-dimensional line,
> at most K = 2, 4 or 8 lines; with Repeat Line every concealed line is the line above;
> and in Page mode every line arrives within 2 ms of its bit count over the baud — with
> nine negative controls that prove each check can fail. It has **never been loaded into
> Resolume**; the only host it has met is
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host and is not
> Resolume. See [Status](#status).

The picture sent as a Group 3 fax over a noisy telephone line, as an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![The test card as a received fax on warm-grey thermal paper: a header line with the time, "FROM: SW FAX" and a page number; the sky and the colour patches rendered as ordered-dither halftone; a resolution wedge of black bars; a dark disc; and here and there a line repeated from the one above, where the receiver concealed a bit error](docs/hero.png)

<sub>One frame, rendered by `fxtest`, the offline harness — not captured from Resolume.
The test card at the defaults.</sub>

## The one idea

A Group 3 fax (ITU-T T.4) does not send pixels. It thresholds each scan line to black and
white — 1728 pels across 215 mm — and sends **run lengths** as variable-length codes,
the Modified Huffman code, one line at a time, each line ended by an EOL. In MR, most
lines are sent as **differences from the line above**, with a plain one-dimensional line
every K lines so that an error cannot run for ever.

This plugin sends the clip through a real T.4 coder, a real noisy line and a real
decoder, and the look of a fax falls out of the code rather than being drawn:

- **A bit error desynchronises the rest of its line.** The decoder reads the wrong code
  lengths until the line's EOL: a streak from the error to the right edge, and nothing
  on the next line.
- **In MR the error runs downwards**, because each following line is decoded against a
  corrupted reference, until the next one-dimensional line: a wedge at most K lines tall
  — 2 at standard resolution, 4 at fine, 8 at superfine.
- **Concealment.** A decoder that finds a bad line (an invalid code, a line that is not
  1728 pels, anything but fill before the EOL) repeats the line above — and in MR every
  line after it until the next one-dimensional line. That vertical stretch is the
  familiar fax glitch.
- **Resolution.** 3.85 lines per millimetre at standard against 8 pels, so a standard fax
  is squashed and doubled vertically: a 16:9 frame is 466 lines.
- **Transmission time.** White runs are cheap. In Page mode the page arrives line by line
  at the line rate — a blank page in 4.7 s at 14400 bit/s, a thresholded frame in about
  six, a halftone photograph in two and a half minutes — faster over blank paper, and never
  faster than the 10 ms minimum a scan line may take.
- **The header** is printed by the *sending* machine into the top of the page before it
  is coded, so it breaks on the line like everything else.

### What is drawn, and what is not

The code tables in `source/T4.h` were typed from the Recommendation by hand and are
checked three ways: against two independent transcriptions, against the standard's code
lengths, and by decoding the plugin's streams with a decoder that shares no code with it.
The receiver finds every EOL first and decodes each line from the stretch between two,
which is what confines an error to its line. The print pass draws the received page by
the exact area black pels cover in each output pixel, on warm-grey thermal paper or white
plain paper. Paper is opaque: the output's alpha is 1.

### The honest limit

Error Correction Mode — the retransmission real fax machines have used since the 1990s,
which is why a modern fax never streaks — is not modelled; this is the Group 3 fax of the
1980s. The error model (independent bit errors, optional bursts) is stated, not measured
from a modem, and the minimum scan-line time is fixed at 10 ms.

## Controls

| Group | |
| --- | --- |
| **Scan** | Fit (Frame, A4 Crop, A4 Letterbox), Resolution (Standard 3.85, Fine 7.7, Superfine 15.4 lines/mm), Threshold, Halftone (an 8 x 8 ordered dither, as fax photo modes did). |
| **Line** | Coding (MH, MR), Line Noise (0 clean; above it a bit error rate from 1e-6 to 1e-2), Bursts (each error a burst of up to 64 bits), Baud (2400, 4800, 9600, 14400), Mode (Page: a page captured and sent at the line rate; Live: every frame a whole new page). |
| **Receiver** | Concealment (Off, Repeat Line), Paper (Thermal, Plain), Header, Mix. |

The defaults are the frame as the page, standard resolution, Halftone at threshold 0.5,
MR at 14400 bit/s with Line Noise 0.4 (a bit error rate of 4e-5), Live, Repeat Line,
thermal paper and the header. Resolume's bundled clips are dark, and a plain threshold
prints most of them solid black; the halftone renders their tones. For the page arriving
line by line, choose Page — and turn Halftone off, or wait two and a half minutes a page.

## Status

**Unreleased — 25 September 2026.** A local v0.1.0: no download, no user guide, no
browser demo, no OpenFX port.

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4) against a fresh universal
Release build, running every picture check at **two rasters**, 320×180 and 1280×720, and
again at 320×180 on Apple's software renderer (what CI has). What it establishes, in
numbers:

| check | result |
| --- | --- |
| `--tables` | **204** codes (Tables 2, 3a, 3b, 4) agree bit for bit with golang.org/x/image and pdfminer.six (408 comparisons, 0 differ); **217** code lengths equal the fixture; every set prefix-free; at most 7 leading and 3 trailing zeros, so no EOL in valid data; **5,820** runs (0–2560, and to 5000 by 7s, both colours) round-trip at exactly the standard's bits |
| `gocheck` | golang.org/x/image/ccitt decodes the plugin's MH (T.4) and 2-D (T.6) streams of the card, thresholded and halftone, to the plugin's page: **0** of 100,656 bytes differ; it rejects a stream coded with VR1 written as VR2 |
| `--roundtrip` | 28 corpus pages using all nine 2-D modes and 8 pages through the plugin (MH/MR, standard/fine, threshold/halftone, header on, up to 4.4 million bits) decode to the sent page **bit for bit** |
| `--scan` | **0** wrong pels of 0.8–3.9 million per case against the scan recomputed in double, six cases across the three fits and three resolutions, halftone, and a transparent third that scans as paper |
| `--print` | **0** of 57,600 / 921,600 pixels off the page's pel coverage by more than one 8-bit step, four cases; alpha **255** everywhere over a partly transparent source |
| `--streak` | 47 single-bit errors in MH lines: **47** damaged no line but their own, none before the error; all felt (39 visibly, the rest a streak in the line's own colour); **0** pixels moved outside the damaged rows |
| `--wedge` | 23 errors at each of K = **2, 4, 8**, with concealment off and Repeat Line: all contained before the next one-dimensional line; with concealment off the tallest wedge is exactly K |
| `--conceal` | MH 113 and MR 161 concealed lines on a noisy halftone page, **every one** equal to the line above bit for bit |
| `--timing` | blank and busy pages, MH at 14400 and MR at 9600: all **466** line arrivals within 2 ms of ( bits through the line's EOL ) / baud; every blank line exactly the 10 ms floor; the blank page 4.66 s against 5.6 s busy |
| `--resize` | a resize to another aspect mid-page: the page identical to an unresized run on **every** frame |
| `--negative` | nine perturbed models — a swapped table entry, a wrong vertical mode, BT.601 luma, nearest-pel printing, no EOL resync, no MR refresh, the wrong line concealed, no fill, a flat line time — each **fails** its check |
| mutation | one character of the shipped GLSL (a pel's coverage over-counted by one) was caught by `--print` at both rasters, then reverted |
| `tools/sweep.py` | all **13** controls measurably change the picture, at 320×180 and 480×270 |
| shaders | all 3, as the plugin assembles them, compile through `glslc` |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused (exit 2); a failed render and a closed stdout exit 1, not SIGPIPE; options step between cues |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Fax` / `FX01` / `effect` and renders 120 frames through `plugMain` |

Render cost (`fxtest --bench`, best of three runs of 60 frames, `glFinish` both sides, on
a CPU and GPU shared with other builds), with the CPU half — read-back, the sending
machine, the line and the receiver — separately per page:

| mode | raster | ms/frame | % of a 60 fps frame | CPU a page: read-back / sender / line / receiver (ms) |
| --- | --- | --- | --- | --- |
| defaults (Live, halftone: ~2.2 million bits a page, every frame) | 1280x720 | 10.42 | 62.5% | 1.43 / 3.00 / 1.50 / 4.36 |
| | 1920x1080 | 9.15 | 54.9% | 1.52 / 2.42 / 1.35 / 3.81 |
| | 3840x2160 | 10.66 | 64.0% | 3.15 / 2.35 / 1.35 / 3.76 |
| Live, threshold (~90,000 bits a page) | 1280x720 | 0.73 | 4.4% | 0.30 / 0.16 / 0.06 / 0.18 |
| | 1920x1080 | 0.83 | 5.0% | 0.39 / 0.16 / 0.06 / 0.19 |
| | 3840x2160 | 1.76 | 10.6% | 1.32 / 0.17 / 0.06 / 0.18 |
| Page, threshold (between pages: the print pass alone) | 1280x720 | 0.03 | 0.2% | once a page, as Live threshold |
| | 1920x1080 | 0.04 | 0.2% | |
| | 3840x2160 | 0.06 | 0.4% | |
| Live, superfine halftone (~9 million bits a page) | 1280x720 | 32.17 | 193% | 1.03 / 11.57 / 5.56 / 13.88 |
| | 1920x1080 | 31.91 | 191% | 1.20 / 11.47 / 5.51 / 13.61 |
| | 3840x2160 | 32.66 | 196% | 1.86 / 11.30 / 5.54 / 13.83 |

At the defaults the CPU half is almost all of the frame: about 9 ms at 1080p, the
sender and the receiver each ~2.5–4 ms, the line (a hash per bit) ~1.4 ms. Superfine
halftone in Live mode is **not real-time** (about 31 fps). Page mode costs the print pass
between pages and one CPU spike of the Live figure per page.

### Not established

It has **never been loaded into Resolume.** Everything above was compiled, rendered and
measured offline against the real plugin class in a headless CGL context, plus an `oxbow`
load. Windows has never been built. The look has been seen on the synthetic test card and,
through the harness's `--pipe`, on six of Resolume's bundled demo clips, which is where the
defaults came from. The superfine K of 8 is recalled from the Recommendation, not read
from its text. A superfine halftone page every frame is not real-time. The About block and
`ATTRIBUTIONS.md` are provisional.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/fax
cd fax
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic clock:

```bash
./build/fxtest --out /tmp/frame.png --size 1920x1080   # the test card
./build/fxtest --list                                  # every control, kind and default
./build/fxtest --tables                                # the codes are T.4's (no GL)
python3 tools/check_tables.py                          # ... against two other transcriptions
./build/fxtest --roundtrip --scan --print              # lossless; the scan; the print
./build/fxtest --streak --wedge --conceal              # where an error goes, and what hides it
./build/fxtest --timing --resize                       # the page arrives at the line's rate
./build/fxtest --negative                              # and the checks can fail
./build/fxtest --bench                                 # 720p through 4K, and the CPU half
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about, and
with `FXTEST_RENDERER=software` for CI's renderer. Footage goes through the real plugin
with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/fxtest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
```

OpenFX port and browser demo: not in 0.1.0.

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and [`AGENTS.md`](AGENTS.md)
for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
