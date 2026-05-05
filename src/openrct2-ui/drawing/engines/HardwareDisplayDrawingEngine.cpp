/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "DrawingEngineFactory.hpp"

#include <SDL.h>
#include <cmath>
#include <cstdio>  // OPENRCT2MINI: cut 39m, fputs/fflush for renderer fallback msg
#include <cstring> // OPENRCT2MINI: cut 44d, memset for cursor backup edges
#include <memory>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Guard.hpp>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/drawing/LightFX.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/Input.h>
#include <openrct2/interface/Cursors.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>  // OPENRCT2MINI cut 41c: gWindowList
#include <openrct2/paint/Paint.h>
#include <openrct2/ui/UiContext.h>
#include <vector>

// OPENRCT2MINI: cut 38. Software cursor — for platforms with no compositor /
// hardware cursor overlay (Miyoo Mini libmi_gfx framebuffer).
#ifdef ENABLE_SOFTWARE_CURSOR
    #include "../../SoftwareCursor.h"
#endif

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

class HardwareDisplayDrawingEngine final : public X8DrawingEngine
{
private:
    constexpr static uint32_t kDirtyVisualTime = 40;
    constexpr static uint32_t kDirtyRegionAlpha = 100;

    IUiContext& _uiContext;
    SDL_Window* _window = nullptr;
    SDL_Renderer* _sdlRenderer = nullptr;
    SDL_Texture* _screenTexture = nullptr;
    SDL_Texture* _scaledScreenTexture = nullptr;
    SDL_PixelFormat* _screenTextureFormat = nullptr;
    uint32_t _paletteHWMapped[256] = { 0 };
    uint32_t _lightPaletteHWMapped[256] = { 0 };

    bool _useVsync = true;

    std::vector<uint32_t> _dirtyVisualsTime;

    bool smoothNN = false;

#ifdef ENABLE_SOFTWARE_CURSOR
    // OPENRCT2MINI: cut 38 / 43c. Software cursor — composited into _bits
    // each frame BEFORE the texture upload so it rides on the same single
    // SDL_RenderCopy the mmiyoo driver actually paints. See SoftwareCursor.h.
    std::unique_ptr<SoftwareCursor> _swCursor;
    CursorID _lastSwCursor = CursorID::Undefined;
    // OPENRCT2MINI cut 44: track previous cursor footprint so we can
    // invalidate it for the next frame's PaintWindows. Helps when
    // PaintWindows DOES repaint the affected area (toolbar, dynamic
    // viewport). Doesn't help on static screens (preloader) where
    // nothing redraws — see the save/restore buffer below.
    int32_t _lastCursorRectL = 0, _lastCursorRectT = 0;
    int32_t _lastCursorRectR = 0, _lastCursorRectB = 0;
    bool _lastCursorRectValid = false;

    // OPENRCT2MINI cut 44d: save+restore the pixels under the cursor.
    // PaintWindows only repaints regions it considers dirty, which on
    // the preloader / static UI chrome doesn't include our cursor
    // footprint. Save the under-cursor pixels right before we composite
    // each frame, restore them at the START of next frame's BeginDraw
    // (before PaintWindows runs). Buffer is sized for the cursor sprite.
    static constexpr int kCursorBackupCap = 64; // a bit larger than 32 for
                                                // hot-spot offsets / safety
    OpenRCT2::Drawing::PaletteIndex _cursorBackup[kCursorBackupCap * kCursorBackupCap]{};
    int32_t _cursorBackupX = 0;
    int32_t _cursorBackupY = 0;
    int32_t _cursorBackupW = 0;
    int32_t _cursorBackupH = 0;
    bool _cursorBackupValid = false;
#endif

public:
    explicit HardwareDisplayDrawingEngine(IUiContext& uiContext)
        : X8DrawingEngine(uiContext)
        , _uiContext(uiContext)
    {
        _window = static_cast<SDL_Window*>(_uiContext.GetWindow());
    }

