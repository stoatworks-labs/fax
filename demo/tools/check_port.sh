#!/usr/bin/env bash
#
# The browser demo's CPU half (demo/fax.js) against the plugin's own source.
#
#   demo/tools/check_port.sh [FXTEST]
#
# Builds refchain.cpp against source/Codec.cpp, Line.cpp, Header.cpp, Font.cpp,
# Layout.cpp and Controls.cpp (unchanged), exports the test card as the plugin
# scans it (`fxtest --export`, 1280x720), and runs check_port.mjs, which says
# exactly what it compares and what it cannot. Called from tools/verify.sh;
# exits 3 (skip) without node or a C++ compiler.
#
set -uo pipefail
cd "$(dirname "$0")/../.."

FXTEST="${1:-build-universal/fxtest}"
command -v node >/dev/null 2>&1 || { echo "skipped: node not installed"; exit 3; }
command -v c++ >/dev/null 2>&1 || { echo "skipped: no C++ compiler"; exit 3; }
[ -x "$FXTEST" ] || { echo "no fxtest at $FXTEST"; exit 1; }

dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

if ! c++ -std=c++17 -O2 -o "$dir/refchain" demo/tools/refchain.cpp \
	source/Codec.cpp source/Line.cpp source/Header.cpp source/Font.cpp source/Layout.cpp source/Controls.cpp 2>"$dir/err"; then
	cat "$dir/err"
	echo "refchain did not build"
	exit 1
fi
mkdir -p "$dir/card"
"$FXTEST" --export "$dir/card" --size 1280x720 >/dev/null || { echo "fxtest --export failed"; exit 1; }
node demo/tools/check_port.mjs "$dir/refchain" "$dir/card"
