#!/bin/bash
# Assemble dist/OpenRCT2mini-<version>-x64.zip from the cross-build output
# at build-windows/ and the install tree at build-windows/AppDir/.
# Patterned on scripts/build-portable + Packaging/appimage/package.sh.
#
# Bundle layout produced:
#   OpenRCT2mini-x64/
#     openrct2.exe
#     openrct2.com        (objcopy --subsystem console copy)
#     openrct2-cli.exe
#     SDL2.dll
#     libgcc_s_seh-1.dll
#     libstdc++-6.dll
#     libwinpthread-1.dll
#     g2.dat / fonts.dat / palettes.dat / tracks.dat
#     data/
#       language/ object/ scenario_patches/ sequence/ shaders/ assetpack/
#     readme.txt / changelog.txt / licence.txt / contributors.md

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-windows"
DIST_DIR="$PROJECT_ROOT/dist"
STAGE_DIR="$DIST_DIR/OpenRCT2mini-x64"
APPDIR="$BUILD_DIR/AppDir"

mkdir -p "$DIST_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/data"

# Version stamp.
VERSION=$(git -C "$PROJECT_ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)

##############################################################################
# 1) The two executables.
##############################################################################
echo "==[ executables ]==============================================="
cp "$BUILD_DIR/openrct2.exe"     "$STAGE_DIR/openrct2.exe"
# openrct2-cli.exe is not built in the cross — the weak-symbol bridge
# (drawFocusOutlineIfActive) doesn't work the same on PE as on ELF, so
# the CLI link fails. The .dat files were produced by the host build at
# stage 0; the cross binary doesn't need a CLI counterpart.
# cp "$BUILD_DIR/openrct2-cli.exe" "$STAGE_DIR/openrct2-cli.exe"

# .exe/.com split — duplicate openrct2.exe as openrct2.com with the PE
# subsystem field flipped to console. cmd.exe resolves .com before .exe
# on PATH, so users can invoke `openrct2` from a terminal and get stdout
# in the window, or double-click openrct2.exe for a GUI launch with no
# stray cmd window. Same UX as upstream's MSBuild + editbin split.
cp "$BUILD_DIR/openrct2.exe" "$STAGE_DIR/openrct2.com"
x86_64-w64-mingw32-objcopy --subsystem console "$STAGE_DIR/openrct2.com"

##############################################################################
# 2) Runtime DLLs — SDL2 + mingw libgcc/libstdc++/libwinpthread.
##############################################################################
echo "==[ runtime DLLs ]=============================================="
# SDL2.dll from our cross-sysroot. The build's actual DLL is in the
# Docker image at /opt/mingw-sysroot/bin/SDL2.dll — copy it out via the
# bind-mounted volume isn't available here, so we go through the image.
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$STAGE_DIR:/out" \
    "${OPENRCT2MINI_WINDOWS_IMAGE:-openrctmini-windows-cross:latest}" \
    bash -c "
        set -euo pipefail
        cp /opt/mingw-sysroot/bin/SDL2.dll /out/
        # mingw runtime DLLs from the apt-installed cross-toolchain.
        # Paths checked: /usr/lib/gcc/x86_64-w64-mingw32/<ver>/ and
        # /usr/x86_64-w64-mingw32/lib/.
        for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
            src=\$(find /usr/lib/gcc/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32 -name \"\$dll\" 2>/dev/null | head -1)
            if [ -z \"\$src\" ]; then
                echo \"ERROR: \$dll not found in cross-toolchain\" >&2
                exit 1
            fi
            cp \"\$src\" /out/\$dll
            echo \"  copied \$src -> /out/\$dll\"
        done
    "

##############################################################################
# 3) Generated graphics .dat files (from build-host/).
##############################################################################
echo "==[ graphics .dat files ]======================================="
cp "$PROJECT_ROOT/build-host"/{g2,fonts,palettes,tracks}.dat "$STAGE_DIR/"

##############################################################################
# 4) data/ tree — language, objects, sequences, sfx, music, shaders,
#    scenario_patches. Pulled from the install tree which the cmake
#    install step laid out under AppDir/install/share/openrct2/.
##############################################################################
echo "==[ data tree ]================================================="
# The install step may not have run (e.g. when openrct2-cli won't link).
# We assemble data/ from whatever directories are present. The static
# parts (language, scenario_patches, shaders) always live in the source
# tree. Downloaded asset packs (sequence, object, assetpack) land in
# build-windows/data/ from DOWNLOAD_* configure steps.
echo "  source data/ tree -> $STAGE_DIR/data/"
if [ -d "$PROJECT_ROOT/data" ]; then
    cp -r "$PROJECT_ROOT/data"/* "$STAGE_DIR/data/" 2>/dev/null || true
fi
echo "  CMake-downloaded asset packs -> $STAGE_DIR/data/"
for src in "$BUILD_DIR/data" "$APPDIR/install/share/openrct2"; do
    if [ -d "$src" ]; then
        echo "    from $src"
        cp -r "$src"/* "$STAGE_DIR/data/" 2>/dev/null || true
    fi
done

##############################################################################
# 5) Documentation.
##############################################################################
echo "==[ docs ]======================================================"
cp "$PROJECT_ROOT/distribution/readme.txt"     "$STAGE_DIR/readme.txt"     2>/dev/null || true
cp "$PROJECT_ROOT/distribution/changelog.txt"  "$STAGE_DIR/changelog.txt"  2>/dev/null || true
cp "$PROJECT_ROOT/licence.txt"                 "$STAGE_DIR/licence.txt"    2>/dev/null || true
cp "$PROJECT_ROOT/contributors.md"             "$STAGE_DIR/contributors.md" 2>/dev/null || true

##############################################################################
# 6) Zip the staging dir.
##############################################################################
echo "==[ zip ]======================================================="
FINAL="OpenRCT2mini-${VERSION}-x64.zip"
cd "$DIST_DIR"
rm -f "$FINAL"
zip -r9 "$FINAL" OpenRCT2mini-x64/
echo
echo "==[ produced $DIST_DIR/$FINAL ]================================="
ls -la "$DIST_DIR/$FINAL"
echo
echo "Bundle contents:"
unzip -l "$DIST_DIR/$FINAL" | head -30
echo
echo "(truncated — see 'unzip -l $FINAL' for full listing)"
