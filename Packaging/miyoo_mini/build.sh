#!/bin/bash
# Cross-compile openrct2 for the Miyoo Mini.
# Output: build-arm/openrct2 (armhf, dynamically linked, static libstdc++).
#
# Two stages:
#   (1) docker build  — builds the openrctmini-toolchain image once, derived
#       from the upstream OnionUI toolchain plus pkg-config + the dev deps
#       OpenRCT2 needs (libzstd, libogg, libvorbis, libzip, nlohmann_json).
#       Cached after first run.
#   (2) docker run    — uses that derived image to cmake + make the project.
#
# Run from the project root:   ./Packaging/miyoo_mini/build.sh
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-arm"
IMAGE_TAG="${OPENRCT2MINI_TOOLCHAIN_IMAGE:-openrctmini-toolchain:latest}"
DOCKERFILE_DIR="$PROJECT_ROOT/Packaging/miyoo_mini"

mkdir -p "$BUILD_DIR"

# Stage 1: build the derived toolchain image if it doesn't exist or its
# inputs (Dockerfile, build-deps.sh) changed. `docker build` itself layers
# on top of any cached intermediate so re-runs are cheap.
NEED_BUILD=0
if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    NEED_BUILD=1
fi
# Force-rebuild if --rebuild-toolchain is passed.
for arg in "$@"; do
    if [ "$arg" = "--rebuild-toolchain" ]; then NEED_BUILD=1; fi
done

if [ "$NEED_BUILD" = "1" ]; then
    echo "==[ docker build openrctmini-toolchain — first-run takes ~10 min ]=="
    docker build -t "$IMAGE_TAG" "$DOCKERFILE_DIR"
fi

# Stage 2: cmake + make using the derived image.
# Phase A §4.1 cmake-flag cuts plus Phase B/C's source-level cuts.
CMAKE_FLAGS=(
    -DCMAKE_TOOLCHAIN_FILE=/src/Packaging/miyoo_mini/toolchainfile.cmake
    -DCMAKE_BUILD_TYPE=MinSizeRel
    -DDISABLE_NETWORK=ON
    -DDISABLE_HTTP=ON
    -DDISABLE_OPENGL=ON
    -DDISABLE_TTF=ON
    -DDISABLE_FLAC=ON
    -DDISABLE_DISCORD_RPC=ON
    -DDISABLE_GOOGLE_BENCHMARK=ON
    -DDISABLE_VERSION_CHECKER=ON
    -DENABLE_SCRIPTING=OFF
    -DPORTABLE=ON
    -DDISABLE_ICU=ON
    # OPENRCT2MINI: cut 38. Draw cursor as a sprite — the Miyoo Mini's
    # libmi_gfx framebuffer has no compositor / hardware cursor overlay.
    -DENABLE_SOFTWARE_CURSOR=ON
    -DOPENRCT2_USE_CCACHE=OFF
    # Skip install-time asset downloads — we ship them manually for the device.
    -DDOWNLOAD_TITLE_SEQUENCES=OFF
    -DDOWNLOAD_OBJECTS=OFF
    -DDOWNLOAD_OPENSFX=OFF
    -DDOWNLOAD_OPENMUSIC=OFF
)

# Run as the host user so build artefacts land with the user's UID,
# not root — otherwise rebuilds need sudo to delete the old build dir.
docker run --rm --user "$(id -u):$(id -g)" -v "$PROJECT_ROOT:/src" -w /src/build-arm "$IMAGE_TAG" \
    bash -c "cmake /src ${CMAKE_FLAGS[*]@Q} && make openrct2 -j\$(nproc)"

echo
if [ -x "$BUILD_DIR/openrct2" ]; then
    echo "Built: $BUILD_DIR/openrct2"
    file "$BUILD_DIR/openrct2"
    echo
    echo "ABI sanity:"
    docker run --rm -v "$PROJECT_ROOT:/src" "$IMAGE_TAG" \
        bash -lc 'arm-linux-gnueabihf-readelf -d /src/build-arm/openrct2 | grep -E "NEEDED|RPATH|RUNPATH"'
    echo
    echo "Required GLIBC symbol versions (must be ≤ 2.28 to load on device):"
    docker run --rm -v "$PROJECT_ROOT:/src" "$IMAGE_TAG" \
        bash -lc 'arm-linux-gnueabihf-readelf -V /src/build-arm/openrct2 | grep -oE "GLIBC_[0-9.]+" | sort -V -u | tail -5'

    # OPENRCT2MINI: cut 37. qemu-arm-static smoke test. Pass --skip-qemu (or set
    # the env var) to skip if the host can't run user-mode emulation.
    if [ "${SKIP_QEMU:-0}" = "1" ] || printf '%s\n' "$@" | grep -q -- '--skip-qemu'; then
        echo
        echo "Skipping qemu smoke test (--skip-qemu set)."
    else
        echo
        echo "==[ qemu-arm-static smoke test (--version) ]==================="
        # QEMU_LD_PREFIX makes the user-mode emulator find ld-linux-armhf.so.3
        # and the device's shared libs (libc, libpng, etc.) inside our sysroot.
        # We capture the exit code separately so we can report cleanly even if
        # qemu fails — this is the first signal that the binary is broken,
        # before we ever ship it to a device.
        set +e
        docker run --rm --user "$(id -u):$(id -g)" -v "$PROJECT_ROOT:/src" "$IMAGE_TAG" \
            bash -c '
                export QEMU_LD_PREFIX=/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc
                # 5-second wall-clock cap — --version should exit in <1s; if it
                # hangs that itself is the failure signal we want to surface.
                timeout 5 qemu-arm-static /src/build-arm/openrct2 --version
            '
        QEMU_RC=$?
        set -e
        if [ "$QEMU_RC" = "0" ]; then
            echo "  qemu smoke test: PASS"
        else
            echo "  qemu smoke test: FAIL (exit $QEMU_RC) — binary built but"
            echo "  doesn't run cleanly under user-mode emulation. Investigate"
            echo "  before copying to device."
        fi
    fi

    # OPENRCT2MINI: always package an SD-card-ready Onion port .7z so every
    # build is ready to ship. Pass --no-package to skip if you only want
    # the binary.
    if printf '%s\n' "$@" | grep -q -- '--no-package'; then
        echo
        echo "Skipping packaging (--no-package set)."
    else
        echo
        echo "==[ package Onion port .7z ]=================================="
        bash "$PROJECT_ROOT/Packaging/miyoo_mini/package.sh"
    fi
fi
