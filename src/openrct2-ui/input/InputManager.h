/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2/world/Location.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

typedef struct _SDL_GameController SDL_GameController;
typedef union SDL_Event SDL_Event;

// OPENRCT2MINI focus-mode-plan / Phase F.3: forward-declarations for
// the focus-state fields below. WindowClass lives in the GLOBAL
// namespace (WindowClasses.h does not wrap it in `namespace OpenRCT2`
// — verified by reading WindowClasses.h:14), so the forward decl must
// match. WidgetIndex (a uint16_t alias) and WindowBase live inside
// `namespace OpenRCT2` — those go inside the namespace. Getting the
// scope wrong silently creates a SECOND, distinct type that shadows
// the real one, producing "'foo' is not a member of WindowClass"
// errors across the codebase. Don't move these.
enum class WindowClass : uint8_t;
namespace OpenRCT2
{
    using WidgetIndex = uint16_t;
    struct WindowBase;
}

namespace OpenRCT2::Ui
{
    class RegisteredShortcut;
    struct ShortcutInput;

    enum class InputDeviceKind
    {
        mouse,
        keyboard,
        joyButton,
        joyHat,
        joyAxis,
    };

    enum class InputEventState
    {
        down,
        release,
    };

    struct InputEvent
    {
        InputDeviceKind deviceKind;
        uint32_t modifiers;
        uint32_t button;
        InputEventState state;
        int16_t axisValue{}; // For analogue stick values (-32768 to 32767)
    };

    enum class ModifierKey : uint8_t
    {
        none = 0,
        shift = 1 << 0,
        ctrl = 1 << 1,
        alt = 1 << 2,
        cmd = 1 << 3,
    };

    // OPENRCT2MINI gamepad-plan 1.6b: input context — what the cursor.*
    // bindings drive right now. Resolved once per frame at the top of
    // InputManager::process() and exposed via getActiveContext(). Single
    // source of truth so every consumer (the per-frame cursor poll, the
    // SDL_KEYDOWN OSK route, future focus-mode / list-scroll modes)
    // agrees on what mode the user is in. Resolution rule lives in one
    // place — InputManager::resolveActiveContext().
    //
    // Adding a new context: add an enum entry below, add a priority
    // line to resolveActiveContext, and a consumer that reads
    // getActiveContext() to dispatch its own per-frame logic.
    enum class InputContext : uint8_t
    {
        world = 0,         // default — cursor.* drives the virtual mouse
        osk,               // OSK is up — cursor.* drives OSK key selection
        // OPENRCT2MINI gamepad-plan 1.10: typing / list-modal contexts
        // that gate the per-event shortcut dispatch via
        // InputManager::isShortcutAllowedInActiveContext. Each one
        // narrows the allow-list to dismiss / confirm (and, where it
        // makes sense, list-nav cursor.* / debug toggleConsole). All
        // other shortcuts are blocked so a stray gamepad press in,
        // say, the in-game console doesn't fire a world-mode shortcut.
        // chat is intentionally absent — multiplayer is not shipped in
        // OpenRCT2mini, so gChatOpen is always false and the chat
        // dispatch code path in process(InputEvent) is dead.
        textInput,         // standalone TextInput window open
        loadSaveOverwritePrompt, // overwrite-confirm dialog over loadsave
        loadSave,          // file load/save dialog open
        console,           // in-game console open
        widgetTextBox,     // an in-place widget textbox is being edited
        // OPENRCT2MINI focus-mode-plan / Phase F.1: widget-focus
        // navigation mode (renamed from `caretBrowse` — the original
        // name was borrowed from web browsers and conveyed the wrong
        // intent). Active when a focusable window is on top and either
        // (a) widgetFocusAlwaysOn is true, or (b) the user has
        // explicitly entered focus mode via the
        // kInterfaceEnterFocusMode shortcut. While active, cursor.*
        // shortcuts navigate spatially between focusable widgets in
        // the active window; cursor.click presses the focused widget;
        // dismiss exits. See focus-mode-plan.md for the full design.
        widgetFocus,
        // OPENRCT2MINI input-plan Track 3 / Phase 3.E: tool contexts.
        // Each construction tool window owns one entry. Activated when
        // both (a) the tool's window is open and (b) gInputFlags has
        // toolActive set (the engine's existing signal that a tool is
        // currently mid-operation rather than just showing its window).
        // Phase 3.E ships footpath as the first concrete tool; 3.F adds
        // terrain / water; 3.G adds the rest.
        toolFootpath,
        // OPENRCT2MINI input-plan Track 3 / Phase 3.F: edge-tile tools.
        // Terrain edits tile corners; water edits tile edges. Both
        // use EdgeCursorModel — D-pad cycles through orientations
        // within a tile before advancing to the neighbour tile.
        // Phase 3.F ships the skeletons; verb bodies (LandRaiseAction,
        // WaterRaiseAction, etc.) are wired in the next iteration.
        toolTerrain,
        toolWater,
        // OPENRCT2MINI input-plan Track 3 / Phase 3.G: remaining tool
        // contexts. Scenery + LandRights use GridCursorModel like
        // Footpath (tile-aligned). TileInspector is a debug tool with
        // grid cell selection. RideConstruction (covers track tools) is
        // complex — track placement has its own grammar with previous-
        // segment-constrained orientation — but the skeleton just
        // identifies the slot; the per-tool grammar lives in the verb
        // wiring follow-up.
        toolScenery,
        toolLandRights,
        toolTileInspector,
        toolRideConstruction,
        // OPENRCT2MINI grid-cursor-plan §11.9 / §18.C (2026-05-24):
        // ClearScenery (bulldozer) tool. Same shape as toolLandRights
        // (single-click brush over gLandToolSize footprint).
        toolClearScenery,
        // OPENRCT2MINI grid-cursor-plan §11.10 (2026-05-29):
        // Staff patrol-zone painting. Per-staff bimodal toggle —
        // PAD A auto-toggles Set/Unset based on tile state,
        // PAD B force-unsets. Pattern B (Footpath-style per-press
        // confirm), NOT the LandRights/ClearScenery drag-chain.
        toolPatrol,
        // OPENRCT2MINI grid-cursor-plan §11.11 (2026-05-29):
        // Peep pickup (admin/debug "pick up a guest/staff and
        // drop them" tool). Shares WindowClass::peep with the
        // info panel — disambiguated by gInputFlags.toolActive +
        // gCurrentToolWidget.windowClassification == peep.
        toolPeepPickup,
        // listScroll,     // future: cursor.* drives a focused scrollable list
    };

