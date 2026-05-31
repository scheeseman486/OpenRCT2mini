/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "InputManager.h"

#include "InputContextStrategy.h"
#include "ShortcutIds.h"

#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/MouseInput.h>
#include <openrct2-ui/input/ShortcutManager.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/InGameConsole.h>
#include <openrct2-ui/interface/ViewportInteraction.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/haptic/HapticEvent.h>
#include <openrct2/haptic/LedEvent.h>
#include <openrct2/config/Config.h>
#include <openrct2/interface/Chat.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Widget.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/interface/WindowClasses.h>
#include <openrct2/paint/VirtualFloor.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/world/Footpath.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/MapSelection.h>
// OPENRCT2MINI grid-cursor-plan §11.4 Step C (2026-05-31): for
// gSceneryPlaceZ + gSceneryShiftPressed used by the SmallScenery
// shift+D-pad Z-stack gesture in SceneryContextImpl.
#include <openrct2/world/Scenery.h>
#include "WidgetFocus.h"

using namespace OpenRCT2::Ui;

// OPENRCT2MINI input-plan Track 3 / Phase 3.B: concrete strategy
// classes for the six existing modal InputContext enum entries. Each
// owns the per-context keyboard routing that used to live as a
// switch-on-window-class in InputManager::process(InputEvent). The
// strategies return Passthrough from onShortcut for now — the per-
// context allow-list filter (isShortcutAllowedInActiveContext) still
// owns the "which shortcuts are allowed in this modal" gate; Phase
// 3.E and later layer Consumed semantics on top for the tool
// contexts that need to override world-mode shortcut handling.
//
// The strategies live in an anonymous namespace because they're only
// instantiated by InputManager's constructor. The public interface
// (InputContextStrategy.h) declares only the abstract types.
namespace
{
    // OPENRCT2MINI osk-overhaul §1: the OSK now routes through the
    // shared focus-mode dispatch. focus.* moves the focus ring across
    // OSK keys via WidgetFocus::findNearestInDirection; cursor.click
    // activates the focused key via pressWidgetByIndex; cursor.cancel
    // is overridden to fire Backspace instead of closing the window
    // (the modal `dismiss` hook is what closes the OSK — see
    // OskWindow::onOpen). PAD START / PAD BACK / RETURN / ESCAPE all
    // continue to flow through ShortcutManager into kInterfaceConfirm /
    // kInterfaceDismiss, matched by the OSK's modal hooks.
    class OskContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::osk;
        }

        Disposition onShortcut(std::string_view id, const InputEvent& e) override
        {
            // Mouse events fall through to the normal hit-test path —
            // clicks on OSK keys are dispatched by the mouse pump in
            // MouseInput.cpp, same as any other window. (Same shape
            // as WidgetFocusContextImpl::onShortcut.)
            if (e.deviceKind == InputDeviceKind::mouse)
                return Disposition::Passthrough;

            auto& mgr = OpenRCT2::Ui::GetInputManager();

            // osk-overhaul bug-fix §A3: match widgetFocus mode-gating
            // exactly. When the selector is hidden (user is on real
            // mouse, the OS-level cursor is driving the UI), this
            // strategy is dormant — no navigation, no virtual click
            // synthesis. The user re-enters focus mode via the
            // bindable Enter Focus Mode shortcut (TAB by default),
            // identical to every other window. Modal hooks (dismiss /
            // confirm = OSK Cancel / Commit) work in either mode
            // because they live on the modal-hooks stack, not in
            // onShortcut.
            if (mgr.getSelectorMode() != InputManager::SelectorMode::active)
                return Disposition::Passthrough;

            auto* windowMgr = GetWindowManager();
            if (windowMgr == nullptr)
                return Disposition::Passthrough;
            auto* osk = windowMgr->FindByClass(WindowClass::osk);
            if (osk == nullptr)
                return Disposition::Passthrough;

            // Directional: walk the OSK widget grid via the shared
            // spatial-nearest helper. The OSK lives in its own one-
            // window set so staying inside the OSK is correct (no
            // cross-set traversal to TextInputWindow / console).
            if (const auto dir = directionForShortcut(id); dir.has_value())
            {
                const auto from = mgr.getFocusedWidget();
                const auto next = OpenRCT2::Ui::WidgetFocus::findNearestInDirection(
                    *osk, from, *dir);
                if (next != OpenRCT2::kWidgetIndexNull && next != from)
                {
                    mgr.setFocus(WindowClass::osk, next);
                    osk->invalidate();
                }
                return Disposition::Consumed;
            }

            // cursor.click: press the focused key. Activation lives on
            // OskWindow::onMouseDown — pressWidgetByIndex invokes it.
            if (id == ShortcutId::kCursorClick)
            {
                Windows::OskActivateFocusedKey();
                return Disposition::Consumed;
            }

            // cursor.cancel: backspace. This is the one place the OSK
            // diverges from widgetFocus — widgetFocus closes the
            // focused window (presses its closeBox) which would be
            // wrong here; the OSK has no closeBox and Cancel-the-OSK
            // is reserved for the dismiss modal hook (PAD BACK /
            // ESCAPE).
            if (id == ShortcutId::kCursorCancel)
            {
                Windows::OskActivateBackspace();
                return Disposition::Consumed;
            }

            return Disposition::Passthrough;
        }

    private:
        // Same shortcut → direction mapping the widgetFocus strategy
        // uses (kept local because the strategy's helper is private).
        static std::optional<OpenRCT2::Ui::WidgetFocus::Direction> directionForShortcut(std::string_view id)
        {
            if (id == ShortcutId::kFocusUp)
                return OpenRCT2::Ui::WidgetFocus::Direction::up;
            if (id == ShortcutId::kFocusDown)
                return OpenRCT2::Ui::WidgetFocus::Direction::down;
            if (id == ShortcutId::kFocusLeft)
                return OpenRCT2::Ui::WidgetFocus::Direction::left;
            if (id == ShortcutId::kFocusRight)
                return OpenRCT2::Ui::WidgetFocus::Direction::right;
            return std::nullopt;
        }
    };

    class TextInputContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::textInput;
        }
        void onKeyEvent(const InputEvent& e) override
        {
            // Mirrors the original keyboard branch in process(InputEvent)
            // verbatim — only fires on release, only when the textinput
            // window still exists.
            if (e.state != InputEventState::release)
                return;
            auto* windowMgr = GetWindowManager();
            if (windowMgr == nullptr)
                return;
            auto* w = windowMgr->FindByClass(WindowClass::textinput);
            if (w != nullptr)
                Windows::WindowTextInputKey(w, e.button);
        }
    };

    class LoadSaveOverwritePromptContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::loadSaveOverwritePrompt;
        }
        void onKeyEvent(const InputEvent& e) override
        {
            if (e.state != InputEventState::release)
                return;
            auto* windowMgr = GetWindowManager();
            if (windowMgr == nullptr)
                return;
            auto* w = windowMgr->FindByClass(WindowClass::loadsaveOverwritePrompt);
            if (w != nullptr)
                Windows::WindowLoadSaveOverwritePromptInputKey(w, e.button);
        }
    };

    class LoadSaveContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::loadSave;
        }
        void onKeyEvent(const InputEvent& e) override
        {
            if (e.state != InputEventState::release)
                return;
            auto* windowMgr = GetWindowManager();
            if (windowMgr == nullptr)
                return;
            auto* w = windowMgr->FindByClass(WindowClass::loadsave);
            if (w != nullptr)
                Windows::WindowLoadSaveInputKey(w, e.button);
        }
    };

    class ConsoleContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::console;
        }
        void onKeyEvent(const InputEvent& e) override
        {
            // Inlined version of InputManager::processInGameConsole —
            // moves the per-key console scroll routing into the
            // strategy. UP/DOWN step through history; PAGEUP/PAGEDOWN
            // scroll the buffer. Only fires on keyboard release.
            if (e.deviceKind != InputDeviceKind::keyboard)
                return;
            if (e.state != InputEventState::release)
                return;
            auto& console = GetInGameConsole();
            if (!console.IsOpen())
                return;
            auto input = ConsoleInput::None;
            switch (e.button)
            {
                case SDLK_UP:
                    input = ConsoleInput::HistoryPrevious;
                    break;
                case SDLK_DOWN:
                    input = ConsoleInput::HistoryNext;
                    break;
                case SDLK_PAGEUP:
                    input = ConsoleInput::ScrollPrevious;
                    break;
                case SDLK_PAGEDOWN:
                    input = ConsoleInput::ScrollNext;
                    break;
            }
            if (input != ConsoleInput::None)
                console.Input(input);
        }
    };

    class WidgetTextBoxContextImpl final : public IInputContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::widgetTextBox;
        }
        // No special keyboard routing — text flows through SDL_TEXT-
        // INPUT and TextComposition, neither of which uses this
        // InputEvent path. The per-context allow-list suppresses
        // every non-dismiss/confirm shortcut while a textbox is
        // being edited. Phase 3.B preserves that — the strategy
        // doesn't need to do anything extra.
    };

    // OPENRCT2MINI focus-mode-plan / Phase F.4: widget-focus strategy.
    // Activated when InputManager::resolveActiveContext() observes a
    // live focused window (the per-frame bootstrap in process() is
    // what populates `_focusedWindowClass`). Routes cursor.* /
    // cursor.click / cursor.cancel / kInterfaceDismiss through the
    // WidgetFocus helpers and the InputManager focus-state accessors.
    //
    // Routing decisions:
    //   cursor.up/down/left/right → findNearestInDirection +
    //       setFocus; window->invalidate() so the focus ring (Phase
    //       F.5) repaints.
    //   cursor.click              → pressWidgetByIndex on the focused
    //       widget. Side-effects fire from WindowBase::onMouseDown,
    //       which is what a real mouse-click at the widget centre
    //       would have triggered.
    //   cursor.cancel             → close the focused window. This
    //       mirrors the console-style "B = back-out" UX. The user is
    //       inside a window's focus ring, B leaves it.
    //   kInterfaceDismiss         → exit focus mode without closing
    //       the window. `clearFocus` + `clearFocusMode` resets every
    //       state field; resolveActiveContext on the next frame
    //       returns world (or whichever modal is on top).
    //
    // Every cursor.* path returns Consumed so the world-cursor
    // virtual-mouse motion doesn't fire alongside (would move both
    // the focus ring AND the cursor pixel). The actions registered
    // against the shortcuts in Shortcuts.cpp are empty lambdas, so
    // Consumed-vs-Passthrough is functionally a no-op there — but
    // Consumed is semantically correct and matches the strategy
    // contract.
    //
    // Press-only filtering happens upstream in
    // ShortcutManager::processEvent (line 393) — release events
    // never reach onShortcut. No need to gate on e.state here.
    //
    // See focus-mode-plan.md §F.4 for the full design.
    class WidgetFocusContextImpl final : public IInputContext
    {
        // OPENRCT2MINI grid-cursor-plan §8.5 (2026-05-25): directional
        // repeat-on-hold tracker. Initial press fires through
        // onShortcut → dispatchDirection and calls notePress() to
        // anchor the press timestamp; processFrame polls every frame
        // and re-fires dispatchDirection for any direction still held
        // past kInitialDelayMs / kRepeatIntervalMs. Slot order matches
        // OpenRCT2::Ui::WidgetFocus::Direction (up=0, down=1, left=2,
        // right=3). SDL OS auto-repeat is already filtered at
        // UiContext.cpp:1041 (task #516), so the poll owns ALL repeats
        // uniformly across keyboard and gamepad.
        OpenRCT2::Ui::DirectionalRepeat _repeat{};

        // Helper extracted from onShortcut's direction-handler block.
        // Both onShortcut (initial press) and processFrame (repeats)
        // call this with the same Direction so the full dispatch path
        // — drag-scroll arm, list-mode arm, set-aware walker, list
        // re-seed on cross-window hop — runs identically for both.
        Disposition dispatchDirection(OpenRCT2::Ui::WidgetFocus::Direction direction)
        {
            auto& mgr = OpenRCT2::Ui::GetInputManager();
            // OPENRCT2MINI cursor-selector-modal-plan §2.1 / CS.3:
            // a focus.* direction confirms selector use. Wake the
            // selector from `mixed`/`hidden` back to `active`.
            // No-op when already `active`.
            mgr.onTransitionEvent(InputManager::SelectorTransitionSource::virtualUserInput);
            auto* w = mgr.getFocusedWindow();
            if (w != nullptr)
            {
                const auto from = mgr.getFocusedWidget();

                // OPENRCT2MINI list-focus-plan: drag-scroll arm.
                // When kInterfaceCameraDrag is held AND focus is on
                // a scroll widget, the four directional inputs
                // scroll the content rather than stepping focus.
                if (from != OpenRCT2::kWidgetIndexNull
                    && static_cast<size_t>(from) < w->widgets.size()
                    && w->widgets[from].type == OpenRCT2::WidgetType::scroll)
                {
                    auto& shortcutMgr = GetShortcutManager();
                    const auto* dragShortcut = shortcutMgr.getShortcut(ShortcutId::kInterfaceCameraDrag);
                    if (dragShortcut != nullptr && mgr.getState(*dragShortcut))
                    {
                        constexpr int32_t kScrollDragStep = 16;
                        const auto& wd = w->widgets[from];
                        const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(*w, from);
                        auto& s = w->scrolls[scrollIdx];
                        const bool canScrollH = (s.flags & OpenRCT2::HSCROLLBAR_VISIBLE) != 0;
                        const bool canScrollV = (s.flags & OpenRCT2::VSCROLLBAR_VISIBLE) != 0;
                        bool moved = false;
                        switch (direction)
                        {
                            case OpenRCT2::Ui::WidgetFocus::Direction::up:
                                if (canScrollV)
                                {
                                    s.contentOffsetY = std::max(0, s.contentOffsetY - kScrollDragStep);
                                    moved = true;
                                }
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::down:
                                if (canScrollV)
                                {
                                    const int32_t viewH = std::max(0, wd.bottom - wd.top - 2);
                                    const int32_t maxY = std::max(
                                        0, static_cast<int32_t>(s.contentHeight) - viewH);
                                    s.contentOffsetY = std::min(maxY, s.contentOffsetY + kScrollDragStep);
                                    moved = true;
                                }
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::left:
                                if (canScrollH)
                                {
                                    s.contentOffsetX = std::max(0, s.contentOffsetX - kScrollDragStep);
                                    moved = true;
                                }
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::right:
                                if (canScrollH)
                                {
                                    const int32_t viewW = std::max(0, wd.right - wd.left - 2);
                                    const int32_t maxX = std::max(
                                        0, static_cast<int32_t>(s.contentWidth) - viewW);
                                    s.contentOffsetX = std::min(maxX, s.contentOffsetX + kScrollDragStep);
                                    moved = true;
                                }
                                break;
                        }
                        if (moved)
                        {
                            widgetScrollUpdateThumbs(*w, from);
                            w->invalidate();
                        }
                        return Disposition::Consumed;
                    }
                }

                // OPENRCT2MINI list-focus-plan §2.3: list-mode arm.
                const ScreenCoordsXY refPoint = computeListEntryReference(
                    *w, from, mgr.getFocusedScrollItem());

                if (OpenRCT2::Ui::WidgetFocus::isListModeScroll(*w, from))
                {
                    const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(*w, from);
                    const int32_t n = w->scrollFocusGetItemCount(scrollIdx);
                    const int32_t cols = std::max(1, w->scrollFocusGetColumnCount(scrollIdx));
                    const int32_t cur = mgr.getFocusedScrollItem();

                    if (cur < 0)
                    {
                        int32_t seed = OpenRCT2::Ui::WidgetFocus::nearestScrollItemTo(*w, from, refPoint);
                        if (seed < 0)
                            seed = 0;
                        mgr.setFocusScrollItem(seed);
                        OpenRCT2::Ui::WidgetFocus::ensureScrollItemVisible(*w, from, seed);
                        syncFocusItemHover(*w, from, seed);
                        w->invalidate();
                        return Disposition::Consumed;
                    }

                    int32_t step = cur;
                    bool exit = false;
                    switch (direction)
                    {
                        case OpenRCT2::Ui::WidgetFocus::Direction::up:
                            step = cur - cols;
                            if (step < 0)
                                exit = true;
                            break;
                        case OpenRCT2::Ui::WidgetFocus::Direction::down:
                            step = cur + cols;
                            if (step >= n)
                                exit = true;
                            break;
                        case OpenRCT2::Ui::WidgetFocus::Direction::left:
                            if (cols > 1 && (cur % cols) > 0)
                                step = cur - 1;
                            else
                                exit = true;
                            break;
                        case OpenRCT2::Ui::WidgetFocus::Direction::right:
                            if (cols > 1 && (cur % cols) < (cols - 1) && (cur + 1) < n)
                                step = cur + 1;
                            else
                                exit = true;
                            break;
                    }

                    while (!exit && step >= 0 && step < n)
                    {
                        const auto cellRect = w->scrollFocusGetItemRect(scrollIdx, step);
                        if (cellRect.GetLeft() < cellRect.GetRight()
                            && cellRect.GetTop() < cellRect.GetBottom())
                            break;
                        switch (direction)
                        {
                            case OpenRCT2::Ui::WidgetFocus::Direction::up:
                                step -= cols;
                                if (step < 0)
                                    exit = true;
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::down:
                                step += cols;
                                if (step >= n)
                                    exit = true;
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::left:
                                if (cols > 1 && (step % cols) > 0)
                                    step -= 1;
                                else
                                    exit = true;
                                break;
                            case OpenRCT2::Ui::WidgetFocus::Direction::right:
                                if (cols > 1 && (step % cols) < (cols - 1) && (step + 1) < n)
                                    step += 1;
                                else
                                    exit = true;
                                break;
                        }
                    }

                    if (!exit && step >= 0 && step < n)
                    {
                        mgr.setFocusScrollItem(step);
                        OpenRCT2::Ui::WidgetFocus::ensureScrollItemVisible(*w, from, step);
                        syncFocusItemHover(*w, from, step);
                        w->invalidate();
                        return Disposition::Consumed;
                    }

                    mgr.clearFocusScrollItem();
                    w->invalidate();
                }

                // OPENRCT2MINI window-set-plan §3.2: set-aware walker.
                WindowClass nextCls = w->classification;
                const auto next = OpenRCT2::Ui::WidgetFocus::findNearestInSetDirection(
                    w->classification, from, direction, &nextCls);
                if (next != OpenRCT2::kWidgetIndexNull
                    && (nextCls != w->classification || next != from))
                {
                    mgr.setFocus(nextCls, next);
                    w->invalidate();
                    if (nextCls != w->classification)
                    {
                        auto* windowMgr = GetWindowManager();
                        if (windowMgr != nullptr)
                            windowMgr->InvalidateByClass(nextCls);
                    }

                    auto* nextWin = mgr.getFocusedWindow();
                    if (nextWin != nullptr
                        && OpenRCT2::Ui::WidgetFocus::isListModeScroll(*nextWin, next))
                    {
                        int32_t seed = OpenRCT2::Ui::WidgetFocus::nearestScrollItemTo(
                            *nextWin, next, refPoint);
                        if (seed < 0)
                            seed = 0;
                        mgr.setFocusScrollItem(seed);
                        OpenRCT2::Ui::WidgetFocus::ensureScrollItemVisible(*nextWin, next, seed);
                        syncFocusItemHover(*nextWin, next, seed);
                    }
                }
            }
            return Disposition::Consumed;
        }

    public:
        InputContext getId() const override
        {
            return InputContext::widgetFocus;
        }

        // OPENRCT2MINI grid-cursor-plan §8.5: reset repeat state on
        // context boundary crossings so a held direction during the
        // transition requires a fresh press in the new context.
        void onActivate() override { _repeat.reset(); }
        void onDeactivate() override { _repeat.reset(); }

        // OPENRCT2MINI grid-cursor-plan §8.5: per-frame repeat poll.
        // Step 0 happens in onShortcut + notePress; subsequent steps
        // fire here every kRepeatIntervalMs after kInitialDelayMs.
        void processFrame(uint32_t nowMs) override
        {
            auto& shortcutMgr = OpenRCT2::Ui::GetShortcutManager();
            auto& mgr = OpenRCT2::Ui::GetInputManager();
            const auto checkHeld = [&](std::string_view shortcutId) -> bool {
                if (const auto* s = shortcutMgr.getShortcut(shortcutId); s != nullptr)
                    return mgr.getState(*s);
                return false;
            };
            // Slot order matches OpenRCT2::Ui::WidgetFocus::Direction.
            const bool held[4] = {
                checkHeld(OpenRCT2::Ui::ShortcutId::kFocusUp),
                checkHeld(OpenRCT2::Ui::ShortcutId::kFocusDown),
                checkHeld(OpenRCT2::Ui::ShortcutId::kFocusLeft),
                checkHeld(OpenRCT2::Ui::ShortcutId::kFocusRight),
            };
            const uint8_t fire = _repeat.tick(nowMs, held);
            for (uint8_t i = 0; i < 4; i++)
            {
                if (fire & static_cast<uint8_t>(1u << i))
                    dispatchDirection(static_cast<OpenRCT2::Ui::WidgetFocus::Direction>(i));
            }
        }

        Disposition onShortcut(std::string_view id, const InputEvent& e) override
        {
            // Critical gate: every cursor.* / cursor.click /
            // cursor.cancel binding has a MOUSE default too
            // (LMB → cursor.click, RMB → cursor.cancel,
            // wheel-bound directions on some setups). When the
            // mouse pump fires those events, the OpenRCT2 mouse-
            // input layer in MouseInput.cpp ALREADY handles the
            // click — finds the widget under the cursor, fires
            // onMouseDown/Up on it. If this strategy ALSO acts on
            // the same event, the user's single LMB click ends
            // up pressing TWO widgets: the one under the cursor
            // AND the focused widget. That bug surfaced on the
            // title scene as "clicking anywhere opens About"
            // because the title logo had latched focus and the
            // strategy's pressWidgetByIndex fired its onMouseUp
            // on top of the normal mouse pump's click handling.
            //
            // Focus-mode activation is only meaningful for
            // keyboard / gamepad input — those device paths have
            // no concept of "the widget under the cursor". Mouse
            // already has that concept via screen coords; the
            // shortcut layer is just a passthrough alias for it.
            // OPENRCT2MINI cursor-cancel-tile-action-plan §3.4
            // (Phase C follow-up 2026-05-17): mouse kCursorCancel
            // needs to fire the tile action even when widgetFocus
            // owns the active context — e.g. user in Land / Water /
            // Footpath etc. mode with the tool window focused (no
            // grid mode engaged), pressing RMB on a destroyable
            // element. Gated on state == release so the press-time
            // dispatch is silently swallowed; the held-state poll
            // for cursor.cancel mouse bindings in UiContext::
            // ProcessWorldCursor synthesises a release event after
            // verifying short-press (<500ms). This preserves the
            // legacy disambiguation — short tap = delete, long
            // hold = no-op (hold-to-not-delete) — while a direct
            // cursor.cancel binding has no drag state to fall
            // through. Non-mouse devices (gamepad PAD B / keyboard)
            // fire on press normally because they don't have the
            // tap-vs-drag ambiguity.
            if (e.deviceKind == InputDeviceKind::mouse && id == ShortcutId::kCursorCancel)
            {
                if (e.state == InputEventState::release)
                {
                    const auto pos = OpenRCT2::ContextGetCursorPosition();
                    ViewportInteractionRightClick(pos);
                }
                // Swallow the press-time dispatch so the empty
                // action lambda doesn't fire and no double-fire
                // can leak from this path either.
                return Disposition::Consumed;
            }

            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment
            // 2026-05-17 #3 — user feedback): non-mouse
            // cursor.cancel (PAD B / keyboard X) in widgetFocus
            // with a tool armed CLOSES the tool window. The
            // gesture is symmetric with grid-cursor mode's
            // Back-closes-the-window verb — pressing PAD B on
            // the focused tool window's UI should drop the user
            // out of the tool entirely, same as pressing it in
            // grid-cursor mode. The "tap-to-delete-at-cursor"
            // behaviour is reserved for grid-cursor mode (where
            // the user has explicitly engaged a destructive
            // workflow); in widgetFocus the user is browsing the
            // tool window's options, and cancel-out is the
            // expected verb.
            //
            // Mouse cursor.cancel above (RMB) keeps the tile-
            // delete behaviour for legacy parity — the
            // ~500 ms short-press timer disambiguates it from a
            // camera drag.
            //
            // OPENRCT2MINI grid-cursor-plan §14.7 (2026-05-20):
            // gate the close-tool behaviour on the selector
            // actually being active. When the user is in cursor
            // mode (selector hidden — real or virtual mouse
            // driving the UI) the active strategy is still
            // widgetFocus because the tool window has focus, but
            // the user's mental model is "I'm using the cursor".
            // PAD B should act as a delete-at-cursor verb (RMB
            // semantic on gamepad), not close the tool. Fall
            // through to the cursor-mode handler below when the
            // selector is hidden so non-mouse cursor.cancel
            // dispatches ViewportInteractionRightClick at the
            // cursor's screen position.
            if (id == ShortcutId::kCursorCancel
                && e.state == InputEventState::down
                && OpenRCT2::gInputFlags.has(OpenRCT2::InputFlag::toolActive)
                && OpenRCT2::Ui::GetInputManager().getSelectorMode()
                    == OpenRCT2::Ui::InputManager::SelectorMode::active)
            {
                const auto cls = OpenRCT2::gCurrentToolWidget.windowClassification;
                OpenRCT2::ToolCancel();
                if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                    windowMgr->CloseByClass(cls);
                return Disposition::Consumed;
            }

            // OPENRCT2MINI grid-cursor-plan §14.7 (2026-05-20):
            // cursor-mode delete-at-cursor for non-mouse devices.
            // Fires when the selector is hidden (cursor mode) and
            // a non-mouse cursor.cancel arrives — the user is
            // driving the cursor and pressed PAD B / keyboard X
            // to delete what they're hovering over. Mirrors the
            // mouse handler above, dispatching
            // ViewportInteractionRightClick at the cursor's
            // screen position. Fires on press for non-mouse
            // devices (no tap-vs-drag ambiguity to resolve).
            if (id == ShortcutId::kCursorCancel
                && e.state == InputEventState::down
                && OpenRCT2::Ui::GetInputManager().getSelectorMode()
                    == OpenRCT2::Ui::InputManager::SelectorMode::hidden)
            {
                const auto pos = OpenRCT2::ContextGetCursorPosition();
                ViewportInteractionRightClick(pos);
                return Disposition::Consumed;
            }

            // So short-circuit early when the input came from a
            // mouse: let the mouse pump own the click, return
            // Passthrough so the action lambda (typically empty)
            // doesn't double-fire.
            if (e.deviceKind == InputDeviceKind::mouse)
                return Disposition::Passthrough;

            auto& mgr = OpenRCT2::Ui::GetInputManager();

            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17):
            // confirm-into-grid shortcut. When a construction tool
            // is armed AND the focused window is the tool's own
            // window, pressing the generic interface.confirm
            // shortcut engages grid-cursor mode. This is the
            // bookend to ToolContext::exitGridCursorMode (which
            // dismisses out) — confirm in the tool window jumps
            // straight to drawing without making the user navigate
            // to one of the construction-mode buttons first. We
            // gate on the focused window being the tool window
            // (gCurrentToolWidget.windowClassification) so confirm
            // in other windows behaves normally even if a tool is
            // background-armed.
            //
            // OPENRCT2MINI grid-cursor-plan §14.5 (2026-05-20):
            // runs *before* the selector-mode dormancy gate below
            // so the gesture also fires from cursor mode (real or
            // virtual mouse driving the selector). The user's
            // mental model is "Start with a tool armed means I'm
            // ready to draw" regardless of which input mode is
            // currently active. ToolContext::onActivate flips the
            // selector to active on the strategy transition that
            // follows, so the visual machinery (cursor hide /
            // selector ring) lines up without an explicit flip
            // here.
            // OPENRCT2MINI grid-cursor-plan §19 (2026-05-23): the
            // "engage condition" now has two paths:
            //
            //   1. Classic: tool is armed (gInputFlags.toolActive)
            //      AND focused window is the tool's window.
            //      Pressing Start engages grid cursor on that tool.
            //
            //   2. Footpath bridge-in-progress: tool is NOT armed
            //      (the mouse path's StartBridgeAtPoint called
            //      ToolCancel — see Footpath.cpp:2352), but the
            //      Footpath window is still mid-build (input mode
            //      != none). User entered focus mode on the
            //      Footpath window to keep tweaking, and now wants
            //      to hand control back to the grid cursor.
            //      Re-arm the Footpath tool for its current mode
            //      and engage as in case 1.
            //
            // Both paths require the focused window to be the
            // Footpath window (or the tool's window for case 1) so
            // Start in other windows behaves normally even if
            // there's a tool background-armed or a bridge mid-
            // build.
            const bool toolArmed = OpenRCT2::gInputFlags.has(OpenRCT2::InputFlag::toolActive);
            const bool focusedOnTool = toolArmed
                && mgr.getFocusedWindowClass() == OpenRCT2::gCurrentToolWidget.windowClassification;
            const bool footpathInMode
                = (Windows::WindowFootpathGetInputMode() != Windows::FootpathInputMode::none);
            const bool focusedOnFootpath
                = (mgr.getFocusedWindowClass() == WindowClass::footpath);
            const bool footpathInModeEngage = !toolArmed && footpathInMode && focusedOnFootpath;
            // OPENRCT2MINI ride-construction-grid-cursor-plan §9 (Phase 1, 2026-05-25):
            // mirror the Footpath in-mode engage for RideConstruction. When the
            // user is focused on the construction window pressing Start re-arms
            // WIDX_CONSTRUCT so the next frame resolveActiveContext routes to
            // RideConstructionContextImpl.
            //
            // §16 follow-up (2026-05-25): the predicate is broader than Footpath's
            // for two reasons:
            //   (a) State0 (the idle "between pieces" state where the user picks
            //       the next piece shape) returns RideInputMode::none. We still
            //       want to engage there so the user can D-pad around and place.
            //       Use "window exists" instead of a state-based check.
            //   (b) WindowClass::ride is included defensively. The sibling ride
            //       info window (stats/vehicle/appearance) usually isn't open
            //       during construction, but if it is and focus drifts to it,
            //       Start should still engage. ReArm always targets the
            //       rideConstruction window's WIDX_CONSTRUCT regardless.
            // Bridge case has a simpler predicate because Footpath has ONE window
            // and a single mode-state enum that's always non-none mid-build.
            auto* _rideWindowMgr = GetWindowManager();
            const bool rideWindowExists = (_rideWindowMgr != nullptr
                && _rideWindowMgr->FindByClass(WindowClass::rideConstruction) != nullptr);
            const auto _focusedCls = mgr.getFocusedWindowClass();
            const bool focusedOnRide
                = (_focusedCls == WindowClass::rideConstruction || _focusedCls == WindowClass::ride);
            const bool rideInModeEngage = rideWindowExists && focusedOnRide;
            if (id == ShortcutId::kInterfaceConfirm && (focusedOnTool || footpathInModeEngage || rideInModeEngage))
            {
                // OPENRCT2MINI grid-cursor-plan §12.1 (amendment
                // 2026-05-17 #6 — off-by-one frame fix engage
                // side): the InvalidateByClass that used to live
                // here fired on the SAME frame as the
                // setToolFocusSelected, but _activeContext had
                // already been resolved at the top of process()
                // and was still widgetFocus. The same-frame paint
                // saw ctx == widgetFocus, redrew the window AND
                // re-painted the focus ring. The active context
                // flipped to toolFootpath on the NEXT frame, but
                // no fresh invalidation fired, so the ring stayed
                // painted from the previous frame.
                //
                // Moved the invalidation into
                // ToolContext::onActivate, which runs from the
                // strategy-transition block on frame N+1 AFTER
                // resolveActiveContext flips _activeContext to
                // toolFootpath. The invalidation dirties the
                // window's blocks for that same frame's paint,
                // which now sees ctx == toolFootpath and gate-1
                // of drawFocusOutlineIfActive returns early. Ring
                // disappears.
                //
                // §19: case 2 re-arms the Footpath tool widget
                // for the current construction mode BEFORE the
                // toolFocusSelected latch. resolveActiveContext
                // gates on `_toolFocusSelected && toolActive`
                // (both required) — without the re-arm, the next
                // frame's routing wouldn't reach the FootpathContext
                // strategy.
                if (footpathInModeEngage)
                    Windows::WindowFootpathReArmForCurrentMode();
                if (rideInModeEngage)
                    Windows::WindowRideConstructionReArmForCurrentMode();
                mgr.setToolFocusSelected(
                    true, OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                return Disposition::Consumed;
            }

            // OPENRCT2MINI cursor-selector-modal-plan v2: when the
            // state machine has the selector OFF (hidden), this
            // strategy is dormant for everything *except* the
            // grid-cursor engage gesture handled just above.
            // focus.* shouldn't navigate (state machine wakes via
            // the modal switch's enterFocusModeRequested path on
            // unshared press, or via the per-frame bootstrap's
            // new-window auto-wake); cursor.click shouldn't
            // synthesise a virtual press through pressWidgetByIndex
            // either — the ProcessWorldCursor synthetic-mouse path
            // already dispatches the click to whichever widget the
            // cursor is over. Pass through everything until the
            // selector is reactivated.
            if (mgr.getSelectorMode() != OpenRCT2::Ui::InputManager::SelectorMode::active)
                return Disposition::Passthrough;

            // OPENRCT2MINI focus-mode-plan §F.10: dropdown
            // specialisation. The dropdown window has exactly one
            // widget (a 1×1 placeholder imgBtn — see Dropdown.cpp:57)
            // so the generic findNearestInDirection / pressWidget-
            // ByIndex paths can't navigate or select its items. The
            // items live in gDropdown.items[] and the highlighted
            // one in gDropdown.highlightedIndex. We hand-route
            // focus.* / cursor.click / cursor.cancel to those rails
            // when the focused window is the dropdown.
            //
            // Focus-ring drawing is suppressed for dropdowns by the
            // bridge (see WidgetFocus.cpp drawFocusOutlineIfActive)
            // — the dropdown's own per-item highlight is the visual
            // cue, the yellow rectangle around the whole dropdown
            // box would just be noise.
            if (mgr.getFocusedWindowClass() == WindowClass::dropdown)
            {
                // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
                // swallow kCursorClick / kCursorCancel that landed in
                // this process() call because the bootstrap auto-woke
                // the selector mid-frame. The user's physical press
                // happened in cursor mode (selector was hidden) and
                // was already handled by the held-state poll
                // synthesising a virtual mouse click — that virtual
                // click is what opened this very dropdown. The same
                // press is now arriving here via SDL event dispatch
                // because processEvents runs AFTER the bootstrap, and
                // by the time it does the dropdown is focused. Acting
                // on it would commit item 0 on what feels to the user
                // like a single button squeeze. Returning Consumed
                // makes the queued press a no-op for the focus
                // context; the next press the user makes will be
                // observed fresh (auto-woke flag cleared next frame).
                if (mgr.wasAutoWokenThisProcess()
                    && (id == ShortcutId::kCursorClick || id == ShortcutId::kCursorCancel))
                {
                    return Disposition::Consumed;
                }
                const auto dir = directionForShortcut(id);
                if (dir.has_value())
                {
                    // OPENRCT2MINI focus-mode-widgets-plan §3.1 /
                    // Cohort A.3 (2026-05-25): 2D directional walk.
                    // WindowDropdownMoveHighlightDir honours the
                    // dropdown's NumColumns/NumRows/ListVertically
                    // (mirrored to gDropdown by setTextItems /
                    // setImageItems) so up/down moves between rows
                    // and left/right within a row on grid dropdowns
                    // (Change Base Land Style, Footpath Style). For
                    // single-column / single-row dropdowns the
                    // helper degenerates to the legacy linear ±1
                    // walker, preserving existing behaviour for the
                    // Options / About / right-click menus.
                    OpenRCT2::Ui::Windows::WindowDropdownMoveHighlightDir(*dir);
                    return Disposition::Consumed;
                }
                if (id == ShortcutId::kCursorClick)
                {
                    // Commit the highlighted item. If nothing's
                    // highlighted yet (initial state), nudge to the
                    // first selectable item and commit it — feels
                    // less surprising than "first press does
                    // nothing".
                    if (OpenRCT2::Ui::Windows::gDropdown.highlightedIndex < 0)
                        OpenRCT2::Ui::Windows::WindowDropdownMoveHighlight(+1);
                    if (OpenRCT2::Ui::Windows::gDropdown.highlightedIndex >= 0)
                        OpenRCT2::Ui::Windows::WindowDropdownSelectIndex(
                            OpenRCT2::Ui::Windows::gDropdown.highlightedIndex);
                    // OPENRCT2MINI focus-mode-plan §F.15: the
                    // dropdown's onDropdown handler typically opens
                    // a new window (Options, About, …). Snap to
                    // whatever is now topmost so the selector
                    // follows the user's choice, same-frame.
                    mgr.snapFocusToTopmostFocusable();
                    return Disposition::Consumed;
                }
                if (id == ShortcutId::kCursorCancel || id == ShortcutId::kInterfaceDismiss)
                {
                    // Close without selecting. _inputState is reset
                    // to Normal so the next click starts clean.
                    OpenRCT2::Ui::Windows::WindowDropdownClose();
                    OpenRCT2::_inputState = OpenRCT2::InputState::Normal;
                    if (OpenRCT2::gInputFlags.has(OpenRCT2::InputFlag::widgetPressed))
                    {
                        OpenRCT2::gInputFlags.unset(OpenRCT2::InputFlag::widgetPressed);
                    }
                    // OPENRCT2MINI focus-mode-plan §F.16: pop back to
                    // the parent widget that opened the dropdown.
                    if (!mgr.restoreFocus())
                        mgr.snapFocusToTopmostFocusable();
                    return Disposition::Consumed;
                }
                // Other shortcuts pass through while a dropdown is
                // up — the user can still e.g. cycle windows away.
                return Disposition::Passthrough;
            }

            // Translate focus.* to a direction. The four directional
            // bindings are the only ones that drive directional
            // navigation; anything else falls through.
            //
            // OPENRCT2MINI grid-cursor-plan §8.5 (2026-05-25): the
            // full direction-dispatch body lives in dispatchDirection
            // so processFrame's repeat poll can call the same path.
            // Initial press fires through here, notes the press
            // timestamp via _repeat.notePress, then enters
            // dispatchDirection.
            const auto direction = directionForShortcut(id);
            if (direction.has_value())
            {
                _repeat.notePress(static_cast<uint8_t>(*direction), SDL_GetTicks());
                return dispatchDirection(*direction);
            }

            if (id == ShortcutId::kCursorClick)
            {
                // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
                // same swallow as the dropdown branch above. If the
                // selector auto-woke during THIS process() call, any
                // kCursorClick we see came from a press that the user
                // made while the selector was hidden — the held-state
                // poll already dispatched it as a virtual mouse click
                // (which opened whatever new window we just snapped
                // focus onto). Acting on it here would press the
                // newly-focused first widget on top of the click the
                // user already made. The dropdown branch above
                // covers the common dropdown case; this guards every
                // other auto-wake source (a toolbar button that
                // opens About, a context window, etc.).
                if (mgr.wasAutoWokenThisProcess())
                    return Disposition::Consumed;
                auto* w = mgr.getFocusedWindow();
                if (w != nullptr)
                {
                    // OPENRCT2MINI list-focus-plan §2.5: list-mode
                    // activation. When the focused widget is a list-
                    // mode scroll widget AND we have a selected item,
                    // route through scrollFocusActivate which the
                    // window overrides (default impl synthesises an
                    // onScrollMouseDown at the item's centre, so
                    // windows that haven't customised activation still
                    // get reasonable behaviour). Otherwise fall back
                    // to the standard pressWidgetByIndex path.
                    const auto fromWidget = mgr.getFocusedWidget();
                    const auto scrollItem = mgr.getFocusedScrollItem();
                    if (scrollItem >= 0
                        && OpenRCT2::Ui::WidgetFocus::isListModeScroll(*w, fromWidget))
                    {
                        const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(*w, fromWidget);
                        w->scrollFocusActivate(scrollIdx, scrollItem);
                    }
                    else
                    {
                        // pressWidgetByIndex is a safe no-op for null /
                        // out-of-range indices; the strategy doesn't need
                        // to second-guess.
                        OpenRCT2::Ui::WidgetFocus::pressWidgetByIndex(*w, fromWidget);
                    }
                    // OPENRCT2MINI focus-mode-plan §F.15: if that
                    // press opened a new window on top, hand the
                    // selector to it immediately. snapFocusToTopmost-
                    // Focusable is a no-op when the topmost
                    // focusable already matches _focusedWindowClass,
                    // so this is free for buttons that don't open
                    // anything (toggles, in-place actions).
                    mgr.snapFocusToTopmostFocusable();
                }
                return Disposition::Consumed;
            }

            // cursor.cancel: if the focused window has a close
            // box, click it (the standard "close this window"
            // action). Otherwise just exit focus mode. This
            // gives the user a way out of any modal-style window
            // they D-padded into — About, Options, Save/Load,
            // etc. all have close boxes — without destroying
            // permanent-fixture windows like the title logo
            // (which has no close box).
            //
            // kInterfaceDismiss stays as a pure "exit focus
            // mode" — the user-facing ESC key shouldn't double
            // as a window close.
            if (id == ShortcutId::kCursorCancel)
            {
                auto* w = mgr.getFocusedWindow();
                if (w != nullptr)
                {
                    // OPENRCT2MINI cursor-selector-modal-plan §2.2 /
                    // CS-R7: cancel inside a bottom-of-stack set
                    // (in-game toolbar/statusbar) exits the selector
                    // to cursor mode unconditionally — the user is
                    // signalling "I'm done driving with the
                    // selector, give me back the mouse." Skips the
                    // close-box click that follows; chrome windows
                    // shouldn't be closable that way anyway.
                    if (const auto* set = OpenRCT2::Ui::WidgetFocus::findSetFor(w->classification);
                        set != nullptr && set->isBottomOfStack)
                    {
                        mgr.onTransitionEvent(
                            InputManager::SelectorTransitionSource::cursorCancelInBottomSet);
                        return Disposition::Consumed;
                    }
                    const auto closeIdx = findCloseBox(*w);
                    if (closeIdx != OpenRCT2::kWidgetIndexNull)
                    {
                        OpenRCT2::Ui::WidgetFocus::pressWidgetByIndex(*w, closeIdx);
                        // OPENRCT2MINI focus-mode-plan §F.16: prefer
                        // restoreFocus over a fresh snap so the
                        // selector lands on the exact widget the
                        // user was on before opening this window.
                        // Falls back to snap when the stack is empty
                        // or fully stale (e.g. the parent was
                        // closed between push and pop).
                        if (!mgr.restoreFocus())
                            mgr.snapFocusToTopmostFocusable();
                        return Disposition::Consumed;
                    }
                }
                // No close box → exit focus mode instead. Same
                // graceful fallback as dismiss.
                mgr.clearFocus();
                mgr.clearFocusMode();
                return Disposition::Consumed;
            }

            if (id == ShortcutId::kInterfaceDismiss)
            {
                mgr.clearFocus();
                mgr.clearFocusMode();
                return Disposition::Consumed;
            }
            return Disposition::Passthrough;
        }

        ICursorModel* getCursorModel() override
        {
            return &_cursor;
        }

    private:
        // Map focus.up/down/left/right shortcut ids to the
        // WidgetFocus::Direction enum. Returns nullopt for non-
        // directional ids. std::optional keeps the caller's two-
        // case structure (directional path vs everything else)
        // clean — switch-on-enum would force a default branch that
        // does nothing useful.
        //
        // OPENRCT2MINI focus-mode-plan §F.9: matches focus.* now,
        // not cursor.*, so the user can rebind the focus ring and
        // mouse cursor independently — see ShortcutIds.h kFocusUp
        // comment.
        static std::optional<OpenRCT2::Ui::WidgetFocus::Direction> directionForShortcut(std::string_view id)
        {
            if (id == ShortcutId::kFocusUp)
                return OpenRCT2::Ui::WidgetFocus::Direction::up;
            if (id == ShortcutId::kFocusDown)
                return OpenRCT2::Ui::WidgetFocus::Direction::down;
            if (id == ShortcutId::kFocusLeft)
                return OpenRCT2::Ui::WidgetFocus::Direction::left;
            if (id == ShortcutId::kFocusRight)
                return OpenRCT2::Ui::WidgetFocus::Direction::right;
            return std::nullopt;
        }

        // OPENRCT2MINI list-focus-plan §2.6 (hover sync): when focus
        // moves to a list item, synthesise an onScrollMouseOver at the
        // item's centre so the window's existing hover-tracking logic
        // (preview pane in ScenarioSelect, summary text in StaffList,
        // etc.) updates as the user navigates. Uses content-local
        // coordinates because that's the space onScrollMouseOver
        // expects (matches what a real cursor at the item position
        // would produce).
        //
        // No-op if the rect is empty (window opted out of this item)
        // or the widget isn't a scroll. The synthesised call is
        // semantically the same as the mouse hovering over the item
        // for one frame — windows that didn't override onScrollMouseOver
        // simply ignore it.
        static void syncFocusItemHover(
            OpenRCT2::WindowBase& window, OpenRCT2::WidgetIndex scrollWidget, int32_t itemIndex)
        {
            if (scrollWidget == OpenRCT2::kWidgetIndexNull
                || static_cast<size_t>(scrollWidget) >= window.widgets.size())
                return;
            if (window.widgets[scrollWidget].type != OpenRCT2::WidgetType::scroll)
                return;
            const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(window, scrollWidget);
            const auto rect = window.scrollFocusGetItemRect(scrollIdx, itemIndex);
            if (rect.GetLeft() >= rect.GetRight() || rect.GetTop() >= rect.GetBottom())
                return;
            const ScreenCoordsXY centre{
                (rect.GetLeft() + rect.GetRight()) / 2,
                (rect.GetTop() + rect.GetBottom()) / 2,
            };
            window.onScrollMouseOver(scrollIdx, centre);
        }

        // OPENRCT2MINI list-focus-plan §2.3 / C1: compute the on-
        // screen reference point used when seeding a list-mode
        // scroll widget on entry. Preference order:
        //   1. If `widget` is a list-mode scroll widget AND
        //      `scrollItem >= 0`, use the item's on-screen rect
        //      centre — this is the user's actual visual position
        //      and gives perfect lane-tracking when hopping list →
        //      list (e.g. EditorObjectSelection's two scroll widgets).
        //   2. Otherwise, the widget's screen centre. Source widget
        //      is typically a button or another widget the user just
        //      stepped from; its centre is the best available proxy
        //      for "where the user thinks they are."
        // Falls back to (0, 0) for invalid widget indices — the
        // nearestScrollItemTo helper that consumes the result is
        // bounded-search so it won't crash on a far-off reference.
        static ScreenCoordsXY computeListEntryReference(
            const OpenRCT2::WindowBase& window, OpenRCT2::WidgetIndex widget, int32_t scrollItem)
        {
            if (widget == OpenRCT2::kWidgetIndexNull
                || static_cast<size_t>(widget) >= window.widgets.size())
                return { 0, 0 };
            const auto& wd = window.widgets[widget];
            if (scrollItem >= 0 && wd.type == OpenRCT2::WidgetType::scroll
                && OpenRCT2::Ui::WidgetFocus::isListModeScroll(window, widget))
            {
                const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(window, widget);
                const auto cr = const_cast<OpenRCT2::WindowBase&>(window)
                                    .scrollFocusGetItemRect(scrollIdx, scrollItem);
                if (cr.GetLeft() < cr.GetRight() && cr.GetTop() < cr.GetBottom())
                {
                    const auto onScreen = OpenRCT2::Ui::WidgetFocus::contentRectToOnScreen(
                        window, widget, cr);
                    return {
                        (onScreen.GetLeft() + onScreen.GetRight()) / 2,
                        (onScreen.GetTop() + onScreen.GetBottom()) / 2,
                    };
                }
            }
            return {
                static_cast<int32_t>(window.windowPos.x + (wd.left + wd.right) / 2),
                static_cast<int32_t>(window.windowPos.y + (wd.top + wd.bottom) / 2),
            };
        }

        // Locate the closeBox widget on a window. closeBox is the
        // OpenRCT2 widget type for the X-shaped close button in
        // a window's top-right corner — every modal-style window
        // (About, Options, Save/Load, the ride/park windows, …)
        // has one; permanent-fixture windows like the title logo
        // and toolbar don't. Returns kWidgetIndexNull when the
        // window has no close box, letting the caller fall back
        // to "just exit focus mode" instead of trying to close
        // something the user can't close anyway.
        static OpenRCT2::WidgetIndex findCloseBox(const OpenRCT2::WindowBase& window)
        {
            const auto count = window.widgets.size();
            for (size_t i = 0; i < count; i++)
            {
                if (window.widgets[i].type == OpenRCT2::WidgetType::closeBox)
                    return static_cast<OpenRCT2::WidgetIndex>(i);
            }
            return OpenRCT2::kWidgetIndexNull;
        }

        // Cursor model kept from the F.1 skeleton — focus-mode
        // doesn't have a useful state on it (focus index lives on
        // InputManager). A no-op stub is fine.
        TextCaretModel _cursor;
    };

    // OPENRCT2MINI input-plan Track 3 / Phase 3.E: FootpathContext.
    // First concrete ToolContext. Verbs (onPlace / onCancel / onRotate /
    // onRaise / onLower) are stubs at Phase 3.E's first cut — the
    // routing exists, the strategy is registered, the active-context
    // resolver activates it when the Footpath window is up with a
    // tool active. Wiring the verbs to FootpathPlaceAction is the
    // next iteration; for now they Consume the shortcut so it doesn't
    // double-fire as a world-cursor click while the user is in tool
    // mode. The Phase-H pre-decision in input-plan.md §5.2 Phase 3.E
    // confirmed this class fits under 200 lines, so action-map
    // indirection (Phase 3.H) is skipped — the polymorphic-context
    // design carries the rest of the tool-context work.
    // OPENRCT2MINI grid-cursor-plan §14.2 / Phase 3.E.1: FootpathContext
    // verb bodies. Each verb calls into the existing FootpathWindow
    // helpers exposed by Windows.h — onPlace dispatches a
    // FootpathPlaceAction at the grid cursor's tile (via the
    // placeAtTile bridge), onCancel runs the mouse-path remove flow,
    // onRotate rotates the railings cycle, onRaise / onLower adjust
    // the placement Z, onStep walks the grid cursor.
    class FootpathContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolFootpath;
        }

        // OPENRCT2MINI ride-construction-grid-cursor-plan §4 (Phase R,
        // 2026-05-25): head-follow integration. bridgeBuild mode owns
        // a head tile (`_footpathConstructFromPosition`); the rest of
        // the modes don't. Returning the head from getHeadTile() lets
        // ToolContext's per-frame poll catch async updates from Footpath-
        // PlaceAction callbacks and chase the cursor to it (replacing
        // the bespoke _lastBridgeHead poll that used to live in this
        // class's processFrame). headFollowPreservesArrow=true keeps the
        // MapSelectFlag::enableArrow flag set across the sync so the
        // bridge-direction arrow renders at the head.
        std::optional<TileCoordsXY> getHeadTile() const override
        {
            if (Windows::WindowFootpathGetInputMode() != Windows::FootpathInputMode::bridgeBuild)
                return std::nullopt;
            return Windows::WindowFootpathGetBridgeHeadTile();
        }
        bool headFollowPreservesArrow() const override { return true; }
        // OPENRCT2MINI grid-cursor Z-follow (2026-05-31): bridge head's
        // world Z. Mirrors getHeadTile's bridgeBuild gating so the grid
        // cursor highlight + bump-scroll only chase elevation while the
        // user is actively extending a bridge — outside bridgeBuild the
        // tool sits on terrain like a regular path tool.
        std::optional<int32_t> getHeadWorldZ() const override
        {
            if (Windows::WindowFootpathGetInputMode() != Windows::FootpathInputMode::bridgeBuild)
                return std::nullopt;
            return Windows::WindowFootpathGetBridgeHeadWorldZ();
        }

        // OPENRCT2MINI grid-cursor-plan §16: top-level verbs branch
        // on the Footpath window's current PathConstructionMode. Each
        // mode has its own *OnLand / *DragArea / *BridgePick /
        // *BridgeBuild body. The OnLand bodies preserve the §14.2
        // grid-cursor behaviour byte-identically — see the per-method
        // comments below for the per-mode logic.
        Disposition onPlace() override
        {
            switch (Windows::WindowFootpathGetInputMode())
            {
                case Windows::FootpathInputMode::onLand:
                    return onPlaceOnLand();
                case Windows::FootpathInputMode::dragArea:
                    return onPlaceDragArea();
                case Windows::FootpathInputMode::bridgePick:
                    return onPlaceBridgePick();
                case Windows::FootpathInputMode::bridgeBuild:
                    return onPlaceBridgeBuild();
                case Windows::FootpathInputMode::none:
                default:
                    return Disposition::Passthrough;
            }
        }

        // OPENRCT2MINI grid-cursor-plan §16: drag-area + bridge-build
        // override cancel; the other two modes fall through to the
        // ToolContext base which dispatches Viewport-
        // InteractionRightClickAtMapPos at the cursor tile (the
        // OnLand "remove the path under the cursor" behaviour, kept
        // for consistency in bridgePick / drag-area-no-anchor).
        Disposition onCancel() override
        {
            switch (Windows::WindowFootpathGetInputMode())
            {
                case Windows::FootpathInputMode::dragArea:
                    return onCancelDragArea();
                case Windows::FootpathInputMode::bridgeBuild:
                    return onCancelBridgeBuild();
                case Windows::FootpathInputMode::onLand:
                case Windows::FootpathInputMode::bridgePick:
                case Windows::FootpathInputMode::none:
                default:
                    return ToolContext::onCancel();
            }
        }

        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17):
        // finish in grid mode closes the Footpath window outright
        // ("I'm done with this tool"). The toolActive false-edge in
        // process() clears _toolFocusSelected as the window closes.
        Disposition onFinishTool() override
        {
            Windows::WindowFootpathClose();
            return Disposition::Consumed;
        }

        Disposition onRotate() override
        {
            // Rotate the railings cycle (TurnRight) in every mode
            // where it has an effect. The keyboard-shortcut helper
            // already no-ops outside bridgeOrTunnel.
            Windows::WindowFootpathKeyboardShortcutTurnRight();
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §16: onRaise/onLower in
        // bridgeBuild dispatch SlopeUp/SlopeDown (the construction-
        // segment slope verb). All other modes use the grid cursor's
        // Z step (the §14.2 Shift+D-pad-Z behaviour).
        Disposition onRaise() override
        {
            if (Windows::WindowFootpathGetInputMode() == Windows::FootpathInputMode::bridgeBuild)
            {
                Windows::WindowFootpathKeyboardShortcutSlopeUp();
                return Disposition::Consumed;
            }
            return onRaiseGridCursor();
        }

        Disposition onLower() override
        {
            if (Windows::WindowFootpathGetInputMode() == Windows::FootpathInputMode::bridgeBuild)
            {
                Windows::WindowFootpathKeyboardShortcutSlopeDown();
                return Disposition::Consumed;
            }
            return onLowerGridCursor();
        }

        // OPENRCT2MINI grid-cursor-plan §7.4 (amendment 2026-05-17):
        // ghost-tile lifecycle. Chains to the base ToolContext for
        // lifecycle / camera / step bookkeeping, then sets the
        // provisional footpath at the cursor's tile so the ghost
        // renders alongside the highlight. §16: bridgeBuild has no
        // grid cursor / no ghost (the Footpath window's own bridge-
        // head arrow takes over), so skip the provisional set there.
        // dragArea also skips the provisional ghost — the rectangle
        // preview / map selection is what's drawn instead.
        void onActivate() override
        {
            ToolContext::onActivate();
            const auto mode = Windows::WindowFootpathGetInputMode();
            if (mode == Windows::FootpathInputMode::onLand || mode == Windows::FootpathInputMode::bridgePick)
            {
                Windows::WindowFootpathSetProvisionalAtTile(gridCursor().getPosition(), gridCursor().getZ());
            }
            else if (mode == Windows::FootpathInputMode::bridgeBuild)
            {
                // OPENRCT2MINI grid-cursor-plan §16.4f rev 2026-05-21:
                // earlier this branch suppressed MapSelectFlag::grid-
                // Cursor to stop the highlight stacking on the bridge
                // head arrow (the visible-stationary-cursor bug). User
                // feedback: the grid cursor SHOULD be visible — it
                // just needs to follow the bridge head as segments
                // extend / retract. Sync the cursor model + selection
                // to the head here so a Start-toggle re-engage in mid-
                // bridge places the highlight at the head, not at the
                // stale grid-cursor position the base onActivate just
                // wrote.
                //
                // OPENRCT2MINI ride-construction-grid-cursor-plan §4
                // (Phase R, 2026-05-25): syncGridCursorToBridgeHead was
                // hoisted to ToolContext::syncGridCursorToHead which
                // reads via the overridden getHeadTile().
                syncGridCursorToHead();
            }
        }

        void onDeactivate() override
        {
            Windows::WindowFootpathClearProvisional();
            // §16: also clear any drag-area anchor — the user may
            // have anchored, then exited grid cursor without
            // committing. Without this, re-engaging would see a
            // stale anchor and treat the next onPlace as a commit.
            Windows::WindowFootpathDragAreaClear();
            ToolContext::onDeactivate();
        }

        // OPENRCT2MINI grid-cursor-plan §16: onStep branches per mode.
        // bridgeBuild re-purposes the D-pad entirely (slope up/down +
        // turn left/right, per user 2026-05-21). dragArea steps the
        // cursor like OnLand, then refreshes the rectangle preview.
        // bridgePick / OnLand both step + refresh the ghost.
        Disposition onStep(::Direction dpad) override
        {
            switch (Windows::WindowFootpathGetInputMode())
            {
                case Windows::FootpathInputMode::bridgeBuild:
                    return onStepBridgeBuild(dpad);
                case Windows::FootpathInputMode::dragArea:
                    return onStepDragArea(dpad);
                case Windows::FootpathInputMode::onLand:
                case Windows::FootpathInputMode::bridgePick:
                case Windows::FootpathInputMode::none:
                default:
                    return onStepGridCursor(dpad);
            }
        }

        // OPENRCT2MINI grid-cursor-plan §14.4 (2026-05-20 follow-up):
        // after a failed place + popup-dismiss, the latch is now
        // cleared (WindowFootpathClearErrorLatch) but the ghost
        // PlaceAtTilePublic cleared at entry (via
        // FootpathUpdateProvisional) is still gone. Re-arm it here
        // so the user sees the ghost immediately on popup dismiss
        // without having to D-pad to a new tile. §16: only meaningful
        // for OnLand / bridgePick — the other modes don't render a
        // single-tile ghost.
        void onPopupDismissed() override
        {
            if (Windows::WindowFootpathGetInputMode() == Windows::FootpathInputMode::onLand
                || Windows::WindowFootpathGetInputMode() == Windows::FootpathInputMode::bridgePick)
            {
                Windows::WindowFootpathSetProvisionalAtTile(gridCursor().getPosition(), gridCursor().getZ());
            }
        }

        // §11.1: Footpath placement is whole-tile. Earlier revisions
        // had a `precisionSubset() → edges in railings mode` override
        // that called WindowFootpathIsRailingsMode() — but that helper
        // was misleadingly named (actually returned true only in
        // bridgeOrTunnel mode), and edge picking doesn't apply to
        // bridge construction either. Removed 2026-05-24; base
        // default SubsetType::none means the precision modifier is a
        // no-op for Footpath, which matches the tool's actual semantics.

    private:
        // OPENRCT2MINI grid-cursor-plan §16 — OnLand mode bodies
        // (preserved byte-identically from §14.2).

        // §10 placement-Z fix (2026-05-20 update: partial slopes).
        // gridCursor().getZ() is the user-driven Z offset (0 on first
        // entry, bumped by onRaise / onLower). On its own that's
        // below the terrain surface and FootpathPlaceAction rejects
        // the placement as "too low".
        //
        // The mouse path resolves the surface placement Z via
        // FootpathGetOnTerrainPlacement (Footpath.cpp:1133); we do
        // the same here. Unlike TileElementHeight (which returns the
        // slope-interpolated height at a specific XY point), the
        // terrain-placement helper returns the path-corrected base Z
        // — for partial-slope tiles where one corner is raised, it
        // bumps baseZ by kPathHeightStep so the resulting flat path
        // sits on top of the raised corner. Add the cursor's Z
        // offset on top of placement.baseZ for raised-plane (bridge)
        // placements. The 16 floor mirrors `_footpathPlaceZ =
        // std::max(mapZ, 16)` in Footpath.cpp:1108. Falls back to
        // TileElementHeight when FootpathGetOnTerrainPlacement can't
        // resolve the tile.
        Disposition onPlaceOnLand()
        {
            const auto pos = gridCursor().getPosition();
            const auto worldXY = pos.ToCoordsXY();
            const int32_t zOffset = gridCursor().getZ();
            auto placement = OpenRCT2::FootpathGetOnTerrainPlacement(pos);
            int32_t baseZ;
            if (placement.isValid())
                baseZ = std::max<int32_t>(placement.baseZ + zOffset, 16);
            else
                baseZ = std::max<int32_t>(OpenRCT2::TileElementHeight(worldXY) + zOffset, 16);
            Windows::WindowFootpathPlaceAtTile(pos, baseZ);
            return Disposition::Consumed;
        }

        Disposition onRaiseGridCursor()
        {
            gridCursor().raiseZ(OpenRCT2::kPathHeightStep);
            // §17: mark that the user touched Z during this zLock-
            // hold gesture. Release-edge logic in processFrame uses
            // it to decide reset-vs-lock.
            _zAdjustedDuringHold = true;
            refreshProvisionalForCurrentMode();
            return Disposition::Consumed;
        }

        Disposition onLowerGridCursor()
        {
            gridCursor().lowerZ(OpenRCT2::kPathHeightStep);
            _zAdjustedDuringHold = true;
            refreshProvisionalForCurrentMode();
            return Disposition::Consumed;
        }

        // §17 (2026-05-23): after a Z adjustment, refresh the
        // provisional preview so the rendered ghost matches the
        // new Z. onLand and bridgePick use the single-tile ghost
        // (SetProvisionalAtTile); dragArea uses the rectangle
        // preview (DragAreaPreviewAtTile) — only meaningful when
        // an anchor exists. bridgeBuild has its own per-frame
        // preview (driven by the construction-segment slope verbs)
        // so the Z step isn't relevant here.
        void refreshProvisionalForCurrentMode()
        {
            const auto mode = Windows::WindowFootpathGetInputMode();
            const auto pos = gridCursor().getPosition();
            const auto z = gridCursor().getZ();
            if (mode == Windows::FootpathInputMode::onLand
                || mode == Windows::FootpathInputMode::bridgePick)
            {
                Windows::WindowFootpathSetProvisionalAtTile(pos, z);
            }
            else if (mode == Windows::FootpathInputMode::dragArea
                && Windows::WindowFootpathDragAreaHasAnchor())
            {
                Windows::WindowFootpathDragAreaPreviewAtTile(pos, z);
            }
        }

        // OnLand / bridgePick onStep: step the grid cursor, refresh
        // the single-tile ghost. §14.2 Z preservation behaviour.
        Disposition onStepGridCursor(::Direction dpad)
        {
            const auto result = ToolContext::onStep(dpad);
            Windows::WindowFootpathSetProvisionalAtTile(gridCursor().getPosition(), gridCursor().getZ());
            return result;
        }

        // OPENRCT2MINI grid-cursor-plan §16 — DragArea mode bodies.
        // Two-stage gesture: no anchor → onPlace = Anchor; anchored →
        // onPlace = Commit (then back to no anchor). Step extends the
        // selection rectangle live.

        Disposition onPlaceDragArea()
        {
            const auto pos = gridCursor().getPosition();
            const int32_t z = gridCursor().getZ();
            if (Windows::WindowFootpathDragAreaHasAnchor())
            {
                // §17 (2026-05-23): pass the CURRENT cursor Z so
                // the commit honours mid-drag Z adjustments.
                Windows::WindowFootpathDragAreaCommitAtTile(pos, z);
            }
            else
            {
                // §17: capture the anchor's natural baseZ and apply
                // the cursor's accumulated Z offset. Subsequent
                // Preview / Commit calls re-apply zOffset against
                // the cached baseZ so Z changes during the drag
                // update the rectangle.
                Windows::WindowFootpathDragAreaAnchorAtTile(pos, z);
            }
            return Disposition::Consumed;
        }

        // §16: cancel-from-anchored clears the anchor. Cancel-from-
        // idle falls through to the base (right-click-remove at the
        // cursor's tile), per the §16.1 mode table — consistency
        // with OnLand's cancel.
        Disposition onCancelDragArea()
        {
            if (Windows::WindowFootpathDragAreaHasAnchor())
            {
                Windows::WindowFootpathDragAreaClear();
                return Disposition::Consumed;
            }
            return ToolContext::onCancel();
        }

        // §16: step + refresh the rectangle preview when anchored.
        // When idle the cursor just walks; the next anchor-onPlace
        // will pick it up at its new position.
        Disposition onStepDragArea(::Direction dpad)
        {
            const auto result = ToolContext::onStep(dpad);
            if (Windows::WindowFootpathDragAreaHasAnchor())
            {
                // §17 (2026-05-23): pass the cursor's current Z so
                // the rectangle preview reflects any mid-drag Z
                // adjustment.
                Windows::WindowFootpathDragAreaPreviewAtTile(
                    gridCursor().getPosition(), gridCursor().getZ());
            }
            return result;
        }

        // OPENRCT2MINI grid-cursor-plan §16 — bridgePick mode body.
        // The user steps the cursor to choose an anchor tile, then
        // onPlace transitions the Footpath window into bridgeOrTunnel
        // (segment construction) via StartBridgeAtTile.

        Disposition onPlaceBridgePick()
        {
            // OPENRCT2MINI grid-cursor-plan §16.4d bug 2026-05-21 #3:
            // pass the cursor's accumulated Z so a Shift+D-pad-raised
            // pick anchors at the chosen elevation. Lets the user
            // start a bridge from an existing raised path / bridge
            // end rather than always anchoring at ground.
            Windows::WindowFootpathStartBridgeAtTile(gridCursor().getPosition(), gridCursor().getZ());
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §16 — bridgeBuild mode bodies.
        // No cursor in bridge construction — the bridge head position
        // is owned by the Footpath window and rendered as its own
        // arrow / map-selection arrow. D-pad is fully re-purposed
        // per user 2026-05-21: up/down = slope up/down, left/right =
        // turn left/right (counter-clockwise / clockwise).

        Disposition onPlaceBridgeBuild()
        {
            Windows::WindowFootpathKeyboardShortcutBuildCurrent();
            // OPENRCT2MINI grid-cursor-plan §16 follow-up 2026-05-21
            // #4a: BuildCurrent advanced the bridge head; chase the
            // grid cursor up to it so the highlight is at the new
            // head rather than the old anchor.
            // (Phase R 2026-05-25: routes through ToolContext::sync-
            // GridCursorToHead via the overridden getHeadTile().)
            syncGridCursorToHead();
            return Disposition::Consumed;
        }

        Disposition onCancelBridgeBuild()
        {
            Windows::WindowFootpathKeyboardShortcutDemolishCurrent();
            // Demolish retracts the bridge head; same sync as above.
            // Note: if Demolish drops out of bridgeOrTunnel entirely
            // (e.g. removing the anchor), getHeadTile returns nullopt
            // and the helper leaves the cursor where it was.
            syncGridCursorToHead();
            return Disposition::Consumed;
        }

        // D-pad mapping per user 2026-05-21:
        //   up    = SlopeUp     (== onRaise dispatch)
        //   down  = SlopeDown   (== onLower dispatch)
        //   left  = TurnLeft    (counter-clockwise)
        //   right = TurnRight   (clockwise — == onRotate dispatch)
        // The Direction values match the kFocusUp/Right/Down/Left
        // dispatch in InputContextStrategy.h:772-779.
        Disposition onStepBridgeBuild(::Direction dpad)
        {
            switch (static_cast<int>(dpad))
            {
                case 0: // up
                    Windows::WindowFootpathKeyboardShortcutSlopeUp();
                    break;
                case 1: // right
                    Windows::WindowFootpathKeyboardShortcutTurnRight();
                    break;
                case 2: // down
                    Windows::WindowFootpathKeyboardShortcutSlopeDown();
                    break;
                case 3: // left
                    Windows::WindowFootpathKeyboardShortcutTurnLeft();
                    break;
                default:
                    return Disposition::Passthrough;
            }
            // Slope changes the head Z; turns don't move XY but keep
            // the cursor pinned at the head for consistency.
            // (Phase R 2026-05-25: routes through ToolContext::sync-
            // GridCursorToHead via the overridden getHeadTile().)
            syncGridCursorToHead();
            return Disposition::Consumed;
        }

        // OPENRCT2MINI ride-construction-grid-cursor-plan §4 (Phase R,
        // 2026-05-25): the bespoke syncGridCursorToBridgeHead helper
        // and its _lastBridgeHead per-frame poll were hoisted to
        // ToolContext::syncGridCursorToHead + the head-change poll in
        // ToolContext::processFrame. This class now opts in via
        // getHeadTile() (above) + headFollowPreservesArrow() = true.
        // Same behaviour, shared between Footpath and RideConstruction.

        // OPENRCT2MINI follow-up 2026-05-21 #5b: per-frame poll of
        // the bridge head. FootpathPlaceAction's callback updates
        // `_footpathConstructFromPosition` AFTER our onPlace­Bridge­
        // Build's post-Build sync runs (the callback fires inside
        // GameActions::Execute but the timing isn't guaranteed
        // synchronous with our call), so syncing right after the
        // verb sees the stale head. Polling per frame catches the
        // new head whenever it lands without relying on action-
        // callback ordering.
        //
        // OPENRCT2MINI ride-construction-grid-cursor-plan §4 (Phase R,
        // 2026-05-25): the poll was hoisted to ToolContext::process-
        // Frame, which reads via the overridden getHeadTile(). This
        // override now only owns the Footpath-specific Z-hold gesture
        // (which IS Footpath-specific — bridgeBuild's Z comes from
        // SlopeUp/SlopeDown, while onLand/bridgePick/dragArea use
        // gridCursor().getZ() and need the snapshot-reset semantics).
        void processFrame(uint32_t nowMs) override
        {
            ToolContext::processFrame(nowMs);

            // OPENRCT2MINI grid-cursor-plan §17 (2026-05-23): hold-Z
            // gesture press/release edge tracking.
            //
            // Behaviour the user wants (paraphrased): hold the
            // vertical-placement modifier (kInterfaceShiftModifier —
            // PAD Y or Shift on keyboard) and tap D-pad up/down to
            // raise/lower Z. The grid cursor's accumulated Z stays
            // wherever the user left it on release — UNLESS the user
            // released without changing Z (tap the modifier alone),
            // in which case reset Z to 0.
            //
            // Two conditions trigger the reset on release:
            //   1. The user never pressed up/down during the hold
            //      (_zAdjustedDuringHold remains false).
            //   2. The user pressed up/down but the net Z change is
            //      zero (e.g. raised then lowered back to where they
            //      started — currentZ == _zSnapshot).
            // Either condition is a clear "no, I changed my mind"
            // signal; reset.
            //
            // Modifier choice note: this is the SHIFT modifier
            // (vertical placement), NOT the CTRL modifier
            // (kInterfaceConstructionZLock = lock cursor at current
            // Z while moving). Distinct features.
            //
            // bridgeBuild mode doesn't participate — its Z is driven
            // by the construction-segment slope verbs (Up/Down ->
            // KeyboardShortcutSlopeUp/Down), not gridCursor().getZ().
            // The hold-Z gesture is meaningful only when D-pad up/down
            // map to gridCursor Z (onLand, bridgePick, dragArea).
            const bool zLockNow = OpenRCT2::Ui::isShiftModifierHeldInTool();
            if (zLockNow != _zLockWasHeld)
            {
                const auto mode = Windows::WindowFootpathGetInputMode();
                const bool zMeaningful
                    = (mode == Windows::FootpathInputMode::onLand
                       || mode == Windows::FootpathInputMode::bridgePick
                       || mode == Windows::FootpathInputMode::dragArea);
                if (zLockNow && zMeaningful)
                {
                    // Press edge: snapshot the current Z so the
                    // release-edge can compare.
                    _zSnapshot = gridCursor().getZ();
                    _zAdjustedDuringHold = false;
                }
                else if (!zLockNow && zMeaningful)
                {
                    // Release edge: check both reset conditions.
                    const int32_t currentZ = gridCursor().getZ();
                    if (!_zAdjustedDuringHold || currentZ == _zSnapshot)
                    {
                        if (currentZ != 0)
                        {
                            // Reset the cursor model's Z to ground,
                            // then re-emit the provisional so the
                            // ghost matches the reset Z. dragArea
                            // mode doesn't use SetProvisionalAtTile
                            // (it uses rectangle preview); the
                            // anchor's _footpathPlaceZ will track on
                            // the next anchor / preview call.
                            auto* grid = dynamic_cast<GridCursorModel*>(getCursorModel());
                            if (grid != nullptr)
                                grid->setZ(0);
                            if (mode == Windows::FootpathInputMode::onLand
                                || mode == Windows::FootpathInputMode::bridgePick)
                            {
                                Windows::WindowFootpathSetProvisionalAtTile(
                                    gridCursor().getPosition(), 0);
                            }
                        }
                    }
                    _zAdjustedDuringHold = false;
                }
                _zLockWasHeld = zLockNow;
            }

            // OPENRCT2MINI ride-construction-grid-cursor-plan §4
            // (Phase R, 2026-05-25): the bridge-head change poll was
            // hoisted to ToolContext::processFrame (above ToolContext::
            // processFrame's base call already ran via the explicit
            // ToolContext::processFrame() at the top of this function).
            // The base poll reads getHeadTile() — which this class
            // overrides to return WindowFootpathGetBridgeHeadTile in
            // bridgeBuild mode — so the head-chase still happens; it
            // just lives in the base now and is shared with
            // RideConstruction.
        }

    private:
        // OPENRCT2MINI grid-cursor-plan §17 (2026-05-23): zLock
        // hold-Z gesture state machine. _zLockWasHeld is the prev-
        // frame value used for edge detection; _zSnapshot is the
        // Z value at the press edge; _zAdjustedDuringHold is set
        // by onRaise/onLowerGridCursor when the user actually presses
        // up/down during the hold.
        bool _zLockWasHeld = false;
        int32_t _zSnapshot = 0;
        bool _zAdjustedDuringHold = false;
    };

    // OPENRCT2MINI grid-cursor-plan §11.2 / §11.3 (2026-05-24 revision):
    // earlier sketch had Terrain / Water sharing an EdgeToolBase +
    // EdgeCursorModel (default orientation corner0 / edge0). Shipped
    // design (per user 2026-05-24 Q3 + §11.2/§11.3 rewrite) uses the base
    // GridCursorModel for both:
    //   - Terrain selects a whole-tile by default; precision modifier
    //     picks one of the four corners via SubsetType::corners.
    //   - Water is whole-tile only; no sub-tile precision (vanilla has
    //     no partial-edge water). Default orientation MapSelectType::full.
    // EdgeCursorModel remains in the header for a future Wall / Banner
    // tool that needs per-edge primitives, but no production context
    // uses it today.
    class TerrainContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolTerrain;
        }
        // §11.2 (re-revised 2026-05-24 after user empirical clarification):
        // precision modifier on Land tool picks BOTH diamond vertices
        // (cardinal D-pad → corner sprite, matches mouse Land tool's
        // corner highlight) AND diamond sides (diagonal D-pad chord →
        // edge sprite, matches the side-of-diamond highlight). The
        // `corners` subset handles both halves of that pair — see
        // ToolContext::onPrecisionDpad. Same visual decoration as the
        // mouse-driven Land tool.
        //
        // Default orientation (GridCursorModel ctor) is
        // MapSelectType::full — whole-tile, matching the mouse path's
        // small-size default.
        //
        // §11.2 follow-up (2026-05-24, paint mode): when the user has
        // toggled the paint-landscape mode button on, the mouse path
        // forces MapSelectType::full and ignores corner/edge selection
        // (Land.cpp ToolUpdateLandPaint:515-576). Mirror that: report
        // precisionSubset = none so the precision picker no-ops,
        // keeping the cursor in whole-tile mode where paint expects it.
        //
        // §18.3 follow-up (2026-05-24): the size > 1 gate is hoisted
        // to base ToolContext::precisionSubset() — we only handle the
        // paint-mode gate here. The base returns SubsetType::none
        // whenever usesGLandToolSize() is true AND gLandToolSize > 1.
        SubsetType precisionSubsetForTool() const override
        {
            return Windows::WindowLandIsPaintMode() ? SubsetType::none : SubsetType::corners;
        }

        // §18.4.e (2026-05-24): opt in to the multi-cell brush via
        // gLandToolSize. Combined with the base ToolContext::precision-
        // Subset() wrapper, holding precision + D-pad at size > 1
        // becomes a no-op; grid cursor writes an NxN rect instead of
        // A == B == single tile.
        bool usesGLandToolSize() const override { return true; }

        // OPENRCT2MINI grid-cursor-plan §11.2 follow-up (2026-05-24,
        // paint-mode user feedback "shouldn't disable directional
        // inputs while the activation button is held"): in paint mode
        // the cursor.click-held + D-pad gesture should keep stepping
        // the cursor, not divert to onRaise/onLower. Mirrors the mouse
        // path of paint: onToolDrag fires SurfaceSetStyleAction on
        // every tick as the mouse pans, so panning across tiles to
        // paint a strip is the natural workflow. Outside paint mode
        // (raise/lower / mountain), keep the default consume so PAD A
        // held + D-pad up/down remains the raise/lower modifier.
        bool consumeDirectionalsWhenCursorClickHeld() const override
        {
            return !Windows::WindowLandIsPaintMode();
        }

        // OPENRCT2MINI grid-cursor-plan §11.2 follow-up (2026-05-24,
        // mode-parity correction): cursor.click ALWAYS dispatches
        // SurfaceSetStyleAction, regardless of mode. Mirrors the
        // mouse path's onToolDown (Land.cpp:533-554) — the initial
        // click paints in every mode; the mode only branches the
        // follow-up drag behaviour:
        //   - paint mode: onToolDrag chains more SurfaceSetStyle
        //     dispatches (gamepad: TerrainContextImpl::onStep below
        //     re-paints on each PAD-A-held D-pad step)
        //   - raise/lower (and mountain): onToolDrag calls
        //     LandToolDrag → raise/lower per mouse Y movement
        //     (gamepad: PAD A held + D-pad up/down routes through
        //     ToolContext::onShortcut's cursor.click-held block to
        //     onRaise / onLower).
        // SurfaceSetStyleAction is a no-op when neither
        // gLandToolTerrainSurface nor gLandToolTerrainEdge is set, so
        // calling it unconditionally costs nothing when the user
        // hasn't picked floor / wall textures.
        Disposition onPlace() override
        {
            Windows::WindowLandPaintAtCursor();
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §11.2 follow-up (2026-05-24,
        // paint-mode drag-paint): when paint mode is engaged AND the
        // user is holding PAD A while stepping the cursor, dispatch
        // SurfaceSetStyleAction at every new tile. Mirrors the mouse
        // path's onToolDrag chain of paints during a hold-and-drag
        // gesture. The base onStep updates the cursor position and
        // re-writes gMapSelectPositionA/B via GridCursorModel::step
        // → WriteGridCursorSelection, so by the time we get here the
        // selection globals already point at the destination tile.
        Disposition onStep(::Direction dpad) override
        {
            const auto result = ToolContext::onStep(dpad);
            if (Windows::WindowLandIsPaintMode() && isCursorClickHeldInTool())
                Windows::WindowLandPaintAtCursor();
            return result;
        }

        // OPENRCT2MINI grid-cursor-plan §11.2 (2026-05-24): onRaise /
        // onLower dispatch the canonical LandRaise / LandLower game
        // actions at the cursor's tile + selected sub-tile orientation
        // (gMapSelectType, set by the precision picker or default
        // MapSelectType::full). Triggered by:
        //   - cursor.click held + D-pad up/down (PAD A as modifier — see
        //     ToolContext::onShortcut's cursor.click-held block)
        //   - shift modifier + D-pad up/down (inherited Z-adjust gesture,
        //     same dispatch path — works on Land too even though there's
        //     no Z preview)
        // Either way the user gets a single raise/lower step. The
        // existing mouse Land tool's logic (LandWindow::SelectionRaise/
        // LowerLand) takes the same path; we just bypass the LandWindow
        // instance method by calling Windows::WindowLandRaiseAtCursor.
        //
        // §11.2 follow-up (2026-05-24, mountain mode): the WindowLand*
        // helpers themselves branch on WindowLandIsMountainMode and
        // swap to LandSmoothAction when mountain mode is engaged — the
        // gamepad path doesn't need a per-mode override here.
        //
        // §11.2 follow-up (2026-05-24, paint mode user feedback): paint
        // mode and raise/lower are mutually exclusive in the mouse
        // path — Land.cpp's onToolDown/onToolDrag dispatches
        // SurfaceSetStyleAction when _landToolPaintMode is set and
        // never calls SelectionRaise/LowerLand in that branch. Mirror
        // that here: when paint mode is engaged, the cursor.click-
        // held + D-pad up/down gesture (and the shift-modifier Z-
        // adjust gesture, which routes through the same onRaise /
        // onLower hooks) is a Consumed no-op so the user can't
        // accidentally terrain-edit while painting.
        Disposition onRaise() override
        {
            if (Windows::WindowLandIsPaintMode())
                return Disposition::Consumed;
            Windows::WindowLandRaiseAtCursor();
            return Disposition::Consumed;
        }
        Disposition onLower() override
        {
            if (Windows::WindowLandIsPaintMode())
                return Disposition::Consumed;
            Windows::WindowLandLowerAtCursor();
            return Disposition::Consumed;
        }
    };

    class WaterContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolWater;
        }
        // §11.3: water is whole-tile only; precisionSubset() inherits
        // the base default SubsetType::none so the precision modifier
        // is a no-op.
        //
        // §18.C (2026-05-24): opt in to the multi-cell brush via
        // gLandToolSize — the Water window already exposes the same
        // size DEC/INC/preview triplet the Land window does, and the
        // WaterRaise/WaterLower actions accept a MapRange natively
        // (see Water.cpp:266-289 mouse path). The base ToolContext's
        // per-frame poll (processFrame) sets GridCursorModel::_brushSize
        // from gLandToolSize, and step() routes through the rect writer
        // when brushSize > 1; nothing else needs doing here.
        bool usesGLandToolSize() const override { return true; }

        // §18.C: verb dispatch. Cursor.click held + D-pad up/down (PAD A
        // as modifier — handled by ToolContext::onShortcut's cursor.click-
        // held block) and the shift-modifier Z-adjust gesture both route
        // through onRaise/onLower. Both hooks call into the Water window
        // helpers, which dispatch WaterRaiseAction / WaterLowerAction
        // against the current map selection rect (set by
        // WriteGridCursorSelection — single-tile A == B at size 1, NxN
        // rect at size > 1).
        Disposition onRaise() override
        {
            Windows::WindowWaterRaiseAtCursor();
            return Disposition::Consumed;
        }
        Disposition onLower() override
        {
            Windows::WindowWaterLowerAtCursor();
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §11.3 retroactive polish
        // (2026-05-24, surfaced by the §11.5.1 / §11.9.1 sanity check):
        // refresh the cost preview after every cursor step and on
        // re-engage, mirroring the mouse onToolUpdate cost recompute.
        // Same shape as LandRights / ClearScenery.
        Disposition onStep(::Direction dpad) override
        {
            const auto result = ToolContext::onStep(dpad);
            Windows::WindowWaterRefreshCost();
            return result;
        }

        void onActivate() override
        {
            ToolContext::onActivate();
            Windows::WindowWaterRefreshCost();
        }

        // §18.C follow-up (2026-05-24, visual parity): the mouse Water
        // tool sets gMapSelectType = MapSelectType::fullWater on every
        // tool update (Water.cpp:354-358) so the selection diamond
        // renders with the blue water tint instead of the white land
        // tint. Mirror that on the grid cursor by overriding the
        // per-tool default orientation — ToolContext::onActivate seeds
        // the GridCursorModel from this, and the precision tap-alone
        // reset routes through this too so the tint survives a
        // precision-modifier tap.
        MapSelectType defaultMapSelectType() const override
        {
            return MapSelectType::fullWater;
        }

        // §18.C follow-up (2026-05-24, visual parity): park the cursor
        // sprite on the water surface, not on the terrain bed under
        // the water. SyncHiddenCursorParking adds the return value to
        // the cursor model's accumulated Z when projecting. For tiles
        // with water, return the delta `waterHeight - landHeight`
        // (positive) so the projection ends up at the water surface.
        // For dry tiles waterHeight is 0 from TileElementWaterHeight,
        // which would clamp to a negative delta — guard with max(0).
        // The caller passes the rect-centre world coord for multi-cell
        // brushes, so a brush partially over water still uses the
        // centre tile's water height as the parking reference.
        int32_t cursorParkZExtra(CoordsXY worldCoord) const override
        {
            const auto centre = worldCoord + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
            const int32_t land = OpenRCT2::TileElementHeight(centre);
            const int32_t water = OpenRCT2::TileElementWaterHeight(centre);
            if (water <= land)
                return 0;
            return water - land;
        }
    };

    // OPENRCT2MINI input-plan Track 3 / Phase 3.G: remaining tool
    // contexts. Scenery + LandRights are tile-aligned (GridCursorModel
    // inherited from ToolContext); TileInspector likewise selects a
    // cell. RideConstruction (covers Track + Maze + custom track) has
    // its own placement grammar — previous-segment-constrained
    // orientation, piece-type catalog navigation — which lives in the
    // verb-wiring follow-up. The skeleton ships the strategy slots so
    // the active-context resolver routes correctly; bodies are TODO.
    class SceneryContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolScenery;
        }
        // OPENRCT2MINI grid-cursor-plan §11.4 Step B rework (2026-05-31):
        // precision modifier is no longer the quadrant picker for non-
        // full-tile SmallScenery. The cursor now moves at half-tile
        // granularity (4 quadrant-stops per tile, see onStep override)
        // so the user gets quadrant selection for free as part of D-pad
        // navigation — no modifier needed.
        //
        // Wall items (Step G, upcoming) will use SubsetType::edges
        // since walls genuinely need a separate gesture for the side
        // picker — there's no half-tile-cursor equivalent for "which
        // side of the tile".
        //
        // §11.4 Step F amendment (2026-06-01): Banner ALSO gets
        // SubsetType::edges because banners attach to one side of a
        // footpath — same shape as wall side selection. Holding
        // precision + diagonal D-pad picks the side directly; tap-
        // alone D-pad still steps the cursor between tiles. When the
        // user hasn't picked a side, the banner ghost falls back to
        // gWindowSceneryRotation (the user can still use tool.rotate
        // for cardinal cycle if preferred).
        //
        // §18.4.e.1 (2026-05-24): subclass hook renamed
        // precisionSubsetForTool; base ToolContext::precisionSubset()
        // wraps it with the size > 1 gate.
        SubsetType precisionSubsetForTool() const override
        {
            const auto sel = Windows::WindowSceneryGetTabSelection();
            if (!sel.IsUndefined() && sel.SceneryType == SCENERY_TYPE_BANNER)
                return SubsetType::edges;
            return SubsetType::none;
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step B polish (2026-05-31):
        // shift the virtual cursor sprite to the centre of the currently-
        // selected sub-quadrant instead of the tile centre. Quadrant
        // centres are at tile-NW + (8 or 24, 8 or 24); ViewportInteraction-
        // MapToScreen adds kCoordsXYHalfTile (=16) internally to project
        // NW corner → tile centre, so to land on the SUB-cell centre we
        // need a (±8, ±8) offset relative to the input.
        //   _quarterPhase bit 0 (E side) → +8 on Y axis (east); 0 → -8
        //   _quarterPhase bit 1 (S side) → +8 on X axis (south); 0 → -8
        // No-op for full-tile / scatter / paint / eyedrop modes
        // (gate matches the same condition that engages half-tile
        // navigation in onStep, so the sprite stays centred when the
        // navigation does).
        CoordsXY cursorParkXYExtra() const override
        {
            if (!Windows::WindowSceneryCurrentItemIsNonFullTileSmall())
                return { 0, 0 };
            const int32_t dx = (_quarterPhase & 0b10) ? +8 : -8; // south (+X) vs north
            const int32_t dy = (_quarterPhase & 0b01) ? +8 : -8; // east (+Y) vs west
            return { dx, dy };
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 (Step A, 2026-05-31):
        // dispatch SmallSceneryPlaceAction at the cursor's tile +
        // quadrant + rotation + Z. The free function in Scenery.cpp
        // reads the placement globals kept current by our
        // RefreshGhostAtCursor pump (onActivate + onStep below).
        // Mirrors WindowLandPaintAtCursor's shape (§11.2). Step A
        // scope is SmallScenery only; paint / eyedropper modes and
        // the other four scenery types early-return inside the helper.
        Disposition onPlace() override
        {
            Windows::WindowSceneryPlaceAtCursor();
            // Refresh the ghost immediately after place — placement
            // consumed the SCENERY_GHOST_FLAG_0 marker (the action
            // succeeded and committed) so the next-frame ghost needs
            // a fresh dispatch to show. Otherwise the cursor would
            // sit "naked" on the next tile until movement re-fires
            // onStep.
            Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step A (2026-05-31): the
        // mouse-driven onToolUpdate dispatcher that normally keeps
        // the scenery ghost current is gated OFF in grid cursor mode
        // (MouseInput.cpp:1490-1526 — three early-return checks for
        // isToolFocusSelected, gridCursorParked, gridCursor). So the
        // grid cursor has to drive the ghost itself: onActivate to
        // paint the initial ghost when the user enters grid cursor
        // mode with the scenery tool armed, onStep to chase the ghost
        // to the new tile every time the cursor moves. Mirrors
        // FootpathContextImpl::onActivate / onStep which solve the
        // same problem for the path ghost via
        // WindowFootpathSetProvisionalAtTile.
        void onActivate() override
        {
            ToolContext::onActivate();
            Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step B rework (2026-05-31):
        // half-tile cursor for non-full-tile SmallScenery. Each D-pad
        // press moves the cursor by ONE QUADRANT (= half a tile in one
        // axis) instead of a full tile. Tile boundaries are crossed
        // smoothly as the quadrant phase rolls over from one side to
        // the other.
        //
        // Per-axis phase encoding (in _quarterPhase):
        //   bit 0 (0b01): east side (1) vs west side (0) of current tile
        //   bit 1 (0b10): south side (1) vs north side (0) of current tile
        //
        // For each D-pad direction (after camera-rotation translation
        // to world direction):
        //   N (move toward -X world): if already on north side → step
        //     tile N and become south side of new tile; else just flip
        //     the south bit to 0 (move from south half to north half of
        //     same tile).
        //   Similarly for S/E/W.
        //
        // Full-tile items + paint/eyedrop/scatter modes pass through to
        // the base onStep (one tile per press, existing behaviour).
        // Step A's ghost-refresh-after-step contract is preserved by
        // calling RefreshGhostAtCursorPublic at the end of both
        // branches.
        //
        // The quadrant highlight (gMapSelectType) is updated to match
        // the current phase via quarterPhaseToMapSelect(); the existing
        // RefreshGhostAtCursorPublic derivation (quadrant = direction ^ 2)
        // picks up the new quadrant for both the visible ghost and the
        // PAD-A dispatch. Drops the Step B precision-picker pathway —
        // no modifier needed anymore for quadrant selection.
        Disposition onStep(::Direction dpad) override
        {
            if (!Windows::WindowSceneryCurrentItemIsNonFullTileSmall())
            {
                const auto result = ToolContext::onStep(dpad);
                Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
                return result;
            }

            // Half-tile mode: translate screen-relative dpad to world
            // direction. Mirrors the canonical rotation transform from
            // stepForDirection's compass mode (InputContextStrategy.cpp:
            // 538-552) — at rot=0 dpad UP=N, at rot=1 dpad UP=W (the
            // camera rotated CCW so the world's W edge is now at the
            // top of the screen), at rot=2 dpad UP=S, at rot=3 dpad
            // UP=E. The formula `(dpad + 4 - rot) & 3` matches the
            // table+Rotate((4-r)&3) shape — equivalent because we're
            // working with direction indices instead of TileCoordsXY
            // deltas. Earlier code had this inverted as `(dpad + rot)
            // & 3`, which sent the cursor the WRONG way at every
            // non-zero camera rotation.
            const uint8_t rot = OpenRCT2::GetCurrentRotation() & 3;
            const uint8_t worldDir = (static_cast<uint8_t>(dpad) + 4 - rot) & 3;

            bool stepCursor = false;
            switch (worldDir)
            {
                case 0: // N: south→north within tile, or step tile N
                    if (_quarterPhase & 0b10)
                        _quarterPhase &= ~0b10;
                    else
                    {
                        _quarterPhase |= 0b10;
                        stepCursor = true;
                    }
                    break;
                case 2: // S
                    if (!(_quarterPhase & 0b10))
                        _quarterPhase |= 0b10;
                    else
                    {
                        _quarterPhase &= ~0b10;
                        stepCursor = true;
                    }
                    break;
                case 1: // E
                    if (!(_quarterPhase & 0b01))
                        _quarterPhase |= 0b01;
                    else
                    {
                        _quarterPhase &= ~0b01;
                        stepCursor = true;
                    }
                    break;
                case 3: // W
                    if (_quarterPhase & 0b01)
                        _quarterPhase &= ~0b01;
                    else
                    {
                        _quarterPhase |= 0b01;
                        stepCursor = true;
                    }
                    break;
            }

            if (stepCursor)
            {
                // Cursor moves to the adjacent tile in the world
                // direction the user pressed. The base onStep handles
                // camera follow + bump scroll + cursor model update.
                ToolContext::onStep(dpad);
            }

            // Sync orientation to the new phase. The cursor model
            // owns the canonical orientation; gMapSelectType is its
            // serialised form (written by WriteGridCursorSelection
            // during the base onStep call). For the non-stepping
            // branch we write gMapSelectType directly since base onStep
            // didn't run.
            const auto orientation = quarterPhaseToMapSelect(_quarterPhase);
            if (auto* model = getCursorModel(); model != nullptr)
            {
                if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                    grid->setOrientation(orientation);
            }
            gMapSelectType = orientation;

            Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step C (2026-05-31): Z-stack
        // gesture for stackable SmallScenery items. The user holds the
        // shift modifier (kInterfaceShiftModifier — PAD Y on gamepad,
        // Shift on keyboard) and taps D-pad up/down to raise/lower the
        // placement Z plane. SmallSceneryFlag::isStackable gates the
        // gesture — non-stackable items (most benches, lamps) have no
        // Z choice in the mouse path either (Scenery.cpp:2522).
        //
        // Z lives in gSceneryPlaceZ (a global written by the existing
        // ghost machinery), not the cursor model's Z, because the
        // mouse path's ghost helpers + place actions all read
        // gSceneryPlaceZ. Direct writes keep us byte-compatible with
        // the mouse path's placement semantics.
        //
        // Step size: kCoordsZStep (8 world units) per press, matching
        // the mouse path's `gSceneryPlaceZ += 8` retry-loop step
        // (Scenery.cpp:1890). Clamp ≥ 0 — SmallScenery doesn't
        // support underground placement, unlike RideConstruction.
        //
        // refresh ghost after every Z change so the visible ghost
        // floats at the new plane immediately.
        // OPENRCT2MINI grid-cursor-plan §11.4 Step C fix (2026-05-31):
        // raise/lower drive the OFFSET above terrain, not the absolute
        // Z. The ghost helper recomputes absolute Z = terrain + offset
        // on every refresh, so the placement floats consistently above
        // terrain as the cursor traverses tiles. Mirrors mouse-path
        // accumulator gSceneryShiftPressZOffset semantics.
        Disposition onRaise() override
        {
            if (!Windows::WindowSceneryCurrentItemIsStackable())
                return Disposition::Consumed;
            // Cap the offset at the same headroom the mouse path uses
            // for maxPossibleHeight; the place action will reject
            // anything that overshoots for the particular item.
            constexpr int16_t kOffsetMax = (255 - 4) * kCoordsZStep;
            gSceneryShiftPressZOffset = static_cast<int16_t>(std::min<int32_t>(
                gSceneryShiftPressZOffset + kCoordsZStep, kOffsetMax));
            gSceneryShiftPressed = true;
            _zAdjustedDuringHold = true;
            Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
            return Disposition::Consumed;
        }

        Disposition onLower() override
        {
            if (!Windows::WindowSceneryCurrentItemIsStackable())
                return Disposition::Consumed;
            gSceneryShiftPressZOffset = static_cast<int16_t>(std::max<int32_t>(
                gSceneryShiftPressZOffset - kCoordsZStep, 0));
            // Drop the shift-pressed flag when the offset returns to 0
            // so the ghost helper takes the "Z=0, action places at
            // terrain" branch instead of "terrain + 0" (which is
            // surface, same result, but match the mouse path's
            // convention precisely).
            gSceneryShiftPressed = gSceneryShiftPressZOffset > 0;
            _zAdjustedDuringHold = true;
            Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
            return Disposition::Consumed;
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step C (2026-05-31):
        // shift-modifier press/release edge detection for the
        // snapshot-reset semantics. Mirrors FootpathContextImpl::
        // processFrame's _zLockWasHeld pattern (InputManager.cpp:1758
        // -1802), adapted to write gSceneryPlaceZ instead of the
        // cursor model's Z.
        //
        // Behaviour:
        //   - Press edge: snapshot gSceneryPlaceZ, clear the
        //     adjusted-during-hold flag.
        //   - Release edge: if the user never adjusted Z during the
        //     hold (tap-alone) OR the net Z change is zero (raised
        //     then lowered back to where they started), reset Z to 0.
        //     Otherwise keep the elevated Z (sticky placement plane).
        //
        // The "tap-alone reset to ground" branch is the discoverable
        // way out of an elevated plane: hold shift, release without
        // pressing anything → back to ground. Matches Footpath.
        void processFrame(uint32_t nowMs) override
        {
            ToolContext::processFrame(nowMs);

            // Gate on stackability so the state machine doesn't churn
            // for non-stackable items where the gesture is a no-op
            // anyway. (Tap-alone reset is also meaningless in that
            // case because gSceneryPlaceZ is forced to 0 by the
            // earlier item selection.)
            if (!Windows::WindowSceneryCurrentItemIsStackable())
            {
                _zLockWasHeld = false;
                _zAdjustedDuringHold = false;
                return;
            }

            const bool zLockNow = OpenRCT2::Ui::isShiftModifierHeldInTool();
            if (zLockNow != _zLockWasHeld)
            {
                if (zLockNow)
                {
                    // Press edge — snapshot the OFFSET so we can detect
                    // net-zero changes on release.
                    _zSnapshot = gSceneryShiftPressZOffset;
                    _zAdjustedDuringHold = false;
                }
                else
                {
                    // Release edge — if user did tap-alone OR raised/
                    // lowered net-zero, reset elevation to ground.
                    const int16_t currentOffset = gSceneryShiftPressZOffset;
                    if ((!_zAdjustedDuringHold || currentOffset == _zSnapshot) && currentOffset != 0)
                    {
                        gSceneryShiftPressZOffset = 0;
                        gSceneryShiftPressed = false;
                        Windows::WindowSceneryRefreshGhostAtCursor(currentCursorTileNw());
                    }
                    _zAdjustedDuringHold = false;
                }
                _zLockWasHeld = zLockNow;
            }
        }

    private:
        // OPENRCT2MINI grid-cursor-plan §11.4 Step D fix (2026-05-31):
        // helper to grab the cursor's tile NW corner from the cursor
        // model. Avoids reading gMapSelectPositionA which gets expanded
        // into a scatter rect — using A as both input (centre) and
        // output (rect) caused the rect to drift NW on every refresh.
        // The cursor model is the canonical source of truth.
        CoordsXY currentCursorTileNw()
        {
            if (auto* model = getCursorModel(); model != nullptr)
            {
                if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                    return grid->getPosition().ToCoordsXY();
            }
            // Fallback when the cursor model isn't a grid model — read
            // gMapSelectPositionA (single-tile case where this is the
            // cursor tile NW corner).
            return { gMapSelectPositionA.x, gMapSelectPositionA.y };
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step B rework (2026-05-31):
        // half-tile cursor phase for non-full-tile SmallScenery. Two bits
        // encoding which quadrant of the current tile the cursor sits in:
        //   bit 0 (0b01) = east side (1) vs west (0)
        //   bit 1 (0b10) = south side (1) vs north (0)
        // So _quarterPhase values map to corners:
        //   0b00 (0) = NW corner
        //   0b01 (1) = NE corner
        //   0b10 (2) = SW corner
        //   0b11 (3) = SE corner
        // Default 0 (NW) is the cursor's "first quadrant" when entering
        // non-full-tile mode; user steps from there via D-pad.
        uint8_t _quarterPhase = 0;

        // OPENRCT2MINI grid-cursor-plan §11.4 Step B rework (2026-05-31):
        // map a phase value to the matching MapSelectType for the
        // visible quadrant highlight. The mapping comes from the
        // precision picker's mod_x/mod_y analysis in
        // InputContextStrategy.cpp:1271-1284:
        //
        //   mod_x <= 16 (N half) && mod_y < 16 (W half) → direction 2
        //   mod_x <= 16 (N half) && mod_y >= 16 (E half) → direction 3
        //   mod_x >  16 (S half) && mod_y < 16 (W half) → direction 1
        //   mod_x >  16 (S half) && mod_y >= 16 (E half) → direction 0
        //
        // So:
        //   quarter0 = SE (S half + E half)
        //   quarter1 = SW (S half + W half)   ← NOT NE
        //   quarter2 = NW (N half + W half)
        //   quarter3 = NE (N half + E half)   ← NOT SW
        //
        // Phase encoding (bit 0 = E, bit 1 = S):
        //   phase 0 = NW (N, W) → quarter2
        //   phase 1 = NE (N, E) → quarter3
        //   phase 2 = SW (S, W) → quarter1
        //   phase 3 = SE (S, E) → quarter0
        //
        // Earlier code had quarter1/quarter3 swapped (labelled them NE/
        // SW respectively when they actually represent SW/NE). With the
        // swap, pressing DOWN at rot=0 cycled the visible highlight
        // NW→NE→(S-neighbour)NW→NE rather than the intended NW→SW→
        // (S-neighbour)NW→SW. User report 2026-05-31: "Pressing down
        // cycles through N, E of that cell, then goes to the cell to
        // the S, and cycles through N, E of that cell" — exactly the
        // swap signature.
        //
        // The existing RefreshGhostAtCursorPublic derivation (quadrant
        // = picked_direction ^ 2) then maps these back to the quadrant
        // index SmallSceneryPlaceAction expects.
        static MapSelectType quarterPhaseToMapSelect(uint8_t phase)
        {
            static constexpr MapSelectType kMap[4] = {
                MapSelectType::quarter2, // phase 0 = NW
                MapSelectType::quarter3, // phase 1 = NE
                MapSelectType::quarter1, // phase 2 = SW
                MapSelectType::quarter0, // phase 3 = SE
            };
            return kMap[phase & 3];
        }

        // OPENRCT2MINI grid-cursor-plan §11.4 Step C (2026-05-31):
        // shift-modifier hold-Z gesture state machine.
        //   _zLockWasHeld: prev-frame value for press/release edge
        //     detection.
        //   _zSnapshot: gSceneryPlaceZ at the press edge; release
        //     compares against current Z to detect net-zero changes.
        //   _zAdjustedDuringHold: set true by onRaise/onLower when
        //     the user actually presses up/down during the hold.
        //     False at release-edge = tap-alone = reset to ground.
        bool _zLockWasHeld = false;
        int16_t _zSnapshot = 0;
        bool _zAdjustedDuringHold = false;
    };

    class LandRightsContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolLandRights;
        }

        // OPENRCT2MINI grid-cursor-plan §11.5 / §18.C (2026-05-24): wire
        // LandRights to the multi-cell brush. Same opt-in shape as Land
        // (§18.A) and Water (§18.C). Per-tool visual + verb deltas:
        //
        //   - fullLandRights highlight tint (mirrors Land Rights.cpp:491-495)
        //   - Water-aware cursor parking (the mouse path's
        //     GetMapCoordinatesFromPos at LandRights.cpp:469 includes
        //     ViewportInteractionItem::water, i.e. underwater tiles are
        //     valid targets, so the sprite should float on water
        //     rather than sit on the bed beneath it — same logic as
        //     WaterContextImpl::cursorParkZExtra)
        //   - onPlace dispatches via the new WindowLandRightsApplyAtCursor
        //     helper which branches on IsOwnershipMode() to pick
        //     LandSetRights vs LandBuyRights (mirrors mouse onToolDown
        //     at LandRights.cpp:562-578)
        //   - onStep/onActivate refresh the cost preview so the
        //     LandRights window's cost line stays current as the user
        //     steps the brush or resizes it (§11.5.1 — replaces the
        //     mouse path's per-onToolUpdate cost recompute, which
        //     never fires in pure grid-cursor flow)
        bool usesGLandToolSize() const override { return true; }

        MapSelectType defaultMapSelectType() const override
        {
            return MapSelectType::fullLandRights;
        }

        int32_t cursorParkZExtra(CoordsXY worldCoord) const override
        {
            const auto centre = worldCoord + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
            const int32_t land = OpenRCT2::TileElementHeight(centre);
            const int32_t water = OpenRCT2::TileElementWaterHeight(centre);
            if (water <= land)
                return 0;
            return water - land;
        }

        // OPENRCT2MINI grid-cursor-plan §11.5 follow-up (2026-05-24,
        // user request): drag-buy chain — holding PAD A while
        // D-padding continues to dispatch the buy/set action at each
        // new tile. Mouse path already does this: onToolDrag at
        // LandRights.cpp:580-583 simply re-fires onToolDown on every
        // mouse-move-while-held tick. Same shape as
        // ClearSceneryContextImpl + Land paint mode (no
        // raise/lower verb on LandRights, so returning false keeps
        // D-pad stepping the cursor instead of diverting to a no-op).
        bool consumeDirectionalsWhenCursorClickHeld() const override { return false; }

        Disposition onPlace() override
        {
            Windows::WindowLandRightsApplyAtCursor();
            return Disposition::Consumed;
        }

        Disposition onStep(::Direction dpad) override
        {
            const auto result = ToolContext::onStep(dpad);
            if (isCursorClickHeldInTool())
                Windows::WindowLandRightsApplyAtCursor();
            Windows::WindowLandRightsRefreshCost();
            return result;
        }

        void onActivate() override
        {
            ToolContext::onActivate();
            Windows::WindowLandRightsRefreshCost();
        }
    };

    // OPENRCT2MINI grid-cursor-plan §11.9 / §18.C (2026-05-24):
    // ClearScenery (bulldozer) wired with the same shape as
    // LandRights. No tinted highlight (mouse path uses
    // MapSelectType::full at ClearScenery.cpp:244) and no
    // water-aware cursor parking (ScreenGetMapXY at
    // ClearScenery.cpp:229 doesn't include the water filter).
    // Default brush size at window open is 2 (ClearScenery.cpp:83),
    // not 1; the per-frame setBrushSize poll handles that without
    // any extra work here.
    class ClearSceneryContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolClearScenery;
        }

        bool usesGLandToolSize() const override { return true; }

        // OPENRCT2MINI grid-cursor-plan §11.9 follow-up (2026-05-24,
        // user request): ClearScenery should behave like Land paint
        // mode — holding PAD A while D-padding around continues to
        // clear at each new tile, rather than requiring per-tile
        // press-and-release. The base ToolContext default routes
        // PAD-A-held + D-pad to onRaise/onLower (the raise/lower
        // modifier gesture); ClearScenery has no raise/lower verb,
        // so returning false here keeps the D-pad steps firing
        // normally while PAD A stays held. onStep then chains another
        // dispatch when isCursorClickHeldInTool(). Same shape as
        // TerrainContextImpl's paint-mode override.
        bool consumeDirectionalsWhenCursorClickHeld() const override { return false; }

        Disposition onPlace() override
        {
            Windows::WindowClearSceneryAtCursor();
            return Disposition::Consumed;
        }

        Disposition onStep(::Direction dpad) override
        {
            const auto result = ToolContext::onStep(dpad);
            // §11.9 drag-clear chain: re-fire the action on each
            // step while PAD A is held, mirroring Land paint mode.
            // WindowClearSceneryAtCursor already includes the
            // error-popup guard (won't fire while WindowClass::error
            // is open) so a held button during an error doesn't spam.
            if (isCursorClickHeldInTool())
                Windows::WindowClearSceneryAtCursor();
            Windows::WindowClearSceneryRefreshCost();
            return result;
        }

        void onActivate() override
        {
            ToolContext::onActivate();
            Windows::WindowClearSceneryRefreshCost();
        }
    };

    class TileInspectorContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolTileInspector;
        }
        // Debug tool; lower priority. Verbs select tile element slots
        // within the inspector window — a follow-up wires onPlace /
        // onCancel to the existing tile-inspector "select previous /
        // next element" affordances.
    };

    // OPENRCT2MINI grid-cursor-plan §11.10 (2026-05-29): staff patrol-
    // zone painting. PATTERN B per §11.10.1 — ghost-mediated per-press
    // confirm, NOT the LandRights/ClearScenery drag-chain.
    //
    // Per-tile semantics:
    //   PAD A → auto-toggle (Set if not patrolled, Unset if patrolled),
    //           computed per-press from live tile state (no _latchedMode
    //           member — Pattern B sidesteps the toxic re-fire problem
    //           by not chaining on D-pad steps).
    //   PAD B → force-unset cursor tile, regardless of current state.
    //           Does NOT close the window (per §(k)).
    //   D-pad → step cursor (no verb dispatch on step, unlike §11.5/§11.9).
    //   PAD Y + D-pad up/down → Z-shift = Consumed no-op (patrol is 2D;
    //           same precedent as Ride entranceExit/Selected per §(h)).
    //
    // Brush size from gLandToolSize (default 4 — see PatrolArea.cpp:68).
    class PatrolContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolPatrol;
        }

        bool usesGLandToolSize() const override { return true; }

        // Pattern B: don't override consumeDirectionalsWhenCursorClick-
        // Held — inherit the base default (true). PAD A held + D-pad
        // diverts to onRaise/onLower, which are no-ops here (§(h)),
        // so the diversion is invisible and the D-pad doesn't
        // accidentally chain-fire while the user is just walking the
        // cursor. Per-press PAD A is the only dispatch path.

        Disposition onPlace() override
        {
            // Mode auto-toggles based on tile state at press time.
            // 0 = Set, 1 = Unset (matches StaffSetPatrolAreaMode enum
            // values in actions/peep/StaffSetPatrolAreaAction.h).
            const auto tile = gridCursor().getPosition();
            const bool alreadyPatrolled = Windows::WindowPatrolAreaIsTilePatrolled(tile);
            Windows::WindowPatrolAreaAtCursor(alreadyPatrolled ? 1 : 0);
            return Disposition::Consumed;
        }

        Disposition onCancel() override
        {
            // §(k): PAD B always force-unsets the cursor tile.
            // Distinct from PAD A's auto-toggle. Does NOT close the
            // patrol window — closing is left to the standard paths
            // (close box, focus-mode cancel). Does NOT fall through
            // to the base tile-action right-click dispatch.
            Windows::WindowPatrolAreaAtCursor(1);
            return Disposition::Consumed;
        }

        // §(h): Z-shift is meaningless for patrol (2D tile flags).
        // Consume the gesture so the cursor's Z visual doesn't bump
        // pointlessly. Same precedent as Ride entranceExit/Selected.
        Disposition onRaise() override { return Disposition::Consumed; }
        Disposition onLower() override { return Disposition::Consumed; }
    };

    // OPENRCT2MINI grid-cursor-plan §11.11 (2026-05-29): peep pickup
    // (admin/debug "pick up a guest/staff and drop them elsewhere"
    // tool). Per-tile contract:
    //   PAD A → drop the picked-up peep at the cursor tile. Z comes
    //           from the surface element's GetBaseZ() at that tile,
    //           matching the mouse path (Guest.cpp:1021,
    //           Staff.cpp:720). The Place action's success callback
    //           fires ToolCancel() which clears the tool arm.
    //   PAD B → cancel — return peep to where they were picked up.
    //           Implementation: call OpenRCT2::ToolCancel(), which
    //           triggers the peep window's onToolAbort, which already
    //           dispatches PeepPickupAction::Cancel with the stored
    //           original X. Keeps the peep window open by design —
    //           ToolCancel only clears the tool arm, doesn't close
    //           the window.
    //   PAD START → cycle focus back to the peep window with the
    //               peep still hanging. Intercept kInterfaceConfirm
    //               in onShortcut and call cycleFocusedWindow(-1).
    //               Crucially does NOT fire Cancel — the peep keeps
    //               dangling across the context swap.
    //   Window close while hanging → handled by existing mouse-path
    //                                 onClose → ToolCancel chain.
    //                                 Free behaviour, no new code.
    //   PAD Y + D-pad up/down → Consumed no-op (Z is locked to tile
    //                            base, Z-shift meaningless).
    class PeepPickupContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolPeepPickup;
        }

        Disposition onPlace() override
        {
            Windows::WindowPeepPickupAtTile(gridCursor().getPosition());
            return Disposition::Consumed;
        }

        Disposition onCancel() override
        {
            // Route through the existing per-window onToolAbort path
            // — both Guest and Staff windows already dispatch
            // PeepPickupAction::Cancel with their stored
            // _pickedPeepX / _pickedPeepOldX field, so we don't need
            // to reach into per-window state from here.
            OpenRCT2::ToolCancel();
            return Disposition::Consumed;
        }

        Disposition onRaise() override { return Disposition::Consumed; }
        Disposition onLower() override { return Disposition::Consumed; }

        // PAD START: cycle focus back to the peep window WITHOUT
        // firing Cancel. The peep keeps hanging because we don't
        // call ToolCancel and the engine's onToolAbort only fires
        // on actual tool cancellation, not on InputContext swaps.
        // Per §11.11.4 open question 2: the action's success path
        // (Place's callback) is the ONLY thing that fires
        // ToolCancel; idle context swaps don't.
        Disposition onShortcut(std::string_view id, const InputEvent& e) override
        {
            if (id == ShortcutId::kInterfaceConfirm)
            {
                auto& mgr = OpenRCT2::Ui::GetInputManager();
                mgr.cycleFocusedWindow(-1);
                return Disposition::Consumed;
            }
            return ToolContext::onShortcut(id, e);
        }

        // OPENRCT2MINI grid-cursor-plan §11.11 polish (2026-05-30):
        // pin the dangling-peep sprite to the grid cursor instead
        // of the (hidden) OS pointer. The mouse path drives
        // gPickupPeepX/Y/Image from onToolUpdate*, which only fires
        // on SDL_MOUSEMOTION; in selectorMode::active there's no
        // mouse motion, so without this poll the sprite freezes at
        // the last mouse position (or never appears at all if the
        // user armed pickup straight from focus mode).
        //
        // Call base processFrame first so all the ToolContext
        // machinery (brush-size sync, precision-modifier edges,
        // directional repeat-on-hold) still runs, then refresh the
        // sprite from the grid cursor's current tile. The helper
        // is the single canonical place that does the world->
        // screen conversion at the actual drop Z (surface base Z)
        // — same Z used by WindowPeepPickupAtTile's PeepPickupAction,
        // so the visual matches the drop site.
        void processFrame(uint32_t nowMs) override
        {
            ToolContext::processFrame(nowMs);
            Windows::WindowPeepPickupRefreshHangingSprite(gridCursor().getPosition());
        }

        // OPENRCT2MINI grid-cursor-plan §11.11 polish (2026-05-30
        // follow-up #5): mirror the mouse-mode invariant exactly so
        // the grid-cursor → virtual-mouse transition is seamless.
        //
        // Mouse mode (Guest.cpp:1022-1023 / Staff.cpp:712-713):
        //   - OS pointer at (mouseX, mouseY)
        //   - Picker sprite hotspot (15, 31) → pincers tip at
        //     (mouseX, mouseY) (the OS pointer position)
        //   - Peep drawn at (mouseX-1, mouseY + 16) → peep feet
        //     16 screen px BELOW the pincers tip
        //   - Tile detection samples at (mouseX, mouseY + 16) —
        //     i.e. the peep position. So the tile under the cursor
        //     is the tile whose CENTRE projects to mouseY + 16.
        //
        // Therefore in mouse mode: cursor reference point sits 16
        // screen px ABOVE the tile centre, peep feet sit AT the
        // tile centre. Pincers 16 px above peep is the natural
        // consequence of this geometry.
        //
        // For grid mode to match this layout AND have the cursor
        // land at the mouse-mode-natural rest position (16 above
        // the tile, so mouse tile detection picks the same tile
        // post-transition with no virtual cursor snap):
        //   - cursorParkZExtra = 16 → cursor at land_z + 16 world,
        //     which projects to 16 screen px above tile centre
        //   - Peep drawn at land_z world (tile centre in screen),
        //     handled in WindowPeepPickupRefreshHangingSprite
        //
        // Earlier follow-up #3 used extraZ = 32 + peep at drop Z
        // (surface + 16) to achieve "peep starts at drop height for
        // a continuous fall on release". This gave the same
        // pincers-above-peep relationship but parked the cursor
        // 16 px higher than mouse mode's natural rest point,
        // breaking the grid → mouse tile-detection handoff (mouse
        // mode would sample +16 below the cursor and hit the tile
        // above the user's intended one). Reverting to mouse-mode
        // geometry: small visible jump on release (peep teleports
        // from tile-centre-Z to drop-Z then falls), but the mouse
        // mode has exactly the same release jump, so the user
        // perception is identical to the existing tool.
        //
        // worldCoord unused — peep pickup is always single-tile.
        int32_t cursorParkZExtra(CoordsXY /*worldCoord*/) const override
        {
            return 16;
        }
    };

    // OPENRCT2MINI ride-construction-grid-cursor-plan §5 (Phase 1, 2026-05-25):
    // gamepad-driven track design. Mirrors FootpathContextImpl's pattern:
    // mode-branching verbs (RideInputMode discriminator); shift-modifier
    // chord scheme (unmodified D-pad = curve/slope, held = bank/chain/special,
    // tap-alone-modifier = full reset to plain straight); head-follow via
    // base ToolContext::getHeadTile() machinery; per-frame snapshot-reset
    // edge detection mirroring FootpathContextImpl's Z-hold gesture.
    //
    // The verb bodies dispatch to existing upstream free functions
    // (WindowRideConstructionKeyboardShortcut*) which handle the per-ride-
    // type validity gating already (slope availability, banking support,
    // chain-lift compatibility, etc.) — no new gate logic needed in this
    // class.
    class RideConstructionContextImpl final : public ToolContext
    {
    public:
        InputContext getId() const override
        {
            return InputContext::toolRideConstruction;
        }

        // Head-follow integration with ToolContext::processFrame's poll.
        // Returns the next-piece head tile (_currentTrackBegin) when the
        // window is in Front/Back/Place/EntranceExit. Arrow flag is
        // preserved so the bridge-direction-style arrow at the head
        // continues to render.
        std::optional<TileCoordsXY> getHeadTile() const override
        {
            return Windows::WindowRideConstructionGetHeadTile();
        }
        bool headFollowPreservesArrow() const override { return true; }
        // OPENRCT2MINI grid-cursor Z-follow (2026-05-31): track head's
        // world Z (_currentTrackBegin.z), so the grid cursor visual +
        // bump-scroll chase elevated track construction up into the
        // air instead of staying glued to the terrain below. Returns
        // nullopt in non-build states (matches getHeadTile's gate).
        std::optional<int32_t> getHeadWorldZ() const override
        {
            return Windows::WindowRideConstructionGetHeadWorldZ();
        }

        Disposition onPlace() override
        {
            const auto mode = Windows::WindowRideConstructionGetInputMode();
            switch (mode)
            {
                case Windows::RideInputMode::buildForward:
                case Windows::RideInputMode::buildBackward:
                    Windows::WindowRideConstructionKeyboardShortcutBuildCurrent();
                    // Post-build the head advances; chase to it. The per-
                    // frame head poll in ToolContext::processFrame catches
                    // the async-callback head update too, but the immediate
                    // sync here keeps the highlight feeling responsive.
                    syncGridCursorToHead();
                    return Disposition::Consumed;
                case Windows::RideInputMode::initialPlace:
                    // Place → Front state flip + WIDX_CONSTRUCT click + Z
                    // trial-and-error. The original SetInitialPlaceAt +
                    // BuildCurrent path didn't work — BuildCurrent bails when
                    // WIDX_CONSTRUCT is disabled, which IS the case in Place
                    // state (WIDX_CONSTRUCT only enables after we flip to
                    // Front). The mouse path's RideConstructionTooldown-
                    // Construct does the flip first; we mirror that via the
                    // new PlaceInitialAtTile helper. Post-commit the state is
                    // Front and the per-frame head poll syncs the cursor to
                    // the new head.
                    Windows::WindowRideConstructionPlaceInitialAtTile(
                        gridCursor().getPosition());
                    return Disposition::Consumed;
                case Windows::RideInputMode::entranceExit:
                    // Plan §7.3 (revised 2026-05-28). Dispatch RideEntrance-
                    // ExitPlaceAction at the cursor tile. Direction is auto-
                    // determined by the adjacency search inside the helper
                    // (re-run as defence-in-depth even though onStep keeps
                    // it fresh). Refuses placement when the cursor isn't
                    // adjacent to a valid station edge.
                    Windows::WindowRideConstructionPlaceEntranceExit(
                        gridCursor().getPosition());
                    return Disposition::Consumed;
                case Windows::RideInputMode::selected:
                case Windows::RideInputMode::none:
                default:
                    return Disposition::Passthrough;
            }
        }

        Disposition onCancel() override
        {
            switch (Windows::WindowRideConstructionGetInputMode())
            {
                case Windows::RideInputMode::buildForward:
                case Windows::RideInputMode::buildBackward:
                    // OPENRCT2MINI ride-construction-grid-cursor-plan §16
                    // (2026-05-28): shape-modifier (PAD Y) + PAD B in
                    // build state enters Selected state by stepping
                    // from the head into the previously-placed piece.
                    // From there D-pad left/right navigates the section
                    // selection (onStep handles that), and a second PAD
                    // B demolishes the highlighted piece (§16 above).
                    // Bare PAD B keeps the demolish-current-piece
                    // semantic so the user can still rapid-undo the last
                    // build without entering Selected.
                    //
                    // Direction selection: the verb that drops into
                    // Selected depends on which head we're at.
                    // RideSelectPreviousSection (RideConstruction.cpp:995)
                    // only handles the Front state head; RideSelectNext-
                    // Section (:937) only handles the Back state head.
                    // Calling the wrong one from the other head is a
                    // no-op. buildForward → Previous, buildBackward →
                    // Next.
                    if (OpenRCT2::Ui::isShiftModifierHeldInTool())
                    {
                        // Suppress the shape-modifier tap-release
                        // UseTrackDefault fallback so the chord is
                        // exclusively the section-back gesture.
                        _shapeModUsedThisHold = true;
                        if (Windows::WindowRideConstructionGetInputMode()
                            == Windows::RideInputMode::buildBackward)
                            Windows::WindowRideConstructionKeyboardShortcutNextTrack();
                        else
                            Windows::WindowRideConstructionKeyboardShortcutPreviousTrack();
                        syncGridCursorToHead();
                        return Disposition::Consumed;
                    }
                    Windows::WindowRideConstructionKeyboardShortcutDemolishCurrent();
                    syncGridCursorToHead();
                    return Disposition::Consumed;
                case Windows::RideInputMode::entranceExit:
                    // Plan §7. PAD B in entranceExit state cancels back to
                    // the previous construction state (Front/Back/Selected),
                    // it does NOT do the base right-click-remove. Mirrors
                    // the mouse path's behaviour at RideConstruction.cpp:1069.
                    Windows::WindowRideConstructionCancelEntranceExitMode();
                    return Disposition::Consumed;
                case Windows::RideInputMode::selected:
                    // OPENRCT2MINI ride-construction-grid-cursor-plan §16
                    // (2026-05-28): PAD B is context-sensitive. The first
                    // PAD B on a placed ride goes through ToolContext::on-
                    // Cancel → ViewportInteractionRightClickAtGridCursor
                    // which picks up the hovered piece (state becomes
                    // Selected, the piece flashes as a ghost). A second
                    // PAD B in that selected state should commit the
                    // delete — without this branch it falls through to
                    // the base right-click which doesn't demolish the
                    // already-selected piece. Mirrors the gesture
                    // contract: PAD B → Select → PAD B → Delete.
                    Windows::WindowRideConstructionKeyboardShortcutDemolishCurrent();
                    return Disposition::Consumed;
                default:
                    return ToolContext::onCancel();
            }
        }

        // Finish in grid mode closes the construction window outright.
        Disposition onFinishTool() override
        {
            auto* windowMgr = GetWindowManager();
            if (windowMgr != nullptr)
                windowMgr->CloseByClass(WindowClass::rideConstruction);
            return Disposition::Consumed;
        }

        // Precision modifier + cardinal D-pad in initialPlace state → set
        // initial direction directly (post-Phase 2 user request, 2026-05-25).
        // Mirrors the Land tool's corner picker pattern but maps the D-pad
        // direction to a world compass direction (via stepForDirection's
        // camera-rotated table) rather than a sub-tile corner. PAD Y cycling
        // remains as a secondary path.
        Disposition onPrecisionDpad(::Direction dpad) override
        {
            if (Windows::WindowRideConstructionGetInputMode()
                == Windows::RideInputMode::initialPlace)
            {
                // Reuse stepForDirection (compass mode) to translate the
                // screen-relative dpad direction to a world TileCoordsXY
                // delta, then read off the world cardinal. The (-1,0)/(0,1)/
                // (1,0)/(0,-1) deltas correspond to N/E/S/W = directions
                // 0/1/2/3 per Map.cpp:88's TileDirectionDelta convention.
                const uint8_t rot = OpenRCT2::GetCurrentRotation();
                const auto delta = OpenRCT2::Ui::stepForDirection(
                    OpenRCT2::Ui::GridCursorMode::compass, rot, dpad);
                uint8_t worldDir = 0; // default N
                if (delta.x == 0 && delta.y == 1) worldDir = 1;      // E
                else if (delta.x == 1 && delta.y == 0) worldDir = 2; // S
                else if (delta.x == 0 && delta.y == -1) worldDir = 3; // W
                Windows::WindowRideConstructionSetInitialDirection(worldDir);
                // Note: don't touch _precisionDpadPressed (private on base).
                // The base's tap-alone reset only matters for the cursor
                // orientation flow which we override entirely here.
                return Disposition::Consumed;
            }
            return ToolContext::onPrecisionDpad(dpad);
        }

        // PAD Y in build state → TurnRight (clockwise piece). In initialPlace,
        // PAD Y cycles the initial direction (Plan §6.2).
        Disposition onRotate() override
        {
            const auto mode = Windows::WindowRideConstructionGetInputMode();
            if (mode == Windows::RideInputMode::buildForward
                || mode == Windows::RideInputMode::buildBackward)
            {
                Windows::WindowRideConstructionKeyboardShortcutTurnRight();
                syncGridCursorToHead();
                return Disposition::Consumed;
            }
            if (mode == Windows::RideInputMode::initialPlace)
            {
                Windows::WindowRideConstructionCycleInitialDirection();
                return Disposition::Consumed;
            }
            if (mode == Windows::RideInputMode::entranceExit)
            {
                // Plan §7 (revised 2026-05-28): direction is now auto-
                // determined by the cursor's adjacency to a station edge
                // (mirrors mouse path). Manual PAD Y cycling is dropped —
                // the user navigates direction by moving the cursor to a
                // different side of the station tile. No-op here so PAD Y
                // doesn't accidentally do something else.
                return Disposition::Consumed;
            }
            return Disposition::Consumed;
        }

        // onRaise/onLower in build state dispatch the SlopeUp/SlopeDown
        // verbs (matching bridgeBuild). In initialPlace state we also
        // adjust _trackPlaceZ so the ride piece ghost preview moves with
        // the grid cursor — without this, the cursor's blue square visually
        // moves but the ghost piece stays at ground level (user-reported
        // bug, 2026-05-26 in plan §16).
        //
        // §16 lesson: gridCursor().raiseZ/lowerZ only moves the cursor
        // visual; the ride piece reads _trackPlaceZ (RideConstruction.cpp
        // :5286), so the placement Z has its own state separate from the
        // cursor Z. We update both: cursor Z for the floor highlight visual
        // and _trackPlaceZ for the ride ghost via WindowRideConstruction-
        // AdjustPlaceZ, which also refreshes the provisional piece preview.
        //
        // Step size: kLandHeightStep (16), NOT kCoordsZStep (8). Most
        // rides only place at 16-multiples; an 8-step bump would get
        // rounded back to the same 16-multiple by the trial-and-error
        // loop in ShowGhostAtTile, making every other tap a no-op. Both
        // cursor visual and _trackPlaceZ step by 16 in lockstep so the
        // floor highlight stays aligned with where the piece will land.
        Disposition onRaise() override
        {
            const auto mode = Windows::WindowRideConstructionGetInputMode();
            if (mode == Windows::RideInputMode::buildForward
                || mode == Windows::RideInputMode::buildBackward)
            {
                Windows::WindowRideConstructionKeyboardShortcutSlopeUp();
                syncGridCursorToHead();
                return Disposition::Consumed;
            }
            if (mode == Windows::RideInputMode::initialPlace)
            {
                gridCursor().raiseZ(::kLandHeightStep);
                Windows::WindowRideConstructionAdjustPlaceZ(
                    gridCursor().getPosition(), ::kLandHeightStep);
                return Disposition::Consumed;
            }
            // OPENRCT2MINI ride-construction-grid-cursor-plan §16
            // (2026-05-29): Z-shift is a Consumed no-op in entranceExit,
            // selected, and none states. Entrance/exit Z is locked to
            // the station base (RideConstruction.cpp's station.GetBaseZ
            // path), selected just navigates already-placed pieces, and
            // none has no placement at all — bumping gridCursor().raise/
            // lowerZ in those states moved the cursor visual to a Z
            // that didn't affect anything, so it's better to swallow the
            // gesture entirely than to mislead the user.
            return Disposition::Consumed;
        }
        Disposition onLower() override
        {
            const auto mode = Windows::WindowRideConstructionGetInputMode();
            if (mode == Windows::RideInputMode::buildForward
                || mode == Windows::RideInputMode::buildBackward)
            {
                Windows::WindowRideConstructionKeyboardShortcutSlopeDown();
                syncGridCursorToHead();
                return Disposition::Consumed;
            }
            if (mode == Windows::RideInputMode::initialPlace)
            {
                gridCursor().lowerZ(::kLandHeightStep);
                Windows::WindowRideConstructionAdjustPlaceZ(
                    gridCursor().getPosition(), -::kLandHeightStep);
                return Disposition::Consumed;
            }
            // Mirror of onRaise: no-op in entranceExit / selected / none.
            return Disposition::Consumed;
        }

        // OPENRCT2MINI ride-construction-grid-cursor-plan §16.10 (2026-05-25):
        // intercept the shift modifier + D-pad chord BEFORE the base class's
        // onShortcut runs. The base ToolContext::onShortcut at line ~1169 of
        // InputContextStrategy.h handles shift+D-pad-up/down as Z-raise/lower
        // (the Footpath bridge gesture) and silently Consumes shift+D-pad-
        // left/right. For Ride, the shift modifier means "configure piece
        // bank/chain/special" — completely different semantic. Without this
        // override the chord dispatch in onStepBuild was unreachable.
        //
        // Only applies in build state (Front/Back); other states fall through
        // to the base for normal grid cursor stepping.
        Disposition onShortcut(std::string_view id, const InputEvent& e) override
        {
            if (OpenRCT2::Ui::isShiftModifierHeldInTool()
                && Windows::WindowRideConstructionIsInBuildState())
            {
                ::Direction d{};
                bool isDpad = true;
                if (id == ShortcutId::kFocusUp)
                    d = static_cast<::Direction>(0);
                else if (id == ShortcutId::kFocusRight)
                    d = static_cast<::Direction>(1);
                else if (id == ShortcutId::kFocusDown)
                    d = static_cast<::Direction>(2);
                else if (id == ShortcutId::kFocusLeft)
                    d = static_cast<::Direction>(3);
                else
                    isDpad = false;
                if (isDpad)
                {
                    return onStepBuild(d);
                }
            }
            return ToolContext::onShortcut(id, e);
        }

        // D-pad in build state: unmodified = curve/slope shape configuration,
        // shape-modifier held = banking/chain/special-track cycling. In
        // initialPlace, fall through to base grid-cursor stepping so the user
        // can pick a tile for the first piece. EntranceExit also uses base
        // stepping (Phase 3 will add the precision-modifier edge picker).
        Disposition onStep(::Direction dpad) override
        {
            const auto mode = Windows::WindowRideConstructionGetInputMode();
            if (mode == Windows::RideInputMode::buildForward
                || mode == Windows::RideInputMode::buildBackward)
            {
                return onStepBuild(dpad);
            }
            if (mode == Windows::RideInputMode::initialPlace)
            {
                // Step the grid cursor, then spawn the ghost piece +
                // directional arrow at the new cursor tile via the mouse-
                // path-mirror helper. The previous SetInitialPlaceAt only
                // wrote _currentTrackBegin and refreshed widgets — it
                // didn't set the enableConstruct/enableArrow MapSelect
                // flags or call PlaceProvisionalTrackPiece, so the user
                // got the regular tile highlight with no piece preview.
                const auto result = ToolContext::onStep(dpad);
                Windows::WindowRideConstructionShowGhostAtTile(
                    gridCursor().getPosition());
                return result;
            }
            if (mode == Windows::RideInputMode::entranceExit)
            {
                // Plan §7 (revised 2026-05-28): after stepping the cursor,
                // re-run the adjacency search to update gRideEntranceExit-
                // PlaceDirection. This is the gamepad equivalent of the
                // mouse path's per-hover RideGetEntranceOrExitPositionFrom-
                // ScreenPosition call. The ride window's preview panel
                // reads gRideEntranceExitPlaceDirection and shows it live
                // so the user gets feedback on whether the cursor tile is
                // valid for placement.
                const auto result = ToolContext::onStep(dpad);
                Windows::WindowRideConstructionUpdateEntranceExitDirection(
                    gridCursor().getPosition());
                // Invalidate so the direction preview updates.
                if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                    windowMgr->InvalidateByClass(WindowClass::rideConstruction);
                return result;
            }
            if (mode == Windows::RideInputMode::selected)
            {
                // OPENRCT2MINI ride-construction-grid-cursor-plan §16
                // (2026-05-28): D-pad navigates the section selection
                // along the placed track. Left → previous, Right →
                // next. Up/Down are swallowed (no grid cursor stepping
                // in Selected state — the "cursor" is the flashing
                // ghost on the selected piece, driven by the section
                // navigation verbs). Direction encoding matches the
                // shape-modifier chord at :2526-2533 (Up=0, Right=1,
                // Down=2, Left=3).
                //
                // After the verb runs, sync the grid cursor to the new
                // head (now exposed for Selected via WindowRideCon-
                // structionGetHeadTile). The per-frame poll in process-
                // Frame catches it too, but the immediate call here
                // keeps the highlight feeling responsive.
                switch (static_cast<int>(dpad))
                {
                    case 3: // left
                        Windows::WindowRideConstructionKeyboardShortcutPreviousTrack();
                        syncGridCursorToHead();
                        return Disposition::Consumed;
                    case 1: // right
                        Windows::WindowRideConstructionKeyboardShortcutNextTrack();
                        syncGridCursorToHead();
                        return Disposition::Consumed;
                    default: // up / down
                        return Disposition::Consumed;
                }
            }
            return Disposition::Passthrough;
        }

        void onActivate() override
        {
            ToolContext::onActivate();
            // Reset mode tracking so the next processFrame's mode-transition
            // edge-detect fires for whatever mode the construction window is
            // in right now (e.g. user re-engages while window is parked in
            // entranceExit — the auto-position should still run).
            _lastSeenInputMode = Windows::RideInputMode::none;
            if (getHeadTile().has_value())
                syncGridCursorToHead();
            // For initialPlace, immediately spawn the ghost piece + arrow at
            // the engaged cursor tile so the user sees the placement preview
            // without having to step first (mirrors the mouse path's first-
            // hover behaviour).
            const auto activateMode = Windows::WindowRideConstructionGetInputMode();
            if (activateMode == Windows::RideInputMode::initialPlace)
            {
                Windows::WindowRideConstructionShowGhostAtTile(gridCursor().getPosition());
            }
            // For entranceExit, auto-position the cursor onto the best
            // valid placement tile (closest to viewport centre) so the
            // user can commit immediately — otherwise the cursor sits on
            // wherever they were last building (usually the head tile,
            // which is on the track and therefore invalid). Then run the
            // adjacency search to set the direction + spawn the ghost
            // preview.
            else if (activateMode == Windows::RideInputMode::entranceExit)
            {
                if (auto best = Windows::WindowRideConstructionFindBestEntranceExitTile();
                    best.has_value())
                {
                    gridCursor().setPosition(*best);
                }
                Windows::WindowRideConstructionUpdateEntranceExitDirection(
                    gridCursor().getPosition());
                if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                    windowMgr->InvalidateByClass(WindowClass::rideConstruction);
            }
        }

        void onDeactivate() override
        {
            ToolContext::onDeactivate();
        }

        // Shape-modifier snapshot-reset (mirrors FootpathContextImpl's Z-hold
        // gesture at InputManager.cpp:1710-ish). Press edge arms the
        // "no chord pressed yet" flag; release edge with the flag still
        // false fires UseTrackDefault — tapping the shift modifier alone
        // (no D-pad during the hold) resets the staged piece to plain
        // straight. Per-ride-type gating happens inside the free function.
        //
        // Also detects RideInputMode transitions: when the ride construction
        // state machine enters EntranceExit (either via the user clicking
        // Entrance/Exit, or via the auto-flip after placing the last track
        // piece via CloseConstructWindowOnCompletion), auto-position the
        // grid cursor onto the nearest valid placement tile and fire the
        // ghost preview. onActivate doesn't catch this because the InputContext
        // doesn't change when the tool widget swaps within the same window;
        // the state transition is internal to the construction window.
        void processFrame(uint32_t nowMs) override
        {
            ToolContext::processFrame(nowMs); // base does head-follow poll + DirectionalRepeat

            // Mode-transition detection (auto-position on enter entranceExit).
            // Cursor re-activation: the auto-flip from Front → EntranceExit
            // goes through ToolSet (in CloseConstructWindowOnCompletion →
            // EntranceClick), which calls ToolCancel first and that fires
            // onDeactivate on the strategy, dropping gridCursor._active to
            // false. The InputContext doesn't change so no onActivate fires
            // to bring it back. Result without intervention: cursor invisible
            // until the user mashes the D-pad and the engage gesture re-arms
            // it. Fix: explicitly re-engage the gridCursor + selector mode
            // here so the cursor pops up immediately on the new station-edge
            // tile with the entrance/exit ghost.
            const auto currentMode = Windows::WindowRideConstructionGetInputMode();
            if (currentMode != _lastSeenInputMode)
            {
                if (currentMode == Windows::RideInputMode::entranceExit)
                {
                    if (auto best = Windows::WindowRideConstructionFindBestEntranceExitTile();
                        best.has_value())
                    {
                        gridCursor().setPosition(*best);
                    }
                    // Force the cursor + selector active so the user sees the
                    // auto-positioned tile without needing to engage manually.
                    gridCursor().onActivate();
                    auto& mgr = OpenRCT2::Ui::GetInputManager();
                    mgr.setToolFocusSelected(
                        true,
                        OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                    mgr.setSelectorMode(
                        OpenRCT2::Ui::InputManager::SelectorMode::active);
                    Windows::WindowRideConstructionUpdateEntranceExitDirection(
                        gridCursor().getPosition());
                    if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                        windowMgr->InvalidateByClass(WindowClass::rideConstruction);
                }
                _lastSeenInputMode = currentMode;
            }

            if (!Windows::WindowRideConstructionIsInBuildState())
            {
                _shapeModWasHeld = false;
                _shapeModUsedThisHold = false;
                return;
            }
            const bool shapeModNow = OpenRCT2::Ui::isShiftModifierHeldInTool();
            if (shapeModNow != _shapeModWasHeld)
            {
                if (shapeModNow)
                {
                    _shapeModUsedThisHold = false;
                }
                else
                {
                    if (!_shapeModUsedThisHold)
                    {
                        Windows::WindowRideConstructionKeyboardShortcutUseTrackDefault();
                        syncGridCursorToHead();
                    }
                }
                _shapeModWasHeld = shapeModNow;
            }
        }

    private:
        // Build-state D-pad mapping per ride-construction-grid-cursor-plan
        // §5.4. Unmodified = curve/slope; shape modifier held = bank/chain/
        // special. Direction values match the ToolContext::onShortcut
        // mapping at InputContextStrategy.h: 0=up, 1=right, 2=down, 3=left.
        Disposition onStepBuild(::Direction dpad)
        {
            const bool shapeModHeld = OpenRCT2::Ui::isShiftModifierHeldInTool();
            if (shapeModHeld)
            {
                _shapeModUsedThisHold = true;
                switch (static_cast<int>(dpad))
                {
                    case 0: // up — toggle chain lift
                        Windows::WindowRideConstructionKeyboardShortcutChainLiftToggle();
                        break;
                    case 1: // right — bank right
                        Windows::WindowRideConstructionKeyboardShortcutBankRight();
                        break;
                    case 2: // down — cycle special-track piece
                        Windows::WindowRideConstructionKeyboardShortcutCycleSpecialNext();
                        break;
                    case 3: // left — bank left
                        Windows::WindowRideConstructionKeyboardShortcutBankLeft();
                        break;
                    default:
                        return Disposition::Passthrough;
                }
            }
            else
            {
                switch (static_cast<int>(dpad))
                {
                    case 0: // up — slope up
                        Windows::WindowRideConstructionKeyboardShortcutSlopeUp();
                        break;
                    case 1: // right — turn right
                        Windows::WindowRideConstructionKeyboardShortcutTurnRight();
                        break;
                    case 2: // down — slope down
                        Windows::WindowRideConstructionKeyboardShortcutSlopeDown();
                        break;
                    case 3: // left — turn left
                        Windows::WindowRideConstructionKeyboardShortcutTurnLeft();
                        break;
                    default:
                        return Disposition::Passthrough;
                }
            }
            syncGridCursorToHead();
            return Disposition::Consumed;
        }

        bool _shapeModWasHeld = false;        // prev-frame state for edge detection
        bool _shapeModUsedThisHold = false;   // any chord pressed during the current hold?
        // Tracks the previous frame's RideInputMode so processFrame can fire
        // auto-position when the construction state machine transitions to
        // entranceExit. Initialised to `none` so a fresh activate followed by
        // entry into entranceExit (e.g. user opens an existing ride and clicks
        // the Entrance button immediately) is treated as a transition.
        Windows::RideInputMode _lastSeenInputMode = Windows::RideInputMode::none;
    };
} // namespace

