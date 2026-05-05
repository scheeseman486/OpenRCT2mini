#!/bin/bash
# Run the cross-compiled openrct2 binary on the host via qemu-arm-static.
# Tests CPU correctness, ABI compat, memory behaviour. Does NOT exercise
# libmi_gfx (vendor blob — needs real hardware).
#
# Usage:
#   ./Packaging/miyoo_mini/run-qemu.sh simulate /path/to/scenario.SC6 100
#   ./Packaging/miyoo_mini/run-qemu.sh --version
#
# Useful env knobs:
#   SDL_VIDEODRIVER=dummy       skip SDL video init (fastest; matches device-floor cgroup test)
#   SDL_AUDIODRIVER=dummy       skip SDL audio init
#   QEMU_CPU=cortex-a7          force ARM CPU model (defaults to qemu's "any")
#   ARM_SYSROOT=/path           override sysroot location (default /opt/openrctmini/arm-sysroot)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BINARY="${OPENRCT2MINI_BIN:-$PROJECT_ROOT/build-arm/openrct2}"
SYSROOT="${ARM_SYSROOT:-/opt/openrctmini/arm-sysroot}"

if [ ! -x "$BINARY" ]; then
    echo "ERROR: $BINARY not built. Run ./Packaging/miyoo_mini/build.sh first."
    exit 1
fi

if [ ! -d "$SYSROOT" ]; then
    echo "ERROR: $SYSROOT not present. Run ./Packaging/miyoo_mini/extract-sysroot.sh first."
    exit 1
fi

if [ ! -f "$SYSROOT/lib/ld-linux-armhf.so.3" ]; then
    echo "ERROR: $SYSROOT/lib/ld-linux-armhf.so.3 missing — sysroot looks corrupt."
    exit 1
fi

# binfmt_misc handles the qemu-arm-static dispatch transparently when ARM ELF
# is exec'd, but we still need QEMU_LD_PREFIX so the dynamic loader finds the
# device's libc (not the host's).
export QEMU_LD_PREFIX="$SYSROOT"

# Defaults that make sense for emulation: no real audio device, no real display.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

exec "$BINARY" "$@"