    ~HardwareDisplayDrawingEngine() override
    {
#ifdef ENABLE_SOFTWARE_CURSOR
        // Tear down before the renderer it depends on.
        _swCursor.reset();
#endif
        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        if (_scaledScreenTexture != nullptr)
        {
            SDL_DestroyTexture(_scaledScreenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);
        SDL_DestroyRenderer(_sdlRenderer);
    }

    void Initialise() override
    {
        // OPENRCT2MINI: cut 39m. Try the accelerated renderer first (the
        // upstream behaviour, fast on desktop), but fall back to the
        // software renderer if SDL can't bring up an accelerated one.
        // Necessary on the Miyoo Mini, where vanilla SDL2 has no driver
        // for the SigmaStar libmi_gfx framebuffer that supports
        // SDL_RENDERER_ACCELERATED — the call returns NULL there, and
        // every later SDL_RenderCopy / SDL_RenderPresent null-derefs.
        const Uint32 vsyncFlag = _useVsync ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC) : 0u;
        _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED | vsyncFlag);
        if (_sdlRenderer == nullptr)
        {
            std::fputs(
                "[OPENRCT2MINI] HW renderer unavailable (",
                stderr);
            std::fputs(SDL_GetError(), stderr);
            std::fputs("); falling back to SDL_RENDERER_SOFTWARE.\n", stderr);
            std::fflush(stderr);
            _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (_sdlRenderer == nullptr)
        {
            std::fputs(
                "[OPENRCT2MINI] FATAL: no SDL renderer available (", stderr);
            std::fputs(SDL_GetError(), stderr);
            std::fputs(
                "). The SDL2 build can't drive this platform's panel. "
                "On the Miyoo Mini this is the cue to integrate the "
                "XK9274/sdl2_miyoo vendor SDL2 port.\n",
                stderr);
            std::fflush(stderr);
        }
#ifdef ENABLE_SOFTWARE_CURSOR
        // Cut 43c: SoftwareCursor no longer needs the SDL_Renderer — it
        // composites paletted pixels directly into _bits.
        _swCursor = std::make_unique<SoftwareCursor>();
        // SDL won't render a cursor for us on a framebuffer-only platform —
        // hide it so we don't double-draw on dev builds that DO have a real
        // display under us (SDL_ShowCursor is a no-op on the device anyway).
        SDL_ShowCursor(SDL_DISABLE);
#endif
    }

    void SetVSync(bool vsync) override
    {
        if (_useVsync != vsync)
        {
            _useVsync = vsync;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_RenderSetVSync(_sdlRenderer, vsync ? 1 : 0);
#else
            SDL_DestroyRenderer(_sdlRenderer);
            _screenTexture = nullptr;
            _scaledScreenTexture = nullptr;
            Initialise();
            Resize(_uiContext->GetWidth(), _uiContext->GetHeight());
#endif
        }
    }

