#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The text compiled here is
#                 what `fxtest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver.
#   tables        source/T4.h against two independent transcriptions of T.4
#                 (tools/check_tables.py; skipped where neither is installed),
#                 then `fxtest --tables` against the code-length fixture:
#                 lengths, prefix-free, no EOL in valid data, every run.
#   names         every parameter name 16 characters or fewer, and unique.
#   checks        every picture check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one
#                 raster was fitted to it:
#                   --roundtrip  decode( encode( page ) ) == page, MH and MR
#                   --scan       the scanned page is the source thresholded
#                   --print      every pixel is the page's pel coverage
#                   --streak     an MH error damages its line and nothing else
#                   --wedge      an MR error runs down only to the next 1-D line
#                   --conceal    Repeat Line shows the line above
#                   --timing     a line takes its bits / baud, floored at 10 ms
#                   --resize     a resize mid-page changes nothing on the page
#                   --negative   every one of those FAILS on a perturbed model
#   software      the same checks at 320x180 on Apple's SOFTWARE renderer,
#                 which is what GitHub's macOS runners have and which is not
#                 repeatable at the last bit (FXTEST_RENDERER=software).
#   gocheck       golang.org/x/image/ccitt -- a decoder that shares no code
#                 with this repo -- decodes the plugin's G3 and G4 streams to
#                 the page exactly, and rejects a stream coded with a wrong
#                 vertical mode. Skipped without Go or the module in the cache.
#   pipe          the fleet's --pipe contract: whole frames only, a cue naming
#                 no parameter refused, a failed render is exit 1, a closed
#                 stdout is exit 1 (SIGPIPE ignored, never 141), and options
#                 STEP between cues rather than ramping through their values
#   sweep         does every control change the picture
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

FXTEST="$BUILD/fxtest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code. glslc is optional -- `brew
# install shaderc` -- so a machine without it skips rather than fails.
#---------------------------------------------------------------------------
step "shaders"
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	dir="$( mktemp -d )"
	"$FXTEST" --dump-shaders "$dir" >/dev/null
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	rm -rf "$dir"
	if [ "$n" -ne 3 ]; then
		fail "$n shaders were dumped, expected 3 -- the dump has gone stale"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi

step "tables (no GL)"
if out=$(python3 tools/check_tables.py 2>&1); then
	pass "check_tables.py: $( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/check_tables.py: T4.h differs from an independent transcription"
	printf '%s\n' "$out" | sed 's/^/      /'
fi
for check in tables names; do
	if out=$("$FXTEST" --$check 2>&1); then
		pass "fxtest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "fxtest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

CHECKS="roundtrip scan print streak wedge conceal timing resize negative"
for size in 320x180 1280x720; do
	step "checks at $size"
	for check in $CHECKS; do
		if out=$("$FXTEST" --$check --size $size 2>&1); then
			pass "fxtest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "fxtest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# The same checks on Apple's software renderer, which is what CI's macOS
# runner falls back to and which is not repeatable at the last bit. This is
# where a check that asserts exactness on this Mac's GPU is found before CI
# finds it.
#---------------------------------------------------------------------------
step "software renderer at 320x180"
for check in $CHECKS; do
	if out=$(FXTEST_RENDERER=software "$FXTEST" --$check --size 320x180 2>&1); then
		pass "fxtest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "fxtest --$check on the software renderer -- run: FXTEST_RENDERER=software $FXTEST --$check --size 320x180"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

#---------------------------------------------------------------------------
# An independent decoder. GOPROXY=off: the module must already be in the
# cache; this never fetches anything.
#---------------------------------------------------------------------------
step "gocheck (golang.org/x/image/ccitt)"
if ! command -v go >/dev/null 2>&1; then
	printf '   skipped: go not installed\n'
else
	dir="$( mktemp -d )"
	export GOFLAGS=-mod=mod GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
	if ! ( cd tools/gocheck && go build -o "$dir/gocheck" . ) >/dev/null 2>&1; then
		printf '   skipped: golang.org/x/image is not in the module cache\n'
	else
		mkdir -p "$dir/good" "$dir/bad"
		"$FXTEST" --export "$dir/good" --size 1280x720 >/dev/null
		"$FXTEST" --export "$dir/bad" --size 1280x720 --perturb 32 >/dev/null
		if out=$("$dir/gocheck" "$dir/good" 2>&1); then
			pass "$( printf '%s\n' "$out" | tail -1 )"
		else
			fail "golang.org/x/image/ccitt does not decode the plugin's streams to its page"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
		if "$dir/gocheck" "$dir/bad" >/dev/null 2>&1; then
			fail "gocheck accepted a stream coded with VR1 written as VR2 -- it cannot fail"
		else
			pass "gocheck rejects a stream coded with VR1 written as VR2 (the negative)"
		fi
	fi
	rm -rf "$dir"
fi

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp ); outs=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

got=$( "$FXTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever fxtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$FXTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

got=$( "$FXTEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

# A reader that goes away after one byte: forty frames is far more than a
# pipe buffer holds, so the writes after head leaves must fail. Exit 1, said
# on stderr -- not the 141 of a process SIGPIPE killed before it could say
# anything.
"$FXTEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (head -c 1): exit 1, not 141"
else
	fail "a closed stdout gave exit $status, not 1"
fi

# Options step between cues. Paper goes Thermal (0) at frame 0 to Plain (1) at
# frame 10: stepped, frame 5 is still Thermal and equals frame 0; ramped, it
# would be 0.5, which rounds to Plain. A clean line and no header, so nothing
# else changes from frame to frame.
printf '0 Line Noise 0\n0 Header 0\n0 Mode 1\n0 Paper 0\n10 Paper 1\n' > "$cues"
head -c $(( frame * 12 )) /dev/zero > "$raw"
"$FXTEST" --pipe --size 64x36 --script "$cues" < "$raw" > "$outs" 2>/dev/null
f0=$( head -c $frame "$outs" | shasum | cut -c1-16 )
f5=$( tail -c +$(( frame * 5 + 1 )) "$outs" | head -c $frame | shasum | cut -c1-16 )
f9=$( tail -c +$(( frame * 9 + 1 )) "$outs" | head -c $frame | shasum | cut -c1-16 )
f10=$( tail -c +$(( frame * 10 + 1 )) "$outs" | head -c $frame | shasum | cut -c1-16 )
if [ "$f0" = "$f5" ] && [ "$f0" = "$f9" ] && [ "$f9" != "$f10" ]; then
	pass "an option steps between cues: Paper holds Thermal through frame 9, Plain at 10"
else
	fail "an option did not step between cues (frames 0/5/9/10: $f0 $f5 $f9 $f10)"
fi
rm -f "$raw" "$many" "$cues" "$outs"

step "sweep"
for size in 320x180 480x270; do
	if out=$(python3 tools/sweep.py --binary "$FXTEST" --size $size 2>/dev/null); then
		pass "$size: $( printf '%s\n' "$out" | tail -1 )"
	else
		fail "tools/sweep.py reports a dead control at $size"
		printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
	fi
done

step "bench (for the record)"
"$FXTEST" --bench --frames 60 2>&1 | sed -n '4,16p' | sed 's/^/   /'

BUNDLE="$BUILD/Fax.bundle"
BIN="$BUNDLE/Contents/MacOS/Fax"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.fax" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Fax.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Fax" "id:          FX01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
