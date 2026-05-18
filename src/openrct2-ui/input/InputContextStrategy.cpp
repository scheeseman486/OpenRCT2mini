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
#include "../interface/ViewportInteraction.h"
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
        gMapSelectType = orientation;
        setMapSelectRange(world);
        MapInvalidateTileFull(world);
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

    // ---- GridCursorModel ------------------------------------------------

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
            // Invalidate the previous tile so its highlight clears, then
            // push the new tile into the selection globals.
            if (prev.x != _position.x || prev.y != _position.y)
                MapInvalidateTileFull(prev.ToCoordsXY());
            WriteGridCursorSelection(_position, _orientation);
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

    void ToolContext::onActivate()
    {
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
            const auto seed = ViewportCentreTile();
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
            {
                if (seed)
                    grid->setPosition(*seed);
                WriteGridCursorSelection(grid->getPosition(), grid->getOrientation());
                if (!seed)
                    ScrollMainWindowIfCursorNearEdge(grid->getPosition());
                _wroteSelection = true;
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                if (seed)
                    edge->setPosition(*seed);
                WriteGridCursorSelection(edge->getPosition(), edge->getOrientation());
                if (!seed)
                    ScrollMainWindowIfCursorNearEdge(edge->getPosition());
                _wroteSelection = true;
            }
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
    Disposition ToolContext::onCancel()
    {
        TileCoordsXY pos{};
        if (auto* model = getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                pos = grid->getPosition();
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
                pos = edge->getPosition();
            else
                return Disposition::Consumed;
        }
        ViewportInteractionRightClickAtMapPos(pos.ToCoordsXY());
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
                    // (amendment 2026-05-17 #4 — user feedback):
                    // setFocus only mutates the InputManager
                    // fields; it does NOT trigger a window
                    // repaint. The focus ring is drawn during
                    // the per-window paint cycle, so without
                    // an explicit invalidation the ring stays
                    // unpainted until something else dirties
                    // the window (e.g. clock tick, hover-state
                    // change). Mirror the engage-direction's
                    // clearFocus + InvalidateByClass shape so
                    // the ring appears on the next frame.
                    windowMgr->InvalidateByClass(cls);
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
                ScrollMainWindowIfCursorNearEdge(grid->getPosition());
            }
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
            {
                edge->step(delta);
                ScrollMainWindowIfCursorNearEdge(edge->getPosition());
            }
        }
        return Disposition::Consumed;
    }

    void ToolContext::processFrame(uint32_t /*nowMs*/)
    {
        // §10.1 blink pump. The viewport repaints only invalidated
        // tiles, so without this the surface-paint hook's
        // `(ms / 500) & 1` gate runs at most once per WriteGridCursorSelection
        // call (i.e. on entry and on each step). Invalidating the
        // current cursor tile every frame keeps the paint pipeline
        // ticking the blink gate.
        if (!_wroteSelection)
            return;
        if (auto* model = getCursorModel(); model != nullptr)
        {
            if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                MapInvalidateTileFull(grid->getPosition().ToCoordsXY());
            else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
                MapInvalidateTileFull(edge->getPosition().ToCoordsXY());
        }
    }

    void ToolContext::onDeactivate()
    {
        if (_wroteSelection)
        {
            gMapSelectFlags.unset(MapSelectFlag::enable);
            gMapSelectFlags.unset(MapSelectFlag::gridCursor);
            _wroteSelection = false;
        }
        // Deactivate the cursor model so its step() side-effects stop
        // writing the paint globals after the tool context is gone.
        if (auto* model = getCursorModel(); model != nullptr)
            model->onDeactivate();
        // Restore whatever SelectorMode the user was in before tool
        // entry.
        GetInputManager().setSelectorMode(_savedSelectorMode);
    }
} // namespace OpenRCT2::Ui
