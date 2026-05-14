#!/bin/bash
# Build OpenRCT2mini as a portable Windows x64 .exe + DLL bundle by
# cross-compiling from Linux.
# Output: dist/OpenRCT2mini-<version>-x64.zip
#
# Three stages:
#   (0) host build   — produces g2.dat / fonts.dat / palettes.dat /
#                       tracks.dat via openrct2-cli on the build host.
#                       Cross-builds can't run openrct2-cli (it's a
#                       Windows .exe in the cross case) so this stage
#                       gives us those .dat files for the cross stage
#                       to bundle. Skipped if build-host/*.dat exist
#                       and are newer than resources/*/sprites.json.
#   (1) docker build — builds openrctmini-windows-cross:latest if its
#                       inputs (Dockerfile, build-deps.sh) are newer
#                       than the cached image. ~10 min first time;
#                       cached on subsequent runs.
#   (2) docker run   — cmake configure + ninja + assemble the portable
#                       bundle inside the image.
#
# Run from the project root:   ./Packaging/windows/build.sh
#
# Flags:
#   --rebuild-toolchain    force docker image rebuild
#   --debug                CMAKE_BUILD_TYPE=Debug
#   --skip-host-graphics   reuse existing build-host/*.dat
#   --no-package           skip the trailing zip step
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-windows"
HOST_BUILD_DIR="$PROJECT_ROOT/build-host"
IMAGE_TAG="${OPENRCT2MINI_WINDOWS_IMAGE:-openrctmini-windows-cross:latest}"
DOCKERFILE_DIR="$PROJECT_ROOT/Packaging/windows"

mkdir -p "$BUILD_DIR"

DEBUG_BUILD=0
SKIP_HOST_GRAPHICS=0
for arg in "$@"; do
    case "$arg" in
        --debug) DEBUG_BUILD=1 ;;
        --skip-host-graphics) SKIP_HOST_GRAPHICS=1 ;;
    esac
done

##############################################################################
# Stage 0: host build for the .dat files.
##############################################################################
HOST_DATS_OK=1
for f in g2.dat fonts.dat palettes.dat tracks.dat; do
    if [ ! -f "$HOST_BUILD_DIR/$f" ]; then
        HOST_DATS_OK=0
        break
    fi
done

if [ "$SKIP_HOST_GRAPHICS" = "1" ] && [ "$HOST_DATS_OK" = "1" ]; then
    echo "==[ stage 0: reusing existing build-host/*.dat ]==============="
