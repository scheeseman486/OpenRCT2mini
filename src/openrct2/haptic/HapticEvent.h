/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../audio/Audio.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// OPENRCT2MINI gamepad-plan 1.11 + 1.11b: haptic-feedback facade.
//
// Two layers live here:
//
//   1. The thin `pulse` / `rumble` API from 1.11. Synchronous helpers
//      that walk every connected SDL game controller and submit a
//      one-shot rumble pulse via UiContext::RumbleControllers. Used by
//      the basic Haptics window's Test button and any future direct-
//      fire paths that don't go through a SoundId.
//
//   2. The 1.11b SoundId-mapped envelope engine. Game code calls
//      `onSoundPlayed` (from Audio::Play) and `updateContinuousSound`
//      (from Vehicle::UpdateSound). The engine stores a per-SoundId
//      RumbleProfile (a dual-motor envelope) and ticks per-frame to
//      sample envelopes against the current playhead, mixing one-shot
//      and continuous contributions and pushing rumble updates to
//      SDL with TTL-based hysteresis to avoid per-frame SDL spam.
//
// The Rumble Editor (1.11b debug window) is the only writer that
// modifies the in-memory `SoundRumbleMap`. It also drives JSON
// persistence at `<userDataPath>/rumble.json` (atomic temp+rename,
// debounced 500 ms after last edit).

namespace OpenRCT2::Haptic
{
    // ---- Data model (1.11b.1) -------------------------------------------

    enum class RumbleMode : uint8_t
    {
        // Envelope plays once on Audio::Play(). Rumble cancels at
        // envelope end (t > envelopeDurationMs).
        oneShot = 0,
        // Envelope loops while a VehicleSound for this SoundId is
        // alive on the followed vehicle. Each evaluated motor
        // intensity is multiplied by the live (audioVolume / 255)
        // before SDL — the envelope gives the *shape*, the audio
        // volume gives the *strength*.
        continuous = 1,
    };

    // One envelope control point: timeMs within the envelope's
    // duration, target motor intensity at that point.
    struct EnvelopePoint
    {
        uint32_t timeMs = 0;
        float intensity = 0.0f; // 0..1
    };

    struct MotorEnvelope
    {
        // Sorted ascending by timeMs. Editor maintains the invariant
        // on every insert / drag-release. Empty vector ⇒ motor
        // disabled for this SoundId.
        std::vector<EnvelopePoint> points;
    };

    struct RumbleProfile
    {
        RumbleMode mode = RumbleMode::oneShot;
        // Envelope total duration. Defaults to the audio sample's
        // natural length when first authored, but is independent
        // thereafter — the user can extend or shorten it. For
        // continuous mode the envelope loops at this period.
        uint32_t envelopeDurationMs = 0;
        MotorEnvelope low;
        MotorEnvelope high;

        // OPENRCT2MINI v2.19: per-profile authoring flags.
        //
        // `disabled` — user-set kill switch. When true, the engine
        // skips this profile entirely: onSoundPlayed adds no entry,
        // updateContinuousSound is a no-op. Lets the user author
        // and store envelope curves without firing rumble (useful
        // for profiles they want to keep around but not currently
        // play, e.g. while comparing different shapes).
        //
        // `scaleWithRate` — only meaningful for continuous mode.
        // When true, the engine multiplies the envelope's evaluated
        // intensity by a rate-derived factor each tick so slower
        // audio (low velocity, low playback rate) produces weaker
        // rumble. Track friction loops are the obvious use case:
        // a stationary train shouldn't buzz the controller as
        // hard as one at speed.
        //
        // `rateScalePercent` — strength of the above reduction,
        // 0..100. At 0 there is no reduction (equivalent to flag
        // off). At 100 the reduction is fully proportional: at
        // rate 0.5 the intensity is halved. Intermediate values
        // produce a softer roll-off. Stored as uint8_t so the
        // JSON layer can serialise it as a small integer.
        bool disabled = false;
        bool scaleWithRate = false;
        uint8_t rateScalePercent = 100;
    };

    using SoundRumbleMap = std::unordered_map<Audio::SoundId, RumbleProfile>;

    // Evaluate envelope at time `t` (0..envelopeDurationMs). Catmull-
    // Rom-derived cubic bezier between adjacent points (auto-tangent
    // at endpoints). Clamp to first/last point's intensity outside
    // the bracketing segment. Empty envelope ⇒ 0.
    float evaluate(const MotorEnvelope& env, uint32_t t);

    // ---- Direct-fire API (1.11) -----------------------------------------

    // Symmetric pulse — both motors at the same magnitude. Bypasses
    // the envelope engine; used by the Haptics window's Test button.
    void pulse(float intensity, uint32_t durationMs);

    // Independent low / high motor magnitudes. Same bypass as pulse.
    void rumble(float low, float high, uint32_t durationMs);

    // ---- Engine API (1.11b) ---------------------------------------------

    // Called from Audio::Play(SoundId, …). One-shot lookup ⇒ pushes
    // a fresh active-playhead entry whose envelope plays once.
    // Continuous-mode entries are ignored here (they're driven from
    // updateContinuousSound). No-op if the SoundId has no profile.
    void onSoundPlayed(Audio::SoundId soundId);

    // Called from Vehicle::UpdateSound for each active VehicleSound,
    // once per tick. Refreshes (or creates) the active-playhead
    // entry for this SoundId with the current normalised audio
    // volume + the audio channel's playback rate (1.0 = nominal,
    // higher = pitch-shifted up by vehicle speed for track friction).
    // The per-frame tick advances the entry's envelope phase by
    // `(delta_ms * rate)` each frame so the rumble loop period
    // tracks the audio loop period as the vehicle accelerates and
    // decelerates. Audio volume = 0..255 from VehicleSound::volume.
    void updateContinuousSound(Audio::SoundId soundId, uint8_t audioVolume, float rate = 1.0f);

    // Called once per frame at the bottom of InputManager::process.
    // Walks the active-playhead queue, evaluates envelopes, mixes
    // contributions, and pushes deltas to UiContext::RumbleControllers.
    // Retires expired entries (one-shot whose envelope finished, or
    // continuous whose `lastSeenMs` is older than this frame).
    void tickEngine(uint32_t nowMs);

    // ---- Map accessors (used by editor + JSON layer) --------------------

    // Returns the live in-memory map. Editor reads + writes through
    // this; JSON layer reads it for save and writes it on load.
    SoundRumbleMap& getMap();

    // Loads `<userDataPath>/rumble.json` into the in-memory map.
    // On missing / malformed file, seeds the built-in default
    // profile (HapticDefaults). Idempotent — calling twice replaces
    // the in-memory map.
    void loadProfilesFromDisk();

    // Atomic write of the in-memory map back to rumble.json.
    // Debounced by callers via the editor's auto-save logic.
    void saveProfilesToDisk();
} // namespace OpenRCT2::Haptic