    void Resize(uint32_t width, uint32_t height) override
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);

        SDL_RendererInfo rendererInfo = {};
        int32_t result = SDL_GetRendererInfo(_sdlRenderer, &rendererInfo);
        if (result < 0)
        {
            LOG_WARNING("HWDisplayDrawingEngine::Resize error: %s", SDL_GetError());
            return;
        }
        uint32_t pixelFormat = SDL_PIXELFORMAT_UNKNOWN;
        for (uint32_t i = 0; i < rendererInfo.num_texture_formats; i++)
        {
            uint32_t format = rendererInfo.texture_formats[i];
            // OPENRCT2MINI cut 40c: skip UNKNOWN (=0). Some render drivers
            // (e.g. the XK9274 sdl2_miyoo fork pre-fix) leave gaps in their
            // texture_formats array; iterating past num_texture_formats sees
            // zero, which previously won the bytes-per-pixel race because
            // SDL_BYTESPERPIXEL(0) = 0 < any concrete format. Then
            // SDL_CreateTexture(format=0) rejects it and we crash.
            if (format == SDL_PIXELFORMAT_UNKNOWN)
                continue;
            if (SDL_ISPIXELFORMAT_FOURCC(format) || SDL_ISPIXELFORMAT_INDEXED(format))
                continue;
            // OPENRCT2MINI cut 41d: prefer the LARGEST bytes-per-pixel
            // (i.e. ARGB8888) instead of the smallest. The non-ARGB8888
            // paths in CopyBitsToTexture below are written with broken
            // padding math (`pitch == (width*2) + (pitch - width*4)` only
            // holds at width == 0) and a byte-truncating palette read, so
            // any 16-bpp texture renders black even if the underlying blit
            // works. ARGB8888 is the only branch that's actually exercised
            // upstream and that we know paints correctly. Costs ~600 KB
            // more texture RAM at 640×480 vs RGB565.
            if (pixelFormat == SDL_PIXELFORMAT_UNKNOWN
                || SDL_BYTESPERPIXEL(format) > SDL_BYTESPERPIXEL(pixelFormat))
            {
                pixelFormat = format;
            }
        }

        ScaleQuality scaleQuality = GetContext()->GetUiContext().GetScaleQuality();
        if (scaleQuality == ScaleQuality::SmoothNearestNeighbour)
        {
            scaleQuality = ScaleQuality::Linear;
            smoothNN = true;
        }
        else
        {
            smoothNN = false;
        }

        if (smoothNN)
        {
            if (_scaledScreenTexture != nullptr)
            {
                SDL_DestroyTexture(_scaledScreenTexture);
            }

            char scaleQualityBuffer[4];
            snprintf(scaleQualityBuffer, sizeof(scaleQualityBuffer), "%d", static_cast<int32_t>(scaleQuality));
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create unscaled screen texture (%ux%u, pixelFormat = %u): %s", width,
                height, pixelFormat, SDL_GetError());

            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaleQualityBuffer);

            uint32_t scale = std::ceil(Config::Get().general.windowScale);
            _scaledScreenTexture = SDL_CreateTexture(
                _sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_TARGET, width * scale, height * scale);

            Guard::Assert(
                _scaledScreenTexture != nullptr,
                "Failed to create scaled screen texture (%ux%u, scale = %u, pixelFormat = %u): %s", width, height, scale,
                pixelFormat, SDL_GetError());
        }
        else
        {
            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create screen texture (%ux%u, pixelFormat = %u): %s", width, height,
                pixelFormat, SDL_GetError());
        }

        uint32_t format;
        SDL_QueryTexture(_screenTexture, &format, nullptr, nullptr, nullptr);
        _screenTextureFormat = SDL_AllocFormat(format);

        X8DrawingEngine::Resize(width, height);
    }

    void SetPalette(const GamePalette& palette) override
    {
        if (_screenTextureFormat != nullptr)
        {
            for (int32_t i = 0; i < 256; i++)
            {
                _paletteHWMapped[i] = SDL_MapRGB(_screenTextureFormat, palette[i].red, palette[i].green, palette[i].blue);
            }

            if (Config::Get().general.enableLightFx)
            {
                auto& lightPalette = LightFx::GetPalette();
                for (int32_t i = 0; i < 256; i++)
                {
                    const auto& src = lightPalette[i];
                    _lightPaletteHWMapped[i] = SDL_MapRGBA(_screenTextureFormat, src.red, src.green, src.blue, src.alpha);
                }
            }
        }
    }

    void BeginDraw() override
    {
        X8DrawingEngine::BeginDraw();

#ifdef ENABLE_SOFTWARE_CURSOR
        // OPENRCT2MINI cut 44d: restore the pixels we saved under last
        // frame's cursor BEFORE PaintWindows runs. If a window paints
        // over the area, our restore is harmless (gets overwritten with
        // fresh content). If nothing paints over it (preloader, static
        // chrome), our restore makes _bits show the original under-
        // cursor content, so the cursor appears to "move away cleanly".
        if (_cursorBackupValid && _bits != nullptr)
        {
            const int32_t bitsW = static_cast<int32_t>(_width);
            const int32_t bitsH = static_cast<int32_t>(_height);
            for (int32_t row = 0; row < _cursorBackupH; ++row)
            {
                const int32_t dy = _cursorBackupY + row;
                if (dy < 0 || dy >= bitsH)
                    continue;
                const int32_t srcOff = row * _cursorBackupW;
                for (int32_t col = 0; col < _cursorBackupW; ++col)
                {
                    const int32_t dx = _cursorBackupX + col;
                    if (dx < 0 || dx >= bitsW)
                        continue;
                    _bits[static_cast<size_t>(dy) * bitsW + dx] = _cursorBackup[srcOff + col];
                }
            }
            _cursorBackupValid = false;
        }
#endif
    }

    void EndDraw() override
    {
        X8DrawingEngine::EndDraw();

        Display();
    }

protected:
    void OnDrawDirtyBlock(int32_t left, int32_t top, int32_t right, int32_t bottom) override
    {
        if (gShowDirtyVisuals)
        {
            const auto columns = ((right - left) + (_invalidationGrid.getBlockWidth() - 1)) / _invalidationGrid.getBlockWidth();
            const auto rows = ((bottom - top) + (_invalidationGrid.getBlockHeight() - 1)) / _invalidationGrid.getBlockHeight();
            const auto firstRow = top / _invalidationGrid.getBlockHeight();
            const auto firstColumn = left / _invalidationGrid.getBlockWidth();

            for (uint32_t y = 0; y < rows; y++)
            {
                for (uint32_t x = 0; x < columns; x++)
                {
                    SetDirtyVisualTime(firstColumn + x, firstRow + y, gCurrentRealTimeTicks + kDirtyVisualTime);
                }
            }
        }
    }

private:
    void Display()
    {
        auto* viewport = WindowGetViewport(WindowGetMain());

#ifdef ENABLE_SOFTWARE_CURSOR
        // OPENRCT2MINI cut 43c / 44: composite cursor sprite into the
        // paletted X8 framebuffer BEFORE the texture upload. The mmiyoo
        // SDL render driver only blits one texture per frame; we can't
        // issue a separate SDL_RenderCopy for the cursor. So we bake the
        // cursor pixels into _bits and let the single CopyBitsToTexture
        // below carry them to the panel.
        //
        // Cut 44: invalidate the PREVIOUS cursor footprint right before
        // we draw the new one, so PaintWindows in the NEXT frame repaints
        // over the trail. Without this the X8 engine's dirty-block grid
        // doesn't know we wrote pixels there — the old cursor lingers
        // indefinitely if nothing else dirties that area.
        if (_swCursor != nullptr)
        {
            // Invalidate the previous cursor rect for next frame's repaint.
            if (_lastCursorRectValid)
            {
                Invalidate(_lastCursorRectL, _lastCursorRectT, _lastCursorRectR, _lastCursorRectB);
            }

            CursorID cur = _uiContext.GetCursor();
            if (cur != _lastSwCursor)
            {
                _swCursor->SetCursor(cur);
                _lastSwCursor = cur;
            }
            const auto* state = _uiContext.GetCursorState();
            if (state != nullptr && _bits != nullptr)
            {
                // OPENRCT2MINI cut 44d: snapshot the under-cursor pixels
                // BEFORE compositing, so the next frame's BeginDraw can
                // restore them. The save area is the same kCursorBackupCap-
                // sized rect we'll overwrite with the cursor sprite; the
                // hot-spot offset means the cursor can poke a few pixels
                // to the upper-left of the position, hence the kCursorBackupCap
                // > sprite size for safety.
                const int32_t bitsW = static_cast<int32_t>(_width);
                const int32_t bitsH = static_cast<int32_t>(_height);
                const int32_t bx = state->position.x - 16;
                const int32_t by = state->position.y - 16;
                _cursorBackupX = bx;
                _cursorBackupY = by;
                _cursorBackupW = kCursorBackupCap;
                _cursorBackupH = kCursorBackupCap;
                for (int32_t row = 0; row < _cursorBackupH; ++row)
                {
                    const int32_t dy = by + row;
                    if (dy < 0 || dy >= bitsH)
                    {
                        // Fill out-of-bounds rows with zero so the restore
                        // doesn't write garbage if the cursor is at an edge.
                        std::memset(
                            &_cursorBackup[row * _cursorBackupW], 0,
                            _cursorBackupW * sizeof(_cursorBackup[0]));
                        continue;
                    }
                    for (int32_t col = 0; col < _cursorBackupW; ++col)
                    {
                        const int32_t dx = bx + col;
                        _cursorBackup[row * _cursorBackupW + col]
                            = (dx < 0 || dx >= bitsW)
                                  ? OpenRCT2::Drawing::PaletteIndex{}
                                  : _bits[static_cast<size_t>(dy) * bitsW + dx];
                    }
                }
                _cursorBackupValid = true;

                _swCursor->Composite(
                    _bits, bitsW, bitsH, state->position.x, state->position.y);

                // Track the rect for cut 44's Invalidate fallback (helps
                // window areas redraw promptly even before our save/
                // restore kicks in).
                _lastCursorRectL = bx;
                _lastCursorRectT = by;
                _lastCursorRectR = bx + kCursorBackupCap;
                _lastCursorRectB = by + kCursorBackupCap;
                _lastCursorRectValid = true;
            }
        }
#endif

        if (Config::Get().general.enableLightFx && viewport != nullptr)
        {
            void* pixels;
            int32_t pitch;
            if (SDL_LockTexture(_screenTexture, nullptr, &pixels, &pitch) == 0)
            {
                LightFx::RenderToTexture(
                    *viewport, pixels, pitch, _bits, _width, _height, _paletteHWMapped, _lightPaletteHWMapped);
                SDL_UnlockTexture(_screenTexture);
            }
        }
        else
        {
            CopyBitsToTexture(
                _screenTexture, _bits, static_cast<int32_t>(_width), static_cast<int32_t>(_height), _paletteHWMapped);
        }
        if (smoothNN)
        {
            SDL_SetRenderTarget(_sdlRenderer, _scaledScreenTexture);
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);

            SDL_SetRenderTarget(_sdlRenderer, nullptr);
            SDL_RenderCopy(_sdlRenderer, _scaledScreenTexture, nullptr, nullptr);
        }
        else
        {
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);
        }

        if (gShowDirtyVisuals)
        {
            RenderDirtyVisuals();
        }

// OPENRCT2MINI cut 43c: cursor compositing moved up, before
// CopyBitsToTexture, so it rides on the single screen-texture blit.

        // OPENRCT2MINI cut 40g: cut 40e magenta probe removed — we
        // confirmed (top-left magenta on device) that the texture-blit
        // path reaches the panel correctly. Cheap present-count log
        // remains, useful for distinguishing "renderer hung" from
        // "renderer presenting empty frames".
        static uint32_t s_presentCount = 0;
        if ((s_presentCount % 300) == 0)
        {
            // OPENRCT2MINI cut 41c: count non-zero bytes in _bits to know
            // whether anything is being painted into the X8 framebuffer.
            // Sample the first 65536 bytes (top of screen) which is enough
            // to distinguish "all-zero" from "has content". Also report
            // the open-window count.
            size_t sampleSize = static_cast<size_t>(_width) * _height;
            if (sampleSize > 65536) sampleSize = 65536;
            size_t nonZero = 0;
            // _bits is PaletteIndex (a strong typedef); reinterpret as raw
            // bytes for the all-zero check.
            const auto* rawBits = reinterpret_cast<const uint8_t*>(_bits);
            for (size_t i = 0; i < sampleSize; ++i)
                if (rawBits[i] != 0) ++nonZero;
            std::fprintf(
                stderr,
                "[OPENRCT2MINI] HWDDE present #%u — _bits %zu/%zu non-zero, windows=%zu\n",
                s_presentCount, nonZero, sampleSize, gWindowList.size());
            std::fflush(stderr);
        }
        s_presentCount++;

        SDL_RenderPresent(_sdlRenderer);
    }

    void CopyBitsToTexture(SDL_Texture* texture, PaletteIndex* src, int32_t width, int32_t height, const uint32_t* palette)
    {
        void* pixels;
        int32_t pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0)
        {
            // OPENRCT2MINI cut 41d: figure out actual bytes-per-pixel of the
            // locked texture rather than guessing via the (broken) padding
            // arithmetic the upstream code uses.
            uint32_t fmt = 0;
            SDL_QueryTexture(texture, &fmt, nullptr, nullptr, nullptr);
            int32_t bpp = SDL_BYTESPERPIXEL(fmt);
            if (bpp == 0) bpp = 4;  // defensive — should never happen
            const int32_t rowBytes = width * bpp;
            const int32_t padding = pitch - rowBytes;

            if (bpp == 4)
            {
                // ARGB8888 (or any 32-bit format SDL_MapRGB returns full
                // 32-bit values for). Fast path: one palette lookup per
                // pixel, write a uint32_t.
                if (padding == 0)
                {
                    uint32_t* dst = static_cast<uint32_t*>(pixels);
                    for (int32_t i = width * height; i > 0; i--)
                        *dst++ = palette[EnumValue(*src++)];
                }
                else
                {
                    auto* dstRow = static_cast<uint8_t*>(pixels);
                    for (int32_t y = 0; y < height; ++y, dstRow += pitch)
                    {
                        auto* dst = reinterpret_cast<uint32_t*>(dstRow);
                        for (int32_t x = 0; x < width; ++x)
                            *dst++ = palette[EnumValue(*src++)];
                    }
                }
            }
            else if (bpp == 2)
            {
                // RGB565 (or any 16-bit format). palette[idx] is already
                // the SDL-mapped 16-bit value in the low half of a uint32;
                // truncate it.
                auto* dstRow = static_cast<uint8_t*>(pixels);
                for (int32_t y = 0; y < height; ++y, dstRow += pitch)
                {
                    auto* dst = reinterpret_cast<uint16_t*>(dstRow);
                    for (int32_t x = 0; x < width; ++x)
                        *dst++ = static_cast<uint16_t>(palette[EnumValue(*src++)]);
                }
            }
            else if (bpp == 1)
            {
                // 8-bit indexed-style format. Take the low byte of the
                // SDL-mapped value (whatever that means for the format).
                auto* dstRow = static_cast<uint8_t*>(pixels);
                for (int32_t y = 0; y < height; ++y, dstRow += pitch)
                {
                    uint8_t* dst = dstRow;
                    for (int32_t x = 0; x < width; ++x)
                        *dst++ = static_cast<uint8_t>(palette[EnumValue(*src++)]);
                }
            }
            (void)padding;  // silence unused warning when all paths use pitch directly
            SDL_UnlockTexture(texture);
        }
    }

    uint32_t GetDirtyVisualTime(uint32_t x, uint32_t y)
    {
        uint32_t result = 0;
        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            result = _dirtyVisualsTime[i];
        }
        return result;
    }

    void SetDirtyVisualTime(uint32_t x, uint32_t y, uint32_t value)
    {
        const auto rows = _invalidationGrid.getRowCount();
        const auto columns = _invalidationGrid.getColumnCount();

        _dirtyVisualsTime.resize(rows * columns);

        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            _dirtyVisualsTime[i] = value;
        }
    }

    void RenderDirtyVisuals()
    {
        int windowX, windowY, renderX, renderY;
        SDL_GetWindowSize(_window, &windowX, &windowY);
        SDL_GetRendererOutputSize(_sdlRenderer, &renderX, &renderY);

        float scaleX = Config::Get().general.windowScale * renderX / static_cast<float>(windowX);
        float scaleY = Config::Get().general.windowScale * renderY / static_cast<float>(windowY);

        SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_BLEND);
        for (uint32_t y = 0; y < _invalidationGrid.getRowCount(); y++)
        {
            for (uint32_t x = 0; x < _invalidationGrid.getColumnCount(); x++)
            {
                const auto timeEnd = GetDirtyVisualTime(x, y);
                const auto timeLeft = gCurrentRealTimeTicks < timeEnd ? timeEnd - gCurrentRealTimeTicks : 0;
                if (timeLeft > 0)
                {
                    uint8_t alpha = timeLeft * kDirtyRegionAlpha / kDirtyVisualTime;
                    SDL_Rect ddRect;
                    ddRect.x = static_cast<int32_t>(x * _invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.y = static_cast<int32_t>(y * _invalidationGrid.getBlockHeight() * scaleY);
                    ddRect.w = static_cast<int32_t>(_invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.h = static_cast<int32_t>(_invalidationGrid.getBlockHeight() * scaleY);

                    SDL_SetRenderDrawColor(_sdlRenderer, 255, 255, 255, alpha);
                    SDL_RenderFillRect(_sdlRenderer, &ddRect);
                }
            }
        }
    }
};

std::unique_ptr<IDrawingEngine> Ui::CreateHardwareDisplayDrawingEngine(IUiContext& uiContext)
{
    return std::make_unique<HardwareDisplayDrawingEngine>(uiContext);
}
