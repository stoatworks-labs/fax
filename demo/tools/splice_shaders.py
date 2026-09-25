"""Copy the plugin's shader bodies into demo/plugin.js, unedited.

    python3 demo/tools/splice_shaders.py

Replaces the `//@@SHADERS@@` marker (first run) or the three existing
`const X_BODY = `...`;` literals with kVertexBody, kScanBody and kPrintBody from
source/Shaders.cpp, exactly as they are there. Then run check_shaders.py, which
is what verify.sh runs and what actually guards the copy. A backtick or `${` in
a shader would change inside a template literal, so either one stops the splice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

SHADERS = [
    ("VERTEX_BODY", "kVertexBody"),
    ("SCAN_BODY", "kScanBody"),
    ("PRINT_BODY", "kPrintBody"),
]


def main():
    cpp = open(os.path.join(REPO, "source", "Shaders.cpp")).read()
    path = os.path.join(REPO, "demo", "plugin.js")
    js = open(path).read()

    blocks = []
    for name, symbol in SHADERS:
        match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', cpp, re.S)
        if match is None:
            print(f"{symbol} not found in source/Shaders.cpp")
            return 1
        body = match.group(1)
        if "`" in body or "${" in body or "\\" in body:
            print(f"{symbol} holds a backtick, ${{ or a backslash; splice it by hand and teach check_shaders.py")
            return 1
        blocks.append((name, f"const {name} = `{body}`;"))

    if "//@@SHADERS@@" in js:
        js = js.replace("//@@SHADERS@@", "\n\n".join(block for _, block in blocks))
    else:
        for name, block in blocks:
            js, n = re.subn(r"^const " + name + r" = `.*?`;$", lambda _m: block, js, flags=re.S | re.M)
            if n != 1:
                print(f"{name} not found once in demo/plugin.js")
                return 1

    open(path, "w").write(js)
    print(f"spliced {len(blocks)} shader bodies into demo/plugin.js")
    return 0


if __name__ == "__main__":
    sys.exit(main())
