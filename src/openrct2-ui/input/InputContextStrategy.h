/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

// OPENRCT2MINI input-plan Track 3 / Phase 3.A: strategy skeleton.
//
// Two interfaces (IInputContext, ICursorModel) and one disposition enum.
// Phase 3.A is scope-controlled — these interfaces exist only as
// header-only contracts; the only concrete implementation in this phase
// is a do-nothing world stub that returns Passthrough for every shortcut.
// Phase 3.C lifts the actual cursor state out of UiContext.cpp and into
// PixelCursorModel; until then, the cursor model is a no-op shape.
//
// See input-plan.md §5.1 "Critical design constraints from the review"
// and modal-input-plan.md "Context as strategy" for the full design.

#include "InputManager.h"  // for InputContext, InputEvent
#include "ShortcutIds.h"   // for ToolContext shortcut dispatch

#include <openrct2/world/Location.hpp>  // TileCoordsXY, Direction
#include <openrct2/world/MapSelection.h> // MapSelectType

#include <cstdint>
#include <string_view>

namespace OpenRCT2::Ui
{
    // OPENRCT2MINI grid-cursor-plan §3: D-pad-to-tile-step mapping mode.
    // Stored on Config::Interface::gridCursorMode; consulted by the
    // stepForDirection helper each step. Default `compass` (world-
    // relative cardinal directions).
    enum class GridCursorMode : uint8_t
    {
        compass = 0,
        diagonalLeft = 1,
        diagonalRight = 2,
    };

    // OPENRCT2MINI grid-cursor-plan §4.1: each tool advertises which
    // subset of the 13 sub-tile positions its precision modifier
    // cycles through. The grid cursor consults this when the
    // precision modifier is held.
    enum class SubsetType : uint8_t
    {
        none = 0,
        corners = 1,
        edges = 2,
        quadrants = 3,
    };

    // OPENRCT2MINI grid-cursor-plan §8.2: shared discrete-step rate
    // constants. The grid cursor's per-frame held-state poll fires
    // the first step immediately on press; subsequent steps fire
    // kRepeatIntervalMs apart after the kInitialDelayMs hold. Future
    // focus-mode list-step work can consume the same constants if
    // desired.
    namespace DiscreteStep
    {
        constexpr uint32_t kInitialDelayMs = 200;
        constexpr uint32_t kRepeatIntervalMs = 80;
        // §6.1: viewport inset margin in pixels. If the grid cursor's
        // screen projection falls within this distance of any
        // viewport edge after a step, scroll the camera so the
        // cursor sits at the margin instead of off-screen.
        constexpr int32_t kViewportMargin = 64;
        // §11.3 / §8.4: fast-modifier multiplier. When
        // kCursorFastModifier is held (and precision is not),
        // multiply the step granularity by this factor.
        constexpr int32_t kFastMultiplier = 4;
    } // namespace DiscreteStep

    // OPENRCT2MINI grid-cursor-plan §3.3 / §14.1: D-pad direction →
    // TileCoordsXY delta under the active GridCursorMode and the
    // camera's current rotation (0..3). dpad uses the Direction enum
    // (uint8_t 0..3) with the canonical N/E/S/W = 0/1/2/3 mapping
    // used by MapSelectType helpers. Result is the world-tile delta
    // to apply on a single step.
    TileCoordsXY stepForDirection(GridCursorMode mode, uint8_t rotation, ::Direction dpad);

    // OPENRCT2MINI grid-cursor-plan §6.1: scroll the main viewport
    // ONLY when the cursor's projected centre is within
    // DiscreteStep::kViewportMargin of any edge. Inside that margin
    // the camera is left alone so the user sees the highlight move
    // across the screen. Each tool's onStep calls this after the
    // model step so edge-approaching D-pad presses recruit a pan
    // without every step glueing the cursor to viewport centre.
    void ScrollMainWindowIfCursorNearEdge(TileCoordsXY pos);
    // What the active strategy wants the dispatcher to do with this
    // shortcut event:
    //   - Passthrough: shortcut's action lambda fires as normal.
    //   - Consumed:    strategy handled it; suppress the lambda.
    //
    // Default for almost every (context, shortcut_id) pair is
    // Passthrough. WorldContext returns Passthrough for everything.
    // Modal contexts return Consumed only for the IDs they semantically
    // own (e.g. OskContext consumes cursor.* so they don't move the
    // world cursor while the OSK is up).
    enum class Disposition : uint8_t
    {
        Passthrough = 0,
        Consumed = 1,
    };