InputManager::InputManager()
{
    _modifierKeyState = EnumValue(ModifierKey::none);

    // OPENRCT2MINI input-plan Track 3 / Phase 3.A: instantiate the
    // world-context stub as the universal fallback strategy. It
    // returns Passthrough for every shortcut so behaviour is
    // byte-identical to the pre-routing state. Phase 3.C lifts the
    // real cursor logic into this slot via a PixelCursorModel-backed
    // implementation. Phase 3.B fills the remaining slots
    // (osk / loadSave / etc.) with their real strategy classes.
    _worldContext = std::make_unique<WorldContextStub>();
    _worldContext->onActivate();

    // OPENRCT2MINI input-plan Track 3 / Phase 3.B: populate the
    // strategy registry for the six existing modal InputContext enum
    // entries. Registry slot index matches the enum's underlying
    // value. Strategies are owned by the registry array (unique_ptr);
    // when getActiveContextStrategy() looks up by enum, an empty slot
    // falls back to _worldContext. Phase 3.B fills every slot so the
    // fallback is only used for genuinely unknown values.
    _contextRegistry[static_cast<size_t>(InputContext::world)] = std::make_unique<WorldContextStub>();
    _contextRegistry[static_cast<size_t>(InputContext::osk)] = std::make_unique<OskContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::textInput)] = std::make_unique<TextInputContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::loadSaveOverwritePrompt)]
        = std::make_unique<LoadSaveOverwritePromptContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::loadSave)] = std::make_unique<LoadSaveContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::console)] = std::make_unique<ConsoleContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::widgetTextBox)] = std::make_unique<WidgetTextBoxContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::widgetFocus)] = std::make_unique<WidgetFocusContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolFootpath)] = std::make_unique<FootpathContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolTerrain)] = std::make_unique<TerrainContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolWater)] = std::make_unique<WaterContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolScenery)] = std::make_unique<SceneryContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolLandRights)] = std::make_unique<LandRightsContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolTileInspector)] = std::make_unique<TileInspectorContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolRideConstruction)] = std::make_unique<RideConstructionContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolClearScenery)] = std::make_unique<ClearSceneryContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolPatrol)] = std::make_unique<PatrolContextImpl>();
    _contextRegistry[static_cast<size_t>(InputContext::toolPeepPickup)] = std::make_unique<PeepPickupContextImpl>();

    // OPENRCT2MINI cursor-selector-modal-plan §3.1: seed selector
    // mode from config. widgetFocusAlwaysOn defaulted true today,
    // so most users boot into `active` (selector ring on, cursor
    // hidden until first input wakes it). Users who opt out via
    // Options boot into `hidden` (cursor visible, ring dormant).
    _selectorMode = Config::Get().general.widgetFocusAlwaysOn
        ? SelectorMode::active
        : SelectorMode::hidden;
}

