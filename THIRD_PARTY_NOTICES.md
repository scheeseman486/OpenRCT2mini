# Third-Party Notices

OpenRCT2mini is a downstream fork of [OpenRCT2](https://github.com/OpenRCT2/OpenRCT2) (upstream commit `e65fb01`, version `0.5.0`) and inherits OpenRCT2's licensing — see `licence.txt` (GNU GPL v3) for the project itself. Files specific to this fork (the `Packaging/miyoo_mini/` tree, the OPENRCT2MINI-tagged source patches, and these notices) are likewise GPL-3.0-only.

This document catalogues every third-party component that ends up in the binary or the device tarball, the license it ships under, and any patches OpenRCT2mini applies on top.

OpenRCT2's own upstream third-party manifest is `contributors.md` plus `src/thirdparty/`; that material continues to apply unchanged. The notices below cover only the additions OpenRCT2mini introduces.

## Components built into the device binary

### vendor SDL2 — `XK9274/sdl2_miyoo`

The Miyoo Mini's panel is driven by the SigmaStar `libmi_gfx` 2D blitter, and audio by `libmi_ao`. Vanilla SDL2 has no driver for either; pixels written through vanilla SDL2 never reach the panel and audio never reaches the speaker. The `XK9274/sdl2_miyoo` fork integrates both: it is a derivative of upstream SDL2 with mmiyoo video / render / audio / event drivers, originally authored by Steward Fu and maintained by XK9274.

| Field | Value |
|---|---|
| Source | <https://github.com/XK9274/sdl2_miyoo> (branch `main`) |
| Upstream of fork | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) version 2.x |
| License | zlib (the SDL2 license; mmiyoo additions are derivative works under the same terms) |
| Where it ends up | `App/OpenRCT2mini/lib/libSDL2-2.0.so.0` inside the device tarball |

OpenRCT2mini patches the fork at toolchain-image build time, all from `Packaging/miyoo_mini/build-deps.sh`. The patches do not change SDL2's licensing — they remain zlib-licensed derivatives. Summary:

- **Strip optional sub-library includes** (`SDL_ttf`, `SDL_image`, `SDL_mixer`, `SDL2_gfxPrimitives`, `SDL2_rotozoom` from `SDL_video_mmiyoo.h/.c` and `SDL_render_mmiyoo.c`). The fork's headers reference them for an unimplemented battery/wifi overlay; the .c sources never call any of those symbols. We don't ship those libraries, so we elide the includes.
- **Strip the `-lEGL -lGLESv2` link flags** the fork's `configure.ac` adds for SwiftShader integration. We do not bundle SwiftShader and OpenRCT2 uses software rendering.
- **Replace `SDL_opengles_mmiyoo.c`** with a no-op stub that returns failure / NULL for every entry point.
- **Fix the `texture_formats[2]` designator typo** in `SDL_render_mmiyoo.c`. The fork declares `[0] = RGB565, [2] = ARGB8888` with `num_texture_formats = 2`; SDL iterates 0–1, so slot 1 is `SDL_PIXELFORMAT_UNKNOWN` and OpenRCT2's renderer picks it. We rewrite slot 1 to ARGB8888.
- **Force `HOST=arm-linux`** so `CheckMMiyooAudio` in `configure.ac` actually links `-lmi_ao -lshmvar -lmi_common -lmi_sys -lmi_gfx`.
- **Patch `set_key()` in `SDL_event_mmiyoo.c`** to emit SDL keyboard events directly. The fork's input thread reads `/dev/input/event0` into an internal bitmap and never queues SDL events; we synthesize key-down / key-up so OpenRCT2's input pipeline sees button presses.
- **Disable the `code[]`-loop in `MMIYOO_PumpEvents`**. Pre-patch this loop also synthesized SDL key events, with a different scancode mapping than `set_key()` — every face-button press fired two distinct keys. With `set_key()` now emitting the desired scancodes, the second loop is duplicate work and a bug source.
- **Force `spec.freq = 48000` in the audio driver's `OpenDevice`** and use `E_MI_AUDIO_SAMPLE_RATE_48000` in the MI_AO config. The SoC's audio block only accepts 48 kHz; OpenRCT2 resamples to that rate via `SDL_BuildAudioCVT` since we negotiate `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`.

