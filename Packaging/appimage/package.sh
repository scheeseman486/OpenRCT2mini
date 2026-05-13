#!/bin/bash
# Wrap dist/OpenRCT2mini-<version>-x86_64.AppImage into a portable zip
# the user can unzip-and-run.
#
# The AppImage itself is fully self-contained (build.sh sets
# DOWNLOAD_TITLE_SEQUENCES / OBJECTS / OPENSFX / OPENMUSIC = ON so
# CMake's install rules fetch the upstream content packs at build time
# and linuxdeploy bundles them inside the AppImage). So the zip just
# adds:
#   - The AppImage (renamed to a stable filename)
#   - A readme.txt for the end user
#   - The fork's main licence file
#   - An empty save/ that the runtime populates on first launch
#
# Output: dist/OpenRCT2mini-x86_64-<version>.zip
# Layout when extracted:
#     OpenRCT2mini-x86_64-<version>/
#         OpenRCT2mini-x86_64.AppImage
#         save/                            (empty; populated on first run)
#         readme.txt
#         LICENSE
#
# The Linux platform code (Platform.Linux.cpp) reads the APPIMAGE env var
# the AppImageKit runtime sets and routes user-data to dirname($APPIMAGE).
# So saves end up in the save/ folder this zip ships.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST_DIR="$PROJECT_ROOT/dist"

# Locate the AppImage build.sh just produced.
APPIMAGE_PATH=$(ls "$DIST_DIR"/OpenRCT2mini-*-x86_64.AppImage 2>/dev/null | head -1 || true)
if [ -z "$APPIMAGE_PATH" ]; then
    echo "ERROR: no AppImage in $DIST_DIR — run ./Packaging/appimage/build.sh first." >&2
    exit 1
fi
APPIMAGE_NAME=$(basename "$APPIMAGE_PATH")

# Version is the chunk between "OpenRCT2mini-" and "-x86_64.AppImage".
VERSION=$(echo "$APPIMAGE_NAME" | sed -E 's/^OpenRCT2mini-(.+)-x86_64\.AppImage$/\1/')
STAGE_NAME="OpenRCT2mini-x86_64-$VERSION"
STAGE_DIR="$DIST_DIR/$STAGE_NAME"
ZIP_OUT="$DIST_DIR/${STAGE_NAME}.zip"

echo "==[ staging $STAGE_DIR ]==========================================="
rm -rf "$STAGE_DIR" "$ZIP_OUT"
mkdir -p "$STAGE_DIR/save"

# AppImage — copy in, rename to a stable user-facing filename.
cp "$APPIMAGE_PATH" "$STAGE_DIR/OpenRCT2mini-x86_64.AppImage"
chmod 0755 "$STAGE_DIR/OpenRCT2mini-x86_64.AppImage"

# Top-level LICENSE (the GPLv3 fork licence). The bundled content packs'
# individual licences (CC-BY-4.0 / MIT / CC-BY-SA-4.0) travel inside the
# AppImage in their respective share/openrct2/<dir>/ subdirs — they're
# preserved by the upstream zips CMake fetches.
if [ -f "$PROJECT_ROOT/licence.txt" ]; then
    cp "$PROJECT_ROOT/licence.txt" "$STAGE_DIR/LICENSE"
fi
if [ -f "$PROJECT_ROOT/THIRD_PARTY_NOTICES.md" ]; then
    cp "$PROJECT_ROOT/THIRD_PARTY_NOTICES.md" "$STAGE_DIR/THIRD_PARTY_NOTICES.md"
fi

# Readme — generated, version-substituted.
cat > "$STAGE_DIR/readme.txt" <<README_EOF
OpenRCT2mini ${VERSION} — x86_64 Linux AppImage portable build
==============================================================

How to use:
  1. Extract this zip to a directory you can write to, e.g.
       unzip OpenRCT2mini-x86_64-${VERSION}.zip -d ~/Games/
  2. Drop your legitimate RollerCoaster Tycoon 2 install directory next
     to OpenRCT2mini-x86_64.AppImage — the binary auto-detects an RCT2
     install in the same folder. RCT1 next to RCT2 is also auto-
     detected (used by the RCT1 title sequence).
  3. Run:
       cd ~/Games/OpenRCT2mini-x86_64-${VERSION}
       ./OpenRCT2mini-x86_64.AppImage

The 'save/' folder will hold your config + saves + screenshots after
first launch. The whole directory is portable — move it anywhere
(including a USB stick) and it keeps working.

What's inside the AppImage:
  * The engine (bundles SDL2, libpng, libzip, libzstd, libvorbis, libogg,
    libflac, libduktape, GL libs).
  * Title sequences (CC-BY-4.0).
  * Object packs (CC-BY-4.0).
  * OpenSFX (MIT) + OpenMusic (CC-BY-SA-4.0).
  Each pack's individual licence file travels inside the AppImage at
  /usr/share/openrct2/<dir>/.

System requirements:
  * Any reasonably modern x86_64 Linux desktop.
  * glibc >= 2.35 (Ubuntu 22.04, Debian 12, Fedora 36 or newer).
  * libfuse2 OR run the AppImage with --appimage-extract-and-run.

Multiplayer is disabled in this build. Software cursor is enabled by
default (matches the Miyoo Mini handheld build's rendering path).

Bug reports: https://github.com/scheeseman486/OpenRCT2mini/issues
README_EOF

# Zip the staged dir.
echo "==[ writing $ZIP_OUT ]============================================="
( cd "$DIST_DIR" && zip -r "$(basename "$ZIP_OUT")" "$STAGE_NAME" >/dev/null )

ls -la "$ZIP_OUT"
echo
echo "Portable zip ready: $ZIP_OUT"
echo "  Contents:"
unzip -l "$ZIP_OUT"