// OPENRCT2MINI input-plan Track 3 / Phase 3.A: out-of-line dtor so
// the unique_ptr<IInputContext> members destruct in this TU where
// IInputContext is complete (InputContextStrategy.h is included
// above). The header only forward-declares IInputContext.
InputManager::~InputManager() = default;

// OPENRCT2MINI focus-mode-plan / Phase F.3: focus-state accessors.
// Out-of-line because the header forward-declares the types they
// touch (the global `WindowClass` enum, OpenRCT2's WindowBase
// struct). The default values chosen in the header are spelled as
// raw integers to dodge the forward-decl restriction; these
// static_asserts pin them to the real sentinel constants so
// accidental drift would fail the build.
static_assert(static_cast<uint8_t>(WindowClass::null) == 255, "WindowClass::null must equal 255 for InputManager focus default");
static_assert(OpenRCT2::kWidgetIndexNull == 0xFFFF, "kWidgetIndexNull must equal 0xFFFF for InputManager focus default");

OpenRCT2::WindowBase* InputManager::getFocusedWindow() const
{
    // Per-frame re-lookup; never store the raw pointer. If the user
    // closed the focused window between frames, FindByClass returns
    // null and the caller treats focus as cleared (process() then
    // resets the sentinels via clearFocus on the next tick).
    if (_focusedWindowClass == WindowClass::null)
        return nullptr;
    auto* windowMgr = GetWindowManager();
    if (windowMgr == nullptr)
        return nullptr;
    return windowMgr->FindByClass(_focusedWindowClass);
}

