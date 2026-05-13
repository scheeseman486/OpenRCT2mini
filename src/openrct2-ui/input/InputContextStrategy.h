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

#include <cstdint>
#include <string_view>

namespace OpenRCT2::Ui
{
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

    // Tile-aligned cursor. D-pad press steps one tile; analog stick
    // continuous-steps proportional to deflection. Position stored as
    // `TileCoordsXY`. Render layer: viewport (paint phase, below UI).
    // Phase 3.E implements the body when FootpathContext lands.
    class GridCursorModel final : public ICursorModel
    {
        // Phase 3.C: empty stub.
    };

    // Tile-edge or tile-corner aligned cursor. Position is a tile coord
    // plus a sub-tile orientation (N/S/E/W edge or NE/NW/SE/SW corner).
    // Used by TerrainContext (corners) and WaterContext (edges) once
    // Phase 3.F lands. Render layer: viewport.
    class EdgeCursorModel final : public ICursorModel
    {
        // Phase 3.C: empty stub.
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

    public:
        // Verbs — override as needed. Default returns Consumed so the
        // tool context swallows the shortcut even if the verb hasn't
        // been implemented yet; this is the desired behaviour during
        // 3.E development so a half-wired tool doesn't fall through to
        // a world-cursor click on the same press.
        virtual Disposition onPlace()  { return Disposition::Consumed; }
        virtual Disposition onCancel() { return Disposition::Consumed; }
        virtual Disposition onRotate() { return Disposition::Consumed; }
        virtual Disposition onRaise()  { return Disposition::Consumed; }
        virtual Disposition onLower()  { return Disposition::Consumed; }

        // Shared shortcut routing: maps the generic shortcut IDs onto
        // verb calls. Subclasses can override to handle additional IDs
        // (e.g. TrackContext segment-extension shortcuts) but the
        // common set lives here.
        Disposition onShortcut(std::string_view id, const InputEvent& /*e*/) override
        {
            if (id == ShortcutId::kCursorClick)
                return onPlace();
            if (id == ShortcutId::kCursorCancel)
                return onCancel();
            if (id == ShortcutId::kInterfaceRotateConstruction)
                return onRotate();
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
