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

#include <optional>

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
    //
    // OPENRCT2MINI grid-cursor-deletion-plan §3.6 (2026-05-20):
    // superseded by ViewportInteractionRightClickAtGridCursor for
    // the tool-context onCancel path. Left in place as a parking
    // spot for future map-pos right-click flows; no current
    // consumers.
    bool ViewportInteractionRightClickAtMapPos(const CoordsXY& mapCoords);

    // OPENRCT2MINI grid-cursor-deletion-plan §3.2 (2026-05-20):
    // grid-cursor-specific right-click dispatcher. Walks the tile's
    // linked-list of TileElements (ascending Z, then placement
    // order at the same Z — guaranteed by TileElementInsert),
    // classifies each into one of nine priority bands per the
    // fork's deletion ladder (sub-tile decoration → full-tile
    // decoration → large scenery → wall → banner → path addition →
    // path → park entrance → ride/track), and applies the matching
    // action to the highest-priority candidate. Successive presses
    // peel the tile back one element at a time. Diverges from the
    // mouse-screen-coord picker so the gamepad / keyboard user can
    // address every sub-tile element deterministically, not
    // whatever happens to lie under the projected tile-centre
    // pixel. The `cursorZ` argument is the cursor's effective world
    // Z (surface Z + raised-plane offset). When the user has lifted
    // the cursor off the ground (gridCursor().getZ() != 0), a Z-
    // window filter clips candidates to ±kPathHeightStep of the
    // cursor's Z so only elements near the targeted plane are
    // considered.
    bool ViewportInteractionRightClickAtGridCursor(const TileCoordsXY& tile, int32_t cursorZ);

    // OPENRCT2MINI cursor-sync (2026-05-17): project a world tile-
    // centre onto the main viewport's screen pixel coords. Returns
    // an empty optional if the main viewport doesn't exist yet
    // (title scene, pre-init). Used by the per-frame pixel-cursor
    // sync in UiContext::ProcessWorldCursor — when the grid cursor
    // owns input and the pixel cursor is hidden, the hidden cursor's
    // underlying position is parked at the grid cursor's screen
    // tile, so a switch back to mouse input wakes the pointer
    // exactly where the user was working.
    std::optional<ScreenCoordsXY> ViewportInteractionMapToScreen(const CoordsXY& mapCoords);
    // OPENRCT2MINI grid-cursor-plan §14.2 polish 2 (2026-05-20): Z-aware
    // overload — adds zOffset (world units, kPathHeightStep multiples)
    // on top of the resolved surface Z before projecting. Used by the
    // grid cursor's parking sync (SyncHiddenCursorParking) so the
    // software cursor sprite rides up the iso projection alongside
    // the raised placement Z plane instead of staying tethered to the
    // ground.
    std::optional<ScreenCoordsXY> ViewportInteractionMapToScreen(const CoordsXY& mapCoords, int32_t zOffset);

    CoordsXY ViewportInteractionGetTileStartAtCursor(const ScreenCoordsXY& screenCoords);
} // namespace OpenRCT2::Ui