void InputManager::setFocus(WindowClass cls, OpenRCT2::WidgetIndex widget)
{
    // OPENRCT2MINI list-focus-plan §2.2: changing the focused widget
    // invalidates any list-mode scroll item — the new widget might not
    // be a scroll widget at all. Callers that want to keep the item
    // index (e.g. cursor → focus mode transition per C7) write to it
    // immediately after via setFocusScrollItem.
    const bool focusChanged = (_focusedWindowClass != cls || _focusedWidget != widget);
    if (focusChanged)
        _focusedScrollItem = -1;
    const auto prevCls = _focusedWindowClass;
    _focusedWindowClass = cls;
    _focusedWidget = widget;

    // OPENRCT2MINI focus-memory-plan §3 (2026-05-25): single write
    // site for the per-window memory map. Every setFocus that lands
    // on a real (cls, widget) records it, so cycle / auto-snap /
    // restoreFocus can later read back where the user was on each
    // window. On 65th distinct window insert, evict the first entry
    // whose backing class no longer exists (lazy stale cleanup); if
    // all entries are still live, drop an arbitrary one — extremely
    // unlikely (engine has ~50 distinct focusable classes).
    if (cls != WindowClass::null && widget != OpenRCT2::kWidgetIndexNull)
    {
        constexpr size_t kFocusMemoryCap = 64;
        _focusMemory[cls] = widget;
        if (_focusMemory.size() > kFocusMemoryCap)
        {
            auto* windowMgr = GetWindowManager();
            bool evicted = false;
            if (windowMgr != nullptr)
            {
                for (auto it = _focusMemory.begin(); it != _focusMemory.end(); ++it)
                {
                    if (it->first == cls)
                        continue; // never evict the one we just wrote
                    if (windowMgr->FindByClass(it->first) == nullptr)
                    {
                        _focusMemory.erase(it);
                        evicted = true;
                        break;
                    }
                }
            }
            if (!evicted)
            {
                // No stale entry found — drop the first non-current entry.
                for (auto it = _focusMemory.begin(); it != _focusMemory.end(); ++it)
                {
                    if (it->first != cls)
                    {
                        _focusMemory.erase(it);
                        break;
                    }
                }
            }
        }
    }

    // OPENRCT2MINI focus-ring redraw (2026-05-24): the yellow focus
    // ring is overlaid by WindowDraw → drawFocusOutlineIfActive, and
    // that only fires on windows whose dirty region intersects the
    // current paint pass. On a static screen (e.g. title menu where
    // the menu window's widgets never change), nothing dirties either
    // the old or new focused window when focus moves — so the old
    // ring trails and the new one doesn't draw in until some
    // unrelated event (attract-mode viewport pan, another widget
    // animation) happens to dirty those regions.
    //
    // Invalidate both windows on focus change. Cheap when focus
    // didn't actually move (early bail above keeps the no-op path).
    if (focusChanged)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr)
        {
            if (prevCls != WindowClass::null && prevCls != cls)
                windowMgr->InvalidateByClass(prevCls);
            if (cls != WindowClass::null)
                windowMgr->InvalidateByClass(cls);
        }
    }

    // OPENRCT2MINI focus-mode-widgets-plan §3.2 / Cohort B (2026-05-25):
    // when focus lands on a list-mode scroll widget AND no scroll item
    // is seeded yet, default to item 0 so the window's row-highlight
    // tracks the focus ring from the first frame. Without this, single-
    // focusable-widget windows like Staff and Scenario Select show the
    // focus ring around the bare scroll widget but no row-highlight
    // until the user makes a directional press — and for Scenario
    // Select that breaks the right-hand preview pane because the
    // preview is gated on the hovered scenario.
    //
    // The "first directional press" path in dispatchDirection seeds the
    // item via spatial nearestScrollItemTo — that still works for
    // cursor → focus mode transitions where refPoint is meaningful.
    // This seed-on-entry path covers the case where focus arrives via
    // cycleFocusedWindow / snapFocusToTopmostFocusable / enterFocusMode /
    // restoreFocus, where no directional press has fired.
    //
    // Item 0 is the deterministic top-of-list landing. List-item memory
    // across visits is a separate (larger) feature — see
    // focus-mode-widgets-plan §5 E3.
    if (focusChanged && cls != WindowClass::null && widget != OpenRCT2::kWidgetIndexNull
        && _focusedScrollItem < 0)
    {
        if (auto* w = getFocusedWindow();
            w != nullptr && static_cast<size_t>(widget) < w->widgets.size()
            && WidgetFocus::isListModeScroll(*w, widget))
        {
            _focusedScrollItem = 0;
            WidgetFocus::ensureScrollItemVisible(*w, widget, 0);
            restoreFocusedListHover();
        }
    }
}

