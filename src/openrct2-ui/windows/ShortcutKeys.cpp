/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Windows.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/ShortcutManager.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/G1Element.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/ui/WindowManager.h>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    WindowBase* ResetShortcutKeysPromptOpen();

    static constexpr StringId kWindowTitle = STR_SHORTCUTS_TITLE;
    // OPENRCT2MINI gamepad-plan 1.7a / mouse-column refactor: window
    // sized at 630 wide to fit three binding columns (Keyboard /
    // Gamepad / Mouse) at their fixed widths plus the action name.
    // 630×280 is now the MINIMUM size — the window is user-resizable
    // up to kMaximumWindowSize. When the user drags it wider, only
    // the action-name column flexes; the three binding columns stay
    // at their fixed pixel widths so chord strings don't suddenly
    // get more breathing room they don't need.
    static constexpr ScreenSize kWindowSize = { 630, 280 };
    static constexpr ScreenSize kMaximumWindowSize = { 1200, 800 };

    // OPENRCT2MINI mouse-input refactor: fixed pixel widths for the
    // three binding columns. Tuned to the original proportional
    // layout at 630px (~120px per column = ~20% of a 602px scroll
    // area). Keeping them fixed means dragging the window wider only
    // benefits the leftmost (action name) column where long shortcut
    // names actually need the room — the binding columns are
    // already sized for the longest typical chord strings.
    static constexpr int32_t kBindingColumnWidth = 120;
    static constexpr int32_t kColumnGap = 4;

    enum WindowShortcutWidgetIdx
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_TAB_CONTENT_PANEL,
        WIDX_SCROLL,
        WIDX_RESET,
        WIDX_HAPTICS,
        WIDX_CONTROLLER_LED,
        WIDX_TAB_0,
    };

    // OPENRCT2MINI input-bindings-rework §4.1: toolbar row layout.
    // Three buttons sized 150 / 90 / 110, gapped 4px each, anchored to
    // `height - 15`. Total 358px @ min window width 630 — slack to spare.
    static constexpr int32_t kToolbarButtonY = 15;        // distance from bottom to button top
    static constexpr int32_t kToolbarButtonH = 12;
    static constexpr int32_t kToolbarResetW = 170;        // wider than 150 to fit "Restore Defaults"
    static constexpr int32_t kToolbarHapticsW = 90;
    static constexpr int32_t kToolbarLedW = 110;
    static constexpr int32_t kToolbarGap = 4;

    // clang-format off
    static const auto _shortcutWidgets = makeWidgets(
        makeWindowShim(kWindowTitle, kWindowSize),
        // OPENRCT2MINI mouse-column refactor: resize panel widened to
        // match the 630-wide window. Scroll widget pushed down 12 px
        // (47→59) and shortened by 12 (215→203) to leave room for the
        // column header row drawn in onDraw between y=47 and y=58.
        makeWidget({0,                      43}, {kWindowSize.width - 70, 287}, WidgetType::resize, WindowColour::secondary                                       ),
        makeWidget({4,                      59}, {kWindowSize.width - 8,  203}, WidgetType::scroll, WindowColour::secondary, SCROLL_VERTICAL,           STR_SHORTCUT_LIST_TIP        ),
        // Toolbar row — Restore Defaults / Haptics / Controller LED.
        // onPrepareDraw reanchors these to `height - 15` and rewrites
        // their right edges so they grow with the window. Initial x/y
        // here are placeholder values for the static widget array; the
        // runtime layout is the source of truth.
        makeWidget({4,                                                                  kWindowSize.height - kToolbarButtonY}, {kToolbarResetW,   kToolbarButtonH}, WidgetType::button, WindowColour::secondary, STR_SHORTCUT_ACTION_RESET, STR_SHORTCUT_ACTION_RESET_TIP),
        makeWidget({4 + kToolbarResetW + kToolbarGap,                                   kWindowSize.height - kToolbarButtonY}, {kToolbarHapticsW, kToolbarButtonH}, WidgetType::button, WindowColour::secondary, STR_HAPTICS,               STR_HAPTICS                  ),
        makeWidget({4 + kToolbarResetW + kToolbarGap + kToolbarHapticsW + kToolbarGap,  kWindowSize.height - kToolbarButtonY}, {kToolbarLedW,     kToolbarButtonH}, WidgetType::button, WindowColour::secondary, STR_LED,                   STR_LED                      )
    );
    // clang-format on

    static constexpr StringId kWindowTitleChange = STR_SHORTCUT_CHANGE_TITLE;
    // OPENRCT2MINI input-bindings-rework §6.4: capture window stripped
    // to a passive countdown. No buttons, no checkbox — only the
    // background, title, and close-box widgets from makeWindowShim.
    // Height 90 = title 14 + prompt y=16 (2 wrapped lines ~28) + preview
    // line 14 + countdown line 14 + bottom margin 4.
    static constexpr ScreenSize kWindowSizeChange = { 250, 90 };

    // OPENRCT2MINI input-bindings-rework §6.2: auto-close timer. The
    // window closes itself 5 seconds after onOpen without committing
    // a binding. Cancellation parity with the previous ESC-only path.
    static constexpr uint32_t kCaptureAutoCloseMs = 5000;

    // clang-format off
    static const auto window_shortcut_change_widgets = makeWidgets(
        makeWindowShim(kWindowTitleChange, kWindowSizeChange)
    );
    // clang-format on

    class ChangeShortcutWindow final : public Window
    {
    private:
        std::string _shortcutId;
        StringId _shortcutLocalisedName{};
        std::string _shortcutCustomName;
        // OPENRCT2MINI gamepad-plan 1.7b: which kind the active capture
        // session is filtering for. Stored on the modal so onPrepareDraw
        // can render the right "press a key" / "press a button" prompt
        // (1.7d will use this for the prompt-text variation).
        PendingShortcutKind _kind = PendingShortcutKind::any;
        // OPENRCT2MINI gamepad-plan 1.7d: token from InputManager::
        // pushModalHooks. ESC and PAD BACK route through the hook to
        // close the modal without committing.
        OpenRCT2::Ui::InputManager::ModalHooksToken _modalHooksToken{};
        // OPENRCT2MINI input-bindings-rework §6.2: SDL tick at onOpen.
        // onUpdate auto-closes the window after kCaptureAutoCloseMs
        // elapses without a binding committing. 0 means "not yet
        // armed" — set in onOpen.
        uint32_t _openedAtMs = 0;

    public:
        // OPENRCT2MINI input-bindings-rework §3: append is now the
        // only mode. The `kind` parameter still routes column-keyed
        // capture from the Input Bindings list; `mode` is preserved
        // in the signature for ABI consistency but is ignored — all
        // captures append to existing bindings. Use the per-cell bin
        // button to clear a column.
        static ChangeShortcutWindow* Open(
            std::string_view shortcutId, PendingShortcutKind kind = PendingShortcutKind::any,
            [[maybe_unused]] PendingShortcutMode mode = PendingShortcutMode::append)
        {
            auto& shortcutManager = GetShortcutManager();
            auto registeredShortcut = shortcutManager.getShortcut(shortcutId);
            if (registeredShortcut != nullptr)
            {
                auto* windowMgr = GetWindowManager();
                windowMgr->CloseByClass(WindowClass::changeKeyboardShortcut);
                auto* w = windowMgr->Create<ChangeShortcutWindow>(
                    WindowClass::changeKeyboardShortcut, kWindowSizeChange, WindowFlag::centreScreen);
                if (w != nullptr)
                {
                    w->_shortcutId = shortcutId;
                    w->_shortcutLocalisedName = registeredShortcut->localisedName;
                    w->_shortcutCustomName = registeredShortcut->customName;
                    w->_kind = kind;
                    // Always append — the rework removed the
                    // replace path entirely.
                    shortcutManager.setPendingShortcutChange(
                        registeredShortcut->id, kind, PendingShortcutMode::append);
                    return w;
                }
            }
            return nullptr;
        }

        void onOpen() override
        {
            setWidgets(window_shortcut_change_widgets);
            WindowInitScrollWidgets(*this);

            // OPENRCT2MINI input-bindings-rework §6.2: arm the auto-
            // close timer. onUpdate compares to SDL_GetTicks each frame.
            _openedAtMs = SDL_GetTicks();
            // OPENRCT2MINI input-bindings-rework §6.3: pause the game
            // via GAME_PAUSED_MODAL — same mechanism the OSK uses. In-
            // game simulation halts; title-sequence animation keeps
            // running (TitleScene::Tick checks GAME_PAUSED_NORMAL only).
            gGamePaused |= GAME_PAUSED_MODAL;

            // OPENRCT2MINI gamepad-plan 1.7d: install ModalHooks so
            // ESC and PAD BACK close the modal without committing.
            // Confirm hook intentionally not installed — RETURN /
            // PAD START during a capture session is a binding event,
            // not a confirm gesture (the user might be trying to
            // bind RETURN itself).
            _modalHooksToken = OpenRCT2::Ui::GetInputManager().pushModalHooks({
                /*dismiss=*/ [this](const OpenRCT2::Ui::InputEvent&) {
                    close();
                    return true;
                },
                /*confirm=*/ {},
            });
        }

        void onClose() override
        {
            // OPENRCT2MINI gamepad-plan 1.7d: pop the modal hooks
            // before clearing the pending state so ESC/PAD BACK
            // can no longer fire the cancel callback after the
            // window is gone.
            OpenRCT2::Ui::GetInputManager().popModalHooks(_modalHooksToken);
            // OPENRCT2MINI input-bindings-rework §6.3: drop the
            // modal pause bit. Mirrors Osk.cpp's onClose.
            gGamePaused &= ~GAME_PAUSED_MODAL;

            auto& shortcutManager = GetShortcutManager();
            shortcutManager.setPendingShortcutChange({});
            NotifyShortcutKeysWindow();
        }

        void onUpdate() override
        {
            // OPENRCT2MINI input-bindings-rework §6.2: auto-close
            // after kCaptureAutoCloseMs. The capture machine commits
            // independently when a binding is captured — if commit
            // already happened the window was closed by ShortcutManager
            // and we won't reach here. SDL_GetTicks wraps after ~49 days;
            // using subtraction sidesteps the wrap.
            if (_openedAtMs != 0
                && static_cast<uint32_t>(SDL_GetTicks() - _openedAtMs) >= kCaptureAutoCloseMs)
            {
                close();
                return;
            }
            // OPENRCT2MINI gamepad-plan 1.7c: invalidate every frame so
            // the captured-chord preview and countdown text below the
            // prompt redraw as the user adds buttons / waits for the
            // 5-second auto-commit.
            invalidate();
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            // OPENRCT2MINI input-bindings-rework §6.1: only the close-
            // box remains. The Remove + Append-checkbox widgets are
            // gone; the per-cell bin button in the Input Bindings list
            // is now the way to clear a binding (§2).
            if (widgetIndex == WIDX_CLOSE)
                close();
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);

            ScreenCoordsXY stringCoords(windowPos.x + 125, windowPos.y + widgets[WIDX_TITLE].bottom + 16);

            auto ft = Formatter();
            if (_shortcutCustomName.empty())
            {
                ft.Add<StringId>(_shortcutLocalisedName);
            }
            else
            {
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(_shortcutCustomName.c_str());
            }
            drawTextWrapped(rt, stringCoords, 242, STR_SHORTCUT_CHANGE_PROMPT, ft, { TextAlignment::centre });

            // OPENRCT2MINI gamepad-plan 1.7c: live capture feedback.
            // Below the existing prompt we render two extra lines for
            // gamepad capture sessions:
            //   line 1 — the chord captured so far ("PAD L1+R1")
            //   line 2 — countdown text when the 5s auto-commit timer
            //            is armed ("Locking in 4s...")
            // Without this, the user has no feedback that pressing L1
            // was registered, or that holding L1+R1 will commit at
            // the timer's expiry.
            // OPENRCT2MINI hold-binding capture: countdown / preview
            // text now renders for keyboard / mouse sessions too,
            // not just gamepad. The 5-second hold-elapsed path
            // commits the binding as a HOLD binding regardless of
            // input device, so the user gets the same visual
            // feedback ("Locking in 4s...") on every kind.
            {
                auto& shortcutManager = GetShortcutManager();
                const auto preview = shortcutManager.getCaptureMaxChordPreview();
                if (!preview.empty())
                {
                    // OPENRCT2MINI gamepad-plan 1.7c: STR_SHORTCUT_-
                    // CAPTURED_CHORD wraps the chord string with
                    // {WINDOW_COLOUR_2} so the text inherits the
                    // modal's colour scheme — matches the styling of
                    // the prompt above (STR_SHORTCUT_CHANGE_PROMPT,
                    // which also starts with that token).
                    auto ft2 = Formatter();
                    ft2.Add<const char*>(preview.c_str());
                    ScreenCoordsXY previewCoords(
                        windowPos.x + 125,
                        windowPos.y + widgets[WIDX_TITLE].bottom + 32);
                    drawTextWrapped(
                        rt, previewCoords, 242, STR_SHORTCUT_CAPTURED_CHORD, ft2,
                        { TextAlignment::centre });
                }

                if (shortcutManager.isCaptureChordTimerArmed())
                {
                    const uint32_t msRemaining = shortcutManager.getCaptureCountdownMsRemaining(SDL_GetTicks());
                    char countdown[32];
                    std::snprintf(
                        countdown, sizeof(countdown), "Hold %us for HOLD binding...",
                        static_cast<unsigned>((msRemaining + 999) / 1000));
                    auto ft3 = Formatter();
                    ft3.Add<const char*>(countdown);
                    ScreenCoordsXY countdownCoords(
                        windowPos.x + 125,
                        windowPos.y + widgets[WIDX_TITLE].bottom + 46);
                    drawTextWrapped(
                        rt, countdownCoords, 242, STR_SHORTCUT_LOCKING_IN_COUNTDOWN, ft3,
                        { TextAlignment::centre });
                }
                else if (_openedAtMs != 0)
                {
                    // OPENRCT2MINI input-bindings-rework §6.2: auto-
                    // close countdown when no hold-binding chord is
                    // armed. Hold-binding takes priority because it's
                    // the more relevant cue while a chord is in
                    // progress; once that timer releases (or the user
                    // hasn't started a chord at all) this line takes
                    // over so the user knows the modal will dismiss.
                    const uint32_t elapsed = SDL_GetTicks() - _openedAtMs;
                    const uint32_t remaining = (elapsed >= kCaptureAutoCloseMs)
                        ? 0
                        : (kCaptureAutoCloseMs - elapsed);
                    char autoClose[32];
                    std::snprintf(
                        autoClose, sizeof(autoClose), "Closing in %us...",
                        static_cast<unsigned>((remaining + 999) / 1000));
                    auto ft4 = Formatter();
                    ft4.Add<const char*>(autoClose);
                    ScreenCoordsXY autoCloseCoords(
                        windowPos.x + 125,
                        windowPos.y + widgets[WIDX_TITLE].bottom + 46);
                    drawTextWrapped(
                        rt, autoCloseCoords, 242, STR_SHORTCUT_LOCKING_IN_COUNTDOWN, ft4,
                        { TextAlignment::centre });
                }
            }

            // OPENRCT2MINI overlap-warn refactor: the conflict-
            // rejection message rendering is gone. Duplicate
            // bindings are now allowed at commit time; the
            // overlapping bindings get rendered yellow in the
            // Input Bindings list as the soft warning.
        }

    private:
        void NotifyShortcutKeysWindow();
    };

    class ShortcutKeysWindow final : public Window
    {
    private:
        struct ShortcutStringPair
        {
            std::string ShortcutId;
            ::StringId StringId = kStringIdNone;
            std::string CustomString;
            // OPENRCT2MINI gamepad-plan 1.7a / mouse-column refactor:
            // split single Binding into THREE per-kind columns.
            //   KeyboardBinding: kind == keyboard.
            //   GamepadBinding:  kind == joyButton / joyHat / joyAxis.
            //   MouseBinding:    kind == mouse — used to be lumped in
            //                    with KeyboardBinding, but now that the
            //                    rebind UI distinguishes mouse as its
            //                    own capture column it gets its own
            //                    field too.
            // Any of the three may be empty when the shortcut has no
            // binding of that kind.
            std::string KeyboardBinding;
            std::string GamepadBinding;
            std::string MouseBinding;
            // OPENRCT2MINI overlap-warn: per-column flag set by
            // initialiseList when ANY current binding of that kind
            // duplicates a binding registered against another shortcut
            // ID (compared via ShortcutManager::isBindingOverlapping,
            // which uses fire-time identity — kind, code, modifiers,
            // and holdMs). Drives a yellow {YELLOW}{STRINGID} colour
            // wrapper in DrawItem instead of the default black, as a
            // soft "warning" that this column will fight another
            // shortcut for the same input. Allowed at commit time —
            // the rebind UI no longer rejects overlapping captures —
            // so this is purely a visual cue.
            bool KeyboardOverlap = false;
            bool GamepadOverlap = false;
            bool MouseOverlap = false;
        };

        struct ShortcutTabDesc
        {
            std::string_view IdGroup;
            uint32_t ImageId;
            uint32_t ImageDivisor;
            uint32_t ImageNumFrames;
        };

        // OPENRCT2MINI input-bindings-rework §1.2: per-cell hover. The
        // row was the highlight unit upstream; we now track an explicit
        // cell within the row so only that cell's background tints. The
        // label column is non-interactive; the bin cells exist only
        // when the adjacent binding is non-empty.
        enum class Cell : uint8_t
        {
            none = 0, // not hovering / hovering an empty bin slot
            label,
            binKbd,
            keyboard,
            binPad,
            gamepad,
            binMouse,
            mouse,
        };

        std::vector<ShortcutTabDesc> _tabs;
        std::vector<Widget> _widgets;
        std::vector<ShortcutStringPair> _list;
        int_fast16_t _highlightedRow{ -1 };
        Cell _highlightedCell{ Cell::none };
        size_t _currentTabIndex{};
        uint32_t _tabAnimationIndex{};

    public:
        void onOpen() override
        {
            initialiseTabs();
            initialiseWidgets();
            initialiseList();
            // OPENRCT2MINI mouse-input refactor: follow the RideList
            // pattern — set min / max once on open. Min is the
            // shipped baseline so the three binding columns and the
            // action column always have enough room; max gives users
            // ample horizontal space for long shortcut / chord names.
            WindowSetResize(*this, kWindowSize, kMaximumWindowSize);
        }

        void onClose() override
        {
            auto* windowMgr = GetWindowManager();
            windowMgr->CloseByClass(WindowClass::resetShortcutKeysPrompt);
        }

        void onResize() override
        {
            // Lightweight clamp — min / max already set in onOpen.
            // Mirrors RideListWindow::onResize.
            if (width < minWidth)
            {
                invalidate();
                width = minWidth;
            }
            if (height < minHeight)
            {
                invalidate();
                height = minHeight;
            }
        }

        void onUpdate() override
        {
            // Remove highlight when the mouse is not hovering over the list
            if ((_highlightedRow != -1 || _highlightedCell != Cell::none)
                && !widgetIsHighlighted(*this, WIDX_SCROLL))
            {
                _highlightedRow = -1;
                _highlightedCell = Cell::none;
                invalidateWidget(WIDX_SCROLL);
            }

            _tabAnimationIndex++;
            invalidateWidget(static_cast<WidgetIndex>(WIDX_TAB_0 + _currentTabIndex));
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    break;
                case WIDX_RESET:
                    ResetShortcutKeysPromptOpen();
                    break;
                case WIDX_HAPTICS:
                    ContextOpenWindow(WindowClass::haptics);
                    break;
                case WIDX_CONTROLLER_LED:
                    ContextOpenWindow(WindowClass::led);
                    break;
                default:
                {
                    auto tabIndex = static_cast<size_t>(widgetIndex - WIDX_TAB_0);
                    if (tabIndex < _tabs.size())
                    {
                        SetTab(tabIndex);
                    }
                }
            }
        }

        void onPrepareDraw() override
        {
            widgets[WIDX_SCROLL].right = width - 5;
            widgets[WIDX_SCROLL].bottom = height - 19;
            // OPENRCT2MINI input-bindings-rework §4.1: three-button
            // toolbar row anchored to height - 15. Buttons keep their
            // fixed widths; only the leftmost (Restore Defaults) shifts
            // around as the window resizes (it starts at x=4 anyway).
            const int32_t toolbarTop = height - kToolbarButtonY - 1;
            const int32_t toolbarBottom = height - 5;
            widgets[WIDX_RESET].top = toolbarTop;
            widgets[WIDX_RESET].bottom = toolbarBottom;
            widgets[WIDX_RESET].left = 4;
            widgets[WIDX_RESET].right = 4 + kToolbarResetW - 1;
            widgets[WIDX_HAPTICS].top = toolbarTop;
            widgets[WIDX_HAPTICS].bottom = toolbarBottom;
            widgets[WIDX_HAPTICS].left = widgets[WIDX_RESET].right + 1 + kToolbarGap;
            widgets[WIDX_HAPTICS].right = widgets[WIDX_HAPTICS].left + kToolbarHapticsW - 1;
            widgets[WIDX_CONTROLLER_LED].top = toolbarTop;
            widgets[WIDX_CONTROLLER_LED].bottom = toolbarBottom;
            widgets[WIDX_CONTROLLER_LED].left = widgets[WIDX_HAPTICS].right + 1 + kToolbarGap;
            widgets[WIDX_CONTROLLER_LED].right = widgets[WIDX_CONTROLLER_LED].left + kToolbarLedW - 1;
            WindowAlignTabs(this, WIDX_TAB_0, static_cast<WidgetIndex>(WIDX_TAB_0 + _tabs.size() - 1));

            // Set selected tab
            for (size_t i = 0; i < _tabs.size(); i++)
            {
                setWidgetPressed(static_cast<WidgetIndex>(WIDX_TAB_0 + i), false);
            }
            setWidgetPressed(static_cast<WidgetIndex>(WIDX_TAB_0 + _currentTabIndex), true);
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);
            DrawTabImages(rt);
            DrawColumnHeaders(rt);
        }

        // OPENRCT2MINI mouse-column refactor: column header row drawn
        // above the scroll widget. Tells the user which column is
        // which — without this, three side-by-side binding columns
        // are indistinguishable. Header order matches DrawItem and
        // computeColumnLayout: Keyboard | Gamepad | Mouse.
        //
        // scrollWidth uses the SAME formula as onScrollDraw below
        // (`width - kScrollBarWidth - 10`) so the header offsets line
        // up exactly with the data rows rendered inside the scroll
        // area; without this, dragging the window wider would
        // gradually drift the headers off their columns.
        void DrawColumnHeaders(RenderTarget& rt) const
        {
            const auto& scrollWidget = widgets[WIDX_SCROLL];
            const auto scrollWidth = width - kScrollBarWidth - 10;
            const auto cols = computeColumnLayout(scrollWidth);
            const int32_t headerY = windowPos.y + scrollWidget.top - 12;
            const int32_t scrollX = windowPos.x + scrollWidget.left;

            // Each header is left-aligned into its column. The action-
            // name column has no header (it's just shortcut names).
            auto draw = [&](int32_t offset, int32_t colWidth, StringId stringId) {
                auto ft = Formatter();
                ft.Add<StringId>(stringId);
                drawTextEllipsised(
                    rt, { scrollX + offset, headerY }, colWidth, STR_BLACK_STRING, ft);
            };
            draw(cols.keyboardOffset, cols.keyboardWidth, STR_SHORTCUT_COL_KEYBOARD);
            draw(cols.gamepadOffset, cols.gamepadWidth, STR_SHORTCUT_COL_GAMEPAD);
            draw(cols.mouseOffset, cols.mouseWidth, STR_SHORTCUT_COL_MOUSE);
        }

        ScreenSize onScrollGetSize(int32_t scrollIndex) override
        {
            auto h = static_cast<int32_t>(_list.size() * kScrollableRowHeight);
            auto bottom = std::max(0, h - widgets[WIDX_SCROLL].bottom + widgets[WIDX_SCROLL].top + 21);
            if (bottom < scrolls[0].contentOffsetY)
            {
                scrolls[0].contentOffsetY = bottom;
                invalidate();
            }
            return { 0, h };
        }

        // OPENRCT2MINI input-bindings-rework §1.2: resolve cursor x to
        // which cell it falls on for a given row (which dictates
        // whether bin cells exist based on the row's bindings). Returns
        // Cell::none if the row is a separator or out of range.
        Cell resolveCellAt(const ShortcutStringPair& shortcut, int32_t x) const
        {
            const auto scrollWidth = width - kScrollBarWidth - 10;
            const auto cols = computeColumnLayout(scrollWidth);
            const bool kHasKbd = !shortcut.KeyboardBinding.empty();
            const bool kHasPad = !shortcut.GamepadBinding.empty();
            const bool kHasMouse = !shortcut.MouseBinding.empty();
            // Hit-test right-to-left so the bin cell takes priority over
            // its adjacent binding column's recovered area.
            if (x >= cols.mouseOffset)
                return Cell::mouse;
            if (kHasMouse && x >= cols.binMouseOffset)
                return Cell::binMouse;
            if (x >= cols.gamepadOffset)
                return Cell::gamepad;
            if (kHasPad && x >= cols.binGamepadOffset)
                return Cell::binPad;
            if (x >= cols.keyboardOffset)
                return Cell::keyboard;
            if (kHasKbd && x >= cols.binKeyboardOffset)
                return Cell::binKbd;
            return Cell::label;
        }

        void onScrollMouseOver(int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto index = static_cast<int_fast16_t>((screenCoords.y - 1) / kScrollableRowHeight);
            if (static_cast<size_t>(index) < _list.size())
            {
                const auto& shortcut = _list[index];
                // Separators have empty IDs — never highlight.
                if (shortcut.ShortcutId.empty())
                {
                    if (_highlightedRow != -1 || _highlightedCell != Cell::none)
                    {
                        _highlightedRow = -1;
                        _highlightedCell = Cell::none;
                        invalidate();
                    }
                    return;
                }
                const Cell cell = resolveCellAt(shortcut, screenCoords.x);
                if (_highlightedRow != index || _highlightedCell != cell)
                {
                    _highlightedRow = index;
                    _highlightedCell = cell;
                    invalidate();
                }
            }
            else
            {
                if (_highlightedRow != -1 || _highlightedCell != Cell::none)
                {
                    _highlightedRow = -1;
                    _highlightedCell = Cell::none;
                    invalidate();
                }
            }
        }

        void onScrollMouseDown(int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto selectedItem = static_cast<size_t>((screenCoords.y - 1) / kScrollableRowHeight);
            if (selectedItem >= _list.size())
                return;
            auto& shortcut = _list[selectedItem];
            if (shortcut.ShortcutId.empty())
                return; // separator

            const Cell cell = resolveCellAt(shortcut, screenCoords.x);
            auto& shortcutManager = GetShortcutManager();
            switch (cell)
            {
                case Cell::keyboard:
                    ChangeShortcutWindow::Open(shortcut.ShortcutId, PendingShortcutKind::keyboard);
                    break;
                case Cell::gamepad:
                    ChangeShortcutWindow::Open(shortcut.ShortcutId, PendingShortcutKind::gamepad);
                    break;
                case Cell::mouse:
                    ChangeShortcutWindow::Open(shortcut.ShortcutId, PendingShortcutKind::mouse);
                    break;
                case Cell::binKbd:
                    shortcutManager.clearBindingsOfKind(shortcut.ShortcutId, PendingShortcutKind::keyboard);
                    RefreshBindings();
                    break;
                case Cell::binPad:
                    shortcutManager.clearBindingsOfKind(shortcut.ShortcutId, PendingShortcutKind::gamepad);
                    RefreshBindings();
                    break;
                case Cell::binMouse:
                    shortcutManager.clearBindingsOfKind(shortcut.ShortcutId, PendingShortcutKind::mouse);
                    RefreshBindings();
                    break;
                case Cell::label:
                case Cell::none:
                    // Label is decorative — ignore.
                    break;
            }
        }

        // OPENRCT2MINI list-focus-plan §3.5: per-cell focus opt-in.
        // 7 slots per row: [label, binKbd, keyboard, binPad, gamepad,
        // binMouse, mouse] — matches Cell enum order skipping `none`.
        // itemIndex = row × 7 + slot. Slots return empty rect when:
        //   - the row is a separator (ShortcutId.empty())
        //   - the slot is `label` (non-interactive)
        //   - the slot is a bin cell whose adjacent binding is empty
        // The framework's directional dispatcher skips empty-rect
        // items automatically, so navigation flows naturally past
        // collapsed bin cells.
        static constexpr int32_t kFocusSlotsPerRow = 7;
        enum FocusSlot : int32_t
        {
            kFocusSlotLabel = 0,
            kFocusSlotBinKbd = 1,
            kFocusSlotKeyboard = 2,
            kFocusSlotBinPad = 3,
            kFocusSlotGamepad = 4,
            kFocusSlotBinMouse = 5,
            kFocusSlotMouse = 6,
        };

        int32_t scrollFocusGetItemCount(int32_t scrollIndex) override
        {
            return static_cast<int32_t>(_list.size()) * kFocusSlotsPerRow;
        }

        int32_t scrollFocusGetColumnCount(int32_t scrollIndex) override
        {
            return kFocusSlotsPerRow;
        }

        ScreenRect scrollFocusGetItemRect(int32_t scrollIndex, int32_t itemIndex) override
        {
            if (itemIndex < 0)
                return {};
            const auto row = itemIndex / kFocusSlotsPerRow;
            const auto slot = itemIndex % kFocusSlotsPerRow;
            if (row < 0 || static_cast<size_t>(row) >= _list.size())
                return {};
            const auto& shortcut = _list[row];
            if (shortcut.ShortcutId.empty())
                return {}; // separator — never focusable
            // Label is decorative — clicking it does nothing per
            // onScrollMouseDown — so exclude it from focus stops.
            if (slot == kFocusSlotLabel)
                return {};
            // Bin cells collapse when their adjacent binding is empty.
            if (slot == kFocusSlotBinKbd && shortcut.KeyboardBinding.empty())
                return {};
            if (slot == kFocusSlotBinPad && shortcut.GamepadBinding.empty())
                return {};
            if (slot == kFocusSlotBinMouse && shortcut.MouseBinding.empty())
                return {};
            const auto scrollWidth = width - kScrollBarWidth - 10;
            const auto cols = computeColumnLayout(scrollWidth);
            const int32_t y = 1 + row * kScrollableRowHeight;
            int32_t x0 = 0;
            int32_t x1 = 0;
            switch (slot)
            {
                case kFocusSlotBinKbd:
                    x0 = cols.binKeyboardOffset;
                    x1 = cols.keyboardOffset;
                    break;
                case kFocusSlotKeyboard:
                    x0 = cols.keyboardOffset;
                    x1 = cols.binGamepadOffset;
                    break;
                case kFocusSlotBinPad:
                    x0 = cols.binGamepadOffset;
                    x1 = cols.gamepadOffset;
                    break;
                case kFocusSlotGamepad:
                    x0 = cols.gamepadOffset;
                    x1 = cols.binMouseOffset;
                    break;
                case kFocusSlotBinMouse:
                    x0 = cols.binMouseOffset;
                    x1 = cols.mouseOffset;
                    break;
                case kFocusSlotMouse:
                    x0 = cols.mouseOffset;
                    x1 = cols.mouseOffset + cols.mouseWidth;
                    break;
                default:
                    return {};
            }
            if (x0 >= x1)
                return {};
            return ScreenRect{
                { x0, y },
                { x1 - 1, y + kScrollableRowHeight - 1 },
            };
        }

        void onScrollDraw(int32_t scrollIndex, RenderTarget& rt) override
        {
            auto rtCoords = ScreenCoordsXY{ rt.x, rt.y };
            Rectangle::fill(
                rt, { rtCoords, rtCoords + ScreenCoordsXY{ rt.width - 1, rt.height - 1 } },
                getColourMap(colours[1].colour).midLight);

            // TODO: the line below is a workaround for what is presumably a bug with rt->width
            //       see https://github.com/OpenRCT2/OpenRCT2/issues/11238 for details
            const auto scrollWidth = width - kScrollBarWidth - 10;

            for (size_t i = 0; i < _list.size(); ++i)
            {
                auto y = static_cast<int32_t>(1 + i * kScrollableRowHeight);
                if (y > rt.y + rt.height)
                {
                    break;
                }

                if (y + kScrollableRowHeight < rt.y)
                {
                    continue;
                }

                // Is this a separator?
                if (_list[i].ShortcutId.empty())
                {
                    DrawSeparator(rt, y, scrollWidth);
                }
                else
                {
                    DrawItem(rt, y, scrollWidth, _list[i], i);
                }
            }
        }

        void onLanguageChange() override
        {
            initialiseList();
        }

        void RefreshBindings()
        {
            initialiseList();
        }

        void ResetAllOnActiveTab()
        {
            auto& shortcutManager = GetShortcutManager();
            for (const auto& item : _list)
            {
                auto shortcut = shortcutManager.getShortcut(item.ShortcutId);
                if (shortcut != nullptr)
                {
                    shortcut->current = shortcut->standard;
                }
            }
            shortcutManager.saveUserBindings();
            RefreshBindings();
        }

    private:
        bool IsInCurrentTab(const RegisteredShortcut& shortcut)
        {
            auto groupFilter = _tabs[_currentTabIndex].IdGroup;
            auto group = shortcut.getTopLevelGroup();
            if (groupFilter.empty())
            {
                // Check it doesn't belong in any other tab
                for (const auto& tab : _tabs)
                {
                    if (!tab.IdGroup.empty())
                    {
                        if (tab.IdGroup == group)
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

            return group == groupFilter;
        }

        void initialiseList()
        {
            // Get shortcuts and sort by group
            auto shortcuts = GetShortcutsForCurrentTab();
            std::stable_sort(shortcuts.begin(), shortcuts.end(), [](const RegisteredShortcut* a, const RegisteredShortcut* b) {
                return a->orderIndex < b->orderIndex;
            });

            // OPENRCT2MINI overlap-warn: compute per-column overlap
            // flags by asking the ShortcutManager whether any binding
            // of a given kind on this shortcut collides with a binding
            // on a DIFFERENT shortcut. Done once in initialiseList so
            // DrawItem stays cheap.
            auto& shortcutManager = GetShortcutManager();

            // Create list items with a separator between each group
            _list.clear();
            std::string group;
            for (const auto* shortcut : shortcuts)
            {
                if (group.empty())
                {
                    group = shortcut->getGroup();
                }
                else
                {
                    auto groupName = shortcut->getGroup();
                    if (group != groupName)
                    {
                        // Add separator
                        group = groupName;
                        _list.emplace_back();
                    }
                }

                ShortcutStringPair ssp;
                ssp.ShortcutId = shortcut->id;
                ssp.StringId = shortcut->localisedName;
                ssp.CustomString = shortcut->customName;
                // OPENRCT2MINI gamepad-plan 1.7a / mouse-column refactor:
                // cache per-kind display strings so DrawItem can render
                // three binding columns without re-walking
                // shortcut->current each frame.
                ssp.KeyboardBinding = shortcut->getKeyboardDisplayString();
                ssp.GamepadBinding = shortcut->getGamepadDisplayString();
                ssp.MouseBinding = shortcut->getMouseDisplayString();

                // OPENRCT2MINI overlap-warn: walk shortcut->current and
                // bucket each binding into the keyboard / gamepad /
                // mouse column, asking the manager if it collides with
                // any binding registered on another shortcut. As soon
                // as we hit one collision in a given column we set its
                // flag and stop bucketing that column further (still
                // need to scan for the others though).
                for (const auto& binding : shortcut->current)
                {
                    if (binding.kind == InputDeviceKind::keyboard)
                    {
                        if (!ssp.KeyboardOverlap
                            && shortcutManager.isBindingOverlapping(shortcut->id, binding))
                        {
                            ssp.KeyboardOverlap = true;
                        }
                    }
                    else if (binding.kind == InputDeviceKind::mouse)
                    {
                        if (!ssp.MouseOverlap
                            && shortcutManager.isBindingOverlapping(shortcut->id, binding))
                        {
                            ssp.MouseOverlap = true;
                        }
                    }
                    else if (
                        binding.kind == InputDeviceKind::joyButton
                        || binding.kind == InputDeviceKind::joyHat
                        || binding.kind == InputDeviceKind::joyAxis)
                    {
                        if (!ssp.GamepadOverlap
                            && shortcutManager.isBindingOverlapping(shortcut->id, binding))
                        {
                            ssp.GamepadOverlap = true;
                        }
                    }
                }
                _list.push_back(std::move(ssp));
            }

            invalidate();
        }

        std::vector<const RegisteredShortcut*> GetShortcutsForCurrentTab()
        {
            std::vector<const RegisteredShortcut*> result;
            auto& shortcutManager = GetShortcutManager();
            for (const auto& shortcut : shortcutManager.shortcuts)
            {
                if (IsInCurrentTab(shortcut.second))
                {
                    result.push_back(&shortcut.second);
                }
            }
            return result;
        }

        void initialiseTabs()
        {
            _tabs.clear();
            _tabs.push_back({ "interface", SPR_TAB_GEARS_0, 2, 4 });
            _tabs.push_back({ "view", SPR_G2_VIEW, 0, 0 });
            _tabs.push_back({ "window", SPR_TAB_PARK_ENTRANCE, 0, 0 });
            _tabs.push_back({ {}, SPR_TAB_WRENCH_0, 2, 16 });
        }

        void initialiseWidgets()
        {
            widgets.clear();
            widgets.insert(widgets.begin(), std::begin(_shortcutWidgets), std::end(_shortcutWidgets));

            int32_t x = 3;
            for (size_t i = 0; i < _tabs.size(); i++)
            {
                auto tab = makeTab({ x, 17 }, kStringIdNone);
                widgets.push_back(tab);
                x += 31;
            }

            WindowInitScrollWidgets(*this);
            resizeFrame();
        }

        void SetTab(size_t index)
        {
            if (_currentTabIndex != index)
            {
                _currentTabIndex = index;
                _tabAnimationIndex = 0;
                initialiseList();
            }
        }

        void DrawTabImages(RenderTarget& rt) const
        {
            for (size_t i = 0; i < _tabs.size(); i++)
            {
                DrawTabImage(rt, i);
            }
        }

        void DrawTabImage(RenderTarget& rt, size_t tabIndex) const
        {
            const auto& tabDesc = _tabs[tabIndex];
            auto widgetIndex = static_cast<WidgetIndex>(WIDX_TAB_0 + tabIndex);
            if (!isWidgetDisabled(widgetIndex))
            {
                auto imageId = tabDesc.ImageId;
                if (imageId != 0)
                {
                    if (tabIndex == _currentTabIndex && tabDesc.ImageDivisor != 0 && tabDesc.ImageNumFrames != 0)
                    {
                        auto frame = _tabAnimationIndex / tabDesc.ImageDivisor;
                        imageId += frame % tabDesc.ImageNumFrames;
                    }

                    const auto& widget = widgets[widgetIndex];
                    GfxDrawSprite(rt, ImageId(imageId), windowPos + ScreenCoordsXY{ widget.left, widget.top });
                }
            }
        }

        void DrawSeparator(RenderTarget& rt, int32_t y, int32_t scrollWidth)
        {
            const int32_t top = y + (kScrollableRowHeight / 2) - 1;
            Rectangle::fill(rt, { { 0, top }, { scrollWidth, top } }, getColourMap(colours[1].colour).midDark);
            Rectangle::fill(rt, { { 0, top + 1 }, { scrollWidth, top + 1 } }, getColourMap(colours[1].colour).lightest);
        }

        // OPENRCT2MINI input-bindings-rework §1.2/§2: column-layout
        // helper. Now defines seven cells per row:
        //   [label][binKbd][keyboard][binPad][gamepad][binMouse][mouse]
        // The bin cells are 12px wide and sit immediately LEFT of each
        // binding column, separated by a 2px inner gap. Per-row, a bin
        // cell collapses (recovers its 14px = 12 bin + 2 gap into the
        // adjacent binding column) when that column's binding is empty.
        // ColumnLayout returns the BASE offsets assuming all three bin
        // cells are visible; getRowOffsets() below derives the per-row
        // actual offsets based on which bindings exist.
        static constexpr int32_t kBinCellWidth = 12;
        static constexpr int32_t kBinInnerGap = 2;
        static constexpr int32_t kBinReserve = kBinCellWidth + kBinInnerGap; // 14

        struct ColumnLayout
        {
            int32_t actionWidth;          // 0..actionWidth: action name
            int32_t binKeyboardOffset;    // x of bin cell preceding keyboard
            int32_t keyboardOffset;       // x of keyboard binding text (after the bin reserve)
            int32_t binGamepadOffset;
            int32_t gamepadOffset;
            int32_t binMouseOffset;
            int32_t mouseOffset;
            int32_t keyboardWidth;        // base text width (post-bin-reserve)
            int32_t gamepadWidth;
            int32_t mouseWidth;
        };

        static ColumnLayout computeColumnLayout(int32_t scrollWidth)
        {
            ColumnLayout l{};
            // Each binding column reserves kBinReserve px on its left
            // for a bin cell + inner gap. The displayed text width is
            // kBindingColumnWidth - kBinReserve.
            l.keyboardWidth = kBindingColumnWidth - kBinReserve;
            l.gamepadWidth = kBindingColumnWidth - kBinReserve;
            l.mouseWidth = kBindingColumnWidth - kBinReserve;
            // Anchor the mouse column to the right of the scroll area.
            const int32_t mouseColLeft = scrollWidth - kBindingColumnWidth;
            l.binMouseOffset = mouseColLeft;
            l.mouseOffset = mouseColLeft + kBinReserve;
            const int32_t gamepadColLeft = mouseColLeft - kColumnGap - kBindingColumnWidth;
            l.binGamepadOffset = gamepadColLeft;
            l.gamepadOffset = gamepadColLeft + kBinReserve;
            const int32_t keyboardColLeft = gamepadColLeft - kColumnGap - kBindingColumnWidth;
            l.binKeyboardOffset = keyboardColLeft;
            l.keyboardOffset = keyboardColLeft + kBinReserve;
            l.actionWidth = std::max(0, keyboardColLeft - kColumnGap);
            return l;
        }

        // OPENRCT2MINI input-bindings-rework §2.2: per-row offsets that
        // collapse the bin cell when the adjacent binding is empty. The
        // bin's 14px reserve gets recovered by the binding column's
        // displayed text width, shifting the text left by 14px.
        struct RowOffsets
        {
            int32_t keyboardTextOffset;
            int32_t gamepadTextOffset;
            int32_t mouseTextOffset;
            int32_t keyboardTextWidth;
            int32_t gamepadTextWidth;
            int32_t mouseTextWidth;
        };

        static RowOffsets getRowOffsets(const ColumnLayout& cols, const ShortcutStringPair& shortcut)
        {
            RowOffsets r{};
            const bool kHasKbd = !shortcut.KeyboardBinding.empty();
            const bool kHasPad = !shortcut.GamepadBinding.empty();
            const bool kHasMouse = !shortcut.MouseBinding.empty();
            r.keyboardTextOffset = kHasKbd ? cols.keyboardOffset : cols.binKeyboardOffset;
            r.gamepadTextOffset = kHasPad ? cols.gamepadOffset : cols.binGamepadOffset;
            r.mouseTextOffset = kHasMouse ? cols.mouseOffset : cols.binMouseOffset;
            r.keyboardTextWidth = kHasKbd ? cols.keyboardWidth : cols.keyboardWidth + kBinReserve;
            r.gamepadTextWidth = kHasPad ? cols.gamepadWidth : cols.gamepadWidth + kBinReserve;
            r.mouseTextWidth = kHasMouse ? cols.mouseWidth : cols.mouseWidth + kBinReserve;
            return r;
        }

        void DrawItem(
            RenderTarget& rt, int32_t y, int32_t scrollWidth, const ShortcutStringPair& shortcut, size_t rowIndex)
        {
            const int32_t rowTop = y - 1;
            const int32_t rowBottom = y + (kScrollableRowHeight - 2);

            // §1.1: alternating row band. Even rows get the lighter
            // colour tint; odd rows leave the scroll background showing.
            // Skipping separator rows is handled by the caller — they
            // don't reach DrawItem.
            if ((rowIndex % 2) == 0)
            {
                Rectangle::fill(
                    rt, { 0, rowTop, scrollWidth, rowBottom },
                    getColourMap(colours[1].colour).lighter, true);
            }

            const auto cols = computeColumnLayout(scrollWidth);
            const auto rowOff = getRowOffsets(cols, shortcut);

            // §1.2: per-cell highlight. Decide which cell (if any) to
            // tint based on `_highlightedCell`. Only this row gets
            // highlighting if its index matches _highlightedRow.
            const bool isHoveredRow = _highlightedRow == static_cast<int_fast16_t>(rowIndex);
            const Cell hoverCell = isHoveredRow ? _highlightedCell : Cell::none;

            const auto tintCell = [&](int32_t left, int32_t cellWidth) {
                Rectangle::filter(
                    rt, { left, rowTop, left + cellWidth - 1, rowBottom },
                    FilterPaletteID::paletteDarken1);
            };
            // Highlighted cell drawn over the alternating band but
            // beneath the text — so text colour swaps for legibility.
            StringId actionFormat = STR_BLACK_STRING;
            StringId keyboardFormat = STR_BLACK_STRING;
            StringId gamepadFormat = STR_BLACK_STRING;
            StringId mouseFormat = STR_BLACK_STRING;
            switch (hoverCell)
            {
                case Cell::keyboard:
                    tintCell(rowOff.keyboardTextOffset, rowOff.keyboardTextWidth);
                    keyboardFormat = STR_WINDOW_COLOUR_2_STRINGID;
                    break;
                case Cell::gamepad:
                    tintCell(rowOff.gamepadTextOffset, rowOff.gamepadTextWidth);
                    gamepadFormat = STR_WINDOW_COLOUR_2_STRINGID;
                    break;
                case Cell::mouse:
                    tintCell(rowOff.mouseTextOffset, rowOff.mouseTextWidth);
                    mouseFormat = STR_WINDOW_COLOUR_2_STRINGID;
                    break;
                case Cell::binKbd:
                    if (!shortcut.KeyboardBinding.empty())
                        tintCell(cols.binKeyboardOffset, kBinCellWidth);
                    break;
                case Cell::binPad:
                    if (!shortcut.GamepadBinding.empty())
                        tintCell(cols.binGamepadOffset, kBinCellWidth);
                    break;
                case Cell::binMouse:
                    if (!shortcut.MouseBinding.empty())
                        tintCell(cols.binMouseOffset, kBinCellWidth);
                    break;
                case Cell::label:
                case Cell::none:
                    // Label column never highlights (decorative).
                    break;
            }

            // OPENRCT2MINI overlap-warn: yellow takes priority over the
            // black / window-colour-2 default — overlap is the more
            // important signal. NOTE: explicit StringId-typed locals
            // avoid a -Werror=enum-compare mismatch (overlap warn lives
            // in our UiStringIds enum; STR_BLACK_STRING/STR_WINDOW_-
            // COLOUR_2_STRINGID are upstream unnamed-enum constants).
            if (shortcut.KeyboardOverlap)
                keyboardFormat = static_cast<StringId>(STR_SHORTCUT_BINDING_OVERLAP_WARN);
            if (shortcut.GamepadOverlap)
                gamepadFormat = static_cast<StringId>(STR_SHORTCUT_BINDING_OVERLAP_WARN);
            if (shortcut.MouseOverlap)
                mouseFormat = static_cast<StringId>(STR_SHORTCUT_BINDING_OVERLAP_WARN);

            // Action label (column 0). Always black; never highlighted.
            auto ft = Formatter();
            ft.Add<StringId>(STR_SHORTCUT_ENTRY_FORMAT);
            if (shortcut.CustomString.empty())
            {
                ft.Add<StringId>(shortcut.StringId);
            }
            else
            {
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(shortcut.CustomString.c_str());
            }
            drawTextEllipsised(rt, { 0, y - 1 }, cols.actionWidth, actionFormat, ft);

            // §2: bin cells. Drawn only when the adjacent binding is
            // non-empty. The bin button fills its 12px cell; we centre
            // the sprite using its actual dimensions + baked xOffset/
            // yOffset because SPR_STAFF_ORDERS_EMPTY_BINS isn't exactly
            // 12×12 with origin (0,0) — querying G1Element gives the
            // correct top-left so the icon sits centred regardless of
            // its packed offset. Falls back to a midpoint estimate if
            // the sprite lookup fails (defensive — shouldn't happen
            // since g1.dat ships this sprite).
            const auto* binG1 = GfxGetG1Element(ImageId(SPR_STAFF_ORDERS_EMPTY_BINS));
            const int32_t binSpriteW = (binG1 != nullptr) ? binG1->width : kBinCellWidth;
            const int32_t binSpriteH = (binG1 != nullptr) ? binG1->height : kScrollableRowHeight;
            const int32_t binXOffset = (binG1 != nullptr) ? binG1->xOffset : 0;
            const int32_t binYOffset = (binG1 != nullptr) ? binG1->yOffset : 0;
            const auto drawBin = [&](int32_t left) {
                // Desired top-left in screen space = cell origin + half the
                // empty space. GfxDrawSprite renders at (drawCoord + offset),
                // so subtract the baked offset from the desired top-left to
                // get the coord to pass.
                const int32_t topLeftX = left + (kBinCellWidth - binSpriteW) / 2;
                const int32_t topLeftY = (y - 1) + (kScrollableRowHeight - binSpriteH) / 2;
                GfxDrawSprite(
                    rt, ImageId(SPR_STAFF_ORDERS_EMPTY_BINS),
                    { topLeftX - binXOffset, topLeftY - binYOffset });
            };
            if (!shortcut.KeyboardBinding.empty())
                drawBin(cols.binKeyboardOffset);
            if (!shortcut.GamepadBinding.empty())
                drawBin(cols.binGamepadOffset);
            if (!shortcut.MouseBinding.empty())
                drawBin(cols.binMouseOffset);

            // Binding column text. Use per-row offsets so a missing
            // binding's column expands left into the recovered 14px.
            if (!shortcut.KeyboardBinding.empty())
            {
                ft = Formatter();
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(shortcut.KeyboardBinding.c_str());
                drawTextEllipsised(rt, { rowOff.keyboardTextOffset, y - 1 }, rowOff.keyboardTextWidth, keyboardFormat, ft);
            }

            if (!shortcut.GamepadBinding.empty())
            {
                ft = Formatter();
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(shortcut.GamepadBinding.c_str());
                drawTextEllipsised(rt, { rowOff.gamepadTextOffset, y - 1 }, rowOff.gamepadTextWidth, gamepadFormat, ft);
            }

            if (!shortcut.MouseBinding.empty())
            {
                ft = Formatter();
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(shortcut.MouseBinding.c_str());
                drawTextEllipsised(rt, { rowOff.mouseTextOffset, y - 1 }, rowOff.mouseTextWidth, mouseFormat, ft);
            }
        }
    };

    void ChangeShortcutWindow::NotifyShortcutKeysWindow()
    {
        auto* windowMgr = GetWindowManager();
        auto w = windowMgr->FindByClass(WindowClass::keyboardShortcutList);
        if (w != nullptr)
        {
            static_cast<ShortcutKeysWindow*>(w)->RefreshBindings();
        }
    }

    WindowBase* ShortcutKeysOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto w = windowMgr->BringToFrontByClass(WindowClass::keyboardShortcutList);
        if (w == nullptr)
        {
            w = windowMgr->Create<ShortcutKeysWindow>(WindowClass::keyboardShortcutList, kWindowSize, WindowFlag::resizable);
        }
        return w;
    }

#pragma region Reset prompt
    static constexpr ScreenSize kWindowSizeReset = { 200, 80 };

    enum
    {
        WIDX_RESET_PROMPT_BACKGROUND,
        WIDX_RESET_PROMPT_TITLE,
        WIDX_RESET_PROMPT_CLOSE,
        WIDX_RESET_PROMPT_LABEL,
        WIDX_RESET_PROMPT_RESET,
        WIDX_RESET_PROMPT_CANCEL
    };

    // clang-format off
    static const auto WindowResetShortcutKeysPromptWidgets = makeWidgets(
        makeWindowShim(STR_SHORTCUT_ACTION_RESET, kWindowSizeReset),
        makeWidget({                           2,                           30 }, { kWindowSizeReset.width - 4, 12 }, WidgetType::labelCentred, WindowColour::primary, STR_RESET_SHORTCUT_KEYS_PROMPT),
        makeWidget({                           8, kWindowSizeReset.height - 22 }, {                         85, 14 }, WidgetType::button,       WindowColour::primary, STR_RESET),
        makeWidget({ kWindowSizeReset.width - 95, kWindowSizeReset.height - 22 }, {                         85, 14 }, WidgetType::button,       WindowColour::primary, STR_SAVE_PROMPT_CANCEL)
    );
    // clang-format on

    class ResetShortcutKeysPrompt final : public Window
    {
        void onOpen() override
        {
            setWidgets(WindowResetShortcutKeysPromptWidgets);
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_RESET_PROMPT_RESET:
                {
                    auto* windowMgr = GetWindowManager();
                    auto w = windowMgr->FindByClass(WindowClass::keyboardShortcutList);
                    if (w != nullptr)
                    {
                        static_cast<ShortcutKeysWindow*>(w)->ResetAllOnActiveTab();
                    }
                    close();
                    break;
                }
                case WIDX_RESET_PROMPT_CANCEL:
                case WIDX_RESET_PROMPT_CLOSE:
                    close();
                    break;
            }
        }
    };

    WindowBase* ResetShortcutKeysPromptOpen()
    {
        auto* windowMgr = GetWindowManager();
        return windowMgr->FocusOrCreate<ResetShortcutKeysPrompt>(
            WindowClass::resetShortcutKeysPrompt, kWindowSizeReset, { WindowFlag::centreScreen, WindowFlag::transparent });
    }
#pragma endregion
} // namespace OpenRCT2::Ui::Windows
