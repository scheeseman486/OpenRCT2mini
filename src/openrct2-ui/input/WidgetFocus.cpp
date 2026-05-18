/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI focus-mode-plan / Phase F.2: widget-focus helpers
// implementation. See WidgetFocus.h for the API contract and
// focus-mode-plan.md §3.3 for the design.

#include "WidgetFocus.h"

#include "InputManager.h"

#include <openrct2/Input.h>
#include <openrct2/drawing/Colour.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WidgetFocusBridge.h>
#include <openrct2/world/Location.hpp>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/UiStringIds.h>

#include <openrct2/ui/WindowManager.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <vector>

namespace OpenRCT2::Ui::WidgetFocus
{
    bool isFocusable(const Widget& widget)
    {
        // Visibility gate first — invisible widgets (hidden or
        // shade-hidden) must never receive focus regardless of type.
        // The sanity-check review (focus-mode-plan-review.md §F1)
        // surfaced this requirement explicitly.
        if (!widget.isVisible())
            return false;
        switch (widget.type)
        {
            case WidgetType::button:
            case WidgetType::imgBtn:
            case WidgetType::flatBtn:
            case WidgetType::colourBtn:
            case WidgetType::trnBtn:
            case WidgetType::checkbox:
            case WidgetType::spinner:
            case WidgetType::dropdownMenu:
            case WidgetType::scroll:
            case WidgetType::textBox:
            case WidgetType::tab:
                return true;
            // OPENRCT2MINI list-focus-plan §6.7: tableHeader is the
            // OpenRCT2 widget type for clickable column headers above
            // a scroll list (e.g. RideList's WIDX_HEADER_NAME /
            // WIDX_HEADER_OTHER sort buttons). They fire onMouseUp on
            // click — same dispatch the focus path uses via
            // pressWidgetByIndex — so they should be reachable as
            // focus stops. Decorative-only consumers of tableHeader
            // (if any) won't break: pressing a header with no
            // onMouseUp case is a silent no-op.
            case WidgetType::tableHeader:
                return true;
            default:
                return false;
        }
    }

    namespace
    {
        // Local helpers (not part of the public API).

        // OPENRCT2MINI focus-mode-plan §F.11: dropdown trigger
        // pairing. The OpenRCT2 convention expresses a dropdown
        // control as TWO adjacent widgets:
        //   [N]   WidgetType::dropdownMenu  — wide label / text strip
        //   [N+1] WidgetType::button        — arrow chevron with text
        //                                     == STR_DROPDOWN_GLYPH
        // Mouse clicks on the chevron dispatch the parent's
        // onMouseDown for index N+1, which opens the dropdown. The
        // dropdownMenu widget itself (index N) has no click handler;
        // clicking the text strip is a no-op in real mouse handling.
        //
        // For focus we want them perceived as one button: one focus
        // stop, one press opens, ring spans both. We (a) skip the
        // chevron in enumeration, (b) extend the ring across the
        // union, and (c) redirect a focus.click on the dropdownMenu
        // to fire onMouseDown at the chevron's index.

        // True iff widget[idx] is the chevron half of a dropdown
        // pair: a `button` whose text is STR_DROPDOWN_GLYPH and
        // whose preceding widget is `dropdownMenu`.
        bool isDropdownChevron(const WindowBase& window, WidgetIndex idx)
        {
            if (idx == kWidgetIndexNull)
                return false;
            const auto count = window.widgets.size();
            if (idx == 0 || static_cast<size_t>(idx) >= count)
                return false;
            const auto& w = window.widgets[idx];
            if (w.type != WidgetType::button)
                return false;
            if (w.text != STR_DROPDOWN_GLYPH)
                return false;
            const auto& prev = window.widgets[static_cast<size_t>(idx) - 1];
            return prev.type == WidgetType::dropdownMenu;
        }

        // True iff widget[idx] is a dropdownMenu paired with a
        // chevron at +1. Drives focus-ring union bounds and the
        // click-redirect in pressWidgetByIndex.
        bool hasDropdownChevron(const WindowBase& window, WidgetIndex idx)
        {
            if (idx == kWidgetIndexNull)
                return false;
            const auto count = window.widgets.size();
            if (static_cast<size_t>(idx) + 1 >= count)
                return false;
            const auto& w = window.widgets[idx];
            if (w.type != WidgetType::dropdownMenu)
                return false;
            const auto& next = window.widgets[static_cast<size_t>(idx) + 1];
            return next.type == WidgetType::button && next.text == STR_DROPDOWN_GLYPH;
        }

        struct WidgetCentre
        {
            int32_t x;
            int32_t y;
        };

        // Widget centre in window-local coordinates. Screen coords
        // would require windowPos but cost-comparisons are
        // translation-invariant so window-local is fine.
        WidgetCentre centreOf(const Widget& w) noexcept
        {
            return { static_cast<int32_t>(w.midX()), static_cast<int32_t>(w.midY()) };
        }

        // Is `candidate` "on the right side of" `from` for the given
        // direction? Used to filter candidates before cost-ranking.
        bool isInDirection(const WidgetCentre& from, const WidgetCentre& candidate, Direction dir) noexcept
        {
            switch (dir)
            {
                case Direction::up:    return candidate.y < from.y;
                case Direction::down:  return candidate.y > from.y;
                case Direction::left:  return candidate.x < from.x;
                case Direction::right: return candidate.x > from.x;
            }
            return false;
        }

        // cost = |Δ in scan direction| + 2 × |Δ perpendicular|. The 2×
        // perpendicular bias keeps focus roughly in line during sweeps.
        // The constant `2` matches the focus-mode-plan §3.4 design;
        // tunable via Open Question Q2 once Phase F.4 testing shows
        // whether it feels right on real window layouts.
        int64_t directionalCost(const WidgetCentre& from, const WidgetCentre& candidate, Direction dir) noexcept
        {
            const int64_t dx = std::abs(static_cast<int64_t>(candidate.x) - from.x);
            const int64_t dy = std::abs(static_cast<int64_t>(candidate.y) - from.y);
            switch (dir)
            {
                case Direction::up:
                case Direction::down:
                    return dy + 2 * dx;
                case Direction::left:
                case Direction::right:
                    return dx + 2 * dy;
            }
            return std::numeric_limits<int64_t>::max();
        }

