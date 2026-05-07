#!/bin/bash
# OPENRCT2MINI: revision 63. Build an SD-card-ready OnionUI **port** package.
#
# Run from the project root (after build.sh has produced build-arm/openrct2):
#   ./Packaging/miyoo_mini/package.sh
#
# Output: dist/OpenRCT2mini-<version>-<git-short>.7z containing
#
#   Roms/PORTS/
#     Games/OpenRCT2mini/
#       openrct2        (stripped ARM binary)
#       launch.sh       (the actual launch script — stays here so all
#                        port files live in one folder)
#       lib/            (vendor libpng16 / libz / SDL2)
#       data/           (OpenRCT2 stock data + supplemental asset packs)
#       save/           (default config.ini)
#       INSTALL.txt
#     Imgs/
#       OpenRCT2mini.png    (250x376 portrait box art)
#     Shortcuts/Simulation/
#       OpenRCT2mini.port   (Onion port shortcut — execs launch.sh)
#
# Distributed as 7z so it merges cleanly into Onion's port-collection
# convention (extract at SD-card root). Game Library detection: the
# .port shortcut declares GameDataFile so Onion's import-ports script
# hides the entry until the user has supplied their RCT2 install.
#
# Pass --no-strip to keep debug info (24 → 26 MB unstripped, ~6 MB stripped).
# Pass --no-assets to skip the supplemental-pack download.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-arm"
DIST_DIR="$PROJECT_ROOT/dist"
ASSET_CACHE="$DIST_DIR/asset-cache"
IMAGE_TAG="${OPENRCT2MINI_TOOLCHAIN_IMAGE:-openrctmini-toolchain:latest}"

if [ ! -x "$BUILD_DIR/openrct2" ]; then
    echo "ERROR: $BUILD_DIR/openrct2 not found — run ./Packaging/miyoo_mini/build.sh first." >&2
    exit 1
fi

STRIP=1
# OPENRCT2MINI: bundle OpenRCT2's supplemental asset packs by default.
# Each pack's license (CC-BY-4.0 for title-sequences and objects; MIT for
# OpenSFX; CC-BY-SA-4.0 for OpenMusic) permits redistribution as long as
# attribution is preserved — see THIRD_PARTY_NOTICES.md. Pass --no-assets
# to skip the embed (smaller archive; the user then has to fetch the packs
# from <https://github.com/OpenRCT2/OpenRCT2/releases>).
BUNDLE_ASSETS=1
# OPENRCT2MINI revision 64: --debug makes package.sh drop a `.debug`
# sentinel in the staged install. launch.sh checks for that sentinel on
# the device and only redirects stderr to log/run.log when present.
# Release packages get no sentinel → no log file.
PACKAGE_DEBUG=0
for arg in "$@"; do
    if [ "$arg" = "--no-strip" ]; then STRIP=0; fi
    if [ "$arg" = "--no-assets" ]; then BUNDLE_ASSETS=0; fi
    if [ "$arg" = "--debug" ]; then PACKAGE_DEBUG=1; fi
done

##############################################################################
# OPENRCT2MINI: cut 39b. OpenRCT2 normally fetches title sequences, the object
# pack, OpenSFX and OpenMusic at install time (cmake DOWNLOAD_* steps) or at
# first run via HTTP. We disabled both paths for the device build, so the
# package script is the only place these can come from. Fetch from the URLs
# pinned in assets.json (same source the upstream cmake uses), verify SHA256,
# cache in dist/asset-cache/ so re-packaging is cheap.
#
# Layout in the staged tree:
#   data/sequence/       <- title-sequences.zip
#   data/object/         <- objects.zip
#   data/assetpack/      <- opensound.zip and openmusic.zip both contain
#                            this subdir already, so we extract them into
#                            data/ directly.
##############################################################################
fetch_asset() {
    local key="$1" url="$2" sha="$3" dest="$4"
    local zip_name
    zip_name=$(basename "$url")
    local cached="$ASSET_CACHE/$zip_name"

    mkdir -p "$ASSET_CACHE" "$dest"

    if [ -f "$cached" ]; then
        local actual
        actual=$(sha256sum "$cached" | awk '{print $1}')
        if [ "$actual" != "$sha" ]; then
            echo "  cache miss for $zip_name (sha256 mismatch); re-fetching"
            rm -f "$cached"
        fi
    fi
    if [ ! -f "$cached" ]; then
        echo "  downloading $key from $url"
        curl --fail --location --silent --show-error -o "$cached" "$url"
        local actual
        actual=$(sha256sum "$cached" | awk '{print $1}')
        if [ "$actual" != "$sha" ]; then
            echo "ERROR: $key sha256 mismatch (got $actual, expected $sha)" >&2
            exit 1
        fi
    fi
    echo "  extracting $zip_name → ${dest#$STAGE_DIR/}"
    ( cd "$dest" && unzip -q -o "$cached" )
}

