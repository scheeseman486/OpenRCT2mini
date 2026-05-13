/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <array>
#include <functional>
#include <openrct2-ui/UiStringIds.h>
#include <openrct2/Limits.h>
#include <openrct2/core/EnumUtils.hpp>
#include <openrct2/core/StringTypes.h>
#include <openrct2/interface/Window.h>
#include <optional>
#include <openrct2/core/Span.hpp>

struct ImageId;

namespace OpenRCT2
{
    struct ColourWithFlags;
    class Formatter;
} // namespace OpenRCT2

namespace OpenRCT2::Dropdown
{
    struct Item;

    constexpr StringId kSeparatorString = 0;
    constexpr int32_t kItemsMaxSize = 1024;
    static_assert(kItemsMaxSize >= Limits::kMaxRidesInPark);

    struct DropdownState;

    using CellDrawFunction = std::function<void(Drawing::RenderTarget&, const Item&, int32_t)>;

} // namespace OpenRCT2::Dropdown

namespace OpenRCT2::Ui::Windows
{
    extern Dropdown::DropdownState gDropdown;

    void WindowDropdownShowText(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags, size_t num_items,
        size_t prefRowsPerColumn = 0);
    void WindowDropdownShowText(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags,
        std::span<const Dropdown::Item> items, size_t prefRowsPerColumn = 0);
    void WindowDropdownShowTextCustomWidth(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t custom_height, uint8_t flags,
        size_t num_items, int32_t width, size_t prefRowsPerColumn = 0);
    void WindowDropdownShowTextCustomWidth(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t custom_height, uint8_t flags,
        std::span<const Dropdown::Item> items, int32_t width, size_t prefRowsPerColumn = 0);

    void WindowDropdownShowImage(
        const ScreenCoordsXY& screenPos, int32_t extray, ColourWithFlags colour, uint8_t flags, int32_t numItems,
        int32_t itemWidth, int32_t itemHeight, int32_t numColumns);
    void WindowDropdownShowCustom(
        const ScreenCoordsXY& screenPos, int32_t extraY, ColourWithFlags colour, uint8_t flags,
        Dropdown::CellDrawFunction drawFunction, int32_t numItems, int32_t itemWidth, int32_t itemHeight, int32_t numColumns);

    void WindowDropdownClose();

    int32_t DropdownIndexFromPoint(const ScreenCoordsXY& loc, WindowBase* w);

    // OPENRCT2MINI focus-mode-plan §F.10: focus-mode helpers for the
    // dropdown window. The dropdown stores item state in gDropdown
    // (highlightedIndex + items[]); the mouse-input path drives
    // selection via per-frame cursor-over-item hit-testing. Focus
    // mode bypasses the cursor by walking highlightedIndex with
    // focus.up/down and selecting via a synthetic dispatch.
    //
    // WindowDropdownMoveHighlight(direction): adjust highlightedIndex
    // by direction (+1/-1), skipping disabled items and separators,
    // wrapping at the ends. If highlightedIndex is -1 (initial state)
    // the first call lands on item 0 (forward) or last (backward).
    //
    // WindowDropdownSelectIndex(index): commit `index` as the
    // selected item — closes the dropdown and dispatches to the
    // parent window's onDropdown handler (the parent is captured
    // from gPressedWidget which the click-time onMouseDown chain
    // populated). Mirrors the rightPress block in MouseInput.cpp
    // (the path real mouse clicks use to close + dispatch).
    void WindowDropdownMoveHighlight(int32_t direction);
    void WindowDropdownSelectIndex(int32_t index);

    void WindowDropdownShowColour(
        WindowBase* w, Widget* widget, ColourWithFlags dropdownColour, Drawing::Colour selectedColour,
        bool alwaysHideSpecialColours = false);

    Drawing::Colour ColourDropDownIndexToColour(uint8_t ddidx);

    uint32_t DropdownGetAppropriateImageDropdownItemsPerRow(uint32_t numItems);
} // namespace OpenRCT2::Ui::Windows

namespace OpenRCT2::Dropdown
{
    enum Flag
    {
        CustomHeight = (1 << 6), // never set?
        StayOpen = (1 << 7)
    };

    enum class ItemFlag : uint8_t
    {
        isDisabled = 0,
        isChecked = 1,
    };
    using ItemFlags = FlagHolder<uint8_t, ItemFlag>;

    enum class ItemType
    {
        /**
         * Regular label with padding on the left. Will get marked with » if selected.
         */
        regular,
        /**
         * Same as ‘regular’, but will get marked with a tick if selected.
         */
        toggle,
        /**
         * Label with no padding
         */
        plain,
        image,
        colour,
        separator,
    };

