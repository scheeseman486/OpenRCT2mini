#!/bin/bash
# OpenRCT2mini Windows cross-build: build the dev libraries that the
# mingw-w64-x86-64-dev package doesn't ship.
#
# Pattern parallels Packaging/miyoo_mini/build-deps.sh — runs inside the
# Dockerfile during `docker build`, installs each dep into the cross
# sysroot at /opt/mingw-sysroot/ so CMake / pkg-config find them
# automatically when the toolchain file points there.
#
# Libraries built (all static except SDL2, which is built as a DLL we
# ship next to openrct2.exe so SDL2 itself can be updated independently):
#
#   - zlib 1.3.1          - SDL2 / libpng / libzip transitive
#   - libpng 1.6.43       - sprite paging + cursor rendering
#   - libzstd 1.5.5       - object pack compression
#   - libogg 1.3.5        - libvorbis dep
#   - libvorbis 1.3.7     - music streaming
#   - libflac 1.4.3       - FLAC audio
#   - freetype 2.13.3     - TTF rendering
#   - libzip 1.10.1       - .park / .sv6 zip wrapping
#   - nlohmann_json 3.11.3 (single header) - JSON parsing everywhere
#   - SDL2 2.30.10        - the whole UI/input/audio surface (shared)
#
# All from upstream tarballs. No sha256 verification yet (parity with
# Mini's build-deps.sh which hasn't pinned hashes either) — a follow-up
# can add the checksums.

set -euo pipefail

HOST_TRIPLE=x86_64-w64-mingw32
PREFIX=/opt/mingw-sysroot
BUILD_DIR=/tmp/build-deps
mkdir -p "$BUILD_DIR" "$PREFIX"
cd "$BUILD_DIR"

# Common cross-compile environment.
# Ubuntu jammy ships mingw-w64 binaries with the -posix / -win32 suffix.
# The unsuffixed names exist via update-alternatives and default to
# -posix on jammy. Be explicit anyway.
export CC="${HOST_TRIPLE}-gcc-posix"
export CXX="${HOST_TRIPLE}-g++-posix"
export AR="${HOST_TRIPLE}-ar"
export RANLIB="${HOST_TRIPLE}-ranlib"
export STRIP="${HOST_TRIPLE}-strip"
export DLLTOOL="${HOST_TRIPLE}-dlltool"
export WINDRES="${HOST_TRIPLE}-windres"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
# Generic optimisation. -O2 is the upstream baseline for desktop builds.
# We deliberately do NOT pass -fstack-protector-strong here: mingw-w64
# resolves __stack_chk_fail from libssp, which is not enabled by default
# in autotools/cmake link lines, and the deps' own configure scripts
# don't know to add -lssp. SDL2's libSDL2 link fails with
# "undefined reference to __stack_chk_fail" if the canary is enabled.
# The Mini build's build-deps.sh has the same issue with -fstack-
# protector-strong but works around it because the OnionUI toolchain
# pre-baked libssp into its sysroot. We don't have that luxury here.
# The actual openrct2.exe gets stack-canary protection from the engine
# CMake which links -fstack-protector-strong via the if(MINGW) branch
# in src/openrct2/CMakeLists.txt:181 (post-P1 edit).
COMMON_CFLAGS="-O2"
export CFLAGS="$COMMON_CFLAGS"
export CXXFLAGS="$COMMON_CFLAGS"

NPROC=$(nproc)

##############################################################################
# zlib 1.3.1 — required by libpng, libzip, SDL2-image (not used), etc.
##############################################################################
echo "==[ zlib ]====================================================="
ZLIB_VER=1.3.1
# www.zlib.net keeps only the very latest tarball, so URLs go 404 the
# moment upstream releases a new version. Use the GitHub release mirror
# which is stable. wget needs --content-disposition to follow the
# redirect-from-GitHub-release-to-Azure-blob chain and save with the
# expected filename.
wget --tries=3 --timeout=30 --no-verbose --content-disposition https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz
tar xf zlib-${ZLIB_VER}.tar.gz
pushd zlib-${ZLIB_VER}
# zlib's configure isn't autotools — it has a custom Win32/Makefile.gcc.
# Use that for static lib + DLL, install both, drop the DLL afterwards
# (we don't ship dynamic zlib).
make -f win32/Makefile.gcc PREFIX="${HOST_TRIPLE}-" SHARED_MODE=0 -j$NPROC libz.a
make -f win32/Makefile.gcc PREFIX="${HOST_TRIPLE}-" SHARED_MODE=0 \
    BINARY_PATH="$PREFIX/bin" INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" \
    install
# Hand-craft zlib.pc — zlib's Makefile.gcc doesn't emit one and pkg-config
# in libpng's configure expects it. Match the standard zlib.pc layout.
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
sharedlibdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: ${ZLIB_VER}

Requires:
Libs: -L\${libdir} -L\${sharedlibdir} -lz
Cflags: -I\${includedir}
EOF
mkdir -p "$PREFIX/lib/pkgconfig"
popd

