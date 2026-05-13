# OpenRCT2mini x86_64 Linux AppImage build image.
#
# Mirrors the symmetric pattern of Packaging/miyoo_mini/Dockerfile but for
# native x86_64 desktop, not ARM cross-compile. Ubuntu 22.04 base sets the
# glibc 2.35 floor.
#
# What's baked in:
#   - GCC + cmake + ninja + the dev libs OpenRCT2 needs on host
#     (SDL2, libpng, libzip, libzstd, libvorbis, libogg, libflac,
#     nlohmann_json, freetype, fontconfig, GL/GLU)
#   - linuxdeploy x86_64 AppImage at /opt/, chmod +x and ready to call
#   - libfuse2 so linuxdeploy --appimage-extract-and-run can untar itself
#     inside the container
#
# Build:  docker build -t openrctmini-appimage Packaging/appimage/
# Use:    Packaging/appimage/build.sh (which docker-builds this if needed
#         and then docker-runs it for the actual AppImage build).

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Build deps + AppImage runtime deps. Kept in one apt-get for layer-size
# efficiency. The list cross-references CMakeLists.txt find_package calls
# and what upstream's openrct2-build:24-jammy installs.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget file git pkg-config \
        cmake ninja-build g++ make \
        libsdl2-dev libpng-dev libzip-dev libzstd-dev \
        libvorbis-dev libogg-dev libflac-dev \
        nlohmann-json3-dev \
        libgl1-mesa-dev libglu1-mesa-dev \
        libfontconfig1-dev libfreetype-dev \
        libcurl4-openssl-dev \
        fuse libfuse2 zsync \
        python3 python3-pil \
        zip unzip xz-utils \
        patchelf \
    && rm -rf /var/lib/apt/lists/*

# Pin linuxdeploy to a specific continuous-tag release. Bump intentionally;
# do not silently track 'latest'. The continuous tag is a rolling release
# from TheAssassin/linuxdeploy — when bumping, copy a SHA-256 from the
# release assets page and add a verification step here.
ARG LINUXDEPLOY_VERSION=continuous
RUN curl -fLo /opt/linuxdeploy-x86_64.AppImage \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage" \
    && chmod +x /opt/linuxdeploy-x86_64.AppImage

WORKDIR /src
