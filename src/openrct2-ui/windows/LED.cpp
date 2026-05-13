/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI input-plan Track 2 §4.2: LED Options window. Reachable
// from the Cheats / Debug dropdown beneath the Rumble Editor entry
// (gated on Config::general.debuggingTools, same as Haptics and the
// profiler). Mirrors the Haptics panel exactly:
//
//   [x] Enable LED                    ← maps to gamepadLedEnabled
//   Brightness: [- |  50% | +]        ← maps to gamepadLedBrightness
//   [ Test LED ]
//   1 LED-capable controller(s)       ← live status
//
// Pressing Test LED slams the lightbar to critical-red for 1 second
// then triggers the engine's 300 ms fade-to-off so the user can
// confirm the pad's lightbar is wired up. Unchecking Enable triggers
// Led::forceClear() — without that, a previously-active severity
// would stay lit until the next news-item retire.

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/UiStringIds.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/config/Config.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/haptic/LedEvent.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/ui/WindowManager.h>

namespace OpenRCT2::Ui::Windows
{
    // OPENRCT2MINI input-plan Track 2 §4.3: extended height (110 →
    // 175) for the three per-severity flash toggles below the Test
    // button. Status line shifts from y=88 to y=153 to match the
    // Haptics window's layout.
    static constexpr ScreenSize kLedWindowSize = { 280, 175 };

    // Test fire holds the lightbar at critical-red for this long
    // before issuing clear() so the engine's 300 ms fade takes over.
    // 1000 ms matches the plan acceptance criteria.
    static constexpr uint32_t kLedTestDurationMs = 1000;

