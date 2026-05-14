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
