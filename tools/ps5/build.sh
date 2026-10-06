#!/usr/bin/env bash
# Configures and builds Vita3K as a PS5 title, and reports honestly whether it worked.
#
#   tools/ps5/build.sh            configure if needed, then build
#   tools/ps5/build.sh --fresh    delete the build directory and start over
#
# Run it from WSL or Linux, never from Git Bash on Windows: the compiler driver is a Linux
# binary and the Windows shell rewrites its /flags into paths.
#
# The exit code is the build's own. Nothing downstream - an upload, a test - should run on
# a build that did not happen, and a script that always succeeds is how a stale binary ends
# up on the console looking like a fix that did not work.
set -o pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${PS5_BUILD_DIR:-$HOME/vita-ps5/build-vita3k-ps5}"
: "${PS5_VULKAN_ROOT:=$HOME/vita-ps5/PS5_Vulkan}"
: "${PS5_PAYLOAD_SDK:=$PS5_VULKAN_ROOT/.deps/native/ps5-payload-sdk}"
export PS5_PAYLOAD_SDK

if [ "$1" = "--fresh" ]; then
	rm -rf "$BUILD" "$BUILD.cfg.log" "$BUILD.build.log"
	shift
fi

# The toolchain is built once, out of tree, and the error you get without it is a linker
# failure thousands of lines in. Say so here instead
missing=
for required in \
	"$PS5_PAYLOAD_SDK" \
	"$PS5_VULKAN_ROOT/tools/radv-link.sh" \
	"$PS5_VULKAN_ROOT/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
	"$PS5_VULKAN_ROOT/build/runtime-shim/ps5-native-tool" \
	"$PS5_VULKAN_ROOT/runtime/libc.prx"
do
	[ -e "$required" ] || missing="$missing\n  $required"
done
if [ -n "$missing" ]; then
	printf 'The PS5 toolchain is incomplete:%b\n\n' "$missing"
	echo "Build it in $PS5_VULKAN_ROOT first:"
	echo "  tools/setup-native-dependencies.sh"
	echo "  tools/build-radv.sh release"
	echo "  tools/rebuild-libc.sh"
	exit 1
fi

# Boost bootstraps itself with /bin/sh, which cannot parse a script checked out with CRLF
# line endings - and a clone made by Git for Windows has them. The failure is far from its
# cause ("Bad for loop variable", then "Could not find b2"), so fix it rather than report it
crlf=0
while IFS= read -r script; do
	if grep -qU $'\r' "$script" 2>/dev/null; then
		sed -i 's/\r$//' "$script"
		crlf=$((crlf + 1))
	fi
done < <(find "$SRC/external/boost" -type f \( -name '*.sh' -o -name 'bootstrap*' -o -name '*.jam' \) 2>/dev/null)
[ "$crlf" -gt 0 ] && echo "converted $crlf Boost build scripts from CRLF to LF"

# The UI kit is fetched as a bare clone and then archived at a pinned revision. A clone that
# failed half way leaves the directory in place, and cmake does not check: the next run gets
# "fatal: not a tree object" instead of retrying. Drop an incomplete one so it is re-fetched
kit_git="$BUILD/external/PS5_VKHomebrewUI.git"
if [ -d "$kit_git" ]; then
	revision=$(sed -n 's/^set(PS5_UI_KIT_REVISION \([0-9a-f]*\))/\1/p' "$SRC/cmake/ps5-ui.cmake")
	if [ -n "$revision" ] && ! git -C "$kit_git" cat-file -e "$revision" 2>/dev/null; then
		echo "the UI kit clone is missing $revision; re-fetching"
		rm -rf "$kit_git"
	fi
fi

if [ ! -f "$BUILD/build.ninja" ]; then
	echo "configuring $BUILD (a first build also cross-compiles Boost, OpenSSL, FFmpeg and curl)"
	cmake -S "$SRC" -B "$BUILD" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/ps5-payload.cmake" \
		-DPS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" \
		-DCMAKE_BUILD_TYPE=Release -DUSE_LTO=NEVER > "$BUILD.cfg.log" 2>&1
	rc=$?
	if [ $rc -ne 0 ]; then
		echo "configure failed (rc=$rc)"
		grep -nE "CMake Error|FATAL_ERROR|Configuring incomplete" -A3 "$BUILD.cfg.log" | tail -30
		exit $rc
	fi
fi

ninja -C "$BUILD" -k 0 -j"$(nproc)" > "$BUILD.build.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
	echo "build failed (rc=$rc)"
	grep -A30 "^FAILED" "$BUILD.build.log" | grep -i "error" | head -10
	exit $rc
fi

title="$BUILD/title/PPSA99300"
if [ ! -f "$title/eboot.bin" ]; then
	echo "build reported success but $title/eboot.bin is missing"
	exit 1
fi
echo "built $title/eboot.bin ($(stat -c %s "$title/eboot.bin") bytes)"
