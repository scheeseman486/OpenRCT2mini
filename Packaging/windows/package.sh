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
        # libssp-0.dll provides __stack_chk_fail / __stack_chk_guard. The
        # engine is built with -fstack-protector-strong via the if (MINGW)
        # branch in src/openrct2/CMakeLists.txt:181, so the runtime DLL
        # is loaded at process start. Without bundling libssp-0.dll the
        # exe fails to load (Wine reports the missing library by name).
        for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll libssp-0.dll; do
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
#
# Path resolution: Platform.Win32.cpp's GetInstallPath() returns
# <exeDir>/data (Platform.Win32.cpp:177), and Drawing.Sprite.cpp:571 loads
# g2.dat / fonts.dat / palettes.dat / tracks.dat from
# env.GetDirectoryPath(DirBase::openrct2), which is exactly that
# Platform::GetInstallPath() return value (PlatformEnvironment.cpp:276).
# So the four .dat files must live INSIDE data/, not at the bundle root.
##############################################################################
echo "==[ graphics .dat files ]======================================="
cp "$PROJECT_ROOT/build-host"/{g2,fonts,palettes,tracks}.dat "$STAGE_DIR/data/"

##############################################################################
# 4) data/ tree — language, objects, sequences, sfx, music, shaders,
#    scenario_patches.
#
# Why we download here, not at cmake-install time: the upstream
# install(CODE ...) download blocks in CMakeLists.txt:546+ aren't tagged
# with any COMPONENT, so they fall in the default "Unspecified" bucket.
# build.sh runs `cmake --install . --component openrct2` to skip the
# (unlinkable) openrct2-cli, which also skips the asset-pack downloads.
# Replicating the four downloads here keeps the bundle self-contained
# without forcing us to either tag every install rule or build a CLI
# target the cross-toolchain can't link.
#
# Layout produced (matches upstream OpenRCT2 portable Windows release):
#   data/g2.dat data/fonts.dat data/palettes.dat data/tracks.dat
#   data/language/  data/scenario_patches/  data/shaders/   (from source tree)
#   data/object/                                            (from objects.zip)
#   data/sequence/                                          (from title-sequences.zip)
#   data/assetpack/openrct2.sound.parkap                    (from opensound.zip)
#   data/assetpack/openrct2.music.alternative.parkap        (from openmusic.zip)
##############################################################################
echo "==[ data tree (static) ]========================================"
echo "  source data/ tree -> $STAGE_DIR/data/"
if [ -d "$PROJECT_ROOT/data" ]; then
    cp -r "$PROJECT_ROOT/data"/* "$STAGE_DIR/data/" 2>/dev/null || true
fi

echo "==[ data tree (asset packs — download) ]========================"
# URLs come from assets.json. Keep this list in sync with that file.
ASSETS_JSON="$PROJECT_ROOT/assets.json"
get_url() { python3 -c "import json,sys; print(json.load(open('$ASSETS_JSON'))['$1']['url'])"; }
get_sha() { python3 -c "import json,sys; print(json.load(open('$ASSETS_JSON'))['$1']['sha256'])"; }

CACHE_DIR="$DIST_DIR/asset-cache"
mkdir -p "$CACHE_DIR"

fetch_and_extract() {
    local key=$1 dest=$2
    local url
    url=$(get_url "$key")
    local sha
    sha=$(get_sha "$key")
    local zip="$CACHE_DIR/$(basename "$url")"

    if [ -f "$zip" ]; then
        local actual_sha
        actual_sha=$(sha256sum "$zip" | awk '{print $1}')
        if [ "$actual_sha" != "$sha" ]; then
            echo "  cache miss (sha mismatch) — re-downloading $key"
            rm -f "$zip"
        fi
    fi
    if [ ! -f "$zip" ]; then
        echo "  download $url"
        curl -fL --retry 3 -o "$zip" "$url"
        local actual_sha
        actual_sha=$(sha256sum "$zip" | awk '{print $1}')
        if [ "$actual_sha" != "$sha" ]; then
            echo "ERROR: sha256 mismatch for $key" >&2
            echo "  expected $sha" >&2
            echo "  got      $actual_sha" >&2
            exit 1
        fi
    fi
    echo "  extract $(basename "$zip") -> $dest"
    mkdir -p "$dest"
    unzip -q -o "$zip" -d "$dest"
}

fetch_and_extract objects          "$STAGE_DIR/data/object"
fetch_and_extract title-sequences  "$STAGE_DIR/data/sequence"
# opensfx + openmusic zips contain the assetpack/ folder at top level,
# so they extract directly into data/ — same convention as the install
# rules in CMakeLists.txt:577 and CMakeLists.txt:587.
fetch_and_extract opensfx          "$STAGE_DIR/data"
fetch_and_extract openmusic        "$STAGE_DIR/data"

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
