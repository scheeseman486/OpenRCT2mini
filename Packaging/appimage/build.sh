#!/bin/bash
# Build OpenRCT2mini as a self-contained x86_64 Linux AppImage.
# Output: dist/OpenRCT2mini-<version>-x86_64.AppImage
#
# Two stages:
#   (1) docker build  — builds the openrctmini-appimage image once, derived
#       from ubuntu:22.04 plus dev deps + pinned linuxdeploy. Cached after
#       first run.
#   (2) docker run    — uses that image to cmake + make + linuxdeploy the
#       project into an AppImage.
#
# Run from the project root:   ./Packaging/appimage/build.sh
#
# Flags:
#   --rebuild-toolchain  force rebuild of the docker image
#   --debug              CMAKE_BUILD_TYPE=Debug, +OPENRCT2MINI_DEBUG (the
#                       diagnostic logging compiles in, but OPENRCT2MINI=OFF
#                       keeps the cuts inactive — useful for debugging host
#                       code paths from the same container)
#   --no-package         skip the trailing call to package.sh (skip the zip
#                       wrap step; the bare AppImage is still produced)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-appimage"
IMAGE_TAG="${OPENRCT2MINI_APPIMAGE_IMAGE:-openrctmini-appimage:latest}"
DOCKERFILE_DIR="$PROJECT_ROOT/Packaging/appimage"

mkdir -p "$BUILD_DIR"

DEBUG_BUILD=0
for arg in "$@"; do
    if [ "$arg" = "--debug" ]; then DEBUG_BUILD=1; fi
done

# Stage 1: build the AppImage image if it doesn't exist or its inputs
# (Dockerfile) changed. Same staleness-check pattern as Mini's build.sh.
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
    for f in "$DOCKERFILE_DIR/Dockerfile"; do
        if [ -f "$f" ]; then
            FILE_EPOCH=$(stat -c '%Y' "$f" 2>/dev/null || echo 0)
            if [ "$FILE_EPOCH" -gt "$IMAGE_EPOCH" ]; then
                echo "==[ AppImage image is older than $(basename "$f") — rebuilding ]=="
                NEED_BUILD=1
                break
            fi
        fi
    done
fi

if [ "$NEED_BUILD" = "1" ]; then
    echo "==[ docker build openrctmini-appimage — first-run takes ~3 min ]=="
    docker build -t "$IMAGE_TAG" "$DOCKERFILE_DIR"
fi

# Stage 2: cmake + make + linuxdeploy using the AppImage image.
#
# The CMake flag set: APPIMAGE=ON enables the install-rpath that linuxdeploy
# needs. OPENRCT2MINI=OFF (explicit) selects the host-restoration paths.
# DISABLE_NETWORK/ICU/DISCORD_RPC + ENABLE_SOFTWARE_CURSOR are preserved
# from the Mini baseline per host-restoration-plan. OpenGL / FLAC / Vorbis
# flip back on for desktop. ENABLE_SCRIPTING stays OFF — the plugin API
# isn't in scope for this fork. DOWNLOAD_* flips ON so CMake fetches the
# upstream title sequences / objects / OpenSFX / OpenMusic at install
# time; linuxdeploy then bundles them into the AppImage payload.
if [ "$DEBUG_BUILD" = "1" ]; then
    BUILD_TYPE=Debug
    MINI_DEBUG_FLAG="-DOPENRCT2MINI_DEBUG=ON"
else
    BUILD_TYPE=Release
    MINI_DEBUG_FLAG=""
fi
CMAKE_FLAGS=(
    -DAPPIMAGE=ON
    -DOPENRCT2MINI=OFF
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE
    -DCMAKE_INSTALL_PREFIX=/usr
    -DDISABLE_NETWORK=ON
    -DDISABLE_HTTP=ON
    -DDISABLE_ICU=ON
    -DDISABLE_DISCORD_RPC=ON
    -DDISABLE_GOOGLE_BENCHMARK=ON
    -DDISABLE_VERSION_CHECKER=ON
    -DENABLE_SOFTWARE_CURSOR=ON
    # Scripting stays OFF on host too. The plugin API isn't a goal for this
    # fork (no user mods expected), and ENABLE_SCRIPTING=ON pulls in code
    # that wasn't carried through the C++17 backport (cut 33) — the Mini
    # build doesn't hit it because scripting is OFF there. Re-enabling on
    # host would mean either restoring C++20 or backporting the scripting
    # surface; both are out of scope.
    -DENABLE_SCRIPTING=OFF
    -DDISABLE_OPENGL=OFF
    -DDISABLE_FLAC=OFF
    -DDISABLE_VORBIS=OFF
    # DOWNLOAD_* flipped ON so CMake fetches the upstream title sequences,
    # objects, OpenSFX, OpenMusic at install time and lays them out under
    # AppDir/usr/share/openrct2/{sequence,object,assetpack}/. linuxdeploy
    # then bundles everything into the AppImage so a single .AppImage file
    # is fully self-contained — engine + content packs both. Same approach
    # upstream OpenRCT2's CI uses for its AppImage build.
    -DDOWNLOAD_TITLE_SEQUENCES=ON
    -DDOWNLOAD_OBJECTS=ON
    -DDOWNLOAD_OPENSFX=ON
    -DDOWNLOAD_OPENMUSIC=ON
    -DOPENRCT2_USE_CCACHE=OFF
)
if [ -n "$MINI_DEBUG_FLAG" ]; then
    CMAKE_FLAGS+=("$MINI_DEBUG_FLAG")
