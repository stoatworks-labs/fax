#!/usr/bin/env python3
"""Cross-check source/T4.h against two independent transcriptions of T.4.

The tables in source/T4.h were typed by hand from ITU-T T.4. A transcription
checked only against itself proves it was typed once. This compares every
code, bit for bit, with two other people's transcriptions that happen to be
on this machine:

  golang.org/x/image/ccitt/table.go  (BSD-3-Clause; cites T.6 Tables 1-3,
                                      which are T.4's codes)
  pdfminer.six pdfminer/ccitt.py     (MIT)

Neither is vendored and neither is required: a machine without them skips
that source (exit 0 with a note), and a machine with neither skips the whole
check. `fxtest --tables` does not depend on this script; it compares the code
LENGTHS against tools/fixtures/t4-code-lengths.txt, which this script wrote
from golang's table with --emit-lengths.

    python3 tools/check_tables.py                 compare
    python3 tools/check_tables.py --emit-lengths  rewrite the fixture from golang

Exit 1 means a code differs.
"""
import glob
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HOME = pathlib.Path.home()


def ours():
    """{(table, value): bits} from source/T4.h."""
    text = (ROOT / "source" / "T4.h").read_text()
    tables = {}
    names = {
        "kWhiteTerminating": "white-term",
        "kBlackTerminating": "black-term",
        "kWhiteMakeup": "white-makeup",
        "kBlackMakeup": "black-makeup",
        "kExtendedMakeup": "ext-makeup",
        "kModes": "modes",
    }
    modes = {"kPass": 0, "kHorizontal": 1, "kV0": 2, "kVR1": 3, "kVR2": 4, "kVR3": 5,
             "kVL1": 6, "kVL2": 7, "kVL3": 8}
    for cname, tname in names.items():
        m = re.search(r"inline constexpr Code " + cname + r"\[[^\]]*\] = \{(.*?)\n\};", text, re.S)
        if not m:
            raise SystemExit(f"cannot find {cname} in T4.h")
        for value, bits in re.findall(r"\{\s*([A-Za-z0-9]+)\s*,\s*\"([01]+)\"\s*\}", m.group(1)):
            v = modes[value] if value in modes else int(value)
            tables[(tname, v)] = bits
    return tables


def golang():
    paths = sorted(glob.glob(str(HOME / "go/pkg/mod/golang.org/x/image@*/ccitt/table.go")))
    if not paths:
        return None, None
    path = paths[-1]
    text = pathlib.Path(path).read_text()
    out = {}

    def table(name):
        m = re.search(r"var " + name + r" = \[\.\.\.\]bitString\{(.*?)\n\}", text, re.S)
        return [(int(i), b) for i, b in re.findall(r"(\d+):\s*\{[^}]*\},\s*//\s*\"([01]+)\"", m.group(1))]

    for i, b in table("whiteEncodeTable2"):
        out[("white-term", i)] = b
    for i, b in table("blackEncodeTable2"):
        out[("black-term", i)] = b
    # Table 3 in golang runs 64..2560: 27 colour make-ups then the 13 shared ones.
    for colour in ("white", "black"):
        for i, b in table(colour + "EncodeTable3"):
            run = 64 * (i + 1)
            key = (colour + "-makeup", run) if run <= 1728 else ("ext-makeup", run)
            if key in out and out[key] != b:
                out[("ext-makeup-" + colour, run)] = b
            else:
                out[key] = b
    for i, b in table("modeEncodeTable"):
        if i < 9:
            out[("modes", i)] = b
    return path, out


def pdfminer():
    candidates = glob.glob(str(HOME / ".cache/**/site-packages/pdfminer/ccitt.py"), recursive=True)
    candidates += glob.glob("/opt/homebrew/lib/python3*/site-packages/pdfminer/ccitt.py")
    if not candidates:
        return None, None
    path = sorted(candidates)[-1]
    text = pathlib.Path(path).read_text()
    out = {}
    for colour in ("WHITE", "BLACK"):
        for v, b in re.findall(r"BitParser\.add\(" + colour + r",\s*(\d+),\s*\"([01]+)\"\)", text):
            v = int(v)
            c = colour.lower()
            if v < 64:
                out[(c + "-term", v)] = b
            elif v <= 1728:
                out[(c + "-makeup", v)] = b
            else:
                out[("ext-makeup", v)] = b
    names = {"p": 0, "h": 1, "0": 2, "+1": 3, "+2": 4, "+3": 5, "-1": 6, "-2": 7, "-3": 8}
    for v, b in re.findall(r"BitParser\.add\(MODE,\s*\"?([+-]?[0-9a-z]+)\"?,\s*\"([01]+)\"\)", text):
        if v in names:
            out[("modes", names[v])] = b
    return path, out


def compare(label, path, theirs, mine):
    if theirs is None:
        print(f"skip  {label}: not on this machine")
        return 0, 0
    bad = 0
    seen = 0
    for key, bits in sorted(mine.items()):
        if key not in theirs:
            continue
        seen += 1
        if theirs[key] != bits:
            bad += 1
            print(f"DIFF  {label}: {key[0]} {key[1]}: ours {bits}, theirs {theirs[key]}")
    missing = [k for k in mine if k not in theirs]
    print(f"{'ok  ' if bad == 0 else 'FAIL'}  {label}: {seen} codes compared bit for bit, {bad} differ"
          f"{'' if not missing else f', {len(missing)} not in their table'}  ({path})")
    return seen, bad


def main():
    mine = ours()
    if len(mine) != 64 + 64 + 27 + 27 + 13 + 9:
        print(f"FAIL  T4.h holds {len(mine)} codes, expected 204")
        return 1
    gpath, g = golang()
    ppath, p = pdfminer()

    if "--emit-lengths" in sys.argv:
        if g is None:
            print("golang.org/x/image is not on this machine")
            return 1
        lines = ["# T.4 code lengths, extracted by tools/check_tables.py --emit-lengths",
                 f"# from an INDEPENDENT transcription: {os.path.relpath(gpath, HOME)}",
                 "# table value length"]
        for key in sorted(g):
            if key[0].startswith("ext-makeup-"):
                continue
            lines.append(f"{key[0]} {key[1]} {len(g[key])}")
        out = ROOT / "tools" / "fixtures" / "t4-code-lengths.txt"
        out.write_text("\n".join(lines) + "\n")
        print(f"wrote {len(lines) - 3} lengths to {out.relative_to(ROOT)}")
        return 0

    total_seen = total_bad = 0
    for label, path, theirs in (("golang.org/x/image/ccitt", gpath, g), ("pdfminer.six", ppath, p)):
        s, b = compare(label, path, theirs, mine)
        total_seen += s
        total_bad += b
    if total_seen == 0:
        print("skipped: neither independent transcription is on this machine")
        return 0
    if total_bad:
        print(f"{total_bad} code(s) differ from an independent transcription")
        return 1
    print(f"all {len(mine)} codes of T4.h agree with every independent transcription present ({total_seen} comparisons)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