    // Cursor model interface. Each context picks its cursor model at
    // construction; the model owns the cursor unit (pixel for the world
    // free cursor, tile for grid-aligned construction, character for
    // text caret) and the analog/D-pad-to-unit plumbing.
    //
    // Phase 3.A defines the contract only — every method is a no-op
    // default so the WorldContextStub doesn't need to override
    // anything. Phase 3.C lifts the real PixelCursorModel state out
    // of UiContext.cpp and into a concrete implementation.
    //
    // Render integration uses two phases to land in the correct depth
    // layer:
    //   - renderInViewport runs inside PaintSession (tile-aligned
    //     overlays draw under UI windows).
    //   - renderOnFramebuffer runs in HardwareDisplayDrawingEngine
    //     after windows are composited (pixel-space cursors draw
    //     above UI).
    // Phase 3.A leaves the render hooks as no-op virtuals — the
    // engine isn't wired to call them yet. They're declared now so
    // Phase 3.C doesn't have to introduce a binary-breaking vtable
    // change.
    class ICursorModel
    {
    public:
        virtual ~ICursorModel() = default;

        virtual void onActivate() {}
        virtual void onDeactivate() {}

        // Discrete step (D-pad press, single-tap). Model owns the unit.
        virtual void onDiscreteStep(int8_t /*dx*/, int8_t /*dy*/) {}

        // Continuous motion (analog stick, held D-pad). Inputs are
        // post-deadzone normalised; the shared analog plumbing on
        // InputManager owns the deadzone + hysteresis so each model
        // doesn't re-implement it.
        virtual void onContinuousMotion(float /*ax*/, float /*ay*/, uint32_t /*dtMs*/) {}

        // Render hooks — Phase 3.C wires the engine to call these.
        // Each model overrides whichever phase(s) it needs.
        // virtual void renderInViewport(PaintSession&)    {}
        // virtual void renderOnFramebuffer(/*FB ctx*/)    {}
    };

    // Input context strategy. The ContextRegistry on InputManager owns
    // one instance per InputContext enum value. The currently-active
    // strategy is picked per-frame by resolveActiveContext (window-poll
    // model — Track 3 explicitly keeps that data-flow direction rather
    // than introducing a push/pop stack).
    //
    // Activation lifecycle: when _activeContext changes between frames,
    // the outgoing strategy's onDeactivate runs, then the incoming
    // strategy's onActivate. The lifecycle is the place where a context
    // synthesises a release event for any of its presses that are still
    // held — without that, a mid-press context swap leaves a stranded
    // press in the held-set that the new context will mishandle on
    // release. Phase 3.A's stub doesn't need this; Phase 3.B implements
    // it for the real modal contexts.
    class IInputContext
    {
    public:
        virtual ~IInputContext() = default;

        // Which enum value this strategy serves. Used by debug logging
        // and assert-against-mismatch checks. Required override.
        virtual InputContext getId() const = 0;

        virtual void onActivate() {}
        virtual void onDeactivate() {}

        // Called once per frame after resolveActiveContext picks the
        // active strategy. Owns per-frame held-state polls and edge
        // synthesis for shortcuts this context interprets specially.
        // Phase 3.A stub: empty.
        virtual void processFrame(uint32_t /*nowMs*/) {}

        // Routed shortcut. Called by InputManager::routeToContext before
        // the shortcut's action lambda fires. Return Consumed to
        // suppress the lambda; Passthrough lets it fire normally.
        // Default returns Passthrough — every context that doesn't
        // override stays out of the way.
        virtual Disposition onShortcut(std::string_view /*id*/, const InputEvent& /*e*/)
        {
            return Disposition::Passthrough;
        }

