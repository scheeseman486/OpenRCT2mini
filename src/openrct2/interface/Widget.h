/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../core/FlagHolder.hpp"
#include "../core/StringTypes.h"
#include "../drawing/ImageId.hpp"
#include "../localisation/StringIdType.h"
#include "../world/Location.hpp"

#include <cstdint>

namespace OpenRCT2
{
    using WidgetIndex = uint16_t;
    constexpr WidgetIndex kWidgetIndexNull = 0xFFFF;

    enum class WidgetType : uint8_t
    {
        empty = 0,
        frame = 1,
        resize = 2,
        imgBtn = 3,
        colourBtn = 6,
        trnBtn = 7,
        tab = 8,
        flatBtn = 9,
        /**
         * For stuff that mostly acts as a label, while providing a way for power users to quickly
         * open the associated window. For example: the money display in the lower left of the screen.
         */
        hiddenButton = 11,
        button = 10,
        labelCentred = 12, // Centred text
        tableHeader = 13,  // Left-aligned textual button
        label = 14,        // Left-aligned text
        spinner = 15,
        dropdownMenu = 16,
        viewport = 17,
        groupbox = 19,
        caption = 20,
        closeBox = 21,
        scroll = 22,
        checkbox = 23,
        placeholder = 25,
        progressBar = 29,
        custom = 28,
        textBox = 27,
        horizontalSeparator = 30,
        // OPENRCT2MINI W5: window-shade button. Drawn like closeBox but
        // toggles WindowBase::isShaded instead of closing. Positioned by
        // resizeFrame() on the side opposite the close box.
        shadeBox = 31,
    };

    enum class WidgetFlag : uint8_t
    {
        textIsString = 0,
        isPressed = 2,
        isDisabled = 3,
        tooltipIsString = 4,
        isHidden = 5,
        isHoldable = 6,
        // OPENRCT2MINI W4: window-shade body hide. Set on every body widget
        // (anything except frame/caption/closeBox/shadeBox) when the window
        // is shaded. isVisible() checks both isHidden and shadeHidden so the
        // shade collapse is independent of per-window setWidgetHidden calls.
        shadeHidden = 7,
    };
    using WidgetFlags = FlagHolder<uint8_t, WidgetFlag>;

    enum
    {
        SCROLL_HORIZONTAL = (1 << 0),
        SCROLL_VERTICAL = (1 << 1),
        SCROLL_BOTH = SCROLL_HORIZONTAL | SCROLL_VERTICAL
    };

    constexpr const char* kCloseBoxStringBlackNormal = u8"{BLACK}❌";
    constexpr const char* kCloseBoxStringBlackLarge = u8"{BLACK}X";
    constexpr const char* kCloseBoxStringWhiteNormal = u8"{WHITE}❌";
    constexpr const char* kCloseBoxStringWhiteLarge = u8"{WHITE}X";

    // OPENRCT2MINI W5: window-shade button glyphs. Triangles point in the
    // direction the body would move on click — ▾ when expanded ("click to
    // collapse downward into title bar"), ▴ when shaded ("click to expand
    // back out"). All four glyphs are in the OpenRCT2 sprite font (see
    // UnicodeChar::small_down/small_up/down/up in core/UnicodeChar.h and
    // their Font.cpp registrations); no g2.dat work needed.
    constexpr const char* kShadeBoxStringExpandBlackNormal = u8"{BLACK}▾";
    constexpr const char* kShadeBoxStringExpandWhiteNormal = u8"{WHITE}▾";
    constexpr const char* kShadeBoxStringExpandBlackLarge = u8"{BLACK}▼";
    constexpr const char* kShadeBoxStringExpandWhiteLarge = u8"{WHITE}▼";
    constexpr const char* kShadeBoxStringCollapseBlackNormal = u8"{BLACK}▴";
    constexpr const char* kShadeBoxStringCollapseWhiteNormal = u8"{WHITE}▴";
    constexpr const char* kShadeBoxStringCollapseBlackLarge = u8"{BLACK}▲";
    constexpr const char* kShadeBoxStringCollapseWhiteLarge = u8"{WHITE}▲";

    struct Widget
    {
        WidgetType type{};
        uint8_t colour{};
        int16_t left{};
        int16_t right{};
        int16_t top{};
        int16_t bottom{};
        // OPENRCT2MINI: cut 35 — restored the union layout the renderer expects.
        // Cut 33 had broken this by converting to plain fields, which silently
        // dropped the bit-pattern equivalence between content/image/text/string
        // — the Options-menu renderer reads widget.text but the makeWidget
        // overload had written to widget.content, so text was 0 and labels
        // didn't draw. Union restored; the C++17 active-member-change problem
        // is solved instead by dropping constexpr from makeWidget (consumer
        // arrays move from `static constexpr auto` to `static const auto`).
        // `content` is the default-init member; bit-pattern equivalence makes
        // every union member readable regardless of which one was written.
        union
        {
            uint32_t content{};
            ImageId image;
            StringId text;
            const utf8* string;
        };
        StringId tooltip{ kStringIdNone };

        // New properties
        WidgetFlags flags{};
        const utf8* sztooltip{};

        int16_t width() const
        {
            return right - left + 1;
        }

        void setWidth(int16_t newWidth)
        {
            right = left + newWidth - 1;
        }

        int16_t height() const
        {
            return bottom - top + 1;
        }

        void setHeight(int16_t newHeight)
        {
            bottom = top + newHeight - 1;
        }

        int16_t midX() const
        {
            return left + (width() / 2);
        }

        int16_t midY() const
        {
            return top + (height() / 2);
        }

        int16_t textTop() const
        {
            if (height() >= 11)
                return std::max<int32_t>(top, top + (height() / 2) - 6);

            return top - 1;
        }

        void moveRight(int32_t amount)
        {
            left += amount;
            right += amount;
        }

        void moveDown(int32_t amount)
        {
            top += amount;
            bottom += amount;
        }

        void moveTo(ScreenCoordsXY coords)
        {
            moveRight(coords.x - left);
            moveDown(coords.y - top);
        }

        void moveToX(int16_t x)
        {
            moveRight(x - left);
        }

        void moveToY(int16_t y)
        {
            moveDown(y - top);
        }

        bool isVisible() const
        {
            // OPENRCT2MINI W4: shadeHidden is set on body widgets when the
            // owning window is shaded. Both flags must be clear for the
            // widget to render and be hit-testable.
            return !flags.has(WidgetFlag::isHidden) && !flags.has(WidgetFlag::shadeHidden);
        }

        void setString(StringId newStringId)
        {
            text = newStringId;
            flags.unset(WidgetFlag::textIsString);
        }

        void setString(const utf8* newString)
        {
            string = newString;
            flags.set(WidgetFlag::textIsString);
        }

        void setTooltip(StringId newStringId)
        {
            tooltip = newStringId;
            flags.unset(WidgetFlag::tooltipIsString);
        }

        void setTooltip(const utf8* newString)
        {
            sztooltip = newString;
            flags.set(WidgetFlag::tooltipIsString);
        }
    };

    constexpr uint8_t kTitleHeightNormal = 13;
    constexpr uint8_t kTitleHeightLarge = 24;

    constexpr uint8_t kCloseButtonSize = 10;
    constexpr uint8_t kCloseButtonSizeTouch = 20;

    constexpr int32_t kScrollableRowHeight = 12;
    constexpr uint8_t kListRowHeight = 12;
    constexpr uint8_t kTableCellHeight = 12;
    constexpr uint8_t kButtonFaceHeight = 12;
    constexpr uint8_t kSpinnerHeight = 12;
    constexpr uint8_t kDropdownHeight = 12;

    constexpr uint16_t kTextInputSize = 1024;
    constexpr uint16_t kTopToolbarHeight = 27;
} // namespace OpenRCT2
