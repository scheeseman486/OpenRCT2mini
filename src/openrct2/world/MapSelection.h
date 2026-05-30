/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../core/FlagHolder.hpp"
#include "Location.hpp"

#include <vector>

enum class MapSelectFlag : uint8_t
{
    enable,
    enableConstruct,
    enableArrow,
    green,
    // OPENRCT2MINI grid-cursor-plan §7.2 (Option A): when ALSO set
    // alongside `enable`, the existing tile-marker paint hooks in
    // Paint.Surface.cpp:1093-1190 add a blinking overlay so the
    // gamepad-driven grid cursor is visually distinct from the
    // mouse-driven tool ghost. The blink is paint-time only — the
    // flag carries no extra state and clears on tool exit.
    gridCursor,
    // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #7
    // — user feedback): when set, the grid-cursor marker is in
    // "parked" state — the user is navigating the tool window's
    // widgets with focus mode, not driving the cursor on the map.
    // The Paint.Surface.cpp hook blinks the marker on a 500 ms
    // cycle so the user can see WHERE the cursor will be when
    // they engage grid-cursor mode, distinct from the solid
    // marker shown while they're actively driving it.
    gridCursorParked,
};
using MapSelectFlags = FlagHolder<uint8_t, MapSelectFlag>;

enum class MapSelectType : uint8_t
{
    corner0,
    corner1,
    corner2,
    corner3,
    full,
    fullWater,
    fullLandRights,
    quarter0,
    quarter1,
    quarter2,
    quarter3,
    edge0,
    edge1,
    edge2,
    edge3,
};
constexpr MapSelectType getMapSelectCorner(Direction direction)
{
    return static_cast<MapSelectType>(EnumValue(MapSelectType::corner0) + direction);
}
constexpr MapSelectType getMapSelectQuarter(Direction direction)
{
    return static_cast<MapSelectType>(EnumValue(MapSelectType::quarter0) + direction);
}
constexpr MapSelectType getMapSelectEdge(Direction direction)
{
    return static_cast<MapSelectType>(EnumValue(MapSelectType::edge0) + direction);
}

extern MapSelectFlags gMapSelectFlags;
extern MapSelectType gMapSelectType;
extern CoordsXY gMapSelectPositionA;
extern CoordsXY gMapSelectPositionB;
extern CoordsXYZ gMapSelectArrowPosition;
extern uint8_t gMapSelectArrowDirection;
// OPENRCT2MINI grid-cursor Z-follow (2026-05-31): when set to a value
// other than `kGridCursorZUseTerrain`, the gridCursor paint hook in
// Paint.Surface.cpp draws the highlight as a free-standing paint struct
// at this absolute world Z instead of attaching it to the surface tile
// (terrain Z). Used by ride / footpath-bridge construction so the grid
// cursor visual + bump-scroll anchor follow the construction head's
// elevation rather than getting stuck at ground level. ToolContext::
// syncGridCursorToHead is the canonical writer; the bottom-of-frame
// reset in ToolContext::processFrame restores the terrain default when
// the head-Z source is no longer providing one.
constexpr int32_t kGridCursorZUseTerrain = INT32_MIN;
extern int32_t gMapSelectGridCursorZ;

MapRange getMapSelectRange();
void setMapSelectRange(const MapRange& range);
void setMapSelectRange(CoordsXY coords);

namespace OpenRCT2::MapSelection
{
    void clearSelectedTiles();
    void addSelectedTile(const CoordsXY& coords);
    const std::vector<CoordsXY>& getSelectedTiles();

    void invalidate();
} // namespace OpenRCT2::MapSelection
