# OpenRCT2mini

A fork of [OpenRCT2](https://github.com/OpenRCT2/OpenRCT2) v0.5.0 (`e65fb01` on `develop`) targeting low-spec ARM handhelds, for now only the [Miyoo Mini](https://www.miyoominihandheld.com/) and Miyoo Mini Plus running [OnionUI](https://github.com/OnionUI/Onion). The overall goal is to fit a OpenRCT2's vanilla-compatible feature set (with some raised limits and non-memory intensive features) onto 128-256 MB devices running ancient toolchains.

This is a **hard fork**. The game has been aggressively trimmed and backported to older dependencies. We may try to sync to a newer version in the future, but the core game is feature complete at v0.5.0, so eh.

## Status

OpenRCT2mini is currently playable. We match upstream v0.5.0 minus things that don't make sense on a handheld: multiplayer, plugins/scripting, OpenGL, FLAC, Discord-RPC, Japanese/Chinese/Arabic language options (IPC eats too much memory) are all in the bin.

## Installing on the device

OpenRCT2mini ships as an OnionUI **port**. It appears in your Game Library under the **Simulation** category, alongside ROMs and other ports (Doom, Duke3D, etc.), not in the App tab.

It does **not** contain RCT2 game data.

1. Extract the `.7z` to the **root** of your Miyoo Mini's SD card.
2. Copy your legitimately-owned RollerCoaster Tycoon 2 install to `/SDCARD/Roms/PORTS/Games/OpenRCT2mini/rct2/`. The folder must contain the original `Data/` and `ObjData/` directories with `g1.dat`, `css1.dat`, etc.
3. From the OnionUI Game Switcher, run **Refresh roms**. "OpenRCT2mini" appears in the **Simulation** category in your Game Library.

Optionally, RCT1 can be placed at `/SDCARD/Roms/PORTS/Games/OpenRCT2mini/rct1/` to enable RCT1 scenarios.

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

## Building

The cross-build runs entirely inside Docker; no host installation of the toolchain or its dependencies is required other than Docker itself.

```sh
./Packaging/miyoo_mini/build.sh
```

That script will:
1. Build the `openrctmini-toolchain` Docker image (first run only, ~10 min). The image derives from `ghcr.io/onionui/miyoomini-toolchain` and adds pkg-config plus the libraries OpenRCT2 needs that the upstream image doesn't ship (libzstd, libogg, libvorbis, libzip, nlohmann-json, the patched XK9274/sdl2_miyoo SDL2). Subsequent runs reuse the image.
2. Run cmake + make inside the image, producing `build-arm/openrct2`.
3. Smoke-test the binary under `qemu-arm-static` to catch any GLIBC / loader issues before SD-swapping.
4. Run `package.sh` to produce a release archive at `dist/OpenRCT2mini-<version>-<git-short>.7z` (Onion port-collection format — extract at SD-card root).

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
