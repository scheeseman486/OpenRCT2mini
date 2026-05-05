/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "../UiContext.h"
#include "../interface/InGameConsole.h"
#include "../scripting/CustomMenu.h"

#include <iterator>
#include <limits>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/LandTool.h>
#include <openrct2-ui/interface/Viewport.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Cheats.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/Editor.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/Version.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/general/GameSetSpeedAction.h>
#include <openrct2/actions/general/LoadOrQuitAction.h>
#include <openrct2/actions/general/PauseToggleAction.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Numerics.hpp>
#include <openrct2/core/String.hpp>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/interface/Chat.h>
#include <openrct2/interface/ColourWithFlags.h>
#include <openrct2/interface/Screenshot.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/network/Network.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <openrct2/world/Footpath.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/Scenery.h>
#include <string>

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
extern "C" {
extern void EmscriptenLoadGame();
}
#endif

using namespace OpenRCT2::Numerics;

namespace OpenRCT2::Ui::Windows
{
    enum
    {
        WIDX_PAUSE,
        WIDX_FILE_MENU,
        WIDX_MUTE,
        WIDX_ZOOM_OUT,
        WIDX_ZOOM_IN,
        WIDX_ROTATE_ANTI_CLOCKWISE,
        WIDX_ROTATE_CLOCKWISE,
        WIDX_VIEW_MENU,
        WIDX_MAP,

        WIDX_LAND,
        WIDX_WATER,
        WIDX_SCENERY,
        WIDX_PATH,
        WIDX_CONSTRUCT_RIDE,
        WIDX_RIDES,
        WIDX_PARK,
        WIDX_STAFF,
        WIDX_GUESTS,
        WIDX_CLEAR_SCENERY,

        WIDX_FASTFORWARD,
        WIDX_CHEATS,
        WIDX_DEBUG,
        WIDX_FINANCES,
        WIDX_RESEARCH,
        WIDX_NEWS,
        WIDX_NETWORK,
        WIDX_CHAT,

        WIDX_SEPARATOR,
    };

    enum FileMenuDdidx
    {
        DDIDX_NEW_GAME = 0,
        DDIDX_LOAD_GAME = 1,
        // separator
        DDIDX_SAVE_GAME = 3,
        DDIDX_SAVE_GAME_AS = 4,
        // separator
        DDIDX_SCREENSHOT = 6,
        DDIDX_GIANT_SCREENSHOT = 7,
        // separator
        DDIDX_ABOUT = 9,
        // OPENRCT2MINI: removed DDIDX_FILE_BUG_ON_GITHUB. The upstream entry
        // pointed users at OpenRCT2's own GitHub issue tracker — wrong place
        // to file OpenRCT2mini-specific bugs. All subsequent indices shift
        // down by one.
        DDIDX_UPDATE_AVAILABLE = 10,
        DDIDX_OPTIONS = 11,
        // separator
        DDIDX_QUIT_TO_MENU = 13,
        DDIDX_EXIT_OPENRCT2 = 14,
    };

    enum TopToolbarViewMenuDdidx
    {
        DDIDX_UNDERGROUND_INSIDE = 0,
        DDIDX_TRANSPARENT_WATER = 1,
        DDIDX_HIDE_BASE = 2,
        DDIDX_HIDE_VERTICAL = 3,
        // separator
        DDIDX_HIDE_RIDES = 5,
        DDIDX_HIDE_VEHICLES = 6,
        DDIDX_HIDE_VEGETATION = 7,
        DDIDX_HIDE_SCENERY = 8,
        DDIDX_HIDE_PATHS = 9,
        DDIDX_HIDE_SUPPORTS = 10,
        DDIDX_HIDE_GUESTS = 11,
        DDIDX_HIDE_STAFF = 12,
        // separator
        DDIDX_LAND_HEIGHTS = 14,
        DDIDX_TRACK_HEIGHTS = 15,
        DDIDX_PATH_HEIGHTS = 16,
        // separator
        DDIDX_VIEW_CLIPPING = 18,
        DDIDX_HIGHLIGHT_PATH_ISSUES = 19,
        // separator
        DDIDX_TRANSPARENCY = 21,

        TOP_TOOLBAR_VIEW_MENU_COUNT,
    };

    enum TopToolbarDebugDdidx
    {
        DDIDX_CONSOLE = 0,
        DDIDX_DEBUG_PAINT = 1,

        TOP_TOOLBAR_DEBUG_COUNT,
    };

    enum TopToolbarNetworkDdidx
    {
        DDIDX_MULTIPLAYER = 0,
        DDIDX_MULTIPLAYER_RECONNECT = 1,

        TOP_TOOLBAR_NETWORK_COUNT,
    };

    enum
    {
        DDIDX_CHEATS,
        DDIDX_TILE_INSPECTOR = 1,
        DDIDX_OBJECT_SELECTION = 2,
        DDIDX_INVENTIONS_LIST = 3,
        DDIDX_SCENARIO_OPTIONS = 4,
        // 5 is a separator
        DDIDX_ENABLE_SANDBOX_MODE = 6,
        DDIDX_DISABLE_CLEARANCE_CHECKS = 7,
        DDIDX_DISABLE_SUPPORT_LIMITS = 8,
        // OPENRCT2MINI: cut 30. Debug items folded into the cheats dropdown so
        // the toolbar drops one button. Indices 9/10 are added at runtime when
        // Config::general.debuggingTools is on; the standalone WIDX_DEBUG
        // widget is hidden permanently.
        // 9 is a separator (added when debug items are appended)
        DDIDX_DEBUG_CONSOLE = 10,
        DDIDX_DEBUG_PAINT_TOOL = 11,

        TOP_TOOLBAR_CHEATS_COUNT,
    };

    enum
    {
        DDIDX_SHOW_MAP,
        DDIDX_OPEN_VIEWPORT,
    };

    enum
    {
        DDIDX_ROTATE_CLOCKWISE,
        DDIDX_ROTATE_ANTI_CLOCKWISE,
    };

#pragma region Toolbar_widget_ordering

    // clang-format off
    static constexpr std::array kWidgetOrderLeftGroup = {
        WIDX_PAUSE,
        WIDX_FASTFORWARD,
        WIDX_FILE_MENU,
        WIDX_MUTE,
        WIDX_NETWORK,
        WIDX_CHAT,
        WIDX_CHEATS,
        WIDX_DEBUG,

        WIDX_SEPARATOR,

        WIDX_ZOOM_OUT,
        WIDX_ZOOM_IN,
        WIDX_ROTATE_ANTI_CLOCKWISE,
        WIDX_ROTATE_CLOCKWISE,
        WIDX_VIEW_MENU,
        WIDX_MAP,
    };

    static constexpr std::array kWidgetOrderRightGroup = {
        WIDX_CLEAR_SCENERY,
        WIDX_LAND,
        WIDX_WATER,
        WIDX_SCENERY,
        WIDX_PATH,
        WIDX_CONSTRUCT_RIDE,

        WIDX_SEPARATOR,

        WIDX_FINANCES,
        WIDX_RESEARCH,
        WIDX_RIDES,
        WIDX_PARK,
        WIDX_STAFF,
        WIDX_GUESTS,
        WIDX_NEWS,
    };

    static constexpr size_t _totalToolbarElements = kWidgetOrderLeftGroup.size() + kWidgetOrderRightGroup.size();

    // Make a combined version of both halves of the toolbar, with a separator halfway.
    // OPENRCT2MINI: cut 33. std::copy became constexpr in C++20; manual loops
    // for C++17 compat.
    static constexpr std::array<int, _totalToolbarElements + 1> kWidgetOrderCombined = []() {
        std::array<int, _totalToolbarElements + 1> combined{};
        size_t idx = 0;
        for (auto v : kWidgetOrderLeftGroup) combined[idx++] = v;
        combined[idx++] = WIDX_SEPARATOR;
        for (auto v : kWidgetOrderRightGroup) combined[idx++] = v;
        return combined;
    }();

#pragma endregion

