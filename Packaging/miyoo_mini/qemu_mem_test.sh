#!/bin/bash
# OPENRCT2MINI: cut 39c. Steady-state memory measurement of the cross-compiled
# ARM binary under qemu-arm-static. Boots openrct2 against the host's RCT2
# install in a sandbox, lets it reach the title screen (vanilla SDL2 video
# dummy backend means no actual display, but all asset loading and state
# init still happens), samples VmRSS / VmHWM from /proc every second.
#
# The numbers reported include qemu's translation cache + internal state on
# top of the emulated guest. Subtract the qemu_baseline.sh baseline from the
# Vm{RSS,HWM} value to estimate what the guest costs on real hardware.
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/../.." && pwd)"
SANDBOX=/tmp/openrct2_qemu_test
BIN="$PROJECT/build-arm/openrct2"
RCT2="$PROJECT/gamedata/rct2"
SAMPLES_OUT="${1:-/tmp/qemu_samples.csv}"
LOG_OUT="${2:-/tmp/qemu_run.log}"
DURATION="${DURATION:-30}"

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not present — run ./Packaging/miyoo_mini/build.sh first." >&2
    exit 1
fi
if [ ! -d "$RCT2/Data" ]; then
    echo "ERROR: $RCT2/Data not found — adjust RCT2 path in the script." >&2
    exit 1
fi

rm -rf "$SANDBOX"
mkdir -p "$SANDBOX/save/OpenRCT2" "$SANDBOX/cache/sprite-scratch" "$SANDBOX/log"
cp "$BIN" "$SANDBOX/openrct2"
cp -r "$PROJECT/data" "$SANDBOX/data"
# OPENRCT2MINI: cut 39c. g2.dat / palettes.dat / fonts.dat / tracks.dat are
# generated at host-build time (the cross-build skips this — can't run an
# ARM binary to generate them on an x86 host). They're architecture-neutral
# byte files. Pull them from the host build dir if it exists.
HOST_BUILD="$PROJECT/build-host"
for f in g2.dat palettes.dat fonts.dat tracks.dat; do
    if [ -f "$HOST_BUILD/$f" ]; then
        cp "$HOST_BUILD/$f" "$SANDBOX/data/$f"
    fi
done
# Pull the bundled asset packs (objects/, sequence/, assetpack/) from the
# already-fetched package staging if present, so the test exercises the same
# data the device build will see.
if [ -d "$PROJECT/dist/asset-cache" ]; then
    for z in "$PROJECT/dist/asset-cache"/objects.zip; do
        [ -f "$z" ] && (cd "$SANDBOX/data" && mkdir -p object && cd object && unzip -q -o "$z")
    done
    for z in "$PROJECT/dist/asset-cache"/title-sequences.zip; do
        [ -f "$z" ] && (cd "$SANDBOX/data" && mkdir -p sequence && cd sequence && unzip -q -o "$z")
    done
    for z in "$PROJECT/dist/asset-cache"/opensound.zip "$PROJECT/dist/asset-cache"/openmusic.zip; do
        [ -f "$z" ] && (cd "$SANDBOX/data" && unzip -q -o "$z")
    done
fi
ln -s "$RCT2" "$SANDBOX/rct2"

cat > "$SANDBOX/save/OpenRCT2/config.ini" <<EOF
[general]
game_path = $SANDBOX/rct2
window_width = 640
window_height = 480
window_scale = 1
EOF

cd "$SANDBOX"

# Headless: vanilla SDL2 was built with --enable-video-dummy so video init
# succeeds without a display. Audio dummy avoids PulseAudio fights with
# qemu user-mode emulation.
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=dummy
export XDG_CONFIG_HOME="$SANDBOX/save"
export OPENRCT2MINI_SPRITESCRATCH="$SANDBOX/cache/sprite-scratch"
export MALLOC_ARENA_MAX=2
export QEMU_LD_PREFIX=/home/holodeck/openrctmini-arm-sysroot

echo "elapsed_s,VmRSS_kB,VmHWM_kB,VmSize_kB" > "$SAMPLES_OUT"

# Pick a scenario for the headless `simulate` subcommand. simulate loads the
# park, ticks the simulation N times, exits. Doesn't need a display, and
# exercises the real asset-load path so we measure realistic steady-state
# memory. Tick count picked to be > 1 in-game day so save loading and
# entity/sprite-paging settle.
SCENARIO="${SCENARIO:-$RCT2/Scenarios/Forest Frontiers.SC6}"
if [ ! -f "$SCENARIO" ]; then
    # Fall back to whatever's in the Scenarios dir.
    SCENARIO="$(find "$RCT2/Scenarios" -maxdepth 1 -iname '*.sc6' | head -1)"
fi
TICKS="${TICKS:-1500}"

echo "scenario: $SCENARIO"
echo "ticks: $TICKS"
# OPENRCT2MINI: cut 39c. qemu-user-mode's /proc/self/exe points at the qemu
# binary, not the guest, so OpenRCT2's install-path discovery breaks under
# emulation (it walks /proc/self/exe → /usr/bin/data/... and finds nothing).
# Pass --openrct2-data-path / --rct2-data-path / --user-data-path explicitly.
# OPENRCT2MINI: cut 39c. simulate rejects --openrct2-data-path (those options
# are root-command only, and "All options must be passed at the end" is only
# parsed for that root command). The cwd-based fallback in Platform.Linux
# (cwd/data, cwd/object, cwd/rct2 via game_path) is what we rely on, having
# already cd'd to $SANDBOX above.
qemu-arm-static "$SANDBOX/openrct2" simulate "$SCENARIO" "$TICKS" >"$LOG_OUT" 2>&1 &
QEMU_PID=$!
echo "qemu pid=$QEMU_PID, sandbox=$SANDBOX, samples=$SAMPLES_OUT"

start=$(date +%s)
peak_hwm=0
while kill -0 "$QEMU_PID" 2>/dev/null; do
    if [ -r "/proc/$QEMU_PID/status" ]; then
        elapsed=$(( $(date +%s) - start ))
        rss=$(awk '/^VmRSS:/ {print $2}'  "/proc/$QEMU_PID/status" 2>/dev/null || echo 0)
        hwm=$(awk '/^VmHWM:/ {print $2}'  "/proc/$QEMU_PID/status" 2>/dev/null || echo 0)
        sz=$(awk  '/^VmSize:/ {print $2}' "/proc/$QEMU_PID/status" 2>/dev/null || echo 0)
        echo "$elapsed,$rss,$hwm,$sz" >> "$SAMPLES_OUT"
        if [ "${hwm:-0}" -gt "$peak_hwm" ]; then peak_hwm=$hwm; fi
    fi
    if [ "$elapsed" -ge "$DURATION" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        break
    fi
    sleep 1
done
