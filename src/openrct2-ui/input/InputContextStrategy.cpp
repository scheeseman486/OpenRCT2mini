/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI grid-cursor-plan §14.1 / Phase 3.E.0: out-of-line
// definitions for the grid cursor models, the step helper, and the
// shared ToolContext lifecycle hooks. The header keeps the bodies
// minimal so it can be included without dragging the full
// MapSelection / Window / Viewport include graph into every TU.

#include "InputContextStrategy.h"

#include "../UiContext.h"
#include "../interface/LandTool.h"
#include "../interface/ViewportInteraction.h"
#include "../windows/Windows.h"
#include "ShortcutIds.h"
#include "ShortcutManager.h"
#include "WidgetFocus.h"

#include <openrct2/Context.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/config/Config.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/MapLimits.h>
#include <openrct2/world/MapSelection.h>

#include <algorithm>
#include <optional>

namespace OpenRCT2::Ui
{
    // OPENRCT2MINI grid-cursor-plan §14.2 (amendment 2026-05-20):
    // Shift-modifier query for the inline ToolContext::onShortcut
    // dispatch in InputContextStrategy.h. Lives here so the header
    // doesn't have to pull in UiContext.h to reach GetInputManager().
    //
    // OPENRCT2MINI grid-cursor-plan §17 (2026-05-23): semantically
    // this is the "vertical placement / Z-adjust" modifier the user
    // holds to enter Z-adjust mode. kInterfaceShiftModifier is the
    // shortcut that drives it. NOT kInterfaceConstructionZLock —
    // that's the cursor-Z-lock-while-moving modifier, a different
    // feature.
    bool isShiftModifierHeldInTool()
    {
        return GetInputManager().isModifierKeyPressed(ModifierKey::shift);
    }

    // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1 (2026-05-24):
    // precision modifier query. Unlike kInterfaceShiftModifier (which
    // piggy-backs on ModifierKey::shift), kCursorPrecisionModifier is a
    // standalone held-only shortcut. We query its held state via the
    // ShortcutManager's getState() the same way kCursorFastModifier is
    // queried in UiContext::ProcessWorldCursor.
    bool isPrecisionModifierHeldInTool()
    {
        auto& sm = GetShortcutManager();
        if (auto* shortcut = sm.getShortcut(ShortcutId::kCursorPrecisionModifier))
        {
            return GetInputManager().getState(*shortcut);
        }
        return false;
    }

    // OPENRCT2MINI grid-cursor-plan §11.2 (2026-05-24): cursor.click
    // held-state query. Same shape as isPrecisionModifierHeldInTool,
    // just polling kCursorClick (the verb that PAD A maps to by default).
    bool isCursorClickHeldInTool()
    {
        auto& sm = GetShortcutManager();
        if (auto* shortcut = sm.getShortcut(ShortcutId::kCursorClick))
        {
            return GetInputManager().getState(*shortcut);
        }
        return false;
    }

    // OPENRCT2MINI grid-cursor-plan §14.4 (2026-05-20): dismiss an
    // active error popup (WindowClass::error from
    // windows/Error.cpp). The mouse path closes the popup on any
    // left-click via InputWidgetLeft (MouseInput.cpp:1202); the
    // grid-cursor path has no equivalent intercept because click
    // dispatch goes through the tool-context strategy, which acts
    // on the cursor's tile rather than any popup. Result: a place
    // action that errored leaves the red popup floating until its
    // 8-second auto-close timeout while the user's next action
    // press starts placing again.
    //
    // Bridge the gap by letting ToolContext::onShortcut consume the
    // next action press as a popup-dismiss when one exists. The
    // popup's red colour is its own visual "press something to
    // dismiss" cue, so no extra focus-ring overlay is needed; the
    // single-action interception is enough to make the popup
    // feel responsive to the gamepad user.
    bool closeToolErrorPopupIfShowing()
    {
        auto* wm = GetWindowManager();
        if (wm == nullptr)
            return false;
        if (wm->FindByClass(WindowClass::error) == nullptr)
            return false;
        wm->CloseByClass(WindowClass::error);
        // OPENRCT2MINI grid-cursor-plan §14.4 (2026-05-20 follow-up):
        // tool-side latches the mouse path would clear on the
        // implicit mouse-up after a failed click. Without these the
        // next grid-cursor press silently no-ops (provisional and
        // place both early-return on a sticky error flag). The
        // bridge no-ops when the tool window isn't open.
        Windows::WindowFootpathClearErrorLatch();
        return true;
    }

    // ---- helpers --------------------------------------------------------

    // OPENRCT2MINI grid-cursor-plan §7.1: push the cursor's tile to the
    // existing gMapSelectFlags / gMapSelectPositionA/B / gMapSelectType
    // globals that Paint.Surface.cpp:1093-1190 already consumes. Single-
    // tile selection: A == B == tile's world coords. Also sets
    // MapSelectFlag::gridCursor so the blink-overlay paint hook can
    // gate on it.
    //
    // Pattern mirrors WindowFootpathSetProvisionalPathAtPoint
    // (Footpath.cpp:1123-1143) — the canonical single-tile selection
    // write used by every mouse-driven tool:
    //   1. unset enableArrow (we are not pointing an arrow at this tile)
    //   2. set enable (turn the selection on)
    //   3. set gMapSelectType (selection shape — full / corner / edge)
    //   4. call setMapSelectRange(coords), which normalises A==B==coords
    // We also (Mini-specific) set MapSelectFlag::gridCursor so the
    // paint-time blink-gate engages, and invalidate the tile so the
    // selection redraws immediately rather than waiting for the next
    // MapSelection::invalidate() prev/current diff to notice the change.
    static void WriteGridCursorSelection(TileCoordsXY pos, MapSelectType orientation)
    {
        const auto world = pos.ToCoordsXY();
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        gMapSelectFlags.set(MapSelectFlag::enable);
        gMapSelectFlags.set(MapSelectFlag::gridCursor);
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17
        // #7 — blinking-parked-cursor): active write (from onStep /
        // onActivate while the tool context owns dispatch) clears
        // the parked flag, so the marker becomes solid again. The
        // parked flag is re-set in onDeactivate when the tool is
        // still armed (user toggled out to widget focus).
        gMapSelectFlags.unset(MapSelectFlag::gridCursorParked);
        gMapSelectType = orientation;
        setMapSelectRange(world);
        MapInvalidateTileFull(world);
    }

    // OPENRCT2MINI grid-cursor-plan §18.4.b (2026-05-24): multi-cell
    // selection write. Same flag setup as the single-tile path; the
    // difference is the use of the MapRange overload of
    // setMapSelectRange (MapSelection.h:81) and MapInvalidateRegion
    // (Map.h:146) for the brush footprint. MapSelection::invalidate's
    // built-in prev/curr A/B diff (MapSelection.cpp:82-88) already
    // handles the case where the previous selection was a different
    // rect — but we invalidate the new rect explicitly anyway so the
    // marker draws on the SAME frame as the write (matches the
    // single-tile path's same-frame invalidate behaviour).
    static void WriteGridCursorSelection(TileCoordsXY a, TileCoordsXY b, MapSelectType orientation)
    {
        const auto worldA = a.ToCoordsXY();
        const auto worldB = b.ToCoordsXY();
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        gMapSelectFlags.set(MapSelectFlag::enable);
        gMapSelectFlags.set(MapSelectFlag::gridCursor);
        gMapSelectFlags.unset(MapSelectFlag::gridCursorParked);
        gMapSelectType = orientation;
        setMapSelectRange(MapRange{ worldA, worldB });
        MapInvalidateRegion(worldA, worldB);
    }