# Use a short git rev in the artifact name when we can.
GITREV="$(cd "$PROJECT_ROOT" && git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)"
# OpenRCT2mini fork version. Single source of truth is src/openrct2/Version.h
# (kOpenRCT2miniVersion); we re-extract it here so the tarball name and the
# binary banner can never drift apart.
FORK_VERSION="$(grep -oE 'kOpenRCT2miniVersion "[^"]*"' \
    "$PROJECT_ROOT/src/openrct2/Version.h" | sed -E 's/.*"([^"]+)"/\1/' \
    || echo unknown)"
STAGE_DIR="$DIST_DIR/stage-$$"
# OPENRCT2MINI revision 63: stage Onion's port-collection layout. APP_DIR
# now points at Roms/PORTS/Games/OpenRCT2mini (where the binary lives).
# Imgs/ and Shortcuts/ live alongside it in Roms/PORTS/.
PORTS_ROOT="$STAGE_DIR/Roms/PORTS"
APP_DIR="$PORTS_ROOT/Games/OpenRCT2mini"
IMGS_DIR="$PORTS_ROOT/Imgs"
SHORTCUTS_DIR="$PORTS_ROOT/Shortcuts/Simulation"
mkdir -p "$APP_DIR/data"
mkdir -p "$APP_DIR/save/OpenRCT2"
mkdir -p "$IMGS_DIR" "$SHORTCUTS_DIR"

##############################################################################
# 1. Binary — strip in the cross-toolchain so we use arm-linux-gnueabihf-strip
#    (host strip won't touch ARM ELF flags correctly).
##############################################################################
cp "$BUILD_DIR/openrct2" "$APP_DIR/openrct2"
# OPENRCT2MINI: cut 39f. Bake $ORIGIN/lib into RUNPATH so the loader picks our
# bundled libs first, regardless of the parent's LD_LIBRARY_PATH. (OnionUI's
# launcher chain may set LD_LIBRARY_PATH such that /mnt/SDCARD/.tmp_update/
# lib/parasyte ranks higher than our launch.sh prepend; RUNPATH avoids that
# fight entirely.)
if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/lib' "$APP_DIR/openrct2"
    patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$APP_DIR/openrct2" 2>/dev/null || true
    echo "  patched RUNPATH = \$ORIGIN/lib"
else
    echo "WARNING: patchelf not on host — RUNPATH unchanged. Install patchelf." >&2
fi
if [ "$STRIP" = "1" ]; then
    docker run --rm --user "$(id -u):$(id -g)" -v "$STAGE_DIR:/stage" "$IMAGE_TAG" \
        bash -c "arm-linux-gnueabihf-strip -s /stage/Roms/PORTS/Games/OpenRCT2mini/openrct2"
fi
chmod 0755 "$APP_DIR/openrct2"

##############################################################################
# 1b. OPENRCT2MINI: cut 39e. Bundle dynamic libraries the binary still depends
#     on so we don't inherit OnionUI's broken pair: the device ships
#     /mnt/SDCARD/.tmp_update/lib/parasyte/libpng16.so.16 (requires
#     ZLIB_1.2.9) against /mnt/SDCARD/miyoo/lib/libz.so.1 (older, doesn't
#     provide ZLIB_1.2.9). The cross-toolchain sysroot has matched
#     libpng16 1.6.37 + zlib 1.2.11. launch.sh prepends $APPDIR/lib to
#     LD_LIBRARY_PATH so the loader picks ours.
##############################################################################
mkdir -p "$APP_DIR/lib"
docker run --rm --user "$(id -u):$(id -g)" -v "$STAGE_DIR:/stage" "$IMAGE_TAG" \
    bash -c '
        SYSROOT=$(arm-linux-gnueabihf-gcc -print-sysroot)
        # OPENRCT2MINI: cut 40. libSDL2-2.0.so.0 is the XK9274/sdl2_miyoo
        # vendor port (libmi_gfx integration) — without it the binary
        # paints into a window whose surface never reaches the panel.
        for libname in libpng16.so.16 libz.so.1 libSDL2-2.0.so.0; do
            for cand in "$SYSROOT/usr/lib/$libname" "$SYSROOT/lib/$libname" \
                        "$SYSROOT/usr/lib/arm-linux-gnueabihf/$libname"; do
                if [ -e "$cand" ]; then
                    cp -L "$cand" "/stage/Roms/PORTS/Games/OpenRCT2mini/lib/$libname"
                    arm-linux-gnueabihf-strip -s --strip-unneeded \
                        "/stage/Roms/PORTS/Games/OpenRCT2mini/lib/$libname" 2>/dev/null || true
                    sz=$(du -h "/stage/Roms/PORTS/Games/OpenRCT2mini/lib/$libname" | awk "{print \$1}")
                    echo "  bundled $libname ($sz)"
                    break
                fi
            done
        done
    '