// OPENRCT2MINI list-focus-plan flicker fix: see InputManager.h for the
// rationale. Re-synthesise an onScrollMouseOver at the focused item's
// content-local centre so per-window hover state survives the per-
// frame reset chain. Same body as the static syncFocusItemHover used
// by the directional dispatch — duplicated here as a method so
// non-strategy callers (InvalidateAllWindowsAfterInput) can invoke it
// without touching dispatch internals.
void InputManager::restoreFocusedListHover()
{
    if (_focusedScrollItem < 0)
        return;
    auto* w = getFocusedWindow();
    if (w == nullptr)
        return;
    const auto widget = _focusedWidget;
    if (widget == OpenRCT2::kWidgetIndexNull
        || static_cast<size_t>(widget) >= w->widgets.size())
        return;
    if (w->widgets[widget].type != OpenRCT2::WidgetType::scroll)
        return;
    // Only fire when the window has opted in to list-mode focus.
    // isListModeScroll cross-checks scrollFocusGetItemCount > 0 so
    // we never call onScrollMouseOver on an opt-out scroll widget.
    if (!OpenRCT2::Ui::WidgetFocus::isListModeScroll(*w, widget))
        return;
    const auto scrollIdx = OpenRCT2::WindowGetScrollDataIndex(*w, widget);
    const auto rect = w->scrollFocusGetItemRect(scrollIdx, _focusedScrollItem);
    if (rect.GetLeft() >= rect.GetRight() || rect.GetTop() >= rect.GetBottom())
        return;
    const ScreenCoordsXY centre{
        (rect.GetLeft() + rect.GetRight()) / 2,
        (rect.GetTop() + rect.GetBottom()) / 2,
    };
    w->onScrollMouseOver(scrollIdx, centre);
}

void InputManager::snapFocusToTopmostFocusable()
{
    // OPENRCT2MINI focus-mode-plan §F.15: same reverse-walk +
    // skip rules as the per-frame bootstrap, with one extra
    // preference: pick a non-chrome window if any exist.
    // Rationale: bottom-of-stack chrome (top + bottom toolbars)
    // is always-present background scaffolding the user didn't
    // explicitly summon, so a click that opens a normal modal
    // (Options, About, Save/Load, …) should focus the modal —
    // not the toolbar that happens to be drawn over it.
    //
    // Two-pass walk:
    //   pass 1: reverse-iterate, skipping bottom-of-stack
    //           WindowSet members. The topmost qualifying non-
    //           chrome window wins (e.g. the just-opened Options
    //           window, or on the title scene, the title menu —
    //           the title set isn't bottom-of-stack, so its
    //           members are valid pass-1 picks).
    //   pass 2: only runs if pass 1 found nothing. Reverse-
    //           iterate including chrome, picking the topmost
    //           focusable. This is the fallback for scenes
    //           where the only focusable windows ARE chrome
    //           (world view with no modals open) — focus the
    //           toolbar so the user has something to navigate.
    //
    // OPENRCT2MINI focus-mode-plan §F.15 follow-up (2026-05-30):
    // the "chrome" filter was originally "WindowFlag::stickToFront"
    // (with a dropdown exception), but that caught user-summoned
    // modals that happen to be stickToFront. LoadSave is the
    // load-bearing case — its window flag set includes
    // stickToFront (LoadSave.cpp:1295), so the old filter skipped
    // it during pass 1, then pass 1 found titleMenu underneath
    // and returned it. The bootstrap saw "topmost unchanged"
    // (titleMenu was already focused) and never re-snapped, so
    // focus stayed on the title screen even after LoadSave
    // opened on top of it. Switching to "member of a bottom-of-
    // stack WindowSet" treats the in-game toolbar set as chrome
    // (the only set with isBottomOfStack=true) while letting
    // every individual dialog through. The dropdown exception is
    // no longer needed because dropdowns aren't set members.
    //
    // _lastTopmostFocusable is updated to the snapped class so
    // the next bootstrap pass doesn't treat this as a fresh
    // "topmost changed" event and re-snap to itself.
    const auto findTopmost = [](bool includeChrome) -> WindowClass {
        for (auto it = gWindowList.rbegin(); it != gWindowList.rend(); ++it)
        {
            auto& wPtr = *it;
            if (wPtr == nullptr)
                continue;
            if (wPtr->flags.has(WindowFlag::dead))
                continue;
            if (wPtr->classification == WindowClass::mainWindow)
                continue;
            // OPENRCT2MINI: purely visual windows (tooltip, map
            // hover-identification overlay) are never a focus
            // target — see WidgetFocus::isPurelyVisualWindow.
            if (WidgetFocus::isPurelyVisualWindow(wPtr->classification))
                continue;
            if (!includeChrome)
            {
                const auto* set = WidgetFocus::findSetFor(wPtr->classification);
                if (set != nullptr && set->isBottomOfStack)
                    continue;
            }
            if (WidgetFocus::firstFocusable(*wPtr) == kWidgetIndexNull)
                continue;
            return wPtr->classification;
        }
        return WindowClass::null;
    };

    WindowClass topmost = findTopmost(/*includeChrome=*/ false);
    if (topmost == WindowClass::null)
        topmost = findTopmost(/*includeChrome=*/ true);

    // OPENRCT2MINI window-set-plan §3.3: don't re-snap when the
    // new topmost is in the same set as the current focus — that's
    // a set-internal navigation, handled by the directional walker
    // setting focus to a sibling directly.
    if (topmost != WindowClass::null && !WidgetFocus::sameSetOrClass(topmost, _focusedWindowClass))
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr)
        {
            // Pick the set's defaultClass for set-aware landing; the
            // widget on that class is resolved by resolveLandingWidget
            // (which consults per-window memory FIRST, then set
            // defaultWidget, then firstFocusable).
            // OPENRCT2MINI focus-memory-plan §4.2.
            WindowClass landingCls = topmost;
            if (const auto* set = WidgetFocus::findSetFor(topmost);
                set != nullptr && set->defaultClass != WindowClass::null)
            {
                auto* dfltW = windowMgr->FindByClass(set->defaultClass);
                if (dfltW != nullptr && !dfltW->flags.has(WindowFlag::dead))
                    landingCls = set->defaultClass;
            }
            const WidgetIndex landingWidget = resolveLandingWidget(landingCls);
            if (landingWidget != kWidgetIndexNull)
            {
                // OPENRCT2MINI focus-mode-plan §F.16: push the old
                // focus onto the history stack before reassigning,
                // so cursor.cancel can later restoreFocus back to
                // it. Only push when we actually have a non-null
                // class to restore — initial snaps from null
                // (fresh activation, post-clearFocus) shouldn't
                // pollute the stack.
                if (_focusedWindowClass != WindowClass::null
                    && _focusedWidget != OpenRCT2::kWidgetIndexNull)
                {
                    constexpr size_t kFocusStackCap = 32;
                    if (_focusStack.empty()
                        || _focusStack.back().first != _focusedWindowClass
                        || _focusStack.back().second != _focusedWidget)
                    {
                        _focusStack.emplace_back(_focusedWindowClass, _focusedWidget);
                        while (_focusStack.size() > kFocusStackCap)
                            _focusStack.erase(_focusStack.begin());
                    }
                }
                setFocus(landingCls, landingWidget);
            }
        }
    }
    _lastTopmostFocusable = topmost;
}

bool InputManager::restoreFocus()
{
    // OPENRCT2MINI focus-mode-plan §F.16: pop until we find a
    // live frame. Stale entries (closed windows) get discarded
    // automatically — that's what lets a chain like
    // titleMenu → dropdown → About land back on titleMenu's
    // Game Tools widget after closing About: the dropdown entry
    // in the middle is dead by the time we pop, so we skip past
    // it and keep popping until we hit titleMenu, which is still
    // alive.
    auto* windowMgr = GetWindowManager();
    if (windowMgr == nullptr)
        return false;
    while (!_focusStack.empty())
    {
        auto entry = _focusStack.back();
        _focusStack.pop_back();
        auto* w = windowMgr->FindByClass(entry.first);
        if (w == nullptr)
            continue;
        if (w->flags.has(WindowFlag::dead))
            continue;
        // Validate the saved widget index against the current
        // widget list (the window might have re-laid-out its
        // widgets between the push and pop). Prefer the stack-saved
        // widget if still valid (that's the exact state at the time
        // the child was opened); otherwise fall through to the
        // per-window memory and the rest of the resolveLandingWidget
        // ladder.
        // OPENRCT2MINI focus-memory-plan §4.4.
        WidgetIndex widget = entry.second;
        if (widget == OpenRCT2::kWidgetIndexNull
            || widget >= w->widgets.size()
            || !WidgetFocus::isFocusable(w->widgets[widget]))
        {
            widget = resolveLandingWidget(entry.first);
        }
        if (widget == OpenRCT2::kWidgetIndexNull)
            continue;
        setFocus(entry.first, widget);
        // Sync the bootstrap's "last topmost" tracker so the
        // next per-frame pass doesn't immediately undo this
        // restore by thinking the topmost changed.
        _lastTopmostFocusable = entry.first;
        return true;
    }
    return false;
}

OpenRCT2::WidgetIndex InputManager::resolveLandingWidget(WindowClass cls)
{
    // OPENRCT2MINI focus-memory-plan §3 + §4 (2026-05-25): one
    // helper, four entry-path callers (cycleFocusedWindow,
    // snapFocusToTopmostFocusable, enterFocusMode, restoreFocus).
    // Consults the per-window memory FIRST so the user lands on the
    // widget they last touched on this window; falls back to the
    // window-set's defaultWidget then firstFocusable. Each candidate
    // is validated against the LIVE widget list so a tab rebuild or
    // resize that invalidated the saved index falls through cleanly.
    if (cls == WindowClass::null)
        return kWidgetIndexNull;
    auto* windowMgr = GetWindowManager();
    if (windowMgr == nullptr)
        return kWidgetIndexNull;
    auto* w = windowMgr->FindByClass(cls);
    if (w == nullptr)
        return kWidgetIndexNull;

    // 1. Per-window memory. The recorded index may be stale (window
    //    re-laid-out its widget list, focusable bit flipped off).
    //    Validate before returning; if it fails, fall through.
    if (auto it = _focusMemory.find(cls); it != _focusMemory.end())
    {
        const auto saved = it->second;
        if (saved != kWidgetIndexNull
            && saved < w->widgets.size()
            && WidgetFocus::isFocusable(w->widgets[saved]))
        {
            return saved;
        }
    }

    // 2. Window-set default. Only applies when cls IS the set's
    //    defaultClass — otherwise the caller picked a non-default
    //    set member intentionally and we shouldn't override that
    //    choice. The defaultWidget index must still be focusable on
    //    the live window.
    if (const auto* set = WidgetFocus::findSetFor(cls);
        set != nullptr && set->defaultClass == cls && set->defaultWidget != kWidgetIndexNull)
    {
        if (set->defaultWidget < w->widgets.size()
            && WidgetFocus::isFocusable(w->widgets[set->defaultWidget]))
        {
            return set->defaultWidget;
        }
    }

    // 3. First focusable widget.
    return WidgetFocus::firstFocusable(*w);
}

void InputManager::clearFocus()
{
    // OPENRCT2MINI focus-ring redraw (2026-05-24): invalidate the
    // about-to-be-cleared window so its lingering yellow ring gets
    // wiped on the next paint. Without this the old ring stays
    // visible until the window happens to be dirtied for other
    // reasons.
    const auto prevCls = _focusedWindowClass;
    _focusedWindowClass = WindowClass::null;
    _focusedWidget = OpenRCT2::kWidgetIndexNull;
    _focusedScrollItem = -1;
    if (prevCls != WindowClass::null)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr)
            windowMgr->InvalidateByClass(prevCls);
    }
    // OPENRCT2MINI focus-mode-plan §F.16: clearing focus mode
    // ends the navigation session — drop the history stack so a
    // future re-activation starts clean. Without this, a stale
    // entry from a previous focus session could fire on the
    // first cancel of the new one and dump the user on some
    // arbitrary widget that hasn't been visible for minutes.
    _focusStack.clear();
    // OPENRCT2MINI focus-mode-plan §F.12: reset the topmost
    // tracking so the next bootstrap pass treats whatever's on top
    // as a fresh topmost arrival. Without this, after focus is
    // cleared the bootstrap's "topmost changed?" check would
    // compare the new topmost against the stale class from the
    // previous focused session, occasionally skipping a snap.
    _lastTopmostFocusable = WindowClass::null;
}

// OPENRCT2MINI cursor-selector-modal-plan §3.1 / CS.1: state setter.
// Single mutation point so future hooks (logging, dirty-rect, OS
// cursor-visibility update) can live here. The mode dictates the
// software-cursor compositor's draw-skip and is consulted by
// resolveActiveContext for widgetFocus eligibility.
void InputManager::setSelectorMode(SelectorMode mode)
{
    if (_selectorMode == mode)
        return;
    _selectorMode = mode;
    // Invalidate the focused window so the focus ring repaints when
    // hidden→active or active→hidden flips happen.
    if (_focusedWindowClass != WindowClass::null)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr)
            windowMgr->InvalidateByClass(_focusedWindowClass);
    }
}

// OPENRCT2MINI cursor-selector-modal-plan v2: simplified state-
// machine transition pump. Two states: active (ring on, cursor
// off) and hidden (cursor on, ring off). The auto-transition
// `hidden → active when a new window opens` lives in the per-
// frame bootstrap, not here — by the time onTransitionEvent
// would fire, the bootstrap has already updated focus.
//
// _lastInputWasRealMouse is updated alongside the state changes
// here so the bootstrap can ignore new-window snaps that came
// from a real-mouse-driven flow.
void InputManager::onTransitionEvent(SelectorTransitionSource src)
{
    switch (src)
    {
        case SelectorTransitionSource::realMouseMotion:
            // Real mouse motion: user is mousing. Cursor mode.
            _lastInputWasRealMouse = true;
            if (_selectorMode != SelectorMode::hidden)
                setSelectorMode(SelectorMode::hidden);
            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment
            // 2026-05-17): mouse motion exits grid-cursor mode too,
            // matching the equivalent cursor.* exit path in
            // ShortcutManager. Without this clear, moving the mouse
            // while in grid mode would leave the latch on — the OS
            // cursor would be visible (selector hidden) but the
            // tool-context strategy would still own the dispatch,
            // and the grid cursor would still render its tile-
            // selection highlight. Symmetric to the cursor.* path:
            // any "I'm using a real pointer now" gesture should
            // drop both modes.
            if (_toolFocusSelected)
                _toolFocusSelected = false;
            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-18
            // — full grid-cursor kick-out on cursor activation): also
            // drop the parked highlight. Without this, the realMouse-
            // Motion handler only clears _toolFocusSelected (the latch)
            // but the grid-cursor flags survive: ToolContext::on-
            // Deactivate set MapSelectFlag::gridCursorParked + kept
            // MapSelectFlag::enable when the tool was still armed, and
            // those flags are what drive the Paint.Surface tile-blink
            // overlay + the software cursor sprite parking. After
            // clearing _toolFocusSelected the user is no longer in any
            // grid-cursor strategy yet the parked tile keeps rendering
            // — exactly the "kick out of grid cursor mode" gesture the
            // user expects to fully end.
            //
            // OPENRCT2MINI Z-plane flicker fix (2026-05-20): only
            // clear MapSelectFlag::enable when we were actually IN
            // grid-cursor mode. Previously this handler unset
            // `enable` on every SDL_MOUSEMOTION — but `enable` is the
            // load-bearing gate for VirtualFloorTileIsFloor (paint/
            // VirtualFloor.cpp:203). During a Footpath shift+drag-Z
            // gesture the user is driving the real mouse, so this
            // handler fires on every motion event and clobbers
            // `enable`. WindowFootpathSetProvisionalPathAtPoint's
            // early-return (windows/Footpath.cpp:1173) then doesn't
            // re-set it, so the Z plane disappears until the next Z
            // step crossing skips the early-return. Gridcursor-flag
            // clears are idempotent and safe — only `enable` needs
            // gating because every world tool (Footpath, Ride-
            // Construction, Scenery, …) relies on it.
            {
                const bool wasInGridCursorMode = gMapSelectFlags.has(MapSelectFlag::gridCursor)
                    || gMapSelectFlags.has(MapSelectFlag::gridCursorParked);
                // OPENRCT2MINI grid-cursor-plan §18.A follow-up
                // (2026-05-24, user report): dirty the selection-rect
                // tiles before clearing the gridCursor flags. The blink
                // pumps (ToolContext::processFrame for active state,
                // InputManager::process for parked state) invalidate
                // the rect on every frame WHILE the cursor is in those
                // states — but the moment the flags clear, neither
                // pump fires, and the rect tiles keep their stale
                // highlight pixels until something else dirties them
                // (user moves the cursor, camera pans, etc.). Same
                // shape as the per-frame fix from commit 790f583bd3 —
                // multi-cell rect via positionA != positionB, single-
                // tile otherwise.
                if (wasInGridCursorMode)
                {
                    if (gMapSelectPositionA != gMapSelectPositionB)
                        MapInvalidateRegion(gMapSelectPositionA, gMapSelectPositionB);
                    else
                        MapInvalidateTileFull(gMapSelectPositionA);
                }
                gMapSelectFlags.unset(MapSelectFlag::gridCursorParked);
                gMapSelectFlags.unset(MapSelectFlag::gridCursor);
                if (wasInGridCursorMode)
                    gMapSelectFlags.unset(MapSelectFlag::enable);
            }
            break;
        case SelectorTransitionSource::realMouseClick:
            // A real-mouse click. Mark the input source so the
            // post-click bootstrap doesn't auto-wake the selector
            // for whichever window the click summoned.
            _lastInputWasRealMouse = true;
            // Mode doesn't change here — if the user was already
            // in hidden (typical real-mouse flow), they stay
            // there. If they were somehow in active (clicked from
            // selector mode via mouse), we leave them; the active
            // session continues.
            break;
        case SelectorTransitionSource::virtualUserInput:
            // Any non-mouse user input — D-pad, keyboard, gamepad
            // button. Records that the most recent input was
            // virtual so the new-window auto-wake fires.
            _lastInputWasRealMouse = false;
            break;
        case SelectorTransitionSource::cursorCancelInBottomSet:
            // Cancel from the bottom-of-stack chrome (toolbar/
            // status bar). Drop to cursor mode. Focus is cleared
            // so the per-frame bootstrap doesn't immediately re-
            // snap to the toolbar (the cursor will pick up via
            // either virtual-cursor polling or real-mouse motion
            // depending on the user's last input source).
            setSelectorMode(SelectorMode::hidden);
            clearFocus();
            break;
        case SelectorTransitionSource::wakeCursorRequested:
            // Explicit "give me the cursor" shortcut.
            if (_selectorMode != SelectorMode::hidden)
                setSelectorMode(SelectorMode::hidden);
            break;
        case SelectorTransitionSource::enterFocusModeRequested:
            // Explicit "give me the selector" shortcut. Also
            // marks input as virtual so any newly-opened windows
            // after this stay in selector mode.
            _lastInputWasRealMouse = false;
            if (_selectorMode != SelectorMode::active)
                setSelectorMode(SelectorMode::active);
            break;
    }
}

// OPENRCT2MINI grid-cursor-plan §12.1 (amended): single-site mutator
// for the tool-focus selector. Routes through SelectorTransitionSource
// so future logging / SelectorMode invalidation can hook one place
// rather than chasing writes around the source tree. `src` is
// currently informational — we don't gate on it — but keeping it on
// the signature mirrors onTransitionEvent's contract and gives later
// patches a hook.
void InputManager::setToolFocusSelected(bool selected, SelectorTransitionSource src)
{
    (void)src;
    _toolFocusSelected = selected;
}

// OPENRCT2MINI active-window-emphasis plan §4.2: convert SDL KMOD_*
// bits (uint32_t, with separate L/R sides) to the collapsed
// ModifierKey bit layout (uint8_t, sides collapsed). The two are NOT
// bit-aligned — see plan §5.5 for the verified KMOD vs ModifierKey
// table. `kUsefulModifiers` here mirrors ShortcutInput.cpp:157.
static uint8_t KmodToModifierKey(uint32_t kmod) noexcept
{
    constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;
    kmod &= kUsefulModifiers;
    uint8_t mask = 0;
    if (kmod & KMOD_SHIFT)
        mask |= EnumValue(ModifierKey::shift);
    if (kmod & KMOD_CTRL)
        mask |= EnumValue(ModifierKey::ctrl);
    if (kmod & KMOD_ALT)
        mask |= EnumValue(ModifierKey::alt);
    if (kmod & KMOD_GUI)
        mask |= EnumValue(ModifierKey::cmd);
    return mask;
}