        // Direct keyboard-event hook. Called from InputManager::process
        // ahead of shortcut dispatch when the active context is this
        // strategy. Side-effect only — the event continues to flow
        // through shortcut dispatch regardless of what this hook does
        // (the per-context allow-list filter handles suppression). Used
        // by TextInputContext / LoadSaveContext / ConsoleContext to
        // route non-character keys (arrow keys, page up/down, etc.) to
        // their respective Windows::WindowText*InputKey handlers without
        // a switch-on-context-enum in InputManager itself.
        //
        // Phase 3.B absorbs the `// TODO: replace with event` keyboard
        // routing block this hook replaces.
        virtual void onKeyEvent(const InputEvent& /*e*/) {}

        // Modal dismiss / confirm semantics. Today these route through
        // the ModalHooks token-stack (InputManager.h:151-168); Phase 3.B
        // migrates them onto the strategy. Phase 3.A doesn't yet call
        // these — defaults return false so anything that does will fall
        // back to the existing hook stack.
        virtual bool onConfirm() { return false; }
        virtual bool onDismiss() { return false; }

        // Cursor model accessor. Phase 3.A stub returns nullptr.
        // Phase 3.C wires the real models through.
        virtual ICursorModel* getCursorModel() { return nullptr; }
    };

    // ---- Phase 3.C: Concrete cursor model stubs -------------------------
    //
    // Four concrete models, one per planned cursor unit. Phase 3.C lands
    // the abstraction (the classes exist; WorldContext composes a Pixel-
    // CursorModel) but defers the actual state migration — the floating-
    // point virtual cursor state (`_vcursorX/Y`, `_vcursorLastIntX/Y`,
    // `_vcursorLastTickMs`) still lives in UiContext.cpp, and the models
    // are functionally pass-through placeholders. The state lift happens
    // in a later phase, most likely when Phase 3.E forces coexistence
    // with a real GridCursorModel.
    //
    // The four models compile-link cleanly and inherit ICursorModel's
    // default no-op virtuals. Contexts can already pick one via
    // getCursorModel() — the abstraction is wired; the bodies are
    // intentionally empty.

    // Pixel-space free cursor. The current world cursor model — owns
    // the floating-point cursor that PixelCursor synthesises mouse
    // events from. Render layer: framebuffer (composite phase, above UI).
    class PixelCursorModel final : public ICursorModel
    {
        // Phase 3.C stub: no state. The actual `_vcursorX/Y` and
        // friends still live in UiContext.cpp; lifting them here is a
        // later phase that needs to also rewire mouse synthesis,
        // SDL_WarpMouseInWindow, software-cursor save/restore, and the
        // viewport auto-scroll. Doing it standalone risks bugs in three
        // subsystems at once; doing it alongside Phase 3.E (when
        // GridCursorModel needs to coexist) lets us validate both
        // models together.
    };

    // OPENRCT2MINI grid-cursor-plan §14.1 / Phase 3.E.0: tile-aligned
    // cursor. Owns a TileCoordsXY position, a sub-tile orientation
    // (MapSelectType — full / corner / edge / quadrant), a Z level,
    // and a per-direction press-tracking struct for the per-frame
    // held-state poll. Rendering is delegated to the existing
    // gMapSelectFlags / gMapSelectType / gMapSelectPositionA/B globals
    // (see Paint.Surface.cpp:1093-1190); this model writes those
    // globals when active and clears them on deactivate.
    class GridCursorModel final : public ICursorModel
    {
    public:
        // Per-D-pad-direction state for the per-frame held-state poll.
        // pressStartMs = timestamp of first step in this hold burst;
        // lastStepMs = timestamp of the last step we fired. Both zero
        // when no press is currently held.
        struct HoldState
        {
            uint32_t pressStartMs{ 0 };
            uint32_t lastStepMs{ 0 };
        };

        GridCursorModel() = default;

        void onActivate() override
        {
            _active = true;
            for (auto& s : _holdState)
                s = HoldState{};
        }
        void onDeactivate() override
        {
            _active = false;
            for (auto& s : _holdState)
                s = HoldState{};
        }

        bool isActive() const { return _active; }

        TileCoordsXY getPosition() const { return _position; }
        void setPosition(TileCoordsXY pos) { _position = pos; }

        int32_t getZ() const { return _z; }
        void setZ(int32_t z) { _z = z; }
        void raiseZ(int32_t step) { _z += step; }
        void lowerZ(int32_t step)
        {
            _z -= step;
            if (_z < 0)
                _z = 0;
        }

        MapSelectType getOrientation() const { return _orientation; }
        void setOrientation(MapSelectType o) { _orientation = o; }

        // Step the cursor by a TileCoordsXY delta. Clamps to the
        // playable map range. Returns the new position.
        TileCoordsXY step(TileCoordsXY delta);

        HoldState& holdState(uint8_t dir)
        {
            return _holdState[dir & 3];
        }
        const HoldState& holdState(uint8_t dir) const
        {
            return _holdState[dir & 3];
        }

    private:
        bool _active{ false };
        TileCoordsXY _position{ 1, 1 };
        // §6 sub-tile persistence: full-tile by default; precision
        // modifier shifts to a specific sub-tile and persists across
        // tile steps (Open Question 6 = persist).
        MapSelectType _orientation{ MapSelectType::full };
        int32_t _z{ 0 };
        HoldState _holdState[4]{};
    };

    // OPENRCT2MINI grid-cursor-plan §14.1: edge-tile cursor. Same
    // shape as GridCursorModel but defaults the sub-tile orientation
    // to corner0 (Terrain) — subclasses / consumers override via
    // setOrientation. Used by Terrain / Water once Phase 3.F lands.
    class EdgeCursorModel final : public ICursorModel
    {
    public:
        EdgeCursorModel() = default;

        void onActivate() override
        {
            _active = true;
        }
        void onDeactivate() override
        {
            _active = false;
        }

        bool isActive() const { return _active; }

        TileCoordsXY getPosition() const { return _position; }
        void setPosition(TileCoordsXY pos) { _position = pos; }

        int32_t getZ() const { return _z; }
        void setZ(int32_t z) { _z = z; }
        void raiseZ(int32_t step) { _z += step; }
        void lowerZ(int32_t step)
        {
            _z -= step;
            if (_z < 0)
                _z = 0;
        }

        MapSelectType getOrientation() const { return _orientation; }
        void setOrientation(MapSelectType o) { _orientation = o; }

        TileCoordsXY step(TileCoordsXY delta);

    private:
        bool _active{ false };
        TileCoordsXY _position{ 1, 1 };
        MapSelectType _orientation{ MapSelectType::corner0 };
        int32_t _z{ 0 };
    };

    // Text caret. Line + column within a textbox's content. D-pad
    // left/right step characters, up/down step lines. Used by
    // (legacy — was named TextCaret for the caret-browse skeleton;
    // the focus-mode-plan rebuild may replace this with a different
    // cursor model when Phase F.3 lands). Render layer: none —
    // the caret position is drawn by the textbox widget; the model
    // just updates the index the widget reads.
    class TextCaretModel final : public ICursorModel
    {
        // Phase 3.C: empty stub.
    };

    // ---- World context ---------------------------------------------------
    //
    // The fallback strategy. Used when no other context is registered
    // for the active enum slot, or when resolveActiveContext returns
    // InputContext::world (no modal / tool open).
    //
    // Phase 3.A introduced this as `WorldContextStub` with a single
    // getId() override. Phase 3.C promotes it to a real WorldContext
    // that composes a PixelCursorModel and exposes it via the
    // ICursorModel hook. Behaviour is still byte-identical to the pre-
    // refactor state — the cursor model itself is a stub and the
    // pixel-cursor logic still runs in UiContext.cpp. The point of the
    // promotion is that future code can ask the active context for its
    // cursor model and get back the right type for free-pixel world
    // mode without per-context branching.
    class WorldContext final : public IInputContext
    {
        PixelCursorModel _cursor;

