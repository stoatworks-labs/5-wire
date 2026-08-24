#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each check answers a question none of the others can:
#
#   --kernel      is the cable's response the response a cable has: unit gain,
#                 strictly one-sided, never a boost, and a bandwidth that goes
#                 as the square root of frequency
#   --eq          is the equaliser really the inverse of it -- and, the check
#                 that matters most, does it never make the picture SOFTER
#   --impulse     end to end. Does the GPU convolve with the kernel Cable.cpp
#                 computed, or with whatever was in the uniform before
#   --ghost       do the repeats land at twice the transit time, and does a
#                 back-matched amplifier really kill them
#   --presets     do the factory presets survive a host that ignores value
#                 events, which is the host this plugin will run in
#   sweep.py      does every control actually reach the picture
#   registration  does the bundle contain its own plugin and nobody else's
#   lipo          is the macOS build really universal
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() {
	printf '\n\033[1m== %s\033[0m\n' "$1"
}

check() {
	if "$@"; then
		return 0
	fi
	printf '\033[31mFAILED: %s\033[0m\n' "$*"
	failures=$((failures + 1))
}

if [ ! -x "$BUILD/fwtest" ]; then
	echo "$BUILD/fwtest not found."
	echo "Configure with -DFIVEWIRE_BUILD_TOOLS=ON and build first:"
	echo "  cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

step "the cable's response"
check "$BUILD/fwtest" --kernel

step "the equaliser against the cable it inverts"
check "$BUILD/fwtest" --eq

step "the GPU convolves with the kernel Cable.cpp computed"
check "$BUILD/fwtest" --impulse

step "where the repeats land"
check "$BUILD/fwtest" --ghost

step "factory presets against three host behaviours"
check "$BUILD/fwtest" --presets

step "no dead controls"
check python3 tools/sweep.py

# ---------------------------------------------------------------------------
# Registration.
#
# The failure this catches is specific and silent: `CFFGLPluginInfo` registers
# itself from a file-scope constructor and nothing references it by name, so a
# linker that drops the translation unit gives a bundle which loads, exports
# plugMain, and reports that it contains no plugins. Resolume shows an empty
# effects list and no error at all.
#
# The id is checked as well as the symbol, because a plugin repo started by
# copying another one keeps the DONOR's four-character id until somebody
# changes it -- and two plugins with the same id in one Resolume install is a
# fight nobody wins.
# ---------------------------------------------------------------------------
step "the bundle contains its own plugin"
binary="$BUILD/5-wire.bundle/Contents/MacOS/5-wire"
if [ ! -f "$binary" ]; then
	printf '\033[31mFAILED: %s not built\033[0m\n' "$binary"
	failures=$((failures + 1))
else
	# Read once into variables rather than piping into `grep -q`.
	#
	# `grep -q` exits the instant it matches, which closes the pipe under the
	# still-running `nm` or `strings`; they take SIGPIPE and exit 141, and with
	# `set -o pipefail` the *pipeline* is then a failure however well the grep
	# went. It is only intermittent from the shell -- a short output fits the
	# pipe buffer and the writer finishes before the reader leaves.
	symbols=$(nm -gU "$binary" 2>/dev/null)
	literals=$(strings "$binary" 2>/dev/null)

	if ! grep -q plugMain <<<"$symbols"; then
		printf '\033[31mFAILED: the bundle exports no plugMain\033[0m\n'
		failures=$((failures + 1))
	elif ! grep -qx "5W01" <<<"$literals"; then
		printf '\033[31mFAILED: the bundle does not carry its own id 5W01\033[0m\n'
		failures=$((failures + 1))
	else
		printf 'ok   exports plugMain and carries 5W01\n'
	fi
fi

# ---------------------------------------------------------------------------
# The bundle's plist.
#
# CFBundleExecutable naming a file that is not there is what killed a release
# in flipbook: the bundle assembles, the binary is universal, everything loads,
# and only codesign notices -- with a message about a "subcomponent" that never
# mentions the plist. This one is parameterised through CMake rather than
# hardcoded, so the check is cheap insurance rather than a live worry, and it
# runs the release step on a COPY so a verify run never leaves a signature on
# the build tree that the release job did not put there.
# ---------------------------------------------------------------------------
if [ -d "$BUILD/5-wire.bundle" ]; then
	step "the bundle signs"

	plist="$BUILD/5-wire.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [ ! -f "$BUILD/5-wire.bundle/Contents/MacOS/$named" ]; then
		printf '\033[31mFAILED: Info.plist names "%s", which is not in Contents/MacOS\033[0m\n' "$named"
		failures=$((failures + 1))
	else
		scratch="${TMPDIR:-/tmp}/5-wire-signcheck.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/5-wire.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			printf 'ok   CFBundleExecutable is %s, and the bundle ad-hoc signs\n' "$named"
		else
			printf '\033[31mFAILED: the bundle will not codesign\033[0m\n'
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures=$((failures + 1))
		fi
		rm -rf "$scratch"
	fi
fi

# ---------------------------------------------------------------------------
# Universal.
#
# CMake latches CMAKE_OSX_ARCHITECTURES when the first target is created, so
# setting it late is silently ignored and the build log still says success. The
# only honest answer comes from lipo. Skipped when the developer asked for a
# single-architecture build on purpose.
# ---------------------------------------------------------------------------
step "the macOS build is universal"
if grep -q "CMAKE_OSX_ARCHITECTURES:.*arm64;x86_64" "$BUILD/CMakeCache.txt" 2>/dev/null; then
	arches=$(lipo -archs "$binary" 2>/dev/null)
	case "$arches" in
		*arm64*x86_64* | *x86_64*arm64*)
			printf 'ok   5-wire: %s\n' "$arches" ;;
		*)
			printf '\033[31mFAILED: 5-wire is %s, not universal\033[0m\n' "${arches:-missing}"
			failures=$((failures + 1)) ;;
	esac
else
	echo "skipped: this build was configured for one architecture"
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit "$failures"
