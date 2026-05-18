/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "MouseInput.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/ViewportInteraction.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/interface/Cursors.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/MapSelection.h>
#include <optional>

namespace OpenRCT2
{
    using namespace OpenRCT2::Ui;
    using namespace OpenRCT2::Ui::Windows;

    struct RCTMouseData
    {
        uint32_t x;
        uint32_t y;
        MouseState state;
    };

    static RCTMouseData _mouseInputQueue[64];
    static uint8_t _mouseInputQueueReadIndex = 0;
    static uint8_t _mouseInputQueueWriteIndex = 0;

    static std::optional<uint32_t> _ticksSinceDragStart;
    static WidgetRef _dragWidget;
    static uint8_t _dragScrollIndex;
    static int32_t _originalWindowWidth;
    static int32_t _originalWindowHeight;

    static uint8_t _currentScrollIndex;
    static uint8_t _currentScrollArea;

    ScreenCoordsXY gInputDragLast;

    uint32_t gTooltipCloseTimeout;
    WidgetRef gTooltipWidget;
    ScreenCoordsXY gTooltipCursor;

    static std::optional<uint32_t> _clickRepeatTicks;

    static MouseState GameGetNextInput(ScreenCoordsXY& screenCoords);
    static void InputWidgetOver(const ScreenCoordsXY& screenCoords, WindowBase* w, WidgetIndex widgetIndex);
    static void InputWidgetOverChangeCheck(WindowClass windowClass, WindowNumber windowNumber, WidgetIndex widgetIndex);
    static void InputWidgetOverFlatbuttonInvalidate();
    void ProcessMouseOver(const ScreenCoordsXY& screenCoords);
    void ProcessMouseTool(const ScreenCoordsXY& screenCoords);
    void InvalidateScroll();
    static RCTMouseData* GetMouseInput();
    void TileElementRightClick(int32_t type, TileElement* tileElement, const ScreenCoordsXY& screenCoords);
    static void GameHandleInputMouse(const ScreenCoordsXY& screenCoords, MouseState state);
    static void InputWidgetLeft(const ScreenCoordsXY& screenCoords, WindowBase* w, WidgetIndex widgetIndex);
    void InputStateWidgetPressed(
        const ScreenCoordsXY& screenCoords, MouseState state, WidgetIndex widgetIndex, WindowBase* w, Widget* widget);
    void SetCursor(CursorID cursor_id);
    static void InputWindowPositionContinue(
        WindowBase& w, const ScreenCoordsXY& lastScreenCoords, const ScreenCoordsXY& newScreenCoords);
    static void InputWindowPositionEnd(WindowBase& w, const ScreenCoordsXY& screenCoords);
    static void InputWindowResizeBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
    static void InputWindowResizeContinue(WindowBase& w, const ScreenCoordsXY& screenCoords);
    static void InputWindowResizeEnd();
    static void InputViewportDragBegin(WindowBase& w);
    static void InputViewportDragContinue();
    static void InputViewportDragEnd();
    static void InputScrollBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
    static void InputScrollContinue(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
    static void InputScrollEnd();
    static void InputScrollPartUpdateHThumb(WindowBase& w, WidgetIndex widgetIndex, int32_t x, int32_t scroll_id);
    static void InputScrollPartUpdateHLeft(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id);
    static void InputScrollPartUpdateHRight(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id);
    static void InputScrollPartUpdateVThumb(WindowBase& w, WidgetIndex widgetIndex, int32_t y, int32_t scroll_id);
    static void InputScrollPartUpdateVTop(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id);
    static void InputScrollPartUpdateVBottom(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id);
    static void InputUpdateTooltip(WindowBase* w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);

#pragma region Mouse input

    /**
     *
     *  rct2: 0x006EA627
     */
    void GameHandleInput()
    {
        InvalidateAllWindowsAfterInput();

        MouseState state;
        ScreenCoordsXY screenCoords;
        while ((state = GameGetNextInput(screenCoords)) != MouseState::released)
        {
            GameHandleInputMouse(screenCoords, state);
        }

        if (gInputFlags.has(InputFlag::rightMousePressed))
        {
            GameHandleInputMouse(screenCoords, state);
        }
        else
        {
            int32_t screenWidth = ContextGetWidth();
            int32_t screenHeight = ContextGetHeight();
            screenCoords.x = std::clamp(screenCoords.x, 0, screenWidth - 1);
            screenCoords.y = std::clamp(screenCoords.y, 0, screenHeight - 1);

            GameHandleInputMouse(screenCoords, state);
            ProcessMouseOver(screenCoords);
            ProcessMouseTool(screenCoords);
        }
    }

    /**
     *
     *  rct2: 0x006E83C7
     */
    static MouseState GameGetNextInput(ScreenCoordsXY& screenCoords)
    {
        RCTMouseData* input = GetMouseInput();
        if (input == nullptr)
        {
            const CursorState* cursorState = ContextGetCursorState();
            screenCoords = cursorState->position;
            return MouseState::released;
        }

        screenCoords.x = input->x;
        screenCoords.y = input->y;
        return input->state;
    }

    /**
     *
     *  rct2: 0x00407074
     */
    static RCTMouseData* GetMouseInput()
    {
        // Check if that location has been written to yet
        if (_mouseInputQueueReadIndex == _mouseInputQueueWriteIndex)
        {
            return nullptr;
        }

        RCTMouseData* result = &_mouseInputQueue[_mouseInputQueueReadIndex];
        _mouseInputQueueReadIndex = (_mouseInputQueueReadIndex + 1) % std::size(_mouseInputQueue);
        return result;
    }

    /**
     *
     *  rct2: 0x006E957F
     */
    static void InputScrollDragBegin(const ScreenCoordsXY& screenCoords, WindowBase* w, WidgetIndex widgetIndex)
    {
        _inputState = InputState::ScrollRight;
        gInputDragLast = screenCoords;
        _dragWidget.windowClassification = w->classification;
        _dragWidget.windowNumber = w->number;
        _dragWidget.widgetIndex = widgetIndex;
        _ticksSinceDragStart = gCurrentRealTimeTicks;

        _dragScrollIndex = WindowGetScrollDataIndex(*w, widgetIndex);
        ContextHideCursor();
    }

    /**
     * Based on (heavily changed)
     *  rct2: 0x006E9E0E,  0x006E9ED0
     */
    static void InputScrollDragContinue(const ScreenCoordsXY& screenCoords, WindowBase* w)
    {
        WidgetIndex widgetIndex = _dragWidget.widgetIndex;
        uint8_t scrollIndex = _dragScrollIndex;

        const auto& widget = w->widgets[widgetIndex];
        auto& scroll = w->scrolls[scrollIndex];

        ScreenCoordsXY differentialCoords = screenCoords - gInputDragLast;
        if (differentialCoords.x == 0 && differentialCoords.y == 0)
            return;

        if (scroll.flags & HSCROLLBAR_VISIBLE)
        {
            int16_t size = widget.width() - 2;
            if (scroll.flags & VSCROLLBAR_VISIBLE)
                size -= 11;
            size = std::max(0, scroll.contentWidth - size);
            scroll.contentOffsetX = std::min<uint16_t>(std::max(0, scroll.contentOffsetX + differentialCoords.x), size);
        }

        if (scroll.flags & VSCROLLBAR_VISIBLE)
        {
            int16_t size = widget.height() - 2;
            if (scroll.flags & HSCROLLBAR_VISIBLE)
                size -= 11;
            size = std::max(0, scroll.contentHeight - size);
            scroll.contentOffsetY = std::min<uint16_t>(std::max(0, scroll.contentOffsetY + differentialCoords.y), size);
        }

        widgetScrollUpdateThumbs(*w, widgetIndex);

        auto* windowMgr = GetWindowManager();
        windowMgr->InvalidateByNumber(w->classification, w->number);

        ScreenCoordsXY fixedCursorPosition = {
            static_cast<int32_t>(std::ceil(gInputDragLast.x * Config::Get().general.windowScale)),
            static_cast<int32_t>(std::ceil(gInputDragLast.y * Config::Get().general.windowScale))
        };

        ContextSetCursorPosition(fixedCursorPosition);
    }

    /**
     *
     *  rct2: 0x006E8ACB
     */
    static void InputScrollRight(const ScreenCoordsXY& screenCoords, MouseState state)
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByNumber(_dragWidget.windowClassification, _dragWidget.windowNumber);
        if (w == nullptr)
        {
            ContextShowCursor();
            _inputState = InputState::Reset;
            return;
        }

        switch (state)
        {
            case MouseState::released:
                if (screenCoords.x != 0 || screenCoords.y != 0)
                {
                    _ticksSinceDragStart = std::nullopt;
                    InputScrollDragContinue(screenCoords, w);
                }
                break;
            case MouseState::rightRelease:
                _inputState = InputState::Reset;
                ContextShowCursor();
                break;
            case MouseState::leftPress:
            case MouseState::leftRelease:
            case MouseState::rightPress:
                // Function only handles right button, so it's the only one relevant
                break;
        }
    }