        // Wrap-around cost: distance maximised in the OPPOSITE
        // direction so the search finds the candidate that's "furthest
        // wrong" — i.e. the leftmost when wrapping for D-pad-right.
        // Returns negative cost so smaller-cost-wins still selects the
        // right candidate. The "perpendicular" axis keeps its
        // alignment bias.
        int64_t wrapAroundCost(const WidgetCentre& from, const WidgetCentre& candidate, Direction dir) noexcept
        {
            const int64_t dx = static_cast<int64_t>(candidate.x) - from.x;
            const int64_t dy = static_cast<int64_t>(candidate.y) - from.y;
            // For wrap: prefer candidates furthest in the OPPOSITE
            // direction (negative cost), penalise by perpendicular
            // distance.
            switch (dir)
            {
                case Direction::up:    return dy + 2 * std::abs(dx);    // dy >= 0 (candidate below — opposite of up)
                case Direction::down:  return -dy + 2 * std::abs(dx);   // candidate above
                case Direction::left:  return dx + 2 * std::abs(dy);
                case Direction::right: return -dx + 2 * std::abs(dy);
            }
            return std::numeric_limits<int64_t>::max();
        }
    } // namespace

    WidgetIndex findNearestInDirection(const WindowBase& window, WidgetIndex from, Direction dir)
    {
        const auto count = window.widgets.size();
        if (count == 0)
            return kWidgetIndexNull;

        // The "from" widget centre — used both as the directional
        // anchor for the in-direction search and for the wrap-around
        // fallback's opposite-direction sort. If `from` is kWidget-
        // IndexNull (no current focus), use the window centre as a
        // neutral reference so first focus tends to land near the
        // middle.
        WidgetCentre fromCentre{};
        if (from != kWidgetIndexNull && from < count)
        {
            fromCentre = centreOf(window.widgets[from]);
        }
        else
        {
            // No current focus — pick the spatial centre of the window
            // as the anchor for "nearest in dir." Widgets are in
            // window-local coords so the centre is half the widget
            // bounding box. Use (0, 0) as a coarse anchor; the cost
            // metric will naturally prefer the closest focusable
            // widget along the desired axis.
            fromCentre = { 0, 0 };
        }

        // First pass: candidates in the requested direction. Cost-
        // minimising selection picks the nearest by the weighted
        // formula. Skip the `from` widget itself.
        WidgetIndex best = kWidgetIndexNull;
        int64_t bestCost = std::numeric_limits<int64_t>::max();
        for (size_t i = 0; i < count; i++)
        {
            if (static_cast<WidgetIndex>(i) == from)
                continue;
            const auto& w = window.widgets[i];
            if (!isFocusable(w))
                continue;
            // Dropdown chevron buttons are passengers of their
            // dropdownMenu neighbour; skip them so the pair acts as
            // a single focus stop. See isDropdownChevron comment for
            // the rationale.
            if (isDropdownChevron(window, static_cast<WidgetIndex>(i)))
                continue;
            // OPENRCT2MINI list-focus-plan §2.1: skip opt-out scrolls.
            if (w.type == WidgetType::scroll
                && !isListModeScroll(window, static_cast<WidgetIndex>(i)))
                continue;
            const auto cand = centreOf(w);
            // If we have a real `from`, filter to "in direction" only.
            // For the no-current-focus case (from == kWidgetIndexNull),
            // we still bias by direction but allow candidates anywhere
            // in the window — the cost metric handles ranking.
            if (from != kWidgetIndexNull && !isInDirection(fromCentre, cand, dir))
                continue;
            const auto cost = directionalCost(fromCentre, cand, dir);
            if (cost < bestCost)
            {
                bestCost = cost;
                best = static_cast<WidgetIndex>(i);
            }
        }
        if (best != kWidgetIndexNull)
            return best;

        // Wrap-around fallback: nothing in the requested direction, so
        // search the OPPOSITE side. Pick the candidate "furthest
        // wrong" — for D-pad-right when at the rightmost widget, that
        // picks the leftmost focusable. Skip if `from` is null
        // (nothing to wrap relative to; first-focus case already
        // handled above by the cost minimiser).
        if (from == kWidgetIndexNull)
            return kWidgetIndexNull;
        int64_t wrapBest = std::numeric_limits<int64_t>::max();
        for (size_t i = 0; i < count; i++)
        {
            if (static_cast<WidgetIndex>(i) == from)
                continue;
            const auto& w = window.widgets[i];
            if (!isFocusable(w))
                continue;
            if (isDropdownChevron(window, static_cast<WidgetIndex>(i)))
                continue;
            if (w.type == WidgetType::scroll
                && !isListModeScroll(window, static_cast<WidgetIndex>(i)))
                continue;
            const auto cand = centreOf(w);
            // The wrap-around cost prefers candidates on the opposite
            // side (negative cost values for the desired side). We
            // still minimise, so the most-negative wins.
            const auto cost = wrapAroundCost(fromCentre, cand, dir);
            if (cost < wrapBest)
            {
                wrapBest = cost;
                best = static_cast<WidgetIndex>(i);
            }
        }
        return best;
    }

    WidgetIndex firstFocusable(const WindowBase& window)
    {
        const auto count = window.widgets.size();
        for (size_t i = 0; i < count; i++)
        {
            if (!isFocusable(window.widgets[i]))
                continue;
            // Skip dropdown chevrons here too — they pair with the
            // preceding dropdownMenu and shouldn't be a separate
            // landing spot for first-focus.
            if (isDropdownChevron(window, static_cast<WidgetIndex>(i)))
                continue;
            // OPENRCT2MINI list-focus-plan §2.1 / C2: scroll widgets
            // are only focus stops when the window opts in to list-
            // mode focus. Opt-out scroll widgets are skipped.
            if (window.widgets[i].type == WidgetType::scroll
                && !isListModeScroll(window, static_cast<WidgetIndex>(i)))
                continue;
            return static_cast<WidgetIndex>(i);
        }
        return kWidgetIndexNull;
    }

    // OPENRCT2MINI: purely visual / non-interactive window classes.
    // The tooltip pop-up (rendered when the cursor hovers over an
    // interactive widget) and the map tooltip (hover-identification
    // text over rides / peeps / entrances) both spawn and despawn on
    // cursor motion, and neither contains anything the user can
    // activate. Focusing them would jitter the ring every time the
    // cursor moves and would leave the user stranded with no clicks
    // available; the window switcher likewise mustn't stop on them.
    //
    // Add new classes here as they're identified; this is the single
    // central filter, consulted by the focus bootstrap, snap, and
    // cycle paths in InputManager.cpp.
    // OPENRCT2MINI list-focus-plan §2.1: predicate for list-mode
    // scroll widgets. Mode-aware — a scroll widget is "in list mode"
    // (per-item focus) iff the window has opted in by overriding
    // scrollFocusGetItemCount to return > 0. Opt-out scroll widgets
    // are NOT focus stops at all post-list-focus (the framework
    // default drops them) — see C2 in list-focus-plan.md.
    bool isListModeScroll(const WindowBase& window, WidgetIndex idx)
    {
        if (idx == kWidgetIndexNull || static_cast<size_t>(idx) >= window.widgets.size())
            return false;
        const auto& w = window.widgets[idx];
        if (w.type != WidgetType::scroll)
            return false;
        const auto scrollIdx = WindowGetScrollDataIndex(window, idx);
        return const_cast<WindowBase&>(window).scrollFocusGetItemCount(scrollIdx) > 0;
    }

    ScreenRect contentRectToOnScreen(
        const WindowBase& window, WidgetIndex scrollWidget, const ScreenRect& contentRect)
    {
        if (scrollWidget == kWidgetIndexNull || static_cast<size_t>(scrollWidget) >= window.widgets.size())
            return {};
        const auto& wd = window.widgets[scrollWidget];
        if (wd.type != WidgetType::scroll)
            return {};
        const auto scrollIdx = WindowGetScrollDataIndex(window, scrollWidget);
        const auto& s = window.scrolls[scrollIdx];
        // Scroll widget's content area excludes the bar regions
        // (kBarWidth typically 11 px). For ring rendering we accept
        // a small overpaint into the bar if the item rect is at the
        // edge — clipping is the renderer's job. The content origin
        // sits at (widget.left + 1, widget.top + 1) by convention;
        // live scroll offset is subtracted to translate from
        // content-local to on-screen.
        const int32_t baseX = window.windowPos.x + wd.left + 1 - s.contentOffsetX;
        const int32_t baseY = window.windowPos.y + wd.top + 1 - s.contentOffsetY;
        return {
            { baseX + contentRect.GetLeft(), baseY + contentRect.GetTop() },
            { baseX + contentRect.GetRight(), baseY + contentRect.GetBottom() },
        };
    }

    void ensureScrollItemVisible(WindowBase& window, WidgetIndex scrollWidget, int32_t itemIndex)
    {
        if (scrollWidget == kWidgetIndexNull || static_cast<size_t>(scrollWidget) >= window.widgets.size())
            return;
        const auto& wd = window.widgets[scrollWidget];
        if (wd.type != WidgetType::scroll)
            return;
        const auto rect = window.scrollFocusGetItemRect(
            WindowGetScrollDataIndex(window, scrollWidget), itemIndex);
        if (rect.GetLeft() >= rect.GetRight() || rect.GetTop() >= rect.GetBottom())
            return;
        const auto scrollIdx = WindowGetScrollDataIndex(window, scrollWidget);
        auto& s = window.scrolls[scrollIdx];
        // OPENRCT2MINI list-focus-plan fix: only adjust the axis the
        // scroll widget can actually scroll along. WidgetScrollDraw
        // applies BOTH contentOffsetX and contentOffsetY to the
        // scroll render target (Widget.cpp:803-804) regardless of
        // whether the widget has HSCROLLBAR_VISIBLE / VSCROLLBAR_VISIBLE
        // — so blindly setting contentOffsetX on a SCROLL_VERTICAL-only
        // widget shifts its content off-screen. Item rects from
        // scrollFocusGetItemRect frequently span the full conceptual
        // row width (which can exceed the viewport), so without this
        // gate a vertical step would trigger a phantom horizontal
        // scroll and the entire list would appear blank.
        const bool canScrollH = (s.flags & HSCROLLBAR_VISIBLE) != 0;
        const bool canScrollV = (s.flags & VSCROLLBAR_VISIBLE) != 0;
        if (canScrollH)
        {
            const int32_t viewW = std::max(0, wd.right - wd.left - 2);
            const int32_t maxX = std::max(0, static_cast<int32_t>(s.contentWidth) - viewW);
            if (rect.GetLeft() < s.contentOffsetX)
                s.contentOffsetX = rect.GetLeft();
            else if (rect.GetRight() > s.contentOffsetX + viewW)
                s.contentOffsetX = rect.GetRight() - viewW;
            s.contentOffsetX = std::clamp(s.contentOffsetX, 0, maxX);
        }
        if (canScrollV)
        {
            const int32_t viewH = std::max(0, wd.bottom - wd.top - 2);
            const int32_t maxY = std::max(0, static_cast<int32_t>(s.contentHeight) - viewH);
            if (rect.GetTop() < s.contentOffsetY)
                s.contentOffsetY = rect.GetTop();
            else if (rect.GetBottom() > s.contentOffsetY + viewH)
                s.contentOffsetY = rect.GetBottom() - viewH;
            s.contentOffsetY = std::clamp(s.contentOffsetY, 0, maxY);
        }
        // OPENRCT2MINI list-focus-plan fix: recompute scrollbar thumb
        // geometry to match the new contentOffset. The thumb position
        // (hThumbLeft/Right, vThumbTop/Bottom) is cached in the
        // ScrollArea — WidgetVScrollbarDraw / WidgetHScrollbarDraw
        // read those cached values directly, so without this call the
        // bar stays at the old position visually while the content
        // scrolls underneath. Same call mouse drag / wheel paths
        // make after modifying contentOffset (Window.cpp:114,
        // MouseInput.cpp:223, etc.).
        widgetScrollUpdateThumbs(window, scrollWidget);
        window.invalidate();
    }

