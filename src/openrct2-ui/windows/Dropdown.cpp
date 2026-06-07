/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <algorithm>
#include <bitset>
#include <iterator>
#include <openrct2-ui/input/WidgetFocus.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/Input.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/BitSet.hpp>
#include <openrct2/core/String.hpp>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.String.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/interface/ColourWithFlags.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/ui/WindowManager.h>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    constexpr int32_t kDropdownItemHeight = 12;
    constexpr int32_t kDropdownItemHeightTouch = 24;
    // Padding to the left of an item, where a marker can be drawn.
    static constexpr int32_t kDropdownItemLeftPadding = 10;

    static constexpr std::array<uint8_t, 57> kAppropriateImageDropdownItemsPerRow = {
        1, 1, 1, 1, 2, 2, 3, 3, 4, 3, // 10
        5, 4, 4, 5, 5, 5, 4, 5, 6, 5, // 20
        5, 7, 4, 5, 6, 5, 6, 6, 6, 6, // 30
        6, 8, 8, 8, 9, 9, 9, 9, 9, 9, // 40
        9, 9, 9, 9, 9, 9, 9, 9, 9, 9, // 50
        9, 9, 9, 9, 9, 9, 9,          // 56
    };

    enum WindowDropdownWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
    };

    static const Widget kWindowDropdownWidgets[] = {
        makeWidget({ 0, 0 }, { 1, 1 }, WidgetType::imgBtn, WindowColour::primary),
    };

    Dropdown::DropdownState gDropdown{};

    class DropdownWindow final : public Window
    {
        int32_t NumColumns;
        int32_t NumRows;
        int32_t ItemWidth;
        int32_t ItemHeight;
        int32_t ItemPadding;
        bool ListVertically;

    public:
        void onOpen() override
        {
            setWidgets(kWindowDropdownWidgets);

            // Input state
            gDropdown.highlightedIndex = -1;
            gDropdown.hasTooltips = false;
            gDropdown.defaultIndex = -1;
            // OPENRCT2MINI focus-mode-plan §F.14: every new
            // dropdown starts in cursor-driven mode. The mouse-
            // hover indicator wins by default; the first focus.up/
            // down press flips this to focus until the next real
            // SDL_MOUSEMOTION arrives.
            gDropdown.navigationSource = Dropdown::NavigationSource::cursor;
            InputSetState(InputState::dropdownActive);
        }

        static int32_t GetDefaultRowHeight()
        {
            return Config::Get().interface.enlargedUi ? kDropdownItemHeightTouch : kDropdownItemHeight;
        }

        static int32_t GetAdditionalRowPadding()
        {
            return Config::Get().interface.enlargedUi ? 6 : 0;
        }

        void drawItem(RenderTarget& rt, ScreenCoordsXY screenCoords, int32_t i)
        {
            const int32_t highlightedIndex = gDropdown.highlightedIndex;
            const bool highlighted = (i == highlightedIndex);

            const auto& item = gDropdown.items[i];
            switch (item.type)
            {
                case Dropdown::ItemType::regular:
                {
                    auto formatString = STR_OPTIONS_DROPDOWN_ITEM;
                    if (i < Dropdown::kItemsMaxSize && gDropdown.items[i].isChecked())
                        formatString = STR_OPTIONS_DROPDOWN_ITEM_SELECTED;

                    drawTextItem(rt, screenCoords, width, item, highlighted, formatString, colours[0].colour);
                    break;
                }
                case Dropdown::ItemType::toggle:
                {
                    auto formatString = STR_TOGGLE_OPTION;
                    if (i < Dropdown::kItemsMaxSize && gDropdown.items[i].isChecked())
                        formatString = STR_TOGGLE_OPTION_CHECKED;

                    drawTextItem(rt, screenCoords, width, item, highlighted, formatString, colours[0].colour);
                    break;
                }
                case Dropdown::ItemType::plain:
                {
                    drawTextItem(rt, screenCoords, width, item, highlighted, STR_STRING, colours[0].colour);
                    break;
                }
                case Dropdown::ItemType::image:
                {
                    GfxDrawSprite(rt, item.image, screenCoords);
                    break;
                }
                case Dropdown::ItemType::colour:
                {
                    auto image = item.image;
                    if (highlightedIndex == i)
                        image = image.WithIndexOffset(1);
                    GfxDrawSprite(rt, image, screenCoords);
                    break;
                }
                case Dropdown::ItemType::separator:
                    break;
            }
        }

        void drawSeparator(RenderTarget& rt, ScreenCoordsXY screenCoords)
        {
            const auto leftTop = screenCoords + ScreenCoordsXY{ 2, (ItemHeight / 2) - 1 };
            const auto rightBottom = leftTop + ScreenCoordsXY{ ItemWidth - 4, 0 };
            const auto shadowOffset = ScreenCoordsXY{ 0, 1 };

            if (colours[0].flags.has(ColourFlag::translucent))
            {
                auto palette = kTranslucentWindowPalettes[EnumValue(colours[0].colour)];
                Rectangle::filter(rt, { leftTop, rightBottom }, palette.highlight);
                Rectangle::filter(rt, { leftTop + shadowOffset, rightBottom + shadowOffset }, palette.shadow);
            }
            else
            {
                Rectangle::fill(rt, { leftTop, rightBottom }, getColourMap(colours[0].colour).midDark);
                Rectangle::fill(
                    rt, { leftTop + shadowOffset, rightBottom + shadowOffset }, getColourMap(colours[0].colour).lightest);
            }
        }

        void drawTextItem(
            RenderTarget& rt, ScreenCoordsXY screenCoords, int32_t ddWidth, const Dropdown::Item& item, bool highlighted,
            StringId format, Colour background)
        {
            ColourWithFlags colour = { background };
            if (highlighted)
                colour.colour = Colour::white;
            if (item.isDisabled())
                colour = { background, { ColourFlag::inset } };

            auto yOffset = ItemPadding;
            Formatter ft;
            ft.Add<const utf8*>(item.text);

            drawTextEllipsised(rt, { screenCoords.x + 2, screenCoords.y + yOffset }, ddWidth - 7, format, ft, { colour });
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);

            int32_t highlightedIndex = gDropdown.highlightedIndex;

            for (int32_t i = 0; i < gDropdown.numItems; i++)
            {
                ScreenCoordsXY cellCoords;
                if (ListVertically)
                    cellCoords = { i / NumRows, i % NumRows };
                else
                    cellCoords = { i % NumColumns, i / NumColumns };

                ScreenCoordsXY screenCoords = windowPos
                    + ScreenCoordsXY{ 2 + (cellCoords.x * ItemWidth), 2 + (cellCoords.y * ItemHeight) };

                bool highlighted = (i == highlightedIndex);
                if (highlighted)
                {
                    // Darken the cell's background slightly when highlighted
                    const ScreenCoordsXY rightBottom = screenCoords + ScreenCoordsXY{ ItemWidth - 1, ItemHeight - 1 };
                    Rectangle::filter(rt, { screenCoords, rightBottom }, FilterPaletteID::paletteDarken3);
                }

                if (gDropdown.items[i].isSeparator())
                {
                    drawSeparator(rt, screenCoords);
                }
                else if (gDropdown.cellDrawFunction.has_value())
                {
                    RenderTarget clippedRT;
                    if (ClipRenderTarget(clippedRT, rt, screenCoords, ItemWidth, ItemHeight))
                    {
                        gDropdown.cellDrawFunction.value()(clippedRT, gDropdown.items[i], highlighted);
                    }
                }
                else
                {
                    drawItem(rt, screenCoords, i);
                }
            }
        }

        static int32_t getSpaceUntilBottom(const ScreenCoordsXY& screenPos, int32_t dropdownButtonHeight)
        {
            auto* mainWindow = WindowGetMain();
            if (mainWindow == nullptr)
                return 400;

            return std::max(1, mainWindow->height - (screenPos.y + dropdownButtonHeight + 5));
        }

        void setTextItems(
            const ScreenCoordsXY& screenPos, int32_t extraY, ColourWithFlags colour, uint8_t customItemHeight, uint8_t txtFlags,
            size_t numItems, int32_t itemWidth, int32_t numRowsPerColumn)
        {
            // Set and calculate num items, rows and columns
            ItemHeight = (txtFlags & Dropdown::Flag::CustomHeight) ? customItemHeight : GetDefaultRowHeight();
            ItemPadding = (txtFlags & Dropdown::Flag::CustomHeight) ? 0 : GetAdditionalRowPadding();

            gDropdown.numItems = static_cast<int32_t>(numItems);
            if (gDropdown.numItems > 1)
            {
                int32_t numAvailableRows = std::max(1, getSpaceUntilBottom(screenPos, extraY) / ItemHeight);
                NumRows = std::min({ gDropdown.numItems, numAvailableRows, numRowsPerColumn });
                NumColumns = (gDropdown.numItems + NumRows - 1) / NumRows;
            }
            else
            {
                // There must always be at least one column to prevent dividing by zero
                NumRows = 1;
                NumColumns = 1;
            }

            ItemWidth = itemWidth;

            // Text dropdowns are listed horizontally
            ListVertically = true;

            UpdateSizeAndPosition(screenPos, extraY);

            if (colour.flags.has(ColourFlag::translucent))
                flags |= WindowFlag::transparent;
            colours[0] = colour;

            // OPENRCT2MINI focus-mode-widgets-plan §3.1 / Cohort A.2+A.3
            // (2026-05-25): mirror the just-computed grid layout into
            // gDropdown so focus-mode helpers (per-item ring draw, 2D
            // walker) can read it without downcasting into this
            // window's private members.
            gDropdown.numColumns = NumColumns;
            gDropdown.numRows = NumRows;
            gDropdown.itemWidth = ItemWidth;
            gDropdown.itemHeight = ItemHeight;
            gDropdown.listVertically = ListVertically;
        }

        void setImageItems(
            const ScreenCoordsXY& screenPos, int32_t extraY, ColourWithFlags colour, int32_t numItems, int32_t itemWidth,
            int32_t itemHeight, int32_t numColumns)
        {
            // Set and calculate num items, rows and columns
            ItemWidth = itemWidth;
            ItemHeight = itemHeight;
            gDropdown.numItems = numItems;

            // There must always be at least one column and row to prevent dividing by zero
            if (gDropdown.numItems == 0)
            {
                NumColumns = 1;
                NumRows = 1;
            }
            else
            {
                NumColumns = std::max(1, numColumns);
                NumRows = gDropdown.numItems / NumColumns;
                if (gDropdown.numItems % NumColumns != 0)
                    NumRows++;
            }

            // image dropdowns are listed horizontally
            ListVertically = false;

            UpdateSizeAndPosition(screenPos, extraY);

            if (colour.flags.has(ColourFlag::translucent))
                flags |= WindowFlag::transparent;
            colours[0] = colour;

            // OPENRCT2MINI focus-mode-widgets-plan §3.1 / Cohort A.2+A.3
            // (2026-05-25): mirror grid layout into gDropdown (see
            // setTextItems for rationale).
            gDropdown.numColumns = NumColumns;
            gDropdown.numRows = NumRows;
            gDropdown.itemWidth = ItemWidth;
            gDropdown.itemHeight = ItemHeight;
            gDropdown.listVertically = ListVertically;
        }

        int32_t GetIndexFromPoint(const ScreenCoordsXY& loc)
        {
            int32_t top = loc.y - windowPos.y - 2;
            if (top < 0)
                return -1;

            int32_t left = loc.x - windowPos.x;
            if (left >= width)
                return -1;
            left -= 2;
            if (left < 0)
                return -1;

            int32_t columnNum = left / ItemWidth;
            if (columnNum >= NumColumns)
                return -1;

            int32_t rowNum = top / ItemHeight;
            if (rowNum >= NumRows)
                return -1;

            int32_t dropdownIndex;
            if (ListVertically)
                dropdownIndex = columnNum * NumRows + rowNum;
            else
                dropdownIndex = rowNum * NumColumns + columnNum;

            if (dropdownIndex >= gDropdown.numItems)
                return -1;

            return dropdownIndex;
        }

    private:
        void UpdateSizeAndPosition(const ScreenCoordsXY& screenPos, const int32_t extraY)
        {
            // Calculate position and size
            const auto ddWidth = ItemWidth * NumColumns + 3;
            const auto ddHeight = ItemHeight * NumRows + 3;

            int32_t screenWidth = ContextGetWidth();
            int32_t screenHeight = ContextGetHeight();
            auto boundedScreenPos = screenPos;
            if (screenPos.x + ddWidth > screenWidth)
                boundedScreenPos.x = std::max(0, screenWidth - ddWidth);
            if (screenPos.y + ddHeight > screenHeight)
                boundedScreenPos.y = std::max(0, screenHeight - ddHeight);
            widgets[WIDX_BACKGROUND].right = ddWidth;
            widgets[WIDX_BACKGROUND].bottom = ddHeight;

            invalidate();
            width = ddWidth + 1;
            height = ddHeight + 1;
            windowPos = boundedScreenPos + ScreenCoordsXY{ 0, extraY };
            invalidate();
        }
    };

    static void copyItemsToGlobal(std::span<const Dropdown::Item> items)
    {
        for (size_t i = 0; i < items.size(); i++)
        {
            gDropdown.items[i] = items[i];
        }
    }

    /**
     * Shows a text dropdown menu.
     *  rct2: 0x006ECFB9
     *
     * @param x (cx)
     * @param y (dx)
     * @param extray (di)
     * @param flags (bh)
     * @param num_items (bx)
     * @param colour (al)
     */
    void WindowDropdownShowText(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags, size_t num_items,
        size_t prefRowsPerColumn)
    {
        // Calculate the longest string width
        int32_t maxStringWidth = 0;
        for (size_t i = 0; i < num_items; i++)
        {
            int32_t stringWidth = getStringWidth(gDropdown.items[i].text, FontStyle::medium);
            if (gDropdown.items[i].type != Dropdown::ItemType::plain)
                stringWidth += kDropdownItemLeftPadding;
            maxStringWidth = std::max(stringWidth, maxStringWidth);
        }

        WindowDropdownShowTextCustomWidth(
            screenPos, extray, colour, 0, flags, num_items, maxStringWidth + 3, prefRowsPerColumn);

        gDropdown.cellDrawFunction = std::nullopt;
    }

    void WindowDropdownShowText(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags,
        std::span<const Dropdown::Item> items, size_t prefRowsPerColumn)
    {
        copyItemsToGlobal(items);
        WindowDropdownShowText(screenPos, extray, colour, flags, items.size(), prefRowsPerColumn);
    }

    /**
     * Shows a text dropdown menu.
     *  rct2: 0x006ECFB9, although 0x006ECE50 is real version
     *
     * @param x (cx)
     * @param y (dx)
     * @param extray (di)
     * @param flags (bh)
     * @param num_items (bx)
     * @param colour (al)
     * @param customItemHeight (ah) requires flag set as well
     */
    void WindowDropdownShowTextCustomWidth(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t customItemHeight, uint8_t flags,
        size_t num_items, int32_t width, size_t prefRowsPerColumn)
    {
        gInputFlags.unset(InputFlag::dropdownStayOpen, InputFlag::dropdownMouseUp);
        if (flags & Dropdown::Flag::StayOpen || Config::Get().interface.touchEnhancements)
            gInputFlags.set(InputFlag::dropdownStayOpen);

        WindowDropdownClose();

        // Create the window (width/height position are set later)
        auto* windowMgr = GetWindowManager();
        auto* w = windowMgr->Create<DropdownWindow>(
            WindowClass::dropdown, { width, customItemHeight }, { WindowFlag::stickToFront, WindowFlag::noTitleBar });
        if (w != nullptr)
        {
            auto numRowsPerColumn = prefRowsPerColumn > 0 ? static_cast<int32_t>(prefRowsPerColumn) : Dropdown::kItemsMaxSize;
            w->setTextItems(screenPos, extray, colour, customItemHeight, flags, num_items, width, numRowsPerColumn);
        }

        gDropdown.cellDrawFunction = std::nullopt;
    }

    void WindowDropdownShowTextCustomWidth(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t custom_height, uint8_t flags,
        std::span<const Dropdown::Item> items, int32_t width, size_t prefRowsPerColumn)
    {
        copyItemsToGlobal(items);
        WindowDropdownShowTextCustomWidth(
            screenPos, extray, colour, custom_height, flags, items.size(), width, prefRowsPerColumn);
    }

    /**
     * Shows an image dropdown menu.
     *  rct2: 0x006ECFB9
     *
     * @param x (cx)
     * @param y (dx)
     * @param extray (di)
     * @param flags (bh)
     * @param numItems (bx)
     * @param colour (al)
     * @param itemWidth (bp)
     * @param itemHeight (ah)
     * @param numColumns (bl)
     */
    void WindowDropdownShowImage(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags, int32_t numItems,
        int32_t itemWidth, int32_t itemHeight, int32_t numColumns)
    {
        gInputFlags.unset(InputFlag::dropdownStayOpen, InputFlag::dropdownMouseUp);
        if (flags & Dropdown::Flag::StayOpen || Config::Get().interface.touchEnhancements)
            gInputFlags.set(InputFlag::dropdownStayOpen);

        // Close existing dropdown
        WindowDropdownClose();

        // Create the window (width/height position are set later)
        auto* windowMgr = GetWindowManager();
        auto* w = windowMgr->Create<DropdownWindow>(WindowClass::dropdown, { itemWidth, itemHeight }, WindowFlag::stickToFront);
        if (w != nullptr)
        {
            w->setImageItems(screenPos, extray, colour, numItems, itemWidth, itemHeight, numColumns);
        }

        gDropdown.cellDrawFunction = std::nullopt;
    }

    void WindowDropdownShowCustom(
        const ScreenCoordsXY& screenPos, int32_t extraY, ColourWithFlags colour, uint8_t flags,
        Dropdown::CellDrawFunction drawFunction, int32_t numItems, int32_t itemWidth, int32_t itemHeight, int32_t numColumns)
    {
        // Fall back to image internals
        WindowDropdownShowImage(screenPos, extraY, colour, flags, numItems, itemWidth, itemHeight, numColumns);

        gDropdown.cellDrawFunction = drawFunction;
    }

    void WindowDropdownClose()
    {
        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::dropdown);
    }

    // OPENRCT2MINI focus-mode-plan §F.10: walk gDropdown.highlightedIndex.
    // Skips separators and disabled items. Wraps at both ends. Called
    // from the widget-focus strategy when focus.up/down fires while
    // the dropdown is the focused window. The dropdown's own onDraw
    // re-reads highlightedIndex each frame; invalidating the window
    // here makes the new selection re-render next frame.
    void WindowDropdownMoveHighlight(int32_t direction)
    {
        const int32_t n = gDropdown.numItems;
        if (n <= 0)
            return;
        if (direction == 0)
            return;
        // OPENRCT2MINI focus-mode-plan §F.14: declare this update
        // as focus-driven. ProcessMouseOver checks navigationSource
        // and leaves highlightedIndex alone when it's `focus`, so
        // the value we set below survives across frames even with
        // the cursor parked off the dropdown. SDL_MOUSEMOTION (real
        // mouse motion) flips the source back to `cursor`.
        gDropdown.navigationSource = Dropdown::NavigationSource::focus;
        // Initial state (no item highlighted yet): jump straight to
        // the first or last item depending on direction. The
        // skip-disabled-and-separator loop below picks the right
        // landing index even when the first/last item is one we'd
        // skip.
        int32_t idx = gDropdown.highlightedIndex;
        if (idx < 0 || idx >= n)
            idx = (direction > 0) ? -1 : n;
        // Step at least once, then keep stepping if we land on a
        // skipped item. Cap the loop at n iterations so a fully-
        // disabled dropdown can't infinite-loop us.
        for (int32_t steps = 0; steps < n; steps++)
        {
            idx = ((idx + direction) % n + n) % n;
            const auto& item = gDropdown.items[idx];
            if (!item.isSeparator() && !item.isDisabled())
            {
                gDropdown.highlightedIndex = idx;
                auto* windowMgr = GetWindowManager();
                if (windowMgr != nullptr)
                    windowMgr->InvalidateByClass(WindowClass::dropdown);
                return;
            }
        }
        // No selectable items — leave highlightedIndex untouched.
    }

    // OPENRCT2MINI focus-mode-widgets-plan §3.1 / Cohort A.2 (2026-05-25):
    // return the on-screen ScreenRect of the item at index `i`. Used by
    // the focus-ring drawer (WidgetFocus::drawFocusOutlineIfActive) to
    // paint a yellow outline around the highlighted item — replacing
    // the unconditional early-return that suppressed the ring across
    // the entire dropdown window.
    //
    // Reuses the cell-layout maths from DropdownWindow::onDraw at line
    // 193: cellCoords are derived from i + the grid layout in gDropdown,
    // then screenCoords = windowPos + 2-pixel border + cell offset. The
    // returned rect spans one item cell (itemWidth × itemHeight).
    ScreenRect WindowDropdownGetItemRect(int32_t i)
    {
        if (i < 0 || i >= gDropdown.numItems)
            return ScreenRect{};
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return ScreenRect{};
        auto* w = windowMgr->FindByClass(WindowClass::dropdown);
        if (w == nullptr)
            return ScreenRect{};
        const int32_t cols = std::max(1, gDropdown.numColumns);
        const int32_t rows = std::max(1, gDropdown.numRows);
        ScreenCoordsXY cellCoords;
        if (gDropdown.listVertically)
            cellCoords = { i / rows, i % rows };
        else
            cellCoords = { i % cols, i / cols };
        const ScreenCoordsXY topLeft = w->windowPos
            + ScreenCoordsXY{ 2 + cellCoords.x * gDropdown.itemWidth,
                              2 + cellCoords.y * gDropdown.itemHeight };
        return ScreenRect{
            topLeft,
            { topLeft.x + gDropdown.itemWidth - 1, topLeft.y + gDropdown.itemHeight - 1 },
        };
    }

    // OPENRCT2MINI focus-mode-widgets-plan §3.1 / Cohort A.3 (2026-05-25):
    // 2D directional navigation for grid-layout dropdowns. Walks the
    // grid honouring gDropdown.numColumns / numRows / listVertically.
    // Skips disabled items and separators by stepping in the same
    // direction until a selectable item is found OR the grid bound is
    // hit, then wraps within-column (for up/down) or within-row (for
    // left/right). The linear WindowDropdownMoveHighlight remains as
    // the back-compat ±1 caller.
    void WindowDropdownMoveHighlightDir(WidgetFocus::Direction direction)
    {
        const int32_t n = gDropdown.numItems;
        if (n <= 0)
            return;
        const int32_t cols = std::max(1, gDropdown.numColumns);
        const int32_t rows = std::max(1, gDropdown.numRows);
        // Single column / single row degenerates to the linear walker.
        if (cols == 1 || rows == 1)
        {
            const int32_t delta = (direction == WidgetFocus::Direction::up
                                   || direction == WidgetFocus::Direction::left)
                ? -1 : +1;
            WindowDropdownMoveHighlight(delta);
            return;
        }

        gDropdown.navigationSource = Dropdown::NavigationSource::focus;

        // Convert linear index → (col, row) per the layout convention.
        const auto itemToCell = [&](int32_t idx) -> std::pair<int32_t, int32_t> {
            if (gDropdown.listVertically)
                return { idx / rows, idx % rows };
            else
                return { idx % cols, idx / cols };
        };
        const auto cellToItem = [&](int32_t col, int32_t row) -> int32_t {
            if (gDropdown.listVertically)
                return col * rows + row;
            else
                return row * cols + col;
        };

        // OPENRCT2MINI CI fix (2026-05-25): explicitly initialise at
        // declaration so GCC 8.3 (Miyoo toolchain) and the AppImage
        // compiler don't fire -Werror=maybe-uninitialized on the switch
        // below — neither has a default: case and older GCC can't prove
        // the enum is exhaustively covered. Host GCC's flow analysis was
        // smarter and accepted the original code, but cross-builds reject
        // it. Initial values are overwritten in every path below; they
        // only exist to silence the warning.
        int32_t curCol = 0, curRow = 0;
        if (gDropdown.highlightedIndex < 0 || gDropdown.highlightedIndex >= n)
        {
            // First press in this dropdown — pick a starting cell based
            // on the entry direction so e.g. "up" lands on the bottom
            // row, matching the linear walker's wrap semantics.
            switch (direction)
            {
                case WidgetFocus::Direction::up:
                    curCol = 0;
                    curRow = rows; // step will -1 below into bottom row
                    break;
                case WidgetFocus::Direction::down:
                    curCol = 0;
                    curRow = -1; // step will +1 below into top row
                    break;
                case WidgetFocus::Direction::left:
                    curCol = cols;
                    curRow = 0;
                    break;
                case WidgetFocus::Direction::right:
                    curCol = -1;
                    curRow = 0;
                    break;
            }
        }
        else
        {
            std::tie(curCol, curRow) = itemToCell(gDropdown.highlightedIndex);
        }

        // Step once, then keep stepping past any separator/disabled
        // landing. Cap the loop at n iterations so a fully-disabled
        // grid can't infinite-loop us.
        int32_t targetCol = curCol;
        int32_t targetRow = curRow;
        for (int32_t steps = 0; steps < n; steps++)
        {
            switch (direction)
            {
                case WidgetFocus::Direction::up:
                    targetRow = ((targetRow - 1) % rows + rows) % rows;
                    break;
                case WidgetFocus::Direction::down:
                    targetRow = ((targetRow + 1) % rows + rows) % rows;
                    break;
                case WidgetFocus::Direction::left:
                    targetCol = ((targetCol - 1) % cols + cols) % cols;
                    break;
                case WidgetFocus::Direction::right:
                    targetCol = ((targetCol + 1) % cols + cols) % cols;
                    break;
            }
            const int32_t targetIdx = cellToItem(targetCol, targetRow);
            // Out-of-range cells (last column with fewer rows, or a
            // partial trailing row) — skip past by stepping again.
            if (targetIdx < 0 || targetIdx >= n)
                continue;
            const auto& item = gDropdown.items[targetIdx];
            if (item.isSeparator() || item.isDisabled())
                continue;
            gDropdown.highlightedIndex = targetIdx;
            auto* windowMgr = GetWindowManager();
            if (windowMgr != nullptr)
                windowMgr->InvalidateByClass(WindowClass::dropdown);
            return;
        }
        // No selectable item reachable — leave highlightedIndex alone.
    }

    // OPENRCT2MINI focus-mode-plan §F.10: commit a dropdown selection
    // and dispatch to the parent. Mirrors MouseInput.cpp's rightPress
    // / DropdownActive handler — the path real mouse clicks take to
    // close the dropdown and call parent->onDropdown.
    //
    // The parent is captured from gPressedWidget, which the
    // onMouseDown chain populated when the user activated the
    // dropdown trigger. For mouse clicks that happens automatically
    // via the mouse-input pump; for focus-mode activations
    // WidgetFocus::pressWidgetByIndex now mirrors the same writes
    // (see its comment), so by the time the dropdown is open and we
    // arrive here, gPressedWidget already points at the trigger
    // widget on the parent window.
    void WindowDropdownSelectIndex(int32_t index)
    {
        if (index < 0 || index >= gDropdown.numItems)
            return;
        if (gDropdown.items[index].isDisabled() || gDropdown.items[index].isSeparator())
            return;

        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return;

        // Snapshot the parent ref BEFORE closing the dropdown, since
        // gPressedWidget may be touched during the close (defensive
        // — currently it isn't, but the safe copy costs nothing).
        const auto parentCls = gPressedWidget.windowClassification;
        const auto parentNum = gPressedWidget.windowNumber;
        const auto parentWidget = gPressedWidget.widgetIndex;

        windowMgr->CloseByClass(WindowClass::dropdown);

        auto* parent = windowMgr->FindByNumber(parentCls, parentNum);
        if (parent != nullptr)
            parent->onDropdown(parentWidget, index);

        // Reset the click state machine. The original mouse-input
        // rightPress block sets _inputState = Normal and clears
        // widgetPressed; replicate that here so the next click
        // starts from a clean slate.
        _inputState = InputState::normal;
        if (gInputFlags.has(InputFlag::widgetPressed))
        {
            gInputFlags.unset(InputFlag::widgetPressed);
            windowMgr->InvalidateWidgetByNumber(parentCls, parentNum, parentWidget);
        }
    }

    /**
     * New function based on 6e914e
     * returns -1 if index is invalid
     */
    int32_t DropdownIndexFromPoint(const ScreenCoordsXY& loc, WindowBase* w)
    {
        if (w->classification == WindowClass::dropdown)
        {
            auto* ddWnd = static_cast<DropdownWindow*>(w);
            return ddWnd->GetIndexFromPoint(loc);
        }
        return -1;
    }

    // Colour ordered for use in colour dropdown
    static constexpr Colour kColoursDropdownOrder[] = {
        Colour::black,          Colour::saturatedRed,   Colour::darkOrange,   Colour::darkYellow,
        Colour::forestGreen,    Colour::saturatedGreen, Colour::deepWater,    Colour::darkBlue,
        Colour::violet,

        Colour::grey,           Colour::brightRed,      Colour::lightOrange,  Colour::yellow,
        Colour::mossGreen,      Colour::brightGreen,    Colour::darkWater,    Colour::lightBlue,
        Colour::brightPurple,

        Colour::white,          Colour::lightPink,      Colour::pastelOrange, Colour::brightYellow,
        Colour::chartreuse,     Colour::limeGreen,      Colour::lightWater,   Colour::icyBlue,
        Colour::lavender,

        Colour::umber,          Colour::maroon,         Colour::sepia,        Colour::saturatedBrown,
        Colour::armyGreen,      Colour::hunterGreen,    Colour::viridian,     Colour::darkPurple,
        Colour::darkPink,

        Colour::darkBrown,      Colour::bordeauxRed,    Colour::salmonPink,   Colour::lightBrown,
        Colour::darkOliveGreen, Colour::oliveGreen,     Colour::darkGreen,    Colour::lightPurple,
        Colour::brightPink,

        Colour::beige,          Colour::coralPink,      Colour::peach,        Colour::tan,
        Colour::honeyDew,       Colour::celadon,        Colour::seafoamGreen, Colour::periwinkle,
        Colour::pastelPink,

        Colour::invisible,      Colour::voidBackground,
    };

    constexpr std::array kColourTooltips = {
        STR_COLOUR_BLACK_TIP,
        STR_COLOUR_SATURATED_RED_TIP,
        STR_COLOUR_DARK_ORANGE_TIP,
        STR_COLOUR_DARK_YELLOW_TIP,
        STR_COLOUR_GRASS_GREEN_DARK_TIP,
        STR_COLOUR_SATURATED_GREEN_TIP,
        STR_COLOUR_DEEP_WATER_TIP,
        STR_COLOUR_DARK_BLUE_TIP,
        STR_COLOUR_SATURATED_PURPLE_DARK_TIP,

        STR_COLOUR_GREY_TIP,
        STR_COLOUR_BRIGHT_RED_TIP,
        STR_COLOUR_LIGHT_ORANGE_TIP,
        STR_COLOUR_YELLOW_TIP,
        STR_COLOUR_MOSS_GREEN_TIP,
        STR_COLOUR_BRIGHT_GREEN_TIP,
        STR_COLOUR_DARK_WATER_TIP,
        STR_COLOUR_LIGHT_BLUE_TIP,
        STR_COLOUR_BRIGHT_PURPLE_TIP,

        STR_COLOUR_WHITE_TIP,
        STR_COLOUR_LIGHT_PINK_TIP,
        STR_COLOUR_ORANGE_LIGHT_TIP,
        STR_COLOUR_BRIGHT_YELLOW_TIP,
        STR_COLOUR_GRASS_GREEN_LIGHT_TIP,
        STR_COLOUR_SATURATED_GREEN_LIGHT_TIP,
        STR_COLOUR_LIGHT_WATER_TIP,
        STR_COLOUR_ICY_BLUE_TIP,
        STR_COLOUR_SATURATED_PURPLE_LIGHT_TIP,

        STR_COLOUR_DULL_BROWN_DARK_TIP,
        STR_COLOUR_BORDEAUX_RED_DARK_TIP,
        STR_COLOUR_TAN_DARK_TIP,
        STR_COLOUR_SATURATED_BROWN_TIP,
        STR_COLOUR_DARK_OLIVE_DARK_TIP,
        STR_COLOUR_OLIVE_DARK_TIP,
        STR_COLOUR_DULL_GREEN_DARK_TIP,
        STR_COLOUR_DARK_PURPLE_TIP,
        STR_COLOUR_DARK_PINK_TIP,

        STR_COLOUR_DARK_BROWN_TIP,
        STR_COLOUR_BORDEAUX_RED_TIP,
        STR_COLOUR_SALMON_PINK_TIP,
        STR_COLOUR_LIGHT_BROWN_TIP,
        STR_COLOUR_DARK_OLIVE_GREEN_TIP,
        STR_COLOUR_OLIVE_GREEN_TIP,
        STR_COLOUR_DARK_GREEN_TIP,
        STR_COLOUR_LIGHT_PURPLE_TIP,
        STR_COLOUR_BRIGHT_PINK_TIP,

        STR_COLOUR_DULL_BROWN_LIGHT_TIP,
        STR_COLOUR_BORDEAUX_RED_LIGHT_TIP,
        STR_COLOUR_TAN_LIGHT_TIP,
        STR_COLOUR_SATURATED_BROWN_LIGHT_TIP,
        STR_COLOUR_DARK_OLIVE_LIGHT_TIP,
        STR_COLOUR_OLIVE_LIGHT_TIP,
        STR_COLOUR_DULL_GREEN_LIGHT_TIP,
        STR_COLOUR_DULL_PURPLE_LIGHT_TIP,
        STR_COLOUR_MAGENTA_LIGHT_TIP,

        STR_COLOUR_INVISIBLE_TIP,
        STR_COLOUR_VOID_TIP,
    };

    Colour ColourDropDownIndexToColour(uint8_t ddidx)
    {
        return kColoursDropdownOrder[ddidx];
    }

    /**
     *  rct2: 0x006ED43D
     */
    void WindowDropdownShowColour(
        WindowBase* w, Widget* widget, ColourWithFlags dropdownColour, Colour selectedColour, bool alwaysHideSpecialColours)
    {
        int32_t defaultIndex = -1;

        const bool specialColoursEnabled = !alwaysHideSpecialColours && getGameState().cheats.allowSpecialColourSchemes;
        auto numColours = specialColoursEnabled ? static_cast<uint8_t>(Drawing::kColourNumTotal) : kColourNumNormal;
        // Set items
        for (uint64_t i = 0; i < numColours; i++)
        {
            auto orderedColour = ColourDropDownIndexToColour(i);
            if (selectedColour == orderedColour)
                defaultIndex = i;

            ImageId imageId;
            if (Config::Get().interface.enlargedUi)
            {
                imageId = (orderedColour == Colour::invisible) ? ImageId(SPR_G2_ICON_PALETTE_INVISIBLE_LARGE, Colour::white)
                                                               : ImageId(SPR_G2_ICON_PALETTE_LARGE, orderedColour);
            }
            else
            {
                imageId = (orderedColour == Colour::invisible) ? ImageId(SPR_G2_ICON_PALETTE_INVISIBLE, Colour::white)
                                                               : ImageId(SPR_PALETTE_BTN, orderedColour);
            }

            // OPENRCT2MINI: cut 36. GCC 8.3 can't convert a designated-init
            // brace-list to Dropdown::Item directly when the struct has a
            // FlagHolder member (non-aggregate). Wrap explicitly.
            gDropdown.items[i] = Dropdown::Item{ .type = Dropdown::ItemType::colour, .image = imageId, .tooltip = kColourTooltips[i] };
        }

        // Show dropdown
        auto squareSize = DropdownWindow::GetDefaultRowHeight();
        WindowDropdownShowImage(
            w->windowPos + ScreenCoordsXY{ widget->left, widget->top }, widget->height(), dropdownColour,
            Dropdown::Flag::StayOpen, numColours, squareSize, squareSize,
            DropdownGetAppropriateImageDropdownItemsPerRow(static_cast<uint32_t>(numColours)));

        gDropdown.hasTooltips = true;
        gDropdown.lastTooltipHover = -1;
        gDropdown.defaultIndex = defaultIndex;
    }

    uint32_t DropdownGetAppropriateImageDropdownItemsPerRow(uint32_t numItems)
    {
        // If above the table size return the last element
        return kAppropriateImageDropdownItemsPerRow[std::min<uint32_t>(
            numItems, static_cast<uint32_t>(std::size(kAppropriateImageDropdownItemsPerRow) - 1))];
    }
} // namespace OpenRCT2::Ui::Windows