    public:
        InputContext getId() const override
        {
            return InputContext::world;
        }

        ICursorModel* getCursorModel() override
        {
            return &_cursor;
        }

        // OPENRCT2MINI cursor-cancel-tile-action-plan §3.4 (Phase C):
        // route kCursorCancel through the same right-click tile-action
        // dispatcher the mouse RMB short-press already used. Position
        // source is the OS pointer's screen coord (existing
        // behaviour), so the world-cursor right-click semantic is
        // preserved byte-identically — the synthetic-fire from the
        // mouse short-press release path lands here. Defined out-of-
        // line so the implementation can include ViewportInteraction.h
        // without forcing it on every consumer of this header.
        Disposition onShortcut(std::string_view id, const InputEvent& e) override;
    };

    // Backwards-compatibility alias for the Phase 3.A name. Anything
    // that still references WorldContextStub gets the new WorldContext.
    // Drop once Phase 3.A's commit history is no longer being navigated.
    using WorldContextStub = WorldContext;

    // ---- Tool contexts (Phase 3.E and later) ----------------------------
    //
    // Base class for all construction tool contexts (Footpath, Track,
    // Scenery, Terrain, Water, LandRights, TileInspector). Each tool
    // subclass overrides the verb hooks it cares about and inherits the
    // common shortcut-dispatch routing in onShortcut.
    //
    // Verbs map to the canonical construction actions:
    //   onPlace   — A button / cursor.click → place the current tool at
    //               the GridCursor's tile.
    //   onCancel  — B button / cursor.cancel → cancel the tool / close
    //               the tool window.
    //   onRotate  — kInterfaceRotateConstruction → rotate the placement
    //               orientation 90° clockwise.
    //   onRaise   — Z+ → raise the placement Z level one step.
    //   onLower   — Z- → lower the placement Z level one step.
    //
    // Phase 3.E ships these as no-op virtuals. Concrete tool contexts
    // wire each verb to its respective GameAction (FootpathPlaceAction,
    // TrackPlaceAction, etc.) — that's the "real" Phase 3.E coding work
    // that needs to land before the tool is meaningfully gamepad-
    // driveable. The skeleton makes it visible where the work belongs;
    // doesn't ship the work itself.
    //
    // ToolContext owns a GridCursorModel by default. Tool subclasses
    // that want a different cursor (e.g. TerrainContext using
    // EdgeCursorModel) override getCursorModel().
    class ToolContext : public IInputContext
    {
        GridCursorModel _grid;
        // OPENRCT2MINI grid-cursor-plan §7.3: remember the
        // SelectorMode the user was in when the tool activated, so we
        // can restore it on deactivate. Default to `active` — the
        // common case is selector-ring mode on the Mini.
        InputManager::SelectorMode _savedSelectorMode{ InputManager::SelectorMode::active };
        // OPENRCT2MINI grid-cursor-plan §10.1: clear the grid cursor
        // marker globals on deactivate so the tile-marker paint stops
        // rendering when the tool exits.
        bool _wroteSelection{ false };

