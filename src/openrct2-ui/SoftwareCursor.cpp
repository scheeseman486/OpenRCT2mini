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

    #include <openrct2-ui/windows/Windows.h>
    #include <openrct2/config/Config.h>
    #include <openrct2/core/EnumUtils.hpp>
    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/drawing/FilterPaletteIds.h>
    #include <openrct2/drawing/PaletteMap.h>

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

        // OPENRCT2MINI revision 59: high-contrast palette indices. pi0 is the
        // engine's "void colour" reference — never touched by any palette
        // load (LoadPalette / GfxTransposePalette / UpdatePaletteEffects
        // all start at pi10), zero-initialised RGB(0,0,0) at static init,
        // never overwritten. pi255 is force-set to RGB(255,255,255) on
        // every palette load (Drawing.cpp:732). Both are guaranteed
        // pure-black / pure-white at the framebuffer level.
        constexpr uint8_t kHCBlackIdx = 0;
        constexpr uint8_t kHCWhiteIdx = 255;

        constexpr uint32_t SpriteCacheKey(CursorID id, OpenRCT2::Config::CursorStyle style)
        {
            return (static_cast<uint32_t>(style) << 16) | static_cast<uint32_t>(EnumValue(id));
        }

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
        // OPENRCT2MINI revision 59: cache by (style, cursor) so swapping themes
        // doesn't reuse a stale-palette sprite.
        const uint32_t key = SpriteCacheKey(id, _style);
        auto it = _sprites.find(key);
        if (it != _sprites.end())
            return &it->second;

        // OPENRCT2MINI revision 61 / 77: four themes.
        //   Classic       — pi10/pi17 mono + paletted-pointer gradient.
        //   Default       — pi0 outline + pi255 fill + HC mono pointer.
        //   HighContrast  — pi255 outline + pi0 fill (Default inverted),
        //                   same bitmaps as Default.
        //   Windows       — same palette and bitmaps as Default, except
        //                   the Arrow cursor is replaced with a classic
        //                   Win9x-style pointer (kArrowCursorDataWindows).
        // Default, Windows and HighContrast share everything except:
        //   - HighContrast swaps the outline/fill palette indices.
        //   - Windows swaps the Arrow bitmap (via getWindowsCursorData).
        const bool isClassic = (_style == OpenRCT2::Config::CursorStyle::Classic);
        const bool isInverted = (_style == OpenRCT2::Config::CursorStyle::HighContrast);
        const bool isWindows = (_style == OpenRCT2::Config::CursorStyle::Windows);
        const uint8_t blackIdx = isClassic ? kBlackIdx : (isInverted ? kHCWhiteIdx : kHCBlackIdx);
        const uint8_t whiteIdx = isClassic ? kWhiteIdx : (isInverted ? kHCBlackIdx : kHCWhiteIdx);

        Sprite spr;
        // Initialise to "transparent" so cursors with no data byte at a
        // pixel position don't get drawn as palette index 0 (which would
        // be a visible black box on the panel).
        spr.pixels.fill(kTransparent);

        // OPENRCT2MINI cut 52 / revision 61: prefer the full-palette variant
        // when one exists, but ONLY in Classic. The Default and HighContrast
        // themes drop the paletted gradient and substitute the hand-drawn
        // mono pointer instead — the gradient defeats the whole point of
        // a high-readability theme.
        if (isClassic)
        {
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
        }

        // OPENRCT2MINI revision 59 / 61 / 77: theme-specific Arrow override.
        //   - Windows prefers the Win9x pointer bitmap.
        //   - Default and HighContrast prefer the hand-drawn mono pointer.
        //   - All other cursors fall back to the regular mono blob and
        //     get recoloured with the theme's outline/fill palette indices.
        const CursorData* cd = nullptr;
        if (isWindows)
            cd = getWindowsCursorData(id);
        if (cd == nullptr && !isClassic)
            cd = getHighContrastCursorData(id);
        if (cd == nullptr)
            cd = getCursorData(id);
        if (cd != nullptr)
        {
            // X-cursor pixel rules:
            //   mask=0           → transparent
            //   mask=1, data=1   → black opaque (palette = blackIdx)
            //   mask=1, data=0   → white opaque (palette = whiteIdx)
            for (int y = 0; y < kSpriteH; ++y)
            {
                for (int x = 0; x < kSpriteW; ++x)
                {
                    if (!BitAt(cd->Mask, x, y, kSpriteW))
                        continue;
                    const bool dataBit = BitAt(cd->Data, x, y, kSpriteW);
                    spr.pixels[y * kSpriteW + x] = dataBit ? blackIdx : whiteIdx;
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

    void SoftwareCursor::SetStyle(OpenRCT2::Config::CursorStyle style)
    {
        if (_style == style)
            return;
        _style = style;
        // The (style, id) cache key already partitions sprites — so a fresh
        // Composite with the new style simply misses the cache and re-decodes
        // the active cursor with the new palette indices. We don't even need
        // to evict, but old sprites are now dead weight for any cursors that
        // never get used again. Clear to keep memory tidy; rebuild is cheap.
        _sprites.clear();
    }

    void SoftwareCursor::SetCursor(CursorID id)
    {
        _activeCursor = id;
    }

    void SoftwareCursor::SetVisible(bool visible)
    {
        _visible = visible;
    }

    // OPENRCT2MINI revision 78: hotspot accessor for the host engine's
    // save+restore region. Decodes the sprite if it's not cached yet.
    std::pair<int, int> SoftwareCursor::GetActiveHotspot()
    {
        const Sprite* spr = GetOrCreateSprite(_activeCursor);
        if (spr == nullptr)
            return { 0, 0 };
        return { spr->hotspotX, spr->hotspotY };
    }

    void SoftwareCursor::Composite(
        OpenRCT2::Drawing::PaletteIndex* bits, int32_t bitsW, int32_t bitsH, int32_t x, int32_t y)
    {
        if (!_visible || bits == nullptr || bitsW <= 0 || bitsH <= 0)
            return;
        // OPENRCT2MINI osk-overhaul: the OSK-active gate that used to
        // suppress the cursor here is gone. The OSK now plays by the
        // same rules as every other window — the selector mode + Phase
        // F.5 ring-render-hook decide what's visible. When the selector
        // is hidden (cursor mode), the OS-level cursor shows; when
        // active (focus mode), the cursor is suppressed via the
        // selector-mode gate higher up the SoftwareCursor pipeline.

        const Sprite* spr = GetOrCreateSprite(_activeCursor);
        if (spr == nullptr)
            return;

        const int32_t x0 = x - spr->hotspotX;
        const int32_t y0 = y - spr->hotspotY;

        // OPENRCT2MINI: optional drop shadow under the cursor sprite. We
        // run a pre-pass that walks every non-transparent sprite pixel
        // and remaps the destination FB pixel at (+2, +2) through a
        // darken palette — equivalent to a ~50% black overlay that
        // respects whatever's underneath. The cursor body is then drawn
        // on top so the shadow only ends up visible around the sprite's
        // outline. Default off; toggled in Options > Interface > Cursors.
        if (Config::Get().interface.cursorDropShadow)
        {
            constexpr int32_t kShadowOffX = 1;
            constexpr int32_t kShadowOffY = 2;
            const auto paletteMap = ::GetPaletteMapForColour(
                OpenRCT2::Drawing::FilterPaletteID::paletteDarken2);
            if (paletteMap.has_value())
            {
                const auto& map = *paletteMap;
                for (int sy = 0; sy < kSpriteH; ++sy)
                {
                    const int32_t dy = y0 + sy + kShadowOffY;
                    if (dy < 0 || dy >= bitsH)
                        continue;
                    const uint8_t* srcRow = spr->pixels.data() + sy * kSpriteW;
                    auto* dstRow = bits + static_cast<size_t>(dy) * bitsW;
                    for (int sx = 0; sx < kSpriteW; ++sx)
                    {
                        if (srcRow[sx] == kTransparent)
                            continue;
                        const int32_t dx = x0 + sx + kShadowOffX;
                        if (dx < 0 || dx >= bitsW)
                            continue;
                        const auto under = static_cast<OpenRCT2::Drawing::PaletteIndex>(dstRow[dx]);
                        dstRow[dx] = static_cast<OpenRCT2::Drawing::PaletteIndex>(map[static_cast<size_t>(under)]);
                    }
                }
            }
        }

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