void InputManager::cycleFocusedWindow(int direction)
{
    // Build the focusable-window list in z-order (front-of-list =
    // bottom of stack, back = top). Same skip rules as the bootstrap:
    // dead windows and the world viewport (mainWindow) are out;
    // windows with zero focusable widgets are out (nothing to land on).
    //
    // Walking the list once into a flat vector keeps the cycle step
    // O(n) and lets us cleanly wrap. Reusing gWindowList iterators
    // here would be fragile — windows can be added / removed mid-
    // game, but this lambda runs from a shortcut action lambda so
    // the focus state is checked AGAINST the list snapshot we
    // capture, not against pointers that might dangle.
    // OPENRCT2MINI grid-cursor-plan §12.1 (amended): each cycle entry
    // is either a real window class OR the virtual "tool viewport"
    // sentinel. The vector stores pairs <class, isToolViewport>.
    // The virtual entry has class == WindowClass::null + isVirtual=true.
    // It's only added if a tool is currently armed AND a recognised
    // tool window is in the snapshot — it sits IMMEDIATELY AFTER the
    // entry for the armed tool window so cycle-next from the tool
    // window steps "into the world", and cycle-next from there
    // steps onward to the next real window.
    struct CycleEntry
    {
        WindowClass cls;
        bool isToolViewport;
    };

    // OPENRCT2MINI focus-mode-plan §F.cycle (open-order stability):
    // build the cycle list from _windowCycleOrder (a persistent open-
    // order list of cycleable window classes) rather than walking
    // gWindowList z-order. Each cycle call we first sync the list:
    //   (a) remove entries whose backing window has closed,
    //   (b) append any newly-opened focusable windows (set-deduped)
    //       at the END.
    // The cycle key for a window is its set's defaultClass when it
    // belongs to a WindowSet, otherwise its own classification —
    // matches the existing set-dedup rule so a set acts as one stop.
    auto* windowMgrSync = GetWindowManager();
    const auto cycleKeyFor = [](const WindowBase& w) -> WindowClass {
        const auto* set = WidgetFocus::findSetFor(w.classification);
        if (set != nullptr && set->defaultClass != WindowClass::null)
            return set->defaultClass;
        return w.classification;
    };
    const auto isFocusableForCycle = [](const WindowBase& w) {
        if (w.flags.has(WindowFlag::dead))
            return false;
        if (w.classification == WindowClass::mainWindow)
            return false;
        if (WidgetFocus::isPurelyVisualWindow(w.classification))
            return false;
        return WidgetFocus::firstFocusable(w) != kWidgetIndexNull;
    };
    // (a) Prune closed/no-longer-focusable entries. Liveness is
    // checked by FindByClass — sets resolve via the same canonical
    // key (defaultClass) so a set's siblings closing doesn't remove
    // the entry as long as the canonical class is still open.
    if (windowMgrSync != nullptr)
    {
        _windowCycleOrder.erase(
            std::remove_if(
                _windowCycleOrder.begin(), _windowCycleOrder.end(),
                [&](WindowClass cls) {
                    auto* w = windowMgrSync->FindByClass(cls);
                    return w == nullptr || !isFocusableForCycle(*w);
                }),
            _windowCycleOrder.end());
    }
    // (b) Append any newly-seen focusable windows in z-order — the
    // first time we encounter a cycle key we don't yet know about,
    // it gets the next slot in open order.
    const auto knownInOrder = [&](WindowClass cls) {
        return std::find(_windowCycleOrder.begin(), _windowCycleOrder.end(), cls)
            != _windowCycleOrder.end();
    };
    for (auto& w : gWindowList)
    {
        if (w == nullptr || !isFocusableForCycle(*w))
            continue;
        const auto key = cycleKeyFor(*w);
        if (!knownInOrder(key))
            _windowCycleOrder.push_back(key);
    }
    // Build the cycle list — pure open-order, no virtual entries
    // here (tool viewport sentinel handled below the same way as
    // before).
    std::vector<CycleEntry> focusable;
    focusable.reserve(_windowCycleOrder.size() + 1);
    for (auto cls : _windowCycleOrder)
        focusable.push_back({ cls, false });

    // OPENRCT2MINI grid-cursor-plan §12.1 (amended): if a tool is
    // armed and a tool window is in the snapshot, insert the virtual
    // "tool viewport" entry immediately after that tool window.
    // Footpath / Land / Water / Scenery / RideConstruction /
    // LandRights / TileInspector are recognised — same set as
    // resolveActiveContext's tool-context arm.
    if (gInputFlags.has(InputFlag::toolActive))
    {
        static constexpr WindowClass kToolClasses[] = {
            WindowClass::footpath,         WindowClass::land,             WindowClass::water,
            WindowClass::scenery,          WindowClass::rideConstruction, WindowClass::landRights,
            WindowClass::tileInspector,    WindowClass::clearScenery,     WindowClass::patrolArea,
            WindowClass::peep,
        };
        const auto isToolWindowClass = [](WindowClass c) {
            for (auto t : kToolClasses)
                if (c == t)
                    return true;
            return false;
        };
        for (size_t i = 0; i < focusable.size(); i++)
        {
            if (!focusable[i].isToolViewport && isToolWindowClass(focusable[i].cls))
            {
                // Sentinel: WindowClass::null + isToolViewport=true.
                focusable.insert(
                    focusable.begin() + static_cast<std::ptrdiff_t>(i + 1),
                    CycleEntry{ WindowClass::null, true });
                break;
            }
        }
    }

    if (focusable.empty())
        return;
    if (focusable.size() == 1)
    {
        // Single qualifying entry — snap to it if we weren't already
        // there, no-op otherwise.
        const auto& only = focusable[0];
        if (only.isToolViewport)
        {
            if (!_toolFocusSelected)
            {
                clearFocus();
                setToolFocusSelected(true, SelectorTransitionSource::virtualUserInput);
            }
            return;
        }
        if (_focusedWindowClass != only.cls || _toolFocusSelected)
        {
            setToolFocusSelected(false, SelectorTransitionSource::virtualUserInput);
            auto* windowMgr = GetWindowManager();
            if (windowMgr != nullptr)
            {
                auto* w = windowMgr->FindByClass(only.cls);
                if (w != nullptr)
                    setFocus(only.cls, WidgetFocus::firstFocusable(*w));
            }
        }
        return;
    }

    // Locate the current focus in the snapshot. If we're not
    // currently focused on any qualifying entry (focus cleared,
    // or focused on a non-focusable window like loadSave) start
    // from "just before the front" for forward, "just after the
    // back" for backward — that way the first step lands on
    // index 0 / index size-1. Set-aware: a member of a set
    // matches the set's entry in the cycle. If the user is on
    // the virtual tool entry, currentIdx finds it by isToolViewport.
    int currentIdx = -1;
    for (size_t i = 0; i < focusable.size(); i++)
    {
        if (focusable[i].isToolViewport)
        {
            if (_toolFocusSelected)
            {
                currentIdx = static_cast<int>(i);
                break;
            }
            continue;
        }
        if (!_toolFocusSelected && WidgetFocus::sameSetOrClass(focusable[i].cls, _focusedWindowClass))
        {
            currentIdx = static_cast<int>(i);
            break;
        }
    }
    int nextIdx;
    if (currentIdx < 0)
    {
        nextIdx = (direction > 0) ? 0 : static_cast<int>(focusable.size()) - 1;
    }
    else
    {
        const int n = static_cast<int>(focusable.size());
        nextIdx = ((currentIdx + direction) % n + n) % n; // safe mod for negative direction
    }

    // OPENRCT2MINI grid-cursor-plan §12.1 (amended): if we landed on
    // the virtual tool viewport entry, set the latch and clear focus.
    // resolveActiveContext will route to the tool's strategy because
    // _toolFocusSelected is true AND toolActive is set; the cleared
    // focus prevents the widgetFocus arm from firing.
    if (focusable[nextIdx].isToolViewport)
    {
        clearFocus();
        setToolFocusSelected(true, SelectorTransitionSource::virtualUserInput);
        return;
    }

    // Landing on a real window — make sure the latch is cleared so
    // the tool-context arm doesn't shadow widgetFocus on subsequent
    // frames.
    setToolFocusSelected(false, SelectorTransitionSource::virtualUserInput);

    const auto nextCls = focusable[nextIdx].cls;
    auto* windowMgr = GetWindowManager();
    if (windowMgr == nullptr)
        return;
    // OPENRCT2MINI window-set-plan §3.5: if the target is in a set,
    // land on the set's defaultClass; the widget on that class is
    // resolved by resolveLandingWidget (per-window memory FIRST,
    // then set defaultWidget, then firstFocusable).
    // OPENRCT2MINI focus-memory-plan §4.1.
    WindowClass landingCls = nextCls;
    if (const auto* set = WidgetFocus::findSetFor(nextCls);
        set != nullptr && set->defaultClass != WindowClass::null)
    {
        landingCls = set->defaultClass;
    }
    auto* w = windowMgr->FindByClass(landingCls);
    if (w == nullptr)
        return;
    const WidgetIndex landingWidget = resolveLandingWidget(landingCls);
    if (landingWidget == kWidgetIndexNull)
        return;
    setFocus(landingCls, landingWidget);
    // BringToFront so the focused window becomes the topmost — the
    // user's mental model is "I'm cycling to this window, it's now
    // the active one." Without this the focus ring would jump to a
    // back-layered window while the topmost still draws over it.
    windowMgr->BringToFrontByClass(landingCls);
    w->invalidate();

    // OPENRCT2MINI active-window-emphasis plan §4.3: arm the cycle
    // highlight latch. The white focus outline stays drawn on
    // `landingCls` until the user releases whichever modifier the
    // cycle binding requires (or the fallback timeout fires for
    // modifier-less rebinds). Source the modifier mask from the
    // binding's own ShortcutInput::modifiers — robust against any
    // custom rebind, doesn't depend on hardcoded shift/ctrl.
    {
        constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;
        auto& shortcutMgr = GetShortcutManager();
        uint32_t kmodUnion = 0;
        if (const auto* sc = shortcutMgr.getShortcut(ShortcutId::kInterfaceCycleNextWindow))
            for (const auto& input : sc->current)
                kmodUnion |= (input.modifiers & kUsefulModifiers);
        if (const auto* sc = shortcutMgr.getShortcut(ShortcutId::kInterfaceCyclePreviousWindow))
            for (const auto& input : sc->current)
                kmodUnion |= (input.modifiers & kUsefulModifiers);
        // If a previous cycle target was still highlighted, invalidate
        // it so its outline pixels get repainted as the latch moves
        // to the new window. Without this, a fast Shift+Tab sequence
        // leaves stale white-outline pixels on each previously-cycled
        // window. Same pattern as the active-window tracking in
        // WindowDrawAll's static prev-active cleanup (plan §2.4
        // adapted to the cycle latch).
        constexpr WindowClass kCycleHighlightNoClassArm = static_cast<WindowClass>(255);
        if (_cycleHighlightClass != kCycleHighlightNoClassArm
            && _cycleHighlightClass != landingCls)
        {
            // windowMgr is already in scope from earlier in this
            // function (the BringToFrontByClass call site above).
            if (auto* prev = windowMgr->FindByClass(_cycleHighlightClass))
                prev->invalidate();
        }
        _cycleHighlightClass = landingCls;
        _cycleHighlightWidget = landingWidget;
        _cycleHighlightModifierMask = KmodToModifierKey(kmodUnion);
        // Minimum dwell time. Even when the cycle binding has a
        // modifier prefix (SHIFT+TAB default), the user typically
        // releases SHIFT within one frame of pressing TAB — too
        // fast for the eye to register the outline. Always arm
        // the timeout to 500 ms; the release-pass keeps the latch
        // while EITHER the modifier is held OR nowMs < untilMs.
        // For pure modifier-less rebinds, the modifier branch is
        // a no-op so the 500 ms is the only persistence mechanism.
        _cycleHighlightUntilMs = SDL_GetTicks() + 500;
    }
}

bool InputManager::enterFocusModeOnTopmost()
{
    // OPENRCT2MINI bug 2026-05-22: tool-armed shortcut — if the user
    // is in cursor mode (selectorMode::hidden) AND any tool-like
    // work is mid-flight, the enter-focus-mode shortcut should
    // engage grid cursor mode directly instead of landing widget
    // focus on the tool window. Use case: user is mid-bridge-build,
    // picks up the mouse to do something, then presses Start to
    // "give me my gamepad back". They want grid cursor on the
    // bridge head, not widget focus on the path-type dropdown.
    // The cycle behavior below still fires when already in focus
    // mode.
    //
    // "Tool-like work" detection — two paths:
    //   1. gInputFlags::toolActive — gamepad-driven tools that
    //      keep the map tool armed (controller bridge build, OnLand
    //      path placement, etc.).
    //   2. WindowFootpathGetInputMode() != none — the mouse-coord
    //      WindowFootpathStartBridgeAtPoint runs ToolCancel after
    //      anchoring (toolActive == false) but the Footpath window
    //      is still in bridgeOrTunnel mode and the user wants to
    //      continue from gamepad. Re-arm the tool widget for the
    //      current mode here so resolveActiveContext routes to
    //      FootpathContextImpl on the next frame and the bridge
    //      head sync runs as expected.
    const bool footpathInMode
        = (Windows::WindowFootpathGetInputMode() != Windows::FootpathInputMode::none);
    // OPENRCT2MINI ride-construction-grid-cursor-plan §9 (Phase 1, 2026-05-25):
    // mirror the Footpath in-mode re-arm for RideConstruction. Same scenario:
    // the user opens the Ride window via mouse, places a piece, the underlying
    // ToolCancel drops toolActive (or never armed it), but the window's
    // construction state is live — Start should engage grid cursor on the
    // track head.
    const bool rideInMode
        = (Windows::WindowRideConstructionGetInputMode() != Windows::RideInputMode::none);
    if (_selectorMode == SelectorMode::hidden
        && (gInputFlags.has(InputFlag::toolActive) || footpathInMode || rideInMode))
    {
        if (footpathInMode && !gInputFlags.has(InputFlag::toolActive))
            Windows::WindowFootpathReArmForCurrentMode();
        if (rideInMode && !gInputFlags.has(InputFlag::toolActive))
            Windows::WindowRideConstructionReArmForCurrentMode();
        clearFocus();
        setToolFocusSelected(true, SelectorTransitionSource::enterFocusModeRequested);
        requestFocusMode();
        onTransitionEvent(SelectorTransitionSource::enterFocusModeRequested);
        return true;
    }

    // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up: the TAB
    // (kInterfaceEnterFocusMode) action. First press from cursor mode
    // lands on the first focusable widget of the topmost focusable
    // window. Subsequent presses while focus mode is already active in
    // that same window (or set member) advance through the window's
    // focusable widgets in declaration order, wrapping back to the
    // first when the end is hit. See WidgetFocus::nextFocusable for
    // the filter (mirrors firstFocusable: focusable predicate +
    // dropdown chevron skip).
    //
    // Same two-pass walk as snapFocusToTopmostFocusable: prefer non-
    // stickToFront windows so a newly-opened modal beats always-on-top
    // chrome; fall back to including stickToFront for scenes where the
    // only focusable windows ARE chrome (e.g. world view with no
    // modals open — focus the toolbar so the user has something to
    // navigate).
    const auto findTopmost = [](bool includeStickToFront) -> WindowClass {
        for (auto it = gWindowList.rbegin(); it != gWindowList.rend(); ++it)
        {
            auto& wPtr = *it;
            if (wPtr == nullptr)
                continue;
            if (wPtr->flags.has(WindowFlag::dead))
                continue;
            if (wPtr->classification == WindowClass::mainWindow)
                continue;
            // OPENRCT2MINI: purely visual windows (tooltip, map
            // hover-identification overlay) are never a focus target —
            // mirrors the per-frame bootstrap / snap path filters.
            // Without this, an in-flight tooltip popup would race the
            // TAB-press: TAB enters focus mode on whatever window is
            // topmost at that instant; if the tooltip overlay is on
            // top, focus latches on its (zero-widget) frame and the
            // user can't see or interact with anything.
            if (WidgetFocus::isPurelyVisualWindow(wPtr->classification))
                continue;
            if (!includeStickToFront && wPtr->flags.has(WindowFlag::stickToFront)
                && wPtr->classification != WindowClass::dropdown)
                continue;
            if (WidgetFocus::firstFocusable(*wPtr) == kWidgetIndexNull)
                continue;
            return wPtr->classification;
        }
        return WindowClass::null;
    };
    WindowClass topmost = findTopmost(/*includeStickToFront=*/ false);
    if (topmost == WindowClass::null)
        topmost = findTopmost(/*includeStickToFront=*/ true);
    if (topmost == WindowClass::null)
        return false; // No focusable window — no-op per spec.

    auto* windowMgr = GetWindowManager();
    if (windowMgr == nullptr)
        return false;

    // OPENRCT2MINI window-set-plan §3.5: if the topmost is in a set,
    // honour its defaultClass; the widget on that class is resolved
    // by resolveLandingWidget (per-window memory FIRST, then set
    // defaultWidget, then firstFocusable).
    // OPENRCT2MINI focus-memory-plan §4.3.
    WindowClass landingCls = topmost;
    if (const auto* set = WidgetFocus::findSetFor(topmost); set != nullptr
        && set->defaultClass != WindowClass::null)
    {
        if (auto* dfltW = windowMgr->FindByClass(set->defaultClass);
            dfltW != nullptr && !dfltW->flags.has(WindowFlag::dead))
        {
            landingCls = set->defaultClass;
        }
    }
    WidgetIndex landingWidget = resolveLandingWidget(landingCls);
    if (landingWidget == kWidgetIndexNull)
        return false;

    // OPENRCT2MINI focus-mode-plan §F.cycle: cycle when the user re-
    // presses Enter-Focus-Mode while focus mode is already active on
    // the topmost window or any member of its set. First press from
    // cursor mode falls through to the landingCls/landingWidget snap
    // above; subsequent presses advance through the focused window's
    // widgets in declaration order and, when they run out, across set
    // members (kInGameChrome: topToolbar↔bottomToolbar; kTitleScene:
    // titleMenu→titleOptions→titleExit→titleLogo). The set-aware
    // walker honours WindowFlag::dead and skips members with no
    // focusable widgets, so a closed set member doesn't stall the
    // cycle. defaultClass is only consulted on first entry — once the
    // user has navigated, cycle stays where they are.
    if (_selectorMode == SelectorMode::active && _activeContext == InputContext::widgetFocus
        && _focusedWindowClass != WindowClass::null && _focusedWidget != kWidgetIndexNull)
    {
        if (auto* focusedWin = windowMgr->FindByClass(_focusedWindowClass);
            focusedWin != nullptr && !focusedWin->flags.has(WindowFlag::dead)
            && WidgetFocus::sameSetOrClass(_focusedWindowClass, topmost))
        {
            WindowClass nextCls = _focusedWindowClass;
            const auto nextIdx = WidgetFocus::nextFocusableInSet(
                _focusedWindowClass, _focusedWidget, &nextCls);
            if (nextIdx != kWidgetIndexNull)
            {
                landingCls = nextCls;
                landingWidget = nextIdx;
            }
        }
    }

    const auto previousFocusCls = _focusedWindowClass;
    setFocus(landingCls, landingWidget);
    // OPENRCT2MINI focus-mode-plan §F.cycle: setFocus only mutates
    // the InputManager fields — windows don't get a redraw signal,
    // so the focus ring would stay painted on the previously-
    // focused widget until something else dirtied the window.
    // Mirror what WidgetFocusContextImpl::onShortcut (arrow keys)
    // and cycleFocusedWindow do: invalidate the landing window
    // (and the previous one, if different, so its ring clears).
    windowMgr->InvalidateByClass(landingCls);
    if (previousFocusCls != WindowClass::null && previousFocusCls != landingCls)
        windowMgr->InvalidateByClass(previousFocusCls);
    // Sync the bootstrap's "last topmost" tracker so the next per-
    // frame pass doesn't treat this snap as a stale "topmost changed"
    // event and re-snap on top of us.
    _lastTopmostFocusable = landingCls;

    // Mark this as virtual input + flip into selector active. The
    // requestFocusMode latch is what lets users without
    // widgetFocusAlwaysOn get widgetFocus promoted in resolveActive-
    // Context; the transition event handles the SelectorMode flip
    // and clears _lastInputWasRealMouse so subsequent new windows
    // stay in selector mode.
    requestFocusMode();
    onTransitionEvent(SelectorTransitionSource::enterFocusModeRequested);
    return true;
}