elif [ "$HOST_DATS_OK" = "1" ]; then
    # All four .dat files exist — check if any input is newer.
    NEWEST_DAT=$(stat -c '%Y' "$HOST_BUILD_DIR"/*.dat 2>/dev/null | sort -rn | head -1)
    NEWEST_INPUT=$(find "$PROJECT_ROOT/resources"/{g2,fonts,palettes,tracks} -type f -printf '%T@\n' 2>/dev/null | sort -rn | head -1 | cut -d. -f1)
    if [ -n "$NEWEST_INPUT" ] && [ -n "$NEWEST_DAT" ] && [ "$NEWEST_INPUT" -gt "$NEWEST_DAT" ]; then
        echo "==[ stage 0: build-host/*.dat stale — rebuilding ]=============="
        HOST_DATS_OK=0
    else
        echo "==[ stage 0: build-host/*.dat up-to-date — skipping ]=========="
    fi
fi

if [ "$HOST_DATS_OK" = "0" ]; then
    echo "==[ stage 0: host build — graphics target ]===================="
    mkdir -p "$HOST_BUILD_DIR"
    # Configure flags mirror the AppImage host-graphics step in
    # .github/workflows/build.yml so the .dat output is byte-identical.
    cmake -S "$PROJECT_ROOT" -B "$HOST_BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_CXX_FLAGS="-Wno-error=null-dereference -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=stringop-overread -Wno-error=dangling-reference -Wno-error=maybe-uninitialized" \
        -DDISABLE_NETWORK=ON -DDISABLE_HTTP=ON -DDISABLE_OPENGL=ON \
        -DDISABLE_TTF=ON -DDISABLE_FLAC=ON -DDISABLE_DISCORD_RPC=ON \
        -DDISABLE_GOOGLE_BENCHMARK=ON -DDISABLE_VERSION_CHECKER=ON \
        -DENABLE_SCRIPTING=OFF -DDISABLE_ICU=ON \
        -DDOWNLOAD_TITLE_SEQUENCES=OFF -DDOWNLOAD_OBJECTS=OFF \
        -DDOWNLOAD_OPENSFX=OFF -DDOWNLOAD_OPENMUSIC=OFF \
        -DOPENRCT2_USE_CCACHE=OFF
    cmake --build "$HOST_BUILD_DIR" --target graphics
    ls -la "$HOST_BUILD_DIR"/*.dat
fi

##############################################################################
# Stage 1: build the cross-toolchain image if stale.
##############################################################################
NEED_BUILD=0
if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    NEED_BUILD=1
fi
for arg in "$@"; do
    if [ "$arg" = "--rebuild-toolchain" ]; then NEED_BUILD=1; fi
done
if [ "$NEED_BUILD" = "0" ] && docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    IMAGE_EPOCH=$(docker image inspect "$IMAGE_TAG" --format '{{.Created}}' \
        | xargs -I{} date -d {} +%s 2>/dev/null || echo 0)
    for f in "$DOCKERFILE_DIR/Dockerfile" "$DOCKERFILE_DIR/build-deps.sh"; do
        if [ -f "$f" ]; then
            FILE_EPOCH=$(stat -c '%Y' "$f" 2>/dev/null || echo 0)
            if [ "$FILE_EPOCH" -gt "$IMAGE_EPOCH" ]; then
                echo "==[ Windows-cross image older than $(basename "$f") — rebuilding ]=="
                NEED_BUILD=1
                break
            fi
        fi
    done
fi

if [ "$NEED_BUILD" = "1" ]; then
    echo "==[ docker build openrctmini-windows-cross — ~10 min first-run ]=="
    docker build -t "$IMAGE_TAG" "$DOCKERFILE_DIR"
fi

##############################################################################
# Stage 2: cmake + ninja inside the image, then assemble the bundle.
##############################################################################
if [ "$DEBUG_BUILD" = "1" ]; then
    BUILD_TYPE=Debug
    MINI_DEBUG_FLAG="-DOPENRCT2MINI_DEBUG=ON"
else
    BUILD_TYPE=Release
    MINI_DEBUG_FLAG=""
fi

# Patch resources/version.h with a real version string. The MSBuild path
# does this from scripts/build:14-23; we replicate inside build.sh so the
# .rc-compiled version info reflects the actual git describe output. The
# file is tracked in git at stub values; restored at the end of this
# script so the working tree stays clean.
ORIGINAL_VERSION_H="$PROJECT_ROOT/resources/version.h"
ORIGINAL_VERSION_H_BACKUP="$(mktemp)"
cp "$ORIGINAL_VERSION_H" "$ORIGINAL_VERSION_H_BACKUP"

VERSION=$(git -C "$PROJECT_ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)
SHORT_SHA=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo 0000000)
# FILEVERSION takes a 4-tuple of u16's. Parse v0.3.0-X-gHHHH-dirty into
# 0,3,0,X format. Fall back to 0,0,0,0 if regex doesn't match.
FILEVER_RAW="$VERSION"
FILEVER=$(echo "$FILEVER_RAW" | sed -E 's/^v?([0-9]+)\.([0-9]+)\.([0-9]+)(-([0-9]+))?.*/\1,\2,\3,\5/' | sed 's/,$/,0/' | sed 's/,,/,0,/g')
if ! echo "$FILEVER" | grep -qE '^[0-9]+,[0-9]+,[0-9]+,[0-9]+$'; then
    FILEVER="0,0,0,0"
fi

cat > "$ORIGINAL_VERSION_H" <<EOF
#define OPENRCT2_FILE_VERSION ${FILEVER}
#define OPENRCT2_PRODUCT_VERSION "${VERSION}-${SHORT_SHA}"
EOF
echo "==[ resources/version.h patched: FILEVER=${FILEVER}, PRODUCT=${VERSION}-${SHORT_SHA} ]=="

# Restore on exit (success or failure).
restore_version_h() {
    if [ -f "$ORIGINAL_VERSION_H_BACKUP" ]; then
        cp "$ORIGINAL_VERSION_H_BACKUP" "$ORIGINAL_VERSION_H"
        rm "$ORIGINAL_VERSION_H_BACKUP"
        echo "==[ resources/version.h restored to stub values ]=="
    fi
}
trap restore_version_h EXIT

CMAKE_FLAGS=(
    "-DCMAKE_TOOLCHAIN_FILE=/src/Packaging/windows/toolchain-mingw-w64-x86_64.cmake"
    "-DOPENRCT2MINI=OFF"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCMAKE_INSTALL_PREFIX=/install"
    "-DDISABLE_NETWORK=ON"
    "-DDISABLE_HTTP=ON"
    "-DDISABLE_ICU=ON"
    "-DDISABLE_DISCORD_RPC=ON"
    "-DDISABLE_GOOGLE_BENCHMARK=ON"
    "-DDISABLE_VERSION_CHECKER=ON"
    "-DENABLE_SOFTWARE_CURSOR=ON"
    "-DENABLE_SCRIPTING=OFF"
    "-DDISABLE_OPENGL=OFF"
    "-DDISABLE_FLAC=OFF"
    "-DDISABLE_VORBIS=OFF"
    # CMake's graphics .dat custom-command is gated on
    # NOT CMAKE_CROSSCOMPILING (root CMakeLists.txt:493). The toolchain
    # file sets CMAKE_SYSTEM_NAME=Windows which trips CMAKE_CROSSCOMPILING,
    # so the target is skipped — we copy the .dat files in from
    # build-host/ before linking instead.
    "-DDOWNLOAD_TITLE_SEQUENCES=ON"
    "-DDOWNLOAD_OBJECTS=ON"
    "-DDOWNLOAD_OPENSFX=ON"
    "-DDOWNLOAD_OPENMUSIC=ON"
    "-DOPENRCT2_USE_CCACHE=OFF"
)
if [ -n "$MINI_DEBUG_FLAG" ]; then
    CMAKE_FLAGS+=("$MINI_DEBUG_FLAG")
fi

docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PROJECT_ROOT:/src" \
    -w /src/build-windows \
    "$IMAGE_TAG" \
    bash -c "
        set -euo pipefail

        echo '==[ cmake configure ]=========================================='
        cmake /src -G Ninja ${CMAKE_FLAGS[*]@Q}

        # Sanity: SDL2 should resolve to the cross-sysroot. If the host
        # libSDL2 leaked through somehow (libsdl2-dev is installed in the
        # Dockerfile only as a transitive of the AppImage image, shouldn't
        # apply here but defence-in-depth) we want to know early.
        if grep -E 'SDL2.*/usr/lib/x86_64-linux-gnu|/usr/include/SDL2' CMakeCache.txt >/dev/null 2>&1; then
            echo 'ERROR: SDL2 resolved to a Linux host path — toolchain leak' >&2
            grep -E 'SDL2' CMakeCache.txt >&2
            exit 1
        fi

        echo '==[ copy host-built .dat files into the cross build ]=========='
        cp /src/build-host/g2.dat /src/build-host/fonts.dat \
           /src/build-host/palettes.dat /src/build-host/tracks.dat \
           /src/build-windows/

        echo '==[ ninja ]===================================================='
        # The 'graphics' target is gated out by CMAKE_CROSSCOMPILING (root
        # CMakeLists.txt:493) so we don't ask for it here — the .dat files
        # were copied in from build-host/ in the step above.
        ninja openrct2 openrct2-cli -j\$(nproc)

        echo '==[ ninja install DESTDIR=AppDir ]============================='
        rm -rf AppDir
        DESTDIR=AppDir ninja install

        echo '==[ produced binaries ]========================================'
        ls -la /src/build-windows/openrct2.exe /src/build-windows/openrct2-cli.exe
        file /src/build-windows/openrct2.exe
    "

echo
echo "==[ post-build inspection (host side) ]============================"
ls -la "$BUILD_DIR/openrct2.exe" "$BUILD_DIR/openrct2-cli.exe"

# Optional packaging step.
if printf '%s\n' "$@" | grep -q -- '--no-package'; then
    echo "Skipping packaging (--no-package set)."
else
    echo
    echo "==[ assemble portable bundle ]====================================="
    bash "$DOCKERFILE_DIR/package.sh"
fi
