#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word,
and a control whose value never reaches the coder is dead just as quietly:
everything compiles, links, loads and renders. Nothing else in this repo
catches that.

This renders each parameter at both ends of its range against the same
moving test card and reports any that made no difference at all.

    python3 tools/sweep.py [--binary build/fxtest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**The page arrives at the line's rate -- in Page mode.** The default is Live
(a whole page every frame), where Baud does nothing: it is swept in Page mode
with the halftone off, where forty host frames are two thirds of a second, about
sixty of the 466 lines of a standard page at 14400 and far fewer at 2400.

**The line's noise is only visible where it lands.** Line Noise, Bursts and
Concealment are swept in Live mode (the whole page every frame) with the noise
up, so there is a page full of errors for them to change.

**An option's range is its element count.** `fxtest --list` prints it, and
the ends are what get swept.

**Every name must be unique.** `--set` finds a parameter by name and takes
the first match. `fxtest --names` checks it.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 320, 180
FRAMES = 40

# What else has to be true for a parameter to have any effect at all. Keys
# beginning with "_" are HARNESS settings, not plugin parameters:
#   _frames     how many frames to render (default 40)
#   _low/_high  the two positions to compare, when not the range's ends
LIVE_NOISY = {"Mode": 1, "Line Noise": 0.6}
# Page mode, thresholded: a halftone page is ~2 million bits, two minutes at
# 14400, and forty frames would see two lines of it.
PAGE = {"Mode": 0, "Halftone": 0}
CONTEXT = {
    "Line Noise": {"Mode": 1},
    "Bursts": LIVE_NOISY,
    "Concealment": LIVE_NOISY,
    "Baud": PAGE,
}


def parameters(binary):
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def render(binary, path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [binary, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    binary, scratch, pid, name, low, high, context = job

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(binary, f"{scratch}/{pid}_lo.png", lo)
    b = render(binary, f"{scratch}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run cut off by a CI timeout
    # still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "fxtest"))
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    binary = str(pathlib.Path(args.binary).resolve())
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 1

    scratch = tempfile.mkdtemp(prefix="fxsweep")

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters(binary):
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("buffer", "text"):
            skipped.append((name, "no scalar to sweep"))
            continue
        work.append((binary, scratch, pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1
    print(f"all {len(results)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
