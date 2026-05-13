/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

// OPENRCT2MINI focus-mode-plan / Phase F.2: widget-focus navigation
// helpers. Four pure utilities the InputManager and the
// WidgetFocusContextImpl strategy compose to navigate spatially
// between focusable widgets in the active window. Each function is a
// pure transformation of its inputs — no global state, no
// InputManager dependency at the call site (the strategy owns the
// focus state and just plugs these helpers in).
//
// See focus-mode-plan.md §3.3 for the design and §F.2 for the
// acceptance criteria.

#include <openrct2/interface/Widget.h> // for WidgetIndex, kWidgetIndexNull
#include <openrct2/interface/WindowClasses.h> // for WindowClass
#include <openrct2/world/Location.hpp> // for ScreenCoordsXY, ScreenRect

#include <cstddef>
#include <cstdint>

// Forward declarations to keep this header light. WindowBase lives
// in the OpenRCT2 namespace — the forward decl must match or the
// helper signatures resolve to a different type than the call sites
// expect (the .cpp's `OpenRCT2::WindowBase` from the full include).
// RenderTarget is in OpenRCT2::Drawing; forward-decl to avoid
// pulling Drawing.h into every consumer of this header.
namespace OpenRCT2
{
    struct WindowBase;
    namespace Drawing
    {
        struct RenderTarget;
    }
}

namespace OpenRCT2::Ui::WidgetFocus
{
    // Cardinal step direction for spatial navigation. Local to focus-
    // mode work — OpenRCT2 has no global Direction enum for cardinal
    // navigation that fits this use. Order matches the four
    // cursor.up/down/left/right shortcut IDs.
    enum class Direction : uint8_t
    {
        up = 0,
        down,
        left,
        right,
    };

    // Per-Widget focusable predicate. Returns true if and only if:
    //   - the widget's type is one of the user-interactive types
    //     (button, checkbox, dropdown, spinner, scroll, textBox, tab,
    //     image / flat / colour / translucent button), AND
    //   - the widget is currently visible (Widget::isVisible() —
    //     checks both isHidden and shadeHidden flags). Invisible
    //     widgets must never receive focus because the user can't see
    //     where it would land.
    // Decorative types (frame, label, viewport, groupbox, etc.)
    // return false unconditionally — even when visible, they aren't
    // user-interactive.
    bool isFocusable(const Widget& widget);

    // Spatial search: walk `window.widgets` and find the focusable
    // widget whose centre is in `dir` from the widget at `from`,
    // choosing the one with minimum cost = |Δ in scan direction| +
    // 2 × |Δ perpendicular|. The 2× perpendicular bias keeps focus
    // roughly in line during vertical or horizontal sweeps. Returns
    // kWidgetIndexNull when no candidate exists in `dir` AND wrap-
    // around finds nothing — i.e. there are no other focusable
    // widgets in the window.
    //
    // Wrap-around: if no candidate exists in the requested direction
    // (focus is on the edge widget), retry with the cost flipped so
    // candidates on the opposite side become valid. The user gets
    // console-style "D-pad-right at the rightmost widget moves to the
    // leftmost" behaviour for free. Pass `from == kWidgetIndexNull`
    // to skip the "in direction" filter entirely — useful for the
    // first-focus path which has no current widget to reference.
    WidgetIndex findNearestInDirection(
        const WindowBase& window, WidgetIndex from, Direction dir);

    // Return the index of the first focusable widget in the window's
    // widget list, in declaration order. Returns kWidgetIndexNull if
    // the window has no focusable widgets at all. The strategy uses
    // this on context activation to snap to a sensible initial
    // selection.
    WidgetIndex firstFocusable(const WindowBase& window);

    // OPENRCT2MINI: identify windows that are purely visual chrome —
    // tooltip pop-ups and the map hover-identification overlay — and
    // therefore must never be considered by the focus-mode bootstrap
    // (snap / per-frame auto-track) or the window switcher (cycle
    // next / prev). These windows have no user-actionable widgets, so
    // focusing them would just leave the user stranded; they also
    // appear and disappear constantly on cursor motion, which would
    // make the focus ring jitter if they qualified for snapping.
    bool isPurelyVisualWindow(WindowClass cls);