    public:
        // Verbs — override as needed. Default returns Consumed so the
        // tool context swallows the shortcut even if the verb hasn't
        // been implemented yet; this is the desired behaviour during
        // 3.E development so a half-wired tool doesn't fall through to
        // a world-cursor click on the same press.
        virtual Disposition onPlace()  { return Disposition::Consumed; }
        // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5 (Phase B):
        // default base routes cancel through the same right-click
        // tile-action dispatcher the mouse RMB short-press uses,
        // sourced from the cursor's tile. OSK-pattern destructive
        // action — delete the element at the cursor without exiting
        // grid-cursor mode. Subclasses with non-tile-delete cancel
        // semantics (e.g. RideConstruction's segment back-step) can
        // override. Defined out-of-line so the implementation can
        // include ViewportInteraction.h without forcing it on
        // every consumer of this header.
        virtual Disposition onCancel();
        // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5 (Phase C
        // follow-up 2026-05-17): mouse cursor.cancel routes through
        // here instead of onCancel(). The mouse user's cursor is the
        // OS pointer, not the grid cursor, so a right-click should
        // act on the tile under the pointer. Default base dispatches
        // ViewportInteractionRightClick(ContextGetCursorPosition()) —
        // same call the legacy mouse RMB short-press path made
        // directly. Subclasses with bespoke cancel semantics
        // override this too if they need to diverge. Defined out-of-
        // line for the same header-hygiene reason as onCancel.
        virtual Disposition onCancelAtScreen();
        virtual Disposition onRotate() { return Disposition::Consumed; }
        virtual Disposition onRaise()  { return Disposition::Consumed; }
        virtual Disposition onLower()  { return Disposition::Consumed; }
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17):
        // finish verb — the user is done with the tool. Default
        // base behaviour: drop the grid-cursor latch so the strategy
        // hands focus back to the tool window's widgets; subclasses
        // (e.g. FootpathContextImpl) override to also close the tool
        // window. Wired to the interface.confirm shortcut so the UX
        // pattern is symmetric: confirm in widgetFocus engages the
        // grid cursor, confirm in grid-cursor mode closes the tool.
        // Distinct from IInputContext::onConfirm (the modal-hooks
        // bool API for OSK/loadSave/etc.) — different verb, kept
        // separate so the modal stack doesn't fire on tool confirm.
        virtual Disposition onFinishTool() { return exitGridCursorMode(); }
        // OPENRCT2MINI grid-cursor-plan §14.1: D-pad step (or sub-tile
        // selection if precision is held). The directional channel
        // routes through here from the focus.* shortcut IDs (the
        // tool-context allow-list adds them in InputManager.cpp's
        // gating table) and from the per-frame held-state poll.
        //
        // The base implementation (defined out-of-line in
        // InputContextStrategy.cpp) handles the common case:
        // compute the world delta via stepForDirection under the
        // active GridCursorMode + camera rotation, step whichever
        // cursor model getCursorModel() returns (Grid for Footpath
        // / Scenery / LandRights / TileInspector / RideConstruction;
        // Edge for Terrain / Water), then nudge the camera if the
        // cursor reaches the viewport margin. Subclasses only need
        // to override this when they want a non-default step shape
        // (e.g. RideConstruction's segment-aligned placement).
        virtual Disposition onStep(::Direction dpad);

        // OPENRCT2MINI grid-cursor-plan §4.1 / §11: per-tool sub-tile
        // subset that the precision modifier cycles through. Base
        // default = none (precision modifier becomes a no-op);
        // subclasses override (e.g. FootpathContextImpl returns
        // edges for railings mode).
        virtual SubsetType precisionSubset() const { return SubsetType::none; }

        GridCursorModel& gridCursor() { return _grid; }
        const GridCursorModel& gridCursor() const { return _grid; }

        // OPENRCT2MINI grid-cursor-plan §10.1 / §7.3: hide the pixel
        // cursor on tool entry, restore it on exit. Mirrors Focus
        // mode's SelectorMode lifecycle.
        void onActivate() override;
        void onDeactivate() override;

        // OPENRCT2MINI grid-cursor-plan §10.1: per-frame tick. The
        // surface-paint hook gates the highlight on a 500ms blink
        // (Paint.Surface.cpp:1101-1105), but the viewport only
        // repaints tiles that something invalidated. Without a
        // per-frame invalidate the framebuffer freezes at whichever
        // blink phase last redrew, so the cursor either stays "on"
        // forever or never reappears after a step. Invalidating the
        // cursor's current tile every frame keeps the paint
        // pipeline pumping at full rate; the blink-gate decides
        // whether the highlight is drawn or skipped that frame.
        void processFrame(uint32_t /*nowMs*/) override;

