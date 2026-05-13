/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2/interface/Window.h>

namespace OpenRCT2
{
    enum class MouseState : uint32_t
    {
        released,
        leftPress,
        leftRelease,
        rightPress,
        rightRelease,
    };

    extern ScreenCoordsXY gInputDragLast;

    void InputWindowPositionBegin(WindowBase& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
    void GameHandleInput();
    void GameHandleEdgeScroll();

    void StoreMouseInput(MouseState state, const ScreenCoordsXY& screenCoords);

    void InputScrollViewport(const ScreenCoordsXY& screenCoords);
    void InputScrollViewportSmooth(const ScreenCoordsXY& screenCoords);
    void InputScrollViewportSmooth(const ScreenCoordsXY& screenCoords, WindowBase* targetWindow);

    // OPENRCT2MINI cursor-cancel-drag split / mouse-input refactor:
    // context-sensitive drag entry points exposed for the
    // kInterfaceCameraDrag shortcut poll. The poll lives in
    // UiContext::ProcessWorldCursor and drives begin / end on rising /
    // falling edge of the bound shortcut. cursor.cancel no longer
    // drives any drag — kInterfaceCameraDrag owns all three drag
    // gestures (camera-pan / scroll-drag / window-drag), dispatching
    // by cursor location at begin time.
    //
    // CameraDragWasShortPress returns true only if the camera-pan
    // variant of the drag was held for less than ~500ms — used to fire
    // ViewportInteractionRightClick (delete tile element, etc.) at
    // end-of-drag. The tap-action fallback applies only to the camera
    // branch; scroll and window-drag have no equivalent.
    //
    // Motion-continue is still routed through the existing state
    // machine in MouseInput (the per-frame "no events queued" released
    // case in each drag state) since it's identical whether the drag
    // was initiated by mouse, gamepad or keyboard.
    void InputContextDragBeginAtCursor();
    void InputContextDragEndCurrent();
    bool CameraDragWasShortPress();
    bool CameraDragInProgress();
} // namespace OpenRCT2