    // OPENRCT2MINI focus-mode-plan §F.cycle: return the next focusable
    // widget after `from` in declaration order, wrapping back to the
    // start of the widget list when needed. Same focusable predicate
    // and dropdown-chevron skip as firstFocusable, so the returned
    // index is a valid landing spot for setFocus. When `from ==
    // kWidgetIndexNull` (no current focus), behaves exactly like
    // firstFocusable. Returns kWidgetIndexNull only when the window
    // has zero focusable widgets at all; if there is exactly one,
    // returns that same index (cycling onto itself — caller can no-
    // op or live with it). Used by the kInterfaceEnterFocusMode
    // shortcut to cycle through widgets when focus mode is already
    // active in the topmost window.
    WidgetIndex nextFocusable(const WindowBase& window, WidgetIndex from);

    // Synthesise a press on a specific widget by directly invoking
    // its window's onMouseDown handler. Bypasses cursor hit-testing —
    // useful when the focus state already identifies which widget
    // the user means to press. Mouse-event side-effects from
    // hit-testing (tooltip positioning, hover state) are NOT
    // reproduced; the gamepad path doesn't have a pixel cursor to
    // hover.
    //
    // Safe no-op if `idx` is out of range or kWidgetIndexNull. The
    // strategy calls this from the cursor.click → onPlace path.
    void pressWidgetByIndex(WindowBase& window, WidgetIndex idx);

    // OPENRCT2MINI list-focus-plan §2.1: predicate for list-mode
    // scroll widgets. Returns true iff the widget at `idx` is a
    // WidgetType::scroll AND the window has opted in to per-item
    // focus by overriding scrollFocusGetItemCount to return > 0.
    // Centralised so multiple call sites (isFocusable, the
    // directional dispatch in WidgetFocusContextImpl) stay in sync.
    bool isListModeScroll(const WindowBase& window, WidgetIndex idx);

    // OPENRCT2MINI list-focus-plan §2.3: ensure the focused list
    // item is inside the visible scroll viewport. Adjusts the
    // window's scrolls[scrollIdx].contentOffsetX/Y so the item's
    // content-local rect sits inside the visible area. Clamped to
    // valid scroll range. Called by the directional dispatch on
    // every list-item step.
    void ensureScrollItemVisible(
        WindowBase& window, WidgetIndex scrollWidget, int32_t itemIndex);

    // OPENRCT2MINI list-focus-plan §2.1: convert a content-local
    // item rect (as returned by scrollFocusGetItemRect) into the
    // on-screen rect for the focus-ring renderer — applies window
    // position, scroll widget origin, and live scroll offset.
    // ScreenRect and ScreenCoordsXY live in the global namespace
    // (see world/Location.hpp:70, 885) — no OpenRCT2:: prefix.
    ::ScreenRect contentRectToOnScreen(
        const WindowBase& window, WidgetIndex scrollWidget,
        const ::ScreenRect& contentRect);

    // OPENRCT2MINI list-focus-plan §C1: spatially-nearest entry
    // point for a list-mode scroll widget. Given a previous on-
    // screen rect (e.g. the previous focus widget the user is
    // stepping from), find the item index in `scrollWidget` whose
    // on-screen position is closest to it. Used when focus first
    // enters a list-mode scroll widget — gives the user the item
    // closest to wherever the ring was, not always item 0.
    int32_t nearestScrollItemTo(
        WindowBase& window, WidgetIndex scrollWidget,
        const ::ScreenCoordsXY& reference);

    // Draw a focus ring over the widget at `idx` in `window`. Called
    // by the shared window-paint code (WindowDrawSingle) after the
    // window's own onDraw, so the ring sits on top of whatever the
    // window normally renders. Uses Rectangle::fillInset with
    // brightYellow + outset border + FillMode::none — same recipe the
    // OSK uses for its key highlight today, kept consistent so all
    // focused widgets look the same regardless of window.
    //
    // Safe no-op if `idx` is out of range, kWidgetIndexNull, or
    // resolves to a non-focusable widget (defensive — focus state
    // should never point at one, but the render path shouldn't crash
    // if it does). Coordinates are translated into screen space via
    // `window.windowPos`; the focus ring respects the window's
    // current position.
    void drawFocusOutline(
        OpenRCT2::Drawing::RenderTarget& rt, const WindowBase& window, WidgetIndex idx);

