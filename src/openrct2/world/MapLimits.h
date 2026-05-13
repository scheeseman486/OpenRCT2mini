/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <cstdint>

constexpr int32_t kCoordsXYStep = 32;
constexpr int32_t kCoordsXYHalfTile = (kCoordsXYStep / 2);
constexpr int32_t kCoordsZStep = 8;
constexpr int32_t kCoordsZPerTinyZ = 16;

constexpr uint8_t kMinimumMapSizeTechnical = 5;
// OPENRCT2MINI host-restoration-plan §1a: 257 on Mini (cut L1 — keeps the
// TilePointerIndex small for the 128 MB device), 1001 on host (upstream
// OpenRCT2 v0.5.0 value, allows community parks up to 999×999). Runtime
// override Config::General::mapSizeOverride takes precedence when non-zero;
// it's read by ParkFile's saved-size check and the editor's resize UI.
#ifdef OPENRCT2MINI
constexpr uint16_t kMaximumMapSizeTechnical = 257;
#else
constexpr uint16_t kMaximumMapSizeTechnical = 1001;
#endif
constexpr int16_t kMinimumMapSizePractical = (kMinimumMapSizeTechnical - 2);
constexpr int16_t kMaximumMapSizePractical = (kMaximumMapSizeTechnical - 2);
constexpr int32_t kMaximumMapSizeBig = kCoordsXYStep * kMaximumMapSizeTechnical;

constexpr uint8_t kMinimumLandHeight = 2;
constexpr uint8_t kMaximumLandHeight = 254;
constexpr uint8_t kMinimumWaterHeight = 2;
constexpr uint8_t kMaximumWaterHeight = 254;

constexpr int16_t kMapMinimumXY = (-kMaximumMapSizeTechnical);
constexpr int32_t kMaximumTileStartXY = kMaximumMapSizeBig - kCoordsXYStep;
constexpr int32_t kLandHeightStep = 2 * kCoordsZStep;
constexpr int32_t kWaterHeightStep = 2 * kCoordsZStep;
constexpr int32_t kMinimumLandZ = kMinimumLandHeight * kCoordsZStep;

// The land height that counts as 0 metres/feet for the land height labels and altitude graphs.
constexpr uint8_t kMapBaseZ = 7;

// How high construction has to be off the ground when the player owns construction rights, in tile coords.
constexpr uint8_t kConstructionRightsClearanceSmall = 3;
// Same as previous, but in big coords.
constexpr uint8_t kConstructionRightsClearanceBig = 3 * kCoordsZStep;

constexpr uint32_t kMaxTileElementsWithSpaceRoom = 0x1000000;
constexpr uint32_t kMaxTileElements = kMaxTileElementsWithSpaceRoom - 512;