    // OPENRCT2MINI input-plan Track 3 / Phase 3.A: forward-declared
    // strategy interface. The full definition is in
    // InputContextStrategy.h. We forward-declare here so InputManager
    // can hold a registry of strategies without bringing the whole
    // interface header into every InputManager.h consumer.
    class IInputContext;

    class InputManager
    {
    private:
        uint32_t _lastJoystickCheck{};
        std::vector<SDL_GameController*> _gameControllers;
        std::queue<InputEvent> _events;
        ScreenCoordsXY _viewScroll;
        ScreenCoordsXY _analogueScroll;     // analogue stick scroll values
        float _analogueScrollAccumX = 0.0f; // Fractional accumulator for X axis
        float _analogueScrollAccumY = 0.0f; // Fractional accumulator for Y axis
        uint32_t _mouseState{};
        std::vector<uint8_t> _keyboardState;
        uint8_t _modifierKeyState;

        // OPENRCT2MINI gamepad-plan 1.2: held gamepad-button set, used as
        // the chord-modifier source when matching ShortcutInput against an
        // incoming joyButton / joyAxis event. Updated in queueInputEvent
        // before the InputEvent is enqueued, so by the time the event
        // reaches process() the held-set already reflects this transition.
        // Triggers (LEFTTRIGGER / RIGHTTRIGGER axes) participate in the
        // held-set after their press-threshold crossing — see queueInputEvent.
        // Stick-direction-as-button entries (PAD STICK_L UP etc.) likewise
        // enter and leave the set on threshold crossings. Indices follow
        // SDL_CONTROLLER_BUTTON_* for buttons; for axis-as-button entries
        // we encode them in the high range (kAxisAsButtonBase + axis*4 +
        // direction) to avoid collision with real button indices. See
        // ShortcutInput.cpp for the encoding helpers.
        std::set<uint32_t> _heldGamepadButtons;

        // OPENRCT2MINI gamepad-plan 1.6b step 1: cached input context.
        // Resolved at the top of process() (once per frame) by
        // resolveActiveContext() and exposed via getActiveContext().
        // Default world; flips to osk while the on-screen keyboard is
        // up. Cached so all consumers within one frame see the same
        // value even if a window opens/closes mid-frame.
        InputContext _activeContext = InputContext::world;

        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: ContextRegistry.
        // One strategy slot per InputContext enum value (sized to cover
        // the future widgetFocus / listScroll stubs commented out in
        // the enum above without re-allocation). Slot for each enum
        // value is filled on-demand at InputManager init for world
        // (WorldContextStub registers itself) and by future Phase B
        // migration for the modal contexts. Empty slots fall back to
        // the world strategy — see getActiveContextStrategy().
        //
        // Registry is poll-derived: the active strategy is picked
        // each frame by resolveActiveContext via the existing window-
        // poll model. No push/pop stack semantics — see
        // input-plan-review.md §F1 for why a stack was rejected.
        //
        // Phase 3.A keeps this minimal: storage + accessor + the
        // activate/deactivate lifecycle hook called when
        // _activeContext changes across frames. Phase 3.B exposes
        // public register/unregister methods for modal window owners
        // to call.
        // Sized to cover every current enum entry plus a margin for
        // the commented-out listScroll stub. Phase 3.G's tool entries
        // reach index 14; bump to 16 for headroom. Bumped to 17 for
        // toolClearScenery (§11.9, 2026-05-24). Bumped to 18 for
        // toolPatrol (§11.10, 2026-05-29). Bumped to 19 for
        // toolPeepPickup (§11.11, 2026-05-29).
        static constexpr size_t kInputContextCount = 19;
        // No NSDMI brace-init `{}` here on purpose. GCC 8.3 (OnionUI
        // toolchain) instantiates `unique_ptr<IInputContext>::~unique_ptr`
        // at the NSDMI site when `{}` is present, which then requires
        // `IInputContext` to be complete in every TU that includes this
        // header — UiContext.cpp and WindowManager.cpp don't include
        // InputContextStrategy.h and break. The members default-construct
        // each unique_ptr to nullptr without the brace-init, and the
        // out-of-line ~InputManager() in InputManager.cpp (where the
        // strategy header IS included) handles the actual destruction.
        std::array<std::unique_ptr<IInputContext>, kInputContextCount> _contextRegistry;
        std::unique_ptr<IInputContext> _worldContext;
        // Cached previous frame's _activeContext so we can detect the
        // transition edge and fire onDeactivate/onActivate exactly
        // once per change.
        InputContext _previousActiveContext = InputContext::world;

