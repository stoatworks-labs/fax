# fax

The picture sent as a Group 3 fax (ITU-T T.4) over a noisy line, as an FFGL **effect**
for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT. v0.1.0 released 2026-09-25 (github.com/stoatworks-labs/fax); never loaded into
Resolume on macOS; Arena on win-lab 9/9 (software rendering).

Read `AGENTS.md` before changing the tables, the coder, the decoder, the line or the page
timing.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/fxtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Mode=0" --set "Halftone=0" --set "Line Noise=0.6"`
  (0..1 for sliders and booleans, the element index for options: Fit 0 Frame, 1 A4 Crop,
  2 A4 Letterbox; Resolution 0 Standard, 1 Fine, 2 Superfine; Coding 0 MH, 1 MR; Baud
  0 2400 … 3 14400; Mode 0 Page, 1 Live; Concealment 0 Off, 1 Repeat Line; Paper 0
  Thermal, 1 Plain)
- List parameters, kinds, defaults and ranges: `./build/fxtest --list`
- Other sources: `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/fxtest --dump-shaders DIR`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` and an optional `--script` of `frame Parameter Name
  value` cues; sliders ramp between cues, options, booleans, integers and events STEP; a
  cue naming no parameter is refused with exit 2, a partial frame at the end of stdin ends
  the stream cleanly, a failed render (`--fail-render-at N`) or a closed stdout exits 1
  (SIGPIPE is ignored, so `| head -c 1` gives 1, not 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/fxtest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build, glslc, the table cross-check,
  every check at 320x180 AND 1280x720 AND on the software renderer, the Go decoder,
  `--pipe`, two sweeps, the bench, the bundle, oxbow; ~6 min on a shared machine)
- The codes are T.4's (no GL): `./build/fxtest --tables`; against two independent
  transcriptions: `python3 tools/check_tables.py`
- Names ≤ 16 characters and unique: `./build/fxtest --names`
- Lossless, MH and MR: `./build/fxtest --roundtrip`
- The scan and the print, recomputed in double: `./build/fxtest --scan --print`
- One MH error stays in its line; one MR error stays above the next 1-D line:
  `./build/fxtest --streak --wedge`
- Repeat Line shows the line above: `./build/fxtest --conceal`
- Bits / baud, floored at 10 ms; blank beats busy: `./build/fxtest --timing`
- A resize mid-page changes nothing: `./build/fxtest --resize`
- The checks can fail: `./build/fxtest --negative`; one perturbation verbosely:
  `./build/fxtest --perturb BITS --streak` (bits in `Codec.h`)
- Every check takes `--size WxH`; CI runs them at 320x180. `FXTEST_RENDERER=software`
  runs on Apple's software renderer (CI's), which is not repeatable at the last bit.
- An independent decoder: `./build/fxtest --export DIR && (cd tools/gocheck &&
  GOFLAGS=-mod=mod GOPROXY=off GOSUMDB=off go run . DIR)`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the CPU half's share: `./build/fxtest --bench`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Fax.bundle`

## Notes
- **The coder, the line and the decoder run on the CPU**, from a read-back of the scan
  pass: 216 bytes a line, packed eight pels a byte, pel 8i + b in bit b. The same layout
  is uploaded for the print pass, so nothing is repacked.
- **The decoder finds every EOL first** and decodes each line from the stretch between
  two. That is the resynchronisation: a bit error runs to the end of its line and no
  further (MH), or down to the next one-dimensional line (MR).
- **The tables are typed by hand from T.4** (`source/T4.h`) and checked, never generated.
- **Defaults are Live + Halftone**: Resolume's demo clips are dark, and a plain threshold
  prints them solid. A halftone page is ~2.2 million bits — two and a half minutes at
  14400 in Page mode — so Page mode wants Halftone off.
- **Paper is opaque**: alpha 1 wherever the effect is fully in; a transparent source
  scans as blank paper.
- **Nothing absolute crosses into GLSL.** The page's clock is double seconds relative to
  the page's start.
- **The page memory is on the CPU** and a resize never reallocates it; `--resize` checks.
- **`Perturb` bits are test hooks**, always 0 in the plugin.
- Options are mapped by index in `Controls.cpp`; an option's range reads back 0..1.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `fax_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include it by hand.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `FX01`, display name `SW Fax`.

## Browser demo
- `demo/` is served at https://fax-demo.stoatworks-labs.com/ by this repo's Worker, through
  a proxied AAAA `100::` DNS record + a route (the zone is out of custom domains; deleting
  the record takes the page dark with green deploys). A push to main deploys it
  (`deploy.yml`); by hand: `cf-run npx wrangler deploy`. Verify by content:
  `curl -s 'https://fax-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`
- The shaders in `demo/plugin.js` are spliced from `source/Shaders.cpp` by
  `python3 demo/tools/splice_shaders.py` and checked by `demo/tools/check_shaders.py`.
- `demo/fax.js` is a hand PORT of the CPU half; `demo/tools/check_port.sh build/fxtest`
  compares it with the C++ source on 12 pages. The frame sequence in `plugin.js` only a
  reader checks. Change a codec, line, header, layout or controls file: change fax.js too.
- `demo/vendor/` is the shared kit: never edit it; re-run
  `stoatworks-backend/resolume-demo/sync.sh fax`.

## Not done yet
- **Never loaded into Resolume on macOS.** Measured offline, plus an `oxbow` load. On
  Windows, Arena 7.27.1 on llvmpipe: gate 9/9, six controls inconclusive (Live mode's
  per-frame noise), `plugin-bench/arena/expect/fax.json`.
- No OpenFX port, no presets. The user guide is `docs/USER-GUIDE.md` (the site page and
  both PDFs are generated from it by the website's `build_guides.py fax`).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/fax/fax.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\fax\logs\fax.YYYY-MM-DD.log   (Windows)