    int32_t nearestScrollItemTo(
        WindowBase& window, WidgetIndex scrollWidget, const ScreenCoordsXY& reference)
    {
        if (scrollWidget == kWidgetIndexNull || static_cast<size_t>(scrollWidget) >= window.widgets.size())
            return -1;
        const auto scrollIdx = WindowGetScrollDataIndex(window, scrollWidget);
        const int32_t n = window.scrollFocusGetItemCount(scrollIdx);
        if (n <= 0)
            return -1;
        int32_t best = 0;
        int64_t bestCost = std::numeric_limits<int64_t>::max();
        for (int32_t i = 0; i < n; ++i)
        {
            const auto cr = window.scrollFocusGetItemRect(scrollIdx, i);
            if (cr.GetLeft() >= cr.GetRight() || cr.GetTop() >= cr.GetBottom())
                continue;
            const auto onScreen = contentRectToOnScreen(window, scrollWidget, cr);
            const int64_t cx = (onScreen.GetLeft() + onScreen.GetRight()) / 2;
            const int64_t cy = (onScreen.GetTop() + onScreen.GetBottom()) / 2;
            const int64_t dx = cx - reference.x;
            const int64_t dy = cy - reference.y;
            const int64_t cost = dx * dx + dy * dy;
            if (cost < bestCost)
            {
                bestCost = cost;
                best = i;
            }
        }
        return best;
    }

    bool isPurelyVisualWindow(WindowClass cls)
    {
        switch (cls)
        {
            case WindowClass::tooltip:
            case WindowClass::mapTooltip:
                return true;
            default:
                return false;
        }
    }

    // OPENRCT2MINI focus-mode-plan §F.cycle: declaration-order
    // forward step with wrap. Mirrors firstFocusable's filters
    // (focusable predicate + chevron skip) so any landing spot it
    // returns is a valid setFocus target.
    WidgetIndex nextFocusable(const WindowBase& window, WidgetIndex from)
    {
        const auto count = window.widgets.size();
        if (count == 0)
            return kWidgetIndexNull;
        // No current focus → behave like firstFocusable. Cheaper than
        // duplicating its loop here and keeps the two functions in
        // lock-step if the focusable rules ever change.
        if (from == kWidgetIndexNull || static_cast<size_t>(from) >= count)
            return firstFocusable(window);

        // Two-phase walk: [from+1, count) then [0, from]. Visiting
        // `from` itself in phase two means a window with exactly one
        // focusable widget returns that same widget — caller treats
        // it as a no-op cycle.
        for (size_t step = 1; step <= count; step++)
        {
            const size_t i = (static_cast<size_t>(from) + step) % count;
            if (!isFocusable(window.widgets[i]))
                continue;
            if (isDropdownChevron(window, static_cast<WidgetIndex>(i)))
                continue;
            // OPENRCT2MINI list-focus-plan §2.1: opt-out scroll widgets
            // aren't focus stops.
            if (window.widgets[i].type == WidgetType::scroll
                && !isListModeScroll(window, static_cast<WidgetIndex>(i)))
                continue;
            return static_cast<WidgetIndex>(i);
        }
        return kWidgetIndexNull;
    }

    void pressWidgetByIndex(WindowBase& window, WidgetIndex idx)
    {
        if (idx == kWidgetIndexNull || idx >= window.widgets.size())
            return;
        // OPENRCT2MINI focus-mode-plan §F.11: dropdown pair
        // press redirect. When focus is on a dropdownMenu that
        // has an adjacent chevron, the parent window's actual
        // dropdown-opening case statement is keyed on the
        // chevron's widget index (see Options.cpp's
        // DisplayMouseDown switch, every WIDX_*_DROPDOWN case).
        // Pressing the dropdownMenu index directly is a no-op
        // because no window handles that index. Redirect the
        // press to the chevron so focus-mode clicks open the
        // dropdown exactly like a real chevron click would.
        WidgetIndex effective = idx;
        const bool openingDropdown = hasDropdownChevron(window, idx);
        if (openingDropdown)
            effective = static_cast<WidgetIndex>(idx + 1);

        // OPENRCT2MINI focus-mode-plan §F.13: keep dropdowns open
        // after the activating button is released. Without this,
        // the user's physical button release runs through
        // MouseInput::InputStateWidgetPressed's leftRelease arm
        // (the dropdown has already transitioned _inputState to
        // DropdownActive), which by default calls CloseByClass on
        // the dropdown — the popup "disappears the moment you let
        // go of the button". Pre-setting InputFlag::dropdownStayOpen
        // makes that release arm swallow the FIRST release as a
        // latch (and clear it), so the dropdown stays up until the
        // next click selects an item or cancels. Mirrors the
        // Dropdown::Flag::StayOpen mode but applied at the call
        // site instead of plumbed through every WindowDropdownShow*
        // overload.
        if (openingDropdown)
            OpenRCT2::gInputFlags.set(OpenRCT2::InputFlag::dropdownStayOpen);

        // A real mouse click in OpenRCT2 fires BOTH events in
        // order — onMouseDown on press, onMouseUp on release —
        // and different widgets respond to each:
        //   - Most buttons (button, imgBtn, flatBtn, tab,
        //     checkbox, colourBtn, trnBtn) handle onMouseUp.
        //   - Spinners, dropdowns, scrollbar arrows handle
        //     onMouseDown.
        // See MouseInput.cpp:1277 (press path) and :1588 (release
        // path). The first revision of this helper only fired
        // onMouseDown, which broke activation for the entire
        // imgBtn family — title menu buttons, scenario picker
        // tabs, most Options-page entries. Fire both in
        // press-then-release order to match what a real click
        // produces.
        //
        // Update gPressedWidget BEFORE onMouseDown so any
        // downstream code that reads it sees the right parent.
        // The dropdown subsystem is the load-bearing case: when
        // onMouseDown opens a dropdown, the dropdown's close path
        // later reads gPressedWidget to find the parent window to
        // dispatch the selection back to. Without this assignment
        // the dropdown would either dispatch to a stale parent
        // (whatever last got pressed via mouse) or fail to
        // dispatch entirely.
        gPressedWidget.windowClassification = window.classification;
        gPressedWidget.windowNumber = window.number;
        gPressedWidget.widgetIndex = effective;
        window.onMouseDown(effective);
        window.onMouseUp(effective);
    }

