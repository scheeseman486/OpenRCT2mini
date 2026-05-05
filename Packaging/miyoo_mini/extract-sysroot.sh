#!/bin/bash
# Extract the toolchain image's ARM sysroot to a host path so qemu-arm-static
# can find the device's glibc-2.28 / libstdc++ / libmi_*  via QEMU_LD_PREFIX.
#
# This is a one-time setup step; rerun only when bumping the toolchain image.
#
# Usage:  ./extract-sysroot.sh [DEST]
# Default DEST is /opt/openrctmini/arm-sysroot (writable by the running user).
set -euo pipefail

DEST="${1:-/opt/openrctmini/arm-sysroot}"
IMAGE="${OPENRCT2MINI_TOOLCHAIN_IMAGE:-ghcr.io/onionui/miyoomini-toolchain:latest}"

mkdir -p "$DEST"

echo "Extracting $IMAGE sysroot → $DEST"
docker run --rm -v "$DEST:/out" "$IMAGE" bash -lc '
  SYSROOT=$(arm-linux-gnueabihf-gcc -print-sysroot)
  cp -r "$SYSROOT"/* /out/
  echo "Copied $(du -sh /out | awk "{print \$1}")"
'

# Verify the dynamic loader landed where qemu expects it (/lib/ld-linux-armhf.so.3).
if [ ! -f "$DEST/lib/ld-linux-armhf.so.3" ]; then
  echo "ERROR: $DEST/lib/ld-linux-armhf.so.3 missing — qemu-arm will fail to load binaries."
  exit 1
fi

echo "OK. To run an ARM openrct2 binary on the host:"
echo "    QEMU_LD_PREFIX=$DEST ./build-arm/openrct2 simulate ..."
echo "(binfmt_misc is already registered for qemu-arm-static; no chroot needed)"