    static const auto _topToolbarWidgets = makeWidgets(
        makeRemapWidget({  0, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TOOLBAR_PAUSE,          STR_PAUSE_GAME_TIP                ), // Pause
        makeRemapWidget({ 60, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TOOLBAR_FILE,           STR_DISC_AND_GAME_OPTIONS_TIP     ), // File menu
        makeRemapWidget({250, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_G2_TOOLBAR_MUTE,        STR_TOOLBAR_MUTE_TIP              ), // Mute
        makeRemapWidget({100, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TOOLBAR_ZOOM_OUT,       STR_ZOOM_OUT_TIP                  ), // Zoom out
        makeRemapWidget({130, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TOOLBAR_ZOOM_IN,        STR_ZOOM_IN_TIP                   ), // Zoom in
        makeRemapWidget({190, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TAB_TOOLBAR,            STR_ROTATE_ANTI_CLOCKWISE         ), // Rotate camera anti-clockwise
        makeRemapWidget({160, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TOOLBAR_ROTATE,         STR_ROTATE_CLOCKWISE              ), // Rotate camera clockwise
        makeRemapWidget({220, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TOOLBAR_VIEW,           STR_VIEW_OPTIONS_TIP              ), // Transparency menu
        makeRemapWidget({267, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::secondary , SPR_TOOLBAR_MAP,            STR_SHOW_MAP_TIP                  ), // Map
        makeRemapWidget({297, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_LAND,           STR_ADJUST_LAND_TIP               ), // Land
        makeRemapWidget({327, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_WATER,          STR_ADJUST_WATER_TIP              ), // Water
        makeRemapWidget({357, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_SCENERY,        STR_PLACE_SCENERY_TIP             ), // Scenery
        makeRemapWidget({387, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_FOOTPATH,       STR_BUILD_FOOTPATH_TIP            ), // Path
        makeRemapWidget({490, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_CONSTRUCT_RIDE, STR_BUILD_RIDE_TIP                ), // Construct ride
        makeRemapWidget({520, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TOOLBAR_RIDES,          STR_RIDES_IN_PARK_TIP             ), // Rides
        makeRemapWidget({550, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TOOLBAR_PARK,           STR_PARK_INFORMATION_TIP          ), // Park
        makeRemapWidget({560, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TAB_TOOLBAR,            STR_STAFF_TIP                     ), // Staff
        makeRemapWidget({560, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TOOLBAR_GUESTS,         STR_GUESTS_TIP                    ), // Guests
        makeRemapWidget({560, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::tertiary  , SPR_TOOLBAR_CLEAR_SCENERY,  STR_CLEAR_SCENERY_TIP             ), // Clear scenery
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TAB_TOOLBAR,            STR_GAME_SPEED_TIP                ), // Fast forward
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TAB_TOOLBAR,            STR_CHEATS_TIP                    ), // Cheats
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TAB_TOOLBAR,            STR_DEBUG_TIP                     ), // Debug
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TAB_TOOLBAR,            STR_SCENARIO_OPTIONS_FINANCIAL_TIP), // Finances
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TAB_TOOLBAR,            STR_FINANCES_RESEARCH_TIP         ), // Research
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::quaternary, SPR_TAB_TOOLBAR,            STR_SHOW_RECENT_MESSAGES_TIP      ), // News
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_G2_TOOLBAR_MULTIPLAYER, STR_SHOW_MULTIPLAYER_STATUS_TIP   ), // Network
        makeRemapWidget({ 30, 0}, {30, kTopToolbarHeight + 1}, WidgetType::trnBtn, WindowColour::primary   , SPR_TAB_TOOLBAR,            STR_TOOLBAR_CHAT_TIP              ), // Chat
        makeWidget     ({  0, 0}, {10,                     1}, WidgetType::empty,  WindowColour::primary                                                                   )  // Artificial widget separator
    );
    // clang-format on

    class TopToolbar final : public Window
    {
    private:
        bool _waitingForPause{ false };

        void initViewMenu(Widget& widget)
        {
            using namespace Dropdown;
            constexpr ItemExt items[] = {
                ToggleOption(DDIDX_UNDERGROUND_INSIDE, STR_UNDERGROUND_VIEW),
                ToggleOption(DDIDX_TRANSPARENT_WATER, STR_VIEWPORT_TRANSPARENT_WATER),
                ToggleOption(DDIDX_HIDE_BASE, STR_REMOVE_BASE_LAND),
                ToggleOption(DDIDX_HIDE_VERTICAL, STR_REMOVE_VERTICAL_FACES),
                ExtSeparator(),
                ToggleOption(DDIDX_HIDE_RIDES, STR_SEE_THROUGH_RIDES),
                ToggleOption(DDIDX_HIDE_VEHICLES, STR_SEE_THROUGH_VEHICLES),
                ToggleOption(DDIDX_HIDE_VEGETATION, STR_SEE_THROUGH_VEGETATION),
                ToggleOption(DDIDX_HIDE_SCENERY, STR_SEE_THROUGH_SCENERY),
                ToggleOption(DDIDX_HIDE_PATHS, STR_SEE_THROUGH_PATHS),
                ToggleOption(DDIDX_HIDE_SUPPORTS, STR_SEE_THROUGH_SUPPORTS),
                ToggleOption(DDIDX_HIDE_GUESTS, STR_SEE_THROUGH_GUESTS),
                ToggleOption(DDIDX_HIDE_STAFF, STR_SEE_THROUGH_STAFF),
                ExtSeparator(),
                ToggleOption(DDIDX_LAND_HEIGHTS, STR_HEIGHT_MARKS_ON_LAND),
                ToggleOption(DDIDX_TRACK_HEIGHTS, STR_HEIGHT_MARKS_ON_RIDE_TRACKS),
                ToggleOption(DDIDX_PATH_HEIGHTS, STR_HEIGHT_MARKS_ON_PATHS),
                ExtSeparator(),
                ToggleOption(DDIDX_VIEW_CLIPPING, STR_VIEW_CLIPPING_MENU),
                ToggleOption(DDIDX_HIGHLIGHT_PATH_ISSUES, STR_HIGHLIGHT_PATH_ISSUES_MENU),
                ExtSeparator(),
                ToggleOption(DDIDX_TRANSPARENCY, STR_TRANSPARENCY_OPTIONS),
            };

            static_assert(ItemIDsMatchIndices(items));

            SetItems(items);

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[1].withFlag(ColourFlag::translucent, true), Dropdown::Flag::StayOpen, TOP_TOOLBAR_VIEW_MENU_COUNT);

            auto mvpFlags = WindowGetMain()->viewport->flags;
            gDropdown.items[DDIDX_UNDERGROUND_INSIDE].setChecked(mvpFlags & VIEWPORT_FLAG_UNDERGROUND_INSIDE);
            gDropdown.items[DDIDX_TRANSPARENT_WATER].setChecked(Config::Get().general.transparentWater);
            gDropdown.items[DDIDX_HIDE_BASE].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_BASE);
            gDropdown.items[DDIDX_HIDE_VERTICAL].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_VERTICAL);
            gDropdown.items[DDIDX_HIDE_RIDES].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_RIDES);
            gDropdown.items[DDIDX_HIDE_VEHICLES].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_VEHICLES);
            gDropdown.items[DDIDX_HIDE_VEGETATION].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_VEGETATION);
            gDropdown.items[DDIDX_HIDE_SCENERY].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_SCENERY);
            gDropdown.items[DDIDX_HIDE_PATHS].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_PATHS);
            gDropdown.items[DDIDX_HIDE_SUPPORTS].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_SUPPORTS);
            gDropdown.items[DDIDX_HIDE_GUESTS].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_GUESTS);
            gDropdown.items[DDIDX_HIDE_STAFF].setChecked(mvpFlags & VIEWPORT_FLAG_HIDE_STAFF);
            gDropdown.items[DDIDX_LAND_HEIGHTS].setChecked(mvpFlags & VIEWPORT_FLAG_LAND_HEIGHTS);
            gDropdown.items[DDIDX_TRACK_HEIGHTS].setChecked(mvpFlags & VIEWPORT_FLAG_TRACK_HEIGHTS);
            gDropdown.items[DDIDX_PATH_HEIGHTS].setChecked(mvpFlags & VIEWPORT_FLAG_PATH_HEIGHTS);
            gDropdown.items[DDIDX_VIEW_CLIPPING].setChecked(mvpFlags & VIEWPORT_FLAG_CLIP_VIEW);
            gDropdown.items[DDIDX_HIGHLIGHT_PATH_ISSUES].setChecked(mvpFlags & VIEWPORT_FLAG_HIGHLIGHT_PATH_ISSUES);

            gDropdown.defaultIndex = DDIDX_UNDERGROUND_INSIDE;
        }

        void viewMenuDropdown(int16_t dropdownIndex)
        {
            auto* w = WindowGetMain();
            if (w != nullptr)
            {
                switch (dropdownIndex)
                {
                    case DDIDX_UNDERGROUND_INSIDE:
                        w->viewport->flags ^= VIEWPORT_FLAG_UNDERGROUND_INSIDE;
                        break;
                    case DDIDX_TRANSPARENT_WATER:
                        Config::Get().general.transparentWater ^= 1;
                        Config::Save();
                        break;
                    case DDIDX_HIDE_BASE:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_BASE;
                        break;
                    case DDIDX_HIDE_VERTICAL:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_VERTICAL;
                        break;
                    case DDIDX_HIDE_RIDES:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_RIDES;
                        break;
                    case DDIDX_HIDE_VEHICLES:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_VEHICLES;
                        break;
                    case DDIDX_HIDE_VEGETATION:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_VEGETATION;
                        break;
                    case DDIDX_HIDE_SCENERY:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_SCENERY;
                        break;
                    case DDIDX_HIDE_PATHS:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_PATHS;
                        break;
                    case DDIDX_HIDE_SUPPORTS:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_SUPPORTS;
                        break;
                    case DDIDX_HIDE_GUESTS:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_GUESTS;
                        break;
                    case DDIDX_HIDE_STAFF:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIDE_STAFF;
                        break;
                    case DDIDX_LAND_HEIGHTS:
                        w->viewport->flags ^= VIEWPORT_FLAG_LAND_HEIGHTS;
                        break;
                    case DDIDX_TRACK_HEIGHTS:
                        w->viewport->flags ^= VIEWPORT_FLAG_TRACK_HEIGHTS;
                        break;
                    case DDIDX_PATH_HEIGHTS:
                        w->viewport->flags ^= VIEWPORT_FLAG_PATH_HEIGHTS;
                        break;
                    case DDIDX_VIEW_CLIPPING:
                    {
                        auto* windowMgr = GetWindowManager();
                        if (windowMgr->FindByClass(WindowClass::viewClipping) == nullptr)
                        {
                            ContextOpenWindow(WindowClass::viewClipping);
                        }
                        else
                        {
                            // If window is already open, toggle the view clipping on/off
                            w->viewport->flags ^= VIEWPORT_FLAG_CLIP_VIEW;
                        }
                        break;
                    }
                    case DDIDX_HIGHLIGHT_PATH_ISSUES:
                        w->viewport->flags ^= VIEWPORT_FLAG_HIGHLIGHT_PATH_ISSUES;
                        break;
                    case DDIDX_TRANSPARENCY:
                        ContextOpenWindow(WindowClass::transparency);
                        break;
                    default:
                        return;
                }
                w->invalidate();
            }
        }

        void initMapMenu(Widget& widget)
        {
            auto i = 0;
            gDropdown.items[i++] = Dropdown::PlainMenuLabel(STR_SHORTCUT_SHOW_MAP);
            gDropdown.items[i++] = Dropdown::PlainMenuLabel(STR_EXTRA_VIEWPORT);
            if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep == EditorStep::LandscapeEditor)
            {
                gDropdown.items[i++] = Dropdown::PlainMenuLabel(STR_MAPGEN_MENU_ITEM);
            }

#ifdef ENABLE_SCRIPTING
            const auto& customMenuItems = Scripting::CustomMenuItems;
            if (!customMenuItems.empty())
            {
                gDropdown.items[i++] = Dropdown::Separator();
                for (const auto& item : customMenuItems)
                {
                    if (item.Kind == Scripting::CustomToolbarMenuItemKind::Standard)
                    {
                        gDropdown.items[i] = Dropdown::PlainMenuLabel(item.Text.c_str());
                        i++;
                    }
                }
            }
#endif

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[1].withFlag(ColourFlag::translucent, true), 0, i);
            gDropdown.defaultIndex = DDIDX_SHOW_MAP;
        }

        void mapMenuDropdown(int16_t dropdownIndex)
        {
            int32_t customStartIndex = 3;
            if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep == EditorStep::LandscapeEditor)
            {
                customStartIndex++;
            }

            if (dropdownIndex < customStartIndex)
            {
                switch (dropdownIndex)
                {
                    case 0:
                        ContextOpenWindow(WindowClass::map);
                        break;
                    case 1:
                        ContextOpenWindow(WindowClass::viewport);
                        break;
                    case 2:
                        ContextOpenWindow(WindowClass::mapgen);
                        break;
                }
            }
            else
            {
#ifdef ENABLE_SCRIPTING
                const auto& customMenuItems = Scripting::CustomMenuItems;
                auto customIndex = static_cast<size_t>(dropdownIndex - customStartIndex);
                size_t i = 0;
                for (const auto& item : customMenuItems)
                {
                    if (item.Kind == Scripting::CustomToolbarMenuItemKind::Standard)
                    {
                        if (i == customIndex)
                        {
                            item.Invoke();
                            break;
                        }
                        i++;
                    }
                }
#endif
            }
        }

        // OPENRCT2MINI: cut 31. Pause is a toggle at the top of the speed
        // dropdown now — the standalone WIDX_PAUSE button is hidden. Indices:
        //   0  Pause (toggle)
        //   1  separator
        //   2..5  Normal / Quick / Fast / Turbo
        //   6  separator        ┐ debuggingTools only
        //   7  Hyper            ┘
        static constexpr int32_t kFFDDIDX_PAUSE = 0;
        static constexpr int32_t kFFDDIDX_SEP1 = 1;
        static constexpr int32_t kFFDDIDX_NORMAL = 2;
        static constexpr int32_t kFFDDIDX_QUICK = 3;
        static constexpr int32_t kFFDDIDX_FAST = 4;
        static constexpr int32_t kFFDDIDX_TURBO = 5;
        static constexpr int32_t kFFDDIDX_SEP2 = 6;
        static constexpr int32_t kFFDDIDX_HYPER = 7;

        void initFastforwardMenu(Widget& widget)
        {
            gDropdown.items[kFFDDIDX_PAUSE] = Dropdown::ToggleOption(STR_SHORTCUT_PAUSE);
            gDropdown.items[kFFDDIDX_SEP1] = Dropdown::Separator();
            gDropdown.items[kFFDDIDX_NORMAL] = Dropdown::MenuLabel(STR_SPEED_NORMAL);
            gDropdown.items[kFFDDIDX_QUICK] = Dropdown::MenuLabel(STR_SPEED_QUICK);
            gDropdown.items[kFFDDIDX_FAST] = Dropdown::MenuLabel(STR_SPEED_FAST);
            gDropdown.items[kFFDDIDX_TURBO] = Dropdown::MenuLabel(STR_SPEED_TURBO);

            int32_t num_items = kFFDDIDX_TURBO + 1; // 6
            if (Config::Get().general.debuggingTools)
            {
                gDropdown.items[kFFDDIDX_SEP2] = Dropdown::Separator();
                gDropdown.items[kFFDDIDX_HYPER] = Dropdown::MenuLabel(STR_SPEED_HYPER);
                num_items = kFFDDIDX_HYPER + 1; // 8
            }

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[0].withFlag(ColourFlag::translucent, true), 0, num_items);

            // Pause check state
            const bool paused = (gGamePaused & GAME_PAUSED_NORMAL) != 0;
            if (paused)
                gDropdown.items[kFFDDIDX_PAUSE].setChecked(true);
            // Multiplayer clients can't toggle pause (mirrors the legacy
            // WIDX_PAUSE click guard).
            if (Network::GetMode() == Network::Mode::client)
                gDropdown.items[kFFDDIDX_PAUSE].setDisabled(true);

            // Speed checkmarks — gGameSpeed is 1-based; map to the new indices.
            if (gGameSpeed >= 1 && gGameSpeed <= 4)
                gDropdown.items[kFFDDIDX_NORMAL + (gGameSpeed - 1)].setChecked(true);
            if (gGameSpeed == 8)
                gDropdown.items[kFFDDIDX_HYPER].setChecked(true);

