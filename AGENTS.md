# AGENTS.md — Fax

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you tell
anybody this works.

---

## What the plugin is

The picture sent as a Group 3 fax (ITU-T T.4) over a noisy telephone line, as an FFGL 2.1
effect (`FX01`, shown as `SW Fax`) for Resolume Arena and Avenue. C++17 + GLSL 4.10,
CMake, universal macOS `.bundle` and (by CI, untested) a Windows `.dll`. MIT; intended
home `github.com/stoatworks-labs/fax`, which does not exist yet.

Built 2026-09-25 in one session from `specs/SPEC-fax.md` and the fleet's templates:
teletext for the whole read-back-then-CPU shape, the harness, `--pipe` and the negative
controls; slowscan for a page that arrives a line at a time; tinsel for `PassBuffer`,
the sweep and CI; graticule's font by way of teletext; repousse and wipe for the
software-renderer pass. Tranche five, Allan's own pick. A local v0.1.0: no GitHub, no
release, no website, never in Arena.

---

## The one idea

**A fax does not send pixels.** It thresholds each scan line to black and white and sends
**run lengths** as variable-length codes (MH), a line at a time, each ended by an EOL; in
MR most lines are sent as the **differences from the line above** (pass, horizontal and
vertical modes), with a one-dimensional line every K. So the clip goes through a real T.4
coder, a real noisy line and a real decoder, and the look falls out of the code:

| the mechanism | what comes out |
| --- | --- |
| a bit error makes the decoder read the wrong code lengths | **a streak** from the error to the right edge of its line — and only that line, because the next line starts at the next EOL |
| an MR line is decoded against the line above | **a wedge**: the damage runs down until the next one-dimensional line, at most K lines (2 standard, 4 fine, 8 superfine) |
| the decoder can tell a line is bad (an invalid code, a line that is not 1728 pels, anything but fill after it) | **Repeat Line**: the bad line is the line above — and in MR every 2-D line after it until the next 1-D line — the familiar vertical stretch |
| 1728 pels across 215 mm, 3.85 / 7.7 / 15.4 lines per mm | a standard fax is **squashed and doubled** vertically: 466 lines for a 16:9 frame |
| each line takes its bit count over the line rate, floored at the minimum scan-line time by fill | in Page mode **the page arrives line by line**, fast over blank paper and slow over detail: a blank standard page in 4.66 s at 14400, a halftone photo in two and a half minutes |
| an EOL is eleven zeros and a one, which valid data never holds | an error that forges one splits its line, and one that breaks one merges two: the page below moves by a line, as on paper |

### The pipeline

1. **Scan** (GPU, `Shaders.cpp`, 54 x Lines RGBA8). For each pel of the page (`Layout`
   decides how the frame lands on it), the BT.709 luma of the source pixels whose centres
   fall in its rectangle, composited over white paper by the source's alpha; black below
   `Threshold`, or below it plus an 8 x 8 Bayer offset in Halftone. Written packed, eight
   pels a channel.
2. **Read-back**: 216 bytes a line (100 KB for a standard 16:9 page).
3. **The sending machine** (CPU): the header line printed into the top of the page, then
   `codec::Encode` — MH, or MR with K from the resolution — with fill to the 10 ms
   minimum scan-line time, EOLs, tags and RTC.
4. **The line** (`Line.cpp`): every bit flips when a PCG hash of (page, line, bit) falls
   under the bit error rate; `Bursts` turns each event into a burst.
5. **The receiving machine** (`codec::Decode`): every EOL first, each line decoded from
   the stretch between two, bad lines flagged, concealed or shown as decoded.
6. **Arrival**: in Page mode, line i appears when its EOL has arrived, ( preamble +
   lines 0..i ) / baud after the page began, over the last page; in Live every frame is a
   whole new page.
7. **Print** (GPU): each output pixel's footprint over the page, the exact area black pels
   cover, paper, toner and desk mixed by area; alpha 1; Mix.

### The code tables are the standard's, and three things say so

