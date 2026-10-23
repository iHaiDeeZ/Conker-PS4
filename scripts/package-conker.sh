#!/usr/bin/env bash
# Package the PS4 build (after build-conker.sh) as Conker's Bad Fur Day: Recompiled, CBFD00064
# (IV0000-CBFD00064_00-CONKERRECOMPPS40.pkg), into out/Conker-vXX.YY.pkg. Earlier packages in out/ stay.
#   CONKER_VERSION=00.01 package-conker.sh
# The package carries the recompiled game code built from your own ROM, and the ROM itself
# (/app0/game/conker.z64, which the game reads its data from; stored on first start, so there's no
# Load ROM step): it's for your console only, never to be shared. CBFD_ROM picks another ROM file.
# The console gets the package from Windows with scripts/upload-to-console.sh (uploads sent from WSL
# were lost on the console).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
BUILD="$PS4_BUILD_ROOT/build/conker"
SRC="${CBFD_SRC:-$PS4_BUILD_ROOT/src/CBFD-Recompiled}"
EBOOT="$BUILD/ConkerRecomp_eboot/eboot.bin"
ELF="$BUILD/ConkerRecomp"
VERSION="${CONKER_VERSION:-00.01}"

[ -f "$EBOOT" ] || { echo "missing $EBOOT: build first" >&2; exit 1; }
[ "$EBOOT" -nt "$ELF" ] || { echo "$EBOOT is older than the ELF: the last build failed" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9][0-9]\.[0-9][0-9]$ ]] || { echo "version must look like 01.00" >&2; exit 1; }
STAGE="$PS4_BUILD_ROOT/build/conker-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys"
cp "$EBOOT" "$STAGE/eboot.bin"
python3 "$HERE/make-art.py" "$HERE/../assets" "$STAGE/sce_sys"
echo "$VERSION" > "$STAGE/version.txt"
# RecompFrontend's fonts, icons, style sheet and controller mappings (/app0/assets)
cp -r "$SRC/host/assets" "$STAGE/assets"
# the ROM the code was recompiled from (the US ROM; the build checked it). Not in a folder named
# rom: create-gp4 leaves that one out of the package's folder list, and PkgTool then fails.
ROM="${CBFD_ROM:-$SRC/conker/baserom.us.z64}"
[ -f "$ROM" ] || { echo "missing the ROM ($ROM)" >&2; exit 1; }
mkdir -p "$STAGE/game"
cp "$ROM" "$STAGE/game/conker.z64"
cp "$SRC/LICENSE" "$STAGE/LICENSE.txt"
# PkgTool's pkg_build crashes ("Sequence contains no elements") on empty folders.
find "$STAGE" -type d -empty -delete
# Vulkan (RADV on GNM) runs under the plain homebrew identity (4.5 GiB of direct memory).
PKG="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 DOLPHIN_PS4_MODULES_DIR=/nonexistent "$HERE/make-pkg.sh" "$STAGE" CBFD00064 "Conker's Bad Fur Day: Recompiled" "$VERSION" CONKERRECOMPPS4 "$PS4_BUILD_ROOT/out" | tail -1)"
ls -la "$PKG"
# a copy next to the repo (E:\PS4 Projects\Conker\out), easy to find from Windows; *.pkg is ignored by git
mkdir -p "$HERE/../out"
NAME="Conker-v$VERSION.pkg"
cp "$PKG" "$HERE/../out/$NAME"
# Keep the symbols of every packaged build: crash logs must be symbolized against the exact binary.
mkdir -p "$PS4_BUILD_ROOT/oelf"
cp "$BUILD/ConkerRecomp_eboot/ConkerRecomp.oelf" "$PS4_BUILD_ROOT/oelf/v$VERSION.oelf"
cp "$ELF" "$PS4_BUILD_ROOT/oelf/v$VERSION.elf"
echo "now, from Windows (Git Bash): scripts/upload-to-console.sh out/$NAME"
