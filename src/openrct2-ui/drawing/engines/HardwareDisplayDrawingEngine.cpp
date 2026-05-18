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
#include <openrct2/MiniDebug.h>  // OPENRCT2MINI revision 64 — gated debug logging
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
#include <openrct2/interface/WindowClasses.h> // OPENRCT2MINI: cursor-during-loading suppression
#include <openrct2/paint/Paint.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WindowManager.h>       // OPENRCT2MINI: cursor-during-loading suppression
#include <openrct2/world/MapSelection.h>     // OPENRCT2MINI grid-cursor-plan §12.1
#include <openrct2-ui/UiContext.h>           // OPENRCT2MINI cursor-selector-modal-plan §3.3
#include <openrct2-ui/input/InputManager.h>  // OPENRCT2MINI cursor-selector-modal-plan §3.3
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
    // OPENRCT2MINI revision 78: tightened back to 32×32 — the exact
    // sprite size — now that the save origin is anchored at the sprite's
    // top-left (state.position - hotspot) instead of being centred on
    // state.position. Earlier 64-px / 96-px caps existed to give the
    // centred-on-state save enough margin to cover any 32×32 sprite for
    // any hotspot; with sprite-relative anchoring the cap matches the
    // sprite footprint exactly. Buffer cost: 1 KB (vs 9 KB at 96×96).
    static constexpr int kCursorBackupCap = 32;
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
            LOG_WARNING("HW renderer unavailable (%s); falling back to SDL_RENDERER_SOFTWARE.", SDL_GetError());
            _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (_sdlRenderer == nullptr)
        {
            LOG_ERROR("FATAL: no SDL renderer available (%s). The SDL2 build can't drive this platform's panel. "
                      "On the Miyoo Mini this is the cue to integrate the XK9274/sdl2_miyoo vendor SDL2 port.",
                      SDL_GetError());
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
        // OPENRCT2MINI: suppress the cursor whenever a loading window is up.
        // The progress window (WindowClass::progressWindow, used by park
        // imports / object pack scans / etc.) is modal-feeling: input is
        // routed past it and the user can't usefully click anything. Drawing
        // the cursor on top of it produced visible trails when the user
        // dragged-via-D-pad and hit the loading window's title bar (the
        // X8 dirty-block engine doesn't repaint the loading window's
        // contents, so cursor pixels we composite over it stay forever).
        // Hiding the cursor outright makes the trails moot.
        auto* wm = OpenRCT2::Ui::GetWindowManager();
        const bool loadingWindowVisible
            = (wm != nullptr && wm->FindByClass(WindowClass::progressWindow) != nullptr);

        // OPENRCT2MINI cursor-selector-modal-plan §3.3 / CS-R4: hide
        // the software cursor when the focus selector owns input. The
        // ring is the user's visual target; drawing the mouse cursor
        // on top of it would be noise. Real mouse motion transitions
        // the state machine back to `mixed`, which re-enables the
        // draw path.
        //
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #9
        // — parked-cursor sprite): when the grid cursor is in parked
        // state (user is on the tool window in focus mode), keep the
        // software cursor sprite drawn as the visual "this is where
        // you'll be working when you engage" indicator. The cursor
        // sync code already parks _cursorState.position at the grid
        // tile's screen projection, so the sprite naturally lines up
        // with the parked tile. Cheap to re-enable since the sprite
        // path was already there.
        //
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-18
        // — placing-mode sprite): also keep the sprite drawn when
        // the grid cursor is actively being controlled (gridCursor
        // flag set, no parked flag). _uiContext.GetCursor() returns
        // the active tool's cursor (gCurrentToolId — e.g. Footpath /
        // Bulldozer / TreeDown) in this state, which makes the
        // sprite a useful "tool head" indicator riding on top of
        // the grid tile selection rectangle. Sync already runs in
        // placing mode because SelectorMode is active, so the
        // sprite naturally lands on the grid tile via the same
        // gMapSelectPositionA path used for parked mode.
        const bool selectorActive
            = OpenRCT2::Ui::GetInputManager().getSelectorMode()
                == OpenRCT2::Ui::InputManager::SelectorMode::active;
        const bool gridCursorParked
            = gMapSelectFlags.has(MapSelectFlag::gridCursorParked);
        const bool gridCursorActive
            = gMapSelectFlags.has(MapSelectFlag::gridCursor);
        const bool selectorOwnsScreen
            = selectorActive && !gridCursorParked && !gridCursorActive;

        if (_swCursor != nullptr && !loadingWindowVisible && !selectorOwnsScreen)
        {
            // Invalidate the previous cursor rect for next frame's repaint.
            if (_lastCursorRectValid)
            {
                Invalidate(_lastCursorRectL, _lastCursorRectT, _lastCursorRectR, _lastCursorRectB);
            }

            // OPENRCT2MINI revision 59: poll the cursor-style theme each frame.
            // Cheap (one int compare + fast return inside SetStyle on miss);
            // avoids needing a save-time hook from the Options window.
            _swCursor->SetStyle(Config::Get().interface.cursorStyle);

            CursorID cur = _uiContext.GetCursor();
            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-18
            // — tool-cursor in grid states): in placing / parked grid
            // cursor states, SelectorMode is `active`, which causes
            // MouseInput.cpp's ProcessMouseOver to return early without
            // ever calling SetCursor(static_cast<CursorID>(gCurrentToolId)).
            // The cursor identity therefore stays at whatever was last
            // set (typically Arrow from widget-focus navigation),
            // which makes the sprite an Arrow above the parked tile
            // instead of the tool's cursor (PathDown for Footpath,
            // Bulldozer for Clear Scenery, etc.). Mirror MouseInput's
            // viewport-hover behaviour here: when a tool is armed and
            // the grid cursor flags are live, use gCurrentToolId. This
            // also guarantees a non-Undefined cursor for the sprite
            // path so Composite always has a valid sprite to draw —
            // covers the "sprite not visible at all in placing mode"
            // case if _uiContext.GetCursor() happened to be Undefined
            // at the moment we enter a tool-armed grid state.
            if ((gridCursorActive || gridCursorParked)
                && OpenRCT2::gInputFlags.has(OpenRCT2::InputFlag::toolActive))
            {
                cur = static_cast<CursorID>(gCurrentToolId);
            }
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
                // restore them.
                //
                // OPENRCT2MINI revision 60: shifted the save origin from
                // -16 to -32 so the 64x64 rect fully covers any 32x32
                // cursor sprite regardless of hotspot. The earlier -16
                // worked for Arrow / HandPoint (small hotspots) but
                // failed for cursors with `hotspot.y = 31` like
                // PickerArrow (15, 31) and TreeDown / Bulldozer (6, 31)
                // — those composite from (state.y - 31) up, leaving
                // 15 px of cursor pixels above the saved rect, which
                // produced visible trails until the dirty-block path
                // eventually repainted them.
                const int32_t bitsW = static_cast<int32_t>(_width);
                const int32_t bitsH = static_cast<int32_t>(_height);
                // OPENRCT2MINI revision 78: anchor save at the sprite
                // top-left (state.position - hotspot). The save region is
                // exactly the sprite footprint, so kCursorBackupCap can
                // match the 32×32 sprite size with no margin needed.
                const auto [hsX, hsY] = _swCursor->GetActiveHotspot();
                const int32_t bx = state->position.x - hsX;
                const int32_t by = state->position.y - hsY;
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
        else if (_swCursor != nullptr && _lastCursorRectValid)
        {
            // OPENRCT2MINI: cursor was just suppressed (loadingWindowVisible
            // flipped from false to true). Invalidate the previous cursor
            // footprint one last time so the next paint overwrites the old
            // pixels — without this the cursor sprite remains visible until
            // some other window happens to dirty that block.
            Invalidate(_lastCursorRectL, _lastCursorRectT, _lastCursorRectR, _lastCursorRectB);
            _lastCursorRectValid = false;
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
            // OPENRCT2MINI: nonZero is consumed only by MINI_DBG_LOG
            // below; that macro compiles to nothing in release builds,
            // so under GCC 16 the variable looks unused. [[maybe_unused]]
            // silences -Werror=unused-but-set-variable.
            [[maybe_unused]] size_t nonZero = 0;
            // _bits is PaletteIndex (a strong typedef); reinterpret as raw
            // bytes for the all-zero check.
            const auto* rawBits = reinterpret_cast<const uint8_t*>(_bits);
            for (size_t i = 0; i < sampleSize; ++i)
                if (rawBits[i] != 0) ++nonZero;
            MINI_DBG_LOG("HWDDE present #%u — _bits %zu/%zu non-zero, windows=%zu\n",
                         s_presentCount, nonZero, sampleSize, gWindowList.size());
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