`source/T4.h` was typed by hand from T.4's Tables 2, 3a, 3b and 4, as bit strings. Then:

- `tools/check_tables.py` compares all 204 codes bit for bit with **two independent
  transcriptions** on this machine (golang.org/x/image/ccitt's `table.go` and
  pdfminer.six's `ccitt.py`): 408 comparisons, 0 differ — on the first run.
- `fxtest --tables` compares every code's **length** with
  `tools/fixtures/t4-code-lengths.txt` (extracted from golang's table by
  `check_tables.py --emit-lengths`), checks each set is prefix-free (Kraft sums 255/256,
  the missing 1/256 being the `00000000` prefix where only EOL lives), that no two codes
  in a row can make eleven zeros (7 leading + 3 trailing), and round-trips every run
  0..2560 and on to 5000 of both colours, each costing exactly the fixture's bits.
- `tools/gocheck` decodes the plugin's own MH stream (T.4, EOLs, RTC) and an all-2-D T.6
  stream with **golang.org/x/image/ccitt**, a decoder that shares no code with this repo,
  and gets the plugin's page back exactly — and rejects a stream coded with VR1 written
  as VR2. That is the evidence that the 2-D mode logic is the standard's and not merely
  self-consistent.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/T4.h` | The Recommendation's code tables, hand-transcribed as bit strings, and its constants. |
| `source/Codec.{h,cpp}` | The page (packed bits), the coder (MH, MR), the decoder (EOL segmentation, bad-line detection, concealment), the `Perturb` hooks. Compiled code arrays and 13-bit lookup tables for speed. |
| `source/Line.{h,cpp}` | Bit errors and bursts by integer hash; the page timing. |
| `source/Layout.{h,cpp}` | Fit and Resolution: the page's lines, where the source lands on it, where the sheet lands on the output. |
| `source/Header.{h,cpp}` | The sending machine's header line. |
| `source/Controls.{h,cpp}` | Option names, the noise curve, paper colours, the 10 ms floor. |
| `source/Shaders.{h,cpp}` | The scan pass and the print pass. |
| `source/Fax.{h,cpp}` | The plugin: parameters, the clock, the page in flight, the two passes. |
| `source/Font.*`, `PassBuffer.*`, `Diag.*`, `StoatworksAbout*` | graticule's font; tinsel's buffer; the log; the About block (generated by `sync-about.py`). |
| `tools/fxtest/` | The offline harness: renders, measures, benchmarks, exports, pipes, dumps shaders. |
| `tools/check_tables.py` | T4.h against two independent transcriptions; writes the length fixture. |
| `tools/gocheck/` | The independent decoder (Go, development only). |
| `tools/sweep.py`, `tools/verify.sh` | No control is silently dead; all of it. |

---

## Traps

Roughly in the order they bit.

### A corrupted line can decode to itself

`--streak` first failed 39 of 47: a bit error in an all-black sky line (white 0, black
1728, black 0 — 31 bits) made the decoder fail, and "the colour it was in at the failure,
to the edge" is black, so the damaged line was identical to the clean one. That is the
physics — a black streak on a black line — so the check now asks that every error be
*felt* (its line damaged **or** flagged bad: 47 of 47), that nothing but its own line is
damaged, and that no pel before the decoder's position at the error changed. 39 of 47
are visibly damaged.

### Concealment hides damage in a solid area, and moves it

`--wedge` first required the damage to start on the error's line. With Repeat Line a bad
1-D line in the black sky is concealed with the black line above (no change), its 2-D
followers are concealed too — and the first of them that was *not* black in the clean
page is the only line that differs, three lines below the error. The claim is
containment, `damage ⊆ [line, next 1-D line)`, and that is what is checked; the height
histogram is reported beside it.

### A negative control that passes because the card is too simple

"Conceal with the line two above" passed `--conceal` on the card: its solid areas make a
line equal the one two above as often as the one above. `--conceal` now runs in Halftone,
where the Bayer pattern makes neighbouring lines differ, and the control fails (1 of 113
concealed lines still equal to the line above).

### A line you cannot see arrive

`--timing` watches lines replace the old page. On a black old page the busy card's black
sky lines were invisible. The old page is now one Live frame of dithered mid grey — a
Bayer pattern on every line — and the check asserts that no test line equals it.

### EOL forgeries

A 1 between two runs of zeros that total ten becomes eleven zeros and a one when flipped:
a forged EOL, which splits the line and moves the page below down. It is real fax
behaviour, but it is not the streak claim, so `--streak` and `--wedge` leave such flips out
and count them (1 of 48, 9 of 32 — the short 2-D lines of blank areas are mostly fill and
EOL).

### The CPU half was 20 ms a frame

A standard halftone page is 2.2 million bits. With a trie decoder, codes appended from
strings a character at a time, `std::upper_bound` per 2-D code and two `std::vector`
allocations per line, coding took 8.7 ms and decoding 10.8 ms. Now: precompiled bit arrays,
changing-element indices that only move forward, the stream packed eight bytes at a time
with one multiply, EOLs found with `clz`, codes read from 13-bit lookup tables. See the
bench below.

### Eleven thousand renders on the software renderer

`--timing` steps a 2 ms clock through four pages. On Apple's software renderer that took
minutes; the steps now draw into a one-pixel host viewport, because the timing is read
off the receiver's page and not the picture (the print is `--print`'s job).

### Resolume's clips are dark, and a threshold prints them black

Through `--pipe` on six bundled clips, Threshold 0.5 printed most of every frame solid;
0.3 was little better. The defaults are Halftone on, which renders the clips' tones, and
Live mode (see the decision below). Transparent regions of DXV clips scan as blank paper.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put
back before the print); every `ffglex::Scoped*` clears to 0 on exit, so the scan buffer is
ensured before anything binds; `FFGLFBO::Release()` leaks the colour texture
(`PassBuffer::Destroy()`); `SetTextParameter` must return `FF_SUCCESS` for the About block;
the core is an **OBJECT** library; `FFGLScopedFBOBinding.h` is not in the umbrella header;
an option's range reads back 0..1; Resolume's clock overflows a float, so the page's clock
is double seconds relative to its start; the page memory is on the CPU and a resize never
touches it (photofinish's trap, checked by `--resize`); `nm | grep -q` fails under
pipefail; reserved GLSL words (`packed`, `sample`, `input`, `output`, `filter`, `common`,
`active`, `half`, `layout`, `flat`, `patch`) — none used; no `M_PI`, no `far`/`near`.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every check ran at 320 x 180 and 1280 x 720, and at 320 x 180 on
Apple's software renderer (`FXTEST_RENDERER=software`), in `verify.sh`.

| check | what it measures | tolerance and where it comes from | raster / rasteriser dependence |
| --- | --- | --- | --- |
| `--tables` | code lengths, prefix-freedom, zero runs, every run's round trip | **exact**: strings and integers | none: no GL |
| `--roundtrip` | the received page against the sent page; a 28-page corpus | **exact bits** | the raster only chooses which page the card scans to; the corpus has no GL |
| `--scan` | every pel of the scanned page against the same scan in double | **exact** per pel, leaving out pels whose luma is within **1e-4** of the threshold (float32 sums of ≤ 64 terms err by ~4e-6; unorm8 → float by less) or whose rectangle edge or centre is within **1e-3 px** of a pixel-centre boundary (float32 positions err by ≤ 2.3e-4 px at 4K); at least 90% must be compared. Left out: 0.4% at 320 x 180, 3.8% at 1280 x 720 | holds at any raster; the margins only remove pels |
| `--print` | every output pixel against the page's pel coverage in double; alpha | **one 8-bit step** per channel (a rounding tie; the float32 coverage sums err by ~1e-5 of a pixel); alpha **exactly 255**. Worst measured: 1 step | none beyond the tie; identical on the software renderer |
| `--streak` | which page lines differ from the clean page; the first damaged pel; pixels outside the damaged rows | lines and pels **exact**; pixels **one 8-bit step**, because the software renderer is not repeatable at the last bit (repousse) | the page is raster-independent; "rows touching a damaged line" are computed in double from the page's placement |
| `--wedge` | the same, for K = 2, 4, 8, off and Repeat Line | as `--streak` | as `--streak` |
| `--conceal` | concealed lines against the line above | **exact bits** | none: the page |
| `--timing` | each line's arrival, bracketed by a 2 ms synthetic clock, against ( the stream through its EOL ) / baud from the harness's own EOL parse; bits per line against the floor | **one clock step, 2 ms** (a fifth of the 10 ms floor) + 1e-9 s of double arithmetic; bits **exact integers** | none: read off the receiver's page; the steps draw one pixel |
| `--resize` | pages, arrived lines and the page bytes against an unresized run | **exact** | resizes to 1.5W+1 x 0.75H+1, another aspect |
| sweep | any subpixel differs | ≥ 1 | 320 x 180 and 480 x 270 |

Deliberately NOT relied on: bit-identical renders between two instances (only one step is
allowed between a clean and a damaged render); `pow` anywhere; exact float cancellation.
What might still differ on another GPU: the scan's float luma on a pel within 1e-4 of the
threshold may land the other side, so a real clip's page may differ by a pel here and
there between GPUs — no check asserts a specific clip's bits, only structure and the
recomputation with its stated margins. The codec is integer and agrees bit for bit given
the same page.

### The negative controls

`fxtest --negative` runs nine, and `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin's* model through a `codec::Perturb` bit the shipped plugin
carries at zero, never the harness's expectation. Plus one in `verify.sh` for the Go
decoder.

| perturbation | what fails, at 320 x 180 |
| --- | --- |
| white 5 and white 8 exchanged in the table | `--tables`: the lengths differ from the fixture (round trip alone would pass: a swapped table is still consistent) |
| VR1 written as VR2 | `--roundtrip`: corpus pages decode wrong; and `gocheck` rejects the exported G4 stream |
| BT.601 luma weights in the scan shader | `--scan`: pels differ from the BT.709 recomputation |
| the nearest pel instead of the coverage in the print shader | `--print`: pixels off by up to half the ink-to-paper range |
| a decoder that counts pels and never resynchronises on EOL | `--streak`: errors damage every line after them |
| MR with no one-dimensional refresh | `--wedge`: damage runs past the next 1-D line |
| concealment with the line two above | `--conceal`: concealed lines unlike the line above |
| no fill (no minimum scan-line time) | `--timing`: lines under the floor |
| every line the page's mean duration | `--timing`: arrivals outside their brackets |

### The mutation

One character of the shipped GLSL, on a clean committed tree (1210cd6): in the print
pass, `row += min( cu1, float( i + 1 ) )` became `float( i + 2 )` — each black pel's
coverage over-counted by up to one pel. Caught by **`--print`** at both rasters (23,692
of 57,600 pixels off by more than a step at 320 x 180, worst 72; 371,067 of 921,600 at
1280 x 720). Correctly not caught by `--roundtrip`, `--scan`, `--conceal`, `--timing` and
`--resize` (none reads a pixel) or by `--streak` and `--wedge` (which compare the mutated
print with itself). Reverted with `git checkout source/Shaders.cpp` and a `touch`; the
rebuilt `--print` passes.

---

## Decisions taken without asking

- **The coder and decoder run on the CPU**, from a read-back, as spec'd. MH and MR are
  serial run-length codes whose claims (the tables, losslessness, where an error goes) are
  integer claims; a GPU form was optional and not attempted.
- **`Fit`** is Frame (the page is the frame, 1728 pels across its width — the default),
  A4 Crop and A4 Letterbox (an A4 sheet, 1143 / 2287 / 4574 lines, shown whole on a black
  desk). The spec's "letterbox or crop" is read as how the frame lands on an A4 sheet.
- **The page's shape is fixed at its start.** A resize or an aspect change mid-page
  changes where the sheet is drawn, never the page; the next page takes the new aspect.
- **Mode Live is the default.** Page mode holds a frame for the whole page — 4.7 s blank,
  about 6 s for a thresholded frame, two and a half minutes for a halftone one at 14400 —
  which is the transmission look but reads as a frozen clip. Page is one click away;
  Halftone off makes it quick.
- **Halftone on by default**, Threshold 0.5: Resolume's bundled clips are dark (above).
- **Line Noise** is 0 (clean) or 10^(-6 + 4v): 1e-6 to 1e-2. The default 0.4 is 4e-5,
  about ninety errors on a standard halftone page and three on a thresholded one. Stated,
  not measured from any modem.
- **Bursts**: each error event flips its first bit and each of the next 1 + round(63 v)
  − 1 bits with probability one half. The event rate stays the bit error rate.
- **The minimum scan-line time is 10 ms**, one of T.30's values, as a constant.
- **Unarrived lines show the previous page** (blank paper for the first page and after a
  shape change), so a new page overwrites the last one from the top — continuous for a
  show, where a real receiver would feed new paper.
- **Concealment Off shows what was decoded**: the pels up to the failure and the colour at
  the failure to the right edge. **Repeat Line** repeats the line above and, in MR, every
  2-D line after it until a 1-D line, as the reference is spoilt.
- **Lost or split lines move the page**, as on paper; the page is cut or padded with blank
  paper at the bottom. Only noise causes it; no check depends on it.
- **The header** is printed by the *sender* into the page before coding, so it breaks on
  the line with the rest: time (composition time at the page's start, not the wall clock),
  `FROM: SW FAX`, `G3 T.4`, the page number. No real sender or number.
- **Output alpha is 1** wherever the effect is fully in: paper is opaque. Mix blends the
  whole RGBA with the source. A transparent source scans as blank paper (luma composited
  over white by straight alpha — whether Resolume's textures are premultiplied was not
  established; it only matters at partly transparent edges).
- **Thermal paper** is a warm grey (0.86, 0.85, 0.80) with brown-black (0.20, 0.17, 0.16);
  plain paper (0.96, 0.96, 0.95) with toner (0.06, 0.06, 0.07). Chosen by eye.
- **Superfine K = 8** is read from the text of T.4 (07/2003), fetched from itu.int at
  release (2026-09-25): 4.2.1.1 sets the maximum K by vertical resolution — standard 2,
  200 lines/25.4 mm 4, 400 lines/25.4 mm 8 — and the note to Table 2 (2.x) treats 7.7 and
  15.4 lines/mm as equivalent to 200 and 400. So 4 at fine and 8 at superfine are the
  standard's by that equivalence; the build session had recalled 8 without the text.
  `--wedge` measures whatever `layout::K` returns.
- **No OpenFX port and no presets** (not required for 0.1.0). `StoatworksAbout.h` and
  `ATTRIBUTIONS.md` are generated by the backend's `sync-about.py` and
  `sync-attributions.py` since registration.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

Every number is `tools/verify.sh` on this machine against a fresh universal Release build.

- **Tables.** 204 codes agree bit for bit with two independent transcriptions; 217 code
  lengths (the extended make-ups counted for both colours) equal the fixture; prefix-free,
  Kraft sums 0.996094 / 0.996094 / 0.984375; 7 leading and 3 trailing zeros at most;
  5,820 runs round-trip at exactly the standard's bits.
- **Independent decoder.** golang.org/x/image/ccitt decodes the plugin's MH and T.6 streams
  of the card, thresholded and halftone, to the page with 0 of 100,656 bytes different, and
  rejects the VR1-as-VR2 stream.
- **Round trip.** 28 corpus pages (all nine 2-D modes used) and eight plugin pages (MH/MR
  x standard/fine x threshold/halftone, header on; 76,281 to 4,448,921 bits) lossless.
- **Scan.** 0 wrong pels of 0.8–3.9 million compared per case, six cases (all three fits,
  all three resolutions, thresholds 0.3/0.5/0.7, halftone, a transparent third).
- **Print.** 0 pixels off by more than one step in four cases at each raster, worst 1;
  every pixel alpha 255 over a source with a transparent third.
- **Streak.** 47 errors (1 left out as an EOL forgery): 47 contained in their own line, 47
  with no pel damaged before the error, 47 felt, 39 visibly damaged; 0 pixels moved outside
  the damaged rows.
- **Wedge.** K = 2, 4, 8, off and Repeat Line: 23 errors each, all contained before the
  next 1-D line; with concealment off the tallest is exactly K (2, 4, 8) and an error in a 1-D line
  ran exactly K lines in 12 of 16 (K = 2) and 10 of 16 (K = 4 and 8), the rest shorter or
  invisible; 0 pixels outside.
- **Conceal.** MH 113 (112 at 1280 x 720) and MR 161 concealed lines, every one equal to
  the line above; with Off every bad line unlike the line above.
- **Timing.** Blank and busy pages, MH at 14400 and MR at 9600: all 466 arrivals inside
  their 2 ms brackets; every blank line exactly the floor (144 / 96 bits); no line under
  it; 49 / 58 busy lines above it; blank 4.662 s against busy 5.604 / 5.586 s (320 x 180;
  6.772 / 6.818 s at 1280 x 720, where the card scans busier).
- **Resize.** 120 frames with a resize to another aspect at frame 40: the page identical
  to an unresized run on every frame.
- **Negative controls.** All nine fail their check, at both rasters and on the software
  renderer.
- **Mutation.** Caught by `--print` at both rasters.
- **Software renderer.** Every check passes at 320 x 180.
- **No dead controls**, all 13, at 320 x 180 and 480 x 270.
- **Every shader compiles** through `glslc`, as the plugin assembles it.
- **`--pipe`**: 2.5 frames in, 2 out; unknown cue exit 2; a failed render exit 1 with one
  frame out; a closed stdout exit 1; Paper steps between cues.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.fax`, ad-hoc signs; `oxbow` reports `SW Fax` / `FX01` / `effect`
  and renders 120 frames through `plugMain`.
- **Render cost** (`fxtest --bench`, best of three runs of 60 frames, `glFinish` both
  sides, on a shared machine):

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

### Assumed, or not done

- **Never loaded into Resolume.** Everything above is offline against the real plugin
  class in a headless CGL context, plus an `oxbow` load. How the CPU half behaves inside a
  busy host, and what the host's clock does over hours, is untested.
- **Windows has never been built.** The CI workflow is written and cannot run yet.
- **Footage has only been seen through `--pipe`**, on six of Resolume's bundled clips at
  1280 x 720; the defaults were chosen there.
- The error model and the paper colours are stated, not measured.
- **Superfine halftone in Live mode is not real-time** (see the bench): ~31 ms of CPU a
  frame.

---

## Open questions

- **Should the CPU half run on a worker thread**, one frame behind, so a superfine halftone
  page does not stall the render thread?
- **Page mode's default look**: should a new page feed new paper (blank below the print
  line), as a real receiver does, rather than overwrite the last page?
- **Should Resolution change K**, or should K be its own control (a real terminal
  negotiates it)?
- **Error Correction Mode (T.30 Annex A)**: real G3 machines since the 1990s retransmit
  bad frames and never show a streak. Out of scope, but it is why modern faxes look clean.
- **Premultiplied alpha**: does Resolume hand an effect premultiplied textures? It moves the
  partly transparent edges of a clip by a pel or two.
- **A contrast control** like a real machine's Light / Normal / Dark, instead of the raw
  threshold.

---

## Siblings

- **teletext** — the plugin shape (GPU pass, read-back, CPU codec, upload, render), the
  harness, `--pipe`, the negative-control pattern.
- **slowscan** — a picture sent a line at a time over a channel.
- **slope** — 1-bit, scan order.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, and the fleet's trap list.
- **repousse**, **wipe** — the software-renderer switch and why it exists.
- **graticule** — the 5x7 font.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