    /**
     *
     *  rct2: 0x006E8655
     */
    static void GameHandleInputMouse(const ScreenCoordsXY& screenCoords, MouseState state)
    {
        WindowBase* w;
        Widget* widget;
        WidgetIndex widgetIndex;

        auto* windowMgr = GetWindowManager();

        // Get window and widget under cursor position
        w = windowMgr->FindFromPoint(screenCoords);
        widgetIndex = w == nullptr ? kWidgetIndexNull : windowMgr->FindWidgetFromPoint(*w, screenCoords);
        widget = widgetIndex == kWidgetIndexNull ? nullptr : &w->widgets[widgetIndex];

        switch (_inputState)
        {
            case InputState::Reset:
                WindowTooltipReset(screenCoords);
                [[fallthrough]];
            case InputState::Normal:
                switch (state)
                {
                    case MouseState::released:
                        InputWidgetOver(screenCoords, w, widgetIndex);
                        break;
                    case MouseState::leftPress:
                        InputWidgetLeft(screenCoords, w, widgetIndex);
                        break;
                    case MouseState::rightPress:
                        // OPENRCT2MINI input-rework: cursor.cancel (the
                        // shortcut bound to RMB / PAD B / X by default)
                        // no longer initiates ANY drag. Window-drag,
                        // scroll-drag and camera-drag are owned by
                        // kInterfaceCameraDrag's held-state poll in
                        // UiContext::ProcessWorldCursor → InputContextDrag
                        // BeginAtCursor / InputContextDragEndCurrent.
                        // cursor.cancel's right-press now does only the
                        // ambient cleanup the legacy upstream code did:
                        // close the tooltip and raise the window under
                        // the cursor to the front (so a single right-tap
                        // is still a useful "interact with this window"
                        // gesture). All the drag-init dispatch lives in
                        // InputContextDragBeginAtCursor.
                        windowMgr->CloseByClass(WindowClass::tooltip);
                        if (w != nullptr)
                        {
                            w = windowMgr->BringToFront(*w);
                        }
                        break;
                    case MouseState::leftRelease:
                    case MouseState::rightRelease:
                        // In this switch only button presses are relevant
                        break;
                }
                break;
            case InputState::WidgetPressed:
                InputStateWidgetPressed(screenCoords, state, widgetIndex, w, widget);
                break;
            case InputState::PositioningWindow:
                w = windowMgr->FindByNumber(_dragWidget.windowClassification, _dragWidget.windowNumber);
                if (w == nullptr)
                {
                    _inputState = InputState::Reset;
                }
                else
                {
                    InputWindowPositionContinue(*w, gInputDragLast, screenCoords);
                    // OPENRCT2MINI: drag may have begun via left-click on the
                    // caption (existing) OR via right-click anywhere on
                    // window chrome (new). Either button releasing ends it.
                    if (state == MouseState::leftRelease || state == MouseState::rightRelease)
                    {
                        InputWindowPositionEnd(*w, screenCoords);
                    }
                }
                break;
            case InputState::ViewportRight:
                // OPENRCT2MINI mouse-input refactor: camera drag end /
                // short-press right-click is owned by the
                // kInterfaceCameraDrag poll now (handles non-mouse
                // bindings like PAD B). Motion-continue stays here;
                // it's driven by the per-frame "no events queued"
                // released fallback the same way regardless of which
                // input started the drag. RMB-specific rightRelease
                // is no longer special-cased.
                if (state == MouseState::released)
                {
                    InputViewportDragContinue();
                }
                break;
            case InputState::DropdownActive:
                InputStateWidgetPressed(screenCoords, state, widgetIndex, w, widget);
                break;
            case InputState::ViewportLeft:
                w = windowMgr->FindByNumber(_dragWidget.windowClassification, _dragWidget.windowNumber);
                if (w == nullptr)
                {
                    _inputState = InputState::Reset;
                    break;
                }

                switch (state)
                {
                    case MouseState::released:
                        if (w->viewport == nullptr)
                        {
                            _inputState = InputState::Reset;
                            break;
                        }

                        if (!gInputFlags.has(InputFlag::leftMousePressed))
                            break;

                        if (w->classification != _dragWidget.windowClassification || w->number != _dragWidget.windowNumber
                            || !gInputFlags.has(InputFlag::toolActive))
                        {
                            break;
                        }

                        w = windowMgr->FindByNumber(gCurrentToolWidget.windowClassification, gCurrentToolWidget.windowNumber);
                        if (w == nullptr)
                        {
                            break;
                        }

                        w->onToolDrag(gCurrentToolWidget.widgetIndex, screenCoords);
                        break;
                    case MouseState::leftRelease:
                        _inputState = InputState::Reset;
                        if (_dragWidget.windowNumber == w->number)
                        {
                            if (gInputFlags.has(InputFlag::toolActive))
                            {
                                w = windowMgr->FindByNumber(
                                    gCurrentToolWidget.windowClassification, gCurrentToolWidget.windowNumber);
                                if (w != nullptr)
                                {
                                    w->onToolUp(gCurrentToolWidget.widgetIndex, screenCoords);
                                }
                            }
                            else if (!gInputFlags.has(InputFlag::leftMousePressed))
                            {
                                ViewportInteractionLeftClick(screenCoords);
                            }
                        }
                        break;
                    case MouseState::leftPress:
                    case MouseState::rightPress:
                    case MouseState::rightRelease:
                        // In this switch only left button release is relevant
                        break;
                }
                break;
            case InputState::ScrollLeft:
                switch (state)
                {
                    case MouseState::released:
                        InputScrollContinue(*w, widgetIndex, screenCoords);
                        break;
                    case MouseState::leftRelease:
                        InputScrollEnd();
                        break;
                    case MouseState::leftPress:
                    case MouseState::rightPress:
                    case MouseState::rightRelease:
                        // In this switch only left button release is relevant
                        break;
                }
                break;
            case InputState::Resizing:
                w = windowMgr->FindByNumber(_dragWidget.windowClassification, _dragWidget.windowNumber);
                if (w == nullptr)
                {
                    _inputState = InputState::Reset;
                }
                else
                {
                    if (state == MouseState::leftRelease)
                    {
                        InputWindowResizeEnd();
                    }
                    if (state == MouseState::released || state == MouseState::leftRelease)
                    {
                        InputWindowResizeContinue(*w, screenCoords);
                    }
                }
                break;
            case InputState::ScrollRight:
                InputScrollRight(screenCoords, state);
                break;
        }
    }

#pragma region Window positioning / resizing

    void InputWindowPositionBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords)
    {
        _inputState = InputState::PositioningWindow;
        gInputDragLast = screenCoords - w.windowPos;
        _dragWidget.windowClassification = w.classification;
        _dragWidget.windowNumber = w.number;
        _dragWidget.widgetIndex = widgetIndex;
    }

    static void InputWindowPositionContinue(
        WindowBase& w, const ScreenCoordsXY& lastScreenCoords, const ScreenCoordsXY& newScreenCoords)
    {
        int32_t snapProximity = w.flags.has(WindowFlag::noSnapping) ? 0 : Config::Get().general.windowSnapProximity;
        WindowMoveAndSnap(w, newScreenCoords - lastScreenCoords, snapProximity);
    }

    static void InputWindowPositionEnd(WindowBase& w, const ScreenCoordsXY& screenCoords)
    {
        _inputState = InputState::Normal;
        gTooltipCloseTimeout = 0;
        gTooltipWidget = _dragWidget;
        w.onMoved(screenCoords);
    }

    // OPENRCT2MINI W2: held-state-driven window drag.
    //
    // Polled once per frame from InputManager::processHoldEvents. The
    // shortcut (default "C", which is the device's face X button under the
    // W0 keymap) begins a positioning drag on rising edge and ends it on
    // falling edge. Internally tracks the previous-frame held state for
    // edge detection.
    //
    // Begin path is gated on:
    //   * _inputState being Normal/Reset — don't stomp on resize, scroll,
    //     viewport drag, widget press, dropdown, etc. that already own the
    //     state machine.
    //   * A non-toolbar window being under the cursor. stickToBack is the
    //     main viewport, stickToFront is the in-game console; noBackground
    //     covers the top/bottom toolbars and tooltips. None of those are
    //     draggable today via the title-bar path either.
    //
    // End path looks up the dragged window via the static _dragWidget set
    // by InputWindowPositionBegin and calls InputWindowPositionEnd, mirror-
    // ing what the leftRelease branch in GameHandleInputMouse does.
    static void InputWindowResizeBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords)
    {
        _inputState = InputState::Resizing;
        gInputDragLast = screenCoords;
        _dragWidget.windowClassification = w.classification;
        _dragWidget.windowNumber = w.number;
        _dragWidget.widgetIndex = widgetIndex;
        _originalWindowWidth = w.width;
        _originalWindowHeight = w.height;
    }

    static void InputWindowResizeContinue(WindowBase& w, const ScreenCoordsXY& screenCoords)
    {
        if (screenCoords.y < (ContextGetHeight() - 2))
        {
            auto differentialCoords = screenCoords - gInputDragLast;
            int32_t targetWidth = _originalWindowWidth + differentialCoords.x - w.width;
            int32_t targetHeight = _originalWindowHeight + differentialCoords.y - w.height;

            WindowResizeByDelta(w, targetWidth, targetHeight);
        }
    }

    static void InputWindowResizeEnd()
    {
        _inputState = InputState::Normal;
        gTooltipCloseTimeout = 0;
        gTooltipWidget = _dragWidget;
    }

