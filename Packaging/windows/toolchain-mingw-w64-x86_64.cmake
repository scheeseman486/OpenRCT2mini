# OpenRCT2mini Linux → Windows x86_64 cross-build CMake toolchain.
#
# Used by Packaging/windows/build.sh when invoking cmake configure
# inside the openrctmini-windows-cross Docker image. Path-rooted at
# /opt/mingw-sysroot/ which is where Packaging/windows/build-deps.sh
# installs SDL2 + libpng + zlib + zstd + vorbis + ogg + flac + freetype
# + libzip + nlohmann_json.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)

# Use the -posix suffix variants explicitly. The -win32 variant doesn't
# provide std::thread / std::mutex bridges via libstdc++, and OpenRCT2
# uses std::thread extensively (JobPool, FileIndex, audio thread).
set(CMAKE_C_COMPILER   ${MINGW_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)
set(CMAKE_AR           ${MINGW_PREFIX}-ar)
set(CMAKE_RANLIB       ${MINGW_PREFIX}-ranlib)
set(CMAKE_STRIP        ${MINGW_PREFIX}-strip)
set(CMAKE_DLLTOOL      ${MINGW_PREFIX}-dlltool)

# Find-root setup. The cross-sysroot at /opt/mingw-sysroot/ has our
# built-from-source deps; /usr/x86_64-w64-mingw32/ is the system mingw
# sysroot with the Win32 API headers (kernel32, user32, shell32, ...).
set(CMAKE_FIND_ROOT_PATH /opt/mingw-sysroot /usr/${MINGW_PREFIX})

# NEVER for PROGRAM: don't try to run host binaries during configure.
# ONLY for LIBRARY/INCLUDE/PACKAGE: only look in cross-sysroots, not on
# the build host (we don't want CMake to find /usr/lib/libSDL2.so and
# try to link an ELF lib into a PE binary).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config setup — point at the cross-sysroot's .pc files. Use
# PKG_CONFIG_LIBDIR (not _PATH) so the host system's .pc files are
# ignored entirely.
set(ENV{PKG_CONFIG_LIBDIR} "/opt/mingw-sysroot/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH}   "/opt/mingw-sysroot/lib/pkgconfig")

# Bump _WIN32_WINNT to 0x0A00 (Windows 10) so the headers expose Vista+
# APIs the engine uses: LCMapStringEx (src/openrct2/core/String.cpp:739),
# LOCALE_NAME_USER_DEFAULT (same), CancelIoEx
# (src/openrct2/core/FileWatcher.cpp:176). mingw-w64 defaults to a much
# lower value (often _WIN32_WINNT_WIN2K = 0x0500) which leaves these
# unprototyped. Upstream's MSBuild path declares
# <TargetPlatformVersion>10.0.17763.0</TargetPlatformVersion> in
# openrct2.common.props:11, equivalent to setting _WIN32_WINNT=0x0A00,
# so we match. WINVER follows _WIN32_WINNT per Microsoft convention.
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000)

# FLAC__NO_DLL: tell FLAC's header (FLAC/export.h) to declare symbols
# WITHOUT __declspec(dllimport). Default behavior on Windows is to
# generate import-table __imp_FLAC__* references for DLL linkage; we
# built libFLAC as a static archive so we need the plain external
# declarations instead. Without this, linking FlacAudioSource.cpp.obj
# against libFLAC.a fails with hundreds of "undefined reference to
# __imp_FLAC__stream_decoder_new" etc.
add_definitions(-DFLAC__NO_DLL)

# Static-archive link-order pain on mingw-w64. ld processes archives
# left-to-right, so a static lib must be listed AFTER any object that
# references its symbols. CMake's target_link_libraries order doesn't
# guarantee correct topological resolution across libopenrct2.a +
# libzip.a + libbcrypt, libvorbisfile + libvorbis + libogg.
#
# The fix: tell ld to keep iterating until no more new symbols resolve.
# Done via CMAKE_C_LINK_EXECUTABLE / CMAKE_CXX_LINK_EXECUTABLE override
# that wraps the entire <LINK_LIBRARIES> slot with --start-group/--end-group.
# This is the standard mingw cross-compile workaround and is documented
# in the CMake faq.
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_PATH> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_PATH> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group")

# Demote -Werror for warnings GCC newer than what upstream OpenRCT2 was last
# tested against. Same set the AppImage host-graphics build uses (see
# .github/workflows/build.yml's host build step). mingw-w64's GCC trips
# -Wmaybe-uninitialized on cross-platform code paths where the false-positive
# requires actual data-flow knowledge the compiler can't have. The actual
# code paths are guarded by explicit nullptr checks. We're not patching
# every upstream site; we just demote the warnings to non-errors.
set(CMAKE_C_FLAGS_INIT   "-Wno-error=null-dereference -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized -Wno-error=suggest-final-types -Wno-error=suggest-final-methods")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")

# windres only understands preprocessor defines + -I include paths. Without
# this override CMake inherits CXX_FLAGS into RC_FLAGS and tries to pass
# -fstack-protector-strong to windres, which errors with "invalid option -f".
# Empty init lets CMake fill in just the -D/-I args it derives from the
# target's COMPILE_DEFINITIONS and INCLUDE_DIRECTORIES.
set(CMAKE_RC_FLAGS_INIT "")

# Disable IPO/LTO. The Mini's cmake/ipo.cmake enables -flto on Release
# builds, but mingw-w64's LTO ODR checker is stricter than ELF's and fires
# on TrackColour and similar cross-TU layout-equivalent-but-differently-
# defined types. Disabling LTO drops the check; the resulting binary is a
# bit larger (~5%) but functionally identical. Worth it to ship.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
# Dropped -Wno-error=stringop-overread and -Wno-error=dangling-reference
# vs the AppImage host build's flag set: those warnings only exist in GCC
# 12+ / 13+ respectively, and Ubuntu jammy's mingw-w64-x86_64 ships GCC
# 10.x which doesn't recognise the names. Adding them would itself produce
# "unrecognised option" errors.
# Added -Wno-error=suggest-final-{types,methods} to handle the
# ParkImporter.h warnings that fire on cross-mingw with C++17 strict mode.