    void drawFocusOutline(OpenRCT2::Drawing::RenderTarget& rt, const WindowBase& window, WidgetIndex idx)
    {
        // Defensive bounds: focus state should never point at an
        // invalid index, but the render path must not crash if it
        // does — silently bail.
        if (idx == kWidgetIndexNull || idx >= window.widgets.size())
            return;
        const auto& widget = window.widgets[idx];
        if (!isFocusable(widget))
            return;
        // OPENRCT2MINI focus-mode-plan §F.11: dropdown pair union.
        // When focus lands on a dropdownMenu with an adjacent
        // chevron, the user should perceive them as a single
        // button — extend the ring across both widgets' bounds
        // so the outline encloses the label + arrow as one.
        int16_t left = widget.left;
        int16_t top = widget.top;
        int16_t right = widget.right;
        int16_t bottom = widget.bottom;
        if (hasDropdownChevron(window, idx))
        {
            const auto& chevron = window.widgets[idx + 1];
            left = std::min(left, chevron.left);
            top = std::min(top, chevron.top);
            right = std::max(right, chevron.right);
            bottom = std::max(bottom, chevron.bottom);
        }
        // Translate widget-local coordinates to screen space. Widget
        // {left, top, right, bottom} are window-local; windowPos is
        // the window's top-left in screen space. ScreenRect takes
        // inclusive coords (right/bottom are the last painted pixel,
        // not one-past-the-end).
        const auto rect = ScreenRect{
            { window.windowPos.x + left, window.windowPos.y + top },
            { window.windowPos.x + right, window.windowPos.y + bottom },
        };
        // brightYellow + outset border + FillMode::none: draws the
        // outline only, leaves the widget's existing fill intact.
        // Matches the OSK key-highlight recipe (Osk.cpp:432) so the
        // focus ring looks the same across every window type.
        // `Colour` lives in OpenRCT2::Drawing (Colour.h:20); the
        // global namespace exposes `ColourWithFlags` as a struct
        // via a using-style alias used elsewhere in the codebase
        // (Osk.cpp:433 calls it unqualified), but from this TU the
        // namespace path resolves it without ambiguity.
        OpenRCT2::Drawing::Rectangle::fillInset(
            rt, rect, ColourWithFlags{ OpenRCT2::Drawing::Colour::brightYellow },
            OpenRCT2::Drawing::Rectangle::BorderStyle::outset,
            OpenRCT2::Drawing::Rectangle::FillBrightness::light,
            OpenRCT2::Drawing::Rectangle::FillMode::none);
    }

    void drawListItemFocusOutline(
        OpenRCT2::Drawing::RenderTarget& rt, const WindowBase& window,
        WidgetIndex scrollWidget, int32_t itemIndex)
    {
        // OPENRCT2MINI list-focus-plan §2.4: list-item ring. Defensive
        // bounds + opt-in check first — if anything is off, silently
        // bail so the render path doesn't crash on a stale focus
        // state. The widget index must be in range, point at a
        // WidgetType::scroll, and the host window must have opted in
        // by overriding scrollFocusGetItemCount.
        if (scrollWidget == kWidgetIndexNull
            || static_cast<size_t>(scrollWidget) >= window.widgets.size())
            return;
        if (itemIndex < 0)
            return;
        const auto& wd = window.widgets[scrollWidget];
        if (wd.type != WidgetType::scroll)
            return;
        const auto scrollIdx = WindowGetScrollDataIndex(window, scrollWidget);
        const int32_t itemCount = const_cast<WindowBase&>(window).scrollFocusGetItemCount(scrollIdx);
        if (itemCount <= 0 || itemIndex >= itemCount)
            return;
        const auto contentRect = const_cast<WindowBase&>(window).scrollFocusGetItemRect(scrollIdx, itemIndex);
        if (contentRect.GetLeft() >= contentRect.GetRight()
            || contentRect.GetTop() >= contentRect.GetBottom())
            return;
        // Map content-local → on-screen via the shared helper that
        // owns the (windowPos + widget origin - live scroll offset)
        // arithmetic. Clip to the scroll viewport so an item that
        // scrolled off-screen doesn't paint a ring over the bar
        // chrome (the framework's clip rect would also cull this,
        // but trimming up front avoids overpaint cost).
        const auto onScreen = contentRectToOnScreen(window, scrollWidget, contentRect);
        const int32_t viewLeft = window.windowPos.x + wd.left + 1;
        const int32_t viewTop = window.windowPos.y + wd.top + 1;
        const int32_t viewRight = window.windowPos.x + wd.right - 1;
        const int32_t viewBottom = window.windowPos.y + wd.bottom - 1;
        const int32_t rectLeft = std::max<int32_t>(onScreen.GetLeft(), viewLeft);
        const int32_t rectTop = std::max<int32_t>(onScreen.GetTop(), viewTop);
        const int32_t rectRight = std::min<int32_t>(onScreen.GetRight(), viewRight);
        const int32_t rectBottom = std::min<int32_t>(onScreen.GetBottom(), viewBottom);
        if (rectLeft >= rectRight || rectTop >= rectBottom)
            return;
        const auto rect = ScreenRect{ { rectLeft, rectTop }, { rectRight, rectBottom } };
        OpenRCT2::Drawing::Rectangle::fillInset(
            rt, rect, ColourWithFlags{ OpenRCT2::Drawing::Colour::brightYellow },
            OpenRCT2::Drawing::Rectangle::BorderStyle::outset,
            OpenRCT2::Drawing::Rectangle::FillBrightness::light,
            OpenRCT2::Drawing::Rectangle::FillMode::none);
    }