        // OPENRCT2MINI focus-mode-plan / Phase F.1: focus-mode
        // activation flag. Only consulted when
        // Config::General::widgetFocusAlwaysOn is false. Flipped on by
        // the kInterfaceEnterFocusMode shortcut's lambda; consulted by
        // resolveActiveContext. Cleared automatically when no
        // focusable window is on top — see resolveActiveContext for
        // the latching logic.
        bool _focusModeRequested = false;

        // OPENRCT2MINI focus-mode-plan / Phase F.3: focused widget
        // state. Stores the class of the window owning focus and the
        // widget index within that window, NOT a raw `WindowBase*`.
        // Raw pointers would dangle the moment the window closes
        // (window storage is a list; nodes are reallocated) — the
        // sanity-check review §F3 surfaced this. Re-lookup happens
        // every frame via `focusedWindowLive()` which calls
        // `WindowManager::FindByClass`; a null return implicitly
        // clears the focus state in `process()`. Sentinel values:
        //   _focusedWindowClass == WindowClass::null → no focus
        //   _focusedWidget == kWidgetIndexNull        → no widget
        //
        // The widget-focus strategy is the sole writer in the
        // current code base; the render hook (Phase F.5) is the
        // sole reader outside InputManager. Public accessors are
        // narrow on purpose (set/clear/getters only) to keep the
        // invariant — focus mirrors a real, live widget index — in
        // one place.
        // Default-init via cast — the forward-declared `WindowClass`
        // doesn't expose enumerator names here, but its underlying
        // `uint8_t` does. `255 == WindowClass::null` (the sentinel for
        // "no window"); `kWidgetIndexNull == 0xFFFF`. Both literals
        // match the canonical sentinel constants defined in the
        // respective headers and verified in InputManager.cpp via
        // static_assert below.
        WindowClass _focusedWindowClass{ static_cast<WindowClass>(255) };
        WidgetIndex _focusedWidget{ 0xFFFFu };
        // OPENRCT2MINI list-focus-plan §2.2: when the focused widget is
        // a list-mode scroll widget (the window opts in via
        // scrollFocusGetItemCount > 0), this is the highlighted row /
        // cell index inside that widget. -1 means "the focused widget
        // is not a list-mode scroll widget" — the regular focus ring
        // logic applies. Cleared on every setFocus / clearFocus call
        // and on every step that moves focus to a different widget.
        int32_t _focusedScrollItem{ -1 };
        // OPENRCT2MINI focus-mode-plan §F.12: remember the topmost
        // focusable class from the previous frame so the bootstrap
        // can detect "a new window opened on top" without forcing
        // focus onto the topmost every single frame (which would
        // override the cycle-window shortcuts when their target is
        // a stickToBack window that BringToFront can't actually
        // raise). Reset on clearFocus.
        WindowClass _lastTopmostFocusable{ static_cast<WindowClass>(255) };

        // OPENRCT2MINI active-window-emphasis plan §4.2: cycle-window
        // outline persistence. Set when cycleFocusedWindow runs,
        // cleared at the end of process() when the modifier the
        // cycle binding requires is released (or after the fallback
        // timeout for modifier-less rebinds). Sentinel 255 is
        // WindowClass::null per the existing _focusedWindowClass
        // convention — the forward-declared WindowClass enum doesn't
        // expose its enumerator names here, but its uint8_t underlying
        // type does. Used by WidgetFocus::drawFocusOutlineIfActive
        // to bypass the normal context / SelectorMode gates while
        // the user is still cycling.
        WindowClass _cycleHighlightClass{ static_cast<WindowClass>(255) };
        // ModifierKey-collapsed mask (NOT SDL KMOD bits) of the
        // modifiers the cycle binding requires. The union of
        // kInterfaceCycleNextWindow + kInterfaceCyclePreviousWindow
        // binding modifiers, converted via kmodToModifierKey. Zero
        // means "binding requires no modifier" — fall back to
        // _cycleHighlightUntilMs timeout instead.
        uint8_t _cycleHighlightModifierMask{ 0 };
        // Platform::GetTicks() deadline for the modifier-less
        // fallback. Zero when the modifier-mask path is in use.
        uint32_t _cycleHighlightUntilMs{ 0 };
        // OPENRCT2MINI AWE post-smoke-test fix: capture the focused
        // widget index alongside the class. The cycle bypass in
        // WidgetFocus::drawFocusOutlineIfActive uses THIS field
        // instead of mgr.getFocusedWidget() so the outline persists
        // even if some other code path clears _focusedWidget between
        // the arming frame and the user releasing the modifier (e.g.
        // the per-frame focus bootstrap re-evaluating in a way that
        // clears the widget index).
        WidgetIndex _cycleHighlightWidget{ 0xFFFFu };

    public:
        // OPENRCT2MINI cursor-selector-modal-plan §3.1 (v2): two-
        // state visibility model.
        //   active — selector ring visible, cursor hidden.
        //   hidden — ring hidden, cursor visible.
        // Auto-transitions live in the per-frame bootstrap and the
        // shortcut dispatch site (see onTransitionEvent). The
        // `_lastInputWasRealMouse` flag below distinguishes whose
        // input the user is currently using; new windows opening
        // while in `hidden` only wake the selector when the last
        // input was virtual (D-pad / gamepad button driving the
        // synthetic cursor) — never on real-mouse-driven flows,
        // which stay in cursor mode forever until the user
        // explicitly toggles.
        enum class SelectorMode : uint8_t
        {
            active = 0,
            hidden,
        };