These patches are applied to a fresh clone in the Docker toolchain image, never committed back upstream, and the modified source is destroyed when the image is rebuilt.

### libzstd 1.5.5

| Field | Value |
|---|---|
| Source | <https://github.com/facebook/zstd> |
| License | BSD-3-Clause |
| Where it ends up | Statically linked into the OpenRCT2mini binary |
| Patches | None — built unmodified |

### libogg 1.3.5 / libvorbis 1.3.7

| Field | Value |
|---|---|
| Source | <https://github.com/xiph/ogg> / <https://github.com/xiph/vorbis> |
| License | BSD-3-Clause (Xiph.org Foundation) |
| Where it ends up | Statically linked into the OpenRCT2mini binary |
| Patches | None — built unmodified |

OpenRCT2mini originally fetched from `downloads.xiph.org` but switched to GitHub mirrors when xiph.org went offline mid-build. The source is byte-identical to the xiph release.

### libzip 1.10.1

| Field | Value |
|---|---|
| Source | <https://libzip.org/download/> |
| License | BSD-3-Clause |
| Where it ends up | Statically linked into the OpenRCT2mini binary |
| Patches | None — built unmodified, with `BZIP2`/`LZMA` disabled (we don't use either) |

### nlohmann/json 3.12.0

| Field | Value |
|---|---|
| Source | <https://github.com/nlohmann/json> |
| License | MIT |
| Where it ends up | Header-only, compiled into translation units that include it |
| Patches | None |

## Components inherited from the upstream OnionUI toolchain image

The base of the toolchain Docker image is `ghcr.io/onionui/miyoomini-toolchain:latest`. That image already contains:

- **GCC 8.3** — GPL-3.0-or-later with the GCC Runtime Library Exception.
- **glibc 2.28** — LGPL-2.1 (loaded dynamically on the device, not redistributed in the binary).
- **libstdc++ 8.3** — GPL-3.0 with the libstdc++ Exception. Statically linked into the OpenRCT2mini binary by upstream-build settings, license terms permit static linking of the runtime.
- **libpng 1.6.37** — libpng license (a permissive variant of zlib).
- **zlib 1.2.11** — zlib license.

`libpng16.so.16` and `libz.so.1` are also redistributed inside the device tarball (in `App/OpenRCT2mini/lib/`), because the device's stock copies are mismatched (the OnionUI launcher's `libpng16` requires `ZLIB_1.2.9` symbols that the device's `libz` doesn't export). Both ship under their original licenses; OpenRCT2mini does not modify them.

## Components linked at runtime against device libraries

The OpenRCT2mini binary calls into the following SigmaStar / OnionUI shared libraries that already exist in the device's read-only OS image:

- `libmi_gfx.so` — SigmaStar 2D blitter and panel framebuffer access.
- `libmi_ao.so` — SigmaStar audio output.
- `libmi_sys.so`, `libmi_common.so` — SigmaStar system / common utilities.
- `libshmvar.so` — SigmaStar shared-memory variable interface (audio).

These libraries are SigmaStar proprietary code, present on every Miyoo Mini, and form the only access path to the panel and speakers. OpenRCT2mini links against the headers in the toolchain SDK at build time but does not redistribute the libraries — the device provides them.

## Game data

OpenRCT2mini does NOT package any game-content files in its release tarball. The user supplies:

- A legally-owned RollerCoaster Tycoon 2 install (`Data/`, `ObjData/`, `g1.dat`, `css1.dat`, etc.), placed at `/SDCARD/App/OpenRCT2mini/rct2/`. Optionally a RollerCoaster Tycoon 1 install at `/SDCARD/App/OpenRCT2mini/rct1/`. Both games' content is © Chris Sawyer / Atari / Infogrames; OpenRCT2mini does not redistribute it.
- The OpenRCT2 project's downloadable supplemental asset packs — the title-sequence collection, the open object pack, OpenSFX (Creative-Commons-licensed sound effects), and OpenMusic (Creative-Commons-licensed music). These are downloaded directly from <https://github.com/OpenRCT2/OpenRCT2/releases> and dropped into `App/OpenRCT2mini/data/` per `INSTALL.txt`.

The packaging script can fetch these on the build machine for convenience during the developer's own builds, but the produced tarball does not embed them — see `Packaging/miyoo_mini/INSTALL.txt` for the user-facing install steps.
