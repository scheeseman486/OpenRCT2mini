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

    // OPENRCT2MINI mouse-input refactor: viewport (camera) drag entry
    // points exposed for the kInterfaceCameraDrag shortcut poll. The
    // poll lives in UiContext::ProcessWorldCursor and drives begin /
    // end on rising / falling edge of the bound shortcut. Continue is
    // still routed through the existing state machine in MouseInput
    // (motion events while _inputState == ViewportRight). Returns
    // true from CameraDragWasShortPress only if the press was held
    // for less than ~500ms — used to fire the context-sensitive
    // right-click action at end-of-drag (delete tile element / etc.).
    void InputViewportDragBeginAtCursor();
    void InputViewportDragEndCurrent();
    bool CameraDragWasShortPress();
    bool CameraDragInProgress();
} // namespace OpenRCT2