    private:
        SelectorMode _selectorMode = SelectorMode::active;
        // OPENRCT2MINI cursor-selector-modal-plan v2: was the most
        // recent user input a real-mouse event (SDL_MOUSEMOTION,
        // SDL_MOUSEBUTTON*) vs anything else (keyboard, gamepad,
        // virtual cursor via polled D-pad)? Used to decide whether
        // new-window auto-wake fires. Default false so first-frame
        // bootstrap behaves selector-friendly on Mini-style
        // launches.
        bool _lastInputWasRealMouse = false;
        // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
        // "selector just auto-woke during this process() call" flag.
        // Set true in the per-frame bootstrap whenever it flips
        // SelectorMode from hidden→active because a new non-chrome
        // window appeared on top (e.g. a held-state-poll-driven mouse
        // click in cursor mode opens a dropdown). Cleared at the top
        // of every process() call.
        //
        // Consumed by WidgetFocusContextImpl::onShortcut: when this
        // is true and the dispatched shortcut is kCursorClick /
        // kCursorCancel, the queued press came from the SAME
        // physical button squeeze that opened the new window. The
        // ProcessWorldCursor held-state poll has already synthesised
        // a virtual mouse click for it (which is what opened the
        // window in the first place) — letting the focus context
        // ALSO commit/cancel against the freshly-focused dropdown
        // would double-fire on a single press. The symptom: PAD A
        // on the Options toolbar widget opens the dropdown for one
        // frame, then immediately commits item 0 (because focus
        // snapped to dropdown and the same press hit
        // WidgetFocusContextImpl's dropdown kCursorClick branch).
        bool _autoWokeThisProcess = false;
        // OPENRCT2MINI grid-cursor-plan §12.1 (amended): when a tool
        // window is open and a tool is armed (gInputFlags.toolActive),
        // the cycle-window list grows a virtual "tool viewport" entry
        // positioned after the tool window itself. Landing on the
        // virtual entry sets this flag; landing on any real window
        // clears it. resolveActiveContext consults the flag to decide
        // whether the tool-context arm wins over the widgetFocus arm
        // (the previous order was unconditional widgetFocus-first,
        // which meant opening a tool window's widgets would silently
        // steal the cursor.* drive even though the user expected the
        // grid cursor in the tool's viewport).
        //
        // Lifecycle: snapped true on the toolActive false→true edge
        // (detected per-frame in process()), cleared on the true→false
        // edge, and overridden by cycleFocusedWindow when the user
        // navigates to a different surface. No new shortcut — the
        // virtual entry rides on kInterfaceCycleNextWindow /
        // kInterfaceCyclePreviousWindow (SHIFT+TAB / CTRL+TAB).
        bool _toolFocusSelected{ false };
        // Cached previous-frame toolActive bit so process() can detect
        // the false→true / true→false edges without polling every
        // consumer separately. Updated at the same point as the
        // _toolFocusSelected snap.
        bool _previousToolActive{ false };

        // OPENRCT2MINI focus-mode-plan §F.16: history stack for
        // cancel/back navigation. Every time the selector snaps to
        // a new topmost window (because a button click opened it,
        // because a dropdown popped up, etc.), the OLD (focused
        // class, focused widget) gets pushed here. When the user
        // cancels out of the topmost window (close-box, dropdown
        // cancel, …), the stack pops and focus restores to the
        // most-recent live frame — i.e. the parent window AND the
        // specific widget the user was on before opening this
        // child. Capped at 32 entries to keep memory bounded if
        // some pathological flow somehow stacks deeper than that.
        std::vector<std::pair<WindowClass, WidgetIndex>> _focusStack;

        // OPENRCT2MINI focus-memory-plan §3 (2026-05-25): per-window
        // selection memory. Written by setFocus() on every non-null
        // (cls, widget) update; consulted by resolveLandingWidget()
        // as the PRIMARY source-of-truth for "what widget should I
        // land on when entering this window?" before falling back to
        // the window-set's defaultWidget or firstFocusable.
        //
        // The map complements _focusStack — the stack owns
        // WHICH-WINDOW for chained-back navigation (close A's child
        // → return to A), the map owns WHICH-WIDGET on every entry
        // path (cycle, auto-snap, enterFocusMode, restoreFocus).
        // They're orthogonal.
        //
        // Stale entries (window class no longer alive at lookup
        // time) are detected lazily by FindByClass returning null
        // in the helper and fall through to the defaultWidget /
        // firstFocusable chain. No window-close hook needed.
        //
        // Capped at kFocusMemoryCap = 64 entries. On insert past
        // cap, evict the first entry whose backing class no longer
        // exists; if all entries are still live (rare — engine has
        // ~50 distinct window classes), drop an arbitrary entry to
        // make room. Tracked as the most-recent-insert hint to avoid
        // a per-frame O(N) scan of the map.
        std::unordered_map<WindowClass, WidgetIndex> _focusMemory;

        // OPENRCT2MINI focus-mode-plan §F.cycle (open-order stability):
        // Persistent open-order list of cycleable window classes (set-
        // deduped). Synced lazily by cycleFocusedWindow against the
        // live gWindowList — entries for closed windows are removed,
        // newly-opened focusable windows are APPENDED at the end. The
        // index of each window stays stable as long as it remains
        // open, regardless of z-order shuffling from BringToFront,
        // user clicks, or any other reorder. This is what gives cycle
        // next/prev a predictable 1→2→3→4→1 wraparound — the old code
        // rebuilt the cycle list from gWindowList z-order on every
        // step, which made cycle order depend on the very thing the
        // cycle was changing.
        //
        // The actual cycle list presented to the user is the
        // playfield virtual entry at slot 0, followed by every entry
        // from _windowCycleOrder. Toolbar's set lands at slot 1
        // naturally because it's the first window created at game
        // start.
        std::vector<WindowClass> _windowCycleOrder;

