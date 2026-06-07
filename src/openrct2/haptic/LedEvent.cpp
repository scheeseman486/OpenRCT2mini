/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "LedEvent.h"

#include "../Context.h"
#include "../config/Config.h"
#include "../ui/UiContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace OpenRCT2::Led
{
    namespace
    {
        // Trailing fade window — how long the LED takes to ramp from
        // full severity colour to off after Clear(). 300 ms matches
        // DS4 native pulse animations and reads as a deliberate
        // notification ending rather than an abrupt switch-off.
        constexpr uint32_t kFadeMs = 300;

        // Sticky active severity. info means "nothing active";
        // setActive(info) is treated as Clear(). When a non-info
        // severity is active the engine submits its colour every tick
        // (with hysteresis below to avoid SDL spam). When cleared, the
        // engine plays a fade-out ramp from the last-active colour
        // before going idle.
        struct State
        {
            // Currently held severity; info ⇒ no steady colour.
            OpenRCT2::News::Severity active = OpenRCT2::News::Severity::info;

            // Fade state. When Clear() is called from a non-info
            // active state we capture the active colour and start a
            // 300 ms ramp from full to zero. While fading, `active`
            // is already info; tickEngine consults `fading` instead.
            bool fading = false;
            uint8_t fadeR = 0;
            uint8_t fadeG = 0;
            uint8_t fadeB = 0;
            uint32_t fadeStartMs = 0;
        };

        State _state;

        // Remember the last submitted (r, g, b) so per-frame ticks
        // only call into SDL when the colour actually changes. The
        // rumble engine uses the same TTL-based hysteresis pattern.
        // -1 sentinel means "never submitted yet" so the first frame
        // after any state change always goes through.
        int16_t _lastR = -1;
        int16_t _lastG = -1;
        int16_t _lastB = -1;

        // Severity → pre-brightness colour map. info has no entry —
        // info is treated as cleared at the setActive() call site.
        void severityColour(OpenRCT2::News::Severity sev, uint8_t& r, uint8_t& g, uint8_t& b)
        {
            switch (sev)
            {
                case OpenRCT2::News::Severity::critical:
                    r = 255;
                    g = 0;
                    b = 0;
                    return;
                case OpenRCT2::News::Severity::warning:
                    r = 255;
                    g = 200;
                    b = 0;
                    return;
                case OpenRCT2::News::Severity::money:
                    r = 0;
                    g = 255;
                    b = 0;
                    return;
                case OpenRCT2::News::Severity::info:
                default:
                    // Treated as no-op by setActive(); falling through
                    // here would request an off colour, which the
                    // caller never actually wants — info is "don't
                    // change the LED". Default arm satisfies
                    // -Wswitch-enum.
                    r = 0;
                    g = 0;
                    b = 0;
                    return;
            }
        }

        void submit(uint8_t r, uint8_t g, uint8_t b)
        {
            // Hysteresis: only submit when the rounded colour changes,
            // so a 16 ms tick at 60 FPS doesn't generate ~60 SetLED
            // calls per second during steady-state hold OR during the
            // 300 ms fade. SDL's LED writes are HID reports and not
            // free — especially on Bluetooth DualShocks where each
            // report buys a Bluetooth round-trip.
            if (r == _lastR && g == _lastG && b == _lastB)
                return;
            _lastR = r;
            _lastG = g;
            _lastB = b;

            auto* ctx = OpenRCT2::GetContext();
            if (ctx == nullptr)
                return;
            auto& ui = ctx->GetUiContext();
            ui.SetControllerLED(r, g, b);
        }
    } // namespace

    void setActive(OpenRCT2::News::Severity severity)
    {
        // info ⇒ "don't light the bar". Equivalent to a Clear() so
        // callers (News::UpdateCurrentItem) can pass through the
        // severity field of the current item unconditionally and
        // get the right behaviour for items the classifier didn't
        // promote past info.
        if (severity == OpenRCT2::News::Severity::info)
        {
            Clear();
            return;
        }

        // Capability short-circuit: if no connected pad has an LED,
        // don't even hold state — saves the per-frame tick from
        // doing colour math no one can see. Cheap insurance on
        // headless / Xbox-only / Mini pre-Phase-2 setups.
        auto* ctx = OpenRCT2::GetContext();
        if (ctx == nullptr)
            return;
        if (!ctx->GetUiContext().ControllerHasLED())
            return;

        // Hot replace: a fresh severity overrides any in-progress
        // fade or different-severity hold. No fade between active
        // severities — critical landing on top of money should slam
        // into view, not crossfade.
        _state.active = severity;
        _state.fading = false;
    }

    void Clear()
    {
        if (_state.active == OpenRCT2::News::Severity::info && !_state.fading)
            return;

        if (_state.active != OpenRCT2::News::Severity::info)
        {
            // Capture the colour we were holding at so the fade
            // ramps from the right starting point.
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            severityColour(_state.active, r, g, b);
            _state.fadeR = r;
            _state.fadeG = g;
            _state.fadeB = b;
            _state.fading = true;
            _state.fadeStartMs = 0; // first tick initialises
        }
        _state.active = OpenRCT2::News::Severity::info;
    }

    void tickEngine(uint32_t nowMs)
    {
        // Steady hold while active. submit() is hysteresis-guarded so
        // this is cheap when the colour hasn't changed.
        if (_state.active != OpenRCT2::News::Severity::info)
        {
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            severityColour(_state.active, r, g, b);
            submit(r, g, b);
            return;
        }

        // Fade-out ramp after Clear().
        if (_state.fading)
        {
            // First-frame init: latch the actual start time so the
            // fade math uses a real playhead. Done here (instead of
            // in Clear()) to keep Clear() free of platform-time deps.
            if (_state.fadeStartMs == 0)
                _state.fadeStartMs = nowMs == 0 ? 1 : nowMs;

            const uint32_t elapsed = nowMs - _state.fadeStartMs;
            if (elapsed >= kFadeMs)
            {
                submit(0, 0, 0);
                _state.fading = false;
                return;
            }

            const float t = static_cast<float>(elapsed) / static_cast<float>(kFadeMs);
            const float intensity = std::clamp(1.0f - t, 0.0f, 1.0f);
            const auto scale = [intensity](uint8_t v) -> uint8_t {
                const auto s = static_cast<int32_t>(std::lround(static_cast<float>(v) * intensity));
                return static_cast<uint8_t>(std::clamp(s, 0, 255));
            };
            submit(scale(_state.fadeR), scale(_state.fadeG), scale(_state.fadeB));
            return;
        }

        // Idle — already at off, nothing to do. The hysteresis cache
        // remembers we last submitted (0,0,0) so no SDL spam.
    }

    void forceClear()
    {
        _state.active = OpenRCT2::News::Severity::info;
        _state.fading = false;
        // Force the next submit to go through even if the last
        // colour was already (0,0,0) — the user might have a manual
        // LED-test sitting at black and expect this to be a real
        // sweep. Cheap insurance.
        _lastR = -1;
        _lastG = -1;
        _lastB = -1;
        submit(0, 0, 0);
    }
} // namespace OpenRCT2::Led
