# Privacy Policy

## Overview

OpenRCT2mini is a fork of OpenRCT2 stripped down for low-RAM devices (the
original target is the Miyoo Mini, a 128 MB handheld). The fork ships with
network features disabled and does not collect, transmit, or process any
personal data.

## Data collection

**None.** OpenRCT2mini builds disable network support (`DISABLE_NETWORK=ON`)
and the in-game version checker (`DISABLE_VERSION_CHECKER=ON`). The binary
makes no outbound network requests of any kind. There is no telemetry, no
crash reporter, no usage analytics, and no opt-in metrics.

## Data stored on your device

OpenRCT2mini stores the same local data the upstream game does:

- Configuration (`config.ini`, `shortcuts.json`, `rumble.json`) in the user
  data directory next to the binary.
- Save games (`.park`, `.sv6`), scenarios, track designs, and screenshots in
  the user data directory.
- A sprite-decode cache in `user/cache/` to speed up subsequent launches.

All of this stays on the device the game runs on. Nothing is uploaded, and
no one but you can read it.

## Third-party services

None. The reduced build links no telemetry, crash-reporting, or
multiplayer services (Discord RPC, breakpad, the upstream multiplayer
server browser, the upstream content download host) — they are compiled
out via the OpenRCT2mini build flags.

## Game data files

OpenRCT2mini reads RollerCoaster Tycoon 1 and 2 game data from a directory
you point it at. Those files are read-only inputs; the game does not modify,
upload, or repackage them.

## Contact

This is a community fork; report concerns or questions via the project's
GitHub repository.