    public:
        // OPENRCT2MINI focus-mode-plan / Phase F.1: focus-mode toggle.
        // Called from the kInterfaceEnterFocusMode shortcut lambda
        // when its activation gesture fires (e.g. PAD BACK 500 ms
        // hold). Idempotent; safe to call when no focusable window is
        // on top (the next frame's resolveActiveContext will
        // auto-clear the flag).
        void requestFocusMode()
        {
            _focusModeRequested = true;
        }
        void clearFocusMode()
        {
            _focusModeRequested = false;
        }
        bool isFocusModeRequested() const
        {
            return _focusModeRequested;
        }

        // OPENRCT2MINI focus-mode-plan / Phase F.3: focus state
        // accessors. The strategy (WidgetFocusContextImpl) calls
        // `setFocus` on D-pad steps and on first-activation snap-to-
        // first-focusable; `getFocusedWindow()` re-resolves the
        // pointer per call so a stale class id (window closed
        // between frames) returns null. `clearFocus` resets both
        // fields back to their sentinels — used on context
        // deactivation. All implemented out-of-line in InputManager
        // .cpp because they touch types that only the .cpp's full
        // includes resolve (WindowClasses.h, WindowManager.h).
        WindowBase* getFocusedWindow() const;
        WidgetIndex getFocusedWidget() const
        {
            return _focusedWidget;
        }
        WindowClass getFocusedWindowClass() const
        {
            return _focusedWindowClass;
        }
        void setFocus(WindowClass cls, WidgetIndex widget);
        void clearFocus();

        // OPENRCT2MINI active-window-emphasis plan §4.3: getter
        // consulted by WidgetFocus::drawFocusOutlineIfActive
        // (openrct2-ui/input/WidgetFocus.cpp) to gate the cycle-window
        // outline. Returns WindowClass::null (255) when no cycle
        // highlight is armed.
        WindowClass getCycleHighlightClass() const noexcept
        {
            return _cycleHighlightClass;
        }
        // OPENRCT2MINI AWE post-smoke-test fix: paired with
        // getCycleHighlightClass() — see _cycleHighlightWidget for
        // the rationale (paint path no longer depends on the
        // _focusedWidget lifecycle).
        WidgetIndex getCycleHighlightWidget() const noexcept
        {
            return _cycleHighlightWidget;
        }

        // OPENRCT2MINI list-focus-plan §2.2: getter / setter for the
        // list-mode scroll item index. Returns -1 when the focused
        // widget is not a list-mode scroll widget. setFocusScrollItem
        // assumes the caller has just set focus to the scroll widget
        // (via setFocus) — it only writes the item index, doesn't
        // touch _focusedWindowClass / _focusedWidget.
        int32_t getFocusedScrollItem() const
        {
            return _focusedScrollItem;
        }
        void setFocusScrollItem(int32_t item)
        {
            _focusedScrollItem = item;
        }
        // Convenience: clear list-mode state without disturbing the
        // widget-level focus. Called when stepping out of a list to
        // an adjacent widget.
        void clearFocusScrollItem()
        {
            _focusedScrollItem = -1;
        }

        // OPENRCT2MINI list-focus-plan flicker fix: re-synthesise the
        // hover event for the focused list item if focus is on a
        // list-mode scroll widget. Upstream code resets per-window
        // hover state every input frame (e.g. StaffList's
        // _highlightedIndex in onScrollGetSize, ShortcutKeys's
        // _highlightedRow in onUpdate) under the assumption that a
        // continuous mouse-hover would re-set it next frame. Focus
        // mode has no continuous hover, so without this restore the
        // row highlight flickers off and on as the user steps. Called
        // from InvalidateAllWindowsAfterInput AFTER the reset chain so
        // the synthesised hover is the LAST write before draw.
        // Safe no-op when no list-mode scroll item is focused.
        void restoreFocusedListHover();

        // OPENRCT2MINI cursor-selector-modal-plan §3.1: state-machine
        // accessors. Getter is `noexcept`; setter routes through one
        // method so future logging / invalidation can hook once
        // rather than dotting writes around the source tree.
        SelectorMode getSelectorMode() const noexcept
        {
            return _selectorMode;
        }
        bool lastInputWasRealMouse() const noexcept
        {
            return _lastInputWasRealMouse;
        }
        // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up: see
        // _autoWokeThisProcess field comment. True for the duration
        // of one process() call after the bootstrap auto-woke the
        // selector this frame.
        bool wasAutoWokenThisProcess() const noexcept
        {
            return _autoWokeThisProcess;
        }
        void setSelectorMode(SelectorMode mode);

        // OPENRCT2MINI cursor-selector-modal-plan v2: transition
        // sources. Real-mouse paths additionally update
        // _lastInputWasRealMouse so the per-frame bootstrap can
        // decide whether to auto-wake the selector when a new
        // window appears.
        enum class SelectorTransitionSource : uint8_t
        {
            realMouseMotion = 0,
            realMouseClick,
            virtualUserInput,
            cursorCancelInBottomSet,
            wakeCursorRequested,
            enterFocusModeRequested,
        };
        void onTransitionEvent(SelectorTransitionSource src);