#pragma endregion

#pragma region Viewport dragging

    static void InputViewportDragBegin(WindowBase& w)
    {
        w.flags.unset(WindowFlag::scrollingToLocation);
        _inputState = InputState::ViewportRight;
        _dragWidget.windowClassification = w.classification;
        _dragWidget.windowNumber = w.number;
        _ticksSinceDragStart = gCurrentRealTimeTicks;
        auto cursorPosition = ContextGetCursorPosition();
        gInputDragLast = cursorPosition;
        if (!Config::Get().general.invertViewportDrag)
        {
            ContextHideCursor();
        }

        // Only unfollow sprites for the main window or ‘extra viewport’ windows.
        // Don’t unfollow for windows where the viewport is always supposed to follow (e.g. Ride, Guest, Staff).
        auto mainWindow = WindowGetMain();
        if (&w == mainWindow || w.classification == WindowClass::viewport)
        {
            WindowUnfollowSprite(w);
        }
    }

    static void InputViewportDragContinue()
    {
        WindowBase* w;
        Viewport* viewport;

        auto newDragCoords = ContextGetCursorPosition();
        auto differentialCoords = newDragCoords - gInputDragLast;
        if (differentialCoords.x == 0 && differentialCoords.y == 0)
            return;

        auto* windowMgr = GetWindowManager();
        w = windowMgr->FindByNumber(_dragWidget.windowClassification, _dragWidget.windowNumber);

        // #3294: Window can be closed during a drag session, so just finish
        //        the session if the window no longer exists
        if (w == nullptr)
        {
            InputViewportDragEnd();
            return;
        }

        viewport = w->viewport;
        if (viewport == nullptr)
        {
            ContextShowCursor();
            _inputState = InputState::Reset;
        }
        else if (differentialCoords.x != 0 || differentialCoords.y != 0)
        {
            if (!w->flags.has(WindowFlag::noScrolling))
            {
                // User dragged a scrollable viewport

                // If the drag time is less than 500 the "drag" is usually interpreted as a right click.
                // As the user moved the mouse, don't interpret it as right click in any case.
                _ticksSinceDragStart = std::nullopt;

                // applying the zoom only with negative values avoids a "deadzone" effect where small positive value round to
                // zero.
                const bool posX = differentialCoords.x > 0;
                const bool posY = differentialCoords.y > 0;
                differentialCoords.x = (viewport->zoom + 1).ApplyTo(-std::abs(differentialCoords.x));
                differentialCoords.y = (viewport->zoom + 1).ApplyTo(-std::abs(differentialCoords.y));
                differentialCoords.x = posX ? -differentialCoords.x : differentialCoords.x;
                differentialCoords.y = posY ? -differentialCoords.y : differentialCoords.y;

                if (Config::Get().general.invertViewportDrag)
                {
                    w->savedViewPos -= differentialCoords;
                }
                else
                {
                    w->savedViewPos += differentialCoords;
                }
            }
        }

#ifndef __EMSCRIPTEN__
        const CursorState* cursorState = ContextGetCursorState();
        if (cursorState->touch || Config::Get().general.invertViewportDrag)
        {
            gInputDragLast = newDragCoords;
        }
        else
        {
            ContextSetCursorPosition(gInputDragLast);
        }
#else
        gInputDragLast = newDragCoords;
#endif
    }

    static void InputViewportDragEnd()
    {
        _inputState = InputState::Reset;
        ContextShowCursor();
    }

    // OPENRCT2MINI mouse-input refactor / cursor-cancel-drag split:
    // public entry points for the kInterfaceCameraDrag held-state poll.
    // The poll lives in UiContext::ProcessWorldCursor. Calling shortcut
    // code from MouseInput.cpp would pull half of openrct2-ui in here,
    // so the poll owns rising / falling edge detection and we just
    // expose Begin / End / state queries.
    //
    // InputContextDragBeginAtCursor is a context-sensitive dispatcher.
    // Same priority order the legacy rightPress switch used (before the
    // drag-init was stripped out of it):
    //   1. Cursor over a main viewport (stickToBack / noBackground /
    //      noTitleBar window with a viewport widget under cursor) →
    //      camera-pan via InputViewportDragBegin. Tap-and-release <
    //      500ms fires ViewportInteractionRightClick (the "delete tile"
    //      action) — the camera-drag poll handles that fallback.
    //   2. Cursor over a scroll widget whose content actually overflows
    //      → InputScrollDragBegin. Non-overflowing scroll widgets fall
    //      through to step 3 (drag-scroll on a fits-in-place list is a
    //      surprise no-op; falling through to window-drag matches user
    //      intent — "I'm trying to move this dialog").
    //   3. Otherwise, on a title-barred non-toolbar window → bring it
    //      forward and start a window-position drag.
    // Toolbars / tooltips / dropdowns / error popups / debug overlays
    // are filtered out by the isUndraggableShell flag check.
    void InputContextDragBeginAtCursor()
    {
        // Re-entrancy guard: don't start a new drag while another drag
        // (left-caption drag, left-scrollbar, prior right-drag) is
        // already in progress. Re-entering would corrupt the shared
        // _dragWidget bookkeeping.
        if (_inputState != InputState::Reset && _inputState != InputState::Normal)
            return;

        const auto cursorPosition = ContextGetCursorPosition();
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindFromPoint(cursorPosition);
        if (w == nullptr)
            return;

        const WidgetIndex widgetIndex = windowMgr->FindWidgetFromPoint(*w, cursorPosition);
        const Widget* widget = (widgetIndex == kWidgetIndexNull) ? nullptr : &w->widgets[widgetIndex];
        const auto wType = widget != nullptr ? widget->type : WidgetType::empty;
        const bool isUndraggableShell = w->flags.hasAny(
            WindowFlag::stickToBack, WindowFlag::noBackground, WindowFlag::noTitleBar);

        // 1. Camera-pan over the main/extra viewport. Legacy rightPress
        // skipped camera-drag in the title-sequence scene (no
        // meaningful camera to drive) and the track-designs-manager
        // scene (no main viewport at all). Both gates apply only to
        // the camera branch — the scroll/window-drag branches below
        // still work because user-facing dialogs like ScenarioSelect
        // appear over the title scene and need to scroll-drag.
        if (isUndraggableShell && wType == WidgetType::viewport)
        {
            if (gLegacyScene == LegacyScene::trackDesignsManager
                || gLegacyScene == LegacyScene::titleSequence)
                return;
            InputViewportDragBegin(*w);
            return;
        }

        // 2. Scroll-drag — only on widgets that actually overflow.
        if (!isUndraggableShell && wType == WidgetType::scroll && widget != nullptr)
        {
            const auto scrollIdx = WindowGetScrollDataIndex(*w, widgetIndex);
            const auto& scroll = w->scrolls[scrollIdx];
            const bool hOverflow = (widget->content & SCROLL_HORIZONTAL)
                && scroll.contentWidth > widget->width();
            const bool vOverflow = (widget->content & SCROLL_VERTICAL)
                && scroll.contentHeight > widget->height();
            if (hOverflow || vOverflow)
            {
                InputScrollDragBegin(cursorPosition, w, widgetIndex);
                return;
            }
            // fall through to window-drag for non-overflowing scrolls.
        }

        // 3. Window-position drag. BringToFront has historically been
        // a side-effect of the rightPress that initiated the drag —
        // since rightPress no longer drives drag-init, do it here so
        // the visual cue still happens regardless of how the drag was
        // triggered (mouse, gamepad, keyboard binding).
        if (!isUndraggableShell)
        {
            w = windowMgr->BringToFront(*w);
            if (w == nullptr)
                return;
            // widgetIndex 0 (frame) when cursor is in a dead zone —
            // only used for tooltip restoration on release, harmless.
            InputWindowPositionBegin(
                *w, widgetIndex == kWidgetIndexNull ? 0 : widgetIndex, cursorPosition);
        }
    }

    // End whichever drag is currently in progress. Safe to call when
    // none of the three drag states is active — it's a no-op.
    void InputContextDragEndCurrent()
    {
        switch (_inputState)
        {
            case InputState::ViewportRight:
                InputViewportDragEnd();
                break;
            case InputState::ScrollRight:
                // Mirrors the rightRelease branch of InputScrollRight.
                _inputState = InputState::Reset;
                ContextShowCursor();
                break;
            case InputState::PositioningWindow:
            {
                auto* windowMgr = GetWindowManager();
                auto* w = windowMgr->FindByNumber(
                    _dragWidget.windowClassification, _dragWidget.windowNumber);
                if (w != nullptr)
                {
                    InputWindowPositionEnd(*w, ContextGetCursorPosition());
                }
                else
                {
                    _inputState = InputState::Reset;
                }
                break;
            }
            default:
                break;
        }
    }

    bool CameraDragWasShortPress()
    {
        return _ticksSinceDragStart.has_value()
            && gCurrentRealTimeTicks - _ticksSinceDragStart.value() < 500;
    }

    bool CameraDragInProgress()
    {
        return _inputState == InputState::ViewportRight;
    }

