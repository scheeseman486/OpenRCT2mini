/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once
#include <openrct2/world/Location.hpp>

namespace OpenRCT2::Ui
{
    bool ViewportInteractionLeftOver(const ScreenCoordsXY& screenCoords);
    bool ViewportInteractionLeftClick(const ScreenCoordsXY& screenCoords);
    bool ViewportInteractionRightOver(const ScreenCoordsXY& screenCoords);
    bool ViewportInteractionRightClick(const ScreenCoordsXY& screenCoords);

    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.2: tile-coord
    // entry point for the right-click tile-action. Reprojects the
    // tile's centre to the main viewport's screen space, then
    // dispatches through the same path the mouse-screen-coord
    // entry uses. Used by the grid-cursor `onCancel` so the
    // gamepad / keyboard path performs the same delete-/-context-
    // open semantic the mouse RMB short-press release does, but
    // sourced from the cursor's tile instead of the OS pointer.
    bool ViewportInteractionRightClickAtMapPos(const CoordsXY& mapCoords);

    CoordsXY ViewportInteractionGetTileStartAtCursor(const ScreenCoordsXY& screenCoords);
} // namespace OpenRCT2::Ui
