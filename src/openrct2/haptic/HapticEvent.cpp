/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "HapticEvent.h"

#include "../Context.h"
#include "../Diagnostic.h"
#include "../PlatformEnvironment.h"
#include "../config/Config.h"
#include "../core/Console.hpp"
#include "../core/Json.hpp"
#include "../core/Path.hpp"
#include "../platform/Platform.h"
#include "../ui/UiContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

// OPENRCT2MINI gamepad-plan 1.11 + 1.11b: haptic engine.
//
// Architecture:
//
//   * `_map` is the in-memory SoundRumbleMap, the single source of
//     truth for per-SoundId envelopes. Editor reads + writes through
//     getMap(). JSON layer reads it for save and writes it on load.
//
//   * `_active` is the per-frame active-playhead queue. One-shot pushes
//     a fresh entry on Audio::Play; continuous refreshes (or creates)
//     an entry once per Vehicle::UpdateSound tick. tickEngine walks
//     the queue each frame, evaluates envelopes, mixes contributions,
//     and pushes deltas to UiContext::RumbleControllers.
//
//   * `_lastEmittedLow / High` track the magnitude we last asked SDL
//     for. We only re-issue SDL_GameControllerRumble (via
//     RumbleControllers) when the rounded magnitudes change — calling
//     it every tick at the same value is wasteful and on some pads
//     causes a click on each call.
//
//   * Per-frame entries pass a 200 ms TTL to RumbleControllers, which
//     SDL uses as the auto-cancel timer. The next frame re-issues
//     with a fresh TTL ⇒ effectively continuous as long as
//     tickEngine keeps refreshing.

namespace OpenRCT2::Haptic
{
    // ---- SoundId ↔ name table (used by JSON layer) ---------------------

    namespace
    {
        // Compact lowerCamel name for each SoundId enumerator. Matches
        // the order in Audio.h's enum so a runtime contains-check is a
        // simple linear scan; serialisation order is enum-declaration
        // order, which keeps rumble.json stable across rebuilds.
        struct NameEntry
        {
            Audio::SoundId id;
            std::string_view name;
        };
        constexpr std::array<NameEntry, 65> kSoundIdNames = { {
            { Audio::SoundId::liftClassic,             "liftClassic" },
            { Audio::SoundId::trackFrictionClassicWood,"trackFrictionClassicWood" },
            { Audio::SoundId::frictionClassic,         "frictionClassic" },
            { Audio::SoundId::scream1,                 "scream1" },
            { Audio::SoundId::click1,                  "click1" },
            { Audio::SoundId::click2,                  "click2" },
            { Audio::SoundId::placeItem,               "placeItem" },
            { Audio::SoundId::scream2,                 "scream2" },
            { Audio::SoundId::scream3,                 "scream3" },
            { Audio::SoundId::scream4,                 "scream4" },
            { Audio::SoundId::scream5,                 "scream5" },
            { Audio::SoundId::scream6,                 "scream6" },
            { Audio::SoundId::liftFrictionWheels,      "liftFrictionWheels" },
            { Audio::SoundId::purchase,                "purchase" },
            { Audio::SoundId::crash,                   "crash" },
            { Audio::SoundId::layingOutWater,          "layingOutWater" },
            { Audio::SoundId::water1,                  "water1" },
            { Audio::SoundId::water2,                  "water2" },
            { Audio::SoundId::trainWhistle,            "trainWhistle" },
            { Audio::SoundId::trainDeparting,          "trainDeparting" },
            { Audio::SoundId::waterSplash,             "waterSplash" },
            { Audio::SoundId::goKartEngine,            "goKartEngine" },
            { Audio::SoundId::rideLaunch1,             "rideLaunch1" },
            { Audio::SoundId::rideLaunch2,             "rideLaunch2" },
            { Audio::SoundId::cough1,                  "cough1" },
            { Audio::SoundId::cough2,                  "cough2" },
            { Audio::SoundId::cough3,                  "cough3" },
            { Audio::SoundId::cough4,                  "cough4" },
            { Audio::SoundId::rain,                    "rain" },
            { Audio::SoundId::thunder1,                "thunder1" },
            { Audio::SoundId::thunder2,                "thunder2" },
            { Audio::SoundId::trackFrictionTrain,      "trackFrictionTrain" },
            { Audio::SoundId::trackFrictionWater,      "trackFrictionWater" },
            { Audio::SoundId::balloonPop,              "balloonPop" },
            { Audio::SoundId::mechanicFix,             "mechanicFix" },
            { Audio::SoundId::scream7,                 "scream7" },
            { Audio::SoundId::toiletFlush,             "toiletFlush" },
            { Audio::SoundId::click3,                  "click3" },
            { Audio::SoundId::quack,                   "quack" },
            { Audio::SoundId::newsItem,                "newsItem" },
            { Audio::SoundId::windowOpen,              "windowOpen" },
            { Audio::SoundId::laugh1,                  "laugh1" },
            { Audio::SoundId::laugh2,                  "laugh2" },
            { Audio::SoundId::laugh3,                  "laugh3" },
            { Audio::SoundId::applause,                "applause" },
            { Audio::SoundId::hauntedHouseScare,       "hauntedHouseScare" },
            { Audio::SoundId::hauntedHouseScream1,     "hauntedHouseScream1" },
            { Audio::SoundId::hauntedHouseScream2,     "hauntedHouseScream2" },
            { Audio::SoundId::blockBrakeClose,         "blockBrakeClose" },
            { Audio::SoundId::blockBrakeRelease,       "blockBrakeRelease" },
            { Audio::SoundId::error,                   "error" },
            { Audio::SoundId::brakeRelease,            "brakeRelease" },
            { Audio::SoundId::liftArrow,               "liftArrow" },
            { Audio::SoundId::liftWood,                "liftWood" },
            { Audio::SoundId::trackFrictionWood,       "trackFrictionWood" },
            { Audio::SoundId::liftWildMouse,           "liftWildMouse" },
            { Audio::SoundId::liftBM,                  "liftBM" },
            { Audio::SoundId::trackFrictionBM,         "trackFrictionBM" },
            { Audio::SoundId::scream8,                 "scream8" },
            { Audio::SoundId::tram,                    "tram" },
            { Audio::SoundId::doorOpen,                "doorOpen" },
            { Audio::SoundId::doorClose,               "doorClose" },
            { Audio::SoundId::portcullis,              "portcullis" },
            { Audio::SoundId::crowdAmbience,           "crowdAmbience" },
            { Audio::SoundId::liftRMC,                 "liftRMC" },
        } };

        [[maybe_unused]] std::string_view nameForSoundId(Audio::SoundId id)
        {
            for (const auto& e : kSoundIdNames)
                if (e.id == id)
                    return e.name;
            return {};
        }

        std::optional<Audio::SoundId> soundIdForName(std::string_view name)
        {
            for (const auto& e : kSoundIdNames)
                if (e.name == name)
                    return e.id;
            return std::nullopt;
        }

        // The in-memory map. Single instance, populated by
        // loadProfilesFromDisk on first access (lazy boot).
        SoundRumbleMap _map;

        // Lazy-load latch. Set on the first call to any access path
        // (onSoundPlayed, updateContinuousSound, tickEngine,
        // getMap). The Rumble Editor's Open path also calls
        // loadProfilesFromDisk explicitly to ensure the map is
        // populated before it tries to render entries — both paths
        // are idempotent, so calling twice is harmless.
        bool _loaded = false;
        void ensureLoaded();

        // ---- Active-playhead queue --------------------------------------

        struct ActiveEntry
        {
            Audio::SoundId soundId;
            RumbleMode mode;
            uint32_t startMs;       // one-shot — defines envelope phase via wall delta
            uint32_t lastSeenMs;    // continuous only — retire if older than this frame
            float audioVolumeNorm;  // continuous only — 0..1, refreshed each tick
            // OPENRCT2MINI v2.18: rate-aware continuous phase. Audio
            // for track friction / scream loops is pitch-shifted with
            // vehicle speed (channel SetRate). The rumble loop period
            // has to track the same rate or it desyncs from the audio
            // (you'd feel rumble cycling at idle tempo while the
            // audio whines up to 1.5x). For continuous entries we
            // ignore startMs and instead integrate phase ourselves
            // each tick: `phaseMs += (now - lastTickMs) * rate`. The
            // envelope is then evaluated at `phaseMs % envDur`.
            // Rate defaults to 1.0 so existing call sites are no-ops.
            float phaseMs;          // continuous only — accumulated rumble-time
            float rate;             // continuous only — last-known channel SetRate
            uint32_t lastTickMs;    // continuous only — for delta integration
        };
        std::vector<ActiveEntry> _active;

