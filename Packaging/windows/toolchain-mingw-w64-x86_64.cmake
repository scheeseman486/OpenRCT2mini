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

# Target Windows 7 SP1 (0x0601). The engine APIs that need a non-default
# _WIN32_WINNT are all Vista+ (so 0x0600 would be enough), but the Win7
# bump opens the door to anything Vista→Win7 the deps may reference
# without sacrificing compatibility. Specifically the engine uses:
#   - LOCALE_NAME_USER_DEFAULT — Vista+ (Platform.Win32.cpp:213/228/612/...)
#   - GetLocaleInfoEx — Vista+ (same file)
#   - CancelIoEx — Vista+ (FileWatcher.cpp:176)
#   - RtlGetVersion — XP+ but loaded dynamically via GetProcAddress
#
# Previous value was 0x0A00 (Windows 10), inherited from upstream OpenRCT2's
# MSBuild path which sets <TargetPlatformVersion>10.0.17763.0</TargetPlatform-
# Version> in openrct2.common.props. That cut off Win7/8 users for no benefit
# — the engine doesn't call any Win8+ exclusive APIs (verified via grep for
# GetSystemTimePreciseAsFileTime / SetThreadDescription / WaitOnAddress etc.).
# WINVER follows _WIN32_WINNT per Microsoft convention; NTDDI_VERSION uses
# the WIN7 build constant (0x06010000).
add_definitions(-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000)


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
# OPENRCT2MINI 2026-05-21: -march=x86-64 baseline so the binary runs on
# Sandy Bridge and older AMD CPUs without AVX/AVX2. GCC's mingw-w64 driver
# defaults to a CPU model slightly newer than baseline x86-64 (often
# -march=nocona) which is still pre-AVX, but explicit baseline future-
# proofs against driver default drift when GCC versions change.
set(CMAKE_C_FLAGS_INIT   "-march=x86-64 -Wno-error=null-dereference -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized -Wno-error=suggest-final-types -Wno-error=suggest-final-methods")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")

# windres only understands preprocessor defines + -I include paths. Without
# this override CMake inherits CXX_FLAGS into RC_FLAGS and tries to pass
# -fstack-protector-strong to windres, which errors with "invalid option -f".
# Empty init lets CMake fill in just the -D/-I args it derives from the
# target's COMPILE_DEFINITIONS and INCLUDE_DIRECTORIES.
set(CMAKE_RC_FLAGS_INIT "")

# Statically link libstdc++ + libgcc + libwinpthread.
#
# OPENRCT2MINI 2026-05-21: dynamic libstdc++-6.dll + MinGW-w64 SEH C++ excep-
# tions interact badly with Wine and with cross-DLL boundaries in general.
# Symptom: try/catch blocks silently fail to match. std::runtime_error thrown
# in Audio::CreateAudioSource ("Unsupported audio codec") is supposed to be
# caught by `catch (const std::exception& e)` in CreateStreamFromWAV one
# frame up; instead the unwinder walks straight through every handler and
# calls std::terminate → abort.
#
# Verified via winedbg --gdb: backtrace shows __cxa_throw →
# _Unwind_RaiseException → kernelbase abort, with the catch frame in
# CreateStreamFromWAV visible on the stack but never entered.
#
# Root cause: with dynamic libstdc++, type_info objects for std::exception
# and std::runtime_error exist BOTH in the EXE's data section AND in
# libstdc++-6.dll. The SEH personality function compares typeinfos by pointer
# first, name string second; under Wine the pointer compare fails and the
# name compare path isn't reached or doesn't resolve. Catch handlers go
# unmatched.
#
# Static linking collapses the type_info to a single instance per binary,
# giving the personality function a deterministic match. Also drops
# libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll, libssp-0.dll
# from the dist (smaller package, no MSVC/UCRT runtime mismatch surprises
# on Win7).
#
# We do NOT pass plain -static, which would also statically link SDL2 and
# every other dep. SDL2.dll stays shared on purpose so the dist can be
# repacked with a driver-specific SDL2 if needed.
#
# The --whole-archive + --no-whole-archive bracket around -lwinpthread
# ensures libwinpthread.a is fully embedded; without --whole-archive, ld
# might pick the import library (libwinpthread.dll.a) over the static
# archive and pull in the DLL anyway.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive -lssp -lssp_nonshared -Wl,-Bdynamic")

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