void InputManager::queueInputEvent(const SDL_Event& e)
{
    // OPENRCT2MINI gamepad-plan 1.2: encoding for axis-as-button entries
    // in _heldGamepadButtons uses the kPadAxisAsButtonBase + encodeAxisAs-
    // Button helpers exported from ShortcutManager.h (single source of
    // truth). Triggers and stick directions enter / leave the held-set
    // on press-threshold crossings.
    constexpr int32_t kPadAxisPressThreshold = 16384;
    constexpr int32_t kPadAxisReleaseThreshold = 9830; // ~30%, hysteresis

    switch (e.type)
    {
        case SDL_CONTROLLERAXISMOTION:
        {
            // OPENRCT2MINI gamepad-plan 1.3: emit joyAxis InputEvents
            // ONLY on threshold-crossing transitions (rising edge →
            // state=down, falling edge → state=release). Every motion
            // sample arriving here would otherwise become a fresh
            // shortcut-fire opportunity, and a partially-held trigger
            // (e.g. 60% pressure) would generate hundreds of events
            // per second as the user's analog reading wobbles. Use
            // _heldGamepadButtons membership before/after the update
            // as the transition signal.
            //
            // Sticks emit two events on a strong push (one for the
            // primary direction crossing, none for the orthogonal axis
            // until it also crosses). Triggers emit one event per
            // press and one per release.
            const int32_t axis = e.caxis.axis;
            const int32_t value = e.caxis.value;
            const bool isStick = (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY
                                  || axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY);
            const bool isTrigger
                = (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            if (isStick || isTrigger)
            {
                auto updateHeldAndEmit = [&](int8_t direction) {
                    const uint32_t encoded = encodeAxisAsButton(axis, direction);
                    const bool wasHeld = _heldGamepadButtons.find(encoded) != _heldGamepadButtons.end();
                    bool nowHeld = wasHeld;
                    if (direction > 0)
                    {
                        if (!wasHeld && value >= kPadAxisPressThreshold)
                            nowHeld = true;
                        else if (wasHeld && value < kPadAxisReleaseThreshold)
                            nowHeld = false;
                    }
                    else
                    {
                        if (!wasHeld && value <= -kPadAxisPressThreshold)
                            nowHeld = true;
                        else if (wasHeld && value > -kPadAxisReleaseThreshold)
                            nowHeld = false;
                    }
                    if (nowHeld == wasHeld)
                        return; // no transition; suppress event

                    if (nowHeld)
                        _heldGamepadButtons.insert(encoded);
                    else
                        _heldGamepadButtons.erase(encoded);

                    // Only press transitions trigger shortcut firing
                    // (RegisteredShortcut::isSuitableInputEvent rejects
                    // release events anyway, so the release-event path
                    // here is mostly for symmetry with future hold-shortcut
                    // bookkeeping).
                    InputEvent ie;
                    ie.deviceKind = InputDeviceKind::joyAxis;
                    ie.modifiers = SDL_GetModState();
                    ie.button = axis;
                    ie.state = nowHeld ? InputEventState::down : InputEventState::release;
                    // Synthesise axisValue at the threshold so the
                    // matches() logic in ShortcutInput evaluates true on
                    // press: positive direction → +threshold, negative
                    // → -threshold. Real-time value is irrelevant once
                    // we've committed to "press" / "release".
                    ie.axisValue = static_cast<int16_t>(direction > 0 ? kPadAxisPressThreshold : -kPadAxisPressThreshold);
                    queueInputEvent(std::move(ie));
                };
                updateHeldAndEmit(+1);
                if (isStick)
                    updateHeldAndEmit(-1);
            }
            break;
        }
        case SDL_JOYHATMOTION:
        {
            if (e.jhat.value != SDL_HAT_CENTERED)
            {
                InputEvent ie;
                ie.deviceKind = InputDeviceKind::joyHat;
                ie.modifiers = SDL_GetModState();
                ie.button = e.jhat.value;
                ie.state = InputEventState::down;
                ie.axisValue = 0;
                queueInputEvent(std::move(ie));
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        {
            // OPENRCT2MINI gamepad-plan 1.2: update held-set BEFORE
            // queueing so by the time the event reaches process() the
            // held-set already includes the newly-pressed button —
            // important for shortcuts that bind THIS button as their
            // action and expect chord prerequisites already in the set.
            // For modifier-only chords (e.g. PAD L1+R1), the second
            // press must observe the first as held in this set.
            _heldGamepadButtons.insert(e.cbutton.button);

            InputEvent ie;
            ie.deviceKind = InputDeviceKind::joyButton;
            ie.modifiers = SDL_GetModState();
            ie.button = e.cbutton.button;
            ie.state = InputEventState::down;
            ie.axisValue = 0;
            queueInputEvent(std::move(ie));
            break;
        }
        case SDL_CONTROLLERBUTTONUP:
        {
            _heldGamepadButtons.erase(e.cbutton.button);

            InputEvent ie;
            ie.deviceKind = InputDeviceKind::joyButton;
            ie.modifiers = SDL_GetModState();
            ie.button = e.cbutton.button;
            ie.state = InputEventState::release;
            ie.axisValue = 0;
            queueInputEvent(std::move(ie));
            break;
        }
        // OPENRCT2MINI: SDL_JOYBUTTONDOWN / SDL_JOYBUTTONUP intentionally
        // dropped from the dispatch. checkJoysticks() above only opens
        // devices via SDL_GameControllerOpen — when SDL recognises a
        // device as a game controller it opens it BOTH as a controller
        // (firing SDL_CONTROLLERBUTTON* events with canonicalised
        // SDL_CONTROLLER_BUTTON_* enum values) AND as a raw joystick
        // (firing SDL_JOYBUTTON* events with the device's hardware
        // button index). The two events share `e.cbutton.button` /
        // `e.jbutton.button` storage but the values are unrelated:
        // controller "LEFTSHOULDER" is enum 9, the DS4's raw L1 joy-
        // button index also happens to be 9 — so handling both arms
        // through a fall-through case fired the same shortcut twice
        // for one physical press (manifested as L1/R1 double-rotating
        // the view). Since every binding token in the shortcut system
        // (`PAD L1`, `PAD A`, etc.) decodes to a controller-button
        // enum value, the joybutton events would only ever match by
        // collision and never by intent. Dropping the arm eliminates
        // the collision without losing any legitimate binding match.
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        {
            // Force joystick refresh on next check
            _lastJoystickCheck = 0;
            break;
        }
    }
}

void InputManager::queueInputEvent(InputEvent&& e)
{
    _events.push(e);
}

void InputManager::checkJoysticks()
{
    constexpr uint32_t kCheckInternalMs = 5000;

    auto tick = SDL_GetTicks();
    // OPENRCT2MINI 2026-05-24: fire the first scan immediately. The
    // prior `tick > _lastJoystickCheck + 5000` condition required
    // `tick > 5000` on the first call because `_lastJoystickCheck`
    // value-inits to 0, meaning controllers stayed unopened (and
    // therefore unresponsive) for the first 5 seconds of every
    // launch. The SDL_CONTROLLERDEVICEADDED event handler resets
    // `_lastJoystickCheck = 0` to force a rescan — that's also a
    // first-scan sentinel and should fire immediately for the same
    // reason. Either condition now lets the open run; once the first
    // scan stamps a non-zero tick, the 5-second cadence resumes for
    // background hot-plug polling.
    if (_lastJoystickCheck == 0 || tick > _lastJoystickCheck + kCheckInternalMs)
    {
        _lastJoystickCheck = tick;

        _gameControllers.clear();
        auto numJoysticks = SDL_NumJoysticks();
        for (auto i = 0; i < numJoysticks; i++)
        {
            if (SDL_IsGameController(i))
            {
                auto gameController = SDL_GameControllerOpen(i);
                if (gameController != nullptr)
                {
                    _gameControllers.push_back(gameController);
                }
            }
        }
    }
}

void InputManager::processAnalogueInput()
{
    _analogueScroll.x = 0;
    _analogueScroll.y = 0;

    // OPENRCT2MINI gamepad-plan 1.9 follow-on: route camera scroll
    // through the bindable kViewScrollUp/Down/Left/Right shortcuts
    // instead of polling SDL_CONTROLLER_AXIS_RIGHTX/Y directly. This
    // makes the right-stick → camera mapping a default that the user
    // can rebind via the Input Bindings window, just like any other
    // input.
    //
    // For each of the four directions we ask InputManager::
    // getAnalogState which returns 0.0..1.0 — analog magnitude for
    // joyAxis bindings (live stick deflection past the 8000 / 24%
    // analog deadzone) or 1.0 for digital bindings that are held
    // (keyboard arrow keys etc.). Net X / Y velocity is the
    // difference of opposite directions; the existing accumulator
    // and pixel-extraction logic stays unchanged.
    auto& shortcutMgr = GetShortcutManager();
    const auto* scrollUp = shortcutMgr.getShortcut(ShortcutId::kViewScrollUp);
    const auto* scrollDown = shortcutMgr.getShortcut(ShortcutId::kViewScrollDown);
    const auto* scrollLeft = shortcutMgr.getShortcut(ShortcutId::kViewScrollLeft);
    const auto* scrollRight = shortcutMgr.getShortcut(ShortcutId::kViewScrollRight);

    const float upMag = (scrollUp != nullptr) ? getAnalogState(*scrollUp) : 0.0f;
    const float downMag = (scrollDown != nullptr) ? getAnalogState(*scrollDown) : 0.0f;
    const float leftMag = (scrollLeft != nullptr) ? getAnalogState(*scrollLeft) : 0.0f;
    const float rightMag = (scrollRight != nullptr) ? getAnalogState(*scrollRight) : 0.0f;

    // Net velocity (signed). Right stick conventions: positive Y =
    // scroll down, positive X = scroll right (matches SDL axis sign).
    float rawX = rightMag - leftMag;
    float rawY = downMag - upMag;

    if (Config::Get().general.gamepadInvertCameraY)
        rawY = -rawY;

    if (rawX != 0.0f || rawY != 0.0f)
    {
        const float sensitivity = Config::Get().general.gamepadCameraSensitivity;
        const float sensitivityCurve = sensitivity * sensitivity;
        const float moveX = rawX * sensitivityCurve * 8.0f; // base scale
        const float moveY = rawY * sensitivityCurve * 8.0f;

        _analogueScrollAccumX += moveX;
        _analogueScrollAccumY += moveY;

        float intPartX, intPartY;
        const float fracX = std::modf(_analogueScrollAccumX, &intPartX);
        const float fracY = std::modf(_analogueScrollAccumY, &intPartY);

        _analogueScrollAccumX = fracX;
        _analogueScrollAccumY = fracY;

        _analogueScroll.x += static_cast<int>(intPartX);
        _analogueScroll.y += static_cast<int>(intPartY);
    }
    else
    {
        // Centred — bleed off any residual fractional accumulator so
        // a tiny sub-pixel offset doesn't sit forever.
        _analogueScrollAccumX = 0.0f;
        _analogueScrollAccumY = 0.0f;
    }
}

void InputManager::updateAnalogueScroll()
{
    _viewScroll.x += _analogueScroll.x;
    _viewScroll.y += _analogueScroll.y;
}

void InputManager::process()
{
    // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up: reset the
    // "auto-woke this process()" flag at the very top of every
    // process() call. The bootstrap below may re-arm it on the
    // hidden→active edge; consumers (WidgetFocusContextImpl) read it
    // during the processEvents() pass that runs later in this same
    // call. See the field definition in InputManager.h for the full
    // rationale.
    _autoWokeThisProcess = false;

    // OPENRCT2MINI gamepad-plan 1.6b step 1: resolve the active input
    // context once per frame. Cached on _activeContext so every
    // consumer within this frame sees the same value (a window
    // opening / closing later in the frame won't surprise downstream
    // consumers). resolveActiveContext is the single source of truth
    // — every "what mode is the user in?" check in the input
    // pipeline asks the manager via getActiveContext().
    //
    // OPENRCT2MINI focus-mode-plan / Phase F.3: per-frame focus
    // lifecycle. Three duties:
    //
    //   (1) Staleness — if the focused window class no longer
    //       resolves to a live window (user closed it between
    //       frames), reset the focus sentinels. Without this the
    //       widgetFocus context would latch on a dead class id
    //       forever.
    //
    //   (2) Bootstrap — if focus mode is wanted (either because
    //       widgetFocusAlwaysOn is on or the user pressed the
    //       explicit shortcut) AND nothing is currently focused,
    //       walk the window list from top to bottom and snap to the
    //       first focusable widget of the topmost qualifying
    //       window. This is what makes focus mode "always on" feel
    //       seamless: the moment a focusable window appears, focus
    //       latches onto it.
    //
    //   (3) _focusModeRequested auto-clear — if the user pressed
    //       the explicit toggle but nothing focusable exists,
    //       drop the request so a stray press in the world view
    //       doesn't leave the flag dangling.
    //
    // Order matters: stale-check first (so the bootstrap doesn't
    // think a dead window is still focused), then bootstrap,
    // then auto-clear the request flag if it failed to latch.
    if (_focusedWindowClass != WindowClass::null && getFocusedWindow() == nullptr)
        clearFocus();

    {
        const bool shouldBeFocused = Config::Get().general.widgetFocusAlwaysOn || _focusModeRequested;
        // OPENRCT2MINI focus-mode-plan §F.12: topmost-tracking
        // bootstrap. The bootstrap needs to fire in three cases:
        //
        //   (1) Fresh start — _focusedWindowClass is null.
        //   (2) Stale latch — the focused window class no longer
        //       resolves to a live window (handled by the staleness
        //       reset above; this loop then catches the resulting
        //       null and bootstraps).
        //   (3) New window appeared on top — e.g. user opens a
        //       dropdown from a focused menu button. Detect this by
        //       comparing the current topmost focusable class to the
        //       one we recorded last frame; only snap when the
        //       topmost actually CHANGED. This is the load-bearing
        //       distinction vs. "force focus = topmost every frame":
        //       cycle-next/prev shortcuts can land focus on a
        //       window that BringToFront couldn't actually raise
        //       (stickToBack rivals like the title menu vs. logo),
        //       and an unconditional snap-to-topmost would
        //       immediately undo the cycle.
        // Two-pass walk: prefer non-chrome windows so a newly-
        // opened modal beats always-on-top chrome (game bottom
        // toolbar, etc.). Mirrors snapFocusToTopmostFocusable. The
        // fallback pass (includeChrome=true) handles scenes where
        // the only focusable windows ARE chrome.
        //
        // OPENRCT2MINI focus-mode-plan §F.15 follow-up (2026-05-30):
        // the "chrome" filter is now "member of a bottom-of-stack
        // WindowSet" (kInGameChromeMembers has isBottomOfStack=true)
        // rather than "WindowFlag::stickToFront". The old filter
        // mis-classified user-summoned modals that happen to be
        // stickToFront (LoadSave is the load-bearing case — it's
        // stickToFront and was being skipped on title-screen open,
        // causing focus to stay on titleMenu instead of jumping to
        // the new dialog). The set-based check distinguishes the
        // always-present in-game chrome (toolbars) from individual
        // dialogs that just happen to draw on top — toolbars are
        // explicit set members, modals aren't in any set.
        const auto findTopmost = [](bool includeChrome) -> WindowClass {
            for (auto it = gWindowList.rbegin(); it != gWindowList.rend(); ++it)
            {
                auto& wPtr = *it;
                if (wPtr == nullptr)
                    continue;
                if (wPtr->flags.has(WindowFlag::dead))
                    continue;
                if (wPtr->classification == WindowClass::mainWindow)
                    continue;
                // OPENRCT2MINI: skip purely visual windows (tooltip,
                // map hover-identification overlay) so the per-frame
                // focus auto-track never tries to snap to one.
                if (WidgetFocus::isPurelyVisualWindow(wPtr->classification))
                    continue;
                if (!includeChrome)
                {
                    const auto* set = WidgetFocus::findSetFor(wPtr->classification);
                    if (set != nullptr && set->isBottomOfStack)
                        continue;
                }
                if (WidgetFocus::firstFocusable(*wPtr) == kWidgetIndexNull)
                    continue;
                return wPtr->classification;
            }
            return WindowClass::null;
        };
        WindowClass topmostFocusableCls = findTopmost(/*includeChrome=*/ false);
        if (topmostFocusableCls == WindowClass::null)
            topmostFocusableCls = findTopmost(/*includeChrome=*/ true);
        // OPENRCT2MINI window-set-plan §3.3: count set-siblings as
        // "same logical surface" — hopping from topToolbar to game-
        // BottomToolbar (both in the in-game chrome set) should NOT
        // re-snap the focus, because the user navigated there
        // explicitly via the directional walker.
        const bool topmostChanged = (topmostFocusableCls != WindowClass::null
                                     && topmostFocusableCls != _lastTopmostFocusable
                                     && !WidgetFocus::sameSetOrClass(topmostFocusableCls, _focusedWindowClass));

        // OPENRCT2MINI cursor-selector-modal-plan v2: auto-wake the
        // selector when a non-chrome window appears AND the user
        // was last using virtual input. Real-mouse-driven flows
        // never trigger the wake (user stays in cursor mode). The
        // bottom-of-stack chrome itself doesn't qualify — the user
        // is presumably backing INTO the toolbar set when state is
        // hidden, and we don't want to fight them.
        if (_selectorMode == SelectorMode::hidden && topmostChanged
            && !_lastInputWasRealMouse)
        {
            const auto* set = WidgetFocus::findSetFor(topmostFocusableCls);
            const bool topmostIsBottomChrome = (set != nullptr && set->isBottomOfStack);
            if (!topmostIsBottomChrome)
            {
                setSelectorMode(SelectorMode::active);
                // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
                // arm the "auto-woke this process()" flag. Same-frame
                // queued SDL events (notably the PAD A press that
                // opened this very window via the held-state poll's
                // synthesised leftPress) are about to flow into
                // processEvents below. Those events belong to the
                // pre-wake cursor-mode interpretation — letting them
                // re-fire through the now-active widgetFocus context
                // double-actions the same physical press. Cleared at
                // the top of the next process() call.
                _autoWokeThisProcess = true;
            }
        }

        _lastTopmostFocusable = topmostFocusableCls;
        // OPENRCT2MINI 2026-06-01 (user report): gate auto-snap on
        // !_lastInputWasRealMouse, mirroring the selector auto-wake
        // gate a few lines above. widgetFocusAlwaysOn defaults to true,
        // so without this gate every real-mouse user gets focus snapped
        // onto whatever window they just opened — which makes
        // resolveActiveContext return widgetFocus, and any widget
        // handler that gates on widgetFocus (e.g. WIDX_CONSTRUCT in
        // RideConstruction / Footpath) takes its gamepad branch on a
        // mouse click. The right semantic for `widgetFocusAlwaysOn`:
        // focus is *available* without first pressing TAB, but doesn't
        // *engage* until the user takes a non-cursor action.
        // enterFocusModeRequested clears _lastInputWasRealMouse, so
        // pressing TAB / focus.confirm still bootstraps focus the same
        // frame.
        if (shouldBeFocused && !_lastInputWasRealMouse
            && (_focusedWindowClass == WindowClass::null || topmostChanged))
        {
            // Strict-z-order rule: focus the TOPMOST window with at
            // least one focusable widget. Nothing more. gWindowList's
            // back element is the topmost in OpenRCT2's draw order
            // (front-of-list = bottom of stack, back-of-list = top),
            // so reverse iteration picks topmost first and the loop
            // exits on the first qualifying window.
            //
            // Skips:
            //   - dead windows (closed but not yet GC'd)
            //   - mainWindow (the world viewport — no user-
            //     interactive widgets; moving focus into it would
            //     just steal cursor control from the viewport).
            //
            // What this DOESN'T do:
            //   - prefer non-stickToBack over stickToBack
            //   - pick the window with the most focusable widgets
            //   - second-guess what the user "probably wants"
            //
            // Earlier revisions tried both of those heuristics to
            // work around "wrong window focused on the title
            // scene" — title logo + title menu are both
            // stickToBack and the topmost was the logo, whose
            // single button only opens About. Those heuristics
            // produced their own surprises in other scenes (toolbar
            // grabbed focus over Options, etc.). User direction
            // (focus-mode-plan §F.8 follow-up): keep the rule
            // strict, surface a way for the user to SWITCH the
            // focused window if topmost is wrong. That switch UX
            // is unbuilt — title menu currently isn't reachable
            // from focus mode when the logo is on top, the user
            // can still click it with the mouse.
            // OPENRCT2MINI window-set-plan §3.3: delegate to the
            // shared snapper. It applies the non-stickToFront
            // preference, the WindowClass::dropdown exemption, and
            // any set-defined defaultClass/defaultWidget — the same
            // rules used by selector-triggered same-frame snaps.
            // The _lastTopmostFocusable assignment a few lines
            // above is still authoritative for the next-frame
            // edge-trigger; snapFocusToTopmostFocusable updates it
            // again to the actual landing class.
            snapFocusToTopmostFocusable();
        }
    }

    if (_focusModeRequested && _focusedWindowClass == WindowClass::null)
        _focusModeRequested = false;

    // OPENRCT2MINI grid-cursor-plan §12.1 (amended): per-frame
    // toolActive edge detection — the source of truth for the
    // _toolFocusSelected latch's lifecycle. Window.cpp lives in the
    // openrct2 library and can't depend on openrct2-ui's
    // InputManager directly (wrong-direction dependency), so we
    // detect the true→false edge here. The earlier code ALSO set
    // the latch true on false→true (tool open) — that auto-snapped
    // the user into grid-cursor mode whenever a tool window came
    // up, which the UX revision (grid-cursor-plan §12.1 amendment
    // 2026-05-17) explicitly inverts: a tool open should leave
    // focus on the tool window's widgets so the user can pick
    // type / mode / settings first, then engage the grid cursor
    // deliberately via the construction-mode widgets (handled in
    // FootpathWindow::onMouseUp). The clear-on-deactivate edge
    // remains so that closing the tool window or cancelling the
    // tool drops the latch and stops resolveActiveContext routing
    // to a dead strategy.
    {
        const bool nowToolActive = gInputFlags.has(InputFlag::toolActive);
        if (!nowToolActive && _previousToolActive)
            setToolFocusSelected(false, SelectorTransitionSource::virtualUserInput);
        _previousToolActive = nowToolActive;
    }

    _activeContext = resolveActiveContext();

    // OPENRCT2MINI input-plan Track 3 / Phase 3.A: activation
    // lifecycle. When the active context flips between frames, the
    // outgoing strategy's onDeactivate runs and the incoming
    // strategy's onActivate fires. Phase 3.A's stubs both do nothing;
    // Phase 3.B uses these hooks to synthesise release events for
    // any presses the outgoing context had open (mid-press context
    // swap correctness — see input-plan-review.md §F5).
    if (_activeContext != _previousActiveContext)
    {
        const auto prevIdx = static_cast<size_t>(_previousActiveContext);
        const auto newIdx = static_cast<size_t>(_activeContext);
        IInputContext* outgoing = (prevIdx < kInputContextCount && _contextRegistry[prevIdx] != nullptr)
            ? _contextRegistry[prevIdx].get()
            : _worldContext.get();
        IInputContext* incoming = (newIdx < kInputContextCount && _contextRegistry[newIdx] != nullptr)
            ? _contextRegistry[newIdx].get()
            : _worldContext.get();
        if (outgoing != nullptr)
            outgoing->onDeactivate();
        if (incoming != nullptr)
            incoming->onActivate();
        _previousActiveContext = _activeContext;
    }

    checkJoysticks();
    // OPENRCT2MINI input-plan Track 1 §3.1: refresh _keyboardState
    // and _mouseState here, BEFORE handleModifiers. Previously the
    // refresh lived in processHoldEvents (called later in this same
    // frame), which produced a 1-frame lag for keyboard-bound
    // modifiers — press Shift in frame N, the shift bit on
    // _modifierKeyState didn't flip until frame N+1, so vertical
    // placement indicators trailed press by a frame. Gamepad-bound
    // modifiers were already current because _heldGamepadButtons is
    // updated synchronously inside queueInputEvent during the SDL
    // pump. Same fix applies to _mouseState for the (rarer) case of
    // a modifier shortcut bound to a side-mouse button.
    refreshDeviceState();
    processAnalogueInput();
    handleModifiers();
    // OPENRCT2MINI input-plan Track 3 / Phase 3.A: per-frame tick
    // for the active strategy. Runs after handleModifiers (so the
    // strategy sees up-to-date modifier state) and before
    // processEvents (so any held-state edges the strategy wants to
    // synthesise enter the event flow alongside SDL events).
    // Phase 3.A's WorldContextStub::processFrame is a no-op.
    getActiveContextStrategy().processFrame(SDL_GetTicks());
    // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #8
    // — parked-cursor blink pump): when the grid cursor is in
    // parked state, the active strategy is widgetFocus (or world)
    // whose default processFrame is a no-op, so ToolContext's
    // per-frame tile invalidation doesn't run. Without that, the
    // tile only repaints when something else invalidates it (a
    // pan, an entity move, etc.), and the surface paint's
    // `(ms / 500) & 1` blink gate gets frozen at whichever phase
    // last redrew — the user sees the marker blink a few times
    // then stop half-drawn.
    //
    // Fire the per-frame invalidate here, independent of which
    // strategy is active. Cheap when the flag is off (one
    // FlagHolder::has check).
    //
    // OPENRCT2MINI grid-cursor-plan §18.A follow-up (2026-05-24,
    // second user report): when the brush is multi-cell (parked
    // selection rect spans more than one tile, i.e.
    // gMapSelectPositionA != gMapSelectPositionB), invalidating just
    // the anchor tile leaves the other N²-1 tiles untouched — their
    // blink frames render on top of stale viewport pixels. Detect
    // multi-cell by comparing A and B and use MapInvalidateRegion
    // for the whole brush footprint. Same shape as the active-state
    // pump fix in ToolContext::processFrame.
    if (gMapSelectFlags.has(MapSelectFlag::gridCursorParked))
    {
        if (gMapSelectPositionA != gMapSelectPositionB)
            MapInvalidateRegion(gMapSelectPositionA, gMapSelectPositionB);
        else
            MapInvalidateTileFull(gMapSelectPositionA);
    }
    processEvents();
    processHoldEvents();
    handleViewScrolling();

    // OPENRCT2MINI gamepad-plan 1.7c: per-frame tick for the chord
    // capture countdown. Runs after processEvents so any DOWN/UP
    // events from this frame have already updated _heldGamepadButtons
    // and _captureLastDownMs. If a 2-button chord has been holding
    // for 5 seconds without further DOWN, the chord auto-commits.
    const auto nowMs = SDL_GetTicks();
    GetShortcutManager().updatePendingCapture(nowMs, &_heldGamepadButtons);

    // OPENRCT2MINI hold-binding refactor: per-frame tick for hold-
    // shortcut threshold. Fires hold actions when their _holdPending
    // entry has been held for >= holdMs and the action hasn't fired
    // yet. Tap firing for the same input is owned by the release-
    // event branch of processEvent; this tick does not fire taps.
    GetShortcutManager().tickHoldShortcuts(nowMs);

    // OPENRCT2MINI gamepad-plan 1.11b: per-frame tick for the rumble
    // engine's active-playhead queue. Walks the queue, evaluates the
    // envelope at the current playhead, mixes one-shot + continuous
    // contributions, and pushes deltas to UiContext::RumbleControllers.
    // Retires expired entries (one-shot finished, continuous
    // lastSeenMs older than this frame).
    Haptic::tickEngine(nowMs);

    // OPENRCT2MINI gamepad-plan 1.13: per-frame tick for the LED
    // flash engine. Walks the single-slot active flash, evaluates
    // the fade ramp at the current playhead, and pushes deltas
    // (with internal hysteresis) to UiContext::SetControllerLED.
    // Submits one trailing (0,0,0) sweep on flash expiry, then
    // idles silently until the next News::AddItemToQueue call.
    Led::tickEngine(nowMs);

    // OPENRCT2MINI active-window-emphasis plan §4.3: release pass for
    // the cycle-window highlight latch. Runs AFTER processEvents()
    // (line 2765) so _modifierKeyState reflects every key event from
    // this frame. The latch clears when:
    //   - any modifier the cycle binding requires is no longer held
    //     (the common path — Shift+Tab / Ctrl+Tab style bindings), OR
    //   - the fallback timeout fires (modifier-less rebinds).
    // On clear, invalidate the highlighted window so its ring is
    // repainted away cleanly next frame. Sentinel 255 matches the
    // class field's default initialiser (see InputManager.h).
    {
        constexpr WindowClass kCycleHighlightNoClass = static_cast<WindowClass>(255);
        if (_cycleHighlightClass != kCycleHighlightNoClass)
        {
            // Keep latch alive while EITHER the modifier is still
            // held OR the minimum dwell timeout has not yet elapsed.
            // The dwell prevents one-frame flicker on fast taps
            // (user releases SHIFT too quickly for the eye to
            // register a modifier-only latch).
            const bool stillHeld = (_cycleHighlightModifierMask != 0)
                && ((_modifierKeyState & _cycleHighlightModifierMask) != 0);
            const bool inDwell = (nowMs < _cycleHighlightUntilMs);
            if (!stillHeld && !inDwell)
            {
                if (auto* windowMgr = Ui::GetWindowManager())
                {
                    if (auto* w = windowMgr->FindByClass(_cycleHighlightClass))
                        w->invalidate();
                }
                _cycleHighlightClass = kCycleHighlightNoClass;
                _cycleHighlightWidget = kWidgetIndexNull;
                _cycleHighlightModifierMask = 0;
                _cycleHighlightUntilMs = 0;
            }
        }
    }
}

InputContext InputManager::resolveActiveContext() const
{
    // Priority order, first match wins. Add new contexts above world
    // in the order users expect them to take precedence over the
    // free-cursor world view.
    //
    // OPENRCT2MINI gamepad-plan 1.10: OSK still wins because it can
    // sit on top of any of the typing modals (TextInput → spawn OSK).
    // After OSK, the order goes most-specific to most-general so the
    // overwrite-confirm dialog beats the parent loadsave; loadsave /
    // textinput / console / widgetTextBox are otherwise mutually
    // exclusive in normal use, but the order is fixed for stability.
    // chat is omitted — multiplayer not shipped.
    if (Windows::OskIsActive())
        return InputContext::osk;

    auto* windowMgr = GetWindowManager();

    // OPENRCT2MINI focus-mode-plan / Phase F.3: text-entry modals
    // win over widgetFocus. These contexts route raw key events
    // straight into the modal's keyboard handler (typing letters,
    // commit, cancel) and must not be intercepted by the focus
    // strategy. Phase F.6 will migrate the OSK to widgetFocus and
    // (eventually) the rest of these modals to share the focus
    // ring; until then they keep their dedicated strategies.
    //
    // OPENRCT2MINI 2026-05-29 — loadSave + loadSaveOverwritePrompt
    // dropped from this priority block. Both windows have a full
    // set of focusable widgets (Save button, sort headers, file
    // list scroll, filename textbox, folder action buttons), so
    // their dedicated context contributed nothing useful: the
    // strategy's onKeyEvent was the only override, and after the
    // gamepad-plan 1.6c.7 cleanup WindowLoadSaveInputKey is a
    // no-op. Worse, the loadSave allow-list omits kFocus*, so D-
    // pad navigation got rejected and focus mode was effectively
    // dead in the dialog. Letting these fall through to widget-
    // Focus enables full focus-mode navigation; dismiss / confirm
    // still fire through the modal-hooks stack (LoadSave.cpp:605
    // — close-window / press WIDX_SAVE on PAD BACK / PAD START).
    // LoadSaveContextImpl + LoadSaveOverwritePromptContextImpl
    // remain registered as defensive dead code (never reachable
    // through this resolver any more, but cheap to keep so
    // nothing referencing the slot crashes).
    //
    // textInput is preserved for now: it's a single-textbox modal
    // where arrow keys should drive caret nav, not widget focus
    // walk. widgetTextBox stays for the same reason.
    if (windowMgr != nullptr)
    {
        if (windowMgr->FindByClass(WindowClass::textinput) != nullptr)
            return InputContext::textInput;
    }

    auto& console = GetInGameConsole();
    if (console.IsOpen())
        return InputContext::console;

    if (Windows::IsUsingWidgetTextBox())
        return InputContext::widgetTextBox;

    // OPENRCT2MINI grid-cursor-plan §12.1 (amended): tool contexts win
    // over widgetFocus when the user has explicitly selected the tool
    // viewport via the cycle-window UX. The previous order checked
    // widgetFocus first (the focused-window arm below), which meant
    // opening a tool window's widgets would always grab cursor.*
    // drive and the FootpathContextImpl never reached — even though
    // the user had just opened the tool with the intent of placing
    // pieces. Now the tool-context arm fires first when the
    // _toolFocusSelected latch is set; if false, fall through to
    // widgetFocus as before.
    //
    // The latch is managed by:
    //   - process() per-frame edge detection on gInputFlags.toolActive
    //     (false→true snaps it true, true→false clears it)
    //   - cycleFocusedWindow when the user cycles onto / off of the
    //     virtual "tool viewport" entry
    //
    // Order roughly by frequency-of-use; Footpath / Terrain / Water /
    // Scenery are the most common construction operations. Ride
    // construction comes next; LandRights and TileInspector are rarer
    // (and TileInspector is debug-only).
    if (_toolFocusSelected && windowMgr != nullptr && gInputFlags.has(InputFlag::toolActive))
    {
        // OPENRCT2MINI multi-tool grid-cursor priority (2026-05-24):
        // when multiple armed tool windows are open at once (e.g.
        // Footpath + Land), pick the one whose window is currently
        // topmost via the same z-order predicate that drives
        // drop-shadow / titlebar dim. The user switches between
        // tools by bringing the desired window to the top —
        // matches the existing "active window" UX cue. The legacy
        // first-match-by-enum-order behaviour kicks in only when
        // none of the tool windows is on top (e.g. a non-tool
        // window sits above both), preserving existing behaviour
        // for the common single-tool case.
        //
        // Note: only fires for the gamepad/grid-cursor path —
        // virtual mouse and real mouse input route through
        // ViewportInteraction / ProcessWorldCursor which target
        // whatever the screen-space cursor is over, so this
        // routing change has no effect on those paths.
        static constexpr WindowClass kToolClasses[] = {
            WindowClass::footpath,
            WindowClass::land,
            WindowClass::water,
            WindowClass::scenery,
            WindowClass::rideConstruction,
            WindowClass::landRights,
            WindowClass::tileInspector,
            // OPENRCT2MINI grid-cursor-plan §11.9 / §18.C (2026-05-24):
            // ClearScenery (bulldozer) is its own tool window class.
            WindowClass::clearScenery,
            // OPENRCT2MINI grid-cursor-plan §11.10 (2026-05-29):
            // PatrolArea (staff patrol-zone painting) is its own
            // tool window class, spawned by the Staff window's
            // "Set Patrol Area" button.
            WindowClass::patrolArea,
            // OPENRCT2MINI grid-cursor-plan §11.11 (2026-05-29):
            // Peep pickup. WindowClass::peep is shared by the
            // per-peep info panel (Guest/Staff windows) AND the
            // pickup tool. The toolActive gate on the tool-context
            // arm in resolveActiveContext below disambiguates —
            // info-panel-without-pickup leaves toolActive=false
            // and the arm falls through to widgetFocus.
            WindowClass::peep,
        };
        const auto topmost = OpenRCT2::GetTopmostWindowClassInSet(
            kToolClasses, std::size(kToolClasses));
        switch (topmost)
        {
            case WindowClass::footpath:
                return InputContext::toolFootpath;
            case WindowClass::land:
                return InputContext::toolTerrain;
            case WindowClass::water:
                return InputContext::toolWater;
            case WindowClass::scenery:
                return InputContext::toolScenery;
            case WindowClass::rideConstruction:
                return InputContext::toolRideConstruction;
            case WindowClass::landRights:
                return InputContext::toolLandRights;
            case WindowClass::tileInspector:
                return InputContext::toolTileInspector;
            case WindowClass::clearScenery:
                return InputContext::toolClearScenery;
            case WindowClass::patrolArea:
                return InputContext::toolPatrol;
            case WindowClass::peep:
                return InputContext::toolPeepPickup;
            default:
                break;
        }
    }

    // OPENRCT2MINI focus-mode-plan / Phase F.3: widget-focus is
    // active whenever a focused window is live AND no text-entry
    // modal is on top. The bootstrap + staleness pass in process()
    // — run earlier this same frame — is what populates / clears
    // `_focusedWindowClass`, so by the time we get here the field
    // already mirrors the user's intent for this frame. We
    // re-call `getFocusedWindow()` here rather than just checking
    // the sentinel because a window could in theory die between
    // the bootstrap pass and now (the pass holds no lock); the
    // extra FindByClass is cheap and keeps the precondition
    // strict.
    if (getFocusedWindow() != nullptr)
        return InputContext::widgetFocus;

    return InputContext::world;
}

bool InputManager::isShortcutAllowedInActiveContext(const InputEvent& e, std::string_view shortcutId) const
{
    (void)e; // reserved — current allow-lists are context-only
    return isShortcutMeaningfulInContext(shortcutId, _activeContext);
}

// OPENRCT2MINI input-plan Track 3 / Phase 3.I.a: programmatic version
// of the per-context allow-list. The active-context variant
// (`isShortcutAllowedInActiveContext`) just delegates here passing
// `_activeContext`; future UI code can iterate the InputContext enum
// to compute "this shortcut is meaningful in N contexts: world,
// toolFootpath, toolTerrain" without duplicating the switch.
bool InputManager::isShortcutMeaningfulInContext(std::string_view shortcutId, InputContext context) const
{
    // OPENRCT2MINI gamepad-plan 1.10: per-context shortcut allow-list.
    // World allows everything (default). Every modal context allows
    // the dismiss / confirm pair (so PAD BACK / PAD START always
    // close or commit) plus any context-specific extras the plan
    // calls out. Anything not on the list is blocked — the gate is
    // intentionally narrow so future shortcut additions can't
    // accidentally fire through a typing modal.
    //
    // Allow-list table per gamepad-plan 1.10.2:
    //   world                    → all
    //   osk                      → dismiss, confirm, cursor.* (+ click/cancel)
    //   console                  → dismiss, confirm, kDebugToggleConsole
    //   loadSave                 → dismiss, confirm, cursor.up/down/left/right
    //   loadSaveOverwritePrompt  → dismiss, confirm
    //   textInput                → dismiss, confirm
    //   widgetTextBox            → dismiss, confirm
    switch (context)
    {
        case InputContext::world:
            return true;

        case InputContext::osk:
        {
            // OPENRCT2MINI osk-overhaul §1: navigation moved to the
            // kFocus* family (matches widgetFocus). Activation +
            // backspace stay on kCursorClick / kCursorCancel; the
            // dismiss / confirm modal hooks pick up the standard
            // PAD START / PAD BACK / RETURN / ESCAPE shortcuts.
            // kCursorUp/Down/Left/Right are dropped from the OSK
            // allow-list — they're world-cursor pan shortcuts post-
            // §F.9 split and would leak through if left in.
            //
            // bug-fix §D: kInterfaceEnterFocusMode is permitted so
            // the user can press the standard "enter focus mode"
            // shortcut (TAB by default) while the OSK is up to wake
            // the selector back up after they've been using the
            // mouse. Same UX as every other window.
            constexpr std::array kAllowed = {
                ShortcutId::kInterfaceDismiss,
                ShortcutId::kInterfaceConfirm,
                ShortcutId::kInterfaceEnterFocusMode,
                ShortcutId::kFocusUp,
                ShortcutId::kFocusDown,
                ShortcutId::kFocusLeft,
                ShortcutId::kFocusRight,
                ShortcutId::kCursorClick,
                ShortcutId::kCursorCancel,
                ShortcutId::kCursorFastModifier,
                // OPENRCT2MINI text-editing-de-hardcode: caret +
                // clipboard shortcuts on the OSK allow-list so they
                // fire while the OSK is up (parent textbox + OSK
                // share the same TextInputSession via _session.Buffer
                // — the OSK's _typing-into-active-session writes go
                // through TextComposition just like a hardware key).
                ShortcutId::kInterfaceCaretLeft,
                ShortcutId::kInterfaceCaretRight,
                ShortcutId::kInterfaceCaretWordLeft,
                ShortcutId::kInterfaceCaretWordRight,
                ShortcutId::kInterfaceTextBackspace,
                ShortcutId::kInterfaceTextBackspaceWord,
                ShortcutId::kInterfaceTextDelete,
                ShortcutId::kInterfaceTextDeleteWord,
                ShortcutId::kInterfaceClipboardCopy,
                ShortcutId::kInterfaceClipboardCut,
                ShortcutId::kInterfaceClipboardPaste,
            };
            for (auto id : kAllowed)
                if (id == shortcutId)
                    return true;
            return false;
        }

        case InputContext::console:
        {
            constexpr std::array kAllowed = {
                ShortcutId::kInterfaceDismiss,
                ShortcutId::kInterfaceConfirm,
                ShortcutId::kDebugToggleConsole,
            };
            for (auto id : kAllowed)
                if (id == shortcutId)
                    return true;
            return false;
        }

        case InputContext::loadSave:
        {
            constexpr std::array kAllowed = {
                ShortcutId::kInterfaceDismiss,
                ShortcutId::kInterfaceConfirm,
                ShortcutId::kCursorUp,
                ShortcutId::kCursorDown,
                ShortcutId::kCursorLeft,
                ShortcutId::kCursorRight,
                // OPENRCT2MINI input-plan fix: mouse clicks must reach
                // widget dispatch — the file list, Save button, title-
                // bar drag, and Close box all need cursor.click /
                // cursor.cancel to fire. Without these, LMB/RMB events
                // bound to cursor.click/cursor.cancel get rejected by
                // this allow-list and the LoadSave window appears
                // input-dead while hover (SDL_MOUSEMOTION, which
                // bypasses ShortcutManager) still works. Same fix
                // already applied to the tool contexts below.
                ShortcutId::kCursorClick,
                ShortcutId::kCursorCancel,
            };
            for (auto id : kAllowed)
                if (id == shortcutId)
                    return true;
            return false;
        }

        case InputContext::loadSaveOverwritePrompt:
        case InputContext::textInput:
        case InputContext::widgetTextBox:
        {
            constexpr std::array kAllowed = {
                ShortcutId::kInterfaceDismiss,
                ShortcutId::kInterfaceConfirm,
                // OPENRCT2MINI focus-mode-plan / Phase F.1: allow the
                // focus-mode activator through every textbox context.
                // The shortcut's lambda flips _focusModeRequested, and
                // resolveActiveContext promotes the next frame to
                // widgetFocus. Without this entry the activation
                // gesture would be suppressed by the textbox
                // allow-list itself.
                ShortcutId::kInterfaceEnterFocusMode,
                // OPENRCT2MINI input-plan fix: same rationale as
                // loadSave above — these modals all have clickable
                // widgets (OverwritePrompt's Yes/No, TextInput's OK/
                // Cancel, widgetTextBox's parent-window buttons /
                // close-box / title-bar drag). Suppressing
                // cursor.click/cancel made every widget in them
                // dead to mouse.
                ShortcutId::kCursorClick,
                ShortcutId::kCursorCancel,
                // OPENRCT2MINI text-editing-de-hardcode: caret +
                // clipboard shortcuts allow-listed so direct typing
                // into a widget textbox / TextInput modal / overwrite
                // prompt routes BACKSPACE / arrows / CTRL+C/V/X /
                // etc. through the bindable dispatcher. These used
                // to be hardcoded in TextComposition's SDL_KEYDOWN
                // switch; with that gone, the allow-list is what
                // lets the new shortcuts reach the action lambdas.
                ShortcutId::kInterfaceCaretLeft,
                ShortcutId::kInterfaceCaretRight,
                ShortcutId::kInterfaceCaretWordLeft,
                ShortcutId::kInterfaceCaretWordRight,
                ShortcutId::kInterfaceTextBackspace,
                ShortcutId::kInterfaceTextBackspaceWord,
                ShortcutId::kInterfaceTextDelete,
                ShortcutId::kInterfaceTextDeleteWord,
                ShortcutId::kInterfaceClipboardCopy,
                ShortcutId::kInterfaceClipboardCut,
                ShortcutId::kInterfaceClipboardPaste,
            };
            for (auto id : kAllowed)
                if (id == shortcutId)
                    return true;
            return false;
        }

        // OPENRCT2MINI input-plan Track 3 / Phase 3.E: tool contexts.
        // Each tool context lets the user navigate (cursor.up/down/
        // left/right step the grid cursor), confirm (cursor.click →
        // OPENRCT2MINI grid-cursor-plan §9.2 / §14.1: tool contexts are
        // OVERLAID on top of the world view, same as widgetFocus is
        // overlaid on a normal window. The user expects camera pan /
        // zoom / rotate / window-cycle / save / options / etc. to keep
        // firing while a tool is armed — only the verb dispatch needs
        // to be intercepted. The tool strategy's onShortcut returns
        // Consumed for the shortcuts it owns (cursor.click / cancel /
        // focus.* / precisionModifier / ZRaise / ZLower / rotate /
        // dismiss / confirm) and Passthrough for everything else, so
        // the global action lambdas fire normally for non-verb
        // shortcuts. A previous narrow allow-list suppressed all non-
        // tool inputs (camera pan, save, etc.) and broke the tool UX
        // — same shape as the title-scene bug Phase F.7 fixed for
        // widgetFocus. Mirror that fix: allow everything, let the
        // strategy decide.
        case InputContext::toolFootpath:
        case InputContext::toolTerrain:
        case InputContext::toolWater:
        case InputContext::toolScenery:
        case InputContext::toolLandRights:
        case InputContext::toolTileInspector:
        case InputContext::toolRideConstruction:
        case InputContext::toolClearScenery:
        case InputContext::toolPatrol:
        case InputContext::toolPeepPickup:
            return true;

        // OPENRCT2MINI focus-mode-plan / Phase F.1: widget-focus mode.
        // Unlike the modal text-entry contexts (loadSave, textInput,
        // …) which gate shortcut dispatch to a tight allow-list,
        // widgetFocus is OVERLAID on top of an otherwise-normal
        // game window — the title scene, the Park window, an
        // Options pane. The user expects normal gameplay /
        // navigation shortcuts to keep firing AROUND focus mode.
        // A bug report on the title scene caught the v1 allow-list
        // suppressing every non-cursor.* binding the user pressed,
        // making the title menu unusable (focus-mode-plan.md §F.7
        // follow-up).
        //
        // Strategy's onShortcut already returns Consumed for the
        // cursor.* / dismiss / cancel paths it owns, so those
        // shortcuts STILL won't double-fire their action lambdas.
        // Anything else this filter lets through reaches the
        // strategy's default Passthrough → action lambda fires
        // normally. Net effect: focus mode adds D-pad navigation
        // without subtracting other shortcuts.
        case InputContext::widgetFocus:
            return true;
    }
    // Unknown context → safe default of allow-everything; the table
    // above must be updated when InputContext is extended.
    return true;
}

// OPENRCT2MINI input-plan Track 3 / Phase 3.A: strategy registry
// accessor. Returns the strategy registered for the currently-active
// InputContext enum value. Falls back to the world stub when no
// strategy is registered for that slot — the world stub serves both
// as the default fallback and as the world-mode handler.
IInputContext& InputManager::getActiveContextStrategy() const
{
    const auto idx = static_cast<size_t>(_activeContext);
    if (idx < kInputContextCount)
    {
        if (auto& slot = _contextRegistry[idx]; slot != nullptr)
            return *slot;
    }
    // World stub is created at construction; this is always non-null.
    return *_worldContext;
}

// OPENRCT2MINI grid-cursor-plan §14.2 polish 3 (2026-05-20): walk
// every registered context strategy looking for a non-zero Grid /
// EdgeCursorModel Z. Used by SyncHiddenCursorParking to preserve
// the user's raised-Z screen position across the grid-cursor → parked
// transition: at that moment the active strategy is widgetFocus
// (whose cursor model is the focus-ring helper, not GridCursorModel),
// but the inactive ToolContext is still in the registry and its
// GridCursorModel preserves the user's accumulated Z across activate
// / deactivate cycles. Returns the first non-zero Z encountered; 0
// when nothing has a raised Z (every tool's model is at ground).
int32_t InputManager::getAnyRegisteredCursorZ() const
{
    for (const auto& slot : _contextRegistry)
    {
        if (slot == nullptr)
            continue;
        if (auto* model = slot->getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                if (grid->getZ() != 0)
                    return grid->getZ();
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                if (edge->getZ() != 0)
                    return edge->getZ();
            }
        }
    }
    return 0;
}

// OPENRCT2MINI grid-cursor-plan §11.11 polish (2026-05-30 follow-up #4):
// parked-state companion to cursorParkZExtra. SyncHiddenCursorParking's
// parked-path branches don't have an active ToolContext to query via
// getActiveContextStrategy() (the active strategy in parked state is
// widgetFocus), but the dormant ToolContext is still in the registry
// and its cursorParkZExtra contract is a pure function of worldCoord.
// Walk the registry, return the first non-zero result. Used to keep the
// peep pickup pincers sprite at its +32 lift across the active→parked
// transition.
int32_t InputManager::getAnyRegisteredCursorParkZExtra(CoordsXY worldCoord) const
{
    for (const auto& slot : _contextRegistry)
    {
        if (slot == nullptr)
            continue;
        if (auto* tool = dynamic_cast<ToolContext*>(slot.get()); tool != nullptr)
        {
            const int32_t z = tool->cursorParkZExtra(worldCoord);
            if (z != 0)
                return z;
        }
    }
    return 0;
}

// OPENRCT2MINI input-plan Track 3 / Phase 3.A: routing entry point.
// Returns true if the active strategy consumed the shortcut and the
// caller (ShortcutManager::processEvent) should suppress the action
// lambda fire. Phase 3.A's WorldContextStub returns Passthrough for
// everything, so this always returns false — behaviour is preserved.
//
// Phase 3.B fills in real modal strategies that may return Consumed
// for specific (context, id) pairs. The lambda layer continues to
// own the action backbone; routing only interposes.
bool InputManager::shouldSuppressAction(std::string_view shortcutId, const InputEvent& e) const
{
    // OPENRCT2MINI focus-mode-widgets-plan §13 (2026-05-26): when ANY
    // dropdown is open, kCursorCancel (PAD B) always closes the
    // dropdown first — regardless of which input context is active.
    // Without this universal pre-check, tool contexts route PAD B
    // through ToolContext::onShortcut → onCancel (delete tile element)
    // and the dropdown stays up; widgetFocus routes through its
    // close-box fallback when focus didn't snap to the dropdown,
    // dismissing the parent tool window instead. The dropdown is
    // always the topmost interactive thing — PAD B over a dropdown
    // should always dismiss the dropdown before any other
    // interpretation. Mirrors the dropdown branch inside
    // WidgetFocusContextImpl's onShortcut (which only fires when
    // active context is widgetFocus AND focus is actually on the
    // dropdown — neither always holds when a dropdown is open).
    //
    // Non-mouse devices fire immediately (no tap-vs-drag ambiguity).
    // Mouse cursor.cancel deliberately falls through to the strategy
    // path so the existing short-press / right-click disambiguation
    // gate in WorldContext / ToolContext still runs — the dropdown
    // is in screen space the mouse cursor isn't necessarily over.
    if (shortcutId == ShortcutId::kCursorCancel
        && e.deviceKind != InputDeviceKind::mouse)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr
            && windowMgr->FindByClass(WindowClass::dropdown) != nullptr)
        {
            OpenRCT2::Ui::Windows::WindowDropdownClose();
            OpenRCT2::_inputState = OpenRCT2::InputState::Normal;
            if (OpenRCT2::gInputFlags.has(OpenRCT2::InputFlag::widgetPressed))
                OpenRCT2::gInputFlags.unset(OpenRCT2::InputFlag::widgetPressed);
            // Pop focus back to the widget that opened the dropdown,
            // matching the focus-mode dropdown branch's behaviour.
            // const_cast: shouldSuppressAction is const but we own
            // the InputManager singleton — the side effects here are
            // intentional cleanup, not logical state mutation.
            auto& mgr = const_cast<InputManager&>(*this);
            if (!mgr.restoreFocus())
                mgr.snapFocusToTopmostFocusable();
            return true;
        }
    }
    auto& strategy = getActiveContextStrategy();
    return strategy.onShortcut(shortcutId, e) == Disposition::Consumed;
}

