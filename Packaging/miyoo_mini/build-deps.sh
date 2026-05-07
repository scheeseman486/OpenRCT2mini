#!/bin/bash
# OPENRCT2MINI: cut 36. Build the dev libraries OpenRCT2 needs that the
# upstream OnionUI toolchain image doesn't ship: libzstd, libogg, libvorbis,
# libzip. zlib and libpng are already present in the sysroot. ICU is
# disabled at the cmake layer (cut 34). nlohmann_json is fetched as a
# single header.
#
# This runs inside the derived Docker image during `docker build`. It
# installs each dep into the toolchain's sysroot so cmake/pkg-config find
# them automatically, no extra search-path wiring needed.
#
# The cross-toolchain's flags are picked up from the toolchainfile.cmake
# and from CC/CXX exports. Each library is fetched, configured for the
# arm-linux-gnueabihf target, built, and installed.

set -euo pipefail

PREFIX=$(arm-linux-gnueabihf-gcc -print-sysroot)/usr
HOST_TRIPLE=arm-linux-gnueabihf
BUILD_DIR=/tmp/build-deps
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Common cross-compile environment.
export CC="${HOST_TRIPLE}-gcc"
export CXX="${HOST_TRIPLE}-g++"
export AR="${HOST_TRIPLE}-ar"
export RANLIB="${HOST_TRIPLE}-ranlib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# Cortex-A7 + NEON-VFPv4 — same flags the project uses (plan §8.1).
COMMON_CFLAGS="-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve -O2"
export CFLAGS="$COMMON_CFLAGS"
export CXXFLAGS="$COMMON_CFLAGS"

NPROC=$(nproc)

##############################################################################
# libzstd 1.5.5 — small, no autotools, just a Makefile.
##############################################################################
echo "==[ libzstd ]=================================================="
ZSTD_VER=1.5.5
wget -q https://github.com/facebook/zstd/releases/download/v${ZSTD_VER}/zstd-${ZSTD_VER}.tar.gz
tar xf zstd-${ZSTD_VER}.tar.gz
pushd zstd-${ZSTD_VER}/lib
# Build only the static library; we don't need the cli or shared.
make -j$NPROC libzstd.a libzstd.pc PREFIX="$PREFIX"
make install-static install-pc PREFIX="$PREFIX"
# Headers
mkdir -p "$PREFIX/include"
cp zstd.h zdict.h zstd_errors.h "$PREFIX/include/"
popd

##############################################################################
# libogg 1.3.5 — base for vorbis.
##############################################################################
echo "==[ libogg ]==================================================="
OGG_VER=1.3.5
wget -q https://github.com/xiph/ogg/releases/download/v${OGG_VER}/libogg-${OGG_VER}.tar.gz
tar xf libogg-${OGG_VER}.tar.gz
pushd libogg-${OGG_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" --enable-static --disable-shared
make -j$NPROC
make install
popd

##############################################################################
# libvorbis 1.3.7 — depends on libogg.
##############################################################################
echo "==[ libvorbis ]================================================"
VORBIS_VER=1.3.7
wget -q https://github.com/xiph/vorbis/releases/download/v${VORBIS_VER}/libvorbis-${VORBIS_VER}.tar.gz
tar xf libvorbis-${VORBIS_VER}.tar.gz
pushd libvorbis-${VORBIS_VER}
./configure --host=$HOST_TRIPLE --prefix="$PREFIX" --enable-static --disable-shared \
    --with-ogg-libraries="$PREFIX/lib" --with-ogg-includes="$PREFIX/include"
make -j$NPROC
make install
popd

##############################################################################
# libzip 1.10.1 — depends on zlib (already present) and libzstd (just built).
##############################################################################
echo "==[ libzip ]==================================================="
ZIP_VER=1.10.1
wget -q https://libzip.org/download/libzip-${ZIP_VER}.tar.gz
tar xf libzip-${ZIP_VER}.tar.gz
mkdir -p libzip-${ZIP_VER}/build
pushd libzip-${ZIP_VER}/build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_C_COMPILER=$CC \
    -DCMAKE_CXX_COMPILER=$CXX \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF \
    -DCMAKE_PREFIX_PATH="$PREFIX"
make -j$NPROC
make install
popd

##############################################################################
# nlohmann/json — single-header drop.
##############################################################################
echo "==[ nlohmann/json ]============================================"
NJ_VER=3.12.0
mkdir -p "$PREFIX/include/nlohmann"
wget -qO "$PREFIX/include/nlohmann/json.hpp" \
    "https://github.com/nlohmann/json/releases/download/v${NJ_VER}/json.hpp"
wget -qO "$PREFIX/include/nlohmann/json_fwd.hpp" \
    "https://github.com/nlohmann/json/releases/download/v${NJ_VER}/json_fwd.hpp"

##############################################################################
# Cleanup the build tree to keep the image small.
##############################################################################
rm -rf "$BUILD_DIR"

# OPENRCT2MINI: cut 36. The upstream toolchain ships both libpng12 and libpng16
# in the sysroot, with libpng.pc resolving to the older libpng12 (1.2.56). Our
# CMake requires libpng>=1.6, so re-point the bare libpng.pc at libpng16.pc.
echo "==[ libpng.pc → libpng16 ]====================================="
if [ -f "$PREFIX/lib/pkgconfig/libpng16.pc" ]; then
    cp "$PREFIX/lib/pkgconfig/libpng16.pc" "$PREFIX/lib/pkgconfig/libpng.pc"
    echo "libpng.pc now resolves to libpng16"
fi

# OPENRCT2MINI: cut 36. The upstream-shipped libpng.pc and zlib.pc use
# `prefix=/usr` (i.e. relative to the sysroot, not absolute). pkg-config
# normally fixes that with PKG_CONFIG_SYSROOT_DIR — but we don't set that,
# because the .pc files we BUILT (libzstd, libzip, sdl2, ogg, vorbis) already
# contain absolute sysroot paths and SYSROOT_DIR would double-prefix them.
# The simplest fix is to make the upstream .pc files match the convention:
# rewrite their `prefix=/usr` line to the absolute sysroot path.
echo "==[ rewrite libpng.pc / zlib.pc prefix → absolute ]============"
SYSROOT_USR="$PREFIX"
for pc in libpng.pc libpng12.pc libpng16.pc zlib.pc; do
    f="$PREFIX/lib/pkgconfig/$pc"
    if [ -f "$f" ]; then
        sed -i "s|^prefix=/usr$|prefix=$SYSROOT_USR|" "$f"
        echo "  patched $pc"
    fi
done

# OPENRCT2MINI: cut 36. Every .pc we built (libzstd, libzip, sdl2, ogg, vorbis)
# emits `-I${prefix}/include` in its Cflags, which resolves to the sysroot's
# /usr/include — already on gcc's default search path under --sysroot, so the
# -I is redundant. Worse: cmake's pkg_check_modules wraps include dirs as
# `-isystem`, which PROMOTES them to before libstdc++'s headers in the search
# order. That breaks libstdc++'s `cstdlib` -> `#include_next <stdlib.h>` trick
# (the next stdlib.h is supposed to be the libc one, searched AFTER
# libstdc++; if it's been promoted to before, #include_next fails). Per-pkg
# subdirs like /usr/include/libpng16 and /usr/include/SDL2 stay (they're not
# default paths). Just the bare /usr/include reference goes.
# Scrub of redundant -I${includedir} happens AFTER SDL2 build below — SDL2's
# `make install` would otherwise re-install a fresh sdl2.pc and clobber any
# in-place scrub we'd done here.

##############################################################################
# SDL2 — XK9274/sdl2_miyoo vendor port (Plan §8.4 / cut 40).
#
# Vanilla SDL2 has no driver for the SigmaStar libmi_gfx framebuffer the
# Miyoo Mini uses, so vanilla painted pixels never reach the panel (verified
# black screen on device, cut 39m). The XK9274 fork integrates libmi_gfx as
# an SDL2 video driver and the Miyoo's gpio buttons as joystick events.
#
# Build is dynamic-linked — we ship libSDL2-2.0.so.0 in the app's lib/ dir
# and the binary's RPATH=$ORIGIN/lib (cut 39f) picks it up. The fork
# dlopens libmi_gfx.so etc. from the device's /usr/lib at runtime.
#
# We only build the `sdl2/` subdir of the fork; their top-level Makefile
# also builds `swiftshader/` (OpenGL ES emulation), but we have OpenGL
# disabled in our cmake config so we don't need it.
##############################################################################
echo "==[ SDL2 (XK9274/sdl2_miyoo, libmi_gfx backend) ]==============="
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
# Fetch the fork as a tarball (smaller + faster than git clone of full
# history including swiftshader). Pin to main; revisit if upstream churn
# breaks our build.
echo "==[ SDL2 download (verbose so we see network failures) ]======="
wget --tries=3 --timeout=30 \
    https://github.com/XK9274/sdl2_miyoo/archive/refs/heads/main.tar.gz \
    -O sdl2_miyoo.tar.gz
ls -la sdl2_miyoo.tar.gz
echo "==[ SDL2 extract ]============================================="
tar xf sdl2_miyoo.tar.gz
ls -d sdl2_miyoo-main/sdl2 || { echo "extract produced unexpected layout"; ls; exit 1; }
cd sdl2_miyoo-main/sdl2

# OPENRCT2MINI: cut 40. Patch out gratuitous includes of optional SDL2
# sub-libraries (SDL_ttf, SDL_image) from SDL_video_mmiyoo.h. The fork
# headers reference them for a battery/wifi/splash overlay system that
# the .c sources never actually exercise (no TTF_* or IMG_* symbols
# anywhere in the mmiyoo video / render sources). Carrying them would
# force building SDL_ttf + SDL_image as prerequisites, which creates a
# circular dep against SDL2 itself.
sed -i \
    -e 's|^#include "SDL_ttf.h"|/* SDL_ttf.h removed by openrctmini cut 40 — unused */ #include "SDL_render.h"|' \
    -e 's|^#include "SDL_image.h"|/* SDL_image.h removed by openrctmini cut 40 — unused */|' \
    -e 's|^#include "SDL_mixer.h"|/* SDL_mixer.h removed by openrctmini cut 40 — unused */|' \
    -e 's|^#include "SDL2_gfxPrimitives.h"|/* SDL2_gfx removed by openrctmini cut 40 — unused */|' \
    -e 's|^#include "SDL2_rotozoom.h"|/* SDL2_rotozoom removed by openrctmini cut 40 — unused */|' \
    src/video/mmiyoo/SDL_video_mmiyoo.h
# OPENRCT2MINI: SDL_image.h indirectly pulled in SDL_render.h; we still
# need SDL_Texture declarations. Inject the direct include in place of
# the SDL_ttf.h substitution above (we replaced two lines of stripping
# with one strip + one re-add for SDL_render.h).
# Also catch the same pattern in the .c file in case any forgot to be in .h
sed -i \
    -e 's|^#include "SDL_ttf.h"|/* removed */|' \
    -e 's|^#include "SDL_image.h"|/* removed */|' \
    -e 's|^#include "SDL_mixer.h"|/* removed */|' \
    -e 's|^#include "SDL2_gfxPrimitives.h"|/* removed */|' \
    -e 's|^#include "SDL2_rotozoom.h"|/* removed */|' \
    src/video/mmiyoo/SDL_video_mmiyoo.c \
    src/render/mmiyoo/SDL_render_mmiyoo.c \
    2>/dev/null || true

# OPENRCT2MINI: cut 40. The fork's CheckMMiyooVideo branch in configure.ac
# adds `-lEGL -lGLESv2` to EXTRA_LDFLAGS. Those libs come from SwiftShader
# in the original repo (built by the fork's top-level Makefile alongside
# SDL2), but we have OpenGL disabled and OpenRCT2 uses software rendering,
# so we don't ship libGLESv2 / libEGL. Strip the link flags from configure.ac,
# then replace SDL_opengles_mmiyoo.c with a stub that doesn't reference any
# egl* symbols — otherwise the .so link still fails on undefined refs.
sed -i 's|EXTRA_LDFLAGS="$EXTRA_LDFLAGS -L\. -lEGL -lGLESv2"|EXTRA_LDFLAGS="$EXTRA_LDFLAGS -lmi_sys -lmi_gfx -lmi_common"|g' configure.ac
# OPENRCT2MINI: cut 40. CheckMMiyooAudio gates `-lmi_ao -lshmvar -lmi_common
# -lmi_sys -lmi_gfx` on `[ "$HOST" = arm-linux ]`, but $HOST is a shell-env
# variable that the fork's top-level Makefile exports before running
# configure (`export HOST=arm-linux`). We invoke configure directly without
# that export — `--host=` sets autoconf's host_triplet but not the literal
# $HOST shell variable — so the gate falls through and the audio LDFLAGS
# never fire. Export HOST explicitly so the gate succeeds. Use `arm-linux`
# to match the original gate (the suffix doesn't actually matter to the
# resulting link).
export HOST=arm-linux
# OPENRCT2MINI: cut 40c. The vendor SDL2 mmiyoo render driver advertises
# its supported pixel formats as `[0] = RGB565, [2] = ARGB8888`. The `[2]`
# is a typo for `[1]` — with num_texture_formats=2, SDL iterates indices
# 0–1, finds `{RGB565, 0}`, and OpenRCT2's renderer picks the smallest
# bytes-per-pixel: SDL_PIXELFORMAT_UNKNOWN (=0, bpp=0) beats RGB565 (bpp=2).
# That zero gets passed to SDL_CreateTexture which rejects it. Patch the
# designator so slot 1 is ARGB8888.
sed -i 's|\[2\] *= *SDL_PIXELFORMAT_ARGB8888|[1] = SDL_PIXELFORMAT_ARGB8888|' \
    src/render/mmiyoo/SDL_render_mmiyoo.c

# OPENRCT2MINI: cut 43. The mmiyoo render driver's MMIYOO_QueueCopy
# unconditionally drops 32×32 blits because the fork's intended use is
# emulators where 32×32 is the system mouse cursor that gets handled
# via a different overlay path. We do NOT want that — our software
# cursor blits a 32×32 ARGB texture every frame and needs the driver
# to actually push it. Comment the early-return so all blit sizes go
# through the threaded path.
# OPENRCT2MINI cuts 43 and 43b reverted. The mmiyoo driver blits exactly
# ONE texture per frame (the most recent SDL_RenderCopy stashes into
# gfx.thread[0], MMIYOO_RenderPresent flips it once). Trying to blit a
# cursor texture as a SECOND RenderCopy just overwrites slot 0 — the
# screen texture is silently dropped, so cursor movement smears over
# black. The fork's hardcoded "skip 32×32" was a hack to avoid that.
# We composite the cursor INTO the X8 framebuffer instead (cut 43c),
# so OpenRCT2 still issues exactly one screen-texture RenderCopy per
# frame and the driver is happy.

# OPENRCT2MINI: cut 43. The mmiyoo input thread (SDL_event_mmiyoo.c) reads
# /dev/input/event0 directly into a private bitmap (`evt.keypad.bitmaps`)
# and never pushes anything to SDL's event queue — so SDL_PollEvent never
# sees keyboard or joystick events from the device's buttons. Patch
# set_key() to ALSO emit SDL keyboard events so cut 38b's keyboard
# cursor handling (arrows/Z/X) reacts to the D-pad and face buttons.
# OPENRCT2MINI: cut 55. The Miyoo Mini's audio SoC only accepts 48 kHz —
# anything else fails MI_AO_SetPubAttr with error 0xa0052009 (AO err 9 =
# NOT_PERM). Earlier cuts (54/54b) tried to keep the application feeding
# 22050 and used MI_AO_EnableReSmp for HW resampling; the SoC rejected
# that path on this firmware. Cut 55: stop fighting it. Force SDL's
# negotiated spec.freq to 48000 at OpenDevice entry — SDL then reports
# `have.freq=48000` to the app, and OpenRCT2's AudioMixer (which already
# resamples each source to _outputFormat.freq via SDL_BuildAudioCVT) does
# the work. The driver hands the SoC native-rate 48000 PCM and never
# touches MI_AO_EnableReSmp.
#
# Cut 56: device starts disabled (confirmed by kernel log "Dev0 has not
# been enabled"), so cut 54b's pre-disable was a misdiagnosis. The real
# NOT_PERM cause is most likely persisted attribute state — clear it
# explicitly with MI_AO_ClrPubAttr, plus clamp u32PtNumPerFrm/u32FrmNum
# to safer values, and dump the struct fields for further diagnosis.
python3 - <<'PYEOF_AUDIO'
import re, pathlib
p = pathlib.Path('src/audio/mmiyoo/SDL_audio_mmiyoo.c')
src = p.read_text()

# 1) Force spec.freq = 48000 BEFORE the mixbuf size is computed from it
#    and before MI_AO config touches it. Anchor right before the first
#    executable statement in OpenDevice (this->hidden = SDL_malloc...).
#    Inserting at the function head would violate C90's
#    declaration-after-statement rule (the function has -Werror).
old_open_stmt = ('    this->hidden = (struct SDL_PrivateAudioData *)SDL_malloc((sizeof * this->hidden));')
new_open_stmt = ('#ifdef MMIYOO\n'
                 '    /* OPENRCT2MINI cut 55: SoC is fixed at 48 kHz. Override SDL\'s\n'
                 '       negotiated rate before we compute buffer sizes or configure\n'
                 '       MI_AO. SDL reports `have.freq` back to the app from this\n'
                 '       same `this->spec.freq` field, so the app sees 48000 and\n'
                 '       (with SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) resamples sources\n'
                 '       directly to 48000. */\n'
                 '    this->spec.freq = 48000;\n'
                 '#endif\n'
                 '    this->hidden = (struct SDL_PrivateAudioData *)SDL_malloc((sizeof * this->hidden));')
if old_open_stmt not in src:
    raise SystemExit('cut 55 patch: could not find OpenDevice first statement')
src = src.replace(old_open_stmt, new_open_stmt, 1)

# 2) cut 57: cuts 54b/56's pre-SetPubAttr fiddling (Disable, ClrPubAttr,
#    clamps) all reverted — they failed independently and addressed the
#    wrong layer. The real cause of NOT_PERM is something else holding the
#    AO device (an audio daemon / LD_PRELOAD shim — see launch.sh cut 57).
#    No SDL-side workaround can win against that. Leave SetPubAttr alone.

# 3) Use the explicit MI enum constant instead of the int cast — spec.freq
#    is now guaranteed to be 48000 by step 1, but the cast assumes the
#    enum value equals the integer rate. The enum constant makes the
#    intent explicit and matches what the SoC actually accepts.
old_rate = 'stSetAttr.eSamplerate = (MI_AUDIO_SampleRate_e)this->spec.freq;'
new_rate = ('stSetAttr.eSamplerate = E_MI_AUDIO_SAMPLE_RATE_48000;'
            ' /* cut 55: SoC native; only 48 kHz is accepted */')
if old_rate not in src:
    raise SystemExit('cut 55 patch: did not find SetPubAttr eSamplerate line')
src = src.replace(old_rate, new_rate, 1)

p.write_text(src)
print('cut 57: patched audio driver — spec.freq=48000 only (SDL stuff reverted)')
PYEOF_AUDIO

python3 - <<'PYEOF'
import re, pathlib
p = pathlib.Path('src/video/mmiyoo/SDL_event_mmiyoo.c')
src = p.read_text()
# SDL_events_c.h already pulls in SDL_keyboard_c.h which declares
# SDL_SendKeyboardKey, so no extra include needed.
old = ('static void set_key(uint32_t bit, int val)\n'
       '{\n'
       '    if (val) {\n'
       '        evt.keypad.bitmaps|= (1 << bit);\n'
       '    }\n'
       '    else {\n'
       '        evt.keypad.bitmaps&= ~(1 << bit);\n'
       '    }\n'
       '}\n')
new = ('static void set_key(uint32_t bit, int val)\n'
       '{\n'
       '    int prev = (evt.keypad.bitmaps & (1 << bit)) ? 1 : 0;\n'
       # OPENRCT2MINI cut 60: function-static tracking which scancode L1
       # actually emitted on press, so the release matches even if R1
       # toggled in between (the chord swap may have already swapped it).
       '    static SDL_Scancode l1_emitted_sc = SDL_SCANCODE_UNKNOWN;\n'
       '    if (val) {\n'
       '        evt.keypad.bitmaps |= (1 << bit);\n'
       '    }\n'
       '    else {\n'
       '        evt.keypad.bitmaps &= ~(1 << bit);\n'
       '    }\n'
       '    /* OPENRCT2MINI cut 43: also push SDL keyboard event so the\n'
       '       app sees button presses. cut 38b\'s keyboard cursor\n'
       '       handling reacts to arrows/Z/X. */\n'
       '    if (val != prev) {\n'
       '        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;\n'
       '        switch (bit) {\n'
       '            case MYKEY_UP:     sc = SDL_SCANCODE_UP;     break;\n'
       '            case MYKEY_DOWN:   sc = SDL_SCANCODE_DOWN;   break;\n'
       '            case MYKEY_LEFT:   sc = SDL_SCANCODE_LEFT;   break;\n'
       '            case MYKEY_RIGHT:  sc = SDL_SCANCODE_RIGHT;  break;\n'
       '            case MYKEY_A:      sc = SDL_SCANCODE_Z;      break;\n'
       '            case MYKEY_B:      sc = SDL_SCANCODE_X;      break;\n'
       # OPENRCT2MINI W0: face X/Y, L2/R2 onto WASD-cluster letters
       # (was F14-F17 in cut 59). Reverts to PC-keyboard-friendly keys
       # so devs can test all device controls on host without F13-F17.
       # UiContext intercepts these letter keys for special handling and
       # checks hasTextInputFocus() to avoid colliding with text entry.
       #   C = face X = window drag (was: cycle game speed)
       #   V = face Y = rotate construction object (3x = CCW with R1)
       #   W = L2     = rotate view CCW (zoom out with R1)
       #   S = R2     = rotate view CW  (zoom in  with R1)
       '            case MYKEY_X:      sc = SDL_SCANCODE_C;      break;\n'
       '            case MYKEY_Y:      sc = SDL_SCANCODE_V;      break;\n'
       '            case MYKEY_L2:     sc = SDL_SCANCODE_W;      break;\n'
       '            case MYKEY_R2:     sc = SDL_SCANCODE_S;      break;\n'
       # OPENRCT2MINI cut 58: R1 -> F13 (was RSHIFT). RSHIFT made the
       # cursor go fast AND set SDL_GetModState() to KMOD_SHIFT.
       # cut 59: L1 stays on LSHIFT (Shift modifier reach) but no longer
       # activates fast cursor in UiContext -- only R1 does.
       # cut 60: L1+R1 chord = LCTRL (Ctrl modifier) for OpenRCT2's
       # construction Z-lock semantics. When R1 transitions while L1 is
       # held we re-emit the L1 modifier so the SDL keyboard state
       # matches the new chord state. UiContext sees LCTRL and disables
       # fast cursor while the chord is engaged.
       # cut 61: R1 emits LALT (was F13). Alt is on every PC keyboard so
       # the dev can test fast-cursor / gamepad-mod behavior natively.
       # OpenRCT2's only Alt bindings are ALT+RETURN (windowed toggle)
       # and CTRL+ALT+C (cheats), neither matters on the device. The
       # UiContext intercept clears KMOD_LALT/RALT from SDL's mod state
       # after latching so R1+Start doesn't accidentally fire ALT+RETURN.
       # OPENRCT2MINI W0: dual-emit. L1 sends Q + LSHIFT (or LCTRL via
       # chord); R1 sends A + LALT. The letter is the user-facing test
       # key (visible in shortcut bindings, accessible on PC keyboard);
       # the modifier drives OpenRCT2's existing Shift/Alt/Ctrl modifier
       # semantics (raise placement Z, gamepad mod, construction Z-lock).
       # Q and A continue held alongside the modifier swap during chord.
       '            case MYKEY_R1:\n'
       '                /* dual-emit: A for testability */\n'
       '                SDL_SendKeyboardKey(val ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_A);\n'
       '                sc = SDL_SCANCODE_LALT;\n'
       '                if (evt.keypad.bitmaps & (1 << MYKEY_L1)) {\n'
       '                    if (val) {\n'
       '                        SDL_SendKeyboardKey(SDL_RELEASED, SDL_SCANCODE_LSHIFT);\n'
       '                        SDL_SendKeyboardKey(SDL_PRESSED,  SDL_SCANCODE_LCTRL);\n'
       '                        l1_emitted_sc = SDL_SCANCODE_LCTRL;\n'
       '                    } else {\n'
       '                        SDL_SendKeyboardKey(SDL_RELEASED, SDL_SCANCODE_LCTRL);\n'
       '                        SDL_SendKeyboardKey(SDL_PRESSED,  SDL_SCANCODE_LSHIFT);\n'
       '                        l1_emitted_sc = SDL_SCANCODE_LSHIFT;\n'
       '                    }\n'
       '                }\n'
       '                break;\n'
       '            case MYKEY_L1:\n'
       '                /* dual-emit: Q for testability */\n'
       '                SDL_SendKeyboardKey(val ? SDL_PRESSED : SDL_RELEASED, SDL_SCANCODE_Q);\n'
       '                if (val) {\n'
       '                    l1_emitted_sc = (evt.keypad.bitmaps & (1 << MYKEY_R1))\n'
       '                                        ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_LSHIFT;\n'
       '                    sc = l1_emitted_sc;\n'
       '                } else {\n'
       '                    sc = (l1_emitted_sc != SDL_SCANCODE_UNKNOWN)\n'
       '                             ? l1_emitted_sc : SDL_SCANCODE_LSHIFT;\n'
       '                    l1_emitted_sc = SDL_SCANCODE_UNKNOWN;\n'
       '                }\n'
       '                break;\n'
       '            case MYKEY_START:  sc = SDL_SCANCODE_RETURN; break;\n'
       '            case MYKEY_SELECT: sc = SDL_SCANCODE_ESCAPE; break;\n'
       '        }\n'
       '        if (sc != SDL_SCANCODE_UNKNOWN) {\n'
       '            SDL_SendKeyboardKey(val ? SDL_PRESSED : SDL_RELEASED, sc);\n'
       '        }\n'
       '    }\n'
       '}\n')
if old not in src:
    raise SystemExit('cut 43 patch: set_key body did not match expected text')
src = src.replace(old, new, 1)

# OPENRCT2MINI cut 59: disable the duplicate code[]-loop in MMIYOO_PumpEvents.
# Pre-cut 43, this loop was the ONLY source of SDL keyboard events for the
# device's buttons. Cut 43 made set_key() emit events directly with the
# scancodes we actually want (cursor arrows, Z/X for click, F13-F17 for
# gamepad-modifier-aware actions). The loop kept emitting the original
# upstream-fork scancodes alongside (SPACE / LCTRL / LSHIFT / LALT / e / t /
# TAB / BACKSPACE / RCTRL / HOME / 0 / 1 / 2 / 3) so every face-button press
# produced TWO keyboard events with different keys. That's why face X also
# fired LSHIFT (= shift modifier on every X press), R2 fired BACKSPACE (=
# close top window), L2 fired TAB (= show map). Replace the loop with a
# no-op; pre_keypad_bitmaps still gets updated below (and below the TRIMUI
# block) for the change-detection in the next pump tick.
old_loop = ('            for (cc=0; cc<=MYKEY_LAST_BITS; cc++) {\n'
            '                bit = 1 << cc;\n'
            '                if (changed & bit) {\n'
            '                    SDL_SendKeyboardKey((evt.keypad.bitmaps & bit) ? SDL_PRESSED : SDL_RELEASED, SDL_GetScancodeFromKey(code[cc]));\n'
            '                }\n'
            '            }\n')
new_loop = ('            /* OPENRCT2MINI cut 59: code[]-loop disabled. set_key()\n'
            '               (cut 43) now emits SDL keyboard events directly with\n'
            '               the scancodes we want; this loop only created\n'
            '               duplicates with the wrong scancodes (face X firing\n'
            '               LSHIFT, R2 firing BACKSPACE, L2 firing TAB, etc.).\n'
            '               cc / bit / changed are still declared above; mark\n'
            '               them as used so -Werror=unused-variable stays happy. */\n'
            '            (void)cc; (void)bit; (void)changed;\n')
if old_loop not in src:
    raise SystemExit('cut 59 patch: PumpEvents code[]-loop did not match expected text')
src = src.replace(old_loop, new_loop, 1)

p.write_text(src)
print('cut 43+59: patched set_key + disabled PumpEvents code[] duplicate loop')
PYEOF
# OPENRCT2MINI: cut 40f reverted — the existing E_MI_GFX_ROTATE_180 IS the
# correct setting. User clarified the magenta probe landed top-left (where
# we wrote it), confirming both rotation and orientation are right. The
# panel's physical mount must apply a complementary 180° relative to the
# framebuffer, so the driver's hard-coded ROTATE_180 is what makes top-of-
# texture appear top-of-panel. Keeping the vendor source untouched here.

cat > src/video/mmiyoo/SDL_opengles_mmiyoo.c <<'EGL_EOF'
/* OPENRCT2MINI cut 40: stub replacement for the EGL-backed GL context
   wrapper. The original referenced SwiftShader's libEGL/libGLESv2 to
   composite the Miyoo's framebuffer; we don't ship those libs and don't
   use GL (OpenRCT2 uses software rendering). All public functions return
   no-op / failure so the wired-up device->GL_* table never crashes if
   accidentally called. */
#include "../../SDL_internal.h"
#if SDL_VIDEO_DRIVER_MMIYOO
#include "SDL_video_mmiyoo.h"
#include "SDL_opengles_mmiyoo.h"

int glLoadLibrary(_THIS, const char *name) { return 0; }
void *glGetProcAddress(_THIS, const char *proc) { return NULL; }
void glUnloadLibrary(_THIS) {}
SDL_GLContext glCreateContext(_THIS, SDL_Window *window) { return NULL; }
int glSetSwapInterval(_THIS, int interval) { return 0; }
int glUpdateBufferSettings(void *pFunc, void *fb_idx, void *fb_vaddr) { return 0; }
int glSwapWindow(_THIS, SDL_Window *window) { return 0; }
int glMakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context) { return 0; }
void glDeleteContext(_THIS, SDL_GLContext context) {}

#endif
EGL_EOF

# OPENRCT2MINI: cut 40. SDL_revision.h is generated from git revision info
# by build-scripts/updaterev.sh, which expects to be inside a git work
# tree. Our tarball extract doesn't have .git, so updaterev fails and the
# build hits "SDL_revision.h: No such file or directory". Generate a stub
# directly — the file is just two #define lines.
mkdir -p include
cat > include/SDL_revision.h <<'REV_EOF'
/* OPENRCT2MINI cut 40: stub generated outside the fork's updaterev.sh
   path because we extracted from a tarball, no .git. */
#define SDL_REVISION "openrctmini-vendor-sdl2"
#define SDL_REVISION_NUMBER 0
REV_EOF

# autogen.sh is ./build-scripts/autogen which regenerates configure from
# configure.ac. The fork ships a pre-generated configure too, but we run
# autogen to be safe in case the in-tree configure is stale.
if [ -x ./autogen.sh ]; then
    ./autogen.sh
fi

# MOD=mmiyoo is the fork's switch for the Miyoo Mini video driver. Without
# it, the fork still builds but only as a vanilla framebuffer build (which
# is what we already had).
# --host: the device's triple. The fork's Makefile uses arm-linux but our
# toolchain is arm-linux-gnueabihf — pass that so configure finds our
# cross-prefixed binaries on PATH.
MOD=mmiyoo ./configure \
    --host=$HOST_TRIPLE \
    --prefix="$PREFIX" \
    --disable-static --enable-shared \
    --disable-joystick-virtual --disable-jack --disable-alsa --disable-sensor \
    --disable-video-wayland --disable-video-vulkan \
    --disable-power --disable-fcitx --disable-hidapi --disable-sndio \
    --disable-libudev --disable-diskaudio --disable-video-x11 --disable-pulseaudio \
    --disable-video-opengles2 --disable-video-opengles \
    --disable-ime --disable-video-kmsdrm --disable-oss --disable-video-opengl \
    --disable-dbus --disable-video-dummy --disable-dummyaudio