    // ────────────────────────────────────────────────────────────────
    // OPENRCT2MINI focus-mode-plan §F.17 / window-set-plan.md:
    // WindowSet registry. Static, populated at link time. Two seeded
    // sets — extend by appending entries. Members are listed in the
    // order findSetFor() / iteration prefers; cross-window directional
    // navigation uses screen geometry so member order doesn't affect
    // results inside a single dir step, but defaultClass/defaultWidget
    // pick where focus lands on first entry into the set.

    namespace
    {
        constexpr std::array<WindowClass, 2> kInGameChromeMembers{
            WindowClass::topToolbar,
            WindowClass::bottomToolbar,
        };
        // Title scene opens five windows in TitleScene.cpp (~lines 302-306):
        //   titleMenu     — main button row (New Game, Load, Multiplayer, …)
        //   titleExit     — Quit icon (single imgBtn, WIDX_EXIT)
        //   titleOptions  — Options icon (single button, WIDX_OPTIONS)
        //   titleLogo     — clickable logo, opens About (single imgBtn, WIDX_LOGO)
        //   titleVersion  — decorative version footer, no focusable widgets
        // All four interactive windows belong in the set; without that
        // findNearestInSetDirection / cycle-window / bootstrap can't see
        // anything outside titleMenu's button row. titleVersion stays out
        // since it has no focusable widgets and would just bloat the
        // candidate pool on every directional step.
        constexpr std::array<WindowClass, 4> kTitleSceneMembers{
            WindowClass::titleMenu,
            WindowClass::titleOptions,
            WindowClass::titleExit,
            WindowClass::titleLogo,
        };

        // Static so the WindowSet::members spans stay valid for the
        // lifetime of the process. Each WindowSet wraps one of the
        // arrays above. defaultClass + defaultWidget seed the
        // bootstrap's "first widget on entry"; null/kWidgetIndexNull
        // means "first focusable of the first live member."
        const WindowSet kRegisteredSets[] = {
            WindowSet{
                kInGameChromeMembers.data(),
                kInGameChromeMembers.size(),
                WindowClass::topToolbar,
                kWidgetIndexNull,
                /*isBottomOfStack=*/ true,
            },
            WindowSet{
                kTitleSceneMembers.data(),
                kTitleSceneMembers.size(),
                WindowClass::titleMenu,
                kWidgetIndexNull,
                /*isBottomOfStack=*/ false,
            },
        };
    } // namespace

    const WindowSet* findSetFor(WindowClass cls)
    {
        if (cls == WindowClass::null)
            return nullptr;
        for (const auto& set : kRegisteredSets)
        {
            for (std::size_t i = 0; i < set.memberCount; i++)
            {
                if (set.members[i] == cls)
                    return &set;
            }
        }
        return nullptr;
    }

    bool sameSetOrClass(WindowClass a, WindowClass b)
    {
        if (a == b)
            return true;
        const auto* setA = findSetFor(a);
        if (setA == nullptr)
            return false;
        for (std::size_t i = 0; i < setA->memberCount; i++)
        {
            if (setA->members[i] == b)
                return true;
        }
        return false;
    }

