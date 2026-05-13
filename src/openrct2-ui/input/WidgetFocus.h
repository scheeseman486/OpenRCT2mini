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

} // namespace OpenRCT2::Ui::WidgetFocus
