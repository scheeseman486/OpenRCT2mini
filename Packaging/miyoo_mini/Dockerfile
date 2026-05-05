# OPENRCT2MINI: cut 36. Derived from the upstream OnionUI toolchain image —
# adds pkg-config (missing from upstream) and the dev libraries OpenRCT2
# needs that the upstream image doesn't ship: libzstd, libogg, libvorbis,
# libzip. zlib and libpng16 are already in the upstream sysroot. ICU is
# disabled at the cmake layer (cut 34); SDL2 is its own derived stage.
#
# This is a deliberately thin layer. It's not a "custom toolchain" — same
# GCC 8.3, same glibc 2.28, same SigmaStar SDK. Just a few standard deps
# pre-baked into the sysroot so first-run builds don't have to fetch.
#
# Build:  docker build -t openrctmini-toolchain Packaging/miyoo_mini/
# Use:    Packaging/miyoo_mini/build.sh (which docker-builds this if needed
#         and then docker-runs it for the actual openrct2 cross-build).

FROM ghcr.io/onionui/miyoomini-toolchain:latest

# Buster's apt sources are archived. Repoint at archive.debian.org.
RUN sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g; /buster-updates/d' \
        /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99no-check-valid-until && \
    apt-get -o Acquire::AllowInsecureRepositories=true update && \
    apt-get install -y --allow-unauthenticated \
        pkg-config wget tar git autoconf automake libtool \
        qemu-user-static && \
    rm -rf /var/lib/apt/lists/*

COPY build-deps.sh /opt/build-deps.sh
# Use a login shell so the toolchain's setup-env (PATH=/opt/miyoomini-toolchain/...)
# is sourced before the script runs.
RUN chmod +x /opt/build-deps.sh && bash -lc /opt/build-deps.sh

# pkg-config needs to know to look in the cross sysroot.
# PKG_CONFIG_LIBDIR alone is enough — our .pc files (zlib.pc, libpng16.pc,
# libzstd.pc, libzip.pc, sdl2.pc, ogg.pc, vorbisfile.pc) already contain
# absolute paths into the sysroot, so we don't want PKG_CONFIG_SYSROOT_DIR
# to prepend its value (that double-prefixes and breaks cmake).
ENV PKG_CONFIG_LIBDIR=/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc/usr/lib/pkgconfig

# OPENRCT2MINI: the upstream image puts arm-linux-gnueabihf-gcc on PATH only
# via /root/.bashrc (interactive login shells). When build.sh runs as the
# host user via docker run --user, that .bashrc doesn't load. Bake the PATH
# into the image's ENV so any user / shell sees it.
ENV PATH=/opt/miyoomini-toolchain/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

WORKDIR /src