    struct Item
    {
        ItemType type = ItemType::regular;
        utf8 text[256]{};
        /**
         * Underlying value, e.g. the ID of an object, the backing value of an enum, etc.
         */
        uint32_t value{};
        ImageId image{};
        ItemFlags flags{};
        StringId tooltip{};

        constexpr bool isSeparator() const
        {
            return type == ItemType::separator;
        }

        constexpr bool isDisabled() const
        {
            return flags.has(ItemFlag::isDisabled);
        }

        constexpr void setDisabled(bool on)
        {
            flags.set(ItemFlag::isDisabled, on);
        }

        constexpr bool isChecked() const
        {
            return flags.has(ItemFlag::isChecked);
        }

        constexpr void setChecked(bool on)
        {
            flags.set(ItemFlag::isChecked, on);
        }
    };

    struct ItemExt
    {
        constexpr ItemExt(int32_t _expectedItemIndex, uint32_t _itemFormat, StringId _stringId)
            : expectedItemIndex(_expectedItemIndex)
            , itemFormat(_itemFormat)
            , stringId(_stringId)
        {
        }

        int32_t expectedItemIndex;
        uint32_t itemFormat;
        StringId stringId;
    };

    constexpr ItemExt ToggleOption(int32_t _expectedItemIndex, StringId _stringId)
    {
        return ItemExt(_expectedItemIndex, STR_TOGGLE_OPTION, _stringId);
    }

    constexpr ItemExt ExtSeparator()
    {
        return ItemExt(-1, kSeparatorString, kStringIdEmpty);
    }

    /**
     * Regular menu item, which shows a » symbol when selected
     */
    Item MenuLabel(StringId stringId);
    Item MenuLabel(u8string string);
    Item MenuLabel(const utf8* string);
    Item MenuLabel(StringId format, const Formatter& ft);

    /**
     * Leaves out the left padding where a checkmark can be drawn, use only for menu where no item can be checked.
     */
    Item PlainMenuLabel(StringId stringId);
    Item PlainMenuLabel(u8string string);
    Item PlainMenuLabel(const utf8* string);
    Item PlainMenuLabel(StringId format, const Formatter& ft);

    /**
     * Like MenuLabel, but shows a tick when selected.
     */
    Item ToggleOption(StringId stringId);

    constexpr Item Separator()
    {
        return Item{ .type = ItemType::separator };
    }

    constexpr Item ImageItem(ImageId image, StringId tooltip = kStringIdEmpty)
    {
        return Item{ .type = ItemType::image, .image = image, .tooltip = tooltip };
    }

    // OPENRCT2MINI focus-mode-plan §F.14: dropdown navigation source.
    // The dropdown highlight (gDropdown.highlightedIndex) can be
    // driven by EITHER:
    //   - cursor: mouse hover via MouseInput::ProcessMouseOver. The
    //     cursor position decides the highlight each frame.
    //   - focus: D-pad / arrow keys via WindowDropdownMoveHighlight,
    //     which steps the highlight ±1 explicitly.
    // Without an explicit source flag the two paths fight each
    // other: ProcessMouseOver runs every frame and clobbers the
    // focus-driven highlight by re-deriving from cursor position.
    // The source flag flips whenever a real input event of either
    // kind arrives (SDL_MOUSEMOTION → cursor;
    // WindowDropdownMoveHighlight → focus) and gates the per-frame
    // ProcessMouseOver mutation. Each path wins exactly when its
    // input was last used; switching modes is a single event away.
    enum class NavigationSource : uint8_t
    {
        cursor = 0,
        focus,
    };

    struct DropdownState
    {
        int32_t numItems{};
        std::array<Item, kItemsMaxSize> items{};
        bool hasTooltips{};
        int32_t lastTooltipHover{};
        int32_t highlightedIndex{};
        int32_t defaultIndex{};
        NavigationSource navigationSource{ NavigationSource::cursor };

        std::optional<CellDrawFunction> cellDrawFunction;
    };

    template<int N>
    void SetItems(const ItemExt (&items)[N])
    {
        for (int i = 0; i < N; ++i)
        {
            const ItemExt& item = items[i];
            if (item.itemFormat == kSeparatorString)
                Ui::Windows::gDropdown.items[i] = Separator();
            else
                Ui::Windows::gDropdown.items[i] = ToggleOption(item.stringId);
        }
    }

    template<int N>
    constexpr bool ItemIDsMatchIndices(const ItemExt (&items)[N])
    {
        for (int i = 0; i < N; ++i)
        {
            const ItemExt& item = items[i];
            if (item.expectedItemIndex >= 0 && item.expectedItemIndex != i)
                return false;
        }

        return true;
    }
} // namespace OpenRCT2::Dropdown
