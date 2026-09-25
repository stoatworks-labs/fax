"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.
Galvo's check, by way of teletext, in this repo's shape. The copy is made by
`demo/tools/splice_shaders.py`; this is what guards it.

------------------------------------------------------------------- why

`demo/plugin.js` holds three GLSL bodies and so does `source/Shaders.cpp`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `fxtest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists, and `tools/verify.sh`'s glslc step
compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. The plugin assembles each shader as `kVersion + body`
(`shaders::assemble`); the page does the same with `assemble()`, so the bodies
are what is compared and the one prepended line is checked here as a literal.

The one transformation is a decode, not a normalisation. A backtick cannot
appear raw inside a JavaScript template literal, so `plugin.js` would have to
escape one as \\`; none of these shaders quotes one today, but a comment could
start to. This unescapes that and *rejects any other backslash on the JS side*;
there are none anywhere in the C++, so a second escape could only be somebody
hiding a difference. A `${` would be interpolated by the literal, so it is
refused on the C++ side before it can become a silent difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `demo/fax.js` is a hand translation of
T4.h, Codec.cpp, Line.cpp, Header.cpp, Font.cpp, Layout.cpp and Controls.cpp,
and the frame sequence in plugin.js of Fax::ProcessOpenGL and Fax::startPage.
`demo/tools/check_port.sh` compares fax.js with the plugin's C++ source on a
set of test pages (it says exactly what it covers); the frame sequence only a
reader checks. When you change one of those, change it here too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX_BODY", "source/Shaders.cpp", "kVertexBody"),
    ("SCAN_BODY", "source/Shaders.cpp", "kScanBody"),
    ("PRINT_BODY", "source/Shaders.cpp", "kPrintBody"),
]

# The line the plugin prepends to every body, and the page must too.
VERSION_CPP = 'const char* const kVersion = "#version 410 core\\n";'
VERSION_JS = "const VERSION = '#version 410 core\\n';"


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp_all = handle.read()

    problems = 0
    if VERSION_CPP not in cpp_all or VERSION_JS not in js:
        print("FAIL  the #version line the plugin prepends is not the one the page prepends")
        problems += 1
    else:
        print("ok    VERSION              matches kVersion")

    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if "${" in cpp_text:
            print(f"FAIL  {symbol} contains ${{, which a template literal would interpolate")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