void InputManager::handleViewScrolling()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto& console = GetInGameConsole();
    if (console.IsOpen())
        return;

    auto mainWindow = WindowGetMain();

    // Handle gamepad analogue scrolling with cursor-based viewport targeting
    if (_analogueScroll.x != 0 || _analogueScroll.y != 0)
    {
        // Get cursor position to find target viewport
        const CursorState* cursorState = ContextGetCursorState();
        Viewport* targetViewport = ViewportFindFromPoint(cursorState->position);

        WindowBase* targetWindow = nullptr;
        if (targetViewport != nullptr)
        {
            // Find the window that owns this viewport
            auto* windowMgr = GetWindowManager();
            targetWindow = windowMgr->GetOwner(targetViewport);
        }

        // Fallback to main window if no viewport found under cursor
        if (targetWindow == nullptr)
        {
            targetWindow = mainWindow;
        }

        if (targetWindow != nullptr)
        {
            // Only unfollow sprites for the main window or viewport windows
            // Don't unfollow for ride windows that might be following vehicles
            if (targetWindow == mainWindow || targetWindow->classification == WindowClass::viewport)
            {
                WindowUnfollowSprite(*targetWindow);
            }
            InputScrollViewportSmooth(_analogueScroll, targetWindow);
        }
    }

    // Handle keyboard shortcut scrolling with edge-based scrolling (but ignore gamepad input)
    ScreenCoordsXY keyboardScroll = { _viewScroll.x - _analogueScroll.x, _viewScroll.y - _analogueScroll.y };
    if (keyboardScroll.x != 0 || keyboardScroll.y != 0)
    {
        if (mainWindow != nullptr)
        {
            WindowUnfollowSprite(*mainWindow);
        }
        InputScrollViewport(keyboardScroll);
    }

    // Mouse edge scrolling
    if (Config::Get().general.edgeScrolling)
    {
        if (InputGetState() != InputState::Normal)
            return;

        if (isModifierKeyPressed(ModifierKey::shift) || isModifierKeyPressed(ModifierKey::ctrl))
            return;

        GameHandleEdgeScroll();
    }
}

void InputManager::handleModifiers()
{
    _modifierKeyState = EnumValue(ModifierKey::none);

    // OPENRCT2MINI shift/ctrl-modifier refactor: ALT and GUI/Cmd still
    // come from SDL's real mod state (they don't have bindable shortcut
    // analogues yet). SHIFT and CTRL no longer do — those are now driven
    // exclusively by the kInterfaceShiftModifier and
    // kInterfaceConstructionZLock shortcuts below, so the user can
    // remap them to any input through the rebind UI. Default bindings
    // include the real Shift / Ctrl keys (LSHIFT/RSHIFT and LCTRL/RCTRL),
    // so out-of-the-box behaviour matches the legacy hardcoded path.
    //
    // Why split the source: SDL's mod state also feeds the chord-shortcut
    // matcher (CTRL+L load, SHIFT+RETURN rotate, etc.), and we still want
    // chords with real Shift/Ctrl to work without forcing the user to
    // also bind those keys to the modifier shortcuts. Removing the SDL
    // hardcoding from THIS path doesn't affect chord matching — chords
    // still consult SDL_GetModState directly.
    auto modifiers = SDL_GetModState();
    if (modifiers & KMOD_ALT)
    {
        _modifierKeyState |= EnumValue(ModifierKey::alt);
    }
#ifdef __MACOSX__
    if (modifiers & KMOD_GUI)
    {
        _modifierKeyState |= EnumValue(ModifierKey::cmd);
    }
#endif

    // OPENRCT2MINI gamepad-plan 1.5d-ext / 1.5g + shift/ctrl-modifier
    // refactor: SHIFT and CTRL behaviours come ONLY from these two
    // bindable shortcuts now. Every construction caller (Scenery,
    // Footpath, TrackDesignPlace, RideConstruction, Land, TileInspector)
    // consults isModifierKeyPressed(ModifierKey::ctrl); shift-modifier
    // behaviours (vertical track stack, scenery vertical-step) consult
    // ModifierKey::shift. With the SDL_GetModState hardcoded path
    // removed above, these shortcut queries are the sole source of
    // truth for the ctrl/shift bits in _modifierKeyState.
    auto& shortcutMgr = GetShortcutManager();
    if (auto* zLock = shortcutMgr.getShortcut(ShortcutId::kInterfaceConstructionZLock))
    {
        if (getState(*zLock))
            _modifierKeyState |= EnumValue(ModifierKey::ctrl);
    }
    if (auto* shiftMod = shortcutMgr.getShortcut(ShortcutId::kInterfaceShiftModifier))
    {
        if (getState(*shiftMod))
            _modifierKeyState |= EnumValue(ModifierKey::shift);
    }

    if (Config::Get().general.virtualFloorStyle != VirtualFloorStyles::Off)
    {
        // OPENRCT2MINI grid-cursor-plan §17 (2026-05-23): floor
        // visibility gated to "while zLock (PAD Y / Ctrl) is held"
        // for the gamepad path. The grid cursor's accumulated Z is
        // PERSISTENT across releases (locked-in until the next
        // tap-resets-to-0 gesture), but the visual floor should
        // only appear while the user is actively adjusting it. This
        // matches the mouse path's behaviour (floor visible only
        // during Shift+drag-Z) and avoids a permanent blue plane
        // floating on screen between placement gestures.
        //
        // OPENRCT2MINI grid-cursor-plan §14.2 polish 7 (2026-05-20):
        // gate on the cursor model's accumulated Z directly, not on
        // VirtualFloorGetHeight(). The painted floor height was set
        // to the raised value during the drag and isn't reset back
        // to 0 when the user returns to ground level — so the
        // height-based check kept the floor enabled forever once
        // raised, even after Z returned to 0. The cursor model's
        // _z resets to 0 cleanly when the user steps back to
        // ground, so it's the correct gate for "is the user
        // actively driving an off-ground Z plane right now."
        //
        // OPENRCT2MINI grid-cursor-plan §17 (2026-05-23): floor
        // only visible while zLock (ctrl) or shift is held — i.e.
        // while the user is actively adjusting Z. The earlier
        // gridCursorRaised branch kept the floor on across the
        // locked-Z lifetime, which the new hold-Z gesture treats
        // as visual clutter — the locked Z still drives
        // placement, but the floor only paints during the active
        // adjustment hold.
        if (isModifierKeyPressed(ModifierKey::ctrl) || isModifierKeyPressed(ModifierKey::shift))
            VirtualFloorEnable();
        else
            VirtualFloorDisable();
    }
}

bool InputManager::isModifierKeyPressed(ModifierKey modifier) const
{
    return _modifierKeyState & EnumValue(modifier);
}

void InputManager::processEvents()
{
    while (!_events.empty())
    {
        const auto& e = _events.front();
        process(e);
        _events.pop();
    }
}

InputManager::ModalHooksToken InputManager::pushModalHooks(ModalHooks hooks)
{
    const auto token = _nextModalHooksToken++;
    _modalHooksStack.push_back({ std::move(hooks), token });
    return token;
}

void InputManager::popModalHooks(ModalHooksToken token)
{
    // Defensive: pop only the slot whose token matches. Out-of-order
    // pop (e.g. parent closes before child for some reason) erases
    // the right slot rather than the top-of-stack. No-op if token
    // not found (already popped, never pushed, etc.).
    for (auto it = _modalHooksStack.begin(); it != _modalHooksStack.end(); ++it)
    {
        if (it->token == token)
        {
            _modalHooksStack.erase(it);
            return;
        }
    }
}

void InputManager::process(const InputEvent& e)
{
    auto& shortcutManager = GetShortcutManager();

    // OPENRCT2MINI gamepad-plan 1.6c.2: dismiss / confirm hook dispatch.
    // Runs ahead of every other gate so a modal-active context (OSK,
    // LoadSave, console, chat, etc.) can push a callback pair via
    // pushModalHooks() and route both keyboard ESC/RETURN AND gamepad
    // PAD BACK / PAD START through it uniformly. Top of stack wins —
    // when a child modal (OSK over TextInput) pushes its own pair, it
    // takes priority; pop on close exposes the parent's pair again.
    //
    // shortcut->matches() short-circuits release events
    // (RegisteredShortcut::isSuitableInputEvent), so the callback fires
    // on the press transition only — same convention as every other
    // ShortcutManager-bound action. The matching key-release event
    // continues through the pipeline; matches() filters it from
    // ShortcutManager too, so no spurious downstream side effect.
    if (!_modalHooksStack.empty())
    {
        // Copy the top handlers before invoking. The callback usually
        // closes its own window, which calls popModalHooks() — that
        // erases the slot we'd otherwise be holding a reference to.
        // Copy (not reference) means we own a stable std::function
        // object for the duration of the call.
        auto dismiss = _modalHooksStack.back().hooks.dismiss;
        auto confirm = _modalHooksStack.back().hooks.confirm;
        if (dismiss)
        {
            const auto* sc = shortcutManager.getShortcut(ShortcutId::kInterfaceDismiss);
            if (sc != nullptr && sc->matches(e, &_heldGamepadButtons))
            {
                if (dismiss(e))
                    return;
            }
        }
        if (confirm)
        {
            const auto* sc = shortcutManager.getShortcut(ShortcutId::kInterfaceConfirm);
            if (sc != nullptr && sc->matches(e, &_heldGamepadButtons))
            {
                if (confirm(e))
                    return;
            }
        }
    }

    // OPENRCT2MINI gamepad-plan 1.6c.5: chat dismiss / confirm dispatch.
    // Chat lives in libopenrct2 and can't directly use the ModalHooks
    // push/pop pattern (cross-library boundary). Handle inline here so
    // both keyboard ESC/RETURN and gamepad PAD BACK / PAD START fire
    // ChatInput::Close / Send while chat is open. Runs ahead of the
    // keyboard-only branch below so gamepad events reach it too.
    if (gChatOpen)
    {
        if (auto* sc = shortcutManager.getShortcut(ShortcutId::kInterfaceDismiss);
            sc != nullptr && sc->matches(e, &_heldGamepadButtons))
        {
            ChatInput(ChatInput::Close);
            return;
        }
        if (auto* sc = shortcutManager.getShortcut(ShortcutId::kInterfaceConfirm);
            sc != nullptr && sc->matches(e, &_heldGamepadButtons))
        {
            ChatInput(ChatInput::Send);
            return;
        }
        // Other keys flow through SDL_TEXTINPUT (typing) — chat doesn't
        // have additional non-text bindings today. processChat() is
        // retained as a no-op placeholder.
        if (e.deviceKind == InputDeviceKind::keyboard)
            processChat(e);
        return;
    }

    // OPENRCT2MINI input-plan Track 3 / Phase 3.B: keyboard-side
    // modal routings delegated to the active context strategy. The
    // previous switch-on-window-class block (TextInput / LoadSave-
    // OverwritePrompt / LoadSave / in-game console) lived inline
    // here; each branch now lives in its respective strategy class
    // at the top of this file and is invoked uniformly.
    //
    // Gamepad / mouse events flow through to shortcut dispatch
    // below — the strategy's onKeyEvent is gated to keyboard events
    // by each impl. Side-effect only — the event continues through
    // the dispatch regardless; the per-context allow-list filter
    // (isShortcutAllowedInActiveContext) suppresses every shortcut
    // except dismiss / confirm in modal contexts, so there's no
    // double-fire risk.
    //
    // The widgetTextBox case has no keyboard routing here — text
    // input flows through SDL_TEXTINPUT and the editing keys go
    // through TextComposition, neither of which uses this Input-
    // Event path. WidgetTextBoxContextImpl reflects this with a
    // no-op onKeyEvent (inherits the base).
    if (e.deviceKind == InputDeviceKind::keyboard)
    {
        getActiveContextStrategy().onKeyEvent(e);
    }

    // OPENRCT2MINI gamepad-plan 1.2: pass the held-set to ShortcutManager
    // so chord bindings can match. For keyboard / mouse / hat events the
    // held-set is irrelevant (matches() short-circuits), but it's cheap
    // to pass and keeps the call site uniform.
    //
    // OPENRCT2MINI gamepad-plan 1.10: pass the per-context filter so
    // the same allow-list applies regardless of device kind. The
    // filter defers to InputManager::isShortcutAllowedInActiveContext
    // which reads the cached _activeContext (resolved once per frame
    // at the top of process()).
    shortcutManager.processEvent(
        e, &_heldGamepadButtons,
        [this](const InputEvent& ev, std::string_view id) {
            return isShortcutAllowedInActiveContext(ev, id);
        },
        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: routing filter.
        // The active context strategy gets the first look at every
        // matched shortcut. WorldContextStub returns Passthrough so
        // this is currently a no-op (every shortcut fires its lambda
        // as today); Phase 3.B's real modal strategies will return
        // Consumed for shortcuts they own.
        [this](const InputEvent& ev, std::string_view id) {
            return shouldSuppressAction(id, ev);
        });
}

// OPENRCT2MINI input-plan Track 3 / Phase 3.B: InputManager::process-
// InGameConsole was moved into ConsoleContextImpl::onKeyEvent at the
// top of this file. The strategy class owns the per-key console
// scroll routing — UP/DOWN history, PAGEUP/PAGEDOWN scroll. Dismiss
// and Confirm (ESC / RETURN / PAD BACK / PAD START) continue to
// route through ModalHooks, the same as before.

void InputManager::processChat(const InputEvent& e)
{
    // OPENRCT2MINI gamepad-plan 1.6c.5: SDLK_ESCAPE / SDLK_RETURN /
    // SDLK_KP_ENTER cases removed. Chat lives in libopenrct2 (cross-
    // library boundary prevents the simple ModalHooks pattern), so
    // confirm / dismiss are handled inline at the chat gate in
    // process(InputEvent) — see the chat branch there. This function
    // remains as a placeholder for future chat-specific routing
    // (typing letters etc., currently handled by SDL_TEXTINPUT).
    (void)e;
}

void InputManager::refreshDeviceState()
{
    // OPENRCT2MINI input-plan Track 1 §3.1: see InputManager.h
    // declaration for full rationale. Refreshes the per-frame
    // keyboard and mouse state caches that getState(ShortcutInput)
    // reads. Called from process() before handleModifiers so the
    // modifier shortcuts' held-state polls see this frame's input.
    _mouseState = SDL_GetMouseState(nullptr, nullptr);

    int numkeys;
    auto keys = SDL_GetKeyboardState(&numkeys);
    _keyboardState.resize(numkeys);
    std::memcpy(_keyboardState.data(), keys, numkeys);
}

void InputManager::processHoldEvents()
{
    // OPENRCT2MINI input-plan Track 1 §3.1: _keyboardState and
    // _mouseState are now refreshed up-front in refreshDeviceState()
    // (called early in process(), before handleModifiers). The view-
    // scroll polls below still read both caches via getState — same
    // data, just refreshed at a friendlier point in the frame.

    // Check view scroll shortcuts
    _viewScroll.x = 0;
    _viewScroll.y = 0;

    if (!hasTextInputFocus())
    {
        auto& shortcutManager = GetShortcutManager();
        if (!shortcutManager.isPendingShortcutChange())
        {
            processViewScrollEvent(ShortcutId::kViewScrollUp, { 0, -1 });
            processViewScrollEvent(ShortcutId::kViewScrollDown, { 0, 1 });
            processViewScrollEvent(ShortcutId::kViewScrollLeft, { -1, 0 });
            processViewScrollEvent(ShortcutId::kViewScrollRight, { 1, 0 });
        }

        updateAnalogueScroll();
    }
}

void InputManager::processViewScrollEvent(std::string_view shortcutId, const ScreenCoordsXY& delta)
{
    auto& shortcutManager = GetShortcutManager();
    auto shortcut = shortcutManager.getShortcut(shortcutId);
    if (shortcut != nullptr && getState(*shortcut))
    {
        _viewScroll.x += delta.x;
        _viewScroll.y += delta.y;
    }
}

bool InputManager::getState(const RegisteredShortcut& shortcut) const
{
    for (const auto& i : shortcut.current)
    {
        if (getState(i))
        {
            return true;
        }
    }
    return false;
}

float InputManager::getAnalogStateAxis(const ShortcutInput& input) const
{
    if (input.kind != InputDeviceKind::joyAxis)
        return 0.0f;
    // Analog deadzone is intentionally LOOSER than the binding's
    // axisThreshold (16384 / 50%) — 8000 / ~24% gives users fine
    // control near the centre. The binding's axisThreshold still
    // gates the digital threshold-cross logic in getState (and the
    // shortcut-firing dispatch) so a binding "held" semantic doesn't
    // change.
    constexpr int32_t kAnalogDeadzone = 8000;

    // Pick the controller with the largest deflection in the bound
    // direction. Summing across multiple controllers would let two
    // half-pushed sticks combine into a full push, which is weird;
    // single-controller-wins matches the digital path where any
    // controller hitting threshold is enough.
    int32_t signedMax = 0;
    for (auto* gc : _gameControllers)
    {
        if (gc == nullptr)
            continue;
        const int16_t v = SDL_GameControllerGetAxis(
            gc, static_cast<SDL_GameControllerAxis>(input.button));
        if (input.axisDirection > 0)
        {
            if (v > signedMax)
                signedMax = v;
        }
        else if (input.axisDirection < 0)
        {
            if (v < signedMax)
                signedMax = v;
        }
    }

    const float deadzoneF = static_cast<float>(kAnalogDeadzone);
    if (input.axisDirection > 0)
    {
        if (signedMax <= kAnalogDeadzone)
            return 0.0f;
        return std::min(1.0f, (signedMax - deadzoneF) / (32767.0f - deadzoneF));
    }
    if (input.axisDirection < 0)
    {
        if (-signedMax <= kAnalogDeadzone)
            return 0.0f;
        return std::min(1.0f, (-signedMax - deadzoneF) / (32767.0f - deadzoneF));
    }
    return 0.0f;
}

float InputManager::getAnalogState(const RegisteredShortcut& shortcut) const
{
    // Only joyAxis bindings contribute analog magnitude. Digital
    // bindings (keyboard, joyButton) report via getState — and
    // mixing the two here would cause keyboard arrow keys to feed
    // both the smooth camera path (via _analogueScroll) AND the
    // discrete keyboard path (via _viewScroll → processViewScroll-
    // Event), which then cancel via the keyboardScroll subtraction
    // in handleViewScrolling. Caller decides how to combine analog
    // and digital input — typically: digital → discrete pixel ticks,
    // analog → smooth velocity.
    float maxMagnitude = 0.0f;
    for (const auto& binding : shortcut.current)
    {
        if (binding.kind == InputDeviceKind::joyAxis)
        {
            const float mag = getAnalogStateAxis(binding);
            if (mag > maxMagnitude)
                maxMagnitude = mag;
        }
    }
    return maxMagnitude;
}

// OPENRCT2MINI chord-shadow refactor (task #400):
// Compute the cardinality of a binding the same way ShortcutManager's
// dispatch path does (chord-modifier count + keyboard-modifier-group
// count + 1 for the action). Held-state polling needs the same number
// so the shadow check below has a directly comparable rank.
static size_t bindingCardinality(const ShortcutInput& binding) noexcept
{
    size_t card = binding.chordModifiers.size() + 1;
    if (binding.modifiers & (KMOD_LCTRL | KMOD_RCTRL))
        card++;
    if (binding.modifiers & (KMOD_LSHIFT | KMOD_RSHIFT))
        card++;
    if (binding.modifiers & (KMOD_LALT | KMOD_RALT))
        card++;
    if (binding.modifiers & (KMOD_LGUI | KMOD_RGUI))
        card++;
    return card;
}

bool InputManager::getState(const ShortcutInput& shortcut) const
{
    constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;
    auto modifiers = SDL_GetModState() & kUsefulModifiers;

    // OPENRCT2MINI gamepad-plan 1.3: chord-modifier prerequisite check
    // for held gamepad shortcuts. Every chord modifier must be in the
    // held-set for the binding to be considered "currently held". For
    // bindings with no chord modifiers this is a no-op pass.
    auto chordSatisfied = [&]() {
        for (uint32_t mod : shortcut.chordModifiers)
        {
            if (_heldGamepadButtons.find(mod) == _heldGamepadButtons.end())
                return false;
        }
        return true;
    };

    // OPENRCT2MINI chord-shadow (task #400): when held-state polling
    // for this binding, check whether any OTHER binding (in any
    // shortcut) sharing the same action button has strictly higher
    // cardinality AND is currently fully satisfied. If so, this lower-
    // cardinality binding is "shadowed" by the chord — the held-state
    // poll should report false even though the action button is held.
    //
    // Mirror of ShortcutManager::processEvent's cardinality-largest-
    // match-wins dispatch behaviour. Without this, binding `kInterface-
    // ConstructionZLock` to `R1+L1` chord while `kInterfaceShift-
    // Modifier` is bound to `L1` bare causes BOTH to report held when
    // R1+L1 are pressed — dispatch correctly suppresses shift's action,
    // but `_modifierKeyState` ends up with both ctrl AND shift bits,
    // so the construction tool engages both effects when the user
    // expected only the chord (ctrl/Z-lock) to win.
    //
    // Scoped to physical-input bindings only (keyboard / mouse /
    // joyButton). joyHat and joyAxis pass through unchanged — they
    // don't participate in chord matching the same way.
    const auto thisCard = bindingCardinality(shortcut);
    auto isShadowed = [&]() {
        if (shortcut.kind != InputDeviceKind::keyboard
            && shortcut.kind != InputDeviceKind::mouse
            && shortcut.kind != InputDeviceKind::joyButton)
            return false;
        const auto& mgr = GetShortcutManager();
        for (const auto& kv : mgr.shortcuts)
        {
            for (const auto& other : kv.second.current)
            {
                if (&other == &shortcut)
                    continue;
                if (other.kind != shortcut.kind)
                    continue;
                if (other.button != shortcut.button)
                    continue;
                const auto otherCard = bindingCardinality(other);
                if (otherCard <= thisCard)
                    continue;

                // `other` shares the action button and outranks us.
                // Check its prerequisites are all currently met.
                bool prereqsOk = true;
                for (uint32_t mod : other.chordModifiers)
                {
                    if (_heldGamepadButtons.find(mod) == _heldGamepadButtons.end())
                    {
                        prereqsOk = false;
                        break;
                    }
                }
                if (!prereqsOk)
                    continue;
                const uint32_t requiredKbdMods = other.modifiers & kUsefulModifiers;
                if (requiredKbdMods != 0)
                {
                    // Check each side-pair (LCTRL/RCTRL etc.) — at least
                    // one side of each required group must be in actual.
                    auto sideOk = [&](uint32_t left, uint32_t right) {
                        const uint32_t needed = requiredKbdMods & (left | right);
                        if (needed == 0)
                            return true;
                        return (modifiers & needed) != 0;
                    };
                    if (!sideOk(KMOD_LCTRL, KMOD_RCTRL)
                        || !sideOk(KMOD_LSHIFT, KMOD_RSHIFT)
                        || !sideOk(KMOD_LALT, KMOD_RALT)
                        || !sideOk(KMOD_LGUI, KMOD_RGUI))
                    {
                        continue;
                    }
                }
                // `other` outranks us and is satisfied — we're shadowed.
                return true;
            }
        }
        return false;
    };

    // OPENRCT2MINI shift/ctrl-modifier refactor: bare-modifier keyboard
    // bindings (LSHIFT / RSHIFT / LCTRL / RCTRL / LALT / RALT with no
    // chord prefix) bypass the modifier gate below. The modifier-state
    // comparison would always mismatch — the modifier we're trying to
    // detect IS the one currently pressed, so SDL's mod state has
    // exactly that bit set while shortcut.modifiers is 0. Read the
    // keyboard scancode bitmap directly instead.
    if (shortcut.kind == InputDeviceKind::keyboard && shortcut.modifiers == 0)
    {
        const auto b = shortcut.button;
        const bool isBareModifierKey
            = (b == SDLK_LSHIFT || b == SDLK_RSHIFT || b == SDLK_LCTRL || b == SDLK_RCTRL
               || b == SDLK_LALT || b == SDLK_RALT);
        if (isBareModifierKey)
        {
            auto scanCode = static_cast<size_t>(SDL_GetScancodeFromKey(b));
            if (scanCode >= _keyboardState.size() || !_keyboardState[scanCode])
                return false;
            // Even a held bare modifier key can be shadowed by a chord-
            // style binding that uses the same scancode as a regular
            // chord member. Rare for SHIFT/CTRL keys but kept symmetric
            // with the non-fast-path cases below.
            return !isShadowed();
        }
    }

    // OPENRCT2MINI shift/ctrl-modifier refactor + chord-click fix:
    // Held-state poll uses a one-sided modifier comparison — the
    // shortcut's required modifiers must all be present in actual,
    // but actual is allowed to have extra modifiers the shortcut
    // doesn't ask for. This mirrors the dispatch-time HasModifier
    // relaxation in ShortcutInput.cpp: holding kInterfaceShift-
    // Modifier's keyboard default (LSHIFT) puts KMOD_SHIFT into
    // SDL_GetModState, and that mustn't block bare-button bindings
    // (cursor.click on PAD A, etc.) from polling as "held". The
    // held-state poll has no cardinality scoring — it's just "is
    // this input active right now?" — so accepting extra mods is
    // the correct relaxation here.
    auto modifierSatisfied = [&]() {
        auto matchGroup = [&](uint32_t left, uint32_t right) {
            const uint32_t shortcutBits = shortcut.modifiers & (left | right);
            if (shortcutBits == 0)
                return true;
            return (modifiers & shortcutBits) != 0;
        };
        return matchGroup(KMOD_LCTRL, KMOD_RCTRL) && matchGroup(KMOD_LSHIFT, KMOD_RSHIFT)
            && matchGroup(KMOD_LALT, KMOD_RALT) && matchGroup(KMOD_LGUI, KMOD_RGUI);
    };
    if (modifierSatisfied())
    {
        switch (shortcut.kind)
        {
            case InputDeviceKind::mouse:
            {
                if (_mouseState & (1 << shortcut.button))
                {
                    if (isShadowed())
                        break;
                    return true;
                }
                break;
            }
            case InputDeviceKind::keyboard:
            {
                auto scanCode = static_cast<size_t>(SDL_GetScancodeFromKey(shortcut.button));
                if (scanCode < _keyboardState.size() && _keyboardState[scanCode])
                {
                    if (isShadowed())
                        break;
                    return true;
                }
                break;
            }
            case InputDeviceKind::joyButton:
            {
                if (!chordSatisfied())
                    break;
                // OPENRCT2MINI gamepad-plan: shortcut.button holds an
                // SDL_CONTROLLER_BUTTON_* enum value (set in queueInputEvent
                // from e.cbutton.button); _heldGamepadButtons is keyed on
                // the same enum so it's the canonical "is this controller
                // button currently held" query.
                //
                // The previous SDL_JoystickGetButton(joystick, shortcut.button)
                // path was broken for any controller whose raw joystick
                // button indices don't 1:1 match the GameController enum
                // — which is virtually every modern controller (DS4, Xbox,
                // Switch Pro, etc.). For DPAD_UP (enum 11) the raw query
                // returned the state of joystick button 11, which on a DS4
                // is L3 stick-click, not DPAD_UP. The held-state poll for
                // any cursor.* / OSK-routed binding always read false on
                // host gamepads as a result.
                if (_heldGamepadButtons.find(shortcut.button) != _heldGamepadButtons.end())
                {
                    if (isShadowed())
                        break;
                    return true;
                }
                break;
            }
            case InputDeviceKind::joyHat:
            {
                for (auto* gameController : _gameControllers)
                {
                    // Get the underlying joystick to maintain compatibility with hat functionality
                    auto* joystick = SDL_GameControllerGetJoystick(gameController);
                    if (joystick)
                    {
                        auto numHats = SDL_JoystickNumHats(joystick);
                        for (int i = 0; i < numHats; i++)
                        {
                            auto hat = SDL_JoystickGetHat(joystick, i);
                            if (hat & shortcut.button)
                            {
                                return true;
                            }
                        }
                    }
                }
                break;
            }
            case InputDeviceKind::joyAxis:
            {
                // OPENRCT2MINI gamepad-plan 1.3: held-state for joyAxis
                // bindings (triggers, stick-direction-as-button) consults
                // _heldGamepadButtons via the axis-as-button encoding. The
                // hysteresis already applied at queueInputEvent time
                // means this is a clean digital read — set membership
                // implies "currently held past press threshold".
                if (!chordSatisfied())
                    break;
                const uint32_t encoded = encodeAxisAsButton(shortcut.button, shortcut.axisDirection);
                if (_heldGamepadButtons.find(encoded) != _heldGamepadButtons.end())
                {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

// OPENRCT2MINI per-binding Modifier mode: query whether the modifier
// portion of a shortcut's chord is currently held. "Modifier portion"
// = every key in the chord EXCEPT the trigger (action) key. Returns
// true if any of the shortcut's bindings has at least 2 keys AND all
// of its modifier keys are presently held — irrespective of trigger.
bool InputManager::isShortcutModifierHeld(std::string_view id) const
{
    constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;
    const uint32_t actualMods = SDL_GetModState() & kUsefulModifiers;

    auto& mgr = GetShortcutManager();
    auto* shortcut = mgr.getShortcut(id);
    if (shortcut == nullptr)
        return false;

    for (const auto& binding : shortcut->current)
    {
        // Keyboard / mouse path: chord = modifier mask + button. Modifier
        // portion is non-empty iff modifier mask != 0.
        if (binding.kind == InputDeviceKind::keyboard || binding.kind == InputDeviceKind::mouse)
        {
            if (binding.modifiers == 0)
                continue;
            auto matchGroup = [&](uint32_t left, uint32_t right) {
                const uint32_t shortcutBits = binding.modifiers & (left | right);
                if (shortcutBits == 0)
                    return true;
                return (actualMods & shortcutBits) != 0;
            };
            if (matchGroup(KMOD_LCTRL, KMOD_RCTRL) && matchGroup(KMOD_LSHIFT, KMOD_RSHIFT)
                && matchGroup(KMOD_LALT, KMOD_RALT) && matchGroup(KMOD_LGUI, KMOD_RGUI))
            {
                return true;
            }
        }
        else if (binding.kind == InputDeviceKind::joyButton || binding.kind == InputDeviceKind::joyAxis)
        {
            // Gamepad path: modifier portion = chordModifiers vector
            // (non-empty when the chord has ≥ 2 keys).
            if (binding.chordModifiers.empty())
                continue;
            bool allHeld = true;
            for (uint32_t mod : binding.chordModifiers)
            {
                if (_heldGamepadButtons.find(mod) == _heldGamepadButtons.end())
                {
                    allHeld = false;
                    break;
                }
            }
            if (allHeld)
                return true;
        }
        // joyHat bindings only ever have 1 effective input — no modifier
        // portion. Same for unbound / unrecognised kinds.
    }
    return false;
}

bool InputManager::hasTextInputFocus() const
{
    if (Windows::IsUsingWidgetTextBox() || gChatOpen)
        return true;

    auto* windowMgr = GetWindowManager();
    auto w = windowMgr->FindByClass(WindowClass::textinput);
    if (w != nullptr)
        return true;

    auto& console = GetInGameConsole();
    if (console.IsOpen())
        return true;

    return false;
}