    // ODR-unique tag — LTO whole-program check flags `enum WidgetIdx` clashes
    // across TUs sharing this namespace (Haptics, RumbleEditor, etc.).
    enum LEDWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_ENABLE_CHECKBOX,
        WIDX_BRIGHTNESS_LABEL,
        // Spinner: value display + decrease + increase buttons.
        WIDX_BRIGHTNESS_SPINNER,
        WIDX_BRIGHTNESS_INCREASE,
        WIDX_BRIGHTNESS_DECREASE,
        WIDX_TEST_BUTTON,
        // OPENRCT2MINI input-plan Track 2 §4.3: per-severity toggles.
        WIDX_ON_CRITICAL,
        WIDX_ON_WARNING,
        WIDX_ON_MONEY,
    };

    // clang-format off
    static const auto kLedWidgets = makeWidgets(
        makeWindowShim   (STR_LED, kLedWindowSize),
        makeWidget       ({  10, 25 }, { 200, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_LED_ENABLE       ),
        makeWidget       ({  10, 45 }, { 100, 12 }, WidgetType::label,    WindowColour::secondary, STR_LED_BRIGHTNESS   ),
        makeSpinnerWidgets({ 110, 43 }, {  90, 14 }, WidgetType::spinner,  WindowColour::secondary, kStringIdNone         ),
        makeWidget       ({  10, 65 }, { 100, 14 }, WidgetType::button,   WindowColour::secondary, STR_LED_TEST           ),
        makeWidget       ({  10, 88 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_LED_ON_CRITICAL    ),
        makeWidget       ({  10,103 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_LED_ON_WARNING     ),
        makeWidget       ({  10,118 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_LED_ON_MONEY       )
    );
    // clang-format on

    class LedWindow final : public Window
    {
        // SDL_GetTicks deadline at which the test-fire pulse should
        // hand off to Led::clear() (engine then fades to black). 0
        // means "no test active". Polled by onUpdate; the window
        // must stay open at least one frame past the deadline for
        // the hand-off to actually fire — which is fine because
        // closing the window during a test calls clear() in onClose.
        uint32_t _testEndsMs = 0;

    public:
        void onOpen() override
        {
            setWidgets(kLedWidgets);
            WindowInitScrollWidgets(*this);
        }

        void onClose() override
        {
            // If a test was still running when the user closed the
            // window, retire the LED so it doesn't stay lit. clear()
            // on a non-active state is a silent no-op so this is
            // safe to call unconditionally.
            if (_testEndsMs != 0)
            {
                OpenRCT2::Led::clear();
                _testEndsMs = 0;
            }
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    return;

                case WIDX_ENABLE_CHECKBOX:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedEnabled = !cfg.gamepadLedEnabled;
                    // Toggling enable off mid-news leaves the lightbar
                    // stuck at the active severity colour (SetController-
                    // LED short-circuits on !gamepadLedEnabled so no
                    // further SDL writes ever happen). forceClear sweeps
                    // it back to (0,0,0) so the un-checked state
                    // visually matches the config.
                    if (!cfg.gamepadLedEnabled)
                        OpenRCT2::Led::forceClear();
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_BRIGHTNESS_INCREASE:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedBrightness = std::min(1.0f, cfg.gamepadLedBrightness + 0.05f);
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_BRIGHTNESS_DECREASE:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedBrightness = std::max(0.0f, cfg.gamepadLedBrightness - 0.05f);
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_TEST_BUTTON:
                {
                    // OPENRCT2MINI input-plan Track 2 §4.2: 1-second
                    // critical-severity flash followed by the engine's
                    // 300 ms fade-to-off. setActive replaces any
                    // existing state immediately (no fade through),
                    // so pressing Test while a news item is on the
                    // ticker will steal the lightbar — which is what
                    // we want; the user is explicitly testing it.
                    // Brightness multiplier in SetControllerLED is
                    // honoured automatically. Test bypasses the per-
                    // severity flags (§4.3) so the user can verify
                    // the lightbar even with critical flash disabled.
                    OpenRCT2::Led::setActive(OpenRCT2::News::Severity::critical);
                    _testEndsMs = SDL_GetTicks() + kLedTestDurationMs;
                    return;
                }

                // OPENRCT2MINI input-plan Track 2 §4.3: per-severity
                // gates. All three gate at the NewsItem::TickCurrent
                // call site (so Test LED above keeps working even
                // when critical is disabled here).
                case WIDX_ON_CRITICAL:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedOnCritical = !cfg.gamepadLedOnCritical;
                    Config::Save();
                    invalidate();
                    return;
                }
                case WIDX_ON_WARNING:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedOnWarning = !cfg.gamepadLedOnWarning;
                    Config::Save();
                    invalidate();
                    return;
                }
                case WIDX_ON_MONEY:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadLedOnMoney = !cfg.gamepadLedOnMoney;
                    Config::Save();
                    invalidate();
                    return;
                }
            }
        }

        void onUpdate() override
        {
            // Poll for the test-fire deadline. onUpdate is called once
            // per game tick (~40 Hz), which is more than fast enough
            // resolution for a 1-second pulse: the user can't perceive
            // the difference between 1000 ms and 1025 ms.
            if (_testEndsMs != 0 && SDL_GetTicks() >= _testEndsMs)
            {
                OpenRCT2::Led::clear();
                _testEndsMs = 0;
            }
        }

        void onPrepareDraw() override
        {
            const auto& cfg = Config::Get().general;
            setCheckboxValue(WIDX_ENABLE_CHECKBOX, cfg.gamepadLedEnabled);

            // Grey out brightness controls and Test button when the
            // master switch is off so it's visually obvious nothing
            // will fire.
            const bool live = cfg.gamepadLedEnabled;
            widgets[WIDX_BRIGHTNESS_LABEL].type = WidgetType::label;
            widgets[WIDX_BRIGHTNESS_SPINNER].type = WidgetType::spinner;
            widgets[WIDX_BRIGHTNESS_INCREASE].type = WidgetType::button;
            widgets[WIDX_BRIGHTNESS_DECREASE].type = WidgetType::button;
            widgets[WIDX_TEST_BUTTON].type = WidgetType::button;

            setWidgetDisabled(WIDX_BRIGHTNESS_SPINNER, !live);
            setWidgetDisabled(WIDX_BRIGHTNESS_INCREASE, !live);
            setWidgetDisabled(WIDX_BRIGHTNESS_DECREASE, !live);
            setWidgetDisabled(WIDX_TEST_BUTTON, !live);

            // OPENRCT2MINI input-plan Track 2 §4.3: per-severity toggles.
            // Greyed out when the master LED switch is off — the
            // per-severity flags can't override the master kill switch.
            setCheckboxValue(WIDX_ON_CRITICAL, cfg.gamepadLedOnCritical);
            setCheckboxValue(WIDX_ON_WARNING,  cfg.gamepadLedOnWarning);
            setCheckboxValue(WIDX_ON_MONEY,    cfg.gamepadLedOnMoney);
            setWidgetDisabled(WIDX_ON_CRITICAL, !live);
            setWidgetDisabled(WIDX_ON_WARNING,  !live);
            setWidgetDisabled(WIDX_ON_MONEY,    !live);
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            drawWidgets(rt);

            const auto& cfg = Config::Get().general;

            // Brightness value rendered inside the spinner display.
            // Mirrors Haptics.cpp's spinner layout pattern.
            const auto& spinner = widgets[WIDX_BRIGHTNESS_SPINNER];
            auto ft = Formatter();
            ft.Add<int32_t>(static_cast<int32_t>(std::lround(cfg.gamepadLedBrightness * 100.0f)));
            ScreenCoordsXY valuePos{
                windowPos.x + spinner.left + 1,
                windowPos.y + spinner.top + 1,
            };
            drawText(rt, valuePos, STR_LED_BRIGHTNESS_VALUE, ft, { colours[1] });

            // Status line: count LED-capable pads. Same per-pad
            // capability check that Haptics.cpp uses — a pad with no
            // lightbar (Xbox, most generic USB pads, the Mini's panel
            // button-board pre-Phase-2) reads as "no LED-capable
            // controller" rather than misleading "1 controller
            // detected".
            int32_t ledCount = 0;
            for (auto* gc : OpenRCT2::Ui::GetInputManager().getGameControllers())
            {
                if (gc != nullptr && SDL_GameControllerHasLED(gc))
                    ledCount++;
            }

            ScreenCoordsXY statusPos{
                windowPos.x + 10,
                windowPos.y + 153,
            };
            if (ledCount > 0)
            {
                Formatter sft;
                sft.Add<int32_t>(ledCount);
                drawText(rt, statusPos, STR_LED_CONTROLLERS_DETECTED, sft, { colours[1] });
            }
            else
            {
                drawText(rt, statusPos, STR_LED_NO_CONTROLLER, {}, { colours[1] });
            }
        }
    };

    WindowBase* LedOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::led);
        if (window == nullptr)
        {
            window = windowMgr->Create<LedWindow>(
                WindowClass::led, kLedWindowSize,
                WindowFlags{ WindowFlag::centreScreen });
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows
