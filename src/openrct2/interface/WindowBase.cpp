#include "WindowBase.h"

#include "../config/Config.h"
#include "../drawing/Drawing.h"
#include "../entity/EntityList.h"
#include "../entity/EntityRegistry.h"
#include "Cursors.h"
#include "Viewport.h"

namespace OpenRCT2
{
    void WindowBase::setViewportLocation(const CoordsXYZ& coords)
    {
        WindowScrollToLocation(*this, coords);
        flags.unset(WindowFlag::scrollingToLocation);

        // Immediately update the viewport position since we are not scrolling.
        if (viewport != nullptr)
        {
            viewport->viewPos = savedViewPos;
        }

        invalidate();
    }

    void WindowBase::invalidate()
    {
        GfxSetDirtyBlocks({ windowPos, windowPos + ScreenCoordsXY{ width, height } });
    }

    void WindowBase::removeViewport()
    {
        if (viewport == nullptr)
            return;

        ViewportRemove(viewport);
        viewport = nullptr;
    }

    void WindowBase::setWidgets(const std::span<const Widget> newWidgets)
    {
        widgets.clear();
        widgets.insert(widgets.end(), newWidgets.begin(), newWidgets.end());

        resizeFrame();
    }

    CursorID WindowBase::onCursor(WidgetIndex, const ScreenCoordsXY&, CursorID)
    {
        return CursorID::Arrow;
    }

    // OPENRCT2MINI W5: position one of the title-bar buttons (close or shade)
    // at either edge of the window. The user-facing rule: if the close box is
    // configured for the right side (default), the shade box goes on the
    // left, and vice versa — the two buttons bracket the title bar.
    static inline void positionTitleButton(
        Widget& button, int32_t windowWidth, bool atLeftEdge, bool translucent, bool isShaded)
    {
        auto buttonSize = Config::Get().interface.enlargedUi ? kCloseButtonSizeTouch : kCloseButtonSize;
        if (atLeftEdge)
        {
            button.left = 2;
            button.right = 2 + buttonSize;
        }
        else
        {
            button.left = windowWidth - 3 - buttonSize;
            button.right = windowWidth - 3;
        }

        if (button.type == WidgetType::closeBox)
        {
            if (buttonSize == kCloseButtonSizeTouch)
                button.string = !translucent ? kCloseBoxStringBlackLarge : kCloseBoxStringWhiteLarge;
            else
                button.string = !translucent ? kCloseBoxStringBlackNormal : kCloseBoxStringWhiteNormal;
        }
        else if (button.type == WidgetType::shadeBox)
        {
            // Glyph shows the direction the body would move on click:
            //   ▾ when expanded ("collapse downward")
            //   ▴ when shaded ("expand back up")
            if (buttonSize == kCloseButtonSizeTouch)
            {
                if (isShaded)
                    button.string = !translucent ? kShadeBoxStringCollapseBlackLarge : kShadeBoxStringCollapseWhiteLarge;
                else
                    button.string = !translucent ? kShadeBoxStringExpandBlackLarge : kShadeBoxStringExpandWhiteLarge;
            }
            else
            {
                if (isShaded)
                    button.string = !translucent ? kShadeBoxStringCollapseBlackNormal
                                                 : kShadeBoxStringCollapseWhiteNormal;
                else
                    button.string = !translucent ? kShadeBoxStringExpandBlackNormal : kShadeBoxStringExpandWhiteNormal;
            }
        }
    }

    void WindowBase::resizeFrame()
    {
        if (widgets.size() < 3)
            return;

        // OPENRCT2MINI W5: NEVER hold references into widgets across a
        // push_back (the shadeBox append below). std::vector reallocation
        // invalidates all references and iterators. Always look up by
        // index right when needed.

        // Frame (widgets[0])
        if (widgets[0].type == WidgetType::frame)
        {
            widgets[0].right = width - 1;
            widgets[0].bottom = height - 1;
        }

        // Title/caption (widgets[1])
        const bool hasTitleWidget = widgets[1].type == WidgetType::caption;
        if (hasTitleWidget)
            widgets[1].right = width - 2;

        // Close button (widgets[2])
        const bool hasCloseBox = widgets[2].type == WidgetType::closeBox;
        if (hasCloseBox || widgets[2].type == WidgetType::empty)
        {
            bool translucent = colours[widgets[2].colour].flags.has(ColourFlag::translucent);
            const bool closeOnLeft = Config::Get().interface.windowButtonsOnTheLeft;
            positionTitleButton(widgets[2], width, closeOnLeft, translucent, isShaded);
        }

        // OPENRCT2MINI W5: append shade button if eligible (window has the
        // standard caption + closeBox prefix). Shade goes on the side
        // OPPOSITE the close button so the two bracket the title bar.
        // setWidgets clears the array each call so we re-append every time;
        // toggleShade calls resizeFrame without going through setWidgets,
        // hence the duplicate-detection scan.
        if (hasTitleWidget && hasCloseBox && !flags.has(WindowFlag::noTitleBar))
        {
            // Skip if a shadeBox is already present (e.g. resizeFrame called
            // twice in quick succession without a setWidgets between).
            bool hasShadeBox = false;
            for (const auto& w : widgets)
            {
                if (w.type == WidgetType::shadeBox)
                {
                    hasShadeBox = true;
                    break;
                }
            }
            if (!hasShadeBox)
            {
                Widget shadeWidget{};
                shadeWidget.type = WidgetType::shadeBox;
                shadeWidget.top = widgets[2].top;
                shadeWidget.bottom = widgets[2].bottom;
                shadeWidget.colour = widgets[2].colour;
                // STR_SHADE_WINDOW_TIP is defined in UiStringIds.h
                // (openrct2-ui layer), not visible from libopenrct2. Use the
                // numeric literal directly here. The string is added in W8.
                shadeWidget.tooltip = static_cast<StringId>(7041);
                widgets.push_back(shadeWidget);
                // ALL existing references / iterators into widgets are now
                // potentially invalid — vector reallocation. Continue using
                // index access only.
            }
            // Position both buttons. Shade is on the opposite side from
            // close. Index 2 = closeBox; we find the shadeBox by type.
            bool translucent = colours[widgets[2].colour].flags.has(ColourFlag::translucent);
            const bool closeOnLeft = Config::Get().interface.windowButtonsOnTheLeft;
            positionTitleButton(widgets[2], width, closeOnLeft, translucent, isShaded);
            for (size_t i = 0; i < widgets.size(); i++)
            {
                if (widgets[i].type == WidgetType::shadeBox)
                {
                    positionTitleButton(widgets[i], width, !closeOnLeft, translucent, isShaded);
                    break;
                }
            }
        }

        // Page/resize widget (widgets[3] if present)
        if (widgets.size() >= 4 && widgets[3].type == WidgetType::resize)
        {
            widgets[3].right = width - 1;
            widgets[3].bottom = height - 1;
        }

        // Figure out if we need to push the other widgets down to accommodate a resized title/caption
        auto preferredHeight = getTitleBarTargetHeight();
        auto currentHeight = widgets[1].height() - 1;
        auto heightDifference = preferredHeight - currentHeight;

        if (hasTitleWidget && heightDifference != 0)
        {
            invalidate();

            // Offset title and the title-bar buttons (close + shade)
            widgets[1].bottom += heightDifference;
            for (size_t i = 0; i < widgets.size(); i++)
            {
                if (widgets[i].type == WidgetType::closeBox || widgets[i].type == WidgetType::shadeBox)
                    widgets[i].bottom += heightDifference;
            }

            height += heightDifference;
            minHeight += heightDifference;
            maxHeight += heightDifference;

            invalidate();

            // Resize frame again to match new height
            if (widgets[0].type == WidgetType::frame)
                widgets[0].bottom = height - 1;

            // Offset body widgets — skip the title bar chrome already
            // handled above (frame / caption / closeBox / shadeBox).
            for (size_t i = 0; i < widgets.size(); i++)
            {
                auto t = widgets[i].type;
                if (t == WidgetType::frame || t == WidgetType::caption
                    || t == WidgetType::closeBox || t == WidgetType::shadeBox)
                {
                    continue;
                }
                widgets[i].top += heightDifference;
                widgets[i].bottom += heightDifference;
            }

            // Offset viewport
            if (viewport != nullptr)
                viewport->pos.y += heightDifference;
        }

        // OPENRCT2MINI W5: shade collapse / expand. Runs after the existing
        // height-adjust logic so the title bar height has already been set
        // to its target value. When shaded: hide every body widget via
        // shadeHidden and collapse height to title-bar height. When not
        // shaded: clear any leftover shadeHidden flags from a previous
        // shade (the per-window setWidgets path would naturally clear them
        // by rebuilding the array, but toggleShade calls resizeFrame
        // without going through setWidgets).
        if (hasTitleWidget && hasCloseBox)
        {
            if (isShaded)
            {
                // Exempt chrome by INDEX (0/1/2 = frame/caption/closeBox)
                // plus any shadeBox by type (the appended one at the tail).
                // Exempting "all frames" by type is wrong: some windows use
                // a SECOND WidgetType::frame at index 3+ as a body backdrop
                // (e.g. About's WIDX_PAGE_BACKGROUND), which we DO want to
                // hide when shaded.
                for (size_t i = 0; i < widgets.size(); i++)
                {
                    if (i < 3 || widgets[i].type == WidgetType::shadeBox)
                        continue;
                    widgets[i].flags.set(WidgetFlag::shadeHidden);
                }
                // OPENRCT2MINI: collapse the window to caption.bottom + 2
                // rows. The +2 (rather than +1) leaves a dedicated bottom
                // row for the frame's outset shadow border below the
                // title's inset hilight bottom edge — without it the
                // title's light bottom row IS the window's bottom row,
                // which clobbers the dark window outline for the middle
                // ~W-3 pixels and gives the bottom-left corner an
                // asymmetric "missing 2 pixels of outline" look.
                int16_t collapsedHeight = static_cast<int16_t>(widgets[1].bottom + 2);
                height = collapsedHeight;
                minHeight = collapsedHeight;
                maxHeight = collapsedHeight;
                if (widgets[0].type == WidgetType::frame)
                    widgets[0].bottom = height - 1;
            }
            else
            {
                for (size_t i = 0; i < widgets.size(); i++)
                    widgets[i].flags.unset(WidgetFlag::shadeHidden);
            }
        }
    }

    void WindowBase::toggleShade()
    {
        if (!isShaded)
        {
            // About to shade — save the current dimensions so we can
            // restore them on un-shade.
            shadeRestoreHeight = height;
            shadeRestoreMinHeight = minHeight;
            shadeRestoreMaxHeight = maxHeight;
            isShaded = true;
            // OPENRCT2MINI W5: stop the window's PIP viewport (Park
            // entrance, Ride PIP, etc.) from rendering OR marking
            // screen rects dirty. Without this, animated map tiles in
            // the PIP's view area (which still tracks at the original
            // on-screen position) would dirty rects below the shaded
            // title bar; the main viewport behind would partially
            // repaint those rects with its own world view, producing
            // jitter/smear in the area where the body used to be.
            if (viewport != nullptr)
                viewport->flags |= VIEWPORT_FLAG_RENDERING_INHIBITED;
        }
        else
        {
            // About to un-shade — restore the saved dimensions BEFORE
            // flipping the bit so resizeFrame's height-adjust path doesn't
            // see the still-collapsed values.
            height = shadeRestoreHeight;
            minHeight = shadeRestoreMinHeight;
            maxHeight = shadeRestoreMaxHeight;
            isShaded = false;
            if (viewport != nullptr)
                viewport->flags &= ~VIEWPORT_FLAG_RENDERING_INHIBITED;
        }
        invalidate();
        resizeFrame();
        invalidate();
    }

    int16_t WindowBase::getTitleBarTargetHeight() const
    {
        return Config::Get().interface.enlargedUi ? kTitleHeightLarge : kTitleHeightNormal;
    }

    int16_t WindowBase::getTitleBarCurrentHeight() const
    {
        if (!flags.has(WindowFlag::noTitleBar) && widgets.size() > 2)
            return widgets[1].height() - 1;
        else
            return 0;
    }

    int16_t WindowBase::getTitleBarDiffTarget() const
    {
        return getTitleBarTargetHeight() - getTitleBarCurrentHeight();
    }

    int16_t WindowBase::getTitleBarDiffNormal() const
    {
        return getTitleBarCurrentHeight() - kTitleHeightNormal;
    }
} // namespace OpenRCT2
