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

    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/interface/Cursors.h>
    #include <array>
    #include <unordered_map>

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

        std::unordered_map<int, Sprite> _sprites; // keyed by CursorID
        CursorID _activeCursor = CursorID::Arrow;
        bool _visible = true;

        const Sprite* GetOrCreateSprite(CursorID id);

    public:
        SoftwareCursor() = default;
        ~SoftwareCursor() = default;

        SoftwareCursor(const SoftwareCursor&) = delete;
        SoftwareCursor& operator=(const SoftwareCursor&) = delete;

        void SetCursor(CursorID id);
        void SetVisible(bool visible);

        // Composite the active cursor sprite into the paletted framebuffer
        // at screen-pixel position (x, y). Bounds-checks and skips
        // transparent texels. Caller hands in the X8DrawingEngine bits
        // pointer + dimensions and calls this BEFORE the texture upload.
        void Composite(
            OpenRCT2::Drawing::PaletteIndex* bits, int32_t bitsW, int32_t bitsH, int32_t x, int32_t y);
    };
} // namespace OpenRCT2::Ui

#endif // ENABLE_SOFTWARE_CURSOR