    // OPENRCT2MINI grid-cursor-plan §6.1: margin-aware camera anchor.
    // Scroll the main window so the cursor stays inside the
    // playable rectangle (viewport minus the §6.1 margin) — but
    // ONLY when the cursor's projected centre falls within the
    // margin of any edge. Inside the margin we leave the camera
    // alone so the cursor visibly moves across the screen as the
    // player D-pads around, rather than appearing to be welded to
    // viewport centre.
    //
    // Earlier the code unconditionally called WindowScrollToLocation
    // on every step. WindowScrollToLocation centres the viewport on
    // the target, so the cursor never left the centre pixel; the
    // world panned under it instead, and the player perceived the
    // cursor as inert. The margin gate fixes that — short steps now
    // move the highlight on screen; only edge-approaching steps
    // recruit the camera.
    void ScrollMainWindowIfCursorNearEdge(TileCoordsXY pos)
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return;
        const auto& vp = *main->viewport;
        const auto worldXY = pos.ToCoordsXY();
        const int32_t z = TileElementHeight(worldXY);
        // Project the tile's centre to viewport-space pixel coords.
        // ContainsTile (Viewport.cpp:1103) uses the same formula —
        // tile centre + half-tile, then Translate3DTo2DWithZ. Stay
        // consistent with that so an "off-screen" verdict here
        // matches the visible-tile cull.
        const auto centreCoords = worldXY + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
        const auto screen = Translate3DTo2DWithZ(vp.rotation, CoordsXYZ{ centreCoords, z });
        const int32_t marginVp = vp.zoom.ApplyTo(DiscreteStep::kViewportMargin);
        const int32_t left = vp.viewPos.x + marginVp;
        const int32_t right = vp.viewPos.x + vp.ViewWidth() - marginVp;
        const int32_t top = vp.viewPos.y + marginVp;
        const int32_t bottom = vp.viewPos.y + vp.ViewHeight() - marginVp;
        const bool nearEdge = screen.x < left || screen.x > right || screen.y < top || screen.y > bottom;
        if (!nearEdge)
            return;
        WindowScrollToLocation(*main, CoordsXYZ{ worldXY, z });
    }

    // OPENRCT2MINI grid-cursor-plan §18.4.d (2026-05-24): rect-centre
    // overload for multi-cell brushes. Projects the centre of the A/B
    // rect to viewport coords (instead of the anchor tile's centre)
    // so the camera follows the brush footprint rather than just the
    // anchor — keeps the brush near viewport centre even at large
    // sizes. The Z is sampled at the rect centre, which is the right
    // anchor for the camera-follow heuristic (terrain height varies
    // across an NxN brush, but the centre tile's elevation is what
    // the user is targeting). Same edge-margin logic as the
    // single-tile overload; only the projected anchor differs.
    void ScrollMainWindowIfCursorNearEdge(TileCoordsXY a, TileCoordsXY b)
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return;
        const auto& vp = *main->viewport;
        // The centre of a tile rect in world coords:
        //   each tile is kCoordsXYStep (32) wide; centre = tileCoord*step + halfTile
        //   centre of [a..b] tile-rect = (a + b) * step / 2 + halfTile
        const auto worldCentre = CoordsXY{
            ((a.x + b.x) * kCoordsXYStep) / 2 + kCoordsXYHalfTile,
            ((a.y + b.y) * kCoordsXYStep) / 2 + kCoordsXYHalfTile,
        };
        const int32_t z = TileElementHeight(worldCentre);
        const auto screen = Translate3DTo2DWithZ(vp.rotation, CoordsXYZ{ worldCentre, z });
        const int32_t marginVp = vp.zoom.ApplyTo(DiscreteStep::kViewportMargin);
        const int32_t left = vp.viewPos.x + marginVp;
        const int32_t right = vp.viewPos.x + vp.ViewWidth() - marginVp;
        const int32_t top = vp.viewPos.y + marginVp;
        const int32_t bottom = vp.viewPos.y + vp.ViewHeight() - marginVp;
        const bool nearEdge = screen.x < left || screen.x > right || screen.y < top || screen.y > bottom;
        if (!nearEdge)
            return;
        WindowScrollToLocation(*main, CoordsXYZ{ worldCentre, z });
    }

    // OPENRCT2MINI grid-cursor-plan §6.1 (seed path): compute the
    // tile under the centre of the main viewport. Used on tool entry
    // to seed the grid cursor at where the player is already looking
    // — that way activating the tool doesn't yank the camera off to
    // wherever the cursor's stale `_position` lived (default {1,1}
    // is the SE map corner, which is rarely what the player is
    // viewing). Returns nullopt if the main window has no viewport
    // yet or the centre projects outside the playable map.
    //
    // The pick uses ScreenGetMapXY rather than just inverting the
    // viewport's centre via ViewportPosToMapPos so we get the same
    // terrain-height-aware coordinate the mouse picker produces.
    // Clamp to [1, kMaximumMapSizePractical - 1] matches the model's
    // step() clamp range (§6.3).
    static std::optional<TileCoordsXY> ViewportCentreTile()
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return std::nullopt;
        const auto& vp = *main->viewport;
        const ScreenCoordsXY centre{
            vp.pos.x + vp.width / 2,
            vp.pos.y + vp.height / 2,
        };
        const auto mapPos = ScreenGetMapXY(centre, nullptr);
        if (!mapPos)
            return std::nullopt;
        const auto tile = TileCoordsXY{ *mapPos };
        const int32_t lo = 1;
        const int32_t hi = kMaximumMapSizePractical - 1;
        if (tile.x < lo || tile.x > hi || tile.y < lo || tile.y > hi)
            return std::nullopt;
        return tile;
    }

    // OPENRCT2MINI grid-cursor-plan §18 (2026-05-23): is a tile
    // currently visible inside the main viewport? Used by onActivate's
    // resume path to decide whether to honour the preserved cursor
    // position (visible → resume) or re-seed at the viewport centre
    // (off-screen → user has scrolled away, treat as fresh engage).
    //
    // Projection logic mirrors ScrollMainWindowIfCursorNearEdge above
    // — tile centre + terrain Z → Translate3DTo2DWithZ → check
    // viewport bounds. The difference: ScrollMainWindowIfCursorNearEdge
    // uses a margin (treat "near edge" as off-screen so the camera
    // chases the cursor); we want pure visibility (no margin) because
    // we only want to re-seed when the tile is GENUINELY off-screen,
    // not just near the edge. Returns false on no viewport.
    static bool IsTileVisibleInViewport(TileCoordsXY pos)
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return false;
        const auto& vp = *main->viewport;
        const auto worldXY = pos.ToCoordsXY();
        const int32_t z = TileElementHeight(worldXY);
        const auto centreCoords = worldXY + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
        const auto screen = Translate3DTo2DWithZ(vp.rotation, CoordsXYZ{ centreCoords, z });
        const int32_t left = vp.viewPos.x;
        const int32_t right = vp.viewPos.x + vp.ViewWidth();
        const int32_t top = vp.viewPos.y;
        const int32_t bottom = vp.viewPos.y + vp.ViewHeight();
        return screen.x >= left && screen.x < right && screen.y >= top && screen.y < bottom;
    }

    // OPENRCT2MINI grid-cursor-plan §14.6 (2026-05-20): tile under
    // the OS / virtual cursor. ContextGetCursorPosition returns the
    // virtual cursor position (see grid-cursor-plan Cut 48 — the
    // platform call routes through the virtual cursor so camera pan
    // works from the gamepad-driven sprite). When the user engages
    // grid-cursor mode from cursor mode via Start, we seed the grid
    // cursor at the tile they were just hovering over — preserves
    // their visual context across the mode switch. Returns nullopt
    // if the cursor is over no tile (off-map, outside any viewport,
    // or out of the practical range).
    static std::optional<TileCoordsXY> CursorTile()
    {
        const auto cursor = ContextGetCursorPosition();
        const auto mapPos = ScreenGetMapXY(cursor, nullptr);
        if (!mapPos)
            return std::nullopt;
        const auto tile = TileCoordsXY{ *mapPos };
        const int32_t lo = 1;
        const int32_t hi = kMaximumMapSizePractical - 1;
        if (tile.x < lo || tile.x > hi || tile.y < lo || tile.y > hi)
            return std::nullopt;
        return tile;
    }

    // ---- GridCursorModel ------------------------------------------------

    // OPENRCT2MINI grid-cursor-plan §18.4.a (2026-05-24): brush-range
    // math per §18.2 with NW-biased centre for even sizes. A and B are
    // clamped INDEPENDENTLY to [1, kMaximumMapSizePractical-1], so a
    // brush near the map edge produces an asymmetric (smaller-than-
    // size) rect rather than walking the anchor away from the edge.
    // The five terraform game actions all accept MapRange and tolerate
    // smaller-than-requested ranges (verified during §18.A: action.cpp
    // wrappers compute centre = (A+B)/2 + 16 directly from A/B).
    std::pair<TileCoordsXY, TileCoordsXY> GridCursorModel::computeBrushRange(uint16_t size) const
    {
        const int32_t lo = 1;
        const int32_t hi = kMaximumMapSizePractical - 1;
        if (size <= 1)
        {
            // Fast path: single-tile rect = anchor clamped to playable
            // range. A == B. Matches the legacy single-tile behaviour
            // that callers see via the existing getPosition() path.
            const int32_t x = std::clamp<int32_t>(_position.x, lo, hi);
            const int32_t y = std::clamp<int32_t>(_position.y, lo, hi);
            const TileCoordsXY p{ x, y };
            return { p, p };
        }
        // §18.2 NW-biased centre:
        //   halfHi = size / 2;       (1 for size 2/3, 2 for size 4/5)
        //   halfLo = (size - 1) / 2; (0 for size 1/2, 1 for size 3/4)
        // size 2 → A=anchor, B=anchor+(1,1)            (NW-biased)
        // size 3 → A=anchor-(1,1), B=anchor+(1,1)      (centred)
        // size 5 → A=anchor-(2,2), B=anchor+(2,2)      (centred)
        const int32_t halfHi = static_cast<int32_t>(size) / 2;
        const int32_t halfLo = (static_cast<int32_t>(size) - 1) / 2;
        const int32_t ax = std::clamp<int32_t>(_position.x - halfLo, lo, hi);
        const int32_t ay = std::clamp<int32_t>(_position.y - halfLo, lo, hi);
        const int32_t bx = std::clamp<int32_t>(_position.x + halfHi, lo, hi);
        const int32_t by = std::clamp<int32_t>(_position.y + halfHi, lo, hi);
        return { TileCoordsXY{ ax, ay }, TileCoordsXY{ bx, by } };
    }

    // OPENRCT2MINI grid-cursor-plan §18.4.a (2026-05-24): brush-size
    // setter. Clamps to [1, UINT16_MAX] (no upper soft-limit — that's
    // the tool window's job via kLandToolMaximumSize). Re-emits the
    // selection if active so the change shows on the same frame the
    // user adjusts size.
    void GridCursorModel::setBrushSize(uint16_t size)
    {
        if (size < 1)
            size = 1;
        if (_brushSize == size)
            return;
        _brushSize = size;
        if (_active)
        {
            // Re-emit the selection at the new size. Existing
            // MapSelection::invalidate diffs the old A/B vs the new
            // A/B and invalidates the symmetric difference, so the
            // old brush highlight clears without us iterating tiles
            // here.
            if (_brushSize > 1)
            {
                const auto [a, b] = computeBrushRange(_brushSize);
                WriteGridCursorSelection(a, b, _orientation);
            }
            else
            {
                WriteGridCursorSelection(_position, _orientation);
            }
        }
    }

    TileCoordsXY GridCursorModel::step(TileCoordsXY delta)
    {
        // Map-edge clamping per §6.3: playable range is
        // [1, kMaximumMapSizePractical - 1]. kMaximumMapSizePractical
        // is fork-defined (Mini: 254, host: 998).
        const int32_t lo = 1;
        const int32_t hi = kMaximumMapSizePractical - 1;
        const auto prev = _position;
        _position.x = std::clamp<int32_t>(_position.x + delta.x, lo, hi);
        _position.y = std::clamp<int32_t>(_position.y + delta.y, lo, hi);
        if (_active)
        {
            // OPENRCT2MINI grid-cursor-plan §18.4.b/c (2026-05-24):
            // route through the size-aware writer when the owning tool
            // has dialled a multi-cell brush. MapSelection::invalidate
            // (MapSelection.cpp:82-88) diffs prev/curr A/B every frame
            // and invalidates the symmetric difference on change, so
            // stale highlights clear automatically without us iterating
            // the prev rect here. Single-tile path keeps the explicit
            // prev-tile invalidate as belt-and-braces (matches the
            // pre-§18 behaviour exactly).
            if (_brushSize > 1)
            {
                const auto [a, b] = computeBrushRange(_brushSize);
                WriteGridCursorSelection(a, b, _orientation);
            }
            else
            {
                // Invalidate the previous tile so its highlight clears, then
                // push the new tile into the selection globals.
                if (prev.x != _position.x || prev.y != _position.y)
                    MapInvalidateTileFull(prev.ToCoordsXY());
                WriteGridCursorSelection(_position, _orientation);
            }
        }
        return _position;
    }

    // ---- EdgeCursorModel ------------------------------------------------

    TileCoordsXY EdgeCursorModel::step(TileCoordsXY delta)
    {
        const int32_t lo = 1;
        const int32_t hi = kMaximumMapSizePractical - 1;
        const auto prev = _position;
        _position.x = std::clamp<int32_t>(_position.x + delta.x, lo, hi);
        _position.y = std::clamp<int32_t>(_position.y + delta.y, lo, hi);
        if (_active)
        {
            if (prev.x != _position.x || prev.y != _position.y)
                MapInvalidateTileFull(prev.ToCoordsXY());
            WriteGridCursorSelection(_position, _orientation);
        }
        return _position;
    }

    // ---- stepForDirection ----------------------------------------------

    // OPENRCT2MINI grid-cursor-plan §3.3: dpad direction → TileCoordsXY
    // delta. Encoding:
    //   compass mode: dpad 0/1/2/3 = N/E/S/W in *world* coords, then
    //                 rotated by the camera rotation so a constant
    //                 world-direction press follows the rotated camera.
    //   diagonal modes: dpad 0/1/2/3 = visually-up/right/down/left on
    //                   screen, with up biased NW (diagonalLeft) or
    //                   NE (diagonalRight). Each rotation tick shifts
    //                   the world mapping one step in the rotation
    //                   carousel.
    //
    // Returns a unit step (single tile). Callers apply fast-modifier
    // multipliers above this layer.
    TileCoordsXY stepForDirection(GridCursorMode mode, uint8_t rotation, ::Direction dpad)
    {
        const uint8_t d = dpad & 3;
        const uint8_t r = rotation & 3;

        // Canonical OpenRCT2 cardinal convention (Map.cpp:88,
        // TileDirectionDelta):
        //   N = (-1,  0)    E = ( 0, +1)    S = (+1,  0)    W = ( 0, -1)
        //
        // At rotation 0 the iso projection puts:
        //   N at the upper-right diamond edge,
        //   E at the lower-right,
        //   S at the lower-left,
        //   W at the upper-left.
        //
        // Compass mode = SCREEN-relative direction. dpad-up should
        // always make the cursor move toward the screen's upper-
        // right (the N diamond edge) regardless of camera rotation;
        // dpad-right toward screen lower-right (E); etc. To pull
        // this off we keep the screen direction constant and let the
        // WORLD direction rotate with the camera. The kCompass table
        // lists the BASE world direction for rotation 0; rotating
        // it by `(4 - r) & 3` (i.e. one CCW step per rotation tick)
        // walks the world direction around the carousel so the
        // visible diamond edge stays the same.
        //
        // Earlier this comment claimed "+y = north" and the kCompass
        // table mirrored that — both were wrong against the
        // canonical TileDirectionDelta layout, and the resulting
        // dispatch made dpad presses go the OPPOSITE screen
        // direction once the camera was at rotation 2 or 3 (caught
        // in user smoke-test at rot=3).
        TileCoordsXY base{};
        switch (mode)
        {
            case GridCursorMode::compass:
            default:
            {
                static const TileCoordsXY kCompass[4] = {
                    { -1,  0 },   // up    = N (upper-right diamond edge)
                    {  0,  1 },   // right = E (lower-right)
                    {  1,  0 },   // down  = S (lower-left)
                    {  0, -1 },   // left  = W (upper-left)
                };
                base = kCompass[d];
                return base.Rotate((4 - r) & 3);
            }
            case GridCursorMode::diagonalLeft:
            {
                // dpad-up = NW visually (top corner of the diamond,
                // biased to the W-side). NW world = N + W =
                // (-1, 0) + (0, -1) = (-1, -1). NE = N + E =
                // (-1, +1). SE = S + E = (+1, +1). SW = S + W =
                // (+1, -1). Diagonals also rotate-cancel so the
                // visual screen direction stays put.
                static const TileCoordsXY kDLeft[4] = {
                    { -1, -1 },   // up    = NW (top corner, W-side)
                    { -1,  1 },   // right = NE (right corner)
                    {  1,  1 },   // down  = SE (bottom corner, E-side)
                    {  1, -1 },   // left  = SW (left corner)
                };
                base = kDLeft[d];
                return base.Rotate((4 - r) & 3);
            }
            case GridCursorMode::diagonalRight:
            {
                // dpad-up = NE visually (top corner, biased to the
                // E-side). Diagonals rotate-cancel like compass.
                static const TileCoordsXY kDRight[4] = {
                    { -1,  1 },   // up    = NE (top corner, E-side)
                    {  1,  1 },   // right = SE (right corner)
                    {  1, -1 },   // down  = SW (bottom corner, W-side)
                    { -1, -1 },   // left  = NW (left corner)
                };
                base = kDRight[d];
                return base.Rotate((4 - r) & 3);
            }
        }
    }

    // ---- ToolContext lifecycle -----------------------------------------

    // OPENRCT2MINI grid-cursor-plan §18.3 / §18.4.e.1 (2026-05-24):
    // wrap each subclass's precisionSubsetForTool() with a uniform
    // size-knob gate. Tools that opt into gLandToolSize via
    // usesGLandToolSize() return SubsetType::none whenever the
    // user has dialled size > 1 — sub-tile picking isn't meaningful
    // on a multi-cell brush (line brushes were considered and
    // rejected per §18.7.3). Out-of-line so this TU owns the
    // <openrct2-ui/interface/LandTool.h> include that pulls in
    // gLandToolSize; the header stays light.
    SubsetType ToolContext::precisionSubset() const
    {
        if (usesGLandToolSize() && gLandToolSize > 1)
            return SubsetType::none;
        return precisionSubsetForTool();
    }

    void ToolContext::onActivate()
    {
        // OPENRCT2MINI grid-cursor-plan §8.5 (2026-05-25): clear any
        // carry-over directional repeat state from a prior context.
        // If the user crossed a context boundary with a direction
        // still held, we want the new context to require a fresh
        // press before the repeat clock starts — otherwise the new
        // context's first tick() would observe the held bit and
        // initial-delay would tick from "now", which feels wrong
        // (the new context never saw the initial press).
        _repeat.reset();

        // Pixel cursor hide / show — mirror Focus mode's lifecycle.
        // Save the current SelectorMode so onDeactivate can restore
        // it. On the Mini the user typically boots into
        // SelectorMode::active (selector ring, cursor hidden), and we
        // want to keep that on exit; on the host the user may be in
        // SelectorMode::hidden (cursor visible), and we want to keep
        // that too.
        auto& mgr = GetInputManager();
        _savedSelectorMode = mgr.getSelectorMode();
        // §7.3: hide the pixel cursor by switching to
        // SelectorMode::hidden... wait, that's backwards. Looking at
        // the SelectorMode comment in InputManager.h:
        //   active — selector ring visible, cursor hidden.
        //   hidden — ring hidden, cursor visible.
        // So to HIDE the pixel cursor we want SelectorMode::active.
        // The plan §7.3 says "hidden", but the field name is from the
        // selector's perspective (ring hidden) — keep the pixel
        // cursor off by going to active.
        mgr.setSelectorMode(InputManager::SelectorMode::active);
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17
        // #6 — position preservation): if we're resuming from a
        // user-initiated grid-mode exit (Start toggle), preserve
        // the grid cursor's tile so the user picks up where they
        // left off. The _resumeFromGridExit flag is set in
        // onDeactivate when the tool is still armed (vs. when the
        // tool was cancelled, where re-seeding on the next engage
        // is the desired behaviour).
        //
        // OPENRCT2MINI grid-cursor-plan §14.6 follow-up (2026-05-20):
        // if the user transitioned grid → focus → cursor → grid (i.e.
        // they exited grid mode, then wandered off into cursor mode
        // via the real/virtual mouse before coming back), the resume
        // semantic no longer fits — the user's most recent context
        // is the cursor's tile, not the grid cursor's last position.
        // Treat that path as a fresh engage so the seed picks up the
        // cursor tile (§14.6 below) and the Z plane resets. The
        // _savedSelectorMode field captures the mode at the top of
        // this onActivate, before the flip to active.
        const bool cameFromCursorMode = _savedSelectorMode == InputManager::SelectorMode::hidden;
        const bool resuming = _resumeFromGridExit && !cameFromCursorMode;
        _resumeFromGridExit = false;
        // §10: seed the grid cursor's initial paint state so the
        // highlight appears immediately on tool entry. Without this
        // the cursor model's _position is set but nothing pushes it
        // to the paint globals until the user's first step.
        if (auto* model = getCursorModel(); model != nullptr)
        {
            // Activate the model first so its step() side-effects
            // know to write the globals.
            model->onActivate();
            // §6.1 seed: snap the cursor to the tile under the
            // viewport centre on tool entry. This makes the cursor
            // appear *where the player is looking*, not at the
            // model's stale `_position` (default {1,1}, the SE map
            // corner). If the seed pick fails (no viewport / off-
            // map projection) we fall back to scrolling the camera
            // to whatever the model currently holds so the cursor
            // is at least visible.
            //
            // On resume (toggle-back-to-grid-mode), skip the
            // viewport-centre seed and re-emit the existing
            // position to the gMapSelect globals so the highlight
            // appears at the user's last grid tile.
            //
            // OPENRCT2MINI grid-cursor-plan §14.6 (2026-05-20):
            // when the user engaged grid mode from cursor mode
            // (_savedSelectorMode == hidden — they were driving the
            // real or virtual mouse), seed at the tile under the
            // cursor instead of the viewport centre. Preserves the
            // user's visual context across the mode switch: the
            // grid cursor appears where they were just looking, not
            // halfway across the viewport. Falls back to the
            // viewport-centre seed if the cursor isn't over a valid
            // tile (off-map, outside the viewport, etc.).
            std::optional<TileCoordsXY> seed;
            if (!resuming)
            {
                if (_savedSelectorMode == InputManager::SelectorMode::hidden)
                    seed = CursorTile();
                if (!seed)
                    seed = ViewportCentreTile();
            }
            // OPENRCT2MINI grid-cursor-plan §18 (2026-05-23): even on
            // the resume path, if the preserved cursor position has
            // scrolled off-screen (user dragged the camera far away
            // between engage cycles), fall back to a viewport-centre
            // re-seed so the cursor reappears where the player is
            // actually looking. Without this, the highlight lands
            // out of view and the user has to D-pad it back into
            // frame.
            if (resuming && seed.has_value() == false)
            {
                if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                {
                    if (!IsTileVisibleInViewport(grid->getPosition()))
                        seed = ViewportCentreTile();
                }
                else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
                {
                    if (!IsTileVisibleInViewport(edge->getPosition()))
                        seed = ViewportCentreTile();
                }
            }
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                if (seed)
                    grid->setPosition(*seed);
                // OPENRCT2MINI grid-cursor-plan §14.6 follow-up
                // (2026-05-20): fresh engage from cursor mode resets
                // Z to ground level. The Z plane reflects the user's
                // construction Z drag; it doesn't carry across a
                // mode round-trip where they've used the cursor in
                // between. Pure resume (focus → grid → focus → grid
                // with no cursor mode in between) keeps Z preserved
                // so the user can step back into bridge mode without
                // re-raising.
                if (cameFromCursorMode)
                    grid->setZ(0);
                // OPENRCT2MINI grid-cursor-plan §18.C follow-up
                // (2026-05-24): seed the model's orientation from the
                // tool's per-tool default. The model persists across
                // tool-context switches (Water → Land → Water reuses
                // the same model instance), and the precision picker
                // can also flip orientation mid-session — so the
                // tool-specific tint (Water's fullWater) must be
                // re-applied on every activate to wash out any state
                // from the previous tool. Default-returning tools
                // (Terrain / LandRights / etc.) get MapSelectType::full
                // which matches the legacy behaviour exactly.
                grid->setOrientation(defaultMapSelectType());
                // OPENRCT2MINI grid-cursor-plan §18.A follow-up
                // (2026-05-24, user report): sync brushSize from
                // gLandToolSize and route through the rect writer when
                // > 1. Without this, the resume path (tool → focus →
                // tool via Start press) wrote a single-tile selection
                // every time — the model's _brushSize persists across
                // activations, so the per-frame processFrame poll
                // (which is a no-op when size hasn't changed) couldn't
                // restore the rect. The fix: do the sync here too,
                // then mirror step()'s brushSize branch when calling
                // WriteGridCursorSelection. Tools that don't opt in
                // get size = 1 and the existing single-tile path.
                const uint16_t brushSize = usesGLandToolSize() ? gLandToolSize : 1;
                grid->setBrushSize(brushSize);
                if (grid->getBrushSize() > 1)
                {
                    const auto [a, b] = grid->computeBrushRange();
                    WriteGridCursorSelection(a, b, grid->getOrientation());
                }
                else
                {
                    WriteGridCursorSelection(grid->getPosition(), grid->getOrientation());
                }
                if (!seed)
                    ScrollMainWindowIfCursorNearEdge(grid->getPosition());
                _wroteSelection = true;
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                if (seed)
                    edge->setPosition(*seed);
                if (cameFromCursorMode)
                    edge->setZ(0);
                WriteGridCursorSelection(edge->getPosition(), edge->getOrientation());
                if (!seed)
                    ScrollMainWindowIfCursorNearEdge(edge->getPosition());
                _wroteSelection = true;
            }
        }
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17
        // #6 — engage-side off-by-one fix): invalidate the tool
        // window so the focus ring clears on the same frame the
        // active context flips to toolFootpath. Symmetric with the
        // onDeactivate-side fix: onActivate runs from the
        // strategy-transition block AFTER resolveActiveContext
        // promoted _activeContext to toolFootpath but BEFORE this
        // frame's Draw(). The invalidation dirties the window's
        // screen blocks for THIS paint, which now sees ctx ==
        // toolFootpath and drawFocusOutlineIfActive's first gate
        // returns early — ring disappears.
        if (gInputFlags.has(InputFlag::toolActive))
        {
            const auto cls = gCurrentToolWidget.windowClassification;
            if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                windowMgr->InvalidateByClass(cls);
            // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-18
            // — tool cursor identity on engage): push the tool's cursor
            // into the global cursor identity right at tool engagement.
            // MouseInput.cpp's ProcessMouseOver normally handles this on
            // viewport hover, but in grid cursor mode SelectorMode is
            // active and ProcessMouseOver returns early. Without this
            // push, the cursor identity stays at whatever the user had
            // before engaging the tool (Arrow from widget navigation,
            // typically), and the software cursor sprite (now drawn at
            // the parked / placing tile by HardwareDisplayDrawingEngine)
            // renders as that stale Arrow instead of the tool's actual
            // cursor — Footpath / Bulldozer / Tree etc.
            ContextSetCurrentCursor(static_cast<CursorID>(gCurrentToolId));
        }
        // OPENRCT2MINI grid-cursor-plan §18.4.e (2026-05-24): sync the
        // brush size from the shared gLandToolSize global into the
        // cursor model so the very next selection write picks up the
        // multi-cell footprint. Tools that don't opt in via
        // usesGLandToolSize() reset the model to size 1 — this
        // matters when switching from a size-aware tool (Land at 5)
        // to a tool that ignores size (Footpath); without the reset
        // the new tool would inherit the stale 5x5 brush.
        if (auto* model = dynamic_cast<GridCursorModel*>(getCursorModel()); model != nullptr)
        {
            const uint16_t size = usesGLandToolSize() ? gLandToolSize : 1;
            model->setBrushSize(size);
        }
    }

    // OPENRCT2MINI grid-cursor-plan §14.1: shared D-pad step body.
    // Lives on the ToolContext base so every tool subclass — Grid-
    // backed (Footpath / Scenery / LandRights / TileInspector /
    // RideConstruction) and Edge-backed (Terrain / Water) — steps
    // its cursor without each subclass having to reimplement the
    // same boilerplate. The earlier shape only had a step body on
    // FootpathContextImpl; Water and Land cursors displayed but
    // never moved on D-pad because EdgeToolBase inherited the no-op
    // base. Verb wiring (onPlace / onRaise / onLower / onCancel /
    // sub-tile precision) stays per-subclass.
    // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17):
    // dismiss / cancel out of grid-cursor mode. Drops the
    // _toolFocusSelected latch so resolveActiveContext stops
    // routing to this tool strategy on the next frame; the
    // strategy transition fires onDeactivate (clears the grid
    // selection flags, restores the saved SelectorMode, deactivates
    // the cursor model), and resolveActiveContext falls through to
    // widgetFocus on the still-armed tool window. The tool itself
    // (gInputFlags.toolActive) is NOT cancelled — the user can
    // re-engage by clicking a mode button or cycling back to the
    // tool's virtual viewport entry.
    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5 (Phase B):
    // default cancel verb for every ToolContext subclass. Mirrors the
    // mouse RMB short-press release path: ask the viewport-interaction
    // layer to dispatch the right-click action at the cursor's tile.
    // For Grid-cursor tools (Footpath / Scenery / etc.) that's the
    // delete-element path. For Edge-cursor tools (Terrain / Water)
    // those items are non-removable so the dispatch is a no-op,
    // matching the mouse path's behaviour. Subclasses with bespoke
    // cancel semantics override.
    //
    // OPENRCT2MINI grid-cursor-deletion-plan §3.6 (Phase C,
    // 2026-05-20): route through the new
    // ViewportInteractionRightClickAtGridCursor dispatcher instead
    // of the screen-pixel-reprojecting RightClickAtMapPos. The new
    // dispatcher walks the tile's element list directly, classifies
    // each into nine priority bands, and acts on the highest-
    // priority candidate — so sub-tile sceneries (one per quadrant)
    // are addressable deterministically, not "whichever one happens
    // to lie under the projected tile-centre pixel". For grid
    // cursors that have been lifted off the surface, the cursor's
    // raised Z offset is added on top of the tile's surface Z to
    // arm the Z-window filter inside the dispatcher (§3.4); when
    // the cursor sits at ground level (`getZ() == 0`) the Z-window
    // is disabled and every actionable element on the tile is a
    // candidate, matching the mouse-path semantic. Edge cursors
    // (Terrain / Water) pass cursorZ = surfaceZ so the dispatcher's
    // Z-window is off; Surface elements are never actionable so the
    // edge path remains a no-op.
    Disposition ToolContext::onCancel()
    {
        TileCoordsXY pos{};
        int32_t rawCursorZ = 0;
        if (auto* model = getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                pos = grid->getPosition();
                rawCursorZ = grid->getZ();
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                pos = edge->getPosition();
                rawCursorZ = edge->getZ();
            }
            else
                return Disposition::Consumed;
        }
        const auto centre = pos.ToCoordsXY() + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
        const int32_t cursorZ = TileElementHeight(centre) + rawCursorZ;
        ViewportInteractionRightClickAtGridCursor(pos, cursorZ);
        return Disposition::Consumed;
    }

    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5 (Phase C
    // follow-up 2026-05-17): mouse-source cancel — dispatch at the
    // OS pointer's screen tile (legacy mouse RMB short-press path),
    // NOT the grid cursor's model position. This is the path the
    // user gets when they're in a tool with the tool window focused
    // (or the viewport entry selected) and right-click on a
    // destroyable element with the mouse: the element under the
    // mouse is the one that should be destroyed, not whatever tile
    // the gamepad grid cursor happens to be on.
    Disposition ToolContext::onCancelAtScreen()
    {
        const auto pos = ContextGetCursorPosition();
        ViewportInteractionRightClick(pos);
        return Disposition::Consumed;
    }

    Disposition ToolContext::exitGridCursorMode()
    {
        auto& mgr = GetInputManager();
        mgr.setToolFocusSelected(
            false, InputManager::SelectorTransitionSource::virtualUserInput);
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17
        // — focus-ring redraw fix): explicitly land focus on the
        // tool window's first focusable widget so the widgetFocus
        // context has a target on the very next frame. Without
        // this, exiting grid mode via cycle-to-virtual-tool-entry
        // (which clears focus on entry) leaves the focus state
        // empty and the focus ring doesn't draw until the user
        // wakes it manually. gCurrentToolWidget.windowClassification
        // tells us which window owns the armed tool — that's the
        // window we want to return to.
        if (gInputFlags.has(InputFlag::toolActive))
        {
            const auto cls = gCurrentToolWidget.windowClassification;
            auto* windowMgr = GetWindowManager();
            if (windowMgr != nullptr)
            {
                if (auto* w = windowMgr->FindByClass(cls); w != nullptr)
                {
                    mgr.setFocus(cls, WidgetFocus::firstFocusable(*w));
                    // OPENRCT2MINI grid-cursor-plan §12.1
                    // (amendment 2026-05-17 #5 — diagnostic
                    // follow-up): the InvalidateByClass call
                    // that was here is now in
                    // ToolContext::onDeactivate. Rationale:
                    // exitGridCursorMode runs inside the same
                    // process() frame that processEvents
                    // dispatched the kInterfaceConfirm to us;
                    // _activeContext was already resolved at
                    // the TOP of process() and is still
                    // toolFootpath, and stays toolFootpath until
                    // the NEXT process() pass re-resolves.
                    // An InvalidateByClass fired here dirties
                    // the window's screen blocks for THIS
                    // frame's paint — but THIS frame's paint
                    // still sees ctx == toolFootpath, so
                    // drawFocusOutlineIfActive's gate-1 (ctx ==
                    // widgetFocus || osk) returns early and the
                    // ring isn't painted. NEXT frame the active
                    // context is widgetFocus, but no fresh
                    // invalidation fires (onDeactivate's
                    // setSelectorMode is a no-op when
                    // _savedSelectorMode == active == current,
                    // so its own invalidation hook doesn't
                    // run), and the tool window doesn't
                    // repaint at all. Net: ring stays
                    // unpainted until something else dirties
                    // the window.
                    //
                    // Moving the invalidation to onDeactivate
                    // gets the timing right — onDeactivate
                    // runs from process()'s strategy-transition
                    // block AFTER resolveActiveContext has
                    // flipped _activeContext to widgetFocus,
                    // but BEFORE that frame's paint. The
                    // InvalidateByClass there dirties the
                    // window's blocks for the SAME frame's
                    // paint, which now sees ctx == widgetFocus
                    // and the ring draws.
                }
            }
        }
        return Disposition::Consumed;
    }

    // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #2 —
    // user feedback): Back / interface.dismiss in grid-cursor mode now
    // closes the tool window outright. ToolCancel() drops the engine
    // tool latch (gInputFlags.toolActive → false); CloseByClass closes
    // the window. The toolActive false-edge in InputManager::process
    // clears _toolFocusSelected, and resolveActiveContext falls
    // through to widgetFocus (on another window) or world. Subclasses
    // can override for special teardown but the generic dispatch is
    // sufficient for every current tool — every tool window keys off
    // gCurrentToolWidget.windowClassification, and ToolCancel handles
    // the tool-side cleanup the per-window close hooks expect.
    Disposition ToolContext::onFinishTool()
    {
        const auto cls = gCurrentToolWidget.windowClassification;
        ToolCancel();
        if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
            windowMgr->CloseByClass(cls);
        return Disposition::Consumed;
    }

    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.4 (Phase C):
    // route kCursorCancel through ViewportInteractionRightClick at
    // the OS pointer's screen position — same call the legacy mouse
    // RMB short-press path made directly. The mouse-short-press
    // release at UiContext.cpp synthesises kCursorCancel into the
    // dispatcher; this handler is what receives it when the active
    // context is `world`. Tool contexts have their own onCancel
    // override that sources the tile from the grid cursor instead.
    Disposition WorldContext::onShortcut(std::string_view id, const InputEvent& e)
    {
        if (id == ShortcutId::kCursorCancel)
        {
            // §3.4 (Phase C follow-up 2026-05-17): mouse needs the
            // short-press disambiguation timer (held mouse button can
            // be a drag, not a delete). Defer mouse presses; the
            // held-state poll for cursor.cancel mouse bindings in
            // UiContext::ProcessWorldCursor synthesises a release
            // event after the < 500 ms gate passes. Non-mouse
            // bindings (gamepad / keyboard) fire on press
            // immediately because they have no tap-vs-drag
            // ambiguity.
            if (e.deviceKind == InputDeviceKind::mouse
                && e.state != InputEventState::release)
            {
                return Disposition::Consumed;
            }
            const auto pos = ContextGetCursorPosition();
            ViewportInteractionRightClick(pos);
            return Disposition::Consumed;
        }
        return Disposition::Passthrough;
    }

    Disposition ToolContext::onStep(::Direction dpad)
    {
        const auto modeRaw = OpenRCT2::Config::Get().interface.gridCursorMode;
        const GridCursorMode mode = (modeRaw <= 2)
            ? static_cast<GridCursorMode>(modeRaw)
            : GridCursorMode::compass;
        const uint8_t rot = OpenRCT2::GetCurrentRotation();
        const auto delta = stepForDirection(mode, rot, dpad);
        if (auto* model = getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                grid->step(delta);
                // OPENRCT2MINI grid-cursor-plan §18.4.d (2026-05-24):
                // when a multi-cell brush is active, scroll the camera
                // to keep the rect centre near viewport centre rather
                // than the anchor tile — otherwise a large brush near
                // a viewport edge would have half its footprint off-
                // screen.
                if (grid->getBrushSize() > 1)
                {
                    const auto [a, b] = grid->computeBrushRange();
                    ScrollMainWindowIfCursorNearEdge(a, b);
                }
                else
                {
                    ScrollMainWindowIfCursorNearEdge(grid->getPosition());
                }
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                edge->step(delta);
                ScrollMainWindowIfCursorNearEdge(edge->getPosition());
            }
        }
        return Disposition::Consumed;
    }

    // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1 (2026-05-24):
    // base implementation of the precision-modifier D-pad picker. Looks
    // up the chosen MapSelectType per the tool's precisionSubset() and
    // the D-pad direction, writes it to gridCursor()'s orientation, and
    // re-emits the selection so the surface paint hook reflects the
    // change immediately. SubsetType::none → no-op (subset doesn't
    // care about sub-tile). SubsetType::quadrants → no-op for now
    // (requires diagonal D-pad which the compass-only ship defers per
    // §3 banner; the user-spec answer 3 says quadrants are picked via
    // up+left / up+right / down+left / down+right diagonals — when the
    // diagonal mode is enabled the dpad value passed here will reflect
    // a diagonal direction and we can wire the quadrant lookup).
    // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1 (2026-05-24):
    // sample held state of all four kFocus* directions. Used by
    // onPrecisionDpad to detect diagonal chord inputs (e.g. up+right
    // for picking the NE edge / quadrant — per user 2026-05-24 spec
    // answer 3 "quadrants are up+left/up+right/down+left/down+right").
    // Returns the bitmask: bit 0 = up, bit 1 = right, bit 2 = down,
    // bit 3 = left.
    static uint8_t sampleFocusDpadHeldMask()
    {
        auto& sm = GetShortcutManager();
        auto& im = GetInputManager();
        uint8_t mask = 0;
        if (auto* s = sm.getShortcut(ShortcutId::kFocusUp); s != nullptr && im.getState(*s))
            mask |= 0x1;
        if (auto* s = sm.getShortcut(ShortcutId::kFocusRight); s != nullptr && im.getState(*s))
            mask |= 0x2;
        if (auto* s = sm.getShortcut(ShortcutId::kFocusDown); s != nullptr && im.getState(*s))
            mask |= 0x4;
        if (auto* s = sm.getShortcut(ShortcutId::kFocusLeft); s != nullptr && im.getState(*s))
            mask |= 0x8;
        return mask;
    }

    // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1 (2026-05-24):
    // map a 4-bit dpad-held mask to a 0..3 diagonal index if exactly
    // two adjacent cardinals are held. Returns -1 (sentinel via
    // optional) otherwise.
    //   diag 0 = up+right  (visually upper-right)
    //   diag 1 = right+down (visually lower-right)
    //   diag 2 = down+left  (visually lower-left)
    //   diag 3 = left+up    (visually upper-left)
    static std::optional<uint8_t> dpadMaskToDiagonal(uint8_t mask)
    {
        switch (mask)
        {
            case 0x3: return 0; // up+right
            case 0x6: return 1; // right+down
            case 0xC: return 2; // down+left
            case 0x9: return 3; // left+up
            default:  return std::nullopt;
        }
    }

    // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1
    // (rewritten 2026-05-24): screen-direction → sub-tile position.
    //
    // Approach: mirror stepForDirection's structure — a fixed table of
    // base WORLD offsets for rotation 0, then rotate by (4 - r) & 3
    // using CoordsXY::Rotate. Same rotation-handling pattern as the
    // already-correct cursor stepping; no enum arithmetic, no paint-
    // code reverse-engineering.
    //
    // The picker works by sampling a small world offset from the tile
    // centre in the direction the user pressed (screen-relative), then
    // reading the mod_x/mod_y of the resulting world coord to derive
    // a sub-tile index. The mod_x/mod_y logic mirrors the canonical
    // helpers ScreenPosToMapPos / ScreenGetMapXYSide (Viewport.cpp) —
    // duplicated here as 15 lines so we don't have to round-trip
    // through screen coords just to invoke that math.
    //
    // Screen vertex offsets at rotation 0 — these are the DIAGONAL
    // world directions (NW/NE/SE/SW), because in iso projection the
    // diamond's four vertices (visible TOP/RIGHT/BOTTOM/LEFT) lie at
    // the four world-diagonal corners of the tile (NW = visible TOP,
    // etc.). At rotation 0, screen-up points to the NW world corner
    // of the tile diamond (matches stepForDirection's kDLeft[0]
    // comment "top corner of the diamond").
    //
    // dpad d (0=up, 1=right, 2=down, 3=left) → kVertex[d]:
    //   up    → NW = (-1, -1)
    //   right → NE = (-1, +1)
    //   down  → SE = (+1, +1)
    //   left  → SW = (+1, -1)
    //
    // Screen side offsets at rotation 0 — these are the CARDINAL world
    // directions (N/E/S/W), because in iso projection the diamond's
    // four sides (upper-right / lower-right / lower-left / upper-left)
    // lie along the four world-cardinal sides of the tile (N world side
    // = upper-right diamond side, per stepForDirection's kCompass
    // comment "N at the upper-right diamond edge"). For the chord
    // diagonals we want the visible diamond SIDE, which corresponds to
    // a world CARDINAL direction.
    //
    // diag d (0=up+right, 1=right+down, 2=down+left, 3=left+up) →
    // kSide[d]:
    //   up+right (upper-right diamond side) → N = (-1, 0)
    //   right+down (lower-right side)        → E = ( 0, +1)
    //   down+left  (lower-left side)         → S = (+1, 0)
    //   left+up    (upper-left side)         → W = ( 0, -1)
    //
    // Both tables are rotated by `.Rotate((4 - r) & 3)` — identical
    // rotation transform to stepForDirection (compass/diagonal modes).
    Disposition ToolContext::onPrecisionDpad(::Direction dpad)
    {
        auto* model = getCursorModel();
        auto* grid = dynamic_cast<GridCursorModel*>(model);
        if (grid == nullptr)
            return Disposition::Consumed;

        const auto subset = precisionSubset();
        if (subset == SubsetType::none)
            return Disposition::Consumed;

        const uint8_t r = OpenRCT2::GetCurrentRotation() & 3;
        const uint8_t mask = sampleFocusDpadHeldMask();
        const auto maybeDiag = dpadMaskToDiagonal(mask);

        // Decide whether this gesture is a vertex pick (single
        // cardinal) or a side pick (diagonal chord). Drives both
        // which offset table we use AND which MapSelectType family.
        const bool wantSide = maybeDiag.has_value();

        // Pick the base world offset (rot 0) and rotate by camera.
        // Scaled by 8 world units so the sample point lands well
        // inside a sub-tile quadrant (each is 16x16) — far enough
        // from the tile centre that the mod-based discriminator
        // picks the right quadrant, near enough to stay inside the
        // current tile.
        constexpr int32_t kSampleStep = 8;
        static const ::CoordsXY kVertex[4] = {
            { -1, -1 }, // up    → NW (top vertex)
            { -1, +1 }, // right → NE (right vertex)
            { +1, +1 }, // down  → SE (bottom vertex)
            { +1, -1 }, // left  → SW (left vertex)
        };
        static const ::CoordsXY kSide[4] = {
            { -1,  0 }, // up+right    → N (upper-right side)
            {  0, +1 }, // right+down  → E (lower-right side)
            { +1,  0 }, // down+left   → S (lower-left side)
            {  0, -1 }, // left+up     → W (upper-left side)
        };
        const auto base = wantSide
            ? kSide[*maybeDiag & 3]
            : kVertex[static_cast<uint8_t>(dpad) & 3];
        const auto rotated = base.Rotate((4 - r) & 3);
        const auto offsetWorld = ::CoordsXY{ rotated.x * kSampleStep, rotated.y * kSampleStep };

        // Sample point = tile centre + offset. Then use the
        // mod_x/mod_y discriminator from ScreenPosToMapPos /
        // ScreenGetMapXYSide (Viewport.cpp). Sample lands within the
        // same tile, so the discriminator returns which sub-tile
        // position the user pointed at.
        const auto tileCentre = grid->getPosition().ToCoordsXY()
            + ::CoordsXY{ ::kCoordsXYHalfTile, ::kCoordsXYHalfTile };
        const auto sample = tileCentre + offsetWorld;
        const auto mod_x = sample.x & 0x1F;
        const auto mod_y = sample.y & 0x1F;

        MapSelectType chosen = grid->getOrientation();
        if (wantSide)
        {
            // ScreenGetMapXYSide convention: side index from
            // mod-quadrant — picks the world side nearest the
            // sample point. Maps directly to edge_d / quarter_d.
            uint8_t side;
            if (mod_x < mod_y)
                side = (mod_x + mod_y < 32) ? 0 : 1;
            else
                side = (mod_x + mod_y < 32) ? 3 : 2;

            switch (subset)
            {
                case SubsetType::corners: // corners-subset also accepts diagonals → edges
                case SubsetType::edges:
                    chosen = getMapSelectEdge(static_cast<::Direction>(side));
                    break;
                case SubsetType::quadrants:
                    chosen = getMapSelectQuarter(static_cast<::Direction>(side));
                    break;
                default:
                    return Disposition::Consumed;
            }
        }
        else
        {
            // ScreenPosToMapPos convention: direction index from
            // mod-quadrant — picks the world corner nearest the
            // sample point. Centre region (between sub-tiles) →
            // direction 4 = MapSelectType::full.
            int32_t direction;
            if (mod_x > 8 && mod_x < 24 && mod_y > 8 && mod_y < 24)
            {
                direction = 4;
            }
            else if (mod_x <= 16)
            {
                direction = (mod_y < 16) ? 2 : 3;
            }
            else
            {
                direction = (mod_y < 16) ? 1 : 0;
            }

            switch (subset)
            {
                case SubsetType::corners:
                    chosen = (direction == 4)
                        ? MapSelectType::full
                        : static_cast<MapSelectType>(direction);
                    break;
                case SubsetType::quadrants:
                    chosen = (direction == 4)
                        ? MapSelectType::full
                        : getMapSelectQuarter(static_cast<::Direction>(direction));
                    break;
                case SubsetType::edges:
                    // Edges subset doesn't accept cardinals (no
                    // sensible "nearest edge" mapping from a vertex
                    // position) — no-op.
                    return Disposition::Consumed;
                default:
                    return Disposition::Consumed;
            }
        }
        grid->setOrientation(chosen);
        WriteGridCursorSelection(grid->getPosition(), chosen);
        return Disposition::Consumed;
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §4 (Phase R, 2026-05-25):
    // head-follow sync. Snaps the GridCursorModel + MapSelect globals + camera
    // to the tile returned by getHeadTile(). Idempotent — safe to call multiple
    // times per frame.
    //
    // Replaces FootpathContextImpl's bespoke syncGridCursorToBridgeHead (was at
    // InputManager.cpp:1648-1663). The same flag setup used by WriteGridCursor-
    // Selection (§7.1), except headFollowPreservesArrow() determines whether
    // MapSelectFlag::enableArrow is left intact — bridgeBuild + RideConstruction
    // both render a direction-arrow at the head and need this set.
    void ToolContext::syncGridCursorToHead()
    {
        const auto head = getHeadTile();
        if (!head.has_value())
            return;
        if (auto* grid = dynamic_cast<GridCursorModel*>(getCursorModel()); grid != nullptr)
            grid->setPosition(*head);
        const auto world = head->ToCoordsXY();
        if (!headFollowPreservesArrow())
            gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        gMapSelectFlags.set(MapSelectFlag::enable);
        gMapSelectFlags.set(MapSelectFlag::gridCursor);
        gMapSelectFlags.unset(MapSelectFlag::gridCursorParked);
        gMapSelectType = MapSelectType::full;
        setMapSelectRange(world);
        MapInvalidateTileFull(world);
        ScrollMainWindowIfCursorNearEdge(*head);
    }

    void ToolContext::processFrame(uint32_t nowMs)
    {
        // OPENRCT2MINI grid-cursor-plan §18.4.e (2026-05-24): per-frame
        // brush-size sync. The shared gLandToolSize global can change
        // mid-session via the tool window's DEC/INC widgets (clicked
        // through widget focus mode), its numpad-OSK preview entry, or
        // the §18.D bindable size shortcuts — none of those paths run
        // through our context, so we poll here. GridCursorModel::set-
        // BrushSize is idempotent when the size is unchanged, so this
        // is cheap. When the tool doesn't opt in (usesGLandToolSize()
        // returns false), the model holds at 1.
        //
        // §18.5.1: when the size transitions 1 → >1 the cursor model's
        // _orientation may hold a stale corner/edge/quadrant from a
        // previous precision pick at size 1. Precision is gated off at
        // size > 1 (§18.3) so there's no way to clear it from the user
        // side, and the rect writer would happily paint with a stale
        // sub-tile gMapSelectType. Snap orientation back to the per-
        // tool default whole-tile type and re-emit the selection so the
        // brush renders cleanly. Single canonical place for the reset:
        // catches the widget DEC/INC path (via widget-focus roundtrip
        // then return), the numpad text-input path, and the new §18.D
        // shortcut path uniformly.
        if (auto* model = dynamic_cast<GridCursorModel*>(getCursorModel()); model != nullptr)
        {
            const uint16_t prevSize = model->getBrushSize();
            const uint16_t size = usesGLandToolSize() ? gLandToolSize : 1;
            // Reset orientation BEFORE setBrushSize so the size-driven
            // re-emit inside setBrushSize picks up the fresh orientation
            // in a single WriteGridCursorSelection call (no double-paint).
            if (prevSize == 1 && size > 1)
            {
                const auto defaultOrient = defaultMapSelectType();
                if (model->getOrientation() != defaultOrient)
                    model->setOrientation(defaultOrient);
            }
            model->setBrushSize(size);
        }

        // OPENRCT2MINI grid-cursor-plan §5 / Phase 3.F.0 step 1
        // (2026-05-24): precision-modifier press/release edge detection.
        // Sub-tile orientation gestures live here on the base so every
        // tool inherits.
        //   - Press edge (false → true): clear _precisionDpadPressed
        //     so a subsequent release with no D-pad activity is a
        //     tap-alone reset.
        //   - Release edge (true → false): if no dpad was pressed
        //     during the hold, reset orientation to MapSelectType::full
        //     and re-emit selection. If dpad WAS pressed, the
        //     orientation set by onPrecisionDpad stays as-is — the
        //     release "locks in" the choice. Either way clear the
        //     _precisionDpadPressed flag for the next gesture.
        const bool precisionNow = isPrecisionModifierHeldInTool();
        if (precisionNow != _precisionWasHeld)
        {
            if (precisionNow)
            {
                // Press edge.
                _precisionDpadPressed = false;
            }
            else
            {
                // Release edge.
                if (!_precisionDpadPressed)
                {
                    // Tap-alone — reset to the tool's default whole-tile
                    // orientation (MapSelectType::full for Terrain etc.,
                    // MapSelectType::fullWater for the Water tool — see
                    // §18.C). Hardcoded `full` would wipe the per-tool
                    // tint every time the user tapped the precision
                    // modifier; routing through defaultMapSelectType()
                    // preserves it.
                    if (auto* model = getCursorModel(); model != nullptr)
                    {
                        if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                        {
                            const auto defaultOrient = defaultMapSelectType();
                            if (grid->getOrientation() != defaultOrient)
                            {
                                grid->setOrientation(defaultOrient);
                                WriteGridCursorSelection(grid->getPosition(), defaultOrient);
                            }
                        }
                    }
                }
                _precisionDpadPressed = false;
            }
            _precisionWasHeld = precisionNow;
        }

        // OPENRCT2MINI grid-cursor-plan §8.5 (2026-05-25): directional
        // repeat-on-hold. Per-event onShortcut fires step 0 + calls
        // notePress() to anchor the press timestamp; this poll fires
        // subsequent steps every DiscreteStep::kRepeatIntervalMs after
        // DiscreteStep::kInitialDelayMs of continuous hold. SDL OS
        // auto-repeat is already filtered globally at UiContext.cpp:1041
        // (task #516), so the poll owns all repeats uniformly across
        // keyboard and gamepad.
        //
        // Slot convention: 0=kFocusUp (::Direction N), 1=kFocusRight
        // (::Direction E), 2=kFocusDown (::Direction S), 3=kFocusLeft
        // (::Direction W) — matches the noteAndStep lambda in
        // ToolContext::onShortcut. Repeats route through onStep() —
        // the same virtual the initial press dispatches to — so per-
        // tool overrides apply identically. Skip the poll if the
        // shift modifier is held: in that case the directions drive
        // raise/lower verbs (per onShortcut), not stepping, and we
        // don't want held shift+up to rapidly raise Z.
        if (!isShiftModifierHeldInTool())
        {
            auto& shortcutMgr = GetShortcutManager();
            const auto checkHeld = [&](std::string_view id) -> bool {
                if (const auto* s = shortcutMgr.getShortcut(id); s != nullptr)
                    return GetInputManager().getState(*s);
                return false;
            };
            const bool held[4] = {
                checkHeld(ShortcutId::kFocusUp),
                checkHeld(ShortcutId::kFocusRight),
                checkHeld(ShortcutId::kFocusDown),
                checkHeld(ShortcutId::kFocusLeft),
            };
            const uint8_t fire = _repeat.tick(nowMs, held);
            for (uint8_t i = 0; i < 4; i++)
                if (fire & static_cast<uint8_t>(1u << i))
                    onStep(static_cast<::Direction>(i));
        }
        else
        {
            // Shift held — clear repeat state so a fresh shift-release
            // followed by direction press starts a clean repeat clock.
            _repeat.reset();
        }

        // OPENRCT2MINI ride-construction-grid-cursor-plan §4 (Phase R,
        // 2026-05-25): head-follow poll. When a subclass overrides
        // getHeadTile() to return a head-following coord (Footpath
        // bridgeBuild → bridge head; RideConstruction Front/Back/Place/
        // EntranceExit → _currentTrackBegin), chase any changes since
        // last frame and fire onHeadTileChanged for subclass hooks
        // (refresh side-panel, etc.). The per-frame poll is necessary
        // because game-action callbacks (TrackPlaceAction, Footpath-
        // PlaceAction) update the head AFTER the verb's post-sync ran
        // — in-line syncs alone miss the async-callback timing. No-op
        // for tools that don't override getHeadTile() (base default
        // returns nullopt; _lastHead gets reset and we return).
        if (const auto head = getHeadTile(); head.has_value())
        {
            if (!_lastHead.has_value() || *_lastHead != *head)
            {
                _lastHead = *head;
                syncGridCursorToHead();
                onHeadTileChanged(*head);
            }
        }
        else
        {
            _lastHead.reset();
        }

        // §10.1 blink pump. The viewport repaints only invalidated
        // tiles, so without this the surface-paint hook's
        // `(ms / 500) & 1` gate runs at most once per WriteGridCursorSelection
        // call (i.e. on entry and on each step). Invalidating the
        // current cursor tile every frame keeps the paint pipeline
        // ticking the blink gate.
        //
        // OPENRCT2MINI grid-cursor-plan §18.A follow-up (2026-05-24,
        // user-reported dirty-draw): for multi-cell brushes
        // (brushSize > 1), the single anchor-tile invalidate isn't
        // enough — the N²-1 other tiles miss the per-frame tick and
        // their highlights go stale (rendered on top of stale
        // viewport pixels from before the cursor moved, producing
        // the dirty-draw artefact the user sees at sizes 3/5/etc.).
        // For Grid model + multi-cell, invalidate the whole brush
        // rect via MapInvalidateRegion — the same primitive
        // WriteGridCursorSelection uses on writes. Same shape as
        // tasks #591-593's dirty-draw fixes (window shadows,
        // playfield outline) — per-frame invalidation must cover
        // every screen block the overlay paints into.
        if (!_wroteSelection)
            return;
        if (auto* model = getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                if (grid->getBrushSize() > 1)
                {
                    const auto [a, b] = grid->computeBrushRange();
                    MapInvalidateRegion(a.ToCoordsXY(), b.ToCoordsXY());
                }
                else
                {
                    MapInvalidateTileFull(grid->getPosition().ToCoordsXY());
                }
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
                MapInvalidateTileFull(edge->getPosition().ToCoordsXY());
        }
    }

    void ToolContext::onDeactivate()
    {
        // OPENRCT2MINI grid-cursor-plan §8.5 (2026-05-25): clear
        // directional repeat state so a direction held during the
        // context transition doesn't carry into the next context's
        // first tick(). Symmetric with onActivate's reset.
        _repeat.reset();

        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #7
        // — blinking-parked-cursor): differentiate tool-still-armed
        // (parked) from tool-cancelled. When parked, KEEP the
        // gMapSelect marker visible but flip it into parked mode so
        // the surface paint blinks it. When cancelled (tool no
        // longer armed), clear everything as before.
        const bool stillArmed = gInputFlags.has(InputFlag::toolActive);
        if (_wroteSelection)
        {
            if (stillArmed)
            {
                // Stay visible, blink. Cursor model stays
                // active so the position is preserved across
                // the re-engage onActivate (per
                // _resumeFromGridExit).
                gMapSelectFlags.unset(MapSelectFlag::gridCursor);
                gMapSelectFlags.set(MapSelectFlag::gridCursorParked);
                // Don't touch enable. Don't reset _wroteSelection.
            }
            else
            {
                // OPENRCT2MINI grid-cursor-plan §18.A follow-up
                // (2026-05-24, user report): dirty the selection-rect
                // tiles before clearing the gridCursor flags. Same
                // shape as the realMouseMotion path in InputManager —
                // the per-frame blink pumps (active + parked) stop
                // firing the moment we clear gridCursor/gridCursorParked,
                // so without this explicit invalidation the rect tiles
                // keep their stale highlight pixels until something
                // else dirties them.
                if (gMapSelectPositionA != gMapSelectPositionB)
                    MapInvalidateRegion(gMapSelectPositionA, gMapSelectPositionB);
                else
                    MapInvalidateTileFull(gMapSelectPositionA);
                gMapSelectFlags.unset(MapSelectFlag::enable);
                gMapSelectFlags.unset(MapSelectFlag::gridCursor);
                gMapSelectFlags.unset(MapSelectFlag::gridCursorParked);
                _wroteSelection = false;
            }
        }
        // Deactivate the cursor model so its step() side-effects stop
        // writing the paint globals after the tool context is gone.
        if (auto* model = getCursorModel(); model != nullptr)
            model->onDeactivate();
        // Restore whatever SelectorMode the user was in before tool
        // entry.
        GetInputManager().setSelectorMode(_savedSelectorMode);
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17 #5
        // — off-by-one frame fix): invalidate the tool window here so
        // the focus ring repaints on the SAME frame that the active
        // context flips to widgetFocus. The InvalidateByClass that
        // used to live in exitGridCursorMode fired on the previous
        // frame, where the paint still saw the active context as
        // toolFootpath and drawFocusOutlineIfActive's first gate
        // (ctx == widgetFocus || osk) returned early. By the time
        // the next frame's paint ran, no fresh invalidation had
        // fired (setSelectorMode here is a no-op when
        // _savedSelectorMode == active and we're already active, so
        // its built-in invalidation hook doesn't trigger), and the
        // tool window's screen blocks weren't repainted at all.
        //
        // onDeactivate runs from InputManager::process()'s
        // strategy-transition block AFTER resolveActiveContext has
        // re-resolved _activeContext to widgetFocus, but BEFORE
        // that same frame's Draw(). The InvalidateByClass here
        // dirties the window's blocks for the SAME frame's paint,
        // which now sees ctx == widgetFocus and draws the ring.
        //
        // ALSO arms _resumeFromGridExit so the next onActivate
        // preserves the grid-cursor tile (toggle UX). Only when
        // the tool is still armed — if onDeactivate is firing
        // because the tool was cancelled, we want the next
        // tool engagement to re-seed at the viewport centre.
        if (gInputFlags.has(InputFlag::toolActive))
        {
            _resumeFromGridExit = true;
            const auto cls = gCurrentToolWidget.windowClassification;
            if (auto* windowMgr = GetWindowManager(); windowMgr != nullptr)
                windowMgr->InvalidateByClass(cls);
        }
    }
} // namespace OpenRCT2::Ui