fi

# Run as host user so artefacts land with the user's UID.
# Bind-mount the project root at /src; cd into /src/build-appimage.
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PROJECT_ROOT:/src" \
    -w /src/build-appimage \
    "$IMAGE_TAG" \
    bash -c "
        set -euo pipefail
        echo '==[ cmake configure ]=========================================='
        cmake /src ${CMAKE_FLAGS[*]@Q}

        # Risk-1 guard from the plan: verify SDL2 resolved to the system
        # libsdl2-dev install path, not anywhere pointing at the vendor
        # mmiyoo SDL2 (which is dist/vendor/sdl2_miyoo-main and would
        # break on every desktop).
        if grep -E 'SDL2.*vendor|sdl2_miyoo' CMakeCache.txt >/dev/null 2>&1; then
            echo 'ERROR: SDL2 resolved to a vendor path — refusing to build' >&2
            grep -E 'SDL2' CMakeCache.txt >&2
            exit 1
        fi

        echo '==[ make ]====================================================='
        # 'graphics' is the umbrella target that builds g2.dat + fonts.dat +
        # palettes.dat + tracks.dat — see CMakeLists.txt 'add_custom_target(graphics ...)'.
        # There is no bare 'g2' target.
        make -j\$(nproc) openrct2 openrct2-cli graphics

        echo '==[ make install DESTDIR=AppDir ]=============================='
        rm -rf AppDir
        make install DESTDIR=AppDir

        echo '==[ linuxdeploy --output appimage ]============================'
        # Older linuxdeploy releases trip the 'do not run as root' guard if
        # the container's UID was root and is now mapped to the host user.
        # --appimage-extract-and-run avoids the FUSE-in-container hassle.
        # NO_STRIP works around an upstream linuxdeploy bug with already-
        # stripped binaries (same workaround the upstream OpenRCT2 CI uses).
        export NO_STRIP=true
        /opt/linuxdeploy-x86_64.AppImage \
            --appimage-extract-and-run \
            --appdir AppDir \
            --output appimage \
            --desktop-file AppDir/usr/share/applications/io.openrct2.openrct2.desktop \
            --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/openrct2.png

        # linuxdeploy produces 'OpenRCT2-x86_64.AppImage' (it derives the
        # name from the .desktop file's Name= entry). Rename to our fork
        # name + version stamp. git describe runs inside the container so
        # we need git available in /src — the host checkout's .git/ is
        # bind-mounted, so 'git -C /src describe' works.
        VERSION=\$(git -C /src describe --tags --always --dirty 2>/dev/null || echo dev)
        FINAL=\"OpenRCT2mini-\${VERSION}-x86_64.AppImage\"
        mkdir -p /src/dist
        mv OpenRCT2-x86_64.AppImage \"/src/dist/\${FINAL}\"
        echo \"==[ produced /src/dist/\${FINAL} ]==============================\"
        file \"/src/dist/\${FINAL}\"
        ls -la \"/src/dist/\${FINAL}\"
    "

echo
echo "==[ post-build inspection (host side) ]============================"
ls -la "$PROJECT_ROOT/dist/"OpenRCT2mini-*-x86_64.AppImage 2>/dev/null || \
    (echo "ERROR: no AppImage produced in dist/" >&2; exit 1)

# Optional packaging step: wrap the AppImage + supplementary content packs
# into a zip ready for end-user download. Skip with --no-package.
if printf '%s\n' "$@" | grep -q -- '--no-package'; then
    echo "Skipping packaging (--no-package set)."
else
    echo
    echo "==[ package portable zip ]====================================="
    bash "$PROJECT_ROOT/Packaging/appimage/package.sh"
fi