        // Last magnitudes we asked SDL for (after global scaling).
        // Used to skip redundant SDL calls when the envelope isn't
        // moving.
        float _lastEmittedLow = 0.0f;
        float _lastEmittedHigh = 0.0f;
        uint32_t _lastEmittedTickMs = 0;

        // TTL we pass to UiContext::RumbleControllers each frame —
        // long enough to absorb a missed-frame stall, short enough to
        // act as an auto-cancel safety net if tickEngine stops being
        // called for some reason.
        constexpr uint32_t kPerFrameTTL = 200;

        // Floor below which we treat magnitude as zero. Avoids
        // perpetual motor hum at trace amplitudes (and lets us emit
        // a single "stop" call when an envelope fades through ~0.05).
        constexpr float kZeroFloor = 0.02f;
    } // namespace

    // ---- Envelope evaluator (1.11b.1) ----------------------------------

    namespace
    {
        // Catmull-Rom-tangent cubic Bezier between two points. We
        // mirror neighbour points at the endpoints so curves don't
        // kink at boundaries.
        float catmullRomBezier(
            float t,                 // 0..1 within segment
            float p0, float p1,      // segment endpoints
            float pPrev, float pNext // neighbours (mirrored at ends)
        )
        {
            // Catmull-Rom tangents (tension = 0.5 per the standard
            // form):  m_i = (P_{i+1} - P_{i-1}) / 2
            const float m0 = 0.5f * (p1 - pPrev);
            const float m1 = 0.5f * (pNext - p0);
            // Hermite -> cubic Bezier control points:
            //   c0 = p0, c1 = p0 + m0/3, c2 = p1 - m1/3, c3 = p1
            // Standard cubic Bezier evaluation in t.
            const float c0 = p0;
            const float c1 = p0 + m0 / 3.0f;
            const float c2 = p1 - m1 / 3.0f;
            const float c3 = p1;
            const float u = 1.0f - t;
            return c0 * (u * u * u) + 3.0f * c1 * (u * u * t) + 3.0f * c2 * (u * t * t) + c3 * (t * t * t);
        }
    } // namespace

    float evaluate(const MotorEnvelope& env, uint32_t tMs)
    {
        if (env.points.empty())
            return 0.0f;
        if (env.points.size() == 1)
            return std::clamp(env.points.front().intensity, 0.0f, 1.0f);

        // Endpoint clamping.
        if (tMs <= env.points.front().timeMs)
            return std::clamp(env.points.front().intensity, 0.0f, 1.0f);
        if (tMs >= env.points.back().timeMs)
            return std::clamp(env.points.back().intensity, 0.0f, 1.0f);

        // Find segment [i, i+1] with points[i].timeMs <= t < points[i+1].timeMs.
        size_t i = 0;
        for (; i + 1 < env.points.size(); i++)
        {
            if (env.points[i].timeMs <= tMs && tMs < env.points[i + 1].timeMs)
                break;
        }

        const auto& p0 = env.points[i];
        const auto& p1 = env.points[i + 1];
        // Mirror at endpoints so the auto-tangent doesn't kink.
        const float prev = (i == 0) ? p0.intensity : env.points[i - 1].intensity;
        const float next = (i + 2 >= env.points.size()) ? p1.intensity : env.points[i + 2].intensity;

        const uint32_t segDur = p1.timeMs - p0.timeMs;
        if (segDur == 0)
            return std::clamp(p1.intensity, 0.0f, 1.0f);
        const float u = static_cast<float>(tMs - p0.timeMs) / static_cast<float>(segDur);
        return std::clamp(catmullRomBezier(u, p0.intensity, p1.intensity, prev, next), 0.0f, 1.0f);
    }

    // ---- Direct-fire API (1.11) -----------------------------------------

    namespace
    {
        OpenRCT2::Ui::IUiContext* tryGetUiContext()
        {
            auto* ctx = GetContext();
            if (ctx == nullptr)
                return nullptr;
            return &ctx->GetUiContext();
        }
    } // namespace

    void pulse(float intensity, uint32_t durationMs)
    {
        if (auto* ui = tryGetUiContext(); ui != nullptr)
            ui->RumbleControllers(intensity, intensity, durationMs);
    }

    void rumble(float low, float high, uint32_t durationMs)
    {
        if (auto* ui = tryGetUiContext(); ui != nullptr)
            ui->RumbleControllers(low, high, durationMs);
    }

    // ---- Engine API (1.11b) ---------------------------------------------

    void onSoundPlayed(Audio::SoundId soundId)
    {
        ensureLoaded();
        // OPENRCT2MINI input-plan Track 2 §4.3: per-event categorical
        // gate. Sits ahead of the per-profile disabled flag so the
        // user can mute an entire category (e.g. all construction-
        // refusal rumble) without re-authoring the envelope. The map
        // is intentionally tiny — only SoundIds the plan explicitly
        // calls out as user-facing events get a category. Everything
        // else (brakes, screams, click sounds) falls through to the
        // existing per-profile flag.
        {
            const auto& gpCfg = Config::Get().general;
            if (soundId == Audio::SoundId::crash && !gpCfg.gamepadRumbleOnCrash)
                return;
            if (soundId == Audio::SoundId::error && !gpCfg.gamepadRumbleOnConstructionRefusal)
                return;
        }
        auto it = _map.find(soundId);
        if (it == _map.end())
            return;
        const auto& profile = it->second;
        // OPENRCT2MINI v2.19: per-profile kill switch.
        if (profile.disabled)
            return;
        // Continuous SoundIds shouldn't double-fire on Audio::Play —
        // they're driven by Vehicle::UpdateSound's per-tick poll.
        // Defensive: ignore them here.
        if (profile.mode != RumbleMode::oneShot)
            return;
        if (profile.envelopeDurationMs == 0)
            return;
        if (profile.low.points.empty() && profile.high.points.empty())
            return;

        const uint32_t now = Platform::GetTicks();
        // Re-firing the same one-shot before its envelope finishes
        // restarts the playhead at 0 (rather than layering two
        // envelopes). Matches SDL's overwrite-in-progress behaviour.
        for (auto& e : _active)
        {
            if (e.soundId == soundId && e.mode == RumbleMode::oneShot)
            {
                e.startMs = now;
                return;
            }
        }
        _active.push_back({ soundId, RumbleMode::oneShot, now, now, 1.0f, 0.0f, 1.0f, now });
    }

    void updateContinuousSound(Audio::SoundId soundId, uint8_t audioVolume, float rate)
    {
        ensureLoaded();
        auto it = _map.find(soundId);
        if (it == _map.end())
            return;
        const auto& profile = it->second;
        // OPENRCT2MINI v2.19: per-profile kill switch.
        if (profile.disabled)
            return;
        if (profile.mode != RumbleMode::continuous)
            return;
        if (profile.envelopeDurationMs == 0)
            return;
        if (profile.low.points.empty() && profile.high.points.empty())
            return;

        const uint32_t now = Platform::GetTicks();
        const float vol = static_cast<float>(audioVolume) / 255.0f;
        // Sanity-clamp the rate. Audio's natural base offset is ~0.5
        // at v=0 and ranges up past 1.0 at speed; clamp to [0.1, 8.0]
        // so a degenerate input can't freeze (rate 0) or run away.
        const float clampedRate = std::clamp(rate, 0.1f, 8.0f);
        for (auto& e : _active)
        {
            if (e.soundId == soundId && e.mode == RumbleMode::continuous)
            {
                e.lastSeenMs = now;
                e.rate = clampedRate;
                // Take the loudest contender if multiple Vehicles
                // assert the same SoundId in one frame (shouldn't
                // happen if focus-scoping in the call site is right;
                // defensive).
                if (vol > e.audioVolumeNorm)
                    e.audioVolumeNorm = vol;
                return;
            }
        }
        _active.push_back({ soundId, RumbleMode::continuous, now, now, vol, 0.0f, clampedRate, now });
    }

    void tickEngine(uint32_t nowMs)
    {
        // Cheap fast path — if there's no rumble work and no profiles
        // loaded yet, skip the lazy-load disk read every frame. The
        // load fires lazily on the first onSoundPlayed /
        // updateContinuousSound / getMap call instead.
        if (!_loaded && _active.empty())
            return;

        // Sum contributions across active entries.
        float lowSum = 0.0f;
        float highSum = 0.0f;

        for (auto it = _active.begin(); it != _active.end();)
        {
            auto& entry = *it;
            const auto mapIt = _map.find(entry.soundId);
            if (mapIt == _map.end())
            {
                // Entry's profile got deleted out from under us. Drop.
                it = _active.erase(it);
                continue;
            }
            const auto& profile = mapIt->second;

            uint32_t t = 0;
            bool retire = false;
            if (entry.mode == RumbleMode::oneShot)
            {
                t = (nowMs >= entry.startMs) ? (nowMs - entry.startMs) : 0u;
                if (t > profile.envelopeDurationMs)
                {
                    retire = true;
                }
            }
            else
            {
                // OPENRCT2MINI v2.18: continuous mode integrates its
                // own phase rather than using wall-clock subtraction.
                // Each tick we advance phaseMs by the wall delta
                // multiplied by the current audio rate, so the rumble
                // loop period speeds up and slows down in lockstep
                // with the audio's pitch (track friction rate scales
                // with vehicle velocity → DStoMixerRate). Wrap at
                // envDur so the envelope still loops the same shape.
                if (nowMs >= entry.lastTickMs)
                {
                    const uint32_t deltaMs = nowMs - entry.lastTickMs;
                    entry.phaseMs += static_cast<float>(deltaMs) * entry.rate;
                }
                entry.lastTickMs = nowMs;
                if (profile.envelopeDurationMs > 0)
                {
                    const float dur = static_cast<float>(profile.envelopeDurationMs);
                    if (entry.phaseMs >= dur)
                        entry.phaseMs = std::fmod(entry.phaseMs, dur);
                    t = static_cast<uint32_t>(entry.phaseMs);
                }
                // Retire if Vehicle::UpdateSound stopped refreshing
                // this entry. Last-seen this frame ⇒ keep alive.
                // Allow a small grace window — 100 ms — so a single
                // missed tick doesn't glitch-cancel.
                if (nowMs - entry.lastSeenMs > 100)
                    retire = true;
            }

            if (retire)
            {
                it = _active.erase(it);
                continue;
            }

            float lowEnv = evaluate(profile.low, t);
            float highEnv = evaluate(profile.high, t);
            if (entry.mode == RumbleMode::continuous)
            {
                lowEnv *= entry.audioVolumeNorm;
                highEnv *= entry.audioVolumeNorm;

                // OPENRCT2MINI v2.19: optional intensity-with-speed
                // reduction. Audio for track friction loops is
                // pitch-shifted by velocity (rate < 1 at low speed,
                // > 1 at high speed). When the user enables the flag,
                // scale the envelope down by the rate, capped at 1.0
                // so faster-than-nominal audio doesn't BOOST rumble.
                //
                //   factor = 1 - (1 - min(rate, 1.0)) * strength
                //
                // strength = rateScalePercent / 100. At 100% the
                // reduction is fully proportional (rate 0.5 → half
                // intensity); at 0% the multiplier is 1 (no-op,
                // identical to flag off). Intermediate values give
                // a softer roll-off so a stopped train can still
                // hum gently if the user wants.
                if (profile.scaleWithRate)
                {
                    const float clampedRate = std::min(entry.rate, 1.0f);
                    const float strength = static_cast<float>(profile.rateScalePercent) / 100.0f;
                    const float factor = std::clamp(1.0f - (1.0f - clampedRate) * strength, 0.0f, 1.0f);
                    lowEnv *= factor;
                    highEnv *= factor;
                }
            }
            lowSum += lowEnv;
            highSum += highEnv;

            ++it;
        }

        // Clamp and floor the sum.
        lowSum = std::clamp(lowSum, 0.0f, 1.0f);
        highSum = std::clamp(highSum, 0.0f, 1.0f);
        if (lowSum < kZeroFloor)
            lowSum = 0.0f;
        if (highSum < kZeroFloor)
            highSum = 0.0f;

        // Hysteresis on SDL submission: only re-issue when rounded
        // magnitudes (approx 1/64 step ≈ 0.0156) differ from last
        // emit, OR when the previous submission's TTL is about to
        // run out (so we don't drop active rumble).
        constexpr float kEpsilon = 0.015f;
        const bool wantsActive = (lowSum > 0.0f || highSum > 0.0f);
        const bool wasActive = (_lastEmittedLow > 0.0f || _lastEmittedHigh > 0.0f);
        const bool magChanged = std::fabs(lowSum - _lastEmittedLow) > kEpsilon
            || std::fabs(highSum - _lastEmittedHigh) > kEpsilon;
        const bool refreshDue = (nowMs - _lastEmittedTickMs) > (kPerFrameTTL / 2);

        if ((wantsActive && (magChanged || refreshDue)) || (wasActive && !wantsActive))
        {
            if (auto* ui = tryGetUiContext(); ui != nullptr)
            {
                if (wantsActive)
                    ui->RumbleControllers(lowSum, highSum, kPerFrameTTL);
                else
                    ui->RumbleControllers(0.0f, 0.0f, 0);
            }
            _lastEmittedLow = lowSum;
            _lastEmittedHigh = highSum;
            _lastEmittedTickMs = nowMs;
        }
    }

    SoundRumbleMap& getMap()
    {
        ensureLoaded();
        return _map;
    }

    // ---- Default profile (1.11b.8) -------------------------------------

    namespace
    {
        // Helper to build a flat envelope (two endpoint points).
        MotorEnvelope flatEnv(uint32_t durMs, float intensity)
        {
            return MotorEnvelope{
                std::vector<EnvelopePoint>{ { 0u, intensity }, { durMs, intensity } }
            };
        }

        // Helper to build a 3-point peak-and-decay envelope:
        //   (0, 0) → (peakMs, peak) → (durMs, 0)
        MotorEnvelope peakEnv(uint32_t durMs, uint32_t peakMs, float peak)
        {
            return MotorEnvelope{
                std::vector<EnvelopePoint>{ { 0u, 0.0f }, { peakMs, peak }, { durMs, 0.0f } }
            };
        }

        void seedDefaults()
        {
            _map.clear();

            // Crash — strong attack, fast decay, long tail.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 500;
                p.low.points = { { 0, 0.0f }, { 20, 1.0f }, { 200, 0.4f }, { 500, 0.0f } };
                p.high.points = { { 0, 0.0f }, { 10, 0.8f }, { 80, 0.2f }, { 200, 0.0f } };
                _map[Audio::SoundId::crash] = std::move(p);
            }

            // Error — sharp high-frequency buzz.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 120;
                p.high = peakEnv(120, 10, 0.8f);
                _map[Audio::SoundId::error] = std::move(p);
            }

            // Place item — soft tick.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 60;
                p.high = peakEnv(60, 5, 0.4f);
                _map[Audio::SoundId::placeItem] = std::move(p);
            }

            // Block brake close / release — subtle thud.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 80;
                p.low = peakEnv(80, 0, 0.3f);
                _map[Audio::SoundId::blockBrakeClose] = std::move(p);
            }
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 60;
                p.low = peakEnv(60, 0, 0.2f);
                _map[Audio::SoundId::blockBrakeRelease] = std::move(p);
            }

            // Water splash — bigger thud + buzz.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 250;
                p.low = peakEnv(250, 30, 0.4f);
                p.high = peakEnv(100, 20, 0.2f);
                _map[Audio::SoundId::waterSplash] = std::move(p);
            }

            // Ride launch — kick + buzz tail.
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 350;
                p.low = peakEnv(350, 50, 0.8f);
                p.high = peakEnv(150, 50, 0.4f);
                _map[Audio::SoundId::rideLaunch1] = std::move(p);
            }
            {
                RumbleProfile p;
                p.mode = RumbleMode::oneShot;
                p.envelopeDurationMs = 350;
                p.low = peakEnv(350, 50, 0.8f);
                p.high = peakEnv(150, 50, 0.4f);
                _map[Audio::SoundId::rideLaunch2] = std::move(p);
            }

            // Continuous — flat envelopes, audio volume drives
            // perceived strength. Track friction & lift hills mostly
            // low-frequency to feel like wheels on rails.
            const auto contLow = [](Audio::SoundId id, uint32_t dur, float lowI, float highI) {
                RumbleProfile p;
                p.mode = RumbleMode::continuous;
                p.envelopeDurationMs = dur;
                if (lowI > 0.0f)
                    p.low = flatEnv(dur, lowI);
                if (highI > 0.0f)
                    p.high = flatEnv(dur, highI);
                _map[id] = std::move(p);
            };
            contLow(Audio::SoundId::trackFrictionTrain,       250, 0.50f, 0.10f);
            contLow(Audio::SoundId::trackFrictionWater,       250, 0.50f, 0.10f);
            contLow(Audio::SoundId::trackFrictionWood,        250, 0.50f, 0.10f);
            contLow(Audio::SoundId::trackFrictionBM,          250, 0.50f, 0.10f);
            contLow(Audio::SoundId::trackFrictionClassicWood, 250, 0.50f, 0.10f);
            contLow(Audio::SoundId::frictionClassic,          250, 0.50f, 0.10f);
            contLow(Audio::SoundId::liftClassic,              250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftWildMouse,            250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftBM,                   250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftRMC,                  250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftWood,                 250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftArrow,                250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftFlume,                250, 0.40f, 0.0f);
            contLow(Audio::SoundId::liftFrictionWheels,       250, 0.40f, 0.0f);
            contLow(Audio::SoundId::goKartEngine,             200, 0.30f, 0.20f);
            contLow(Audio::SoundId::scream1, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream2, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream3, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream4, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream5, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream6, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::scream7, 300, 0.0f, 0.30f);
            contLow(Audio::SoundId::crowdAmbience, 500, 0.05f, 0.0f);
        }

        // ---- JSON (de)serialisation -------------------------------------

        std::string getRumbleJsonPath()
        {
            auto* ctx = GetContext();
            if (ctx == nullptr)
                return {};
            const auto& env = ctx->GetPlatformEnvironment();
            return Path::Combine(env.GetDirectoryPath(DirBase::user), "rumble.json");
        }

        json_t envToJson(const MotorEnvelope& env)
        {
            json_t arr = json_t::array();
            for (const auto& p : env.points)
            {
                json_t pt = json_t::object();
                pt["t"] = p.timeMs;
                pt["i"] = p.intensity;
                arr.push_back(std::move(pt));
            }
            return arr;
        }

        MotorEnvelope envFromJson(const json_t& arr)
        {
            MotorEnvelope env;
            if (!arr.is_array())
                return env;
            env.points.reserve(arr.size());
            for (const auto& pt : arr)
            {
                if (!pt.is_object())
                    continue;
                EnvelopePoint p;
                p.timeMs = Json::GetNumber<uint32_t>(pt["t"], 0u);
                p.intensity = Json::GetNumber<float>(pt["i"], 0.0f);
                env.points.push_back(p);
            }
            // Defensive: ensure sorted-ascending by time.
            std::sort(env.points.begin(), env.points.end(), [](const auto& a, const auto& b) {
                return a.timeMs < b.timeMs;
            });
            return env;
        }
    } // namespace

    void loadProfilesFromDisk()
    {
        const auto path = getRumbleJsonPath();
        if (path.empty())
        {
            seedDefaults();
            return;
        }

        json_t root;
        try
        {
            root = Json::ReadFromFile(path);
        }
        catch (...)
        {
            // Missing or malformed — fall back to defaults.
            seedDefaults();
            return;
        }

        if (!root.is_object())
        {
            seedDefaults();
            return;
        }

        const auto version = Json::GetNumber<int32_t>(root["version"], 1);
        const auto sfx = root.contains("soundEffects") ? Json::AsObject(root["soundEffects"]) : json_t::object();

        _map.clear();
        for (auto it = sfx.begin(); it != sfx.end(); ++it)
        {
            const auto sid = soundIdForName(it.key());
            if (!sid.has_value())
                continue;
            const auto& obj = it.value();
            if (!obj.is_object())
                continue;

            RumbleProfile p;
            const auto modeStr = Json::GetString(obj["mode"], "oneShot");
            p.mode = (modeStr == "continuous") ? RumbleMode::continuous : RumbleMode::oneShot;
            p.envelopeDurationMs = Json::GetNumber<uint32_t>(obj["envelopeDurationMs"], 0u);
            // OPENRCT2MINI v2.19: new per-profile flags. Default to
            // off / 100 so older JSONs (which don't have these keys)
            // keep behaving as before.
            p.disabled = Json::GetBoolean(obj["disabled"], false);
            p.scaleWithRate = Json::GetBoolean(obj["scaleWithRate"], false);
            p.rateScalePercent = static_cast<uint8_t>(
                std::clamp<int32_t>(Json::GetNumber<int32_t>(obj["rateScalePercent"], 100), 0, 100));

            if (version >= 2)
            {
                p.low = obj.contains("low") ? envFromJson(obj["low"]) : MotorEnvelope{};
                p.high = obj.contains("high") ? envFromJson(obj["high"]) : MotorEnvelope{};
            }
            else
            {
                // v1 migration: legacy MotorPulse {intensity, durationMs}
                // → flat envelope ramping to 0 at the end.
                auto migrate = [](const json_t& m) -> MotorEnvelope {
                    if (!m.is_object())
                        return {};
                    const auto inten = Json::GetNumber<float>(m["intensity"], 0.0f);
                    const auto dur = Json::GetNumber<uint32_t>(m["durationMs"], 0u);
                    if (inten <= 0.0f || dur == 0u)
                        return {};
                    MotorEnvelope env;
                    env.points = { { 0u, inten }, { dur, inten }, { dur, 0.0f } };
                    return env;
                };
                p.low = migrate(obj["low"]);
                p.high = migrate(obj["high"]);
                if (p.envelopeDurationMs == 0)
                {
                    uint32_t maxT = 0;
                    for (const auto& pt : p.low.points)
                        maxT = std::max(maxT, pt.timeMs);
                    for (const auto& pt : p.high.points)
                        maxT = std::max(maxT, pt.timeMs);
                    p.envelopeDurationMs = maxT;
                }
            }
            _map[*sid] = std::move(p);
        }

        // If file existed but had zero entries (or all parses
        // failed), still want a sensible baseline — only seed
        // defaults if completely empty.
        if (_map.empty())
            seedDefaults();
    }

    namespace
    {
        void ensureLoaded()
        {
            if (_loaded)
                return;
            // Mark loaded BEFORE the recursive call so a failed load
            // path that calls back into ensureLoaded doesn't infinite-
            // recurse. seedDefaults() is the worst-case fallback and
            // doesn't touch I/O so it's safe.
            _loaded = true;
            loadProfilesFromDisk();
        }
    } // namespace

    void saveProfilesToDisk()
    {
        const auto path = getRumbleJsonPath();
        if (path.empty())
            return;

        json_t root;
        root["version"] = 2;

        json_t sfx = json_t::object();
        // Iterate in enum-declaration order for stable output.
        for (const auto& nameEntry : kSoundIdNames)
        {
            const auto it = _map.find(nameEntry.id);
            if (it == _map.end())
                continue;
            const auto& p = it->second;

            json_t entry = json_t::object();
            entry["mode"] = (p.mode == RumbleMode::continuous) ? "continuous" : "oneShot";
            entry["envelopeDurationMs"] = p.envelopeDurationMs;
            // OPENRCT2MINI v2.19: persist per-profile flags. Always
            // emitted so the format is self-documenting; defaults
            // (false/false/100) are still readable from older builds
            // (Json::GetBoolean / Json::GetNumber tolerate the same
            // shapes).
            entry["disabled"] = p.disabled;
            entry["scaleWithRate"] = p.scaleWithRate;
            entry["rateScalePercent"] = static_cast<int32_t>(p.rateScalePercent);
            entry["low"] = envToJson(p.low);
            entry["high"] = envToJson(p.high);
            sfx[std::string(nameEntry.name)] = std::move(entry);
        }
        root["soundEffects"] = std::move(sfx);

        try
        {
            Json::WriteToFile(path, root);
        }
        catch (...)
        {
            Console::Error::WriteLine("Failed to write rumble.json to %s", path.c_str());
        }
    }
} // namespace OpenRCT2::Haptic