namespace OpenRCT2::Dropdown
{
    static Item StringItem(ItemType _type, const utf8* string)
    {
        auto ret = Item{ .type = _type };
        String::safeUtf8Copy(ret.text, string, sizeof(ret.text));
        return ret;
    }

    Item MenuLabel(StringId stringId)
    {
        return StringItem(ItemType::regular, LanguageGetString(stringId));
    }

    Item MenuLabel(u8string string)
    {
        return StringItem(ItemType::regular, string.c_str());
    }

    Item MenuLabel(const utf8* string)
    {
        return StringItem(ItemType::regular, string);
    }

    Item MenuLabel(StringId format, const Formatter& ft)
    {
        auto string = FormatStringIDLegacy(format, ft.Data());
        return MenuLabel(string);
    }

    Item PlainMenuLabel(u8string string)
    {
        return StringItem(ItemType::plain, string.c_str());
    }

    Item PlainMenuLabel(const utf8* string)
    {
        return StringItem(ItemType::plain, string);
    }

    Item PlainMenuLabel(StringId stringId)
    {
        return StringItem(ItemType::plain, LanguageGetString(stringId));
    }

    Item PlainMenuLabel(StringId format, const Formatter& ft)
    {
        auto string = FormatStringIDLegacy(format, ft.Data());
        return StringItem(ItemType::plain, string.c_str());
    }

    Item ToggleOption(StringId stringId)
    {
        return StringItem(ItemType::toggle, LanguageGetString(stringId));
    }
} // namespace OpenRCT2::Dropdown
