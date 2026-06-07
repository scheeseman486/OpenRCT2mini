/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../management/NewsItem.h"

#include <cstdint>

// OPENRCT2MINI gamepad-plan 1.13: DualShock-style LED lightbar facade.
//
// Sticky severity model — the lightbar reflects the currently-shown
// on-screen news item for as long as that item is in the ticker.
// Lifetime matches `News::ItemQueues::current()` exactly:
//
//   setActive(severity)  — called from News::UpdateCurrentItem the
//                          first tick a queued item becomes current
//                          (ticks == 1, the same point that fires the
//                          newsItem audio cue). The lightbar holds at
//                          the severity colour until clear() runs.
//   Clear()              — called from News::ArchiveCurrent when the
//                          current item retires. Triggers a 300 ms
//                          linear fade-to-off so the transition reads
//                          as deliberate rather than abrupt.
//
// Severity → colour map (pre-brightness; UiContext applies the global
// gamepadLedBrightness multiplier before SDL submission):
//
//   critical → red    (255,   0,   0)
//   warning  → yellow (255, 200,   0)
//   money    → green  (  0, 255,   0)
//   info     → silent — setActive(info) is treated as Clear()
//
// Hot replace: setActive(newSeverity) while another severity is
// active jumps straight to the new colour with no fade — a fresh
// `critical` should slam into view even if a `money` flash was on
// the lightbar. Fade is reserved for the natural-end transition
// from active → cleared.
//
// The engine ticks once per frame from InputManager::process, right
// next to Haptic::tickEngine. SDL submission is gated by
// UiContext::SetControllerLED (which itself checks gamepadLedEnabled
// and applies gamepadLedBrightness) — when the global enabled flag
// flips off mid-display, the LED is left at whatever colour SDL last
// saw; forceClear() is the explicit knob to sweep back to off.

namespace OpenRCT2::Led
{
    // Hold the lightbar at the severity colour. Persists until Clear()
    // is called (or until setActive() is called again with a different
    // severity). severity == info is treated as Clear() — info items
    // should leave the lightbar dark per the plan's colour map.
    void setActive(OpenRCT2::News::Severity severity);

    // Retire the active severity. Starts a 300 ms linear fade-to-off
    // ramp from whatever colour the lightbar is currently at. No-op
    // if nothing is active.
    void Clear();

    // Called once per frame at the bottom of InputManager::process,
    // alongside Haptic::tickEngine. Submits the steady or fading
    // colour via UiContext::SetControllerLED (hysteresis-guarded so a
    // 60 FPS loop doesn't generate 60 HID reports per second).
    void tickEngine(uint32_t nowMs);

    // Force an immediate (0, 0, 0) sweep and clear all state. Called
    // by the Options window when the user un-checks gamepadLed-
    // Enabled so the lightbar doesn't sit at the last colour, and on
    // shutdown to leave the pad in a clean state.
    void forceClear();
} // namespace OpenRCT2::Led
