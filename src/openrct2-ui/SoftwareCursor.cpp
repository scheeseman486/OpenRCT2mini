/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI: cut 38 / 43c. See SoftwareCursor.h.

#ifdef ENABLE_SOFTWARE_CURSOR

    #include "SoftwareCursor.h"

    #include "CursorData.h"

    #include <openrct2/core/EnumUtils.hpp>

namespace OpenRCT2::Ui
{
    namespace
    {
        // Paletted-sprite sentinels. Index 0xFF would collide with the
        // forced-white palette slot the game writes (see Drawing.cpp
        // gPalette[255]={255,255,255}); 255 also conflicts with our
        // transparent sentinel below. So we use real palette indices
        // for cursor pixels and reserve 0xFE for "transparent" — the
        // game has no special meaning at index 0xFE, and our composite
        // loop skips it before writing into _bits.
        //
        // pi10 / pi17 are the OpenRCT2 PaletteIndex enum values for
        // black-ish and white-ish remap colors (see PaletteIndex.h).
        // They look like a dark grey outline + bright fill on every
        // standard palette, which is what we want for a visible cursor.
        constexpr uint8_t kTransparent = 0xFE;
        constexpr uint8_t kBlackIdx = 10;  // pi10 "Black (0-dark)"
        constexpr uint8_t kWhiteIdx = 17;  // pi17 "White (dark)"

        // X-style packed-bitmap cursor format: each row is `widthBits` bits
        // packed MSB-first into bytes. The CursorData blobs are width=32,
        // height-bytes=4 (32 px tall).
        bool BitAt(const uint8_t* data, size_t x, size_t y, size_t widthBits) noexcept
        {
            const size_t pos = y * widthBits + x;
            return (data[pos / 8] & (1u << (7 - (x % 8)))) != 0;
        }

        // ASCII-art fallbacks for cursors CursorData has no entry for
        // (Arrow and HandPoint). Same templates as cut 38b.
        const char* kArrowFallback[] = {
            "##                              ",
            "#X#                             ",
            "#XX#                            ",
            "#XXX#                           ",
            "#XXXX#                          ",
            "#XXXXX#                         ",
            "#XXXXXX#                        ",
            "#XXXXXXX#                       ",
            "#XXXXXXXX#                      ",
            "#XXXXXXXXX#                     ",
            "#XXXXXXXXXX#                    ",
            "#XXXXXXX#####                   ",
            "#XXXXXXX#                       ",
            "#XXX#XXXX#                      ",
            "#XX# #XXX#                      ",
            "##    #XXX#                     ",
            "       #XX#                     ",
            "       #XX#                     ",
            "        ##                      ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
        };

        const char* kHandFallback[] = {
            "         ##                     ",
            "         #X#                    ",
            "         #X#                    ",
            "         #X#                    ",
            "         #X#                    ",
            "         #X##                   ",
            "         #X#X##                 ",
            "         #X#X#X##               ",
            "      ## #X#X#X#X#              ",
            "     #X###X#X#X#X#              ",
            "    #X#X#X#X#X#X#X#             ",
            "    #X#X#X#X#X#X#X#             ",
            "   ##X#X#X#X#X#X#X#             ",
            "  #X##X#X#X#X#X#X#X#            ",
            "  #X#X#X#X#X#X#X#X##            ",
            "  #X#X#X#X#X#X#X#X#             ",
            "   #XX#X#X#X#X#X#X#             ",
            "    #XX#X#X#X#X#X#              ",
            "     #XX#X#X#X#X#               ",
            "      ###########               ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
            "                                ",
        };

        void DrawAsciiBitmap(uint8_t* pixels, const char* const* rows, int w, int h)
        {
            for (int y = 0; y < h; ++y)
            {
                const char* row = rows[y];
                for (int x = 0; x < w; ++x)
                {
                    const char c = row[x];
                    if (c == '#')
                        pixels[y * w + x] = kBlackIdx;
                    else if (c == 'X')
                        pixels[y * w + x] = kWhiteIdx;
                    else
                        pixels[y * w + x] = kTransparent;
                }
            }
        }
    } // namespace

    const SoftwareCursor::Sprite* SoftwareCursor::GetOrCreateSprite(CursorID id)
    {
        const int key = EnumValue(id);
        auto it = _sprites.find(key);
        if (it != _sprites.end())
            return &it->second;

        Sprite spr;
        // Initialise to "transparent" so cursors with no data byte at a
        // pixel position don't get drawn as palette index 0 (which would
        // be a visible black box on the panel).
        spr.pixels.fill(kTransparent);

        // OPENRCT2MINI cut 52: prefer the full-palette variant when one
        // exists for this CursorID. Lets us keep soft-shaded cursors
        // (e.g. the user's pointer.png with greyscale gradient) without
        // quantising to the X-cursor 2-colour format.
        if (const PalettedCursorData* pd = getPalettedCursorData(id); pd != nullptr)
        {
            for (size_t i = 0; i < kSpriteW * kSpriteH; ++i)
            {
                const uint8_t b = pd->Pixels[i];
                // The paletted format and our internal sprite both use
                // 0xFE as the transparent sentinel (kPalettedCursorTransparent
                // == kTransparent), so we can copy bytes through directly.
                spr.pixels[i] = b;
            }
            spr.hotspotX = pd->HotSpot.X;
            spr.hotspotY = pd->HotSpot.Y;
            auto [insIt, _] = _sprites.emplace(key, std::move(spr));
            return &insIt->second;
        }

        const CursorData* cd = getCursorData(id);
        if (cd != nullptr)
        {
            // X-cursor pixel rules:
            //   mask=0           → transparent
            //   mask=1, data=1   → black opaque
            //   mask=1, data=0   → white opaque
            for (int y = 0; y < kSpriteH; ++y)
            {
                for (int x = 0; x < kSpriteW; ++x)
                {
                    if (!BitAt(cd->Mask, x, y, kSpriteW))
                        continue;
                    const bool dataBit = BitAt(cd->Data, x, y, kSpriteW);
                    spr.pixels[y * kSpriteW + x] = dataBit ? kBlackIdx : kWhiteIdx;
                }
            }
            spr.hotspotX = cd->HotSpot.X;
            spr.hotspotY = cd->HotSpot.Y;
        }
        else if (id == CursorID::HandPoint)
        {
            DrawAsciiBitmap(spr.pixels.data(), kHandFallback, kSpriteW, kSpriteH);
            spr.hotspotX = 8;
            spr.hotspotY = 0;
        }
        else
        {
            // Default fallback for Arrow + any other cursor without data.
            DrawAsciiBitmap(spr.pixels.data(), kArrowFallback, kSpriteW, kSpriteH);
            spr.hotspotX = 0;
            spr.hotspotY = 0;
        }

        auto [insIt, _] = _sprites.emplace(key, std::move(spr));
        return &insIt->second;
    }

    void SoftwareCursor::SetCursor(CursorID id)
    {
        _activeCursor = id;
    }

    void SoftwareCursor::SetVisible(bool visible)
    {
        _visible = visible;
    }

    void SoftwareCursor::Composite(
        OpenRCT2::Drawing::PaletteIndex* bits, int32_t bitsW, int32_t bitsH, int32_t x, int32_t y)
    {
        if (!_visible || bits == nullptr || bitsW <= 0 || bitsH <= 0)
            return;

        const Sprite* spr = GetOrCreateSprite(_activeCursor);
        if (spr == nullptr)
            return;

        const int32_t x0 = x - spr->hotspotX;
        const int32_t y0 = y - spr->hotspotY;

        for (int sy = 0; sy < kSpriteH; ++sy)
        {
            const int32_t dy = y0 + sy;
            if (dy < 0 || dy >= bitsH)
                continue;
            const uint8_t* srcRow = spr->pixels.data() + sy * kSpriteW;
            auto* dstRow = bits + static_cast<size_t>(dy) * bitsW;
            for (int sx = 0; sx < kSpriteW; ++sx)
            {
                const int32_t dx = x0 + sx;
                if (dx < 0 || dx >= bitsW)
                    continue;
                const uint8_t p = srcRow[sx];
                if (p == kTransparent)
                    continue;
                dstRow[dx] = static_cast<OpenRCT2::Drawing::PaletteIndex>(p);
            }
        }
    }
} // namespace OpenRCT2::Ui

#endif // ENABLE_SOFTWARE_CURSOR
