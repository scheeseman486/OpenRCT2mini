# OPENRCT2MINI: CMake toolchain file for cross-compiling to the Miyoo Mini target.
#
# Invoked by build.sh inside the toolchain Docker container. Not meant for
# direct host use — the compiler and sysroot paths are container-local.
#
# Cribbed from DevilutionX's Packaging/miyoo_mini/toolchainfile.cmake and
# adapted to the OPENRCT2MINI plan §8.3 (static libstdc++, MinSizeRel, etc.).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_LINKER ${TOOLCHAIN_PREFIX}-ld)
set(CMAKE_AR ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB ${TOOLCHAIN_PREFIX}-ranlib)
set(CMAKE_STRIP ${TOOLCHAIN_PREFIX}-strip)

# Sysroot — derived inside the container, not from CMake's launch dir.
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-sysroot
    OUTPUT_VARIABLE TOOLCHAIN_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_SYSROOT ${TOOLCHAIN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${TOOLCHAIN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# OPENRCT2MINI: cut 36. Imported targets from find_package/pkg-config get their
# include dirs added via `-isystem` by default, which PROMOTES them to before
# libstdc++'s headers in the search list. zlib.pc's Cflags resolve to
# `-I${SYSROOT}/usr/include` (the libc include dir), and -isystem-promoting it
# breaks the libstdc++ trick where <cstdlib> does `#include_next <stdlib.h>`
# expecting to find the libc copy LATER in the search path. Use plain -I
# instead so the default search order survives.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED TRUE)

# Cortex-A7 with NEON-VFPv4. Per plan §8.1.
set(MIYOOMINI_FLAGS "-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve")
set(CMAKE_C_FLAGS_INIT   "${MIYOOMINI_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MIYOOMINI_FLAGS}")

# Static C++ runtime (plan §8.3) — one binary, no LD_LIBRARY_PATH games.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,--gc-sections")

# Default to MinSizeRel because text-segment size is paged from SD card.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()
