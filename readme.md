# OpenRCT2mini

A fork of [OpenRCT2](https://github.com/OpenRCT2/OpenRCT2) v0.5.0 (`e65fb01` on `develop`) targeting low-spec ARM handhelds, for now only the [Miyoo Mini](https://www.miyoominihandheld.com/) and Miyoo Mini Plus running [OnionUI](https://github.com/OnionUI/Onion). The overall goal is to fit a OpenRCT2's vanilla-compatible feature set (with some raised limits and non-memory intensive features) onto 128-256 MB devices running ancient toolchains.

This is a **hard fork**. The game has been aggressively trimmed and backported to older dependencies. We may try to sync to a newer version in the future, but the core game is feature complete at v0.5.0, so eh.

## Status

OpenRCT2mini is currently playable. We match upstream v0.5.0 minus things that don't make sense on a handheld: multiplayer, plugins/scripting, OpenGL, FLAC, Discord-RPC, Japanese/Chinese/Arabic language options (IPC eats too much memory) are all in the bin.

## Installing on the device

The release tarball contains the OpenRCT2mini binary, its bundled libraries, the launch script, OpenRCT2's stock data (language strings, shaders, fonts), and the OpenRCT2 project's supplemental asset packs (title sequences, object pack, OpenSFX, OpenMusic — see `THIRD_PARTY_NOTICES.md` for the per-pack license / attribution).

It does **not** contain RCT2 game data — that part of the install is yours:

1. Extract the tarball to the root of your Miyoo Mini's SD card. The result will be `/SDCARD/App/OpenRCT2mini/`.
2. Copy your legitimately-owned RollerCoaster Tycoon 2 install to `/SDCARD/App/OpenRCT2mini/rct2/`. The folder must contain the original `Data/` and `ObjData/` directories with `g1.dat`, `css1.dat`, etc.
3. Boot OnionUI. "OpenRCT2mini" appears in the App tab.

Optionally, RCT1 can be placed at `/SDCARD/App/OpenRCT2mini/rct1/` to enable RCT1 scenarios and CSG sprites. The launcher auto-detects both directories.

If you want a smaller tarball (e.g. for slow SD cards) you can rebuild without the asset packs via `./Packaging/miyoo_mini/package.sh --no-assets`. The released tarball ships with them included so the device is playable out of the box.

## Controls

```
  D-pad        Move cursor (accelerates after 300 ms hold)
  A            Left click
  B            Right click
  X            Cycle game speed (Normal -> Quick -> Fast -> Turbo -> Pause -> Normal)
  Y            Rotate construction object clockwise
  L1           Shift modifier (raise placement Z, vertical-stack scenery, etc.)
  R1           Hold for fast cursor (~2.5x speed); also acts as gamepad modifier button
  L2           Rotate view counter-clockwise
  R2           Rotate view clockwise
  L1 + R1      Ctrl modifier (sample-and-lock placement Z onto a clicked element)
  R1 + L2      Zoom out
  R1 + R2      Zoom in
  R1 + Y       Rotate construction object counter-clockwise
  Start        Confirm / open in-game menu
  Select       Cancel / back
```

On the dev host (or with a USB keyboard via OTG), Alt is the keyboard equivalent of R1, and the L1+R1 chord is just real-keyboard Ctrl.

## Building

The cross-build runs entirely inside Docker; no host installation of the toolchain or its dependencies is required other than Docker itself.

```sh
./Packaging/miyoo_mini/build.sh
```

That script will:
1. Build the `openrctmini-toolchain` Docker image (first run only, ~10 min). The image derives from `ghcr.io/onionui/miyoomini-toolchain` and adds pkg-config plus the libraries OpenRCT2 needs that the upstream image doesn't ship (libzstd, libogg, libvorbis, libzip, nlohmann-json, the patched XK9274/sdl2_miyoo SDL2). Subsequent runs reuse the image.
2. Run cmake + make inside the image, producing `build-arm/openrct2`.
3. Smoke-test the binary under `qemu-arm-static` to catch any GLIBC / loader issues before SD-swapping.
4. Run `package.sh` to produce a release tarball at `dist/OpenRCT2mini-<git-short>.tar.gz`.

A native host build for development iteration is also supported via the project's standard CMake build (the upstream OpenRCT2 path); see `Packaging/miyoo_mini/build.sh` for the cross-compile command-line flags that should be mirrored on host for an apples-to-apples comparison. I've only tested this on Linux.

## Scope of the fork and what this project does NOT do

- We don't contribute fixes back to OpenRCT2 upstream. Most of the changes are device-specific and would not improve upstream OpenRCT2 on desktop platforms.
- We aren't matching OpenRCT2's release cadence. The fork is pinned to upstream commit `e65fb01`. Future upstream changes will be considered case by case if they're worth merging.
- We aren't bundling or distributing Chris Sawyer's RollerCoaster Tycoon 1 / 2 game data. Those files belong to Atari / Infogrames and the user. It's on Steam and GOG, go buy it!

## Licensing

OpenRCT2mini inherits OpenRCT2's GNU GPL v3 license (`licence.txt`). All OpenRCT2mini-specific changes: the patches in `Packaging/miyoo_mini/`, OPENRCT2MINI-tagged source comments, and the build scripts are also GPL-3.0-only.

`THIRD_PARTY_NOTICES.md` catalogues every third-party component shipped in the binary (vendor SDL2, libzstd, libogg, libvorbis, libzip, nlohmann/json, and the OnionUI toolchain libraries) along with their respective licenses and any patches OpenRCT2mini applies on top.

## Where the original OpenRCT2 README went

If you're looking for upstream OpenRCT2's documentation that's at <https://github.com/OpenRCT2/OpenRCT2>. If your interest is RCT2 on a desktop or mainstream handheld (Steam Deck, etc.) then upstream is what you want.