##############################################################################
# libpng 1.6.43 — sprite paging + cursor rendering.
##############################################################################
echo "==[ libpng ]==================================================="
PNG_VER=1.6.43
wget --tries=3 --timeout=30 --no-verbose "https://downloads.sourceforge.net/libpng/libpng-${PNG_VER}.tar.gz"
tar xf libpng-${PNG_VER}.tar.gz
pushd libpng-${PNG_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" \
    --enable-static --disable-shared \
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib"
make -j$NPROC
make install
popd

##############################################################################
# libzstd 1.5.5 — object pack compression.
##############################################################################
echo "==[ libzstd ]=================================================="
ZSTD_VER=1.5.5
wget --tries=3 --timeout=30 --no-verbose https://github.com/facebook/zstd/releases/download/v${ZSTD_VER}/zstd-${ZSTD_VER}.tar.gz
tar xf zstd-${ZSTD_VER}.tar.gz
pushd zstd-${ZSTD_VER}/lib
make -j$NPROC libzstd.a libzstd.pc PREFIX="$PREFIX"
make install-static install-pc PREFIX="$PREFIX"
mkdir -p "$PREFIX/include"
cp zstd.h zdict.h zstd_errors.h "$PREFIX/include/"
popd

##############################################################################
# libogg 1.3.5 — base for vorbis + flac.
##############################################################################
echo "==[ libogg ]==================================================="
OGG_VER=1.3.5
wget --tries=3 --timeout=30 --no-verbose https://github.com/xiph/ogg/releases/download/v${OGG_VER}/libogg-${OGG_VER}.tar.gz
tar xf libogg-${OGG_VER}.tar.gz
pushd libogg-${OGG_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" --enable-static --disable-shared
make -j$NPROC
make install
popd

##############################################################################
# libvorbis 1.3.7 — music streaming. Depends on libogg.
##############################################################################
echo "==[ libvorbis ]================================================"
VORBIS_VER=1.3.7
wget --tries=3 --timeout=30 --no-verbose https://github.com/xiph/vorbis/releases/download/v${VORBIS_VER}/libvorbis-${VORBIS_VER}.tar.gz
tar xf libvorbis-${VORBIS_VER}.tar.gz
pushd libvorbis-${VORBIS_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" --enable-static --disable-shared \
    --with-ogg-libraries="$PREFIX/lib" --with-ogg-includes="$PREFIX/include"
make -j$NPROC
make install
popd

##############################################################################
# libflac 1.4.3 — FLAC audio. Depends on libogg.
##############################################################################
echo "==[ libflac ]=================================================="
FLAC_VER=1.4.3
wget --tries=3 --timeout=30 --no-verbose https://github.com/xiph/flac/releases/download/${FLAC_VER}/flac-${FLAC_VER}.tar.xz
tar xf flac-${FLAC_VER}.tar.xz
pushd flac-${FLAC_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" \
    --enable-static --disable-shared \
    --disable-cpplibs --disable-doxygen-docs --disable-examples \
    --disable-thorough-tests --disable-programs
make -j$NPROC
make install
popd

##############################################################################
# freetype 2.13.3 — TTF rendering. Disable harfbuzz / bzip2 / png subdeps
# (we don't need OpenType layout, just bitmap glyph rasterization for the
# in-game TTF support). zlib is left enabled because freetype's
# configure auto-detects our cross zlib.
##############################################################################
echo "==[ freetype ]================================================="
FREETYPE_VER=2.13.3
wget --tries=3 --timeout=30 --no-verbose https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VER}.tar.xz
tar xf freetype-${FREETYPE_VER}.tar.xz
pushd freetype-${FREETYPE_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" \
    --enable-static --disable-shared \
    --without-harfbuzz --without-bzip2 --without-png \
    --without-brotli
make -j$NPROC
make install
popd

##############################################################################
# libzip 1.10.1 — .park / .sv6 / scenario zip handling.
##############################################################################
echo "==[ libzip ]==================================================="
ZIP_VER=1.10.1
# libzip.org returns 200 but sometimes ETIMEDOUT under load. GitHub mirror
# is more reliable.
wget --tries=3 --timeout=30 --no-verbose --content-disposition https://github.com/nih-at/libzip/releases/download/v${ZIP_VER}/libzip-${ZIP_VER}.tar.gz
tar xf libzip-${ZIP_VER}.tar.gz
pushd libzip-${ZIP_VER}
mkdir build && cd build
cmake .. -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=${HOST_TRIPLE}-gcc-posix \
    -DCMAKE_RC_COMPILER=${HOST_TRIPLE}-windres \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX;/usr/${HOST_TRIPLE}" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_DOC=OFF -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
    -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_GNUTLS=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_COMMONCRYPTO=OFF \
    -DCMAKE_BUILD_TYPE=Release
ninja install
popd

##############################################################################
# nlohmann_json 3.11.3 — fetch both json.hpp + json_fwd.hpp.
# OpenRCT2's core/JsonFwd.hpp does `#include <nlohmann/json_fwd.hpp>` so the
# forward-decl-only header is mandatory. Both ship as release assets.
##############################################################################
echo "==[ nlohmann_json ]============================================"
NLOHMANN_VER=3.11.3
mkdir -p "$PREFIX/include/nlohmann"
wget --tries=3 --timeout=30 --no-verbose -O "$PREFIX/include/nlohmann/json.hpp" \
    "https://github.com/nlohmann/json/releases/download/v${NLOHMANN_VER}/json.hpp"
wget --tries=3 --timeout=30 --no-verbose -O "$PREFIX/include/nlohmann/json_fwd.hpp" \
    "https://github.com/nlohmann/json/releases/download/v${NLOHMANN_VER}/json_fwd.hpp"

##############################################################################
# SDL2 2.30.10 — built as a DLL so we ship SDL2.dll next to openrct2.exe.
##############################################################################
echo "==[ SDL2 ]====================================================="
SDL2_VER=2.30.10
wget --tries=3 --timeout=30 --no-verbose https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VER}/SDL2-${SDL2_VER}.tar.gz
tar xf SDL2-${SDL2_VER}.tar.gz
pushd SDL2-${SDL2_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" \
    --enable-shared --enable-static
make -j$NPROC
make install
popd

echo
echo "==[ build-deps complete ]======================================"
ls -la "$PREFIX/lib/pkgconfig/" || true
ls -la "$PREFIX/lib/libSDL2"*.{a,dll.a} 2>/dev/null || true
ls -la "$PREFIX/bin/SDL2.dll" 2>/dev/null || true
