#!/usr/bin/env bash
# Exports the PS4 changes in the build tree's checkouts into patches/ (one patch per repository).
# CBFD-Recompiled's tools carry its own patches (recomp/*.patch), committed in each checkout as
# "CBFD-Recompiled patches (baseline)" on branch ps4: the PS4 patches are against those commits.
# plume and sljit are submodules of rt64 and N64ModernRuntime's N64Recomp, untouched by CBFD's patches: against
# their checked-out commits. SDL2 is against release-2.32.10.
#   save-patch.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${CBFD_SRC:-$HOME/cbfd-build/src/CBFD-Recompiled}"
OUT="$HERE/../patches"
mkdir -p "$OUT"
save() {
    local repo="$1" name="$2"
    git -C "$repo" add -A -N . 2>/dev/null || true
    git -C "$repo" diff --binary --ignore-submodules=all HEAD > "$OUT/$name.patch"
    printf '%-28s %s lines\n' "$name.patch" "$(wc -l < "$OUT/$name.patch")"
}
save "$SRC" cbfd-ps4
save "$SRC/tools/N64ModernRuntime" n64modernruntime-ps4
save "$SRC/tools/N64ModernRuntime/N64Recomp/lib/sljit" sljit-ps4
save "$SRC/tools/rt64" rt64-ps4
save "$SRC/tools/rt64/src/contrib/plume" plume-ps4
save "$SRC/tools/RecompFrontend" recompfrontend-ps4
save "$HOME/cbfd-build/src/SDL2" sdl2-ps4
{
    echo "Base revisions of the PS4 patches (CBFD-Recompiled's own patches applied first, see save-patch.sh)"
    echo "CBFD-Recompiled  https://github.com/sciaschi/CBFD-Recompiled  $(git -C "$SRC" rev-parse master)"
    for t in tools/N64Recomp tools/N64ModernRuntime tools/N64ModernRuntime/N64Recomp/lib/sljit tools/rt64 tools/rt64/src/contrib/plume tools/RecompFrontend; do
        echo "$t  $(git -C "$SRC" ls-tree -r HEAD "$t" 2>/dev/null | awk '{print $3}' | head -1 || true)$(git -C "$SRC/$t" rev-parse HEAD~0 >/dev/null 2>&1 && echo " (checkout $(git -C "$SRC/$t" rev-parse --short HEAD))")"
    done
    echo "SDL2  https://github.com/libsdl-org/SDL  release-2.32.10"
    echo "FreeType  https://gitlab.freedesktop.org/freetype/freetype  VER-2-13-3"
} > "$OUT/BASE.txt"
