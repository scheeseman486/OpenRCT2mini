# OpenRCT2mini Linux → Windows x86_64 cross-build image.
#
# Symmetric with Packaging/miyoo_mini/Dockerfile (ARM cross-build) and
# Packaging/appimage/Dockerfile (native AppImage). Base: ubuntu:22.04 for
# stable mingw-w64 + binutils versions and a glibc the GHA runners have.
#
# What's baked in:
#   - mingw-w64 13.x cross-toolchain (POSIX threads variant — required
#     for libstdc++'s std::thread / std::mutex / std::condition_variable
#     wiring; the win32-threads variant is missing those).
#   - The dev libraries OpenRCT2 needs, cross-built from upstream tarballs
#     and installed into /opt/mingw-sysroot/: SDL2 (shared), libpng,
#     zlib, libzstd, libzip, libogg, libvorbis, libflac, freetype,
#     nlohmann_json single-header.
#   - CMake 3.22+ (jammy default), Ninja, pkg-config configured to look
#     in the cross-sysroot.
#
# Build:  docker build -t openrctmini-windows-cross Packaging/windows/
# Use:    Packaging/windows/build.sh

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Base build deps + mingw cross-toolchain.
# gcc-mingw-w64-x86-64-posix selects the POSIX threads runtime
# (winpthreads). Without -posix you get win32-threads mingw which
# doesn't supply std::thread / std::mutex bridges via libstdc++.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget file git pkg-config \
        cmake ninja-build make autoconf automake libtool \
        gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix \
        binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev \
        zip unzip xz-utils tar \
        python3 python3-pil \
    && rm -rf /var/lib/apt/lists/*

# Cross-build the dev deps that mingw-w64-x86-64-dev doesn't ship
# (SDL2, png, zlib, zstd, vorbis, ogg, flac, freetype, libzip,
# nlohmann_json). Goes into /opt/mingw-sysroot/ which the CMake
# toolchain file then sets as CMAKE_FIND_ROOT_PATH.
#
# Reproducibility: each dep is pinned to an upstream version + sha256
# in build-deps.sh, same pattern as Packaging/miyoo_mini/build-deps.sh.
COPY build-deps.sh /opt/build-deps.sh
RUN chmod +x /opt/build-deps.sh && bash /opt/build-deps.sh

# pkg-config needs to know where the cross-sysroot's .pc files live.
# The cross-sysroot ships .pc files with absolute paths into
# /opt/mingw-sysroot/, so we do NOT set PKG_CONFIG_SYSROOT_DIR (which
# would double-prefix and break cmake).
ENV PKG_CONFIG_LIBDIR=/opt/mingw-sysroot/lib/pkgconfig
ENV PATH=/opt/mingw-sysroot/bin:$PATH

WORKDIR /src