make -j$NPROC
make install
# The vendor port's `make install` may not produce the unversioned symlink
# `libSDL2.so` -> `libSDL2-2.0.so.0`. Some linkers want it at link time;
# create it explicitly.
if [ ! -e "$PREFIX/lib/libSDL2.so" ] && [ -e "$PREFIX/lib/libSDL2-2.0.so.0" ]; then
    ln -sf libSDL2-2.0.so.0 "$PREFIX/lib/libSDL2.so"
fi

##############################################################################
# OPENRCT2MINI: cut 36. Final pkg-config Cflags scrub.
#
# Every .pc that emits `-I${includedir}` (where includedir is the sysroot's
# bare /usr/include) is redundant — that path is already on gcc's default
# search list under --sysroot. Worse, cmake's pkg_check_modules wraps it as
# `-isystem`, which PROMOTES it to before libstdc++'s headers in the search
# order. That breaks libstdc++'s `cstdlib` -> `#include_next <stdlib.h>`
# trick — `#include_next` is supposed to find the libc stdlib.h LATER than
# the C++ headers, but if -isystem has promoted /usr/include to BEFORE the
# C++ headers, it can't.
#
# Strip `-I${includedir}` from the Cflags of every .pc that emits it.
# Per-pkg subdirs (libpng16, SDL2) stay — those aren't default paths.
##############################################################################
echo "==[ scrub redundant -I\${includedir} from all .pc Cflags ]====="
# Bare-includedir packages — strip Cflags entirely.
for pc in zlib libzstd libzip ogg vorbis vorbisfile; do
    f="$PREFIX/lib/pkgconfig/$pc.pc"
    [ -f "$f" ] || continue
    sed -i 's|^Cflags: *-I\${includedir} *$|Cflags:|' "$f"
    sed -i 's|^Cflags: *-I\${includedir}\b *|Cflags: |' "$f"
done
# sdl2.pc emits `-I${includedir} -I${includedir}/SDL2 ... -D_REENTRANT` —
# drop the bare-includedir, keep the SDL2 subdir.
sf="$PREFIX/lib/pkgconfig/sdl2.pc"
if [ -f "$sf" ]; then
    sed -i 's|-I\${includedir} \(-I\${includedir}/SDL2\)|\1|' "$sf"
fi
echo "  done. Verifying:"
for pc in zlib libpng libzstd libzip ogg vorbisfile sdl2; do
    f="$PREFIX/lib/pkgconfig/$pc.pc"
    [ -f "$f" ] && echo "    $pc.pc Cflags: $(grep -E '^Cflags:' "$f" | head -1)"
done

echo "==[ done ]====================================================="
ls -la "$PREFIX/lib/pkgconfig/"
