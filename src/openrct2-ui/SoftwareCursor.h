/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI: cut 38 / cut 43c. Software cursor — composites into the
// X8 paletted framebuffer (`_bits`) BEFORE the screen-texture upload.
//
// Originally (cut 38) we used a separate SDL_Texture and SDL_RenderCopy.
// That doesn't work on the Miyoo Mini's mmiyoo SDL render driver: it
// blits exactly one texture per frame (the most recent QueueCopy is
// stashed into gfx.thread[0] and only that gets pushed to the panel).
// A second SDL_RenderCopy for the cursor silently overwrites slot 0 and
// the actual scene never reaches the framebuffer.
//
// Cut 43c: composite cursor pixels directly into `_bits` (paletted)
// before HardwareDisplayDrawingEngine::CopyBitsToTexture runs. That way
// OpenRCT2 still issues exactly one SDL_RenderCopy per frame and the
// driver is happy. We pre-bake each CursorID into a small paletted
// sprite (kTransparent / black-index / white-index) at SetCursor time
// for cheap per-frame compositing.

#pragma once

#ifdef ENABLE_SOFTWARE_CURSOR

    #include <openrct2/config/Config.h>  // OPENRCT2MINI revision 59 — OpenRCT2::Config::CursorStyle enum
    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/interface/Cursors.h>
    #include <array>
    #include <unordered_map>
    #include <utility>

namespace OpenRCT2::Ui
{
    class SoftwareCursor
    {
    public:
        // Cursor sprite is 32×32 (matches the upstream X-style cursor
        // packing the project uses for non-system cursors).
        static constexpr int kSpriteW = 32;
        static constexpr int kSpriteH = 32;

    private:
        struct Sprite
        {
            // 0xFE = transparent (no write), other byte = palette index.
            std::array<uint8_t, kSpriteW * kSpriteH> pixels{};
            int hotspotX = 0;
            int hotspotY = 0;
        };

        // OPENRCT2MINI revision 59: cache key combines cursorId AND active style
        // so swapping themes never reuses a stale Default-palette sprite for
        // a HighContrast composite (or vice versa). The bit packing is just
        // (style << 16) | cursorId — both fit comfortably in 32 bits.
        std::unordered_map<uint32_t, Sprite> _sprites;
        CursorID _activeCursor = CursorID::Arrow;
        bool _visible = true;
        OpenRCT2::Config::CursorStyle _style = OpenRCT2::Config::CursorStyle::Default;

        const Sprite* GetOrCreateSprite(CursorID id);

    public:
        SoftwareCursor() = default;
        ~SoftwareCursor() = default;

        SoftwareCursor(const SoftwareCursor&) = delete;
        SoftwareCursor& operator=(const SoftwareCursor&) = delete;

        void SetCursor(CursorID id);
        void SetVisible(bool visible);

        // OPENRCT2MINI revision 59: change the active cursor theme. Cheap if
        // the style is unchanged; on an actual change, walks the cache
        // and drops sprites belonging to the previous style so the next
        // Composite re-decodes with the new palette.
        void SetStyle(OpenRCT2::Config::CursorStyle style);
        OpenRCT2::Config::CursorStyle GetStyle() const { return _style; }

        // Composite the active cursor sprite into the paletted framebuffer
        // at screen-pixel position (x, y). Bounds-checks and skips
        // transparent texels. Caller hands in the X8DrawingEngine bits
        // pointer + dimensions and calls this BEFORE the texture upload.
        void Composite(
            OpenRCT2::Drawing::PaletteIndex* bits, int32_t bitsW, int32_t bitsH, int32_t x, int32_t y);

        // OPENRCT2MINI revision 78: expose the active cursor's hotspot so
        // HardwareDisplayDrawingEngine can anchor its save+restore region
        // exactly on the sprite footprint. Returns (0, 0) if the active
        // cursor hasn't been decoded yet (cache miss); the next Composite
        // will populate it.
        std::pair<int, int> GetActiveHotspot();
    };
} // namespace OpenRCT2::Ui

#endif // ENABLE_SOFTWARE_CURSOR