    // OPENRCT2MINI list-focus-plan §2.4: draw a focus ring around a
    // single list item inside a list-mode scroll widget. Used by the
    // bridge when _focusedScrollItem >= 0; otherwise the bridge calls
    // drawFocusOutline on the widget itself. Translates the window's
    // content-local item rect to screen space via contentRectToOnScreen
    // and applies the same brightYellow/outset/none recipe so the ring
    // looks identical to the widget-level outline.
    //
    // Safe no-op when the widget at `scrollWidget` is not a scroll
    // type, the window has not opted in (scrollFocusGetItemCount
    // returns 0), or `itemIndex` is out of range.
    void drawListItemFocusOutline(
        OpenRCT2::Drawing::RenderTarget& rt, const WindowBase& window,
        WidgetIndex scrollWidget, int32_t itemIndex);

    // ────────────────────────────────────────────────────────────────
    // OPENRCT2MINI focus-mode-plan §F.17 / window-set-plan.md:
    // Window-set abstraction. A set is a list of WindowClass values
    // the selector treats as ONE logical navigable surface. Members
    // keep their separate WindowBase objects, lifecycles, draw code;
    // the set only changes which widgets the directional navigator
    // considers and how the bootstrap / focus-stack rules count
    // "same window."
    //
    // Sets live in a static registry seeded in WidgetFocus.cpp. The
    // two initial sets are:
    //   - in-game chrome: topToolbar + gameBottomToolbar
    //   - title scene  : titleLogo  + titleMenu
    // Adding a set is one line in the registry; runtime members are
    // filtered by aliveness at lookup time so the set degrades
    // gracefully if a member doesn't exist in the current scene.

    struct WindowSet
    {
        // Pointer to a constexpr WindowClass array + element count.
        // Avoids std::span (C++20) since the project is currently
        // built with -std=gnu++17. The arrays live in WidgetFocus.cpp
        // and are static, so the pointer is stable.
        const WindowClass* members;
        std::size_t memberCount;
        // Preferred starting widget when focus first enters the set
        // (e.g. via bootstrap snap). Belongs to members[0] unless
        // overridden by an explicit (class, idx) pair below.
        WindowClass defaultClass{ WindowClass::null };
        WidgetIndex defaultWidget{ kWidgetIndexNull };
        // OPENRCT2MINI cursor-selector-modal-plan §2.2: when true,
        // a cursor.cancel fired while focus is in this set
        // transitions the selector state machine to `hidden` instead
        // of clearing focus the usual way. Used for the in-game
        // chrome set (top+bottom toolbars) — there is nothing
        // logically "underneath" the toolbar for cancel to back into,
        // so cancel means "I'm done with the selector."
        bool isBottomOfStack{ false };
    };

    // Returns a pointer to the set containing `cls`, or nullptr if
    // `cls` isn't a registered set member. Pointer is to static
    // storage and stable for the lifetime of the process.
    const WindowSet* findSetFor(WindowClass cls);

    // True if both classes are in the same set (or are the same
    // class). Used by the bootstrap / snap / focus-stack to count
    // "same logical surface" — a hop between set members should not
    // trigger a re-snap or push a stack entry.
    bool sameSetOrClass(WindowClass a, WindowClass b);

    // Variant of findNearestInDirection that walks across all live
    // members of a set in screen space. Falls through to the single-
    // window variant when `from` is in no set. Returns the winning
    // (class, widget) — the caller transitions focus by calling
    // setFocus on those. kWidgetIndexNull return signals "no
    // candidate, even with wrap" (entire set has only one focusable
    // widget). When the winner is in a different member than the
    // current focus, *outClass is set to that member's class;
    // otherwise it matches `fromClass`.
    WidgetIndex findNearestInSetDirection(
        WindowClass fromClass, WidgetIndex fromWidget, Direction dir,
        WindowClass* outClass);

    // OPENRCT2MINI focus-mode-plan §F.cycle (set-aware variant): walk
    // forward through the focused window's set, advancing past
    // (fromClass, fromWidget) in declaration order. Steps within the
    // current member's widget list first; when there are no more
    // focusable widgets after fromWidget, advances to the next live
    // set member's firstFocusable. Wraps around to the first member's
    // first focusable widget when the end of the set is hit. Skips
    // dead / unloaded members.
    //
    // For a class that isn't in any set, behaves exactly like
    // nextFocusable on the single window (with wrap within that
    // window).
    //
    // `*outClass` receives the winner's class — same as `fromClass`
    // when the cycle stayed in the current member, a sibling member
    // otherwise. May be nullptr if the caller doesn't need it.
    // Returns kWidgetIndexNull only when the entire set has zero
    // focusable widgets across every live member.
    WidgetIndex nextFocusableInSet(
        WindowClass fromClass, WidgetIndex fromWidget, WindowClass* outClass);

} // namespace OpenRCT2::Ui::WidgetFocus
