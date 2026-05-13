/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI gamepad-plan 1.11: Haptics control window. Reachable
// from the Cheats / Debug dropdown beneath the Performance Profiler
// entry (gated on Config::general.debuggingTools, same as the
// profiler). Three controls plus a status line:
//
//   [x] Enable rumble                 ← maps to gamepadRumbleEnabled
//   Intensity:  [- | 100% | +]        ← maps to gamepadRumbleIntensity
//   [ Test rumble ]
//   1 controller(s) detected          ← live status
//
// Pressing Test rumble fires a short symmetric pulse so the user can
// verify the pad is wired up. Phase 1.11b layers a per-SoundId Rumble
// Editor on top in its own window class — this one stays as the
// global panel.

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
#include <openrct2/haptic/HapticEvent.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/ui/WindowManager.h>

namespace OpenRCT2::Ui::Windows
{
    // OPENRCT2MINI input-plan Track 2 §4.3: extended height (110 →
    // 175) to fit three per-event checkboxes between the Test button
    // and the status line. Status line shifts from y=88 to y=153.
    static constexpr ScreenSize kHapticsWindowSize = { 280, 175 };

    // ODR-unique tag — see LED.cpp for the rationale.
    enum HapticsWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_ENABLE_CHECKBOX,
        WIDX_INTENSITY_LABEL,
        // Spinner: value display + decrease + increase buttons.
        WIDX_INTENSITY_SPINNER,
        WIDX_INTENSITY_INCREASE,
        WIDX_INTENSITY_DECREASE,
        WIDX_TEST_BUTTON,
        // OPENRCT2MINI input-plan Track 2 §4.3: per-event toggles.
        WIDX_ON_CRASH,
        WIDX_ON_CRITICAL_NEWS,
        WIDX_ON_CONSTRUCTION_REFUSAL,
    };

    // clang-format off
    static const auto kHapticsWidgets = makeWidgets(
        makeWindowShim   (STR_HAPTICS, kHapticsWindowSize),
        makeWidget       ({  10, 25 }, { 200, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_HAPTICS_ENABLE                  ),
        makeWidget       ({  10, 45 }, { 100, 12 }, WidgetType::label,    WindowColour::secondary, STR_HAPTICS_INTENSITY               ),
        makeSpinnerWidgets({ 110, 43 }, {  90, 14 }, WidgetType::spinner,  WindowColour::secondary, kStringIdNone                       ),
        makeWidget       ({  10, 65 }, { 100, 14 }, WidgetType::button,   WindowColour::secondary, STR_HAPTICS_TEST_RUMBLE             ),
        makeWidget       ({  10, 88 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_HAPTICS_ON_CRASH                ),
        makeWidget       ({  10,103 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_HAPTICS_ON_CRITICAL_NEWS        ),
        makeWidget       ({  10,118 }, { 260, 12 }, WidgetType::checkbox, WindowColour::secondary, STR_HAPTICS_ON_CONSTRUCTION_REFUSAL )
    );
    // clang-format on

    class HapticsWindow final : public Window
    {
    public:
        void onOpen() override
        {
            setWidgets(kHapticsWidgets);
            WindowInitScrollWidgets(*this);
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
                    cfg.gamepadRumbleEnabled = !cfg.gamepadRumbleEnabled;
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_INTENSITY_INCREASE:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadRumbleIntensity = std::min(1.0f, cfg.gamepadRumbleIntensity + 0.05f);
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_INTENSITY_DECREASE:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadRumbleIntensity = std::max(0.0f, cfg.gamepadRumbleIntensity - 0.05f);
                    Config::Save();
                    invalidate();
                    return;
                }

                case WIDX_TEST_BUTTON:
                {
                    // OPENRCT2MINI gamepad-plan 1.11: 400 ms symmetric
                    // pulse at full motor magnitude. The global
                    // gamepadRumbleIntensity scaler is applied inside
                    // UiContext::RumbleControllers so the test honours
                    // the user's chosen intensity. If rumble is
                    // disabled or no rumble-capable pad is connected,
                    // the call is a silent no-op — the status line
                    // tells the user why nothing happened.
                    Haptic::pulse(1.0f, 400);
                    return;
                }

                // OPENRCT2MINI input-plan Track 2 §4.3: per-event
                // toggles. Crash + ConstructionRefusal gate inside
                // Haptic::onSoundPlayed (categorical SoundId match).
                // CriticalNews gates at the direct pulse call site in
                // NewsItem::TickCurrent.
                case WIDX_ON_CRASH:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadRumbleOnCrash = !cfg.gamepadRumbleOnCrash;
                    Config::Save();
                    invalidate();
                    return;
                }
                case WIDX_ON_CRITICAL_NEWS:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadRumbleOnCriticalNews = !cfg.gamepadRumbleOnCriticalNews;
                    Config::Save();
                    invalidate();
                    return;
                }
                case WIDX_ON_CONSTRUCTION_REFUSAL:
                {
                    auto& cfg = Config::Get().general;
                    cfg.gamepadRumbleOnConstructionRefusal = !cfg.gamepadRumbleOnConstructionRefusal;
                    Config::Save();
                    invalidate();
                    return;
                }
            }
        }

        void onPrepareDraw() override
        {
            const auto& cfg = Config::Get().general;
            setCheckboxValue(WIDX_ENABLE_CHECKBOX, cfg.gamepadRumbleEnabled);

            // Grey out the intensity controls and Test button when the
            // master switch is off so it's visually obvious nothing
            // will fire.
            const bool live = cfg.gamepadRumbleEnabled;
            widgets[WIDX_INTENSITY_LABEL].type = WidgetType::label;
            widgets[WIDX_INTENSITY_SPINNER].type = WidgetType::spinner;
            widgets[WIDX_INTENSITY_INCREASE].type = WidgetType::button;
            widgets[WIDX_INTENSITY_DECREASE].type = WidgetType::button;
            widgets[WIDX_TEST_BUTTON].type = WidgetType::button;

            setWidgetDisabled(WIDX_INTENSITY_SPINNER, !live);
            setWidgetDisabled(WIDX_INTENSITY_INCREASE, !live);
            setWidgetDisabled(WIDX_INTENSITY_DECREASE, !live);
            setWidgetDisabled(WIDX_TEST_BUTTON, !live);

            // OPENRCT2MINI input-plan Track 2 §4.3: per-event toggles.
            // Greyed out when the master rumble switch is off so it's
            // obvious that the sub-toggles can't override it.
            setCheckboxValue(WIDX_ON_CRASH, cfg.gamepadRumbleOnCrash);
            setCheckboxValue(WIDX_ON_CRITICAL_NEWS, cfg.gamepadRumbleOnCriticalNews);
            setCheckboxValue(WIDX_ON_CONSTRUCTION_REFUSAL, cfg.gamepadRumbleOnConstructionRefusal);
            setWidgetDisabled(WIDX_ON_CRASH, !live);
            setWidgetDisabled(WIDX_ON_CRITICAL_NEWS, !live);
            setWidgetDisabled(WIDX_ON_CONSTRUCTION_REFUSAL, !live);
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            drawWidgets(rt);

            const auto& cfg = Config::Get().general;

            // Intensity value rendered inside the spinner display.
            // SpinnerWidgets layout: label at [0], decrease button at
            // [1], increase button at [2] of the three-widget pack —
            // confirmed against Options.cpp's window-scale spinner.
            const auto& spinner = widgets[WIDX_INTENSITY_SPINNER];
            auto ft = Formatter();
            ft.Add<int32_t>(static_cast<int32_t>(std::lround(cfg.gamepadRumbleIntensity * 100.0f)));
            ScreenCoordsXY valuePos{
                windowPos.x + spinner.left + 1,
                windowPos.y + spinner.top + 1,
            };
            drawText(rt, valuePos, STR_HAPTICS_INTENSITY_VALUE, ft, { colours[1] });

            // Status line: count rumble-capable pads. We check
            // capability per-pad so plugging in a controller that SDL
            // can see but doesn't expose rumble (rare on host, normal
            // on Mini pre-2.6) reads as "no rumble-capable controller"
            // rather than misleading "1 controller detected".
            int32_t rumbleCount = 0;
            for (auto* gc : OpenRCT2::Ui::GetInputManager().getGameControllers())
            {
                if (gc != nullptr && SDL_GameControllerHasRumble(gc))
                    rumbleCount++;
            }

            ScreenCoordsXY statusPos{
                windowPos.x + 10,
                windowPos.y + 153,
            };
            if (rumbleCount > 0)
            {
                Formatter sft;
                sft.Add<int32_t>(rumbleCount);
                drawText(rt, statusPos, STR_HAPTICS_CONTROLLERS_DETECTED, sft, { colours[1] });
            }
            else
            {
                drawText(rt, statusPos, STR_HAPTICS_NO_CONTROLLER, {}, { colours[1] });
            }
        }
    };

    WindowBase* HapticsOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::haptics);
        if (window == nullptr)
        {
            window = windowMgr->Create<HapticsWindow>(
                WindowClass::haptics, kHapticsWindowSize,
                WindowFlags{ WindowFlag::centreScreen });
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows
