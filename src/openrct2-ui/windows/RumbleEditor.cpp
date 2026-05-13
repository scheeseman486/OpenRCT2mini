/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI gamepad-plan 1.11b: Rumble Editor — v2.
//
// Reachable from the Cheats / Debug dropdown beneath the Haptics
// entry. Per-SoundId envelope authoring window with a scrolling
// waveform/envelope pane.
//
// Layout:
//
//   ┌─────────────┬─────────────────────────────────────────┐
//   │ list scroll │ mode | motor                            │
//   │             │                                         │
//   │             │  envelope-pane scroll (1 px = 1 ms)     │
//   │             │  ↳ greyscale PCM peaks (waveform)       │
//   │             │  ↳ cyan envelope curve + 3×3 points     │
//   │             │  ↳ yellow vertical playhead             │
//   │             │  ↳ horizontal scrollbar at the bottom   │
//   │             │                                         │
//   └─────────────┴─────────────────────────────────────────┘
//   transport:  Play / Stop / |◀ / ▶| / Delete
//
// v2 changes:
// - Pane is now a SCROLL_HORIZONTAL widget with content width =
//   envelopeDurationMs × 1 px so the time axis is *playback time*,
//   not window width. Long samples scroll horizontally; short
//   samples leave room on the right.
// - Real PCM waveform render via Audio::getPcmForSoundId. Cached
//   per-SoundId in a small LRU.
// - Continuous-mode playback loops via CreateAudioChannel(loop=true).
// - List flicker fixed by drawing in scroll-relative (rt.x / rt.y)
//   coordinates instead of (0, 0).
// - Switching SoundIds resets the playhead and re-evaluates the
//   envelope-pane content width so the scrollbar updates.
// - Drag-to-move points wired through onScrollMouseDrag.

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/UiStringIds.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/audio/AudioChannel.h>
#include <openrct2/audio/AudioSource.h>
#include <openrct2/config/Config.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/G1Element.h>
#include <openrct2/drawing/PaletteIndex.h>
#include <openrct2/drawing/PaletteMap.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/RenderTarget.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/haptic/HapticEvent.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/ui/WindowManager.h>

namespace OpenRCT2::Ui::Windows
{
    using namespace OpenRCT2::Drawing;
    using namespace OpenRCT2::Haptic;

    namespace
    {
        // ---- Layout constants -------------------------------------------

        constexpr int32_t kListWidth = 190;
        constexpr int32_t kHeaderTop = 18;
        // OPENRCT2MINI v2.19: two header rows now —
        //   row 1: Mode / Motor (mode label + value text, motor dropdown)
        //   row 2: per-profile flags (Disabled checkbox, Scale-with-
        //          speed checkbox, Reduction spinner).
        // Bump kHeaderHeight from 22 to 40 to fit both rows, and grow
        // kMinSize.height by 18 to preserve the envelope pane's
        // working area.
        constexpr int32_t kHeaderRowHeight = 18;
        constexpr int32_t kHeaderHeight = kHeaderRowHeight * 2 + 4;
        // OPENRCT2MINI v2.13: transport row now stacks two strips —
        //   * 24px button strip (Play/Pause, Delete, … Zoom Out/In)
        //   * 14px status strip (controller-detected message, right-
        //     justified, sits between the audio list and the right
        //     edge of the window).
        // Total = button(24) + status(14) + 2px breathing = 40.
        constexpr int32_t kButtonRowHeight = 24;
        constexpr int32_t kStatusRowHeight = 14;
        constexpr int32_t kTransportHeight = kButtonRowHeight + kStatusRowHeight + 2;
        constexpr int32_t kListItemHeight = 12;
        constexpr ScreenSize kMinSize = { 640, 378 };
        constexpr ScreenSize kMaxSize = { 1920, 1080 };

        // Time axis: pixels-per-millisecond is now a per-window state
        // controlled by the Zoom +/- buttons in the transport row.
        // The constants below clamp the zoom range; the default of
        // 1.0 px/ms matches the original v2.x behaviour.
        constexpr float kDefaultPixelsPerMs = 1.0f;
        constexpr float kMinPixelsPerMs = 0.05f;
        constexpr float kMaxPixelsPerMs = 8.0f;
        constexpr float kZoomStep = 1.5f; // multiplicative — × on zoom-in, ÷ on zoom-out

        // Timeline ruler row at the top of the envelope pane. Shows
        // ms tick marks + numeric labels that scale with zoom.
        constexpr int32_t kRulerHeight = 14;

        // Envelope point hit-test radius in pixels.
        constexpr int32_t kPointHitRadius = 5;

        constexpr uint32_t kAutoSaveDelayMs = 500;

        struct SoundNameEntry
        {
            Audio::SoundId id;
            const char* name;
        };

        constexpr SoundNameEntry kSoundNames[] = {
            { Audio::SoundId::liftClassic,              "Lift (Classic)" },
            { Audio::SoundId::trackFrictionClassicWood, "Track Friction (Classic Wood)" },
            { Audio::SoundId::frictionClassic,          "Friction (Classic)" },
            { Audio::SoundId::scream1,                  "Scream 1" },
            { Audio::SoundId::click1,                   "Click 1" },
            { Audio::SoundId::click2,                   "Click 2" },
            { Audio::SoundId::placeItem,                "Place Item" },
            { Audio::SoundId::scream2,                  "Scream 2" },
            { Audio::SoundId::scream3,                  "Scream 3" },
            { Audio::SoundId::scream4,                  "Scream 4" },
            { Audio::SoundId::scream5,                  "Scream 5" },
            { Audio::SoundId::scream6,                  "Scream 6" },
            { Audio::SoundId::liftFrictionWheels,       "Lift Friction Wheels" },
            { Audio::SoundId::purchase,                 "Purchase" },
            { Audio::SoundId::crash,                    "Crash" },
            { Audio::SoundId::layingOutWater,           "Laying Out Water" },
            { Audio::SoundId::water1,                   "Water 1" },
            { Audio::SoundId::water2,                   "Water 2" },
            { Audio::SoundId::trainWhistle,             "Train Whistle" },
            { Audio::SoundId::trainDeparting,           "Train Departing" },
            { Audio::SoundId::waterSplash,              "Water Splash" },
            { Audio::SoundId::goKartEngine,             "Go-Kart Engine" },
            { Audio::SoundId::rideLaunch1,              "Ride Launch 1" },
            { Audio::SoundId::rideLaunch2,              "Ride Launch 2" },
            { Audio::SoundId::cough1,                   "Cough 1" },
            { Audio::SoundId::cough2,                   "Cough 2" },
            { Audio::SoundId::cough3,                   "Cough 3" },
            { Audio::SoundId::cough4,                   "Cough 4" },
            { Audio::SoundId::rain,                     "Rain" },
            { Audio::SoundId::thunder1,                 "Thunder 1" },
            { Audio::SoundId::thunder2,                 "Thunder 2" },
            { Audio::SoundId::trackFrictionTrain,       "Track Friction (Train)" },
            { Audio::SoundId::trackFrictionWater,       "Track Friction (Water)" },
            { Audio::SoundId::balloonPop,               "Balloon Pop" },
            { Audio::SoundId::mechanicFix,              "Mechanic Fix" },
            { Audio::SoundId::scream7,                  "Scream 7" },
            { Audio::SoundId::toiletFlush,              "Toilet Flush" },
            { Audio::SoundId::click3,                   "Click 3" },
            { Audio::SoundId::quack,                    "Quack" },
            { Audio::SoundId::newsItem,                 "News Item" },
            { Audio::SoundId::windowOpen,               "Window Open" },
            { Audio::SoundId::laugh1,                   "Laugh 1" },
            { Audio::SoundId::laugh2,                   "Laugh 2" },
            { Audio::SoundId::laugh3,                   "Laugh 3" },
            { Audio::SoundId::applause,                 "Applause" },
            { Audio::SoundId::hauntedHouseScare,        "Haunted House Scare" },
            { Audio::SoundId::hauntedHouseScream1,      "Haunted House Scream 1" },
            { Audio::SoundId::hauntedHouseScream2,      "Haunted House Scream 2" },
            { Audio::SoundId::blockBrakeClose,          "Block Brake — Close" },
            { Audio::SoundId::blockBrakeRelease,        "Block Brake — Release" },
            { Audio::SoundId::error,                    "Error" },
            { Audio::SoundId::brakeRelease,             "Brake Release" },
            { Audio::SoundId::liftArrow,                "Lift (Arrow)" },
            { Audio::SoundId::liftWood,                 "Lift (Wood)" },
            { Audio::SoundId::trackFrictionWood,        "Track Friction (Wood)" },
            { Audio::SoundId::liftWildMouse,            "Lift (Wild Mouse)" },
            { Audio::SoundId::liftBM,                   "Lift (B&M)" },
            { Audio::SoundId::trackFrictionBM,          "Track Friction (B&M)" },
            { Audio::SoundId::scream8,                  "Scream 8" },
            { Audio::SoundId::tram,                     "Tram" },
            { Audio::SoundId::doorOpen,                 "Door Open" },
            { Audio::SoundId::doorClose,                "Door Close" },
            { Audio::SoundId::portcullis,               "Portcullis" },
            { Audio::SoundId::crowdAmbience,            "Crowd Ambience" },
            { Audio::SoundId::liftRMC,                  "Lift (RMC)" },
        };
        constexpr size_t kSoundNamesCount = std::size(kSoundNames);

        // ---- Waveform peak cache ----------------------------------------

        struct WaveformPeaks
        {
            uint32_t durationMs = 0;     // total audio duration
            std::vector<uint8_t> peaks;  // one entry per ms; 0..255
        };

        // Compute peaks for a SoundId. Returns true if PCM was loaded.
        // Reads the entire sample, treats bytes as int16 LE, and
        // finds the absolute peak amplitude per millisecond.
        //
        // Duration is derived from total bytes / bytesPerSec so it
        // exactly matches the audio's actual playback length —
        // critical for keeping the waveform's right edge aligned
        // with the audio's natural end and for continuous-mode
        // looping to wrap on the same beat as the audio channel.
        bool buildWaveformPeaks(Audio::SoundId id, WaveformPeaks& out)
        {
            std::vector<uint8_t> pcm;
            int32_t bytesPerSec = 0;
            if (!Audio::getPcmForSoundId(id, pcm, bytesPerSec))
                return false;
            if (pcm.size() < 2 || bytesPerSec <= 0)
                return false;

            const auto* samples = reinterpret_cast<const int16_t*>(pcm.data());
            const size_t nSamples = pcm.size() / 2;

            // durationMs = totalBytes × 1000 / bytesPerSec. Direct
            // computation avoids the rounding loss the previous
            // (samplesPerMs-based) path took for non-22050 / non-
            // 44100 sample rates.
            const uint64_t totalBytes = pcm.size();
            const uint32_t durationMs = static_cast<uint32_t>(
                (totalBytes * 1000ull) / static_cast<uint64_t>(bytesPerSec));
            if (durationMs == 0)
                return false;

            out.durationMs = durationMs;
            out.peaks.assign(durationMs, 0);

            // Walk samples and place each into its corresponding
            // millisecond bucket. This is robust against odd sample
            // rates and matches the duration math above (no off-by-
            // one drift across the sample's length).
            for (size_t i = 0; i < nSamples; i++)
            {
                // Sample i corresponds to time:
                //   ms = (i × bytesPerSample) × 1000 / bytesPerSec
                //      = (i × 2 × 1000) / bytesPerSec
                // (assumes 16-bit samples, treats stereo as twice
                // the sample stream — peaks across both channels.)
                const uint64_t ms = (static_cast<uint64_t>(i) * 2ull * 1000ull)
                    / static_cast<uint64_t>(bytesPerSec);
                if (ms >= durationMs)
                    break;
                const int32_t a = std::abs(static_cast<int32_t>(samples[i]));
                const auto scaled = static_cast<uint8_t>(std::min<int32_t>(a >> 7, 255));
                if (scaled > out.peaks[ms])
                    out.peaks[ms] = scaled;
            }
            return true;
        }
        // OPENRCT2MINI v2.14: classify SoundIds as continuous vs
        // one-shot. The Haptic engine still uses RumbleMode as a
        // routing filter (onSoundPlayed only handles oneShot,
        // updateContinuousSound only handles continuous), but the
        // mode for any given SoundId isn't authoring choice — it's
        // determined by how the game actually feeds the sound to the
        // audio system. Track friction loops, lift hill loops, the
        // go-kart engine, the crowd ambience, and the looped scream
        // channel are driven via Vehicle::UpdateSound's per-tick poll
        // → updateContinuousSound; everything else fires once via
        // Audio::Play → onSoundPlayed. This list mirrors what
        // HapticEvent.cpp's seedDefaults already curates as
        // continuous, with the addition of trackFrictionRMC (which
        // logically belongs with the other trackFriction* loops).
        bool isContinuousSoundId(Audio::SoundId id)
        {
            switch (id)
            {
                case Audio::SoundId::trackFrictionTrain:
                case Audio::SoundId::trackFrictionWater:
                case Audio::SoundId::trackFrictionWood:
                case Audio::SoundId::trackFrictionBM:
                case Audio::SoundId::trackFrictionClassicWood:
                case Audio::SoundId::trackFrictionRMC:
                case Audio::SoundId::frictionClassic:
                case Audio::SoundId::liftClassic:
                case Audio::SoundId::liftWildMouse:
                case Audio::SoundId::liftBM:
                case Audio::SoundId::liftRMC:
                case Audio::SoundId::liftWood:
                case Audio::SoundId::liftArrow:
                case Audio::SoundId::liftFlume:
                case Audio::SoundId::liftFrictionWheels:
                case Audio::SoundId::goKartEngine:
                case Audio::SoundId::crowdAmbience:
                case Audio::SoundId::scream1:
                case Audio::SoundId::scream2:
                case Audio::SoundId::scream3:
                case Audio::SoundId::scream4:
                case Audio::SoundId::scream5:
                case Audio::SoundId::scream6:
                case Audio::SoundId::scream7:
                    return true;
                default:
                    return false;
            }
        }

        Haptic::RumbleMode inferModeForSound(Audio::SoundId id)
        {
            return isContinuousSoundId(id) ? Haptic::RumbleMode::continuous : Haptic::RumbleMode::oneShot;
        }
    } // namespace

    enum WidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_PAGE_BACKGROUND,
        WIDX_LIST,
        WIDX_MODE_LABEL,
        WIDX_MOTOR_LABEL,
        WIDX_MOTOR_DROPDOWN,
        WIDX_MOTOR_DROPDOWN_BUTTON,
        // OPENRCT2MINI v2.19: per-profile flags row.
        WIDX_DISABLED_CHECKBOX,
        WIDX_SCALE_CHECKBOX,
        WIDX_REDUCTION_LABEL,
        WIDX_REDUCTION,
        WIDX_REDUCTION_INCREASE,
        WIDX_REDUCTION_DECREASE,
        WIDX_ENVELOPE_PANE,
        WIDX_PLAY_PAUSE,
        WIDX_DELETE,
        // OPENRCT2MINI v2.16: tail spinner triplet in transport row.
        // Order matches makeSpinnerWidgets return order — spinner
        // field first (clickable for OSK numpad entry), then increase
        // button, then decrease button.
        WIDX_TAIL_LABEL,
        WIDX_TAIL,
        WIDX_TAIL_INCREASE,
        WIDX_TAIL_DECREASE,
        WIDX_ZOOM_OUT,
        WIDX_ZOOM_IN,
    };

    // clang-format off
    static const auto kRumbleEditorWidgets = makeWidgets(
        makeWindowShim(STR_RUMBLE_EDITOR_TITLE,    kMinSize),
        makeWidget    ({  0, kHeaderTop }, { kMinSize.width,                 kMinSize.height - kHeaderTop }, WidgetType::resize,       WindowColour::secondary),
        makeWidget    ({  3, kHeaderTop + 4 }, { kListWidth - 6,             kMinSize.height - kHeaderTop - 8 }, WidgetType::scroll,    WindowColour::secondary, SCROLL_VERTICAL),
        // OPENRCT2MINI v2.14 / v2.16: "Mode:" is a static label;
        // the value text ("One-shot" / "Continuous") is drawn in
        // onDraw — inferred per SoundId, not editable. v2.16 right-
        // justifies the motor section against the window's right
        // edge in relayoutWidgets; the X here is a placeholder.
        makeWidget    ({ kListWidth +   4,kHeaderTop + 5 }, {  35, 12 }, WidgetType::label,        WindowColour::secondary, STR_RUMBLE_EDITOR_MODE),
        makeWidget    ({ kListWidth + 100,kHeaderTop + 5 }, {  45, 12 }, WidgetType::label,        WindowColour::secondary, STR_RUMBLE_EDITOR_MOTOR),
        makeWidget    ({ kListWidth + 138,kHeaderTop + 3 }, { 160, 14 }, WidgetType::dropdownMenu, WindowColour::secondary, kStringIdEmpty),
        makeWidget    ({ kListWidth + 285,kHeaderTop + 4 }, {  11, 12 }, WidgetType::button,       WindowColour::secondary, STR_DROPDOWN_GLYPH),
        // OPENRCT2MINI v2.19: per-profile flags (second header row).
        // Disabled checkbox kills the profile entirely (engine
        // ignores it). Scale-with-speed (continuous only) reduces
        // rumble intensity proportionally as audio rate drops below
        // 1.0; the spinner sets the strength of that reduction in
        // 10% steps. Positions are placeholders — relayoutWidgets
        // pins them on each layout pass.
        makeWidget       ({ kListWidth +   4, kHeaderTop + 21 }, {  90, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_RUMBLE_EDITOR_DISABLED,         STR_RUMBLE_EDITOR_DISABLED_TIP),
        makeWidget       ({ kListWidth + 100, kHeaderTop + 21 }, { 140, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_RUMBLE_EDITOR_SCALE_WITH_SPEED, STR_RUMBLE_EDITOR_SCALE_WITH_SPEED_TIP),
        makeWidget       ({ kListWidth + 250, kHeaderTop + 21 }, {  60, 12 }, WidgetType::label,    WindowColour::secondary, STR_RUMBLE_EDITOR_REDUCTION),
        makeSpinnerWidgets({ kListWidth +312, kHeaderTop + 19 }, {  60, 14 }, WidgetType::spinner,  WindowColour::secondary, kStringIdEmpty),
        // SCROLL_HORIZONTAL — content width set per-frame to envelopeDurationMs.
        makeWidget    ({ kListWidth + 4, kHeaderTop + kHeaderHeight }, { kMinSize.width - kListWidth - 8, kMinSize.height - kHeaderTop - kHeaderHeight - kTransportHeight - 4 }, WidgetType::scroll, WindowColour::secondary, SCROLL_HORIZONTAL),
        // OPENRCT2MINI v2.12 / v2.13: 24×24 graphical buttons. Content
        // is kWidgetContentEmpty so the framework draws only the inset
        // border / press state; the icon glyph is composited in
        // onDraw. Tooltips are real labels ("Play", etc.) since the
        // in-button text label is gone.
        //
        // Initial positions are placeholders — relayoutWidgets()
        // re-positions Play/Delete at the left edge of the transport
        // row and Zoom Out/In at the right edge, both relative to the
        // current window width (so resizing keeps Zoom anchored to
        // the right). The Y is set in relayoutWidgets too.
        makeWidget    ({ kListWidth +  4, kMinSize.height - kTransportHeight }, {  24, 24 }, WidgetType::flatBtn, WindowColour::secondary, kWidgetContentEmpty, STR_RUMBLE_EDITOR_PLAY),
        makeWidget    ({ kListWidth + 32, kMinSize.height - kTransportHeight }, {  24, 24 }, WidgetType::flatBtn, WindowColour::secondary, kWidgetContentEmpty, STR_RUMBLE_EDITOR_DELETE_POINT),
        // OPENRCT2MINI v2.16: "Tail:" label + spinner triplet (field,
        // increase, decrease) following the same pattern as Park's
        // admission-price spinner. The spinner field is clickable to
        // open a numpad OSK for direct entry; +/- step by 100ms and
        // are marked holdable in onOpen for press-and-hold scrubbing.
        // Positions are placeholders — relayoutWidgets pins these
        // immediately to the right of the Delete button.
        makeWidget       ({ kListWidth +  60, kMinSize.height - kTransportHeight +  6 }, { 28, 12 }, WidgetType::label, WindowColour::secondary, STR_RUMBLE_EDITOR_TAIL),
        makeSpinnerWidgets({ kListWidth +  92, kMinSize.height - kTransportHeight +  4 }, { 76, 14 }, WidgetType::spinner, WindowColour::secondary, kStringIdEmpty),
        makeWidget    ({ kListWidth +176, kMinSize.height - kTransportHeight }, {  24, 24 }, WidgetType::flatBtn, WindowColour::secondary, kWidgetContentEmpty, STR_RUMBLE_EDITOR_ZOOM_OUT),
        makeWidget    ({ kListWidth +204, kMinSize.height - kTransportHeight }, {  24, 24 }, WidgetType::flatBtn, WindowColour::secondary, kWidgetContentEmpty, STR_RUMBLE_EDITOR_ZOOM_IN)
    );
    // clang-format on

    class RumbleEditorWindow final : public Window
    {
    private:
        // Selection state.
        Audio::SoundId _selectedSoundId = Audio::SoundId::null;
        int32_t _selectedListIndex = -1;
        bool _editingHigh = false;
        int32_t _selectedPointIndex = -1;

        // Drag state.
        bool _dragActive = false;
        int32_t _dragPointIndex = -1;

        // Playback state.
        bool _playing = false;
        uint32_t _playStartMs = 0;
        uint32_t _playheadMs = 0;
        // Single channel handle covers both one-shot and continuous
        // playback. We keep it specifically so onUpdate can poll
        // IAudioChannel::GetOffset() to lock the visual playhead to
        // the actual audio playback position — wall-clock-based
        // timing drifts vs SDL's audio buffer (~50–100 ms latency)
        // and can lead the audio noticeably.
        std::shared_ptr<Audio::IAudioChannel> _channel;

        // Auto-save debounce.
        bool _saveDirty = false;
        uint32_t _saveDirtyMs = 0;

        // Cached waveform for currently-selected SoundId (lazy build).
        WaveformPeaks _waveform;
        Audio::SoundId _waveformFor = Audio::SoundId::null;

        // OPENRCT2MINI v2.11: horizontal zoom level (px-per-ms).
        // Driven by the Zoom +/- transport buttons. Affects everything
        // that maps a time-axis ms value to an x-pixel: scroll content
        // width, waveform peaks, envelope curve, point markers, hit
        // tests, the playhead, and the timeline ruler labels.
        float _pixelsPerMs = kDefaultPixelsPerMs;

        // OPENRCT2MINI v2.12: lazily-built per-window 24×24 paletted
        // glyph buffers for the Zoom Out / Zoom In transport buttons.
        // The source toolbar sprites (SPR_TOOLBAR_ZOOM_{IN,OUT}) are
        // 30×28 and intentionally painted against an opaque toolbar
        // colour (not the transparent palette index), so we can't just
        // blit them inside a flatBtn — they'd show their toolbar tile
        // background. Build path:
        //   1. Decode the source g1 element into a flat 30×28 paletted
        //      buffer via GfxSpriteToBuffer.
        //   2. Crop a 24×24 sub-rect from (4, 1).
        //   3. Sample pixel (1, 1) — that's the toolbar background
        //      colour (top-left corner of the icon glyph).
        //   4. Replace every pixel matching that colour with palette
        //      index 0 (PaletteIndex::transparent) so it punches
        //      through when blitted onto the button.
        // Stored as palette indices; blit with a per-pixel skip-on-0
        // loop in drawCenteredIcon's zoom path.
        std::array<uint8_t, 24 * 24> _zoomOutIcon{};
        std::array<uint8_t, 24 * 24> _zoomInIcon{};
        bool _zoomIconsBuilt = false;

    public:
        void onOpen() override
        {
            setWidgets(kRumbleEditorWidgets);
            (void)Haptic::getMap();

            _selectedListIndex = 0;
            _selectedSoundId = kSoundNames[0].id;
            ensureWaveform();

            // OPENRCT2MINI v2.16: tail spinner +/- buttons fire on
            // press-and-hold via the framework's holdable-widget
            // path. Same convention as Park admission price spinner.
            // v2.19 adds the reduction spinner to the same set.
            widgetsSetHoldable(*this, {
                WIDX_TAIL_INCREASE, WIDX_TAIL_DECREASE,
                WIDX_REDUCTION_INCREASE, WIDX_REDUCTION_DECREASE,
            });

            // OPENRCT2MINI v2.17: silence the framework's click1 /
            // click2 sound effects on the Play/Pause button. Without
            // this the click sample plays at the exact moment the
            // preview audio kicks off, masking short one-shot
            // samples and the haptic rumble that fires with them.
            widgets[WIDX_PLAY_PAUSE].flags.set(WidgetFlag::suppressClickSound);

            WindowSetResize(*this, kMinSize, kMaxSize);
            // Initial layout pass before the scroll widgets are
            // measured for content size — getting bounds right at
            // open prevents the very-first scroll viewport from
            // being computed against the un-relayed widget bounds.
            relayoutWidgets();
            WindowInitScrollWidgets(*this);
        }

        void onClose() override
        {
            stopAudio();
            if (_saveDirty)
            {
                Haptic::saveProfilesToDisk();
                _saveDirty = false;
            }
        }

        void onResize() override
        {
            WindowSetResize(*this, kMinSize, kMaxSize);
            relayoutWidgets();
            // Scroll widgets need re-init when their bounds change.
            WindowInitScrollWidgets(*this);
        }

        void onUpdate() override
        {
            if (_saveDirty && Platform::GetTicks() - _saveDirtyMs >= kAutoSaveDelayMs)
            {
                Haptic::saveProfilesToDisk();
                _saveDirty = false;
            }

            if (_playing)
            {
                // OPENRCT2MINI v2.7: drive the playhead off the audio
                // channel's actual offset, with the AUDIO length
                // (waveDur) — not envDur — as the natural reference
                // for both wrap (continuous) and stop (one-shot).
                //
                // Pre-v2.7 the code modulated by envDur and stopped at
                // envDur. That worked when envDur exactly matched the
                // sample length, but every default profile (crash
                // 500ms vs ~1500ms sample, click ~60ms vs longer
                // sample, etc.) intentionally uses envDur < waveDur
                // to focus rumble on a portion of the audio. The
                // result was the playhead either freezing at the
                // envelope's right edge while audio kept playing
                // (one-shot) or wrapping at envDur while the audio
                // wrapped at waveDur, putting them in completely
                // different cycles (continuous).
                //
                // The channel itself wraps `_offset` at the source
                // length for looping channels and stops once `_done`
                // for one-shot — so reading `GetOffset()` directly
                // and converting to ms gives a value that already
                // tracks audio reality.
                const auto envDur = currentEnvelopeDurationMs();
                // Reference for stop/wrap: envDur (the rumble's end).
                // Audio plays for waveDur; rumble plays for envDur ≥
                // waveDur (the difference is the user's tail
                // extension). The playhead visualises the rumble, so
                // it spans the full envDur — past waveDur the audio is
                // silent but the haptic motor keeps firing the tail.
                const uint32_t reference = envDur > 0 ? envDur : 1000u;
                {
                    auto* p = currentProfile();
                    const bool continuous = (p != nullptr && p->mode == RumbleMode::continuous);

                    uint32_t t = 0;
                    bool gotChannelTime = false;
                    bool channelAlive = false;

                    if (_channel != nullptr)
                    {
                        channelAlive = _channel->IsPlaying();
                        auto* source = _channel->GetSource();
                        if (source != nullptr)
                        {
                            const auto bps = source->GetBytesPerSecond();
                            if (bps > 0)
                            {
                                const uint64_t ofs = _channel->GetOffset();
                                t = static_cast<uint32_t>((ofs * 1000ull) / static_cast<uint64_t>(bps));
                                gotChannelTime = true;
                            }
                        }
                    }

                    // OPENRCT2MINI v2.15: while the audio channel is
                    // alive, drive the playhead from its offset (precise
                    // audio sync). As we do, re-anchor _playStartMs so
                    // wall-clock would produce the same value — that
                    // way, when the channel dies at waveDur (one-shot)
                    // or we lose it for any reason, the wall-clock
                    // fallback continues seamlessly from that exact
                    // moment, letting the playhead march through the
                    // tail extension past audio end without a jump.
                    if (gotChannelTime && channelAlive)
                    {
                        _playStartMs = Platform::GetTicks() - t;
                    }
                    else
                    {
                        // No live channel: wall-clock from the last
                        // known anchor (= audio end, or play start).
                        t = Platform::GetTicks() - _playStartMs;
                    }

                    if (continuous)
                    {
                        // Wrap at envDur (the rumble loop period; same
                        // value the Haptic engine wraps at).
                        if (reference > 0 && t >= reference)
                            t %= reference;
                    }
                    else
                    {
                        // One-shot ends when t passes the full envDur
                        // (audio end + tail extension). Stop and pin.
                        if (t >= reference)
                        {
                            _playing = false;
                            t = reference;
                            if (_channel != nullptr)
                                _channel.reset();
                        }
                    }
                    _playheadMs = t;

                    // OPENRCT2MINI v2.8: drive Haptic::rumble directly
                    // from the editor's playhead. The editor uses
                    // Audio::CreateAudioChannel for direct playback
                    // control, which bypasses Audio::Play and
                    // therefore the Haptic::onSoundPlayed hook — so
                    // without this, the rumble engine never sees the
                    // editor previewing a sound and the controller
                    // doesn't pulse. Doing it here also keeps the
                    // rumble exactly aligned with the visible playhead
                    // (the engine's separate Platform::GetTicks-based
                    // timer would drift relative to channel offset).
                    //
                    // Eval at t for one-shot, t % envDur for
                    // continuous (the engine wraps the same way), with
                    // envDur taken from the saved profile so the
                    // intentionally-short loops (60ms click-engine
                    // patterns, etc.) keep their loop period even
                    // when the audio is longer.
                    if (p != nullptr && _playing)
                    {
                        const auto envDurReal = p->envelopeDurationMs;
                        uint32_t tEval = _playheadMs;
                        bool active = true;
                        if (continuous)
                        {
                            if (envDurReal > 0)
                                tEval = _playheadMs % envDurReal;
                        }
                        else
                        {
                            if (envDurReal > 0 && tEval > envDurReal)
                                active = false;
                        }
                        const float low = active ? Haptic::evaluate(p->low, tEval) : 0.0f;
                        const float high = active ? Haptic::evaluate(p->high, tEval) : 0.0f;
                        // 200ms TTL: same as Haptic engine's
                        // kPerFrameTTL. Comfortably exceeds one frame
                        // so successive ticks don't gap.
                        Haptic::rumble(low, high, 200u);
                    }
                    else if (p != nullptr && !_playing)
                    {
                        // One-shot just finished — explicit cancel.
                        Haptic::rumble(0.0f, 0.0f, 0u);
                    }
                }
                invalidate();
            }
        }

        ScreenSize onScrollGetSize(int32_t scrollIndex) override
        {
            if (scrollIndex == 0)
            {
                return { 0, static_cast<int32_t>(kSoundNamesCount) * kListItemHeight };
            }
            // Scroll index 1 = envelope pane. Width = max(envelope,
            // audio) so both the user's authored envelope curve and
            // the full audio waveform are reachable. Many default
            // profiles deliberately use a shorter envDur than the
            // sample length (e.g. crash = 500 ms envelope on a ~1500
            // ms sample so the rumble emphasis lands on the impact
            // attack); without this max the waveform got clipped at
            // the envelope's right edge.
            const auto envDur = currentEnvelopeDurationMs();
            const auto waveDur = (_waveformFor == _selectedSoundId) ? _waveform.durationMs : 0u;
            const uint32_t timeline = std::max(envDur, waveDur);
            return { std::max(1, static_cast<int32_t>(timeline * _pixelsPerMs)), 0 };
        }

        void onScrollMouseDown(int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            if (scrollIndex == 0)
            {
                handleListClick(screenCoords);
                return;
            }
            // scrollIndex 1: envelope pane.
            handlePaneClick(screenCoords, /*startDrag=*/true);
        }

        void onScrollMouseDrag(int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            if (scrollIndex != 1)
                return;
            handlePaneDrag(screenCoords);
        }

        void onMouseDown(WidgetIndex widgetIndex) override
        {
            if (widgetIndex == WIDX_MOTOR_DROPDOWN || widgetIndex == WIDX_MOTOR_DROPDOWN_BUTTON)
            {
                auto& widget = widgets[WIDX_MOTOR_DROPDOWN];
                gDropdown.items[0] = Dropdown::MenuLabel(STR_RUMBLE_EDITOR_MOTOR_LOW);
                gDropdown.items[1] = Dropdown::MenuLabel(STR_RUMBLE_EDITOR_MOTOR_HIGH);
                WindowDropdownShowText(
                    { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                    colours[1], 0, 2);
                gDropdown.items[_editingHigh ? 1 : 0].setChecked(true);
                return;
            }
            // OPENRCT2MINI v2.16: spinner buttons fire on onMouseDown
            // (matches OpenRCT2 convention for spinners + lets them be
            // marked holdable so press-and-hold auto-repeats). The
            // spinner FIELD also fires on mouseDown to pop the OSK,
            // matching the Park admission-price widget.
            switch (widgetIndex)
            {
                case WIDX_TAIL_INCREASE:
                    adjustTail(+100);
                    return;
                case WIDX_TAIL_DECREASE:
                    adjustTail(-100);
                    return;
                case WIDX_TAIL:
                {
                    // Pop a numpad OSK pre-populated with the current
                    // tail value in ms. onTextInput receives the
                    // entered string and applies it via setTailMs.
                    const auto tailNow = currentTailMs();
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(tailNow));
                    WindowTextInputRawOpen(
                        this, WIDX_TAIL, STR_ENTER_NEW_VALUE, STR_ENTER_NEW_VALUE, {}, buf, 8,
                        OskMode::numpad);
                    return;
                }
                case WIDX_REDUCTION_INCREASE:
                    adjustReductionPercent(+10);
                    return;
                case WIDX_REDUCTION_DECREASE:
                    adjustReductionPercent(-10);
                    return;
                case WIDX_REDUCTION:
                {
                    auto* p = currentProfile();
                    const uint8_t cur = (p != nullptr) ? p->rateScalePercent : 100;
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(cur));
                    WindowTextInputRawOpen(
                        this, WIDX_REDUCTION, STR_ENTER_NEW_VALUE, STR_ENTER_NEW_VALUE, {}, buf, 4,
                        OskMode::numpad);
                    return;
                }
            }
        }

        void onTextInput(WidgetIndex widgetIndex, std::string_view text) override
        {
            if (text.empty())
                return;
            int32_t parsed = std::atoi(std::string(text).c_str());
            if (widgetIndex == WIDX_TAIL)
            {
                // Accept any leading-digit string; trailing "ms" or
                // other suffix is ignored. Clamp to a sensible range.
                parsed = std::clamp(parsed, 0, 10000);
                setTailMs(static_cast<uint32_t>(parsed));
            }
            else if (widgetIndex == WIDX_REDUCTION)
            {
                parsed = std::clamp(parsed, 0, 100);
                setReductionPercent(static_cast<uint8_t>(parsed));
            }
        }

        void onDropdown(WidgetIndex widgetIndex, int32_t selectedIndex) override
        {
            if (widgetIndex == WIDX_MOTOR_DROPDOWN || widgetIndex == WIDX_MOTOR_DROPDOWN_BUTTON)
            {
                if (selectedIndex == 0 || selectedIndex == 1)
                {
                    _editingHigh = (selectedIndex == 1);
                    _selectedPointIndex = -1;
                    invalidate();
                }
            }
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            // Drag ends on any mouse-up.
            _dragActive = false;
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    return;
                case WIDX_PLAY_PAUSE:
                    // OPENRCT2MINI v2.12: single button toggles
                    // between Play (when stopped) and Pause (when
                    // playing). Click while playing == stop preview.
                    if (_playing)
                        stopPlayback();
                    else
                        startPlayback();
                    return;
                case WIDX_DISABLED_CHECKBOX:
                    if (auto* p = currentProfile())
                    {
                        p->disabled = !p->disabled;
                        if (p->disabled && _playing)
                            stopPlayback();
                        markDirty();
                        invalidate();
                    }
                    return;
                case WIDX_SCALE_CHECKBOX:
                    if (auto* p = currentProfile())
                    {
                        p->scaleWithRate = !p->scaleWithRate;
                        markDirty();
                        invalidate();
                    }
                    return;
                case WIDX_DELETE:
                    deleteSelectedPoint();
                    return;
                case WIDX_ZOOM_IN:
                    setZoom(_pixelsPerMs * kZoomStep);
                    return;
                case WIDX_ZOOM_OUT:
                    setZoom(_pixelsPerMs / kZoomStep);
                    return;
            }
        }

        // OPENRCT2MINI v2.15 / v2.16: tail helpers. The "tail" is the
        // user-controlled extra rumble time PAST the audio's natural
        // end — needed because some one-shot effects are too short
        // for the haptic motor to fire perceptibly. Stored as part
        // of envelopeDurationMs (envDur = waveDur + tail).
        uint32_t currentTailMs() const
        {
            const auto envDur = currentEnvelopeDurationMs();
            const auto waveDur = (_waveformFor == _selectedSoundId && _waveform.durationMs > 0)
                ? _waveform.durationMs
                : 0u;
            return (envDur > waveDur) ? (envDur - waveDur) : 0u;
        }

        void setTailMs(uint32_t newTailMs)
        {
            auto* p = currentProfile();
            if (p == nullptr)
                return;
            const auto waveDur = (_waveformFor == _selectedSoundId && _waveform.durationMs > 0)
                ? _waveform.durationMs
                : 0u;
            uint32_t newEnvDur = waveDur + newTailMs;
            if (newEnvDur < 100)
                newEnvDur = 100;
            if (newEnvDur == p->envelopeDurationMs)
                return;

            // OPENRCT2MINI v2.17: move each motor's LAST envelope
            // point to the new envDur (preserving intensity). The
            // rumble engine evaluates the bezier between authored
            // points and CLAMPS to the last point's intensity for
            // t > last.timeMs. Without this move, default profiles
            // that end at intensity 0 would produce a flat-zero
            // tail — no rumble during the user's extension, which
            // defeats the whole point of the Tail control. Pulling
            // the last point forward stretches the final segment so
            // the curve actually rolls through the tail region; for
            // "ends at 0" profiles the decay just becomes slower
            // and you feel rumble all the way to envDur. The user
            // can still drag the last point afterwards to shape it.
            auto extendLast = [newEnvDur](MotorEnvelope& env) {
                if (!env.points.empty())
                    env.points.back().timeMs = newEnvDur;
            };
            extendLast(p->low);
            extendLast(p->high);

            p->envelopeDurationMs = newEnvDur;
            markDirty();
            WindowInitScrollWidgets(*this); // content width changes
            invalidate();
        }

        // ±100ms convenience for the spinner buttons. Floor at 0
        // (tail can't go negative — at 0 the rumble plays for exactly
        // the audio length).
        void adjustTail(int32_t deltaMs)
        {
            const int32_t cur = static_cast<int32_t>(currentTailMs());
            const int32_t next = std::max(0, cur + deltaMs);
            setTailMs(static_cast<uint32_t>(next));
        }

        // OPENRCT2MINI v2.19: reduction-strength helpers. The
        // rate-scale strength is stored as a percentage 0..100 on
        // the profile; spinner steps are ±10. Range clamp lives
        // here so onMouseDown / onTextInput / adjust* converge.
        void setReductionPercent(uint8_t newPct)
        {
            auto* p = currentProfile();
            if (p == nullptr)
                return;
            const uint8_t clamped = static_cast<uint8_t>(std::clamp<int32_t>(newPct, 0, 100));
            if (p->rateScalePercent == clamped)
                return;
            p->rateScalePercent = clamped;
            markDirty();
            invalidate();
        }

        void adjustReductionPercent(int32_t delta)
        {
            auto* p = currentProfile();
            if (p == nullptr)
                return;
            const int32_t cur = static_cast<int32_t>(p->rateScalePercent);
            const int32_t next = std::clamp(cur + delta, 0, 100);
            setReductionPercent(static_cast<uint8_t>(next));
        }

        // OPENRCT2MINI v2.11: zoom helper. Clamps to [min, max], then
        // re-init the scroll widget so the framework recomputes
        // viewport extents and the scrollbar geometry against the new
        // content width (scrolled content width = timelineMs * px/ms).
        void setZoom(float newPixelsPerMs)
        {
            _pixelsPerMs = std::clamp(newPixelsPerMs, kMinPixelsPerMs, kMaxPixelsPerMs);
            WindowInitScrollWidgets(*this);
            invalidate();
        }

        // OPENRCT2MINI v2.12: build the runtime-cropped + key-coloured
        // zoom button glyphs (see _zoomInIcon comment). Called lazily
        // on first draw — depends on the g1 sprite cache being loaded,
        // which it always is by the time a window draws.
        void buildZoomIcons()
        {
            if (_zoomIconsBuilt)
                return;
            buildZoomIconOne(SPR_TOOLBAR_ZOOM_OUT, _zoomOutIcon);
            buildZoomIconOne(SPR_TOOLBAR_ZOOM_IN, _zoomInIcon);
            _zoomIconsBuilt = true;
        }

        static void buildZoomIconOne(uint32_t spriteId, std::array<uint8_t, 24 * 24>& dst)
        {
            dst.fill(0); // start fully transparent
            const auto* g1 = GfxGetG1Element(ImageId(spriteId));
            if (g1 == nullptr || g1->width <= 0 || g1->height <= 0)
                return;
            // Decode the full source sprite into a flat paletted
            // buffer using the same path SpriteImageExport uses.
            const int32_t srcW = g1->width;
            const int32_t srcH = g1->height;
            std::vector<uint8_t> srcBuf(static_cast<size_t>(srcW) * srcH, 0);

            Drawing::RenderTarget tempRt{};
            tempRt.bits = reinterpret_cast<Drawing::PaletteIndex*>(srcBuf.data());
            tempRt.x = 0;
            tempRt.y = 0;
            tempRt.width = srcW;
            tempRt.height = srcH;
            tempRt.pitch = 0;
            tempRt.zoom_level = ZoomLevel{ 0 };
            DrawSpriteArgs args(
                ImageId(), Drawing::PaletteMap::GetDefault(), *g1, 0, 0, srcW, srcH,
                tempRt.bits);
            GfxSpriteToBuffer(tempRt, args);

            // Crop 24×24 from (4, 1) per the spec. Clip defensively
            // if the source ends up smaller than expected — the
            // toolbar zoom sprites are 30×28 today but we don't want
            // a future asset swap to crash us.
            constexpr int32_t kCropX = 4;
            constexpr int32_t kCropY = 1;
            constexpr int32_t kIconSize = 24;
            const int32_t copyW = std::min(kIconSize, srcW - kCropX);
            const int32_t copyH = std::min(kIconSize, srcH - kCropY);
            for (int32_t y = 0; y < copyH && y < kIconSize; y++)
            {
                for (int32_t x = 0; x < copyW && x < kIconSize; x++)
                {
                    const auto srcIdx = static_cast<size_t>((kCropY + y) * srcW + (kCropX + x));
                    dst[y * kIconSize + x] = srcBuf[srcIdx];
                }
            }

            // Sample pixel (1, 1) of the cropped buffer — that's the
            // toolbar tile background colour we want punched through.
            // Replace every matching pixel with palette index 0
            // (transparent) so the button's own background shows
            // through the punch-out.
            if (kIconSize >= 2)
            {
                const uint8_t keyColour = dst[1 * kIconSize + 1];
                if (keyColour != 0)
                {
                    for (auto& px : dst)
                        if (px == keyColour)
                            px = 0;
                }
            }
        }

        // OPENRCT2MINI v2.12: draw a g1 sprite centered in a 24×24
        // button. Handles widgets smaller than 24 by centering the
        // glyph, and respects the widget's pressed state by nudging
        // the icon down-right by (1, 1) like flatBtn does.
        //
        // GfxDrawSprite draws the sprite's top-left at
        //   (coords.x + g1->xOffset, coords.y + g1->yOffset)
        // — those baked-in offsets are part of how each sprite was
        // authored in the original RCT2 sheets (e.g. SPR_DEMOLISH has
        // a non-zero xOffset so it lines up with construction toolbar
        // slots). To make the sprite's visible top-left land at our
        // computed centered position, we have to subtract them off
        // the coords. Without this, every icon drew shifted from its
        // intended centre by however many pixels its sprite carries
        // as a hard offset — which is what the user was seeing.
        void drawCenteredSprite(RenderTarget& rt, WidgetIndex idx, uint32_t spriteId) const
        {
            const auto& widget = widgets[idx];
            const auto* g1 = GfxGetG1Element(ImageId(spriteId));
            if (g1 == nullptr)
                return;
            const int32_t btnW = widget.width();
            const int32_t btnH = widget.height();
            const int32_t offsetX = (btnW - g1->width) / 2;
            const int32_t offsetY = (btnH - g1->height) / 2;
            const int32_t pressed = isWidgetPressed(idx) ? 1 : 0;
            const ScreenCoordsXY at{
                windowPos.x + widget.left + offsetX + pressed - g1->xOffset,
                windowPos.y + widget.top + offsetY + pressed - g1->yOffset,
            };
            GfxDrawSprite(rt, ImageId(spriteId), at);
        }

        // OPENRCT2MINI v2.12: blit a 24×24 paletted glyph buffer
        // onto rt at the widget's position, skipping any pixel with
        // palette index 0 (the runtime-punched transparency).
        void drawZoomIconBuffer(RenderTarget& rt, WidgetIndex idx, const std::array<uint8_t, 24 * 24>& glyph) const
        {
            const auto& widget = widgets[idx];
            const int32_t pressed = isWidgetPressed(idx) ? 1 : 0;
            const int32_t baseX = windowPos.x + widget.left + pressed;
            const int32_t baseY = windowPos.y + widget.top + pressed;
            constexpr int32_t kIconSize = 24;
            // Per-pixel blit. rt.bits is row-major paletted; LineStride
            // includes any framework pitch padding. We respect the rt's
            // clipping rect (rt.x / rt.y → rt.x+rt.width / rt.y+rt.height)
            // since the framework gives us a clipped rt during partial
            // dirty-rect repaints.
            const int32_t clipL = rt.x;
            const int32_t clipT = rt.y;
            const int32_t clipR = rt.x + rt.width;
            const int32_t clipB = rt.y + rt.height;
            for (int32_t y = 0; y < kIconSize; y++)
            {
                const int32_t screenY = baseY + y;
                if (screenY < clipT || screenY >= clipB)
                    continue;
                auto* row = rt.bits + (screenY - rt.y) * rt.LineStride();
                for (int32_t x = 0; x < kIconSize; x++)
                {
                    const int32_t screenX = baseX + x;
                    if (screenX < clipL || screenX >= clipR)
                        continue;
                    const uint8_t px = glyph[y * kIconSize + x];
                    if (px == 0)
                        continue;
                    row[screenX - rt.x] = static_cast<Drawing::PaletteIndex>(px);
                }
            }
        }

        void onPrepareDraw() override
        {
            // OPENRCT2MINI v2.3: don't call relayoutWidgets() here.
            // It modifies widget bounds (left/right/top/bottom) mid-
            // frame, which races with the scroll-widget framework's
            // viewport caching and shows up as draw artefacts when
            // the world view scrolls behind us, the cursor moves
            // over the window, or any partial-dirty-rect repaint
            // fires. Layout is fixed at open + onResize only.
            //
            // OPENRCT2MINI v2.12: dynamic play/pause tooltip — text
            // swaps based on transport state so hover-tip matches the
            // glyph the user actually sees.
            widgets[WIDX_PLAY_PAUSE].tooltip = _playing
                ? STR_RUMBLE_EDITOR_PAUSE
                : STR_RUMBLE_EDITOR_PLAY;

            // OPENRCT2MINI v2.19: reflect per-profile flag state on
            // the checkboxes. Read-only lookup (no profile mutation)
            // so this doesn't auto-create entries during draw.
            bool disabled = false;
            bool scaleWithRate = false;
            const auto& map = Haptic::getMap();
            if (auto it = map.find(_selectedSoundId); it != map.end())
            {
                disabled = it->second.disabled;
                scaleWithRate = it->second.scaleWithRate;
            }
            setCheckboxValue(WIDX_DISABLED_CHECKBOX, disabled);
            setCheckboxValue(WIDX_SCALE_CHECKBOX, scaleWithRate);
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);

            // OPENRCT2MINI v2.12: composite the transport-row glyphs
            // on top of the now-rendered button frames. Done after
            // drawWidgets so the inset border + pressed state is in
            // place underneath. The play/pause swap is purely visual —
            // the widget itself is one button; onMouseUp toggles
            // _playing and we just pick which glyph to draw.
            const uint32_t playPauseGlyph = _playing ? SPR_TESTING : SPR_CLOSED;
            drawCenteredSprite(rt, WIDX_PLAY_PAUSE, playPauseGlyph);
            drawCenteredSprite(rt, WIDX_DELETE, SPR_DEMOLISH);

            // The zoom toolbar sprites are drawn against an opaque
            // toolbar tile background, so they can't be blitted via
            // GfxDrawSprite onto our flatBtn without showing that
            // backdrop. buildZoomIcons() pre-builds a 24×24 buffer
            // with the toolbar background colour punched out — we
            // splat that buffer here with per-pixel skip-on-zero.
            buildZoomIcons();
            drawZoomIconBuffer(rt, WIDX_ZOOM_OUT, _zoomOutIcon);
            drawZoomIconBuffer(rt, WIDX_ZOOM_IN, _zoomInIcon);

            // OPENRCT2MINI v2.14: render the read-only Mode value text
            // next to the "Mode:" label. The two were buttons in
            // v2.13 and earlier — gone now, replaced by a single
            // computed label since the mode is inferred per SoundId
            // and not user-editable. Position is the slot the
            // one-shot button used to occupy (kListWidth + 42 ish).
            const auto& modeLabel = widgets[WIDX_MODE_LABEL];
            const StringId modeStr = isContinuousSoundId(_selectedSoundId)
                ? STR_RUMBLE_EDITOR_MODE_CONTINUOUS
                : STR_RUMBLE_EDITOR_MODE_ONESHOT;
            ScreenCoordsXY modePos{ windowPos.x + modeLabel.left + 38, windowPos.y + modeLabel.top };
            drawText(rt, modePos, modeStr, { colours[1] });

            const auto& motorWidget = widgets[WIDX_MOTOR_DROPDOWN];
            const StringId motorStr = _editingHigh ? STR_RUMBLE_EDITOR_MOTOR_HIGH : STR_RUMBLE_EDITOR_MOTOR_LOW;
            ScreenCoordsXY motorPos{ windowPos.x + motorWidget.left + 4, windowPos.y + motorWidget.top + 1 };
            drawText(rt, motorPos, motorStr, { colours[1] });

            // OPENRCT2MINI v2.16: tail value text drawn INSIDE the
            // spinner field (matches Park admission-price pattern —
            // the spinner widget owns the inset frame and we paint
            // the live value at its top-left). Format "<N>ms" where
            // N = envDur - waveDur (extra rumble time past audio).
            // Click the value to pop a numpad OSK; +/- buttons step
            // by 100ms (also press-and-hold).
            const auto& tailWidget = widgets[WIDX_TAIL];
            char tailBuf[16];
            std::snprintf(tailBuf, sizeof(tailBuf), "%ums", static_cast<unsigned>(currentTailMs()));
            Formatter tailFt;
            tailFt.Add<const char*>(tailBuf);
            ScreenCoordsXY tailPos{ windowPos.x + tailWidget.left + 2, windowPos.y + tailWidget.top + 1 };
            drawText(rt, tailPos, STR_STRING, tailFt, { colours[1] });

            // OPENRCT2MINI v2.19: reduction-strength value text drawn
            // inside the spinner field, formatted as "<N>%". Read
            // live from the profile so spinner clicks / OSK entry
            // both reflect without onPrepareDraw plumbing.
            const auto& redWidget = widgets[WIDX_REDUCTION];
            uint8_t redPct = 100;
            if (auto rit = Haptic::getMap().find(_selectedSoundId); rit != Haptic::getMap().end())
                redPct = rit->second.rateScalePercent;
            char redBuf[16];
            std::snprintf(redBuf, sizeof(redBuf), "%u%%", static_cast<unsigned>(redPct));
            Formatter redFt;
            redFt.Add<const char*>(redBuf);
            ScreenCoordsXY redPos{ windowPos.x + redWidget.left + 2, windowPos.y + redWidget.top + 1 };
            drawText(rt, redPos, STR_STRING, redFt, { colours[1] });

            // OPENRCT2MINI v2.13 / v2.15: controller status sits on
            // its own strip BELOW the transport buttons, left-
            // justified just right of the audio sample list.
            int32_t rumbleCount = 0;
            for (auto* gc : OpenRCT2::Ui::GetInputManager().getGameControllers())
            {
                if (gc != nullptr && SDL_GameControllerHasRumble(gc))
                    rumbleCount++;
            }
            const int32_t statusY = height - kStatusRowHeight + 1;
            ScreenCoordsXY statusPos{ windowPos.x + kListWidth + 4, windowPos.y + statusY };
            if (rumbleCount > 0)
            {
                Formatter ft;
                ft.Add<int32_t>(rumbleCount);
                drawText(rt, statusPos, STR_HAPTICS_CONTROLLERS_DETECTED, ft, { colours[1] });
            }
            else
            {
                drawText(rt, statusPos, STR_HAPTICS_NO_CONTROLLER, {}, { colours[1] });
            }
        }

        void onScrollDraw(int32_t scrollIndex, RenderTarget& rt) override
        {
            // CONVENTION (matches ShortcutKeys.cpp:561):
            //   * rt.x / rt.y describe the visible viewport's
            //     top-left in CONTENT space (i.e. scrollOffset).
            //   * The bg-fill clear uses (rt.x, rt.y, rt.x+w-1,
            //     rt.y+h-1) so it covers the currently-visible
            //     portion of content.
            //   * Everything else (list rows, envelope curve, points,
            //     waveform peaks, playhead) is drawn in absolute
            //     content coords starting at (0, 0). The framework
            //     does the scroll-offset translation when blitting
            //     to screen.
            //
            // The v2 bug was using origin = {rt.x, rt.y} as the BASE
            // for every draw, double-applying the scroll offset and
            // leaving stale pixels — list contents appeared to
            // duplicate as the user scrolled.
            if (scrollIndex == 0)
            {
                drawListPane(rt);
            }
            else if (scrollIndex == 1)
            {
                drawEnvelopePane(rt);
            }
        }

    private:
        // ---- Layout ------------------------------------------------------

        void relayoutWidgets()
        {
            const int32_t W = width;
            const int32_t H = height;

            widgets[WIDX_PAGE_BACKGROUND].left = 0;
            widgets[WIDX_PAGE_BACKGROUND].top = kHeaderTop;
            widgets[WIDX_PAGE_BACKGROUND].right = W - 1;
            widgets[WIDX_PAGE_BACKGROUND].bottom = H - 1;

            widgets[WIDX_LIST].left = 3;
            widgets[WIDX_LIST].top = kHeaderTop + 4;
            widgets[WIDX_LIST].right = kListWidth - 4;
            widgets[WIDX_LIST].bottom = H - 5;

            widgets[WIDX_ENVELOPE_PANE].left = kListWidth + 4;
            widgets[WIDX_ENVELOPE_PANE].top = kHeaderTop + kHeaderHeight;
            widgets[WIDX_ENVELOPE_PANE].right = W - 5;
            widgets[WIDX_ENVELOPE_PANE].bottom = H - kTransportHeight - 5;

            // OPENRCT2MINI v2.16: header row — right-justify motor.
            // Mode label/value stays at left; the Motor "Motor:" +
            // dropdown + glyph button group sits flush against the
            // right edge of the window.
            // v2.17 bumps the dropdown width — even at 160px the
            // 12-pt "Low (deep rumble)" string was getting elided
            // with a trailing "...". 200px leaves plenty of margin
            // for the dropdown popup options too (which inherit the
            // source widget's width).
            constexpr int32_t kDropdownWidth = 200;
            constexpr int32_t kDropdownHeight = 14;
            constexpr int32_t kGlyphBtnWidth = 11;
            // OPENRCT2MINI v2.18: label widget uses
            // drawTextEllipsised(width = right - left) — at 35px the
            // 6-char "Motor:" exceeds the cap and renders as "Mot...".
            // 45px gives ~10px headroom for translations / fonts.
            constexpr int32_t kMotorLabelWidth = 45;
            const int32_t motorDropdownRight = W - 5;
            const int32_t motorDropdownLeft = motorDropdownRight - kDropdownWidth + 1;
            const int32_t motorGlyphLeft = motorDropdownRight - kGlyphBtnWidth - 1;
            const int32_t motorGlyphRight = motorGlyphLeft + kGlyphBtnWidth - 1;
            const int32_t motorLabelLeft = motorDropdownLeft - kMotorLabelWidth - 3;
            const int32_t motorLabelRight = motorLabelLeft + kMotorLabelWidth - 1;
            widgets[WIDX_MOTOR_LABEL].left = motorLabelLeft;
            widgets[WIDX_MOTOR_LABEL].right = motorLabelRight;
            widgets[WIDX_MOTOR_DROPDOWN].left = motorDropdownLeft;
            widgets[WIDX_MOTOR_DROPDOWN].right = motorDropdownRight;
            widgets[WIDX_MOTOR_DROPDOWN].top = kHeaderTop + 3;
            widgets[WIDX_MOTOR_DROPDOWN].bottom = kHeaderTop + 3 + kDropdownHeight - 1;
            widgets[WIDX_MOTOR_DROPDOWN_BUTTON].left = motorGlyphLeft;
            widgets[WIDX_MOTOR_DROPDOWN_BUTTON].right = motorGlyphRight;

            // OPENRCT2MINI v2.13 / v2.16: transport row layout.
            //   • Play/Pause + Delete at LEFT edge against the list.
            //   • "Tail:" label + spinner directly right of Delete.
            //   • Zoom Out + Zoom In flush to the RIGHT edge.
            //   • Status text below the row is rendered in onDraw.
            const int32_t buttonRowY = H - kTransportHeight;
            const int32_t buttonBottom = buttonRowY + kButtonRowHeight;
            for (auto idx : { WIDX_PLAY_PAUSE, WIDX_DELETE, WIDX_ZOOM_OUT, WIDX_ZOOM_IN })
            {
                widgets[idx].top = buttonRowY;
                widgets[idx].bottom = buttonBottom;
            }

            // Left-anchored group: Play, Delete.
            widgets[WIDX_PLAY_PAUSE].left = kListWidth + 4;
            widgets[WIDX_PLAY_PAUSE].right = kListWidth + 4 + 24 - 1;
            widgets[WIDX_DELETE].left = kListWidth + 32;
            widgets[WIDX_DELETE].right = kListWidth + 32 + 24 - 1;

            // Tail label + spinner group: vertically centred in the
            // 24px button row (label/spinner are 12/14 tall). Sit
            // directly right of the Delete button.
            constexpr int32_t kSpinnerWidth = 76;
            constexpr int32_t kSpinnerHeight = 14;
            constexpr int32_t kTailLabelWidth = 28;
            const int32_t tailLabelLeft = kListWidth + 60;
            const int32_t spinnerLeft = tailLabelLeft + kTailLabelWidth + 4;
            const int32_t spinnerRight = spinnerLeft + kSpinnerWidth - 1;
            const int32_t spinnerTop = buttonRowY + (kButtonRowHeight - kSpinnerHeight) / 2;
            const int32_t spinnerBottom = spinnerTop + kSpinnerHeight - 1;
            widgets[WIDX_TAIL_LABEL].left = tailLabelLeft;
            widgets[WIDX_TAIL_LABEL].right = tailLabelLeft + kTailLabelWidth - 1;
            widgets[WIDX_TAIL_LABEL].top = buttonRowY + (kButtonRowHeight - 12) / 2 + 1;
            widgets[WIDX_TAIL_LABEL].bottom = widgets[WIDX_TAIL_LABEL].top + 12 - 1;
            widgets[WIDX_TAIL].left = spinnerLeft;
            widgets[WIDX_TAIL].right = spinnerRight;
            widgets[WIDX_TAIL].top = spinnerTop;
            widgets[WIDX_TAIL].bottom = spinnerBottom;
            // The +/- buttons sit on the right side of the spinner
            // field, matching makeSpinnerWidgets' geometry: decrease
            // at (right - 26 + 1)..(right - 14), increase at
            // (right - 13)..(right - 1). Heights = spinner - 2 rows.
            const int32_t incRight = spinnerRight - 1;
            const int32_t incLeft = incRight - 12 + 1;
            const int32_t decRight = incLeft - 1;
            const int32_t decLeft = decRight - 13 + 1;
            widgets[WIDX_TAIL_INCREASE].left = incLeft;
            widgets[WIDX_TAIL_INCREASE].right = incRight;
            widgets[WIDX_TAIL_INCREASE].top = spinnerTop + 1;
            widgets[WIDX_TAIL_INCREASE].bottom = spinnerBottom - 1;
            widgets[WIDX_TAIL_DECREASE].left = decLeft;
            widgets[WIDX_TAIL_DECREASE].right = decRight;
            widgets[WIDX_TAIL_DECREASE].top = spinnerTop + 1;
            widgets[WIDX_TAIL_DECREASE].bottom = spinnerBottom - 1;

            // Right-anchored group. 4px padding from the window's
            // right edge, 4px gap between the two zoom buttons.
            const int32_t zoomInRight = W - 5;
            const int32_t zoomInLeft = zoomInRight - 24 + 1;
            const int32_t zoomOutRight = zoomInLeft - 4 - 1;
            const int32_t zoomOutLeft = zoomOutRight - 24 + 1;
            widgets[WIDX_ZOOM_OUT].left = zoomOutLeft;
            widgets[WIDX_ZOOM_OUT].right = zoomOutRight;
            widgets[WIDX_ZOOM_IN].left = zoomInLeft;
            widgets[WIDX_ZOOM_IN].right = zoomInRight;
        }

        // ---- Profile + envelope helpers ---------------------------------

        RumbleProfile* currentProfile()
        {
            if (_selectedSoundId == Audio::SoundId::null)
                return nullptr;
            auto& map = Haptic::getMap();
            auto it = map.find(_selectedSoundId);
            if (it == map.end())
            {
                // Auto-create. Default duration = sample length if
                // available, else 1 second.
                RumbleProfile fresh;
                fresh.envelopeDurationMs
                    = (_waveformFor == _selectedSoundId && _waveform.durationMs > 0) ? _waveform.durationMs : 1000;
                fresh.mode = inferModeForSound(_selectedSoundId);
                auto [insIt, _] = map.emplace(_selectedSoundId, std::move(fresh));
                return &insIt->second;
            }
            if (it->second.envelopeDurationMs == 0)
                it->second.envelopeDurationMs = (_waveformFor == _selectedSoundId && _waveform.durationMs > 0)
                    ? _waveform.durationMs
                    : 1000;
            // OPENRCT2MINI v2.14: profile.mode is determined by the
            // SoundId's call path (Audio::Play vs Vehicle::UpdateSound),
            // not by the user. Force it to the inferred value so the
            // Haptic engine routes correctly even for profiles loaded
            // from older JSON that may have a stale/wrong mode set.
            const auto inferred = inferModeForSound(_selectedSoundId);
            if (it->second.mode != inferred)
            {
                it->second.mode = inferred;
                markDirty();
            }
            return &it->second;
        }

        MotorEnvelope* currentEnvelope()
        {
            auto* p = currentProfile();
            if (p == nullptr)
                return nullptr;
            return _editingHigh ? &p->high : &p->low;
        }

        uint32_t currentEnvelopeDurationMs() const
        {
            auto& map = Haptic::getMap();
            auto it = map.find(_selectedSoundId);
            if (it != map.end() && it->second.envelopeDurationMs > 0)
                return it->second.envelopeDurationMs;
            // Untouched SoundId — if waveform loaded, use its
            // duration; else fall back to 1s.
            if (_waveformFor == _selectedSoundId && _waveform.durationMs > 0)
                return _waveform.durationMs;
            return 1000;
        }

        void ensureWaveform()
        {
            if (_selectedSoundId == _waveformFor && !_waveform.peaks.empty())
                return;
            _waveform = {};
            _waveformFor = _selectedSoundId;
            (void)buildWaveformPeaks(_selectedSoundId, _waveform);

            // OPENRCT2MINI v2.10: pin profile.envelopeDurationMs to the
            // actual audio length whenever we have it. The user wants
            // rumble timing driven by audio length always — and this
            // is the lever that makes that real:
            //
            //  * One-shot:  the rumble engine retires at envDur, so
            //    bumping it to waveDur keeps the rumble alive for the
            //    full audio sample (the existing point shape still
            //    governs amplitude — past the last point it's 0, so
            //    the audible rumble is unchanged for "ends at 0"
            //    defaults; the engine just no longer cuts out short).
            //  * Continuous / looped: the rumble engine wraps the
            //    envelope at envDur (`t = t % envelopeDurationMs` in
            //    HapticEvent.cpp). Without this fix, the envelope
            //    looped at e.g. 60 ms while the audio looped at 100 ms,
            //    drifting them out of phase. With envDur = waveDur the
            //    two cycles share the same period and stay locked.
            //
            // The whole bezier-doesn't-reach-the-waveform-end visual
            // bug is also a consequence of envDur < waveDur — the
            // timeline width is max(envDur, waveDur), the curve only
            // ranges over envDur, so a gap appears on the right.
            // Equalising them removes the gap (timelineMs == envDur).
            //
            // v2.15: this is now a FLOOR, not an exact pin — envDur
            // ≥ waveDur. Letting envDur exceed waveDur is how the
            // Tail spinner extends the rumble past the audio's natural
            // end so short one-shot samples have enough time for the
            // motor to fire perceptibly.
            if (_waveform.durationMs > 0)
            {
                auto& map = Haptic::getMap();
                auto it = map.find(_selectedSoundId);
                if (it != map.end() && it->second.envelopeDurationMs < _waveform.durationMs)
                {
                    it->second.envelopeDurationMs = _waveform.durationMs;
                    markDirty();
                }
            }
        }

        void markDirty()
        {
            _saveDirty = true;
            _saveDirtyMs = Platform::GetTicks();
        }

        // ---- List pane ---------------------------------------------------

        void handleListClick(const ScreenCoordsXY& sc)
        {
            const int32_t row = sc.y / kListItemHeight;
            if (row < 0 || row >= static_cast<int32_t>(kSoundNamesCount))
                return;
            if (_selectedListIndex == row)
                return;
            _selectedListIndex = row;
            _selectedSoundId = kSoundNames[row].id;
            _selectedPointIndex = -1;
            _dragActive = false;
            _playheadMs = 0;
            stopAudio();
            ensureWaveform();
            // Re-init scroll so envelope pane content width updates.
            WindowInitScrollWidgets(*this);
            invalidate();
        }

        void drawListPane(RenderTarget& rt)
        {
            // Background fill — covers the *visible* viewport in
            // content coords (rt.x/y is the scroll offset).
            const ScreenCoordsXY visTL{ rt.x, rt.y };
            Rectangle::fill(
                rt,
                ScreenRect{
                    visTL,
                    visTL + ScreenCoordsXY{ rt.width - 1, rt.height - 1 },
                },
                getColourMap(colours[1].colour).midDark);

            // Rows in absolute content coords starting at (0, 0).
            // Framework clips to the visible viewport.
            for (size_t i = 0; i < kSoundNamesCount; i++)
            {
                const auto y = static_cast<int32_t>(i) * kListItemHeight;
                const auto& soundEntry = kSoundNames[i];
                const auto isSelected = (_selectedListIndex == static_cast<int32_t>(i));
                if (isSelected)
                {
                    // Selection highlight stretches the full visible
                    // width so it looks contiguous regardless of
                    // horizontal scroll position (the list itself
                    // doesn't scroll horizontally, but the framework
                    // still tracks it).
                    Rectangle::fill(
                        rt,
                        ScreenRect{
                            { rt.x, y },
                            { rt.x + rt.width - 1, y + kListItemHeight - 1 },
                        },
                        getColourMap(colours[1].colour).lighter);
                }

                const auto& map = Haptic::getMap();
                bool hasLow = false;
                bool hasHigh = false;
                if (auto it = map.find(soundEntry.id); it != map.end())
                {
                    hasLow = !it->second.low.points.empty();
                    hasHigh = !it->second.high.points.empty();
                }
                drawIndicator(rt, 4, y + 2, hasLow);
                drawIndicator(rt, 12, y + 2, hasHigh);

                Formatter ft;
                ft.Add<const char*>(soundEntry.name);
                drawText(rt, { 22, y }, STR_STRING, ft, { colours[1] });
            }
        }

        // ---- Envelope pane ---------------------------------------------

        void drawEnvelopePane(RenderTarget& rt) const
        {
            const auto envDur = currentEnvelopeDurationMs();
            const auto waveDur = (_waveformFor == _selectedSoundId) ? _waveform.durationMs : 0u;
            // Total timeline width = whichever of envelope or audio
            // is longer. Same value as the scroll content width.
            const uint32_t timelineMs = std::max(envDur, waveDur);
            const auto& widget = widgets[WIDX_ENVELOPE_PANE];
            constexpr int32_t kScrollBarHeight = 10; // matches kScrollBarWidth
            const int32_t paneFullHeight = std::max<int32_t>(1, widget.height() - kScrollBarHeight - 2);
            // OPENRCT2MINI v2.11: reserve the top kRulerHeight rows
            // for the timeline ruler. The body (waveform / envelope /
            // playhead) lives below the ruler.
            const int32_t bodyTop = kRulerHeight;
            const int32_t bodyHeight = std::max<int32_t>(1, paneFullHeight - kRulerHeight);

            // Background fill — covers the *visible* viewport in
            // content coords (rt.x/y is the scroll offset).
            const ScreenCoordsXY visTL{ rt.x, rt.y };
            Rectangle::fill(
                rt,
                ScreenRect{
                    visTL,
                    visTL + ScreenCoordsXY{ rt.width - 1, rt.height - 1 },
                },
                PaletteIndex::pi10);

            // ---- Timeline ruler --------------------------------------
            // Pick a label spacing in ms so the labels sit ~50–80 px
            // apart at the current zoom — close enough to read the
            // axis, far enough not to overlap. Values chosen so each
            // step is a "nice" number that subdivides cleanly.
            uint32_t labelMs;
            if (_pixelsPerMs >= 0.5f)
                labelMs = 100;
            else if (_pixelsPerMs >= 0.2f)
                labelMs = 250;
            else if (_pixelsPerMs >= 0.1f)
                labelMs = 500;
            else if (_pixelsPerMs >= 0.04f)
                labelMs = 1000;
            else
                labelMs = 2000;
            // Minor ticks at 1/4 of label spacing for a sense of scale.
            uint32_t minorMs = std::max<uint32_t>(1, labelMs / 4);

            // Ruler background strip (slightly different shade so it
            // reads as a separate row). Drawn across the full timeline
            // width; the framework clips to the visible viewport.
            const int32_t rulerBottom = bodyTop - 1;
            Rectangle::fill(
                rt, ScreenRect{ { 0, 0 }, { static_cast<int32_t>(timelineMs * _pixelsPerMs), rulerBottom } },
                PaletteIndex::pi40);
            // Hairline along the bottom of the ruler row to separate
            // it from the body.
            Rectangle::fill(
                rt, ScreenRect{ { 0, rulerBottom }, { static_cast<int32_t>(timelineMs * _pixelsPerMs), rulerBottom } },
                PaletteIndex::pi42);

            // Minor ticks (small marks).
            for (uint32_t t = 0; t <= timelineMs; t += minorMs)
            {
                const int32_t x = static_cast<int32_t>(t * _pixelsPerMs);
                Rectangle::fill(
                    rt, ScreenRect{ { x, kRulerHeight - 4 }, { x, kRulerHeight - 2 } },
                    PaletteIndex::pi42);
            }

            // Major ticks + labels.
            for (uint32_t t = 0; t <= timelineMs; t += labelMs)
            {
                const int32_t x = static_cast<int32_t>(t * _pixelsPerMs);
                Rectangle::fill(
                    rt, ScreenRect{ { x, kRulerHeight - 6 }, { x, kRulerHeight - 2 } },
                    PaletteIndex::pi138);
                // Label text. Skip the very first label at t=0 to
                // avoid clipping, and avoid drawing the last one if
                // it'd run off the right edge.
                if (t == 0)
                    continue;
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(t));
                Formatter ft;
                ft.Add<const char*>(buf);
                drawText(rt, { x + 2, 1 }, STR_STRING, ft, { colours[1] });
            }

            // ---- Body ------------------------------------------------
            const int32_t midY = bodyTop + bodyHeight / 2;
            const int32_t contentRight = static_cast<int32_t>(timelineMs * _pixelsPerMs);

            // Centre line — full timeline width.
            Rectangle::fill(
                rt, ScreenRect{ { 0, midY }, { contentRight, midY } },
                PaletteIndex::pi40);

            // PCM waveform peaks centred on midY. Each ms maps to
            // _pixelsPerMs columns; we draw a peak bar at every
            // *pixel* and average across the ms range it covers, so
            // zooming out doesn't sparse-sample the peaks (which
            // would hide loud transients between sampled columns).
            if (_waveformFor == _selectedSoundId && !_waveform.peaks.empty())
            {
                const uint32_t end = _waveform.durationMs;
                const int32_t halfH = bodyHeight / 2 - 1;
                const int32_t totalCols = static_cast<int32_t>(end * _pixelsPerMs);
                for (int32_t x = 0; x < totalCols; x++)
                {
                    // Range of ms covered by this pixel column.
                    const uint32_t msStart = static_cast<uint32_t>(std::floor(x / _pixelsPerMs));
                    const uint32_t msEndExcl = std::min(end, static_cast<uint32_t>(std::floor((x + 1) / _pixelsPerMs)) + 1);
                    uint8_t maxPeak = 0;
                    for (uint32_t m = msStart; m < msEndExcl && m < end; m++)
                    {
                        if (_waveform.peaks[m] > maxPeak)
                            maxPeak = _waveform.peaks[m];
                    }
                    const int32_t bar = (maxPeak * halfH) / 255;
                    if (bar <= 0)
                        continue;
                    Rectangle::fill(
                        rt,
                        ScreenRect{ { x, midY - bar }, { x, midY + bar } },
                        PaletteIndex::pi17);
                }
            }

            // OPENRCT2MINI v2.15: draw BOTH motor envelopes so the
            // user can see the shape of the non-selected motor while
            // editing the selected one. Each motor uses a distinct
            // hue (Low = warm red, High = cool blue) with a brighter
            // shade when selected and a muted dark shade otherwise.
            // Edit hit-testing still uses currentEnvelope() so only
            // the selected motor's points are interactive — the dim
            // line is read-only.
            //
            // Draw order: unselected first, selected second, so the
            // selected line draws ON TOP if the two envelopes overlap.
            auto& map = Haptic::getMap();
            auto it = map.find(_selectedSoundId);
            if (it != map.end())
            {
                const auto& low = it->second.low;
                const auto& high = it->second.high;
                // Palette picks:
                //   Low  selected/unsel = pi172 (Bright Red light) / pi61 (Bordeaux dark)
                //   High selected/unsel = pi138 (Light Blue light) / pi136 (Icy Blue dark)
                const auto lowColSel    = PaletteIndex::pi172;
                const auto lowColUnsel  = PaletteIndex::pi61;
                const auto highColSel   = PaletteIndex::pi138;
                const auto highColUnsel = PaletteIndex::pi136;
                const auto lowColour    = _editingHigh ? lowColUnsel  : lowColSel;
                const auto highColour   = _editingHigh ? highColSel   : highColUnsel;

                // Order: draw the unselected envelope first so the
                // selected one paints over it where they overlap.
                if (_editingHigh)
                {
                    drawEnvelopeCurve(rt, low, lowColour, bodyTop, bodyHeight, timelineMs);
                    drawEnvelopeCurve(rt, high, highColour, bodyTop, bodyHeight, timelineMs);
                    drawEnvelopePoints(rt, high, highColour, bodyTop, bodyHeight, timelineMs);
                }
                else
                {
                    drawEnvelopeCurve(rt, high, highColour, bodyTop, bodyHeight, timelineMs);
                    drawEnvelopeCurve(rt, low, lowColour, bodyTop, bodyHeight, timelineMs);
                    drawEnvelopePoints(rt, low, lowColour, bodyTop, bodyHeight, timelineMs);
                }
            }

            // Playback cursor — full pane height, including the ruler
            // row so the playhead reads cleanly all the way up.
            if (_playheadMs <= timelineMs)
            {
                const int32_t x = static_cast<int32_t>(_playheadMs * _pixelsPerMs);
                Rectangle::fill(
                    rt, ScreenRect{ { x, 0 }, { x, paneFullHeight - 1 } },
                    PaletteIndex::yellow7);
            }
        }

        // OPENRCT2MINI v2.15: draw one envelope's bezier curve in a
        // given palette colour. Splits the loop in pixel space (not
        // ms space) so the curve stays smooth at any zoom level.
        // Tail past the last authored point is drawn as an explicit
        // 3px-thick horizontal segment to make the "envelope holds at
        // last value" rule visually obvious for "ends at 0" defaults.
        void drawEnvelopeCurve(
            RenderTarget& rt, const MotorEnvelope& env, PaletteIndex colour,
            int32_t bodyTop, int32_t bodyHeight, uint32_t timelineMs) const
        {
            if (env.points.empty())
                return;
            const uint32_t lastT = env.points.back().timeMs;
            const int32_t lastX = static_cast<int32_t>(lastT * _pixelsPerMs);
            int32_t prevX = -1, prevY = -1;
            for (int32_t x = 0; x <= lastX; x++)
            {
                const uint32_t t = static_cast<uint32_t>(x / _pixelsPerMs);
                const float v = Haptic::evaluate(env, t);
                const int32_t y = (bodyTop + bodyHeight - 1)
                    - static_cast<int32_t>(v * (bodyHeight - 1));
                if (prevX >= 0)
                {
                    const int32_t yMin = std::min(prevY, y);
                    const int32_t yMax = std::max(prevY, y);
                    Rectangle::fill(
                        rt, ScreenRect{ { x, yMin }, { x, yMax } }, colour);
                }
                prevX = x;
                prevY = y;
            }
            if (lastT < timelineMs)
            {
                const float vTail = std::clamp(env.points.back().intensity, 0.0f, 1.0f);
                const int32_t yTail = (bodyTop + bodyHeight - 1)
                    - static_cast<int32_t>(vTail * (bodyHeight - 1));
                const int32_t y0 = std::max(bodyTop, yTail - 1);
                const int32_t y1 = std::min(bodyTop + bodyHeight - 1, yTail + 1);
                const int32_t xStart = lastX;
                const int32_t xEnd = static_cast<int32_t>(timelineMs * _pixelsPerMs);
                Rectangle::fill(
                    rt, ScreenRect{ { xStart, y0 }, { xEnd, y1 } }, colour);
            }
        }

        // OPENRCT2MINI v2.15: draw the point markers for one envelope.
        // Only the selected motor calls this; the unselected motor's
        // line is read-only so its dots aren't shown.
        void drawEnvelopePoints(
            RenderTarget& rt, const MotorEnvelope& env, PaletteIndex curveColour,
            int32_t bodyTop, int32_t bodyHeight, uint32_t timelineMs) const
        {
            for (size_t pi = 0; pi < env.points.size(); pi++)
            {
                const auto& pt = env.points[pi];
                if (pt.timeMs > timelineMs)
                    continue;
                const int32_t x = static_cast<int32_t>(pt.timeMs * _pixelsPerMs);
                const int32_t y = (bodyTop + bodyHeight - 1)
                    - static_cast<int32_t>(pt.intensity * (bodyHeight - 1));
                const bool sel = (static_cast<int32_t>(pi) == _selectedPointIndex);
                drawPointMarker(rt, x, y, sel, curveColour);
            }
        }

        static void drawPointMarker(RenderTarget& rt, int32_t cx, int32_t cy, bool selected, PaletteIndex curveColour)
        {
            const auto col = selected ? PaletteIndex::yellow7 : curveColour;
            Rectangle::fill(
                rt, ScreenRect{ { cx - 1, cy - 1 }, { cx + 1, cy + 1 } }, col);
            if (!selected)
            {
                Rectangle::fill(
                    rt, ScreenRect{ { cx, cy }, { cx, cy } }, PaletteIndex::pi10);
            }
        }

        static void drawIndicator(RenderTarget& rt, int32_t x, int32_t y, bool filled)
        {
            const auto col = filled ? PaletteIndex::pi138 : PaletteIndex::pi40;
            Rectangle::fill(
                rt, ScreenRect{ { x, y }, { x + 5, y + 5 } }, col);
            if (!filled)
            {
                Rectangle::fill(
                    rt, ScreenRect{ { x + 1, y + 1 }, { x + 4, y + 4 } },
                    PaletteIndex::pi10);
            }
        }

        // ---- Pane interaction ------------------------------------------

        void handlePaneClick(const ScreenCoordsXY& sc, bool startDrag)
        {
            const auto envDur = currentEnvelopeDurationMs();
            const auto& widget = widgets[WIDX_ENVELOPE_PANE];
            const int32_t paneFullHeight = widget.height() - 12; // minus scrollbar height
            const int32_t bodyHeight = std::max<int32_t>(1, paneFullHeight - kRulerHeight);

            const auto cx = sc.x;
            const auto cy = sc.y;
            // Ignore clicks inside the timeline ruler row — that
            // region isn't editable, it's just an axis label strip.
            if (cy < kRulerHeight)
                return;
            const int32_t bodyY = cy - kRulerHeight;
            const uint32_t timeMs = static_cast<uint32_t>(std::clamp<int32_t>(
                static_cast<int32_t>(cx / _pixelsPerMs), 0, static_cast<int32_t>(envDur)));
            const float intensity = std::clamp(
                1.0f - static_cast<float>(bodyY) / static_cast<float>(std::max(1, bodyHeight - 1)),
                0.0f, 1.0f);

            auto* env = currentEnvelope();
            if (env == nullptr)
                return;

            int32_t hitIdx = -1;
            for (size_t i = 0; i < env->points.size(); i++)
            {
                const auto& pt = env->points[i];
                if (pt.timeMs > envDur)
                    continue;
                const int32_t px = static_cast<int32_t>(pt.timeMs * _pixelsPerMs);
                const int32_t py = kRulerHeight + bodyHeight - 1
                    - static_cast<int32_t>(pt.intensity * (bodyHeight - 1));
                if (std::abs(px - cx) <= kPointHitRadius && std::abs(py - cy) <= kPointHitRadius)
                {
                    hitIdx = static_cast<int32_t>(i);
                    break;
                }
            }

            if (hitIdx >= 0)
            {
                _selectedPointIndex = hitIdx;
                if (startDrag)
                {
                    _dragActive = true;
                    _dragPointIndex = hitIdx;
                }
            }
            else
            {
                EnvelopePoint pt{ timeMs, intensity };
                env->points.push_back(pt);
                std::sort(env->points.begin(), env->points.end(),
                    [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
                for (size_t i = 0; i < env->points.size(); i++)
                {
                    if (env->points[i].timeMs == timeMs
                        && std::fabs(env->points[i].intensity - intensity) < 0.001f)
                    {
                        _selectedPointIndex = static_cast<int32_t>(i);
                        if (startDrag)
                        {
                            _dragActive = true;
                            _dragPointIndex = static_cast<int32_t>(i);
                        }
                        break;
                    }
                }
                markDirty();
            }
            invalidate();
        }

        void handlePaneDrag(const ScreenCoordsXY& sc)
        {
            if (!_dragActive || _dragPointIndex < 0)
                return;
            auto* env = currentEnvelope();
            if (env == nullptr || _dragPointIndex >= static_cast<int32_t>(env->points.size()))
                return;

            const auto envDur = currentEnvelopeDurationMs();
            const auto& widget = widgets[WIDX_ENVELOPE_PANE];
            const int32_t paneFullHeight = widget.height() - 12;
            const int32_t bodyHeight = std::max<int32_t>(1, paneFullHeight - kRulerHeight);
            // Drag y is in body coords (subtract ruler row). Allow
            // negative when the user drags up off the body — clamp
            // to 0 below so intensity caps at 1.0.
            const int32_t bodyY = sc.y - kRulerHeight;

            uint32_t timeMs = static_cast<uint32_t>(std::clamp<int32_t>(
                static_cast<int32_t>(sc.x / _pixelsPerMs), 0, static_cast<int32_t>(envDur)));
            float intensity = std::clamp(
                1.0f - static_cast<float>(bodyY) / static_cast<float>(std::max(1, bodyHeight - 1)),
                0.0f, 1.0f);

            // Don't let the dragged point pass its neighbours.
            if (_dragPointIndex > 0)
                timeMs = std::max(timeMs, env->points[_dragPointIndex - 1].timeMs);
            if (_dragPointIndex + 1 < static_cast<int32_t>(env->points.size()))
                timeMs = std::min(timeMs, env->points[_dragPointIndex + 1].timeMs);

            env->points[_dragPointIndex].timeMs = timeMs;
            env->points[_dragPointIndex].intensity = intensity;
            _selectedPointIndex = _dragPointIndex;
            markDirty();
            invalidate();
        }

        void deleteSelectedPoint()
        {
            if (_selectedPointIndex < 0)
                return;
            auto* env = currentEnvelope();
            if (env == nullptr || _selectedPointIndex >= static_cast<int32_t>(env->points.size()))
                return;
            env->points.erase(env->points.begin() + _selectedPointIndex);
            _selectedPointIndex = -1;
            _dragActive = false;
            markDirty();
            invalidate();
        }

        // ---- Playback ---------------------------------------------------

        void startPlayback()
        {
            if (_selectedSoundId == Audio::SoundId::null)
                return;
            stopAudio();
            _playStartMs = Platform::GetTicks();
            _playheadMs = 0;
            _playing = true;

            auto* p = currentProfile();
            const bool continuous = (p != nullptr && p->mode == RumbleMode::continuous);
            // Always go through CreateAudioChannel so we get a handle
            // to query GetOffset() in onUpdate. For one-shot, the
            // mixer drops the channel when the sample finishes; we
            // detect that in onUpdate and pin the playhead at the end.
            _channel = Audio::CreateAudioChannel(_selectedSoundId, /*loop=*/continuous);
            invalidate();
        }

        void stopPlayback()
        {
            _playing = false;
            stopAudio();
            Haptic::rumble(0.0f, 0.0f, 0);
            invalidate();
        }

        void stopAudio()
        {
            if (_channel != nullptr)
            {
                _channel->Stop();
                _channel.reset();
            }
        }
    };

    WindowBase* RumbleEditorOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::rumbleEditor);
        if (window == nullptr)
        {
            window = windowMgr->Create<RumbleEditorWindow>(
                WindowClass::rumbleEditor, kMinSize, WindowFlags{ WindowFlag::resizable });
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows
