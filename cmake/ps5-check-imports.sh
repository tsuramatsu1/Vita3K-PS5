#!/usr/bin/env bash
# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's, so an import that only their stubs define
# links, and is null at run time: its first call jumps to address 0 (PS5_RetroArch tools/build.sh records the same).
# Fails when the linked title has such an import; bind it to a stand-in instead (vita3k_ps5_title's LIBC_BINDINGS).
#
# ps5-check-imports.sh NM TITLE_ELF SDK_LIB_DIR STUB...
set -euo pipefail

nm=$1 title=$2 sdk_lib=$3
shift 3

missing=$(comm -23 \
    <("$nm" -D --undefined-only "$title" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for stub in "$sdk_lib"/*.so "$@"; do
        case $(basename "$stub") in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$nm" -D --defined-only "$stub" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))

if [[ -n $missing ]]; then
    echo "error: $title imports what no module a title loads exports (null at run time):" $missing >&2
    exit 1
fi