    // OPENRCT2MINI window-set-plan §3.2 / Phase WS.2: cross-window
    // directional walker. Builds a screen-coord candidate list across
    // every live member of `fromClass`'s set, runs the same cost
    // metric as the single-window walker, and returns the winning
    // (class, widget). For a class not in any set, falls back to
    // findNearestInDirection on the single window.
    //
    // Output convention: outClass is set to the winner's class —
    // same as fromClass when the winner is in the same member, a
    // sibling member when the user stepped across the boundary.
    // Returns kWidgetIndexNull if no candidate exists anywhere in
    // the set (even with wrap-around).
    WidgetIndex findNearestInSetDirection(
        WindowClass fromClass, WidgetIndex fromWidget, Direction dir,
        WindowClass* outClass)
    {
        if (outClass != nullptr)
            *outClass = fromClass;

        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return kWidgetIndexNull;

        const auto* set = findSetFor(fromClass);
        if (set == nullptr)
        {
            // Not in a set — defer to single-window logic.
            auto* w = windowMgr->FindByClass(fromClass);
            if (w == nullptr)
                return kWidgetIndexNull;
            return findNearestInDirection(*w, fromWidget, dir);
        }

        // Build a screen-coord candidate list across every live
        // member of the set. Origin is the centre of the current
        // (fromClass, fromWidget); when fromWidget is invalid, use
        // the centre of the focused window — same neutral fallback
        // findNearestInDirection uses.
        struct Candidate
        {
            WindowClass cls;
            WidgetIndex idx;
            int32_t x;
            int32_t y;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(32);

        Candidate from{ fromClass, fromWidget, 0, 0 };
        bool fromValid = false;

        for (std::size_t si = 0; si < set->memberCount; si++)
        {
            const auto memberCls = set->members[si];
            auto* w = windowMgr->FindByClass(memberCls);
            if (w == nullptr)
                continue;
            if (w->flags.has(WindowFlag::dead))
                continue;
            const auto count = w->widgets.size();
            for (size_t i = 0; i < count; i++)
            {
                const auto& widget = w->widgets[i];
                if (!isFocusable(widget))
                    continue;
                if (isDropdownChevron(*w, static_cast<WidgetIndex>(i)))
                    continue;
                Candidate cand{
                    memberCls,
                    static_cast<WidgetIndex>(i),
                    static_cast<int32_t>(w->windowPos.x + widget.midX()),
                    static_cast<int32_t>(w->windowPos.y + widget.midY()),
                };
                if (memberCls == fromClass && static_cast<WidgetIndex>(i) == fromWidget)
                {
                    from = cand;
                    fromValid = true;
                    continue; // never a candidate against itself
                }
                candidates.push_back(cand);
            }
        }

        if (candidates.empty())
            return kWidgetIndexNull;
        if (!fromValid)
        {
            // No current widget reference; pick the spatial centre
            // of the first live member's window as the anchor, so
            // the cost metric still has something to compare to.
            from.x = 0;
            from.y = 0;
        }

        // Cost metric: |Δ along direction| + 2 × |Δ perpendicular|.
        // Same as the single-window walker. In-direction filter rejects
        // candidates on the wrong side of the from-point.
        const auto inDirection = [&](const Candidate& c) {
            switch (dir)
            {
                case Direction::up:    return c.y < from.y;
                case Direction::down:  return c.y > from.y;
                case Direction::left:  return c.x < from.x;
                case Direction::right: return c.x > from.x;
            }
            return false;
        };
        const auto cost = [&](const Candidate& c) -> int64_t {
            const int64_t dx = std::abs(static_cast<int64_t>(c.x) - from.x);
            const int64_t dy = std::abs(static_cast<int64_t>(c.y) - from.y);
            switch (dir)
            {
                case Direction::up:
                case Direction::down:    return dy + 2 * dx;
                case Direction::left:
                case Direction::right:   return dx + 2 * dy;
            }
            return std::numeric_limits<int64_t>::max();
        };
        const auto wrapCost = [&](const Candidate& c) -> int64_t {
            const int64_t dx = static_cast<int64_t>(c.x) - from.x;
            const int64_t dy = static_cast<int64_t>(c.y) - from.y;
            switch (dir)
            {
                case Direction::up:    return dy + 2 * std::abs(dx);
                case Direction::down:  return -dy + 2 * std::abs(dx);
                case Direction::left:  return dx + 2 * std::abs(dy);
                case Direction::right: return -dx + 2 * std::abs(dy);
            }
            return std::numeric_limits<int64_t>::max();
        };

        // Pass 1: candidates in the requested direction.
        const Candidate* best = nullptr;
        int64_t bestCost = std::numeric_limits<int64_t>::max();
        for (const auto& c : candidates)
        {
            if (fromValid && !inDirection(c))
                continue;
            const auto k = cost(c);
            if (k < bestCost)
            {
                bestCost = k;
                best = &c;
            }
        }
        if (best == nullptr && fromValid)
        {
            // Pass 2: wrap-around. Pick the candidate furthest in
            // the OPPOSITE direction (negative wrapCost wins).
            int64_t wrapBestCost = std::numeric_limits<int64_t>::max();
            for (const auto& c : candidates)
            {
                const auto k = wrapCost(c);
                if (k < wrapBestCost)
                {
                    wrapBestCost = k;
                    best = &c;
                }
            }
        }

        if (best == nullptr)
            return kWidgetIndexNull;
        if (outClass != nullptr)
            *outClass = best->cls;
        return best->idx;
    }

    // OPENRCT2MINI focus-mode-plan §F.cycle (set-aware): forward
    // tab-order walker. Steps through widgets in the current member's
    // declaration order, then advances through set members (in the
    // order registered in kRegisteredSets), wrapping back to the
    // first member. For non-set classes, falls back to single-window
    // nextFocusable.
    WidgetIndex nextFocusableInSet(
        WindowClass fromClass, WidgetIndex fromWidget, WindowClass* outClass)
    {
        if (outClass != nullptr)
            *outClass = fromClass;

        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return kWidgetIndexNull;

        const auto* set = findSetFor(fromClass);
        if (set == nullptr)
        {
            // Not part of a set — cycle within the single window with
            // wrap, exactly like the non-set TAB path.
            auto* w = windowMgr->FindByClass(fromClass);
            if (w == nullptr)
                return kWidgetIndexNull;
            return nextFocusable(*w, fromWidget);
        }

        // Locate fromClass within members[]. Cycle order matches the
        // declaration order in kRegisteredSets, which is hand-picked
        // (e.g. titleMenu → titleOptions → titleExit → titleLogo).
        std::size_t startMember = 0;
        bool startMemberFound = false;
        for (std::size_t i = 0; i < set->memberCount; i++)
        {
            if (set->members[i] == fromClass)
            {
                startMember = i;
                startMemberFound = true;
                break;
            }
        }
        if (!startMemberFound)
        {
            // findSetFor said fromClass is in this set, so this branch
            // shouldn't fire. Defensive fallback so a registry edit
            // can't crash this path.
            auto* w = windowMgr->FindByClass(fromClass);
            if (w == nullptr)
                return kWidgetIndexNull;
            return nextFocusable(*w, fromWidget);
        }

        // Local helper: first focusable widget in `cls`'s window
        // STRICTLY after `afterIdx`. Returns kWidgetIndexNull if
        // none; does NOT wrap. afterIdx == kWidgetIndexNull means
        // "start from 0" (i.e. firstFocusable).
        const auto firstAfter = [windowMgr](WindowClass cls, WidgetIndex afterIdx) -> WidgetIndex {
            auto* w = windowMgr->FindByClass(cls);
            if (w == nullptr || w->flags.has(WindowFlag::dead))
                return kWidgetIndexNull;
            const auto count = w->widgets.size();
            const std::size_t start = (afterIdx == kWidgetIndexNull)
                ? 0
                : static_cast<std::size_t>(afterIdx) + 1;
            for (std::size_t i = start; i < count; i++)
            {
                if (!isFocusable(w->widgets[i]))
                    continue;
                if (isDropdownChevron(*w, static_cast<WidgetIndex>(i)))
                    continue;
                return static_cast<WidgetIndex>(i);
            }
            return kWidgetIndexNull;
        };
        const auto firstInMember = [windowMgr](WindowClass cls) -> WidgetIndex {
            auto* w = windowMgr->FindByClass(cls);
            if (w == nullptr || w->flags.has(WindowFlag::dead))
                return kWidgetIndexNull;
            return firstFocusable(*w);
        };

        // Step 1: continue inside the current member past fromWidget.
        if (auto idx = firstAfter(fromClass, fromWidget); idx != kWidgetIndexNull)
        {
            if (outClass != nullptr)
                *outClass = fromClass;
            return idx;
        }

        // Step 2: walk forward through subsequent members, skipping
        // dead / unloaded ones and members with zero focusable
        // widgets. Continue past the end and wrap, but stop one short
        // of the starting member — that wraps back to its first
        // focusable in Step 3.
        for (std::size_t step = 1; step < set->memberCount; step++)
        {
            const auto memberCls = set->members[(startMember + step) % set->memberCount];
            if (auto idx = firstInMember(memberCls); idx != kWidgetIndexNull)
            {
                if (outClass != nullptr)
                    *outClass = memberCls;
                return idx;
            }
        }

        // Step 3: wrap inside the starting member. Returns the first
        // focusable up to and including fromWidget — covers the case
        // where fromWidget is the last focusable in its window and no
        // sibling member has any focusable widgets either.
        if (auto idx = firstInMember(fromClass); idx != kWidgetIndexNull)
        {
            if (outClass != nullptr)
                *outClass = fromClass;
            return idx;
        }

        return kWidgetIndexNull;
    }

} // namespace OpenRCT2::Ui::WidgetFocus

// OPENRCT2MINI focus-mode-plan / Phase F.5: cross-library bridge.
// Declared in src/openrct2/ui/WidgetFocusBridge.h. Implemented here
// so the UI side can query InputManager state without exposing it to
// libopenrct2. Mirrors the GetWindowManager pattern.
namespace OpenRCT2::Ui
{
    void drawFocusOutlineIfActive(OpenRCT2::Drawing::RenderTarget& rt, const OpenRCT2::WindowBase& window)
    {
        auto& mgr = GetInputManager();
        // Three-part gate:
        //   (1) Active context must be one of widgetFocus or osk —
        //       contexts that have a meaningful "focused widget"
        //       concept. Text-entry modals (loadSave, textInput, ...)
        //       route keys character-by-character; the ring would
        //       just visually noise. Phase F.6 adds osk to the
        //       allow-list so the OSK can publish its current key
        //       selection to InputManager focus state and inherit
        //       the ring drawer (replacing OSK's per-key outline
        //       draw in onDrawWidget).
        //   (2) Window class must match the focused class. Prevents
        //       drawing the ring on a window sitting under a modal
        //       that happens to share state with an old focus.
        //   (3) drawFocusOutline itself bounds-checks the widget
        //       index and skips non-focusable types — defensive.
        // OPENRCT2MINI active-window-emphasis plan §4.4 (2026-05-18):
        // cycle-window outline bypass. When the user just dispatched a
        // cycle-window shortcut AND is still holding its modifier, the
        // cycled-to window draws the focus ring regardless of the
        // active context / SelectorMode / focused-class gates below.
        // The user's gesture says "show me which window I just
        // cycled to," so we honour it even in cursor mode.
        const auto cycleClass = mgr.getCycleHighlightClass();
        if (cycleClass != WindowClass::null && window.classification == cycleClass)
        {
            // Use the focused-widget framing path — same body as the
            // normal-mode draw below. Falls through to the regular
            // gates if cycleClass IS set but this window isn't the
            // cycled one (an active dropdown could still draw its
            // own ring on a non-cycled window).
            const auto focusedWidget = mgr.getFocusedWidget();
            WidgetFocus::drawFocusOutline(rt, window, focusedWidget);
            return;
        }
        const auto ctx = mgr.getActiveContext();
        if (ctx != InputContext::widgetFocus && ctx != InputContext::osk)
            return;
        // OPENRCT2MINI cursor-selector-modal-plan v2: ring only
        // draws when the state machine considers the selector
        // visible. Bootstrap may still keep _focusedWindowClass
        // populated in hidden mode so a later wake resumes
        // smoothly, but the ring itself stays off.
        if (mgr.getSelectorMode() != InputManager::SelectorMode::active)
            return;
        if (window.classification != mgr.getFocusedWindowClass())
            return;
        // OPENRCT2MINI focus-mode-plan §F.10: dropdowns suppress the
        // yellow ring entirely. The dropdown is a single 1×1 widget
        // (placeholder background); drawing the outline around it
        // would paint a thin yellow rectangle over the whole list
        // box, which is visually noisy and useless. The dropdown's
        // own per-item highlight (item-background darken, see
        // Dropdown.cpp onDraw) is the navigation cue.
        if (window.classification == WindowClass::dropdown)
            return;
        // OPENRCT2MINI list-focus-plan §2.4: list-item ring. When the
        // focused widget is a list-mode scroll widget AND a specific
        // item is selected, draw the ring around that item's on-screen
        // rect rather than around the whole scroll widget. The widget-
        // level ring would just paint a yellow rectangle around the
        // scroll viewport — useless as a "which item am I on" cue.
        const auto focusedWidget = mgr.getFocusedWidget();
        const auto focusedItem = mgr.getFocusedScrollItem();
        if (focusedItem >= 0
            && WidgetFocus::isListModeScroll(window, focusedWidget))
        {
            WidgetFocus::drawListItemFocusOutline(rt, window, focusedWidget, focusedItem);
            return;
        }
        WidgetFocus::drawFocusOutline(rt, window, focusedWidget);
    }
} // namespace OpenRCT2::Ui