            // Default index = the next reasonable speed step (or pause if paused).
            if (paused)
            {
                gDropdown.defaultIndex = kFFDDIDX_PAUSE;
            }
            else if (Config::Get().general.debuggingTools)
            {
                gDropdown.defaultIndex = (gGameSpeed == 8) ? kFFDDIDX_NORMAL : kFFDDIDX_NORMAL + gGameSpeed;
            }
            else
            {
                gDropdown.defaultIndex = (gGameSpeed >= 4) ? kFFDDIDX_NORMAL : kFFDDIDX_NORMAL + gGameSpeed;
            }
        }

        void fastforwardMenuDropdown(int16_t dropdownIndex)
        {
            if (dropdownIndex == kFFDDIDX_PAUSE)
            {
                if (Network::GetMode() != Network::Mode::client)
                {
                    auto pauseToggleAction = GameActions::PauseToggleAction();
                    GameActions::Execute(&pauseToggleAction, getGameState());
                    _waitingForPause = true;
                }
                return;
            }

            auto* w = WindowGetMain();
            if (w == nullptr)
                return;

            int32_t newSpeed = 0;
            switch (dropdownIndex)
            {
                case kFFDDIDX_NORMAL: newSpeed = 1; break;
                case kFFDDIDX_QUICK:  newSpeed = 2; break;
                case kFFDDIDX_FAST:   newSpeed = 3; break;
                case kFFDDIDX_TURBO:  newSpeed = 4; break;
                case kFFDDIDX_HYPER:  newSpeed = 8; break;
                default: return;
            }
            auto setSpeedAction = GameActions::GameSetSpeedAction(newSpeed);
            GameActions::Execute(&setSpeedAction, getGameState());
        }

        void initFileMenu(Widget& widget)
        {
            int32_t numItems = 0;
            if (isInTrackDesignerOrManager())
            {
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_GIANT_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_ABOUT);
                // OPENRCT2MINI: removed STR_FILE_BUG_ON_GITHUB entry.

                if (GetContext()->HasNewVersionInfo())
                    gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_UPDATE_AVAILABLE);

                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_OPTIONS);
                gDropdown.items[numItems++] = Dropdown::Separator();

                if (gLegacyScene == LegacyScene::trackDesigner)
                    gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_QUIT_ROLLERCOASTER_DESIGNER);
                else
                    gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_QUIT_TRACK_DESIGNS_MANAGER);

                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_EXIT_OPENRCT2);
            }
            else if (gLegacyScene == LegacyScene::scenarioEditor)
            {
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_LOAD_LANDSCAPE);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SAVE_LANDSCAPE);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_GIANT_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_ABOUT);
                // OPENRCT2MINI: removed STR_FILE_BUG_ON_GITHUB entry.

                if (GetContext()->HasNewVersionInfo())
                    gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_UPDATE_AVAILABLE);

                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_OPTIONS);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_QUIT_SCENARIO_EDITOR);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_EXIT_OPENRCT2);
            }
            else
            {
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_NEW_GAME);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_LOAD_GAME);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SAVE_GAME);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SAVE_GAME_AS);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_GIANT_SCREENSHOT);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_ABOUT);
                // OPENRCT2MINI: removed STR_FILE_BUG_ON_GITHUB entry.

                if (GetContext()->HasNewVersionInfo())
                    gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_UPDATE_AVAILABLE);

                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_OPTIONS);
                gDropdown.items[numItems++] = Dropdown::Separator();
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_QUIT_TO_MENU);
                gDropdown.items[numItems++] = Dropdown::PlainMenuLabel(STR_EXIT_OPENRCT2);
            }

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[0].withFlag(ColourFlag::translucent, true), Dropdown::Flag::StayOpen, numItems);
        }

        void initCheatsMenu(Widget& widget)
        {
            using namespace Dropdown;

            // OPENRCT2MINI: cut 30. The cheats dropdown is the combined dev-tools
            // menu now — cheats + (when debuggingTools is on) console + debug paint.
            // Lets us drop the WIDX_DEBUG button entirely so the toolbar fits 640.
            constexpr ItemExt baseItems[] = {
                ToggleOption(DDIDX_CHEATS, STR_CHEAT_TITLE),
                ToggleOption(DDIDX_TILE_INSPECTOR, STR_DEBUG_DROPDOWN_TILE_INSPECTOR),
                ToggleOption(DDIDX_OBJECT_SELECTION, STR_DEBUG_DROPDOWN_OBJECT_SELECTION),
                ToggleOption(DDIDX_INVENTIONS_LIST, STR_DEBUG_DROPDOWN_INVENTIONS_LIST),
                ToggleOption(DDIDX_SCENARIO_OPTIONS, STR_DEBUG_DROPDOWN_SCENARIO_OPTIONS),
                ExtSeparator(),
                ToggleOption(DDIDX_ENABLE_SANDBOX_MODE, STR_ENABLE_SANDBOX_MODE),
                ToggleOption(DDIDX_DISABLE_CLEARANCE_CHECKS, STR_DISABLE_CLEARANCE_CHECKS),
                ToggleOption(DDIDX_DISABLE_SUPPORT_LIMITS, STR_DISABLE_SUPPORT_LIMITS),
            };
            static_assert(ItemIDsMatchIndices(baseItems));

            SetItems(baseItems);

            const bool showDebugItems = Config::Get().general.debuggingTools;
            size_t numItems = std::size(baseItems);
            if (showDebugItems)
            {
                // Slot 9 is a separator, 10 = console, 11 = debug paint. Use the
                // Item-typed overloads (no ItemID), matching the legacy debug
                // dropdown's pattern at the bottom of this file.
                gDropdown.items[9] = Dropdown::Separator();
                gDropdown.items[DDIDX_DEBUG_CONSOLE] = Dropdown::ToggleOption(STR_DEBUG_DROPDOWN_CONSOLE);
                gDropdown.items[DDIDX_DEBUG_PAINT_TOOL] = Dropdown::ToggleOption(STR_DEBUG_DROPDOWN_DEBUG_PAINT);
                numItems = DDIDX_DEBUG_PAINT_TOOL + 1;
            }

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[0].withFlag(ColourFlag::translucent, true), Dropdown::Flag::StayOpen, numItems);

            // Disable items that are not yet available in multiplayer
            if (Network::GetMode() != Network::Mode::none)
            {
                gDropdown.items[DDIDX_OBJECT_SELECTION].setDisabled(true);
                gDropdown.items[DDIDX_INVENTIONS_LIST].setDisabled(true);
            }

            if (isInEditorMode())
            {
                gDropdown.items[DDIDX_OBJECT_SELECTION].setDisabled(true);
                gDropdown.items[DDIDX_INVENTIONS_LIST].setDisabled(true);
                gDropdown.items[DDIDX_SCENARIO_OPTIONS].setDisabled(true);
                gDropdown.items[DDIDX_ENABLE_SANDBOX_MODE].setDisabled(true);
            }

            auto& gameState = getGameState();
            if (gameState.cheats.sandboxMode)
            {
                gDropdown.items[DDIDX_ENABLE_SANDBOX_MODE].setChecked(true);
            }
            if (gameState.cheats.disableClearanceChecks)
            {
                gDropdown.items[DDIDX_DISABLE_CLEARANCE_CHECKS].setChecked(true);
            }
            if (gameState.cheats.disableSupportLimits)
            {
                gDropdown.items[DDIDX_DISABLE_SUPPORT_LIMITS].setChecked(true);
            }

            if (showDebugItems)
            {
                auto* windowMgr = GetWindowManager();
                gDropdown.items[DDIDX_DEBUG_CONSOLE].setChecked(false); // Console doesn't expose an open-state we can poll cheaply
                gDropdown.items[DDIDX_DEBUG_PAINT_TOOL].setChecked(windowMgr->FindByClass(WindowClass::debugPaint) != nullptr);
            }

            gDropdown.defaultIndex = DDIDX_CHEATS;
        }

        void cheatsMenuDropdown(int16_t dropdownIndex)
        {
            switch (dropdownIndex)
            {
                case DDIDX_CHEATS:
                    ContextOpenWindow(WindowClass::cheats);
                    break;
                case DDIDX_TILE_INSPECTOR:
                    ContextOpenWindow(WindowClass::tileInspector);
                    break;
                case DDIDX_OBJECT_SELECTION:
                {
                    auto* windowMgr = GetWindowManager();
                    windowMgr->CloseAll();
                    ContextOpenWindow(WindowClass::editorObjectSelection);
                    break;
                }
                case DDIDX_INVENTIONS_LIST:
                    ContextOpenWindow(WindowClass::editorInventionList);
                    break;
                case DDIDX_SCENARIO_OPTIONS:
                    ContextOpenWindow(WindowClass::editorScenarioOptions);
                    break;
                case DDIDX_ENABLE_SANDBOX_MODE:
                    CheatsSet(CheatType::sandboxMode, !getGameState().cheats.sandboxMode);
                    break;
                case DDIDX_DISABLE_CLEARANCE_CHECKS:
                    CheatsSet(CheatType::disableClearanceChecks, !getGameState().cheats.disableClearanceChecks);
                    break;
                case DDIDX_DISABLE_SUPPORT_LIMITS:
                    CheatsSet(CheatType::disableSupportLimits, !getGameState().cheats.disableSupportLimits);
                    break;
                // OPENRCT2MINI: cut 30. Debug menu items folded into this handler
                // so the standalone WIDX_DEBUG button can disappear.
                case DDIDX_DEBUG_CONSOLE:
                {
                    auto& console = GetInGameConsole();
                    console.Open();
                    break;
                }
                case DDIDX_DEBUG_PAINT_TOOL:
                {
                    auto* windowMgr = GetWindowManager();
                    if (windowMgr->FindByClass(WindowClass::debugPaint) == nullptr)
                        ContextOpenWindow(WindowClass::debugPaint);
                    else
                        windowMgr->CloseByClass(WindowClass::debugPaint);
                    break;
                }
            }
        }

        void initDebugMenu(Widget& widget)
        {
            gDropdown.items[DDIDX_CONSOLE] = Dropdown::ToggleOption(STR_DEBUG_DROPDOWN_CONSOLE);
            gDropdown.items[DDIDX_DEBUG_PAINT] = Dropdown::ToggleOption(STR_DEBUG_DROPDOWN_DEBUG_PAINT);

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[0].withFlag(ColourFlag::translucent, true), Dropdown::Flag::StayOpen, TOP_TOOLBAR_DEBUG_COUNT);

            auto* windowMgr = GetWindowManager();
            gDropdown.items[DDIDX_CONSOLE].setChecked(windowMgr->FindByClass(WindowClass::console) != nullptr);
            gDropdown.items[DDIDX_DEBUG_PAINT].setChecked(windowMgr->FindByClass(WindowClass::debugPaint) != nullptr);
        }

        void debugMenuDropdown(int16_t dropdownIndex)
        {
            auto* w = WindowGetMain();
            if (w != nullptr)
            {
                switch (dropdownIndex)
                {
                    case DDIDX_CONSOLE:
                    {
                        auto& console = GetInGameConsole();
                        console.Open();
                        break;
                    }
                    case DDIDX_DEBUG_PAINT:
                    {
                        auto* windowMgr = GetWindowManager();
                        if (windowMgr->FindByClass(WindowClass::debugPaint) == nullptr)
                        {
                            ContextOpenWindow(WindowClass::debugPaint);
                        }
                        else
                        {
                            windowMgr->CloseByClass(WindowClass::debugPaint);
                        }
                        break;
                    }
                }
            }
        }

        void initNetworkMenu(Widget& widget)
        {
            gDropdown.items[DDIDX_MULTIPLAYER] = Dropdown::PlainMenuLabel(STR_MULTIPLAYER);
            gDropdown.items[DDIDX_MULTIPLAYER_RECONNECT] = Dropdown::PlainMenuLabel(STR_MULTIPLAYER_RECONNECT);

            WindowDropdownShowText(
                { windowPos.x + widget.left, windowPos.y + widget.top }, widget.height(),
                colours[0].withFlag(ColourFlag::translucent, true), 0, TOP_TOOLBAR_NETWORK_COUNT);

            gDropdown.items[DDIDX_MULTIPLAYER_RECONNECT].setDisabled(!Network::IsDesynchronised());

            gDropdown.defaultIndex = DDIDX_MULTIPLAYER;
        }

        void networkMenuDropdown(int16_t dropdownIndex)
        {
            auto* w = WindowGetMain();
            if (w != nullptr)
            {
                switch (dropdownIndex)
                {
                    case DDIDX_MULTIPLAYER:
                        ContextOpenWindow(WindowClass::multiplayer);
                        break;
                    case DDIDX_MULTIPLAYER_RECONNECT:
                        Network::Reconnect();
                        break;
                }
            }
        }

    public:
        void onMouseUp(WidgetIndex widgetIndex) override
        {
            WindowBase* mainWindow;

            switch (widgetIndex)
            {
                case WIDX_PAUSE:
                    if (Network::GetMode() != Network::Mode::client)
                    {
                        auto pauseToggleAction = GameActions::PauseToggleAction();
                        GameActions::Execute(&pauseToggleAction, getGameState());
                        _waitingForPause = true;
                    }
                    break;
                case WIDX_ZOOM_OUT:
                    if ((mainWindow = WindowGetMain()) != nullptr)
                        WindowZoomOut(*mainWindow, false);
                    break;
                case WIDX_ZOOM_IN:
                    if ((mainWindow = WindowGetMain()) != nullptr)
                        WindowZoomIn(*mainWindow, false);
                    break;
                case WIDX_ROTATE_CLOCKWISE:
                    ViewportRotateAll(1);
                    break;
                case WIDX_ROTATE_ANTI_CLOCKWISE:
                    ViewportRotateAll(-1);
                    break;
                case WIDX_CLEAR_SCENERY:
                    ToggleClearSceneryWindow();
                    break;
                case WIDX_LAND:
                    ToggleLandWindow();
                    break;
                case WIDX_WATER:
                    ToggleWaterWindow();
                    break;
                case WIDX_SCENERY:
                    ToggleSceneryWindow();
                    break;
                case WIDX_PATH:
                    ToggleFootpathWindow();
                    break;
                case WIDX_CONSTRUCT_RIDE:
                    ContextOpenWindow(WindowClass::constructRide);
                    break;
                case WIDX_RIDES:
                    ContextOpenWindow(WindowClass::rideList);
                    break;
                case WIDX_PARK:
                    ContextOpenWindow(WindowClass::parkInformation);
                    break;
                case WIDX_STAFF:
                    ContextOpenWindow(WindowClass::staffList);
                    break;
                case WIDX_GUESTS:
                    ContextOpenWindow(WindowClass::guestList);
                    break;
                case WIDX_FINANCES:
                    ContextOpenWindow(WindowClass::finances);
                    break;
                case WIDX_RESEARCH:
                    ContextOpenWindow(WindowClass::research);
                    break;
                case WIDX_NEWS:
                    ContextOpenWindow(WindowClass::recentNews);
                    break;
                case WIDX_MUTE:
                    Audio::ToggleAllSounds();
                    break;
                case WIDX_CHAT:
                    if (ChatAvailable())
                    {
                        ChatToggle();
                    }
                    else
                    {
                        ContextShowError(STR_CHAT_UNAVAILABLE, kStringIdNone, {});
                    }
                    break;
            }
        }

        void onMouseDown(WidgetIndex widgetIndex) override
        {
            Widget& widget = widgets[widgetIndex];

            switch (widgetIndex)
            {
                case WIDX_FILE_MENU:
                    initFileMenu(widget);
                    break;
                case WIDX_CHEATS:
                    initCheatsMenu(widget);
                    break;
                case WIDX_VIEW_MENU:
                    initViewMenu(widget);
                    break;
                case WIDX_MAP:
                    initMapMenu(widget);
                    break;
                case WIDX_FASTFORWARD:
                    initFastforwardMenu(widget);
                    break;
                case WIDX_DEBUG:
                    initDebugMenu(widget);
                    break;
                case WIDX_NETWORK:
                    initNetworkMenu(widget);
                    break;
            }
        }

        void onDropdown(WidgetIndex widgetIndex, int32_t selectedIndex) override
        {
            if (selectedIndex == -1)
            {
                return;
            }

            auto& gameState = getGameState();
            switch (widgetIndex)
            {
                case WIDX_FILE_MENU:

                    // New game is only available in the normal game. Skip one position to avoid incorrect mappings in the menus
                    // of the other modes.
                    if (gLegacyScene == LegacyScene::scenarioEditor)
                        selectedIndex += 1;

                    // Quicksave is only available in the normal game. Skip one position to avoid incorrect mappings in the
                    // menus of the other modes.
                    if (gLegacyScene == LegacyScene::scenarioEditor && selectedIndex > DDIDX_LOAD_GAME)
                        selectedIndex += 1;

                    // Track designer and track designs manager start with Screenshot, not Load/save
                    if (isInTrackDesignerOrManager())
                        selectedIndex += DDIDX_SCREENSHOT;

                    // The "Update available" menu item is only available when there is one
                    if (selectedIndex >= DDIDX_UPDATE_AVAILABLE && !GetContext()->HasNewVersionInfo())
                        selectedIndex += 1;

                    switch (selectedIndex)
                    {
                        case DDIDX_NEW_GAME:
                        {
                            auto loadOrQuitAction = GameActions::LoadOrQuitAction(
                                GameActions::LoadOrQuitModes::OpenSavePrompt, PromptMode::saveBeforeNewGame);
                            GameActions::Execute(&loadOrQuitAction, gameState);
                            break;
                        }
                        case DDIDX_LOAD_GAME:
                        {
                            auto loadOrQuitAction = GameActions::LoadOrQuitAction(GameActions::LoadOrQuitModes::OpenSavePrompt);
                            GameActions::Execute(&loadOrQuitAction, gameState);
                            break;
                        }
                        case DDIDX_SAVE_GAME:
                            ToolCancel();
                            SaveGame();
                            break;
                        case DDIDX_SAVE_GAME_AS:
                            if (gLegacyScene == LegacyScene::scenarioEditor)
                            {
                                auto intent = Intent(WindowClass::loadsave);
                                intent.PutEnumExtra<LoadSaveAction>(INTENT_EXTRA_LOADSAVE_ACTION, LoadSaveAction::save);
                                intent.PutEnumExtra<LoadSaveType>(INTENT_EXTRA_LOADSAVE_TYPE, LoadSaveType::landscape);
                                intent.PutExtra(INTENT_EXTRA_PATH, getGameState().scenarioOptions.name);
                                ContextOpenIntent(&intent);
                            }
                            else
                            {
                                ToolCancel();
                                SaveGameAs();
                            }
                            break;
                        case DDIDX_ABOUT:
                            ContextOpenWindow(WindowClass::about);
                            break;
                        case DDIDX_OPTIONS:
                            ContextOpenWindow(WindowClass::options);
                            break;
                        case DDIDX_SCREENSHOT:
                            gScreenshotCountdown = 10;
                            break;
                        case DDIDX_GIANT_SCREENSHOT:
                            ScreenshotGiant();
                            break;
                        // OPENRCT2MINI: case DDIDX_FILE_BUG_ON_GITHUB removed.
                        case DDIDX_UPDATE_AVAILABLE:
                            ContextOpenWindowView(WindowView::newVersionInfo);
                            break;
                        case DDIDX_QUIT_TO_MENU:
                        {
                            auto* windowMgr = GetWindowManager();
                            windowMgr->CloseByClass(WindowClass::manageTrackDesign);
                            windowMgr->CloseByClass(WindowClass::trackDeletePrompt);
                            auto loadOrQuitAction = GameActions::LoadOrQuitAction(
                                GameActions::LoadOrQuitModes::OpenSavePrompt, PromptMode::saveBeforeQuit);
                            GameActions::Execute(&loadOrQuitAction, gameState);
                            break;
                        }
                        case DDIDX_EXIT_OPENRCT2:
                            ContextQuit();
                            break;
                    }
                    break;
                case WIDX_CHEATS:
                    cheatsMenuDropdown(selectedIndex);
                    break;
                case WIDX_VIEW_MENU:
                    viewMenuDropdown(selectedIndex);
                    break;
                case WIDX_MAP:
                    mapMenuDropdown(selectedIndex);
                    break;
                case WIDX_FASTFORWARD:
                    fastforwardMenuDropdown(selectedIndex);
                    break;
                case WIDX_DEBUG:
                    debugMenuDropdown(selectedIndex);
                    break;
                case WIDX_NETWORK:
                    networkMenuDropdown(selectedIndex);
                    break;
            }
        }