        // OPENRCT2MINI grid-cursor-plan §12.1 (amended): tool-focus
        // selector. True when the cycle-window has landed on the
        // virtual "tool viewport" entry — resolveActiveContext routes
        // the tool-context arm above widgetFocus when set. The setter
        // routes through SelectorTransitionSource so future logging
        // / SelectorMode invalidation can hook one site (parameter
        // matches onTransitionEvent's contract for symmetry; current
        // implementation is informational only).
        bool isToolFocusSelected() const noexcept
        {
            return _toolFocusSelected;
        }
        void setToolFocusSelected(bool selected, SelectorTransitionSource src);

        // OPENRCT2MINI focus-mode-plan §F.8: cycle the focus ring to
        // the next / previous window with focusable widgets, wrapping
        // around the gWindowList ends. Direction = +1 → walk from
        // current position toward the back of the list (toward
        // top-of-z-stack); -1 → walk toward the front (bottom).
        //
        // Behaviour is a strict step through z-order: same skip-list
        // as the per-frame bootstrap (dead windows + mainWindow), and
        // only windows with at least one focusable widget count. If
        // no focus is currently set the first qualifying window
        // becomes the new focus regardless of direction. If only one
        // qualifying window exists the call is a no-op.
        //
        // Wired into the action lambdas for
        // kInterfaceCycleNextWindow / kInterfaceCyclePreviousWindow.
        void cycleFocusedWindow(int direction);

        // OPENRCT2MINI focus-mode-plan §F.15: when a selector action
        // pops a new window on top (e.g. cursor.click on a button
        // that opens About, or a dropdown that opens a sub-window),
        // call this to snap focus onto the new topmost focusable
        // window immediately. Same rules as the per-frame bootstrap
        // (reverse-walk gWindowList, skip dead / mainWindow / no-
        // focusable windows). Does nothing if the topmost focusable
        // already matches _focusedWindowClass. Lets the selector
        // follow the user's intent same-frame instead of waiting
        // for the next process() pass to notice.
        //
        // Side-effect: before changing focus, pushes the OLD
        // (class, widget) onto _focusStack so restoreFocus can
        // return the user there on cancel.
        void snapFocusToTopmostFocusable();

        // OPENRCT2MINI focus-mode-plan §F.16: pop the focus stack
        // and restore the most-recent live frame. Skips entries
        // that point at dead or no-longer-existing windows. Falls
        // back to a fresh snap if the stack is empty or fully
        // stale. Returns true if a restore happened, false if the
        // caller should fall back. Wire into cursor.cancel paths
        // (close box, dropdown cancel) so backing out lands on the
        // exact widget the user was on before opening the child.
        bool restoreFocus();

        // OPENRCT2MINI focus-memory-plan §3 (2026-05-25): given a
        // target window class, return the widget index that should
        // receive focus on entry. Consultation order:
        //   1. _focusMemory[cls] — the last widget focused on this
        //      window, when the saved index is still in-bounds and
        //      focusable (handles tab rebuilds / resizes by re-
        //      validating).
        //   2. The window-set's defaultWidget (if cls is a set
        //      member and that default widget is still focusable on
        //      its defaultClass).
        //   3. WidgetFocus::firstFocusable(*window).
        //   4. kWidgetIndexNull if no focusable widget exists (or
        //      the window itself can't be found).
        //
        // Returned by all four window-entry paths (cycleFocusedWindow,
        // snapFocusToTopmostFocusable, enterFocusMode, restoreFocus)
        // instead of re-implementing the defaultWidget / firstFocusable
        // ladder. cls is the FINAL class to focus (set resolution to
        // defaultClass happens at the caller before this).
        OpenRCT2::WidgetIndex resolveLandingWidget(WindowClass cls);

        // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
        // explicit "enter selector mode and land on the first widget
        // of the topmost window" action, wired to the
        // kInterfaceEnterFocusMode shortcut (default TAB). Unlike
        // snapFocusToTopmostFocusable this does NOT short-circuit
        // when the topmost is already the focused window's set —
        // the user pressed TAB to reset the selector to the first
        // widget, so we always re-land on firstFocusable. Returns
        // true if focus was placed (and selector flipped active);
        // false if no focusable window exists (no-op, the user
        // stays in whatever mode they were in).
        bool enterFocusModeOnTopmost();

    public:
        // OPENRCT2MINI gamepad-plan 1.6c.2: modal dismiss / confirm
        // hook stack. Each modal context (OSK, LoadSave, Overwrite-
        // Prompt, TextInput modal, in-game console, chat, in-place
        // widget textbox) calls pushModalHooks() on activate and
        // popModalHooks(token) on deactivate. The dispatch in
        // process(InputEvent) checks the corresponding shortcut
        // binding ahead of every existing modal gate; if matched and
        // the callback returns true, the event is consumed.
        //
        // Stack rather than single pair because modals nest:
        // TextInput::onOpen spawns an OSK, OSK::onOpen pushes its own
        // hooks, OSK::onClose pops back to TextInput's. Each owner
        // tracks the token returned from push and passes it to pop.
        //
        // Returning false from a callback lets the event continue —
        // useful if the callback wants to peek but defer handling.
        //
        // Token is opaque (uint32_t serial counter); push assigns a
        // fresh value, pop only removes the slot if its serial
        // matches (defensive against double-pop / out-of-order pop).
        using ModalHandler = std::function<bool(const InputEvent&)>;
        struct ModalHooks
        {
            ModalHandler dismiss;
            ModalHandler confirm;
        };
        using ModalHooksToken = uint32_t;
        ModalHooksToken pushModalHooks(ModalHooks hooks);
        void popModalHooks(ModalHooksToken token);

        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: explicit ctor +
        // out-of-line dtor. The dtor must live in InputManager.cpp so
        // the unique_ptr<IInputContext> members destruct where the
        // strategy type is complete (the header only forward-declares
        // IInputContext to keep the include graph slim).
        InputManager();
        ~InputManager();
        InputManager(const InputManager&) = delete;
        InputManager& operator=(const InputManager&) = delete;