        // Shared shortcut routing: maps the generic shortcut IDs onto
        // verb calls. Subclasses can override to handle additional IDs
        // (e.g. TrackContext segment-extension shortcuts) but the
        // common set lives here.
        //
        // §14.1 routing additions:
        //   - kFocusUp/Down/Left/Right → onStep(Direction)
        //   - kInterfaceConstructionZRaise → onRaise
        //   - kInterfaceConstructionZLower → onLower
        // OPENRCT2MINI grid-cursor-plan §12.1 (amendment 2026-05-17):
        // dismiss / cancel exits grid-cursor mode and hands focus
        // back to the tool window — mirrors the OSK lifecycle. The
        // tool itself stays armed (gInputFlags.toolActive remains
        // true) so the user can swap mode buttons or re-engage the
        // grid cursor from another mode without reopening the
        // window. Implementation lives out-of-line so it can call
        // GetInputManager() without dragging InputManager.h into
        // every consumer of this header.
        Disposition exitGridCursorMode();

        Disposition onShortcut(std::string_view id, const InputEvent& e) override
        {
            // Dismiss = back out of grid-cursor mode to the tool
            // window's widget focus. Mirrors OSK dismiss.
            if (id == ShortcutId::kInterfaceDismiss)
                return exitGridCursorMode();
            // Confirm = "OK, I'm done with this tool". Default: drop
            // the latch (subclasses can override to also close the
            // window). Symmetric with the widgetFocus path where
            // interface.confirm ENGAGES grid-cursor mode.
            if (id == ShortcutId::kInterfaceConfirm)
                return onFinishTool();
            if (id == ShortcutId::kCursorClick)
                return onPlace();
            // cursor.cancel keeps its original tool verb meaning
            // (e.g. Footpath: remove path under cursor) — mirrors
            // the right-click behaviour of the mouse-driven tool.
            // For the "back out of grid mode" gesture, use
            // interface.dismiss above.
            //
            // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5
            // (Phase C follow-up 2026-05-17): mouse cursor.cancel
            // needs the ~500 ms short-press disambiguation and
            // wants to delete at the OS pointer's screen tile —
            // legacy mouse RMB behaviour. Defer the press; the
            // held-state poll for kInterfaceCameraDrag in
            // UiContext::ProcessWorldCursor synthesises a release
            // event after the short-press gate passes. The
            // synthesised release flows through this same handler
            // and falls into the screen-coord dispatch below.
            // Non-mouse devices (gamepad / keyboard) fire on press
            // immediately via the grid-cursor-aware onCancel —
            // their cursor IS the grid cursor, so deleting at the
            // model position is the right semantic.
            if (id == ShortcutId::kCursorCancel)
            {
                if (e.deviceKind == InputDeviceKind::mouse)
                {
                    if (e.state != InputEventState::release)
                        return Disposition::Consumed;
                    return onCancelAtScreen();
                }
                return onCancel();
            }
            if (id == ShortcutId::kInterfaceRotateConstruction)
                return onRotate();
            if (id == ShortcutId::kInterfaceConstructionZRaise)
                return onRaise();
            if (id == ShortcutId::kInterfaceConstructionZLower)
                return onLower();
            // focus.* in a tool context drives the grid cursor.
            // Direction values match MapSelectType helpers: 0 N,
            // 1 E, 2 S, 3 W (compass) — see Location.hpp Direction
            // alias. Map focus.up→0, focus.right→1, focus.down→2,
            // focus.left→3 as the base; stepForDirection handles
            // mode + rotation translation.
            if (id == ShortcutId::kFocusUp)
                return onStep(static_cast<::Direction>(0));
            if (id == ShortcutId::kFocusRight)
                return onStep(static_cast<::Direction>(1));
            if (id == ShortcutId::kFocusDown)
                return onStep(static_cast<::Direction>(2));
            if (id == ShortcutId::kFocusLeft)
                return onStep(static_cast<::Direction>(3));
            // Z-raise / Z-lower verbs map to the construction Z-lock
            // modifier paired with the existing height-adjust path
            // — Phase 3.E ships the verb hooks but defers the actual
            // shortcut mapping until tool subclasses are wired. Until
            // then, fall through to Passthrough so existing keyboard
            // bindings continue to work in tool mode.
            return Disposition::Passthrough;
        }

        ICursorModel* getCursorModel() override
        {
            return &_grid;
        }
    };

} // namespace OpenRCT2::Ui