#ifdef ENABLE_SCRIPTING
        // The following are tool events for custom tools set by user scripts.
        // NB: these can't go into CustomWindow.cpp, as tools may be active without a visible window.
        void onToolUpdate(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto& customTool = Scripting::ActiveCustomTool;
            if (customTool)
            {
                customTool->OnUpdate(screenCoords);
            }
        }

        void onToolDown(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto& customTool = Scripting::ActiveCustomTool;
            if (customTool)
            {
                customTool->OnDown(screenCoords);
            }
        }

        void onToolDrag(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto& customTool = Scripting::ActiveCustomTool;
            if (customTool)
            {
                customTool->OnDrag(screenCoords);
            }
        }

        void onToolUp(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            auto& customTool = Scripting::ActiveCustomTool;
            if (customTool)
            {
                customTool->OnUp(screenCoords);
            }
        }

        void onToolAbort(WidgetIndex widgetIndex) override
        {
            auto& customTool = Scripting::ActiveCustomTool;
            if (customTool)
            {
                customTool->OnAbort();
                customTool = {};
            }
        }
#endif

        void ResetWidgetToDefaultState()
        {
            // Enable / disable buttons
            // OPENRCT2MINI: cut 31. Pause is now an entry in the FASTFORWARD
            // speed dropdown — the standalone button is hidden permanently.
            // Pause state shows up as an overlay on the FASTFORWARD icon.
            widgets[WIDX_PAUSE].type = WidgetType::empty;
            widgets[WIDX_FILE_MENU].type = WidgetType::trnBtn;
            widgets[WIDX_ZOOM_OUT].type = WidgetType::trnBtn;
            widgets[WIDX_ZOOM_IN].type = WidgetType::trnBtn;
            widgets[WIDX_ROTATE_CLOCKWISE].type = WidgetType::trnBtn;
            widgets[WIDX_ROTATE_ANTI_CLOCKWISE].type = WidgetType::trnBtn;
            widgets[WIDX_VIEW_MENU].type = WidgetType::trnBtn;
            widgets[WIDX_MAP].type = WidgetType::trnBtn;
            widgets[WIDX_MUTE].type = WidgetType::trnBtn;
            widgets[WIDX_CHAT].type = WidgetType::trnBtn;
            widgets[WIDX_LAND].type = WidgetType::trnBtn;
            widgets[WIDX_WATER].type = WidgetType::trnBtn;
            widgets[WIDX_SCENERY].type = WidgetType::trnBtn;
            widgets[WIDX_PATH].type = WidgetType::trnBtn;
            widgets[WIDX_CONSTRUCT_RIDE].type = WidgetType::trnBtn;
            widgets[WIDX_RIDES].type = WidgetType::trnBtn;
            widgets[WIDX_PARK].type = WidgetType::trnBtn;
            widgets[WIDX_STAFF].type = WidgetType::trnBtn;
            widgets[WIDX_GUESTS].type = WidgetType::trnBtn;
            widgets[WIDX_CLEAR_SCENERY].type = WidgetType::trnBtn;
            widgets[WIDX_FINANCES].type = WidgetType::trnBtn;
            widgets[WIDX_RESEARCH].type = WidgetType::trnBtn;
            widgets[WIDX_FASTFORWARD].type = WidgetType::trnBtn;
            widgets[WIDX_CHEATS].type = WidgetType::trnBtn;
            // OPENRCT2MINI: cut 30. WIDX_DEBUG is permanently empty — its menu
            // items live in the cheats dropdown now (see initCheatsMenu).
            widgets[WIDX_DEBUG].type = WidgetType::empty;
            widgets[WIDX_NEWS].type = WidgetType::trnBtn;
            widgets[WIDX_NETWORK].type = WidgetType::trnBtn;
        }

        void HideDisabledButtons()
        {
            if (!Config::Get().interface.toolbarShowMute)
                widgets[WIDX_MUTE].type = WidgetType::empty;

            if (!Config::Get().interface.toolbarShowChat)
                widgets[WIDX_CHAT].type = WidgetType::empty;

            if (!Config::Get().interface.toolbarShowResearch)
                widgets[WIDX_RESEARCH].type = WidgetType::empty;

            if (!Config::Get().interface.toolbarShowCheats)
                widgets[WIDX_CHEATS].type = WidgetType::empty;

            if (!Config::Get().interface.toolbarShowNews)
                widgets[WIDX_NEWS].type = WidgetType::empty;

            if (!Config::Get().interface.toolbarShowZoom)
            {
                widgets[WIDX_ZOOM_IN].type = WidgetType::empty;
                widgets[WIDX_ZOOM_OUT].type = WidgetType::empty;
            }

            if (!Config::Get().interface.toolbarShowRotateAnticlockwise)
                widgets[WIDX_ROTATE_ANTI_CLOCKWISE].type = WidgetType::empty;

            if (gLegacyScene == LegacyScene::scenarioEditor || gLegacyScene == LegacyScene::trackDesignsManager)
            {
                widgets[WIDX_PAUSE].type = WidgetType::empty;
            }

            if ((getGameState().park.flags & PARK_FLAGS_NO_MONEY) || !Config::Get().interface.toolbarShowFinances)
                widgets[WIDX_FINANCES].type = WidgetType::empty;
        }

        void ApplyEditorMode()
        {
            if (isInEditorMode() == 0)
            {
                return;
            }

            widgets[WIDX_PARK].type = WidgetType::empty;
            widgets[WIDX_STAFF].type = WidgetType::empty;
            widgets[WIDX_GUESTS].type = WidgetType::empty;
            widgets[WIDX_FINANCES].type = WidgetType::empty;
            widgets[WIDX_RESEARCH].type = WidgetType::empty;
            widgets[WIDX_NEWS].type = WidgetType::empty;
            widgets[WIDX_NETWORK].type = WidgetType::empty;

            auto& gameState = getGameState();
            if (gameState.editorStep != EditorStep::LandscapeEditor)
            {
                widgets[WIDX_LAND].type = WidgetType::empty;
                widgets[WIDX_WATER].type = WidgetType::empty;
            }

            if (gameState.editorStep != EditorStep::RollercoasterDesigner)
            {
                widgets[WIDX_RIDES].type = WidgetType::empty;
                widgets[WIDX_CONSTRUCT_RIDE].type = WidgetType::empty;
                widgets[WIDX_FASTFORWARD].type = WidgetType::empty;
            }

            if (gameState.editorStep != EditorStep::LandscapeEditor
                && gameState.editorStep != EditorStep::RollercoasterDesigner)
            {
                widgets[WIDX_MAP].type = WidgetType::empty;
                widgets[WIDX_SCENERY].type = WidgetType::empty;
                widgets[WIDX_PATH].type = WidgetType::empty;
                widgets[WIDX_CLEAR_SCENERY].type = WidgetType::empty;

                widgets[WIDX_ZOOM_OUT].type = WidgetType::empty;
                widgets[WIDX_ZOOM_IN].type = WidgetType::empty;
                widgets[WIDX_ROTATE_ANTI_CLOCKWISE].type = WidgetType::empty;
                widgets[WIDX_ROTATE_CLOCKWISE].type = WidgetType::empty;
                widgets[WIDX_VIEW_MENU].type = WidgetType::empty;
            }
        }

        void ApplyNetworkMode()
        {
            switch (Network::GetMode())
            {
                case Network::Mode::none:
                    widgets[WIDX_NETWORK].type = WidgetType::empty;
                    widgets[WIDX_CHAT].type = WidgetType::empty;
                    break;
                case Network::Mode::client:
                    widgets[WIDX_PAUSE].type = WidgetType::empty;
                    [[fallthrough]];
                case Network::Mode::server:
                    widgets[WIDX_FASTFORWARD].type = WidgetType::empty;
                    break;
            }
        }

        void ApplyZoomState()
        {
            // Zoomed out/in disable. Not sure where this code is in the original.
            const auto* mainWindow = WindowGetMain();
            if (mainWindow == nullptr || mainWindow->viewport == nullptr)
            {
                LOG_ERROR("mainWindow or mainWindow->viewport is null!");
                return;
            }

            setWidgetDisabled(WIDX_ZOOM_IN, mainWindow->viewport->zoom == ZoomLevel::min());
            setWidgetDisabled(WIDX_ZOOM_OUT, mainWindow->viewport->zoom >= ZoomLevel::max());
        }

        void ApplyPausedState()
        {
            bool paused = (gGamePaused & GAME_PAUSED_NORMAL);
            if (paused)
                _waitingForPause = false;
            setWidgetPressed(WIDX_PAUSE, paused || _waitingForPause);
        }

        void ApplyMapRotation()
        {
            // Set map button to the right image.
            if (widgets[WIDX_MAP].type != WidgetType::empty)
            {
                static constexpr uint32_t _imageIdByRotation[] = {
                    SPR_G2_MAP_NORTH,
                    SPR_G2_MAP_WEST,
                    SPR_G2_MAP_SOUTH,
                    SPR_G2_MAP_EAST,
                };

                uint32_t mapImageId = _imageIdByRotation[GetCurrentRotation()];
                widgets[WIDX_MAP].image = ImageId(mapImageId, Drawing::FilterPaletteID::paletteNull);
            }
        }

        void ApplyAudioState()
        {
            if (!Audio::gGameSoundsOff)
                widgets[WIDX_MUTE].image = ImageId(SPR_G2_TOOLBAR_MUTE, Drawing::FilterPaletteID::paletteNull);
            else
                widgets[WIDX_MUTE].image = ImageId(SPR_G2_TOOLBAR_UNMUTE, Drawing::FilterPaletteID::paletteNull);
        }

        void ApplyFootpathPressed()
        {
            // Footpath button pressed down
            auto* windowMgr = GetWindowManager();
            setWidgetPressed(WIDX_PATH, windowMgr->FindByClass(WindowClass::footpath) != nullptr);
        }

        // TODO: look into using std::span
        // OPENRCT2MINI: cut 32 — must mirror AlignButtons' new SEPARATOR-skipping
        // behaviour, otherwise the right group's flush-right alignment lands a
        // separator-sized gap to the right of the actual content.
        template<typename T>
        uint16_t GetToolbarWidth(T toolbarItems)
        {
            auto totalWidth = 0;
            for (auto widgetIndex : toolbarItems)
            {
                auto* widget = &widgets[widgetIndex];
                if (widgetIndex == WIDX_SEPARATOR || widget->type == WidgetType::empty)
                    continue;

                totalWidth += widget->width();
            }
            return totalWidth;
        }

        // OPENRCT2MINI: cut 32 — drop SEPARATOR widgets entirely from layout.
        // Upstream's separators added 10 px of dead space between e.g. cheats
        // and zoom, construct-ride and finances, etc. On a 640 px screen those
        // gaps look like padding that doesn't belong; the icons already have
        // visual breathing room from their own transparent margins. Skipping
        // SEPARATOR keeps the click areas flush.
        template<typename T>
        void AlignButtons(T toolbarItems, uint16_t xPos, uint16_t yOffset = 0)
        {
            for (auto widgetIndex : toolbarItems)
            {
                auto* widget = &widgets[widgetIndex];
                if (widgetIndex == WIDX_SEPARATOR || widget->type == WidgetType::empty)
                    continue;

                auto widgetWidth = widget->width() - 1;
                widget->left = xPos;
                xPos += widgetWidth;
                widget->right = xPos;
                xPos += 1;

                if (yOffset != 0)
                {
                    auto widgetHeight = widget->height();
                    widget->top = yOffset;
                    widget->bottom = yOffset + widgetHeight;
                }
            }
        }

        // OPENRCT2MINI: cut 29. Reset every widget back to row 0 before the
        // per-frame layout pass; otherwise switching from two-row to one-row
        // (e.g. the user resizes the dev-host window wider) would leave widgets
        // stuck on row 1.
        void ResetWidgetRowsToDefault()
        {
            const uint16_t row0Height = kTopToolbarHeight + 1;
            for (auto& widget : widgets)
            {
                if (widget.bottom > row0Height)
                {
                    widget.top = 0;
                    widget.bottom = row0Height;
                }
            }
        }

        // OPENRCT2MINI: cut 30/32. Flow-pack: [left + right] in order, wrap when
        // next button would overflow. SEPARATORs are skipped entirely (cut 32)
        // so adjacent buttons are flush. Returns true if any widget landed
        // below row 0.
        template<typename T>
        bool AlignButtonsFlow(T toolbarItems, uint16_t startX, uint16_t screenWidth, uint16_t rowHeight)
        {
            uint16_t xPos = startX;
            uint16_t yOffset = 0;
            bool wrapped = false;

            for (auto widgetIndex : toolbarItems)
            {
                auto* widget = &widgets[widgetIndex];
                if (widgetIndex == WIDX_SEPARATOR || widget->type == WidgetType::empty)
                    continue;

                const auto widgetWidth = widget->width() - 1;

                if (xPos + widgetWidth > screenWidth)
                {
                    xPos = 0;
                    yOffset = rowHeight;
                    wrapped = true;
                }

                widget->left = xPos;
                xPos += widgetWidth;
                widget->right = xPos;
                xPos += 1;

                const auto widgetHeight = widget->height();
                widget->top = yOffset;
                widget->bottom = yOffset + widgetHeight;
            }
            return wrapped;
        }

        void ApplyToolbarHeight(bool wrapped)
        {
            const uint16_t rowHeight = kTopToolbarHeight + 1;
            const uint16_t targetHeight = wrapped ? rowHeight * 2 : rowHeight;
            if (height != targetHeight)
            {
                height = targetHeight;
                invalidate();
            }
        }

        void AlignButtonsLeftRight()
        {
            // OPENRCT2MINI: cut 32. Always flow-pack — no separator gaps, no
            // inter-group gap. The toolbar packs flush from x=0 left-to-right
            // and wraps to a second row only if the screen is too narrow.
            // This replaces the cut-29/30 split that kept the upstream
            // "right group flush-right with middle gap" layout for wide
            // screens; the user wants flush-packing on every screen size.
            ResetWidgetRowsToDefault();

            const auto screenWidth = ContextGetWidth();
            const uint16_t rowHeight = kTopToolbarHeight + 1;

            const bool wrapped = AlignButtonsFlow(kWidgetOrderCombined, 0, screenWidth, rowHeight);
            ApplyToolbarHeight(wrapped);
        }

        void AlignButtonsCentre()
        {
            ResetWidgetRowsToDefault();

            const auto totalWidth = GetToolbarWidth(kWidgetOrderCombined);
            const auto screenWidth = ContextGetWidth();
            const uint16_t rowHeight = kTopToolbarHeight + 1;

            if (static_cast<int32_t>(totalWidth) > screenWidth)
            {
                // Won't fit centred — wrap via flow-pack from x=0.
                const bool wrapped = AlignButtonsFlow(kWidgetOrderCombined, 0, screenWidth, rowHeight);
                ApplyToolbarHeight(wrapped);
                return;
            }

            const auto xPos = (screenWidth - totalWidth) / 2;
            AlignButtons(kWidgetOrderCombined, xPos);
            ApplyToolbarHeight(false);
        }

        void onPrepareDraw() override
        {
            ResetWidgetToDefaultState();
            HideDisabledButtons();
            ApplyEditorMode();

            ApplyPausedState();
            ApplyAudioState();
            ApplyNetworkMode();

            ApplyZoomState();
            ApplyMapRotation();
            ApplyFootpathPressed();

            if (!Config::Get().interface.toolbarButtonsCentred)
                AlignButtonsLeftRight();
            else
                AlignButtonsCentre();
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            const auto& gameState = getGameState();
            int32_t imgId;

            WindowDrawWidgets(*this, rt);

            ScreenCoordsXY screenPos{};
            // Draw staff button image (setting masks to the staff colours)
            if (widgets[WIDX_STAFF].type != WidgetType::empty)
            {
                screenPos = { windowPos.x + widgets[WIDX_STAFF].left, windowPos.y + widgets[WIDX_STAFF].top };
                imgId = SPR_TOOLBAR_STAFF;
                if (widgetIsPressed(*this, WIDX_STAFF))
                    imgId++;
                GfxDrawSprite(
                    rt, ImageId(imgId, gameState.park.staffHandymanColour, gameState.park.staffMechanicColour), screenPos);
            }

            // Draw fast forward button
            if (widgets[WIDX_FASTFORWARD].type != WidgetType::empty)
            {
                screenPos = { windowPos.x + widgets[WIDX_FASTFORWARD].left + 0,
                              windowPos.y + widgets[WIDX_FASTFORWARD].top + 0 };
                if (widgetIsPressed(*this, WIDX_FASTFORWARD))
                    screenPos.y++;

                // OPENRCT2MINI: cut 31 / polish. The standalone WIDX_PAUSE
                // button is gone (pause is folded into the game-speed
                // dropdown on this button). When paused, draw the
                // upstream pause sprite SPR_TOOLBAR_PAUSE with the
                // toolbar's primary colour remap — exact replica of
                // how upstream's trnBtn renderer drew the original
                // pause widget (see WidgetDrawImage in Widget.cpp:899
                // applying image.WithPrimary(colour)). When NOT paused,
                // show the FASTFORWARD icon plus the speed arrows.
                const bool paused = (gGamePaused & GAME_PAUSED_NORMAL) != 0;
                if (paused)
                {
                    GfxDrawSprite(rt, ImageId(SPR_TOOLBAR_PAUSE).WithPrimary(colours[0].colour), screenPos);
                }
                else
                {
                    GfxDrawSprite(rt, ImageId(SPR_G2_FASTFORWARD), screenPos + ScreenCoordsXY{ 6, 3 });
                    for (int32_t i = 0; i < gGameSpeed && gGameSpeed <= 4; i++)
                    {
                        GfxDrawSprite(rt, ImageId(SPR_G2_SPEED_ARROW), screenPos + ScreenCoordsXY{ 5 + i * 5, 15 });
                    }
                    for (int32_t i = 0; i < 3 && gGameSpeed >= 5; i++)
                    {
                        GfxDrawSprite(rt, ImageId(SPR_G2_HYPER_ARROW), screenPos + ScreenCoordsXY{ 5 + i * 6, 15 });
                    }
                }
            }

            // Draw cheats button
            if (widgets[WIDX_CHEATS].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_CHEATS].left - 1, widgets[WIDX_CHEATS].top - 1 };
                if (widgetIsPressed(*this, WIDX_CHEATS))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_G2_SANDBOX), screenPos);

                // Draw an overlay if clearance checks are disabled
                if (getGameState().cheats.disableClearanceChecks)
                {
                    auto colour = ColourWithFlags{ Drawing::Colour::darkOrange }.withFlag(ColourFlag::withOutline, true);
                    drawText(
                        rt, screenPos + ScreenCoordsXY{ 26, 2 }, STR_OVERLAY_CLEARANCE_CHECKS_DISABLED,
                        { colour, TextAlignment::right });
                }
            }

            // Draw chat button
            if (widgets[WIDX_CHAT].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_CHAT].left, widgets[WIDX_CHAT].top - 2 };
                if (widgetIsPressed(*this, WIDX_CHAT))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_G2_CHAT), screenPos);
            }

            // Draw debug button
            if (widgets[WIDX_DEBUG].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_DEBUG].left, widgets[WIDX_DEBUG].top - 1 };
                if (widgetIsPressed(*this, WIDX_DEBUG))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_TAB_GEARS_0), screenPos);
            }

            // Draw research button
            if (widgets[WIDX_RESEARCH].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_RESEARCH].left - 1, widgets[WIDX_RESEARCH].top };
                if (widgetIsPressed(*this, WIDX_RESEARCH))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_TAB_FINANCES_RESEARCH_0), screenPos);
            }

            // Draw finances button
            if (widgets[WIDX_FINANCES].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_FINANCES].left + 3, widgets[WIDX_FINANCES].top + 1 };
                if (widgetIsPressed(*this, WIDX_FINANCES))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_FINANCE), screenPos);
            }

            // Draw news button
            if (widgets[WIDX_NEWS].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_NEWS].left + 3, widgets[WIDX_NEWS].top + 0 };
                if (widgetIsPressed(*this, WIDX_NEWS))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_G2_TAB_NEWS), screenPos);
            }

            // Draw network button
            if (widgets[WIDX_NETWORK].type != WidgetType::empty)
            {
                screenPos = windowPos + ScreenCoordsXY{ widgets[WIDX_NETWORK].left + 3, widgets[WIDX_NETWORK].top + 0 };
                if (widgetIsPressed(*this, WIDX_NETWORK))
                    screenPos.y++;

                // Draw (de)sync icon.
                imgId = (Network::IsDesynchronised() ? SPR_G2_MULTIPLAYER_DESYNC : SPR_G2_MULTIPLAYER_SYNC);
                GfxDrawSprite(rt, ImageId(imgId), screenPos + ScreenCoordsXY{ 3, 11 });

                // Draw number of players.
                auto ft = Formatter();
                ft.Add<int32_t>(Network::GetNumVisiblePlayers());
                auto colour = ColourWithFlags{ Drawing::Colour::white }.withFlag(ColourFlag::withOutline, true);
                drawText(rt, screenPos + ScreenCoordsXY{ 23, 1 }, STR_COMMA16, ft, { colour, TextAlignment::right });
            }

            if (widgets[WIDX_ROTATE_ANTI_CLOCKWISE].type != WidgetType::empty)
            {
                screenPos = windowPos
                    + ScreenCoordsXY{ widgets[WIDX_ROTATE_ANTI_CLOCKWISE].left + 2,
                                      widgets[WIDX_ROTATE_ANTI_CLOCKWISE].top + 0 };
                if (isWidgetPressed(WIDX_ROTATE_ANTI_CLOCKWISE))
                    screenPos.y++;
                GfxDrawSprite(rt, ImageId(SPR_G2_ICON_ROTATE_ANTI_CLOCKWISE), screenPos);
            }
        }
    };

    /**
     * Creates the main game top toolbar window.
     *  rct2: 0x0066B485 (part of 0x0066B3E8)
     */
    WindowBase* TopToolbarOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->Create<TopToolbar>(
            WindowClass::topToolbar, ScreenCoordsXY(0, 0), { ContextGetWidth(), kTopToolbarHeight + 1 },
            { WindowFlag::stickToFront, WindowFlag::transparent, WindowFlag::noBackground, WindowFlag::noTitleBar });

        window->setWidgets(_topToolbarWidgets);

        WindowInitScrollWidgets(*window);

        return window;
    }
} // namespace OpenRCT2::Ui::Windows