    private:
        struct ModalHooksSlot
        {
            ModalHooks hooks;
            ModalHooksToken token;
        };
        std::vector<ModalHooksSlot> _modalHooksStack;
        ModalHooksToken _nextModalHooksToken{ 1 };

        void checkJoysticks();
        void processAnalogueInput();
        void updateAnalogueScroll();

        // OPENRCT2MINI input-plan Track 1 §3.1: copy SDL's keyboard
        // and mouse state into the per-frame caches _keyboardState /
        // _mouseState. Called once per frame from process() BEFORE
        // handleModifiers, so the shift/ctrl modifier shortcuts' bare-
        // modifier-key fast path (getState line ~1014) reads
        // up-to-date scancode bits. Fresh as long as SDL_PumpEvents
        // has already run in this frame's UiContext::ProcessMessages
        // — which it always has, because ProcessMessages is the SDL
        // event pump and runs before this method through the call
        // chain ProcessMessages → Tick → gameStateTick →
        // GameHandleKeyboardInput → ContextInputHandleKeyboard →
        // HandleKeyboard → process().
        //
        // Previously these refreshes lived in processHoldEvents
        // (called LATER in the same process() frame), producing a
        // 1-frame lag for keyboard-bound modifiers: press Shift in
        // frame N, vertical placement doesn't elevate until frame
        // N+1. Gamepad-bound modifiers were unaffected because
        // _heldGamepadButtons is updated synchronously inside the
        // queueInputEvent SDL pump.
        void refreshDeviceState();

        void handleViewScrolling();
        void handleModifiers();
        void processEvents();
        void process(const InputEvent& e);
        // OPENRCT2MINI input-plan Track 3 / Phase 3.B: processInGameConsole
        // moved into ConsoleContextImpl's onKeyEvent — see anonymous
        // namespace at top of InputManager.cpp.
        void processChat(const InputEvent& e);
        void processHoldEvents();
        void processViewScrollEvent(std::string_view shortcutId, const ScreenCoordsXY& delta);

        bool hasTextInputFocus() const;

        // OPENRCT2MINI gamepad-plan 1.6b step 1: pure resolver, called
        // exactly once per frame from process(). Adding a new context
        // requires extending the InputContext enum AND adding a
        // priority arm here. Order = priority high-to-low; first
        // matching arm wins.
        InputContext resolveActiveContext() const;

    public:
        bool isModifierKeyPressed(ModifierKey modifier) const;
        void queueInputEvent(const SDL_Event& e);
        void queueInputEvent(InputEvent&& e);
        void process();

        // OPENRCT2MINI: cut 38. Expose connected controllers so the virtual
        // gamepad cursor (in UiContext, behind ENABLE_SOFTWARE_CURSOR) can
        // poll D-pad and face-button state without opening a second copy.
        const std::vector<SDL_GameController*>& getGameControllers() const
        {
            return _gameControllers;
        }

        // OPENRCT2MINI gamepad-plan 1.2: read access for shortcut chord
        // matching. ShortcutInput::matches consults this when checking
        // whether a binding's chord-modifier prerequisites are satisfied.
        const std::set<uint32_t>& getHeldGamepadButtons() const
        {
            return _heldGamepadButtons;
        }

        // OPENRCT2MINI gamepad-plan 1.5b: held-state query for a registered
        // shortcut. Public so per-frame pollers (e.g. UiContext::Process-
        // VirtualGamepadCursor for cursor click / cancel) can ask "is this
        // binding currently pressed" without having to track edges
        // themselves. Returns true if any of the shortcut's `current`
        // bindings is currently active given live keyboard / joystick
        // state. Used by 1.5b for cursor.click / cursor.cancel and 1.5d
        // for cursor.fast_modifier / construction_z_lock.
        bool getState(const RegisteredShortcut& shortcut) const;

        // OPENRCT2MINI: held-state query for a single ShortcutInput entry.
        // Useful when the caller wants to filter the shortcut's bindings
        // by InputDeviceKind (e.g. the OSK routing block in
        // ProcessVirtualGamepadCursor only wants non-keyboard sources to
        // avoid double-firing alongside the SDL_KEYDOWN intercept).
        bool getState(const ShortcutInput& input) const;

        // OPENRCT2MINI gamepad-plan 1.9 follow-on: analog magnitude
        // query. Returns 0.0–1.0 for joyAxis bindings (live deflection
        // past the analog deadzone, scaled across the remaining
        // range), 1.0 for any held digital binding (keyboard /
        // joyButton), 0.0 otherwise. Lets cursor / camera analog
        // consumers route through the shortcut system rather than
        // hard-coding LEFT/RIGHT-stick axis indices, so the user can
        // rebind STICK_L → STICK_R (or to triggers, or whatever) from
        // the rebind UI like any other shortcut.
        //
        // Analog deadzone is a fixed 8000 / 32767 (~24%), separate
        // from the binding's axisThreshold (which is the digital
        // threshold-cross point at 16384 / 50%). The looser analog
        // deadzone gives finer control while the digital threshold
        // still gates the binary "shortcut held" semantics that
        // getState reports.
        float getAnalogState(const RegisteredShortcut& shortcut) const;
        float getAnalogStateAxis(const ShortcutInput& input) const;

