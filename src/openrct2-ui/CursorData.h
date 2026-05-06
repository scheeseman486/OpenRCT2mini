/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2/interface/Cursors.h>

namespace OpenRCT2::Ui
{
    constexpr size_t kCursorBitWidth = 32;
    constexpr size_t kCursorHeight = 4;
    constexpr size_t kRawCursorSize = kCursorBitWidth * kCursorBitWidth;
    constexpr size_t kEncodedCursorSize = kRawCursorSize / 8;

    struct CursorData
    {
        struct HotSpot
        {
            int16_t X;
            int16_t Y;
        } HotSpot;
        uint8_t Data[kEncodedCursorSize];
        uint8_t Mask[kEncodedCursorSize];
    };

    // OPENRCT2MINI cut 52: paletted-cursor variant.
    //
    // The X-style CursorData above is strictly 2-colour (transparent + black +
    // white via Mask|Data) and quantises any soft-shaded cursor sprite to a
    // hard outline. For cursors we want to keep at full palette depth (e.g.
    // the user's hand-drawn pointer.png with a greyscale ramp), encode a
    // raw 32×32 palette-index array directly. 0xFE is the transparent
    // sentinel — palette index 254 (a barely-used reserved slot in the RCT2
    // master palette) is the cleanest "this can't possibly be a real cursor
    // colour" value. Any pixel == kPalettedCursorTransparent is skipped at
    // composite time; any other byte is written to _bits unchanged.
    constexpr uint8_t kPalettedCursorTransparent = 0xFE;

    struct PalettedCursorData
    {
        struct HotSpot
        {
            int16_t X;
            int16_t Y;
        } HotSpot;
        uint8_t Pixels[kRawCursorSize]; // row-major, 32×32, 0xFE = transparent
    };

    const CursorData* getCursorData(CursorID cursorId);
    // Returns nullptr if the cursor doesn't have a paletted variant.
    // SoftwareCursor checks this FIRST and falls back to getCursorData.
    const PalettedCursorData* getPalettedCursorData(CursorID cursorId);

    // OPENRCT2MINI revision 59: high-contrast cursor-style override. Returns a
    // mono CursorData* if the cursor has a HC-specific bitmap; nullptr
    // otherwise (the SoftwareCursor then re-uses getCursorData() with
    // HC palette indices). Used only when interface.cursorStyle ==
    // CursorStyle::HighContrast.
    const CursorData* getHighContrastCursorData(CursorID cursorId);

    // OPENRCT2MINI revision 77: Windows-theme overrides. Returns a mono
    // CursorData* for cursors with a Windows-specific bitmap, nullptr
    // otherwise. Used only when interface.cursorStyle == CursorStyle::Windows;
    // all other cursors share the Default mono blob.
    const CursorData* getWindowsCursorData(CursorID cursorId);
} // namespace OpenRCT2::Ui