##############################################################################
# 2. Stock OpenRCT2 data — language pack, shaders, sequence index, etc. The
#    binary searches relative to the executable for `./data/`, so this is
#    where it lands.
##############################################################################
if [ -d "$PROJECT_ROOT/data" ]; then
    # Avoid copying anything network/scripting-only that we've disabled.
    cp -r "$PROJECT_ROOT/data"/* "$APP_DIR/data/" 2>/dev/null || true
fi

##############################################################################
# 2a. Generated data files (g2.dat / palettes.dat / fonts.dat / tracks.dat).
#     These are produced by the host build (cmake target + Sprite.exe-style
#     post-build step). The ARM cross-build can't generate them on its own.
#     The bytes are architecture-neutral, so we just copy them from the host
#     build dir. Without these, the binary aborts with "Unable to load
#     g2.dat graphics" on first asset load.
##############################################################################
HOST_BUILD="$PROJECT_ROOT/build-host"
DATFILES_OK=1
for f in g2.dat palettes.dat fonts.dat tracks.dat; do
    if [ -f "$HOST_BUILD/$f" ]; then
        cp "$HOST_BUILD/$f" "$APP_DIR/data/$f"
    elif [ -f "$HOST_BUILD/data/$f" ]; then
        cp "$HOST_BUILD/data/$f" "$APP_DIR/data/$f"
    else
        echo "WARNING: $f not found in $HOST_BUILD/ or $HOST_BUILD/data/." >&2
        DATFILES_OK=0
    fi
done
if [ "$DATFILES_OK" = "0" ]; then
    echo "WARNING: One or more generated .dat files missing. Build the host" >&2
    echo "         project first (in build-host/) so package.sh can pull them." >&2
fi

##############################################################################
# 2b. Supplemental asset packs (title sequences, objects, OpenSFX, OpenMusic).
#     Bundled by default. Each pack is shipped under a permissive license
#     (CC-BY-4.0 / MIT / CC-BY-SA-4.0 — see THIRD_PARTY_NOTICES.md and the
#     LICENSE file we copy in alongside each pack below). Pass --no-assets
#     to skip the embed.
##############################################################################
if [ "$BUNDLE_ASSETS" != "1" ]; then
    echo "Skipping supplemental asset fetch (--no-assets set)."
else
    if ! command -v curl >/dev/null 2>&1 || ! command -v unzip >/dev/null 2>&1; then
        echo "ERROR: package.sh needs 'curl' and 'unzip' on the host." >&2
        exit 1
    fi
    echo "==[ supplemental asset packs ]================================="

    # Parse assets.json — small enough to grep, avoids a jq hard-dep.
    asset_field() {
        # asset_field <key> <field>  (sha256 or url)
        python3 -c "
import json, sys
with open('$PROJECT_ROOT/assets.json') as f:
    data = json.load(f)
print(data['$1']['$2'])
"
    }

    fetch_asset "title-sequences" \
        "$(asset_field title-sequences url)" \
        "$(asset_field title-sequences sha256)" \
        "$APP_DIR/data/sequence"
    # OPENRCT2MINI cut 44: keep only RCT1 and RCT2 .parkseq files.
    # The other three built-ins (rct1aa, rct1aall, openrct2) include
    # large title parks that exceed our 255-ride cap; cut 41b drops
    # overflow rides and the result renders corrupt. The C++ side
    # (TitleSequenceManager.cpp) was also trimmed to advertise only
    # *RCT1 and *RCT2, but the .parkseq files would still be shipped
    # dead weight if we didn't prune them here.
    rm -f "$APP_DIR/data/sequence/rct1aa.parkseq" \
          "$APP_DIR/data/sequence/rct1aall.parkseq" \
          "$APP_DIR/data/sequence/openrct2.parkseq"
    fetch_asset "objects" \
        "$(asset_field objects url)" \
        "$(asset_field objects sha256)" \
        "$APP_DIR/data/object"
    # opensfx and openmusic both contain assetpack/*.parkap so they extract
    # at the data/ level (creating data/assetpack/...).
    fetch_asset "opensfx" \
        "$(asset_field opensfx url)" \
        "$(asset_field opensfx sha256)" \
        "$APP_DIR/data"
    fetch_asset "openmusic" \
        "$(asset_field openmusic url)" \
        "$(asset_field openmusic sha256)" \
        "$APP_DIR/data"

    ##########################################################################
    # 2b.i. License compliance for the bundled asset packs.
    #
    # CC-BY-4.0 (title-sequences, objects), CC-BY-SA-4.0 (OpenMusic), and MIT
    # (OpenSFX) all require the license text + attribution to travel with the
    # redistributed material. Fetch each upstream LICENSE/COPYING file and
    # drop it next to the data the pack created. THIRD_PARTY_NOTICES.md in
    # the source tree documents the high-level summary; the per-pack files
    # below satisfy the in-tarball notice requirement on their own.
    ##########################################################################
    echo "==[ asset-pack license files ]================================="
    mkdir -p "$APP_DIR/data/sequence" "$APP_DIR/data/object" "$APP_DIR/data/assetpack"
    fetch_license() {
        # fetch_license <upstream-url> <dest-path>
        local url="$1" dest="$2"
        echo "  fetching $(basename "$dest")"
        curl --fail --location --silent --show-error -o "$dest" "$url"
    }
    fetch_license \
        "https://raw.githubusercontent.com/OpenRCT2/title-sequences/master/LICENSE" \
        "$APP_DIR/data/sequence/LICENSE.title-sequences.txt"
    fetch_license \
        "https://raw.githubusercontent.com/OpenRCT2/objects/master/LICENCE" \
        "$APP_DIR/data/object/LICENSE.objects.txt"
    fetch_license \
        "https://raw.githubusercontent.com/OpenRCT2/OpenSoundEffects/master/LICENSE" \
        "$APP_DIR/data/assetpack/LICENSE.opensfx.txt"
    fetch_license \
        "https://raw.githubusercontent.com/OpenRCT2/OpenMusic/master/COPYING" \
        "$APP_DIR/data/assetpack/LICENSE.openmusic.txt"
    cat > "$APP_DIR/data/ASSETPACKS.md" <<'ATTR_EOF'
Bundled OpenRCT2 supplemental asset packs
=========================================

This OpenRCT2mini tarball ships four supplemental asset packs published
by the OpenRCT2 project. Each is included as-is, unmodified, under its
own license. The license text travels with the pack inside the tarball;
high-level attribution is also in this project's THIRD_PARTY_NOTICES.md.

  data/sequence/   — title-sequence demo parks
                     (c) OpenRCT2 contributors, CC-BY-4.0
                     https://github.com/OpenRCT2/title-sequences
                     full text: data/sequence/LICENSE.title-sequences.txt

  data/object/     — additional scenery / ride objects
                     (c) OpenRCT2 contributors, CC-BY-4.0
                     https://github.com/OpenRCT2/objects
                     full text: data/object/LICENSE.objects.txt

  data/assetpack/openrct2.sound.parkap
                   — OpenSFX replacement sound effects
                     (c) 2019 OpenRCT2, MIT
                     https://github.com/OpenRCT2/OpenSoundEffects
                     full text: data/assetpack/LICENSE.opensfx.txt

  data/assetpack/openrct2.music.parkap
                   — OpenMusic replacement music tracks
                     (c) OpenRCT2 contributors, CC-BY-SA-4.0
                     https://github.com/OpenRCT2/OpenMusic
                     full text: data/assetpack/LICENSE.openmusic.txt

These packs were not authored by OpenRCT2mini; we only redistribute
unmodified copies of the corresponding upstream releases. None of the
packs are derivatives of the OpenRCT2mini binary, and the OpenRCT2mini
binary is not a derivative of any of them — they are separate works
shipped alongside one another in the same archive.
ATTR_EOF
    echo "  wrote data/ASSETPACKS.md (attribution summary)"
fi

##############################################################################
# 3. launch.sh — sets up env (low-mem malloc, our cache dirs) and execs the
#    binary. OnionUI invokes this when the user picks the app.
##############################################################################
cat > "$APP_DIR/launch.sh" <<'EOF'
#!/bin/sh
# OPENRCT2MINI launcher (cuts 39 + 39d).
APPDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APPDIR"

# OPENRCT2MINI: cut 12 baked MALLOC_ARENA_MAX=2 into the binary already, but
# we set it again here in case glibc's heuristic tries to override.
export MALLOC_ARENA_MAX=2

# OPENRCT2MINI: cut 16 / 39n — SpriteScratch lives on the SD card to keep
# RSS down. The code reads ORCT_SCRATCH_DIR (NOT OPENRCT2MINI_SPRITESCRATCH;
# that name was a launch.sh bug). Without this, SpriteScratch falls through
# to /var/tmp which on Onion is a tmpfs with a small RAM-backed limit and
# every sprite-table append fails with ENOSPC.
#
# OPENRCT2MINI revision 71: this directory ALSO holds the persistent
# sprite-decode cache (objects.cache + objects.idx). Don't wipe the
# directory itself — that would defeat the cache. Only sweep stale
# anonymous scratch files (mkstemp template "openrctmini-sprites-*"),
# which are normally unlinked on creation but might survive if a previous
# run crashed before the unlink.
export ORCT_SCRATCH_DIR="$APPDIR/cache/sprite-scratch"
# TMPDIR / XDG_CACHE_HOME defenses in case some other code path reaches them.
export TMPDIR="$APPDIR/cache"
export XDG_CACHE_HOME="$APPDIR/cache"
mkdir -p "$APPDIR/cache"
mkdir -p "$ORCT_SCRATCH_DIR"
rm -f "$ORCT_SCRATCH_DIR"/openrctmini-sprites-* 2>/dev/null || true

# OPENRCT2MINI: cut 39e. Prepend our bundled libs so the loader picks the
# matched libpng16 1.6.37 / zlib 1.2.11 pair we shipped, rather than the
# broken pair OnionUI ships (parasyte's libpng16 wants ZLIB_1.2.9 but the
# device's libz doesn't have it).
export LD_LIBRARY_PATH="$APPDIR/lib:${LD_LIBRARY_PATH:-}"

# OPENRCT2MINI: cut 40b. The vendor SDL2 (XK9274/sdl2_miyoo) self-selects
# only when SDL_VIDEODRIVER=mmiyoo is set — its MMIYOO_Available() returns
# 0 otherwise. We disabled every other SDL video driver at configure time,
# so without this export SDL_Init bails with "No available video device".
export SDL_VIDEODRIVER=mmiyoo

# OPENRCT2MINI: cut 40d. Wipe any stale window_width / window_height from
# the saved config.ini so the binary's hard-clamp (640×480) is the only
# source of truth. A previous crashing run wrote 1280×720 here, which the
# mmiyoo renderer can't accept (800×600 texture cap).
# Cut 42: also scrub current_title_sequence so the binary's *RCT2 default
# wins (any previous run will have written *OPENRCT2 → corrupt big parks).
CONF_INI="$XDG_CONFIG_HOME/OpenRCT2/config.ini"
if [ -f "$CONF_INI" ]; then
    # cut 40d: window dims, cut 42: title sequence, cut 53: file-picker.
    # All three are forced from the binary, but a stale ini value would
    # win on first read; scrub them so the binary's defaults stand.
    sed -i '/^window_width *=/d; /^window_height *=/d; /^current_title_sequence *=/d; /^use_native_browse_dialog *=/d' "$CONF_INI" 2>/dev/null || true
fi

# OPENRCT2MINI cut 44 / W0: one-time wipe of shortcuts.json. Older builds
# wrote default keybinds for arrows / Z / X / F-keys into the file, and
# loadUserBindings overrides the binary's defaults with whatever is on
# disk — so subsequent default changes wouldn't take effect on upgrades.
# W0 changed the device → keyboard scancode map (face X/Y and L2/R2
# moved off F14-F17 onto C/V/W/S, L1/R1 dual-emit Q/A alongside the
# modifier keys), so any stored bindings referencing the old F-keys are
# stale. New marker name forces re-wipe on upgrade. Future runs preserve
# whatever the user rebinds via the in-game UI.
SHORTCUTS_FILE="$XDG_CONFIG_HOME/OpenRCT2/shortcuts.json"
WIPE_MARKER="$XDG_CONFIG_HOME/OpenRCT2/.shortcuts_wiped_w0"
if [ -f "$SHORTCUTS_FILE" ] && [ ! -f "$WIPE_MARKER" ]; then
    rm -f "$SHORTCUTS_FILE"
    mkdir -p "$XDG_CONFIG_HOME/OpenRCT2"
    : > "$WIPE_MARKER"
fi

# OpenRCT2 reads its config and saves under XDG_CONFIG_HOME/OpenRCT2/.
# We point that at the app dir so everything stays on the SD card next to
# the binary, and the user only has to back up one folder.
export XDG_CONFIG_HOME="$APPDIR/save"

# OPENRCT2MINI revision 39d / 64. Log is appended across runs only for
# *debug* builds — package.sh drops a `.debug` sentinel in the install
# folder when it's a debug build. In a release build the sentinel is
# absent: LOGFILE points at /dev/null, the log-rotate / header / footer
# blocks below all become silent no-ops, and the binary's stderr (which
# is itself silent in release because the OPENRCT2MINI_DEBUG checkpoints
# compile to nothing) goes to /dev/null.
if [ -f "$APPDIR/.debug" ]; then
    mkdir -p "$APPDIR/log"
    LOGFILE="$APPDIR/log/run.log"
    # Cap the log at ~512 KB so it doesn't grow forever.
    if [ -f "$LOGFILE" ] && [ "$(wc -c < "$LOGFILE" 2>/dev/null || echo 0)" -gt 524288 ]; then
        tail -c 262144 "$LOGFILE" > "$LOGFILE.tmp" && mv "$LOGFILE.tmp" "$LOGFILE"
    fi
else
    LOGFILE="/dev/null"
fi
{
    echo
    echo "================================================================"
    echo "OpenRCT2mini run @ $(date -Iseconds 2>/dev/null || date)"
    echo "----------------------------------------------------------------"
    uname -a 2>/dev/null
    echo
    echo "memory:"
    cat /proc/meminfo 2>/dev/null | head -5
    echo
    echo "env:"
    echo "  XDG_CONFIG_HOME=$XDG_CONFIG_HOME"
    echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    echo "  MALLOC_ARENA_MAX=$MALLOC_ARENA_MAX"
    echo "  SPRITESCRATCH=$OPENRCT2MINI_SPRITESCRATCH"
    echo "  PWD=$(pwd)"
    echo "  PATH=$PATH"
    echo
    echo "ldd ./openrct2:"
    if command -v ldd >/dev/null 2>&1; then
        ldd ./openrct2 2>&1 | sed 's/^/  /'
    else
        echo "  (ldd not present — listing the bundled lib dir instead)"
        ls -la "$APPDIR/lib" 2>&1 | sed 's/^/  /'
    fi
    echo "----------------------------------------------------------------"
} >> "$LOGFILE" 2>&1

# OPENRCT2MINI: cut 39f. Allow the kernel to drop core files alongside the
# binary so we can post-mortem-debug a SIGSEGV with arm-linux-gnueabihf-gdb.
# core_pattern is a system-wide kernel sysctl; we can't change it from a
# user-mode shell, but ulimit -c unlimited at least lets the kernel TRY.
ulimit -c unlimited 2>/dev/null
mkdir -p "$APPDIR/log"

# OPENRCT2MINI: cut 57. Diagnose what's holding MI_AO. Cut 56b's background
# script ran SILENTLY (no trace in run.log) and SetPubAttr still failed
# 0xa0052009 — so either the script raced openrct2, or pkill / the /proc
# unmute didn't apply on this firmware, or something other than audioserver
# is holding the device. Replace it with a SYNCHRONOUS, LOUDLY-LOGGED
# diagnostic so the next run's log tells us exactly what's going on.
{
    echo "[ao-prep] BEGIN at $(date -Iseconds 2>/dev/null || date)"
    echo "[ao-prep] LD_PRELOAD before: '${LD_PRELOAD:-}'"
    unset LD_PRELOAD
    echo "[ao-prep] LD_PRELOAD after unset: '${LD_PRELOAD:-}'"
    echo "[ao-prep] /proc/mi_modules listing:"
    ls -la /proc/mi_modules/ 2>&1 | sed 's/^/  /'
    echo "[ao-prep] /proc/mi_modules/mi_ao listing:"
    ls -la /proc/mi_modules/mi_ao/ 2>&1 | sed 's/^/  /'
    if [ -e /proc/mi_modules/mi_ao/mi_ao0 ]; then
        echo "[ao-prep] /proc/mi_modules/mi_ao/mi_ao0 contents:"
        head -c 4096 /proc/mi_modules/mi_ao/mi_ao0 2>/dev/null | sed 's/^/  /'
    fi
    echo "[ao-prep] processes referencing libpadsp / audio:"
    ps 2>/dev/null | grep -iE 'audio|padsp' | grep -v grep | sed 's/^/  /'
    echo "[ao-prep] full ps (first 60 lines):"
    ps 2>/dev/null | head -60 | sed 's/^/  /'
    # killall is busybox, pkill may not exist. Try several names.
    for proc in audioserver audioserver.mod audioservice padsp pulseaudio; do
        if pidof "$proc" >/dev/null 2>&1; then
            echo "[ao-prep] killing $proc (pid=$(pidof $proc))"
            killall -9 "$proc" 2>/dev/null
        fi
    done
    sleep 0.3
    if [ -e /proc/mi_modules/mi_ao/mi_ao0 ]; then
        echo "[ao-prep] writing set_ao_mute 0"
        echo "set_ao_mute 0" > /proc/mi_modules/mi_ao/mi_ao0 2>&1
        echo "[ao-prep] writing set_ao_volume 0 -10dB"
        echo "set_ao_volume 0 -10dB" > /proc/mi_modules/mi_ao/mi_ao0 2>&1
        echo "[ao-prep] writing set_ao_volume 1 -10dB"
        echo "set_ao_volume 1 -10dB" > /proc/mi_modules/mi_ao/mi_ao0 2>&1
    else
        echo "[ao-prep] /proc/mi_modules/mi_ao/mi_ao0 NOT PRESENT — can't unmute"
    fi
    echo "[ao-prep] END"
} >> "$LOGFILE" 2>&1

# Run, capture exit / signal. Onion's busybox doesn't ship stdbuf, so we
# can't make stdio unbuffered from the shell. The OPENRCT2MINI checkpoint
# helper in main() does its own fflush() after each line, which is what
# matters for crash diagnosis.
#
# OPENRCT2MINI: cut 39k. Pass --rct2-data-path / --user-data-path explicitly
# so we override whatever stale `game_path` the user might have in
# save/OpenRCT2/config.ini (OpenRCT2 saves the config back at first run,
# and tarball extracts don't always overwrite it). gCustomRCT2DataPath is
# checked before config.ini's game_path in Context::GetOrPromptRCT2Path,
# so this is the surest way to point the binary at the right install.
./openrct2 \
    --rct2-data-path="$APPDIR/rct2" \
    --user-data-path="$APPDIR/save" \
    >> "$LOGFILE" 2>&1
RC=$?

# Move any core file the kernel dropped into log/ for the user to retrieve.
for c in "$APPDIR"/core* "$APPDIR"/core.*; do
    [ -f "$c" ] && mv "$c" "$APPDIR/log/$(basename "$c").$(date +%s)" 2>/dev/null
done

{
    echo "----------------------------------------------------------------"
    echo "exit_code=$RC  (128+N = killed by signal N)"
    if [ "$RC" -ge 128 ]; then
        SIG=$((RC - 128))
        case $SIG in
            6)  echo "signal=SIGABRT (assertion or std::terminate)" ;;
            8)  echo "signal=SIGFPE (divide-by-zero or other arithmetic)" ;;
            11) echo "signal=SIGSEGV (segfault — null deref or bad pointer)" ;;
            13) echo "signal=SIGPIPE" ;;
            15) echo "signal=SIGTERM (clean kill)" ;;
            *)  echo "signal=$SIG" ;;
        esac
    fi
    echo "================================================================"
} >> "$LOGFILE" 2>&1

exit $RC
EOF
chmod 0755 "$APP_DIR/launch.sh"

##############################################################################
# 4. Port shortcut + box art. Replaces the old App-style config.json + 74x74
#    icon.png. Onion's import-ports script reads the shortcut to populate
#    the Game Library; the .png in Imgs/ becomes the box-art tile.
##############################################################################
# 4a. Shortcut script — Onion convention is a /bin/sh script with a few
#     declarative metadata lines at the top, followed by the actual launch
#     command. GameDataFile points at a sentinel inside the user's RCT2
#     install: while it's missing the file is renamed to .notfound and the
#     port hides from the Game Library, which is exactly the UX we want
#     (the user supplies their own RCT2 install — see INSTALL.txt).
cat > "$SHORTCUTS_DIR/OpenRCT2mini.port" <<'EOF'
#!/bin/sh
# OpenRCT2mini OnionUI port shortcut (revision 63).
GameName="OpenRCT2mini"
GameDir="Roms/PORTS/Games/OpenRCT2mini/"
GameExecutable="openrct2"
# Sentinel file: present only after the user copies their RCT2 install into
# Games/OpenRCT2mini/rct2/. While missing, ~import ports renames us to
# .notfound and we stay hidden from the Game Library.
GameDataFile="Roms/PORTS/Games/OpenRCT2mini/rct2/Data/g1.dat"
cd "/mnt/SDCARD/$GameDir"
exec ./launch.sh
EOF
chmod 0755 "$SHORTCUTS_DIR/OpenRCT2mini.port"

# 4b. Box art. Imgs/<GameName>.png is the portrait tile Onion shows in the
#     Game Library. The art is generated from the GPL-3 logo by
#     resources/portbox/build_box_art.py and committed at
#     Packaging/miyoo_mini/box_art.png; the script is just a copy.
BOX_ART_SRC="$PROJECT_ROOT/Packaging/miyoo_mini/box_art.png"
if [[ -f "$BOX_ART_SRC" ]]; then
    cp "$BOX_ART_SRC" "$IMGS_DIR/OpenRCT2mini.png"
else
    echo "[package] WARN: $BOX_ART_SRC missing — port will have no box art." >&2
    echo "             Run: python3 resources/portbox/build_box_art.py" >&2
fi

##############################################################################
# 5. (no shipped config.ini) — revision 79.
#
# Earlier revisions shipped a pre-populated save/OpenRCT2/config.ini that set
# game_path = rct2, window_width = 640, etc. None of that is needed any more:
#
#   - Cut 46 makes the binary auto-detect rct2/ (and rct1/) next to the
#     executable when no game_path is configured. INSTALL.txt tells the user
#     to place RCT2 at exactly that location.
#   - Cuts 40c / 40d default window_width/height to 640×480 and hard-clamp
#     them — the saved values don't matter.
#   - fullscreen_mode = 0 and window_scale = 1.0 are the in-engine defaults.
#
# Keeping the directory itself so the binary's first save doesn't have to
# create it.
##############################################################################

##############################################################################
# 7. INSTALL.txt — user-facing instructions, pinned in the tarball root.
##############################################################################
cat > "$APP_DIR/INSTALL.txt" <<EOF
OPENRCT2mini for Miyoo Mini (OnionUI)
=====================================

This archive is OpenRCT2mini v${FORK_VERSION} (commit ${GITREV}), packaged
as an OnionUI port. After install, OpenRCT2mini appears in your Game
Library under "Simulation".

INSTALLING
----------

1. Extract the .7z to the *root* of your Miyoo Mini's SD card. The archive
   already contains the Roms/PORTS/ paths, so the result will be:

       /SDCARD/Roms/PORTS/Games/OpenRCT2mini/openrct2
       /SDCARD/Roms/PORTS/Games/OpenRCT2mini/launch.sh
       /SDCARD/Roms/PORTS/Games/OpenRCT2mini/data/...
       /SDCARD/Roms/PORTS/Imgs/OpenRCT2mini.png         (box art)
       /SDCARD/Roms/PORTS/Shortcuts/Simulation/
                                       OpenRCT2mini.port (or .notfound)

   If your card already has a Roms/PORTS/ folder from other ports, just
   merge, nothing in this archive overlaps with anything Onion ships.

2. Copy your legitimate RollerCoaster Tycoon 2 install to:

       /SDCARD/Roms/PORTS/Games/OpenRCT2mini/rct2/

   The folder must contain the original Data/ and ObjData/ directories
   with g1.dat, css1.dat, etc. We cannot ship these — RCT2 is not free.

   If you also have RollerCoaster Tycoon 1 (Loopy Landscapes), drop it
   alongside at:

       /SDCARD/Roms/PORTS/Games/OpenRCT2mini/rct1/

   The first run auto-detects RCT1 if it's there; the RCT1-themed
   scenarios then appear in the scenario list.

3. From the OnionUI Game Switcher, run "Refresh roms". OpenRCT2mini will
   appear in the **Simulation** category in your Game Library.

CONTROLS
-----------------

(Host PC test keys in parentheses. Each device button maps to a
common keyboard key so all controls are testable on a normal
keyboard.)

  D-pad           Move cursor                              (arrow keys)
  A    (Z)        Left click
  B    (X)        Right click — context-sensitive drag:
                  • over a window's title bar / body → drag the window
                  • over the game viewport → camera pan / quick-click =
                    delete object
                  • over a scrollable list → drag-scroll the list
  X    (C)        (unbound)
  Y    (V)        Rotate construction object clockwise
  L1   (Q)        Shift modifier — also raises placement Z,
                  enables vertical stack, etc.
  R1   (A)        Hold for fast cursor (~2.5x speed); also acts
                  as the gamepad modifier for L2 / R2 / Y below
  L2   (W)        Rotate view counter-clockwise
  R2   (S)        Rotate view clockwise
  L1 + R1         Ctrl modifier (sample-and-lock placement Z onto a
                  clicked element -- footpaths, track, scenery)
  R1 + L2         Zoom out
  R1 + R2         Zoom in
  R1 + Y          Rotate construction object counter-clockwise
  Start          Confirm / open in-game menu                (Enter)
  Select         Cancel / back                              (Esc)

EOF

##############################################################################
# 7c. Debug-build sentinel. launch.sh checks for this and only writes
#     log/run.log if it's present. Release archives get no sentinel.
##############################################################################
if [ "$PACKAGE_DEBUG" = "1" ]; then
    : > "$APP_DIR/.debug"
    echo "  packaged as DEBUG build (.debug sentinel + log/run.log enabled)"
else
    echo "  packaged as RELEASE build (no diagnostic logfile on the device)"
fi

##############################################################################
# 8. 7z archive — Onion's port-collection convention is to extract the
#    archive at the SD-card root so its Roms/PORTS/ paths merge cleanly
#    with what's already there. 7z is the format Onion's port repo uses;
#    sticking to it keeps the install instructions simple ("extract to
#    SD-card root").
##############################################################################
mkdir -p "$DIST_DIR"
ARCHIVE="$DIST_DIR/OpenRCT2mini-${FORK_VERSION}-${GITREV}.7z"
rm -f "$ARCHIVE"
if ! command -v 7z >/dev/null 2>&1; then
    echo "ERROR: 7z not on host — install p7zip-full." >&2
    exit 1
fi
# 7z 'a' adds; -mx=9 = max compression; relative path "Roms" so the archive
# entries are Roms/PORTS/... (extract-at-root produces /SDCARD/Roms/PORTS/...).
( cd "$STAGE_DIR" && 7z a -mx=9 -bd -bb0 "$ARCHIVE" Roms ) >/dev/null
rm -rf "$STAGE_DIR"

echo
echo "Packaged: $ARCHIVE"
ls -la "$ARCHIVE"
echo
echo "Contents (top 20):"
{ 7z l -ba "$ARCHIVE" | awk '{print $NF}' | head -20 || true; } 2>/dev/null
echo "  ... ($(7z l -ba "$ARCHIVE" | wc -l) entries total)"