#pragma endregion

#pragma region Scroll bars

    static void InputScrollBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords)
    {
        const auto& widget = w.widgets[widgetIndex];

        _inputState = InputState::ScrollLeft;
        gPressedWidget.windowClassification = w.classification;
        gPressedWidget.windowNumber = w.number;
        gPressedWidget.widgetIndex = widgetIndex;
        gTooltipCursor = screenCoords;

        int32_t scroll_area, scroll_id;
        ScreenCoordsXY scrollCoords;
        scroll_id = 0; // safety
        widgetScrollGetPart(w, &widget, screenCoords, scrollCoords, &scroll_area, &scroll_id);

        _currentScrollArea = scroll_area;
        _currentScrollIndex = scroll_id;
        w.onScrollSelect(scroll_id, scroll_area);
        if (scroll_area == SCROLL_PART_VIEW)
        {
            w.onScrollMouseDown(scroll_id, scrollCoords);
            return;
        }

        const auto& widg = w.widgets[widgetIndex];
        auto& scroll = w.scrolls[scroll_id];

        int32_t widget_width = widg.width() - 2;
        if (scroll.flags & VSCROLLBAR_VISIBLE)
            widget_width -= kScrollBarWidth + 1;
        int32_t widget_content_width = std::max(scroll.contentWidth - widget_width, 0);

        int32_t widget_height = widg.bottom - widg.top - 1;
        if (scroll.flags & HSCROLLBAR_VISIBLE)
            widget_height -= kScrollBarWidth + 1;
        int32_t widget_content_height = std::max(scroll.contentHeight - widget_height, 0);

        switch (scroll_area)
        {
            case SCROLL_PART_HSCROLLBAR_LEFT:
                scroll.contentOffsetX = std::max(scroll.contentOffsetX - 3, 0);
                break;
            case SCROLL_PART_HSCROLLBAR_RIGHT:
                scroll.contentOffsetX = std::min(scroll.contentOffsetX + 3, widget_content_width);
                break;
            case SCROLL_PART_HSCROLLBAR_LEFT_TROUGH:
                scroll.contentOffsetX = std::max(scroll.contentOffsetX - widget_width, 0);
                break;
            case SCROLL_PART_HSCROLLBAR_RIGHT_TROUGH:
                scroll.contentOffsetX = std::min(scroll.contentOffsetX + widget_width, widget_content_width);
                break;
            case SCROLL_PART_VSCROLLBAR_TOP:
                scroll.contentOffsetY = std::max(scroll.contentOffsetY - 3, 0);
                break;
            case SCROLL_PART_VSCROLLBAR_BOTTOM:
                scroll.contentOffsetY = std::min(scroll.contentOffsetY + 3, widget_content_height);
                break;
            case SCROLL_PART_VSCROLLBAR_TOP_TROUGH:
                scroll.contentOffsetY = std::max(scroll.contentOffsetY - widget_height, 0);
                break;
            case SCROLL_PART_VSCROLLBAR_BOTTOM_TROUGH:
                scroll.contentOffsetY = std::min(scroll.contentOffsetY + widget_height, widget_content_height);
                break;
            default:
                break;
        }
        widgetScrollUpdateThumbs(w, widgetIndex);

        auto* windowMgr = GetWindowManager();
        windowMgr->InvalidateByNumber(w.classification, w.number);
    }

    static void InputScrollContinue(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords)
    {
        int32_t scroll_part, scroll_id;

        const auto& widget = w.widgets[widgetIndex];
        if (w.classification != gPressedWidget.windowClassification || w.number != gPressedWidget.windowNumber
            || widgetIndex != gPressedWidget.widgetIndex)
        {
            InvalidateScroll();
            return;
        }

        ScreenCoordsXY newScreenCoords;
        widgetScrollGetPart(w, &widget, screenCoords, newScreenCoords, &scroll_part, &scroll_id);

        if (_currentScrollArea == SCROLL_PART_HSCROLLBAR_THUMB)
        {
            int32_t originalTooltipCursorX = gTooltipCursor.x;
            gTooltipCursor.x = screenCoords.x;
            InputScrollPartUpdateHThumb(w, widgetIndex, screenCoords.x - originalTooltipCursorX, scroll_id);
            return;
        }

        if (_currentScrollArea == SCROLL_PART_VSCROLLBAR_THUMB)
        {
            int32_t originalTooltipCursorY = gTooltipCursor.y;
            gTooltipCursor.y = screenCoords.y;
            InputScrollPartUpdateVThumb(w, widgetIndex, screenCoords.y - originalTooltipCursorY, scroll_id);
            return;
        }

        if (scroll_part != _currentScrollArea)
        {
            InvalidateScroll();
            return;
        }

        switch (scroll_part)
        {
            case SCROLL_PART_VIEW:
                w.onScrollMouseDrag(scroll_id, newScreenCoords);
                break;
            case SCROLL_PART_HSCROLLBAR_LEFT:
                InputScrollPartUpdateHLeft(w, widgetIndex, scroll_id);
                break;
            case SCROLL_PART_HSCROLLBAR_RIGHT:
                InputScrollPartUpdateHRight(w, widgetIndex, scroll_id);
                break;
            case SCROLL_PART_VSCROLLBAR_TOP:
                InputScrollPartUpdateVTop(w, widgetIndex, scroll_id);
                break;
            case SCROLL_PART_VSCROLLBAR_BOTTOM:
                InputScrollPartUpdateVBottom(w, widgetIndex, scroll_id);
                break;
        }
    }

    static void InputScrollEnd()
    {
        _inputState = InputState::Reset;
        InvalidateScroll();
    }

    /**
     *
     *  rct2: 0x006E98F2
     */
    static void InputScrollPartUpdateHThumb(WindowBase& w, WidgetIndex widgetIndex, int32_t x, int32_t scroll_id)
    {
        const auto& widget = w.widgets[widgetIndex];
        auto& scroll = w.scrolls[scroll_id];

        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            int32_t newLeft;
            newLeft = scroll.contentWidth;
            newLeft *= x;
            x = widget.width() - 22;
            if (scroll.flags & VSCROLLBAR_VISIBLE)
                x -= kScrollBarWidth + 1;
            newLeft /= x;
            x = newLeft;
            scroll.flags |= HSCROLLBAR_THUMB_PRESSED;
            newLeft = scroll.contentOffsetX;
            newLeft += x;
            if (newLeft < 0)
                newLeft = 0;
            x = widget.width() - 2;
            if (scroll.flags & VSCROLLBAR_VISIBLE)
                x -= kScrollBarWidth + 1;
            x *= -1;
            x += scroll.contentWidth;
            if (x < 0)
                x = 0;
            if (newLeft > x)
                newLeft = x;
            scroll.contentOffsetX = newLeft;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

    /**
     *
     *  rct2: 0x006E99A9
     */
    static void InputScrollPartUpdateVThumb(WindowBase& w, WidgetIndex widgetIndex, int32_t y, int32_t scroll_id)
    {
        const auto& widget = w.widgets[widgetIndex];
        auto& scroll = w.scrolls[scroll_id];

        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            int32_t newTop;
            newTop = scroll.contentHeight;
            newTop *= y;
            y = widget.height() - 22;
            if (scroll.flags & HSCROLLBAR_VISIBLE)
                y -= kScrollBarWidth + 1;
            newTop /= y;
            y = newTop;
            scroll.flags |= VSCROLLBAR_THUMB_PRESSED;
            newTop = scroll.contentOffsetY;
            newTop += y;
            if (newTop < 0)
                newTop = 0;
            y = widget.height() - 2;
            if (scroll.flags & HSCROLLBAR_VISIBLE)
                y -= kScrollBarWidth + 1;
            y *= -1;
            y += scroll.contentHeight;
            if (y < 0)
                y = 0;
            if (newTop > y)
                newTop = y;
            scroll.contentOffsetY = newTop;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

    /**
     *
     *  rct2: 0x006E9A60
     */
    static void InputScrollPartUpdateHLeft(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            auto& scroll = w.scrolls[scroll_id];
            scroll.flags |= HSCROLLBAR_LEFT_PRESSED;
            if (scroll.contentOffsetX >= 3)
                scroll.contentOffsetX -= 3;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

    /**
     *
     *  rct2: 0x006E9ABF
     */
    static void InputScrollPartUpdateHRight(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id)
    {
        const auto& widget = w.widgets[widgetIndex];

        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            auto& scroll = w.scrolls[scroll_id];
            scroll.flags |= HSCROLLBAR_RIGHT_PRESSED;
            scroll.contentOffsetX += 3;
            int32_t newLeft = widget.width() - 2;
            if (scroll.flags & VSCROLLBAR_VISIBLE)
                newLeft -= kScrollBarWidth + 1;
            newLeft *= -1;
            newLeft += scroll.contentWidth;
            if (newLeft < 0)
                newLeft = 0;
            if (scroll.contentOffsetX > newLeft)
                scroll.contentOffsetX = newLeft;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

    /**
     *
     *  rct2: 0x006E9C37
     */
    static void InputScrollPartUpdateVTop(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            auto& scroll = w.scrolls[scroll_id];
            scroll.flags |= VSCROLLBAR_UP_PRESSED;
            if (scroll.contentOffsetY >= 3)
                scroll.contentOffsetY -= 3;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

    /**
     *
     *  rct2: 0x006E9C96
     */
    static void InputScrollPartUpdateVBottom(WindowBase& w, WidgetIndex widgetIndex, int32_t scroll_id)
    {
        const auto& widget = w.widgets[widgetIndex];

        auto* windowMgr = GetWindowManager();
        if (windowMgr->FindByNumber(w.classification, w.number) != nullptr)
        {
            auto& scroll = w.scrolls[scroll_id];
            scroll.flags |= VSCROLLBAR_DOWN_PRESSED;
            scroll.contentOffsetY += 3;
            int32_t newTop = widget.height() - 2;
            if (scroll.flags & HSCROLLBAR_VISIBLE)
                newTop -= kScrollBarWidth + 1;
            newTop *= -1;
            newTop += scroll.contentHeight;
            if (newTop < 0)
                newTop = 0;
            if (scroll.contentOffsetY > newTop)
                scroll.contentOffsetY = newTop;
            widgetScrollUpdateThumbs(w, widgetIndex);
            windowMgr->InvalidateWidgetByNumber(w.classification, w.number, widgetIndex);
        }
    }

#pragma endregion

#pragma region Widgets

    /**
     *
     *  rct2: 0x006E9253
     */
    static void InputWidgetOver(const ScreenCoordsXY& screenCoords, WindowBase* w, WidgetIndex widgetIndex)
    {
        WindowClass windowClass = WindowClass::null;
        WindowNumber windowNumber = 0;
        Widget* widget = nullptr;

        if (w != nullptr)
        {
            windowClass = w->classification;
            windowNumber = w->number;
            widget = widgetIndex == kWidgetIndexNull ? nullptr : &w->widgets[widgetIndex];
        }

        InputWidgetOverChangeCheck(windowClass, windowNumber, widgetIndex);

        if (w != nullptr && widgetIndex != kWidgetIndexNull && widget->type == WidgetType::scroll)
        {
            int32_t scroll_part, scrollId;
            ScreenCoordsXY newScreenCoords;
            widgetScrollGetPart(*w, widget, screenCoords, newScreenCoords, &scroll_part, &scrollId);

            if (scroll_part == SCROLL_PART_VIEW)
            {
                w->onScrollMouseOver(scrollId, newScreenCoords);
            }
        }

        InputUpdateTooltip(w, widgetIndex, screenCoords);
    }

    /**
     *
     *  rct2: 0x006E9269
     */
    static void InputWidgetOverChangeCheck(WindowClass windowClass, WindowNumber windowNumber, WidgetIndex widgetIndex)
    {
        // Prevents invalid widgets being clicked source of bug is elsewhere
        if (widgetIndex == kWidgetIndexNull)
            return;

        // Check if the widget that the cursor was over, has changed
        if (windowClass != gHoverWidget.windowClassification || windowNumber != gHoverWidget.windowNumber
            || widgetIndex != gHoverWidget.widgetIndex)
        {
            // Invalidate last widget cursor was on if widget is a flat button
            InputWidgetOverFlatbuttonInvalidate();

            // Set new cursor over widget
            gHoverWidget.windowClassification = windowClass;
            gHoverWidget.windowNumber = windowNumber;
            gHoverWidget.widgetIndex = widgetIndex;

            // Invalidate new widget cursor is on if widget is a flat button
            if (windowClass != WindowClass::null)
                InputWidgetOverFlatbuttonInvalidate();
        }
    }

    /**
     * Used to invalidate flat button widgets when the mouse leaves and enters them. This should be generalised so that all
     * widgets can use this in the future.
     */
    static void InputWidgetOverFlatbuttonInvalidate()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByNumber(gHoverWidget.windowClassification, gHoverWidget.windowNumber);
        if (w != nullptr)
        {
            w->onPrepareDraw();
            if (w->widgets[gHoverWidget.widgetIndex].type == WidgetType::flatBtn)
            {
                windowMgr->InvalidateWidgetByNumber(
                    gHoverWidget.windowClassification, gHoverWidget.windowNumber, gHoverWidget.widgetIndex);
            }
        }
    }

    /**
     *
     *  rct2: 0x006E95F9
     */
    static void InputWidgetLeft(const ScreenCoordsXY& screenCoords, WindowBase* w, WidgetIndex widgetIndex)
    {
        WindowClass windowClass = WindowClass::null;
        WindowNumber windowNumber = 0;

        if (w != nullptr)
        {
            windowClass = w->classification;
            windowNumber = w->number;
        }

        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::error);
        windowMgr->CloseByClass(WindowClass::tooltip);

        // Window might have changed position in the list, therefore find it again
        w = windowMgr->FindByNumber(windowClass, windowNumber);
        if (w == nullptr)
            return;

        w = windowMgr->BringToFront(*w);
        if (widgetIndex == kWidgetIndexNull)
            return;

        if (windowClass != GetCurrentTextBox().window.classification || windowNumber != GetCurrentTextBox().window.number
            || widgetIndex != GetCurrentTextBox().widgetIndex)
        {
            WindowCancelTextbox();
        }

        const auto& widget = w->widgets[widgetIndex];

        switch (widget.type)
        {
            case WidgetType::frame:
            case WidgetType::resize:
                // OPENRCT2MINI W6: don't enter resize mode while the window
                // is shaded — the resize handle's hot zone overlaps the
                // title bar in collapsed state, and minHeight==maxHeight
                // means a resize attempt would no-op anyway.
                if (w->canBeResized() && !w->isShaded
                    && (screenCoords.x >= w->windowPos.x + w->width - 19 && screenCoords.y >= w->windowPos.y + w->height - 19))
                    InputWindowResizeBegin(*w, widgetIndex, screenCoords);
                break;
            case WidgetType::viewport:
                _inputState = InputState::ViewportLeft;
                gInputDragLast = screenCoords;
                _dragWidget.windowClassification = windowClass;
                _dragWidget.windowNumber = windowNumber;
                if (gInputFlags.has(InputFlag::toolActive))
                {
                    w = windowMgr->FindByNumber(gCurrentToolWidget.windowClassification, gCurrentToolWidget.windowNumber);
                    if (w != nullptr)
                    {
                        gInputFlags.set(InputFlag::leftMousePressed);
                        w->onToolDown(gCurrentToolWidget.widgetIndex, screenCoords);
                    }
                }
                break;
            case WidgetType::caption:
                InputWindowPositionBegin(*w, widgetIndex, screenCoords);
                break;
            case WidgetType::scroll:
                InputScrollBegin(*w, widgetIndex, screenCoords);
                break;
            case WidgetType::empty:
            case WidgetType::labelCentred:
            case WidgetType::label:
            case WidgetType::groupbox:
            case WidgetType::progressBar:
            case WidgetType::placeholder:
            case WidgetType::horizontalSeparator:
                // Non-interactive widget type
                break;
            case WidgetType::imgBtn:
            case WidgetType::colourBtn:
            case WidgetType::trnBtn:
            case WidgetType::tab:
            case WidgetType::flatBtn:
            case WidgetType::hiddenButton:
            case WidgetType::button:
            case WidgetType::tableHeader:
            case WidgetType::spinner:
            case WidgetType::dropdownMenu:
            case WidgetType::closeBox:
            case WidgetType::shadeBox: // OPENRCT2MINI W6: same press-handling as closeBox
            case WidgetType::checkbox:
            case WidgetType::textBox:
            case WidgetType::custom:
                if (!widgetIsDisabled(*w, widgetIndex))
                {
                    // OPENRCT2MINI v2.17: widgets can opt out of the
                    // framework click sound via WidgetFlag::
                    // suppressClickSound (e.g. the Rumble Editor's
                    // Play button, where the click1 sample would
                    // mask the rumble preview audio).
                    if (!widget.flags.has(WidgetFlag::suppressClickSound))
                        OpenRCT2::Audio::Play(Audio::SoundId::click1, 0, w->windowPos.x + widget.midX());

                    // Set new cursor down widget
                    gPressedWidget.windowClassification = windowClass;
                    gPressedWidget.windowNumber = windowNumber;
                    gPressedWidget.widgetIndex = widgetIndex;
                    gInputFlags.set(InputFlag::widgetPressed);
                    _inputState = InputState::WidgetPressed;
                    _clickRepeatTicks = gCurrentRealTimeTicks;

                    windowMgr->InvalidateWidgetByNumber(windowClass, windowNumber, widgetIndex);
                    w->onMouseDown(widgetIndex);
                }
                break;
        }
    }

    /**
     *
     *  rct2: 0x006ED833
     */
    void ProcessMouseOver(const ScreenCoordsXY& screenCoords)
    {
        // OPENRCT2MINI cursor-selector-modal-plan: when the selector is
        // active (focus mode driving the UI, cursor hidden), suppress
        // hover-dispatch entirely. The virtual cursor's position is
        // still being polled — without this gate, every frame fires an
        // onScrollMouseOver on whatever widget happens to be under the
        // invisible cursor, which spawns tooltips, highlights list
        // rows, loads preview screenshots, etc. — visible state
        // changes driven by a cursor the user can't see. Focus mode
        // owns its own hover state via restoreFocusedListHover; real
        // mouse motion will transition the selector back to hidden
        // (via SelectorTransitionSource::realMouseMotion) and resume
        // hover-dispatch on the next frame.
        //
        // Also close any tooltip that was already showing so a
        // tooltip spawned by the last cursor-mode hover doesn't
        // linger over the focus ring.
        {
            auto& mgr = GetInputManager();
            if (mgr.getSelectorMode() == InputManager::SelectorMode::active)
            {
                auto* windowMgr = GetWindowManager();
                if (windowMgr != nullptr)
                    windowMgr->CloseByClass(WindowClass::tooltip);
                return;
            }
        }

        CursorID cursorId = CursorID::Arrow;
        auto ft = Formatter();
        ft.Add<StringId>(kStringIdNone);
        SetMapTooltip(ft);

        auto* windowMgr = GetWindowManager();
        WindowBase* window = windowMgr->FindFromPoint(screenCoords);

        if (window != nullptr)
        {
            WidgetIndex widgetId = windowMgr->FindWidgetFromPoint(*window, screenCoords);
            if (widgetId != kWidgetIndexNull)
            {
                switch (window->widgets[widgetId].type)
                {
                    case WidgetType::viewport:
                        if (!gInputFlags.has(InputFlag::toolActive))
                        {
                            if (ViewportInteractionLeftOver(screenCoords))
                            {
                                SetCursor(CursorID::HandPoint);
                                return;
                            }
                            break;
                        }
                        cursorId = static_cast<CursorID>(gCurrentToolId);
                        break;

                    case WidgetType::frame:
                    case WidgetType::resize:
                        if (!window->canBeResized())
                            break;

                        if (screenCoords.x < window->windowPos.x + window->width - 0x13)
                            break;

                        if (screenCoords.y < window->windowPos.y + window->height - 0x13)
                            break;

                        cursorId = CursorID::DiagonalArrows;
                        break;

                    case WidgetType::scroll:
                    {
                        int32_t output_scroll_area, scroll_id;
                        ScreenCoordsXY scrollCoords;
                        widgetScrollGetPart(
                            *window, &window->widgets[widgetId], screenCoords, scrollCoords, &output_scroll_area, &scroll_id);
                        if (output_scroll_area != SCROLL_PART_VIEW)
                        {
                            cursorId = CursorID::Arrow;
                            break;
                        }
                        // Same as default but with scroll_x/y
                        cursorId = window->onCursor(widgetId, scrollCoords, CursorID::Arrow);
                        if (cursorId == CursorID::Undefined)
                            cursorId = CursorID::Arrow;
                        break;
                    }
                    default:
                        cursorId = window->onCursor(widgetId, screenCoords, CursorID::Arrow);
                        if (cursorId == CursorID::Undefined)
                            cursorId = CursorID::Arrow;
                        break;
                }
            }
        }

        ViewportInteractionRightOver(screenCoords);
        SetCursor(cursorId);
    }

    /**
     *
     *  rct2: 0x006ED801
     */
    void ProcessMouseTool(const ScreenCoordsXY& screenCoords)
    {
        if (gInputFlags.has(InputFlag::toolActive))
        {
            auto* windowMgr = GetWindowManager();
            WindowBase* w = windowMgr->FindByNumber(gCurrentToolWidget.windowClassification, gCurrentToolWidget.windowNumber);

            if (w == nullptr)
                ToolCancel();
            else if (InputGetState() != InputState::ViewportRight)
            {
                // OPENRCT2MINI grid-cursor-plan §7: when the user is in
                // grid-cursor mode for the armed tool (cycle-window has
                // landed on the virtual tool-viewport entry, i.e.
                // `_toolFocusSelected == true`), suppress mouse-driven
                // tool updates. Otherwise every polled SDL_MOUSEMOTION
                // re-points onToolUpdate at the mouse's screen position
                // and writes gMapSelectPositionA/B + gMapSelectType,
                // clobbering the grid cursor's tile-aligned selection.
                //
                // Earlier this was keyed off `SelectorMode == active`,
                // but that flips to `hidden` the moment the user moves
                // the mouse (the cursor-wake transition). So once the
                // mouse moved at all, the gate stopped firing and the
                // grid cursor's selection got overwritten — including
                // after cycling back to the tool, when ToolContext::
                // onActivate's write was immediately re-clobbered by
                // residual mouse motion on the next frame.
                //
                // `_toolFocusSelected` is the correct key: it's the
                // latch resolveActiveContext consults to route tool-
                // context shortcuts over widgetFocus, and it persists
                // across cursor wake/sleep transitions until the user
                // explicitly cycles away from the tool viewport.
                auto& mgr = GetInputManager();
                if (mgr.isToolFocusSelected())
                    return;
                // OPENRCT2MINI grid-cursor-plan §12.1 (amendment
                // 2026-05-17 #7 — parked-cursor ghost suppression):
                // also skip onToolUpdate when the grid cursor is
                // in parked state (user is navigating the tool
                // window's widgets in focus mode while the tool
                // is armed). Without this gate, the tool's
                // provisional ghost would draw at the cursor's
                // synthesised mouse position, which is currently
                // parked at the grid cursor tile — visually
                // identical to grid-mode but with no user control.
                // The blinking parked marker is the right visual
                // affordance for "your cursor is here, engage to
                // act"; the ghost on top of it implies a
                // commitment the user hasn't made.
                if (gMapSelectFlags.has(MapSelectFlag::gridCursorParked))
                    return;
                w->onToolUpdate(gCurrentToolWidget.widgetIndex, screenCoords);
            }
        }
    }

    /**
     *
     *  rct2: 0x006E8DA7
     */
    void InputStateWidgetPressed(
        const ScreenCoordsXY& screenCoords, MouseState state, WidgetIndex widgetIndex, WindowBase* w, Widget* widget)
    {
        WindowClass cursor_w_class;
        WindowNumber cursor_w_number;
        cursor_w_class = gPressedWidget.windowClassification;
        cursor_w_number = gPressedWidget.windowNumber;
        WidgetIndex cursor_widgetIndex = gPressedWidget.widgetIndex;

        auto* windowMgr = GetWindowManager();
        WindowBase* cursor_w = windowMgr->FindByNumber(cursor_w_class, cursor_w_number);
        if (cursor_w == nullptr)
        {
            _inputState = InputState::Reset;
            return;
        }

        if (w != nullptr && state == MouseState::leftRelease)
        {
            if (w->widgets[widgetIndex].type == WidgetType::closeBox && cursor_w_class == w->classification
                && cursor_w_number == w->number && widgetIndex == cursor_widgetIndex)
            {
                auto& im = GetInputManager();
                if (im.isModifierKeyPressed(ModifierKey::shift))
                {
                    gLastCloseModifier.window.number = w->number;
                    gLastCloseModifier.window.classification = w->classification;
                    gLastCloseModifier.modifier = CloseWindowModifier::shift;
                }
                else if (im.isModifierKeyPressed(ModifierKey::ctrl))
                {
                    gLastCloseModifier.window.number = w->number;
                    gLastCloseModifier.window.classification = w->classification;
                    gLastCloseModifier.modifier = CloseWindowModifier::control;
                }
            }
        }

        switch (state)
        {
            case MouseState::released:
                if (w == nullptr || cursor_w_class != w->classification || cursor_w_number != w->number
                    || widgetIndex != cursor_widgetIndex)
                    break;

                if (widgetIsDisabled(*w, widgetIndex))
                    break;

                // If this variable is non-zero then its the last tick the mouse down event was fired.
                if (_clickRepeatTicks.has_value())
                {
                    // The initial amount of time in ticks to wait until the first click repeat.
                    constexpr auto kTicksUntilRepeats = 16u;

                    // The amount of ticks between each click repeat.
                    constexpr auto kEventDelayInTicks = 3u;

                    // The amount of ticks since the last click repeat.
                    const auto clickRepeatsDelta = gCurrentRealTimeTicks - _clickRepeatTicks.value();

                    // Handle click repeat, only start this when at least 16 ticks elapsed.
                    if (clickRepeatsDelta >= kTicksUntilRepeats && (clickRepeatsDelta & kEventDelayInTicks) == 0)
                    {
                        if (widgetIsHoldable(*w, widgetIndex))
                        {
                            w->onMouseDown(widgetIndex);
                        }

                        // Subtract initial delay from here on we want the event each third tick.
                        _clickRepeatTicks = gCurrentRealTimeTicks - kTicksUntilRepeats;
                    }
                }

                if (gInputFlags.has(InputFlag::widgetPressed))
                {
                    if (_inputState == InputState::DropdownActive)
                    {
                        gDropdown.highlightedIndex = gDropdown.defaultIndex;
                        windowMgr->InvalidateByClass(WindowClass::dropdown);
                    }
                    return;
                }

                gInputFlags.set(InputFlag::widgetPressed);
                windowMgr->InvalidateWidgetByNumber(cursor_w_class, cursor_w_number, widgetIndex);
                return;
            case MouseState::leftRelease:
            case MouseState::rightPress:
                if (_inputState == InputState::DropdownActive)
                {
                    if (w != nullptr)
                    {
                        auto wClass = w->classification;
                        auto wNumber = w->number;
                        int32_t dropdown_index = 0;
                        bool dropdownCleanup = false;

                        if (w->classification == WindowClass::dropdown)
                        {
                            dropdown_index = DropdownIndexFromPoint(screenCoords, w);
                            dropdownCleanup = dropdown_index == -1
                                || (dropdown_index < Dropdown::kItemsMaxSize && gDropdown.items[dropdown_index].isDisabled())
                                || gDropdown.items[dropdown_index].isSeparator();
                            w = nullptr; // To be closed right next
                        }
                        else
                        {
                            // OPENRCT2MINI focus-mode-plan §F.13: honour
                            // InputFlag::dropdownStayOpen regardless of
                            // whether the physical cursor still sits on
                            // the press widget. Upstream only checked
                            // stayOpen inside the cursor-matches branch,
                            // which works for real mouse clicks (cursor
                            // is naturally on the chevron when you let
                            // go) but breaks focus-mode synthesised
                            // clicks — the virtual cursor is wherever
                            // the user last left it, so the release
                            // falls into dropdownCleanup and the popup
                            // closes the instant the activating button
                            // is released. Hoisting the stayOpen latch
                            // above the cursor-match check makes the
                            // first release after any stayOpen dropdown
                            // a no-op (sets dropdownMouseUp, returns)
                            // regardless of cursor position. Real mouse
                            // workflows still work: a press-drag-release
                            // into a dropdown ITEM lands in the
                            // dropdown-window branch above, which
                            // processes selection and closes; the
                            // stayOpen swallow only fires when the
                            // release is on a non-dropdown widget.
                            if (gInputFlags.has(InputFlag::dropdownStayOpen)
                                && !gInputFlags.has(InputFlag::dropdownMouseUp))
                            {
                                gInputFlags.set(InputFlag::dropdownMouseUp);
                                return;
                            }
                            if (cursor_w_class != w->classification || cursor_w_number != w->number
                                || widgetIndex != cursor_widgetIndex)
                            {
                                dropdownCleanup = true;
                            }
                            else
                            {
                                dropdown_index = -1;
                            }
                        }

                        windowMgr->CloseByClass(WindowClass::dropdown);

                        if (dropdownCleanup)
                        {
                            // Update w as it will be invalid after closing the dropdown window
                            w = windowMgr->FindByNumber(wClass, wNumber);
                        }
                        else
                        {
                            cursor_w = windowMgr->FindByNumber(cursor_w_class, cursor_w_number);
                            if (gInputFlags.has(InputFlag::widgetPressed))
                            {
                                gInputFlags.unset(InputFlag::widgetPressed);
                                windowMgr->InvalidateWidgetByNumber(cursor_w_class, cursor_w_number, cursor_widgetIndex);
                            }

                            _inputState = InputState::Normal;
                            gTooltipCloseTimeout = 0;
                            gTooltipWidget.widgetIndex = cursor_widgetIndex;
                            gTooltipWidget.windowClassification = cursor_w_class;
                            gTooltipWidget.windowNumber = cursor_w_number;

                            if (dropdown_index == -1)
                            {
                                if (gDropdown.defaultIndex != -1 && !gDropdown.items[gDropdown.defaultIndex].isDisabled())
                                {
                                    dropdown_index = gDropdown.defaultIndex;
                                }
                            }
                            cursor_w->onDropdown(cursor_widgetIndex, dropdown_index);
                        }
                    }
                }

                _inputState = InputState::Normal;

                if (state == MouseState::rightPress)
                {
                    return;
                }

                gTooltipCloseTimeout = 0;
                gTooltipWidget.widgetIndex = cursor_widgetIndex;

                if (w == nullptr)
                    break;

                if (widget == nullptr)
                    break;

                {
                    // OPENRCT2MINI v2.17: opt-out via
                    // WidgetFlag::suppressClickSound (release-side
                    // click2; same rationale as the press-side
                    // click1 above).
                    if (!widget->flags.has(WidgetFlag::suppressClickSound))
                    {
                        int32_t mid_point_x = widget->midX() + w->windowPos.x;
                        OpenRCT2::Audio::Play(Audio::SoundId::click2, 0, mid_point_x);
                    }
                }
                if (cursor_w_class != w->classification || cursor_w_number != w->number || widgetIndex != cursor_widgetIndex)
                    break;

                if (widgetIsDisabled(*w, widgetIndex))
                    break;

                windowMgr->InvalidateWidgetByNumber(cursor_w_class, cursor_w_number, widgetIndex);
                // OPENRCT2MINI cursor-selector-modal-plan v2: we used
                // to pump realMouseClick here, but ProcessWorldCursor
                // synthesises identical MouseInput events for
                // virtual cursor clicks (PAD A / D-pad). That
                // would clobber the virtualUserInput flag the
                // cursor.click shortcut just set, breaking the
                // new-window auto-wake. The realMouseMotion
                // transition (gated to non-zero delta) is the only
                // reliable "real mouse" signal we need — real-
                // mouse users move the cursor before clicking,
                // virtual users don't.
                // OPENRCT2MINI W6: shade button is framework-level — every
                // window with the standard caption+closeBox prefix gets one
                // appended by resizeFrame(). The click toggles isShaded
                // directly here without dispatching to per-window onMouseUp,
                // so no per-window handler needs to know about shadeBox.
                if (widget->type == WidgetType::shadeBox)
                {
                    w->toggleShade();
                }
                else
                {
                    w->onMouseUp(widgetIndex);
                }
                return;

            default:
                return;
        }

        _clickRepeatTicks = std::nullopt;
        if (_inputState != InputState::DropdownActive)
        {
            // Hold down widget and drag outside of area??
            if (gInputFlags.has(InputFlag::widgetPressed))
            {
                gInputFlags.unset(InputFlag::widgetPressed);
                windowMgr->InvalidateWidgetByNumber(cursor_w_class, cursor_w_number, cursor_widgetIndex);
            }
            return;
        }
        else if (gDropdown.hasTooltips)
        {
            // This is ordinarily covered in InputWidgetOver but the dropdown with colours is a special case.
            InputUpdateTooltip(w, widgetIndex, screenCoords);
        }

        // OPENRCT2MINI focus-mode-plan §F.14: only the cursor source
        // drives the per-frame highlight reset + re-derive. When the
        // dropdown is in `focus` mode (WindowDropdownMoveHighlight set
        // it), leave gDropdown.highlightedIndex alone — D-pad nav
        // owns it until SDL_MOUSEMOTION flips back to `cursor`. The
        // first real mouse motion event sets navigationSource = cursor
        // and the next ProcessMouseOver tick resumes the hover-driven
        // behaviour, exactly matching upstream.
        if (gDropdown.navigationSource != Dropdown::NavigationSource::cursor)
        {
            return;
        }
        gDropdown.highlightedIndex = -1;
        windowMgr->InvalidateByClass(WindowClass::dropdown);
        if (w == nullptr)
        {
            return;
        }

        if (w->classification == WindowClass::dropdown)
        {
            int32_t dropdown_index = DropdownIndexFromPoint(screenCoords, w);
            if (dropdown_index == -1)
            {
                if (gDropdown.hasTooltips && gDropdown.lastTooltipHover != -1)
                {
                    gDropdown.lastTooltipHover = -1;
                    WindowTooltipClose();
                }
                return;
            }

            if (gDropdown.hasTooltips && gDropdown.lastTooltipHover != dropdown_index)
            {
                gDropdown.lastTooltipHover = dropdown_index;
                WindowTooltipClose();

                WindowTooltipShow(StringWithArgs{ gDropdown.items[dropdown_index].tooltip, {} }, screenCoords);
            }

            if (dropdown_index < Dropdown::kItemsMaxSize && gDropdown.items[dropdown_index].isDisabled())
            {
                return;
            }

            if (gDropdown.items[dropdown_index].isSeparator())
            {
                return;
            }

            gDropdown.highlightedIndex = dropdown_index;
            windowMgr->InvalidateByClass(WindowClass::dropdown);
        }
        else
        {
            gDropdown.lastTooltipHover = -1;
            WindowTooltipClose();
        }
    }

    static void InputUpdateTooltip(WindowBase* w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords)
    {
        if (gTooltipWidget.windowClassification == WindowClass::null)
        {
            if (gTooltipCursor == screenCoords)
            {
                if (gCurrentRealTimeTicks >= _tooltipNotShownTimeout && w != nullptr && widgetIndex != kWidgetIndexNull
                    && widgetIsVisible(*w, widgetIndex))
                {
                    gTooltipCloseTimeout = gCurrentRealTimeTicks + 8000;
                    WindowTooltipOpen(w, widgetIndex, screenCoords);
                }
            }
            else
            {
                ResetTooltipNotShown();
            }

            gTooltipCloseTimeout = gCurrentRealTimeTicks + 8000;
            gTooltipCursor = screenCoords;
        }
        else
        {
            gTooltipCursor = screenCoords;
            ResetTooltipNotShown();

            if (w == nullptr || gTooltipWidget.windowClassification != w->classification
                || gTooltipWidget.windowNumber != w->number || gTooltipWidget.widgetIndex != widgetIndex
                || !widgetIsVisible(*w, widgetIndex))
            {
                WindowTooltipClose();
            }

            if (gCurrentRealTimeTicks >= gTooltipCloseTimeout)
            {
                auto* windowMgr = GetWindowManager();
                windowMgr->CloseByClass(WindowClass::tooltip);
            }
        }
    }

#pragma endregion

    /**
     *
     *  rct2: 0x006ED990
     */
    void SetCursor(CursorID cursor_id)
    {
        assert(cursor_id != CursorID::Undefined);
        if (_inputState == InputState::Resizing)
        {
            cursor_id = CursorID::DiagonalArrows;
        }
        // OPENRCT2MINI: during drag operations show the 4-direction move
        // arrow for a clear "drag in any direction" affordance. Covers
        // ScrollRight (right-click drag-scroll) and ViewportRight
        // (right-click camera-pan), where the cursor is pinned at
        // gInputDragLast and motion translates into list / world
        // movement. Also covers PositioningWindow — which is entered
        // from BOTH the title-bar left-click drag and the right-click
        // anywhere-on-chrome drag — so the user gets the same affordance
        // either way and the drag-window action looks the same regardless
        // of where on the window they grabbed it.
        else if (_inputState == InputState::ScrollRight || _inputState == InputState::ViewportRight
                 || _inputState == InputState::PositioningWindow)
        {
            cursor_id = CursorID::UpDownLeftRightArrow;
        }
        ContextSetCurrentCursor(cursor_id);
    }

    /**
     *
     *  rct2: 0x006E876D
     */
    void InvalidateScroll()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByNumber(gPressedWidget.windowClassification, gPressedWidget.windowNumber);
        if (w != nullptr)
        {
            // Reset to basic scroll
            w->scrolls[_currentScrollIndex].flags &= 0xFF11;
            windowMgr->InvalidateByNumber(gPressedWidget.windowClassification, gPressedWidget.windowNumber);
        }
    }

    /**
     * rct2: 0x00406C96
     */
    void StoreMouseInput(MouseState state, const ScreenCoordsXY& screenCoords)
    {
        uint32_t writeIndex = _mouseInputQueueWriteIndex;
        uint32_t nextWriteIndex = (writeIndex + 1) % std::size(_mouseInputQueue);

        // Check if the queue is full
        if (nextWriteIndex != _mouseInputQueueReadIndex)
        {
            RCTMouseData* item = &_mouseInputQueue[writeIndex];
            item->x = screenCoords.x;
            item->y = screenCoords.y;
            item->state = state;

            _mouseInputQueueWriteIndex = nextWriteIndex;
        }
    }

    void GameHandleEdgeScroll()
    {
        WindowBase* mainWindow;
        int32_t scrollX, scrollY;

        mainWindow = WindowGetMain();
        if (mainWindow == nullptr)
            return;
        if (mainWindow->flags.has(WindowFlag::noScrolling)
            || (gLegacyScene == LegacyScene::trackDesignsManager || gLegacyScene == LegacyScene::titleSequence))
            return;
        if (mainWindow->viewport == nullptr)
            return;
        if (!ContextHasFocus())
            return;

        scrollX = 0;
        scrollY = 0;

        // Scroll left / right
        const CursorState* cursorState = ContextGetCursorState();
        if (cursorState->position.x == 0)
            scrollX = -1;
        else if (cursorState->position.x >= ContextGetWidth() - 1)
            scrollX = 1;

        // Scroll up / down
        if (cursorState->position.y == 0)
            scrollY = -1;
        else if (cursorState->position.y >= ContextGetHeight() - 1)
            scrollY = 1;

        InputScrollViewport(ScreenCoordsXY(scrollX, scrollY));
    }

    void InputScrollViewport(const ScreenCoordsXY& scrollScreenCoords)
    {
        WindowBase* mainWindow = WindowGetMain();
        if (mainWindow == nullptr)
            return;

        Viewport* viewport = mainWindow->viewport;
        if (viewport == nullptr)
            return;

        const int32_t speed = Config::Get().general.edgeScrollingSpeed;

        int32_t multiplier = viewport->zoom.ApplyTo(speed);
        int32_t dx = scrollScreenCoords.x * multiplier;
        int32_t dy = scrollScreenCoords.y * multiplier;

        if (scrollScreenCoords.x != 0)
        {
            // Speed up scrolling horizontally when at the edge of the map
            // so that the speed is consistent with vertical edge scrolling.
            int32_t x = mainWindow->savedViewPos.x + viewport->ViewWidth() / 2 + dx;
            int32_t y = mainWindow->savedViewPos.y + viewport->ViewHeight() / 2;
            int32_t y_dy = mainWindow->savedViewPos.y + viewport->ViewHeight() / 2 + dy;

            auto mapCoord = ViewportPosToMapPos({ x, y }, 0, viewport->rotation);
            auto mapCoord_dy = ViewportPosToMapPos({ x, y_dy }, 0, viewport->rotation);

            // Check if we're crossing the boundary
            // Clamp to the map minimum value
            int32_t at_map_edge = 0;
            int32_t at_map_edge_dy = 0;
            if (mapCoord.x < kMapMinimumXY || mapCoord.y < kMapMinimumXY)
            {
                at_map_edge = 1;
            }
            if (mapCoord_dy.x < kMapMinimumXY || mapCoord_dy.y < kMapMinimumXY)
            {
                at_map_edge_dy = 1;
            }

            // Clamp to the map maximum value (scenario specific)
            auto mapSizeMinus2 = GetMapSizeMinus2();
            if (mapCoord.x > mapSizeMinus2.x || mapCoord.y > mapSizeMinus2.y)
            {
                at_map_edge = 1;
            }
            if (mapCoord_dy.x > mapSizeMinus2.x || mapCoord_dy.y > mapSizeMinus2.y)
            {
                at_map_edge_dy = 1;
            }

            // If we crossed the boundary, multiply the distance by 2
            if (at_map_edge && at_map_edge_dy)
            {
                dx *= 2;
            }

            mainWindow->savedViewPos.x += dx;
            gInputFlags.set(InputFlag::viewportScrolling);
        }
        if (scrollScreenCoords.y != 0)
        {
            mainWindow->savedViewPos.y += dy;
            gInputFlags.set(InputFlag::viewportScrolling);
        }
    }

    void InputScrollViewportSmooth(const ScreenCoordsXY& scrollScreenCoords, WindowBase* targetWindow)
    {
        if (targetWindow == nullptr)
        {
            return;
        }

        Viewport* viewport = targetWindow->viewport;
        if (viewport == nullptr)
        {
            return;
        }

        if (targetWindow->flags.has(WindowFlag::noScrolling))
        {
            return;
        }

        if (scrollScreenCoords.x == 0 && scrollScreenCoords.y == 0)
            return;

        // Apply smooth scrolling similar to mouse drag behavior
        // Use zoom-based scaling like mouse dragging does
        ScreenCoordsXY differentialCoords = scrollScreenCoords;

        // Apply zoom scaling (same logic as mouse drag)
        const bool posX = differentialCoords.x > 0;
        const bool posY = differentialCoords.y > 0;
        differentialCoords.x = (viewport->zoom + 1).ApplyTo(-std::abs(differentialCoords.x));
        differentialCoords.y = (viewport->zoom + 1).ApplyTo(-std::abs(differentialCoords.y));
        differentialCoords.x = posX ? -differentialCoords.x : differentialCoords.x;
        differentialCoords.y = posY ? -differentialCoords.y : differentialCoords.y;

        // Apply the movement (note: we don't invert for gamepad like mouse drag does)
        targetWindow->savedViewPos += differentialCoords;

        gInputFlags.set(InputFlag::viewportScrolling);
    }
} // namespace OpenRCT2
