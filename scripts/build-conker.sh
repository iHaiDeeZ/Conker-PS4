#!/usr/bin/env bash
# Builds the configured PS4 host (configure-conker.sh first): ConkerRecomp and its eboot.bin.
#   build-conker.sh [ninja args...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"   # PacBrew paths, ld.lld's libxml2 compat
BUILD="$PS4_BUILD_ROOT/build/conker"
# ninja's status, not grep's (which fails when there is nothing to show)
set +o pipefail
ninja -C "$BUILD" -j"${JOBS:-16}" "$@" 2>&1 | tee "$PS4_BUILD_ROOT/logs/build.log" | grep -E "error:|FAILED|warning: .*undefined|^ninja: build stopped" | head -${ERRORS:-80}
exit "${PIPESTATUS[0]}"