        // OPENRCT2MINI gamepad-plan 1.6b step 1: read-side accessor for
        // the active input context. Cached value, refreshed at the top
        // of each process() call. Consumers (e.g. UiContext::Process-
        // VirtualGamepadCursor's per-context dispatch, the SDL_KEYDOWN
        // OSK routing intercept) ask the manager rather than each
        // calling Windows::OskIsActive() directly — single source of
        // truth.
        InputContext getActiveContext() const
        {
            return _activeContext;
        }

        // OPENRCT2MINI per-binding Modifier mode: returns true if all
        // the keys in `id`'s binding's chord that come before the
        // trigger (i.e., the "modifier" portion) are currently held,
        // regardless of trigger state. Returns false if `id` has no
        // binding, or if the binding has fewer than 2 keys (no
        // modifier portion). Used by action lambdas that want to
        // detect "modifier still held after fire" for commit-on-
        // release UX. Walks all of the shortcut's current[] bindings
        // and returns true if ANY of them satisfies the predicate
        // (so a shortcut bound to both keyboard CTRL+C and gamepad
        // L1+B can be queried generically).
        bool isShortcutModifierHeld(std::string_view id) const;

        // OPENRCT2MINI gamepad-plan 1.10: per-context shortcut allow-list
        // gate. Returns true if the given shortcut id is permitted to
        // fire in the currently-active input context. Used by
        // ShortcutManager::processEvent (via a filter lambda passed
        // from InputManager::process) so the same allow-list applies
        // to every device kind — keyboard, mouse, joyButton, joyHat,
        // joyAxis. The InputEvent is reserved for future per-event
        // refinements (e.g. context-specific binding overrides) but
        // not consulted in the current implementation.
        //
        // World context is the default and allows everything; every
        // other context is a small allow-list anchored on
        // kInterfaceDismiss / kInterfaceConfirm so PAD BACK / PAD START
        // always close or commit modals. See InputManager.cpp for the
        // per-context allow-list table.
        bool isShortcutAllowedInActiveContext(const InputEvent& e, std::string_view shortcutId) const;

        // OPENRCT2MINI input-plan Track 3 / Phase 3.I.a: programmatic
        // accessor. Same allow-list logic as isShortcutAllowedInActive-
        // Context but parameterised on context so callers can ask
        // "is this shortcut meaningful in toolFootpath?" without
        // pretending that's the active context. The Input Bindings
        // rebind UI can iterate the InputContext enum to compute an
        // "active in: World, toolFootpath, toolTerrain" tag-list per
        // shortcut. Conflict detection can use this to recognise that
        // two shortcuts both bound to PAD A but meaningful in
        // non-coexisting contexts don't actually conflict at runtime.
        bool isShortcutMeaningfulInContext(std::string_view shortcutId, InputContext context) const;

        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: strategy access.
        // Returns the strategy registered for the currently-active
        // InputContext. Falls back to the world stub if no strategy
        // is registered for that slot. Never returns nullptr — the
        // world stub is initialised at construction and serves as the
        // universal fallback.
        IInputContext& getActiveContextStrategy() const;

        // OPENRCT2MINI grid-cursor-plan §14.2 polish 3 (2026-05-20):
        // probe every registered context strategy for a Grid /
        // EdgeCursorModel and return its accumulated Z offset. The
        // active-strategy version (above) only sees the foreground
        // ToolContext; once the user backs out to widgetFocus
        // (parked state), the active strategy is widgetFocus and
        // its cursor model has no Z. The ToolContext is still in
        // the registry, just not active — its cursor model preserves
        // the user's Z across the parked transition. SyncHidden-
        // CursorParking uses this to keep the parked cursor sprite
        // at the raised Z position instead of dropping it back to
        // the surface.
        int32_t getAnyRegisteredCursorZ() const;

        // OPENRCT2MINI grid-cursor-plan §11.11 polish (2026-05-30
        // follow-up #4): walk every registered ToolContext for a
        // non-zero cursorParkZExtra(worldCoord). Parallel to
        // getAnyRegisteredCursorZ above but for the per-tool
        // EXTRA Z offset (the constant that lifts e.g. the peep
        // pickup pincers sprite above the drop height, or the
        // water tool's cursor to the water surface). Needed
        // because SyncHiddenCursorParking's parked-path branches
        // (which run when the user backs out of grid mode to
        // widgetFocus while the tool stays armed) don't have an
        // active ToolContext to query via
        // getActiveContextStrategy() — the active strategy at
        // that moment is widgetFocus, not the tool. The tool is
        // still in the registry, just dormant, but its
        // cursorParkZExtra is a pure function of worldCoord
        // (no per-instance state), so querying the dormant slot
        // returns the same answer as querying it when active.
        // Returns the FIRST non-zero value encountered; 0 when
        // all tools return 0. Single-tool-at-a-time usage means
        // first-non-zero is safe in practice.
        int32_t getAnyRegisteredCursorParkZExtra(CoordsXY worldCoord) const;

        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: routing entry.
        // Called from ShortcutManager::processEvent before firing a
        // matched binding's action lambda. The active strategy decides
        // Consumed (suppress lambda) or Passthrough (fire normally).
        // Phase 3.A's stub returns Passthrough for everything, so
        // behaviour is byte-identical to the pre-routing state.
        //
        // Lives on InputManager rather than ShortcutManager so the
        // routing knows which context is active without
        // ShortcutManager needing a back-pointer to InputManager.
        bool shouldSuppressAction(std::string_view shortcutId, const InputEvent& e) const;
    };
} // namespace OpenRCT2::Ui
