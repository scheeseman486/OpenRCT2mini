/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "../UiStringIds.h"
#include "ShortcutIds.h"
#include "ShortcutManager.h"

#include <functional>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/interface/InGameConsole.h>
#include <openrct2-ui/interface/Viewport.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Editor.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/cheats/CheatSetAction.h>
#include <openrct2/actions/general/LoadOrQuitAction.h>
#include <openrct2/actions/general/PauseToggleAction.h>
#include <openrct2/actions/general/TileModifyAction.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/EnumUtils.hpp>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/interface/Chat.h>
#include <openrct2/interface/Screenshot.h>
#include <openrct2/network/Network.h>
#include <openrct2/object/WallSceneryEntry.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/ride/Track.h>
#include <openrct2/ride/TrackPaint.h>
#include <openrct2/scenes/title/TitleScene.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <openrct2/windows/TileInspectorGlobals.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/Scenery.h>
#include <openrct2/world/TileInspector.h>
#include <openrct2/world/tile_element/WallElement.h>

using namespace OpenRCT2;
using namespace OpenRCT2::Ui;
using namespace OpenRCT2::Ui::Windows;

#pragma region Shortcut Commands

static void OpenWindow(WindowClass wc)
{
    if (gLegacyScene != LegacyScene::titleSequence)
    {
        ContextOpenWindow(wc);
    }
}

static void RotateCamera(int32_t direction)
{
    if (gLegacyScene != LegacyScene::titleSequence)
    {
        ViewportRotateAll(direction);
    }
}

static void ToggleViewFlag(int32_t viewportFlag)
{
    if (gLegacyScene != LegacyScene::titleSequence)
    {
        auto window = WindowGetMain();
        if (window != nullptr)
        {
            window->viewport->flags ^= viewportFlag;
            window->invalidate();
        }
    }
}

// OPENRCT2MINI cut 59: drop `static` so UiContext can call this directly to
// implement the gamepad Y / R1+Y "rotate construction object" binding.
// Forward-declared as a free function `ShortcutRotateConstructionObject()`
// at global scope inside UiContext.cpp. Self-decl here keeps
// -Wmissing-declarations happy without a new public header.
void ShortcutRotateConstructionObject();
void ShortcutRotateConstructionObject()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();

    // Rotate scenery
    WindowBase* w = windowMgr->FindByClass(WindowClass::scenery);
    if (w != nullptr && !widgetIsDisabled(*w, WC_SCENERY__WIDX_SCENERY_ROTATE_OBJECTS_BUTTON)
        && w->widgets[WC_SCENERY__WIDX_SCENERY_ROTATE_OBJECTS_BUTTON].type != WidgetType::empty)
    {
        w->onMouseUp(WC_SCENERY__WIDX_SCENERY_ROTATE_OBJECTS_BUTTON);
        return;
    }

    // Rotate construction track piece
    w = windowMgr->FindByClass(WindowClass::rideConstruction);
    if (w != nullptr && !widgetIsDisabled(*w, WC_RIDE_CONSTRUCTION__WIDX_ROTATE)
        && w->widgets[WC_RIDE_CONSTRUCTION__WIDX_ROTATE].type != WidgetType::empty)
    {
        // Check if building a maze...
        if (w->widgets[WC_RIDE_CONSTRUCTION__WIDX_ROTATE].tooltip != STR_RIDE_CONSTRUCTION_BUILD_MAZE_IN_THIS_DIRECTION_TIP)
        {
            w->onMouseUp(WC_RIDE_CONSTRUCTION__WIDX_ROTATE);
            return;
        }
    }

    // Rotate track design preview
    w = windowMgr->FindByClass(WindowClass::trackDesignList);
    if (w != nullptr && !widgetIsDisabled(*w, WC_TRACK_DESIGN_LIST__WIDX_ROTATE)
        && w->widgets[WC_TRACK_DESIGN_LIST__WIDX_ROTATE].type != WidgetType::empty)
    {
        w->onMouseUp(WC_TRACK_DESIGN_LIST__WIDX_ROTATE);
        return;
    }

    // Rotate track design placement
    w = windowMgr->FindByClass(WindowClass::trackDesignPlace);
    if (w != nullptr && !widgetIsDisabled(*w, WC_TRACK_DESIGN_PLACE__WIDX_ROTATE)
        && w->widgets[WC_TRACK_DESIGN_PLACE__WIDX_ROTATE].type != WidgetType::empty)
    {
        w->onMouseUp(WC_TRACK_DESIGN_PLACE__WIDX_ROTATE);
        return;
    }

    // Rotate park entrance
    w = windowMgr->FindByClass(WindowClass::editorParkEntrance);
    if (w != nullptr && !widgetIsDisabled(*w, WC_EDITOR_PARK_ENTRANCE__WIDX_ROTATE_ENTRANCE_BUTTON)
        && w->widgets[WC_EDITOR_PARK_ENTRANCE__WIDX_ROTATE_ENTRANCE_BUTTON].type != WidgetType::empty)
    {
        w->onMouseUp(WC_EDITOR_PARK_ENTRANCE__WIDX_ROTATE_ENTRANCE_BUTTON);
        return;
    }

    // Rotate selected element in tile inspector
    w = windowMgr->FindByClass(WindowClass::tileInspector);
    if (w != nullptr && !widgetIsDisabled(*w, WC_TILE_INSPECTOR__WIDX_BUTTON_ROTATE)
        && w->widgets[WC_TILE_INSPECTOR__WIDX_BUTTON_ROTATE].type != WidgetType::empty)
    {
        w->onMouseUp(WC_TILE_INSPECTOR__WIDX_BUTTON_ROTATE);
        return;
    }
}

static void ShortcutRemoveTopBottomToolbarToggle()
{
    auto* windowMgr = GetWindowManager();

    if (gLegacyScene == LegacyScene::titleSequence)
    {
        if (windowMgr->FindByClass(WindowClass::titleLogo) != nullptr)
        {
            windowMgr->CloseByClass(WindowClass::titleLogo);
            windowMgr->CloseByClass(WindowClass::titleOptions);
            windowMgr->CloseByClass(WindowClass::titleMenu);
            windowMgr->CloseByClass(WindowClass::titleExit);
            windowMgr->CloseByClass(WindowClass::titleVersion);
        }
        else
        {
            TitleCreateWindows();
        }
    }
    else
    {
        if (windowMgr->FindByClass(WindowClass::topToolbar) != nullptr)
        {
            windowMgr->CloseByClass(WindowClass::dropdown);
            windowMgr->CloseByClass(WindowClass::topToolbar);
            windowMgr->CloseByClass(WindowClass::bottomToolbar);
        }
        else
        {
            if (gLegacyScene == LegacyScene::playing)
            {
                ContextOpenWindow(WindowClass::topToolbar);
                ContextOpenWindow(WindowClass::bottomToolbar);
            }
            else
            {
                ContextOpenWindow(WindowClass::topToolbar);
                ContextOpenWindowView(WindowView::editorBottomToolbar);
            }
        }
    }
    GfxInvalidateScreen();
}

static void ShortcutAdjustLand()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor)
        return;

    if (isInTrackDesignerOrManager())
        return;

    ToggleLandWindow();
}

static void ShortcutAdjustWater()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor)
        return;

    if (isInTrackDesignerOrManager())
        return;

    ToggleWaterWindow();
}

static void ShortcutBuildScenery()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor)
        return;

    if (isInTrackDesignerOrManager())
        return;

    ToggleSceneryWindow();
}

static void ShortcutBuildPaths()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor)
        return;

    if (isInTrackDesignerOrManager())
        return;

    ToggleFootpathWindow();
}

static void ShortcutBuildNewRide()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene != LegacyScene::scenarioEditor)
    {
        if (!(isInTrackDesignerOrManager()))
        {
            ContextOpenWindow(WindowClass::constructRide);
        }
    }
}

static void ShortcutShowFinancialInformation()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!(isInTrackDesignerOrManager()))
        if (!(getGameState().park.flags & PARK_FLAGS_NO_MONEY))
            ContextOpenWindow(WindowClass::finances);
}

static void ShortcutShowResearchInformation()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
    {
        ContextOpenWindowView(WindowView::rideResearch);
    }
}

static void ShortcutShowRidesList()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
    {
        ContextOpenWindow(WindowClass::rideList);
    }
}

static void ShortcutShowParkInformation()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
    {
        ContextOpenWindow(WindowClass::parkInformation);
    }
}

static void ShortcutShowGuestList()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
    {
        ContextOpenWindow(WindowClass::guestList);
    }
}

static void ShortcutShowStaffList()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
    {
        ContextOpenWindow(WindowClass::staffList);
    }
}

static void ShortcutShowRecentMessages()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (!isInEditorMode())
        ContextOpenWindow(WindowClass::recentNews);
}

static void ShortcutShowMap()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene != LegacyScene::scenarioEditor || getGameState().editorStep == EditorStep::LandscapeEditor)
        if (!(isInTrackDesignerOrManager()))
            ContextOpenWindow(WindowClass::map);
}

static void ShortcutReduceGameSpeed()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (Network::GetMode() == Network::Mode::none)
        GameReduceGameSpeed();
}

static void ShortcutIncreaseGameSpeed()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (Network::GetMode() == Network::Mode::none)
        GameIncreaseGameSpeed();
}

static void ShortcutOpenCheatWindow()
{
    if (gLegacyScene != LegacyScene::playing)
        return;

    // Check if window is already open
    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::cheats);
    if (window != nullptr)
    {
        windowMgr->Close(*window);
        return;
    }
    ContextOpenWindow(WindowClass::cheats);
}

static void ShortcutOpenKeyboardShortcutsWindow()
{
    ShortcutKeysOpen();
}

static void ShortcutOpenTransparencyWindow()
{
    if (gLegacyScene != LegacyScene::playing)
        return;

    ContextOpenWindow(WindowClass::transparency);
}

static void ShortcutClearScenery()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    if (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor)
        return;

    if (isInTrackDesignerOrManager())
        return;

    ToggleClearSceneryWindow();
}

static void ShortcutQuickSaveGame()
{
    // Do a quick save in playing mode and a regular save in Scenario Editor mode. In other cases, don't do anything.
    if (gLegacyScene == LegacyScene::playing)
    {
        ToolCancel();
        SaveGame();
    }
    else if (gLegacyScene == LegacyScene::scenarioEditor)
    {
        auto intent = Intent(WindowClass::loadsave);
        intent.PutEnumExtra<LoadSaveAction>(INTENT_EXTRA_LOADSAVE_ACTION, LoadSaveAction::save);
        intent.PutEnumExtra<LoadSaveType>(INTENT_EXTRA_LOADSAVE_TYPE, LoadSaveType::landscape);
        intent.PutExtra(INTENT_EXTRA_PATH, getGameState().scenarioOptions.name);
        ContextOpenIntent(&intent);
    }
}

static void ShortcutLoadGame()
{
    if (!(isInTrackDesignerOrManager()))
    {
        auto loadOrQuitAction = GameActions::LoadOrQuitAction(GameActions::LoadOrQuitModes::OpenSavePrompt);
        GameActions::Execute(&loadOrQuitAction, getGameState());
    }
}

static void ShortcutOpenSceneryPicker()
{
    if ((gLegacyScene == LegacyScene::titleSequence || gLegacyScene == LegacyScene::trackDesigner
         || gLegacyScene == LegacyScene::trackDesignsManager)
        || (gLegacyScene == LegacyScene::scenarioEditor && getGameState().editorStep != EditorStep::LandscapeEditor))
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* sceneryWindow = windowMgr->FindByClass(WindowClass::scenery);
    if (sceneryWindow == nullptr)
        ToggleSceneryWindow();

    sceneryWindow = windowMgr->FindByClass(WindowClass::scenery);
    if (sceneryWindow != nullptr && !widgetIsDisabled(*sceneryWindow, WC_SCENERY__WIDX_SCENERY_EYEDROPPER_BUTTON)
        && !gWindowSceneryEyedropperEnabled)
    {
        sceneryWindow->onMouseUp(WC_SCENERY__WIDX_SCENERY_EYEDROPPER_BUTTON);
        return;
    }
}

static void ShortcutScaleUp()
{
    Config::Get().general.windowScale += 0.25f;
    Config::Save();
    GfxInvalidateScreen();
    ContextTriggerResize();
    ContextUpdateCursorScale();
}

static void ShortcutScaleDown()
{
    Config::Get().general.windowScale -= 0.25f;
    Config::Get().general.windowScale = std::max(0.5f, Config::Get().general.windowScale);
    Config::Save();
    GfxInvalidateScreen();
    ContextTriggerResize();
    ContextUpdateCursorScale();
}

// Tile inspector shortcuts
static void TileInspectorMouseUp(WidgetIndex widgetIndex)
{
    auto* windowMgr = GetWindowManager();
    auto w = windowMgr->FindByClass(WindowClass::tileInspector);
    if (w != nullptr && !widgetIsDisabled(*w, widgetIndex) && w->widgets[widgetIndex].type != WidgetType::empty)
    {
        w->onMouseUp(widgetIndex);
    }
}

static void TileInspectorMouseDown(WidgetIndex widgetIndex)
{
    auto* windowMgr = GetWindowManager();
    auto w = windowMgr->FindByClass(WindowClass::tileInspector);
    if (w != nullptr && !widgetIsDisabled(*w, widgetIndex) && w->widgets[widgetIndex].type != WidgetType::empty)
    {
        w->onMouseDown(widgetIndex);
    }
}

static void ShortcutToggleWallSlope()
{
    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::tileInspector);
    if (window == nullptr)
    {
        return;
    }

    const TileElement* tileElement = TileInspector::GetSelectedElement();

    // Ensure an element is selected and it's a wall
    if (tileElement == nullptr || tileElement->GetType() != TileElementType::Wall)
    {
        return;
    }

    // Ensure a wall can be built on a slope
    if (tileElement->AsWall()->GetEntry()->flags & WALL_SCENERY_CANT_BUILD_ON_SLOPE)
    {
        return;
    }

    int32_t currSlopeValue = tileElement->AsWall()->GetSlope();
    int32_t newSlopeValue = (currSlopeValue + 1) % 3;

    extern TileCoordsXY windowTileInspectorTile;
    auto modifyTile = GameActions::TileModifyAction(
        windowTileInspectorTile.ToCoordsXY(), GameActions::TileModifyType::WallSetSlope, windowTileInspectorSelectedIndex,
        newSlopeValue);
    GameActions::Execute(&modifyTile, getGameState());
}

static void ShortcutIncreaseElementHeight()
{
    auto* windowMgr = GetWindowManager();
    WindowBase* w = windowMgr->FindByClass(WindowClass::tileInspector);
    if (w != nullptr)
    {
        int action = -1;
        switch (EnumValue(w->tileInspectorPage))
        {
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_SURFACE:
                action = WC_TILE_INSPECTOR__WIDX_SURFACE_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_PATH:
                action = WC_TILE_INSPECTOR__WIDX_PATH_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_TRACK:
                action = WC_TILE_INSPECTOR__WIDX_TRACK_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_SCENERY:
                action = WC_TILE_INSPECTOR__WIDX_SCENERY_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_ENTRANCE:
                action = WC_TILE_INSPECTOR__WIDX_ENTRANCE_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_WALL:
                action = WC_TILE_INSPECTOR__WIDX_WALL_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_LARGE_SCENERY:
                action = WC_TILE_INSPECTOR__WIDX_LARGE_SCENERY_SPINNER_HEIGHT_INCREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_BANNER:
                action = WC_TILE_INSPECTOR__WIDX_BANNER_SPINNER_HEIGHT_INCREASE;
                break;
        }
        if (action != -1 && !widgetIsDisabled(*w, action) && w->widgets[action].type != WidgetType::empty)
            w->onMouseDown(action);
        return;
    }
}

static void ShortcutDecreaseElementHeight()
{
    auto* windowMgr = GetWindowManager();
    WindowBase* w = windowMgr->FindByClass(WindowClass::tileInspector);
    if (w != nullptr)
    {
        int action = -1;
        switch (EnumValue(w->tileInspectorPage))
        {
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_SURFACE:
                action = WC_TILE_INSPECTOR__WIDX_SURFACE_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_PATH:
                action = WC_TILE_INSPECTOR__WIDX_PATH_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_TRACK:
                action = WC_TILE_INSPECTOR__WIDX_TRACK_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_SCENERY:
                action = WC_TILE_INSPECTOR__WIDX_SCENERY_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_ENTRANCE:
                action = WC_TILE_INSPECTOR__WIDX_ENTRANCE_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_WALL:
                action = WC_TILE_INSPECTOR__WIDX_WALL_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_LARGE_SCENERY:
                action = WC_TILE_INSPECTOR__WIDX_LARGE_SCENERY_SPINNER_HEIGHT_DECREASE;
                break;
            case WC_TILE_INSPECTOR__TILE_INSPECTOR_PAGE_BANNER:
                action = WC_TILE_INSPECTOR__WIDX_BANNER_SPINNER_HEIGHT_DECREASE;
                break;
        }
        if (action != -1 && !widgetIsDisabled(*w, action) && w->widgets[action].type != WidgetType::empty)
            w->onMouseDown(action);
        return;
    }
}

static void ShortcutToggleClearanceChecks()
{
    auto& gameState = getGameState();
    auto cheatSetAction = GameActions::CheatSetAction(
        CheatType::disableClearanceChecks, gameState.cheats.disableClearanceChecks ? 0 : 1);
    GameActions::Execute(&cheatSetAction, gameState);
}

static void ShortcutToggleConsole()
{
    auto& console = GetInGameConsole();
    if (console.IsOpen())
    {
        console.Toggle();
    }
    else if (Config::Get().general.debuggingTools && !ContextIsInputActive())
    {
        WindowCancelTextbox();
        console.Toggle();
    }
}

static void ShortcutConstructionTurnLeft()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutTurnLeft();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutTurnLeft();
    }
}

static void ShortcutConstructionTurnRight()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutTurnRight();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutTurnRight();
    }
}

static void ShortcutConstructionSlopeUp()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutSlopeUp();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutSlopeUp();
    }
}

static void ShortcutConstructionBuildCurrent()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutBuildCurrent();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutBuildCurrent();
    }
}

static void ShortcutConstructionSlopeDown()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutSlopeDown();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutSlopeDown();
    }
}

static void ShortcutConstructionDemolishCurrent()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    auto* windowMgr = GetWindowManager();
    WindowBase* window = windowMgr->FindByClass(WindowClass::footpath);
    if (window != nullptr)
    {
        WindowFootpathKeyboardShortcutDemolishCurrent();
    }
    else
    {
        WindowRideConstructionKeyboardShortcutDemolishCurrent();
    }
}

static void ShortcutToggleTransparentWater()
{
    if (gLegacyScene == LegacyScene::titleSequence)
        return;

    Config::Get().general.transparentWater ^= 1;
    Config::Save();
    GfxInvalidateScreen();
}

#pragma endregion

using namespace OpenRCT2::Ui;

void ShortcutManager::registerDefaultShortcuts()
{
    // clang-format off
    // Interface
    registerShortcut(ShortcutId::kInterfaceCloseTop, STR_SHORTCUT_CLOSE_TOP_MOST_WINDOW, "BACKSPACE", []() {
        auto* windowMgr = GetWindowManager();
        windowMgr->CloseTop();
    });
    registerShortcut(ShortcutId::kInterfaceCloseAll, STR_SHORTCUT_CLOSE_ALL_FLOATING_WINDOWS, "SHIFT+BACKSPACE", []() {
        auto* windowMgr = GetWindowManager();
        if (gLegacyScene != LegacyScene::scenarioEditor)
        {
            windowMgr->CloseAll();
        }
        else if (getGameState().editorStep == EditorStep::LandscapeEditor)
        {
            windowMgr->CloseTop();
        }
    });
    registerShortcut(ShortcutId::kInterfaceRotateConstruction, STR_SHORTCUT_ROTATE_CONSTRUCTION_OBJECT, "Z", ShortcutRotateConstructionObject);
    registerShortcut(ShortcutId::kInterfaceCancelConstruction, STR_SHORTCUT_CANCEL_CONSTRUCTION_MODE, "ESCAPE", []() {
        if (gLegacyScene != LegacyScene::titleSequence)
        {
            auto* windowMgr = GetWindowManager();
            auto window = windowMgr->FindByClass(WindowClass::error);
            if (window != nullptr)
            {
                windowMgr->Close(*window);
            }
            else if (gInputFlags.has(InputFlag::toolActive))
            {
                ToolCancel();
            }
        }
    });
    // OPENRCT2MINI focus-mode-plan / Phase F.1 + cursor-selector-modal-
    // plan v2 follow-up: focus-mode activator. Default binding TAB —
    // press to flip the selector active and land focus on the first
    // widget of the topmost open window. Re-pressing TAB resets the
    // selector to the first widget of whatever's on top (useful when
    // you've navigated deep into a window and want to start over).
    //
    // enterFocusModeOnTopmost handles all three jobs in one call: it
    // finds the topmost focusable window with the same two-pass walk
    // the per-frame bootstrap uses, sets focus to that window's first
    // focusable widget directly (bypassing the sameSetOrClass guard
    // in snapFocusToTopmostFocusable), and dispatches enterFocusMode-
    // Requested which flips SelectorMode::active. Returns false (no-
    // op) if no focusable window exists — TAB on an empty title scene
    // before the menu loads, for instance.
    registerShortcut(ShortcutId::kInterfaceEnterFocusMode,
                     STR_SHORTCUT_ENTER_FOCUS_MODE, "TAB", []() {
        OpenRCT2::Ui::GetInputManager().enterFocusModeOnTopmost();
    });
    // OPENRCT2MINI focus-mode-plan §F.8: cycle the focus ring between
    // windows. Defaults SHIFT+TAB / CTRL+TAB as requested; gamepad
    // bindings TBD (will surface in a follow-up once we settle the
    // chord scheme on the Miyoo layout).
    registerShortcut(ShortcutId::kInterfaceCycleNextWindow,
                     STR_SHORTCUT_CYCLE_NEXT_WINDOW, "SHIFT+TAB", []() {
        OpenRCT2::Ui::GetInputManager().cycleFocusedWindow(+1);
    });
    registerShortcut(ShortcutId::kInterfaceCyclePreviousWindow,
                     STR_SHORTCUT_CYCLE_PREVIOUS_WINDOW, "CTRL+TAB", []() {
        OpenRCT2::Ui::GetInputManager().cycleFocusedWindow(-1);
    });
    registerShortcut(ShortcutId::kInterfacePause, STR_SHORTCUT_PAUSE_GAME, "PAUSE", []() {
        if (gLegacyScene != LegacyScene::titleSequence && gLegacyScene != LegacyScene::scenarioEditor && gLegacyScene != LegacyScene::trackDesignsManager)
        {
            auto pauseToggleAction = GameActions::PauseToggleAction();
            GameActions::Execute(&pauseToggleAction, getGameState());
        }
    });
    registerShortcut(ShortcutId::kInterfaceDecreaseSpeed, STR_SHORTCUT_REDUCE_GAME_SPEED, "-", ShortcutReduceGameSpeed);
    registerShortcut(ShortcutId::kInterfaceIncreaseSpeed, STR_SHORTCUT_INCREASE_GAME_SPEED, "=", ShortcutIncreaseGameSpeed);
    registerShortcut(ShortcutId::kInterfaceToggleToolbars, STR_SHORTCUT_TOGGLE_VISIBILITY_OF_TOOLBARS, ShortcutRemoveTopBottomToolbarToggle);
    registerShortcut(ShortcutId::kInterfaceScreenshot, STR_SHORTCUT_SCREENSHOT, "CTRL+S", []() { gScreenshotCountdown = 2; });
    registerShortcut(ShortcutId::kInterfaceGiantScreenshot, STR_SHORTCUT_GIANT_SCREENSHOT, "CTRL+SHIFT+S", ScreenshotGiant);
    registerShortcut(ShortcutId::kInterfaceLoadGame, STR_LOAD_GAME, "CTRL+L", ShortcutLoadGame);
    registerShortcut(ShortcutId::kInterfaceSaveGame, STR_SAVE_GAME, "CTRL+F10", ShortcutQuickSaveGame);
    registerShortcut(ShortcutId::kInterfaceMute, STR_SHORTCUT_MUTE_SOUND, Audio::ToggleAllSounds);
    registerShortcut(ShortcutId::kInterfaceSceneryPicker, STR_SHORTCUT_OPEN_SCENERY_PICKER, ShortcutOpenSceneryPicker);
    registerShortcut(
        ShortcutId::kInterfaceDisableClearance, STR_SHORTCUT_TOGGLE_CLEARANCE_CHECKS, ShortcutToggleClearanceChecks);
    registerShortcut(ShortcutId::kInterfaceMultiplayerChat, STR_SHORTCUT_SEND_MESSAGE, "C", []() {
        if (gLegacyScene != LegacyScene::titleSequence && ChatAvailable())
        {
            ChatToggle();
        }
    });
    registerShortcut(ShortcutId::kInterfaceScaleToggleWindowMode, STR_SHORTCUT_WINDOWED_MODE_TOGGLE, "ALT+RETURN", ToggleWindowedMode);
    registerShortcut(ShortcutId::kInterfaceScaleIncrease, STR_SHORTCUT_SCALE_UP, ShortcutScaleUp);
    registerShortcut(ShortcutId::kInterfaceScaleDecrease, STR_SHORTCUT_SCALE_DOWN, ShortcutScaleDown);
    registerShortcut(ShortcutId::kInterfaceOpenLand, STR_SHORTCUT_ADJUST_LAND, "F1", ShortcutAdjustLand);
    registerShortcut(ShortcutId::kInterfaceOpenWater, STR_SHORTCUT_ADJUST_WATER, "F2", ShortcutAdjustWater);
    registerShortcut(ShortcutId::kInterfaceClearScenery, STR_SHORTCUT_CLEAR_SCENERY, "B", ShortcutClearScenery);
    registerShortcut(ShortcutId::kInterfaceOpenScenery, STR_SHORTCUT_BUILD_SCENERY, "F3", ShortcutBuildScenery);
    registerShortcut(ShortcutId::kInterfaceOpenFootpaths, STR_SHORTCUT_BUILD_PATHS, "F4", ShortcutBuildPaths);
    registerShortcut(ShortcutId::kInterfaceOpenNewRide, STR_SHORTCUT_BUILD_NEW_RIDE, "F5", ShortcutBuildNewRide);
    registerShortcut(ShortcutId::kInterfaceOpenFinances, STR_SHORTCUT_SHOW_FINANCIAL_INFORMATION, "F", ShortcutShowFinancialInformation);
    registerShortcut(ShortcutId::kInterfaceOpenResearch, STR_SHORTCUT_SHOW_RESEARCH_INFORMATION, "D", ShortcutShowResearchInformation);
    registerShortcut(ShortcutId::kInterfaceOpenRides, STR_SHORTCUT_SHOW_RIDES_LIST, "R", ShortcutShowRidesList);
    registerShortcut(ShortcutId::kInterfaceOpenPark, STR_SHORTCUT_SHOW_PARK_INFORMATION, "P", ShortcutShowParkInformation);
    registerShortcut(ShortcutId::kInterfaceOpenGuests, STR_SHORTCUT_SHOW_GUEST_LIST, "G", ShortcutShowGuestList);
    registerShortcut(ShortcutId::kInterfaceOpenStaff, STR_SHORTCUT_SHOW_STAFF_LIST, "S", ShortcutShowStaffList);
    registerShortcut(ShortcutId::kInterfaceOpenMessages, STR_SHORTCUT_SHOW_RECENT_MESSAGES, "M", ShortcutShowRecentMessages);
    registerShortcut(ShortcutId::kInterfaceOpenMap, STR_SHORTCUT_SHOW_MAP, "TAB", ShortcutShowMap);
    registerShortcut(ShortcutId::kInterfaceShowOptions, STR_SHORTCUT_SHOW_OPTIONS, std::bind(ContextOpenWindow, WindowClass::options));
    registerShortcut(ShortcutId::kInterfaceOpenKeyboardShortcuts, STR_SHORTCUT_OPEN_KEYBOARD_SHORTCUTS_WINDOW, "SHIFT+/", ShortcutOpenKeyboardShortcutsWindow);
    registerShortcut(ShortcutId::kInterfaceOpenTransparencyOptions, STR_SHORTCUT_OPEN_TRANSPARENCY_OPTIONS, "CTRL+T", ShortcutOpenTransparencyWindow);
    registerShortcut(ShortcutId::kInterfaceOpenCheats, STR_SHORTCUT_OPEN_CHEATS_WINDOW, "CTRL+ALT+C", ShortcutOpenCheatWindow);
    registerShortcut(ShortcutId::kInterfaceOpenTileInspector, STR_SHORTCUT_OPEN_TILE_INSPECTOR, []() {
        if (Config::Get().interface.toolbarShowCheats)
        {
            OpenWindow(WindowClass::tileInspector);
        }
    });
    registerShortcut(ShortcutId::kInterfaceMultiplayerShow, STR_SHORTCUT_SHOW_MULTIPLAYER, []() {
        if (Network::GetMode() != Network::Mode::none)
        {
            OpenWindow(WindowClass::multiplayer);
        }
    });

    // View
    registerShortcut(ShortcutId::kViewGeneralZoomOut, STR_SHORTCUT_ZOOM_VIEW_OUT, "PAGEUP", std::bind(MainWindowZoom, false, false));
    registerShortcut(ShortcutId::kViewGeneralZoomIn, STR_SHORTCUT_ZOOM_VIEW_IN, "PAGEDOWN", std::bind(MainWindowZoom, true, false));
    // OPENRCT2MINI mouse-input refactor: context-sensitive wheel
    // shortcuts. Action lambdas zoom only when the cursor is over a
    // viewport-class window; over scroll widgets / spinners they
    // no-op so the existing _cursorState.wheel-driven WindowAllWheelInput
    // dispatch can scroll the widget without doubling up with zoom.
    // Default-bound to MOUSE WHEEL UP / DOWN (further down in this
    // function); kViewGeneralZoomIn/Out keep their unconditional
    // action and PAGEUP/PAGEDOWN/PAD L2/R2 defaults for force-zoom.
    registerShortcut(ShortcutId::kViewZoomScrollUp,   STR_SHORTCUT_ZOOM_OR_SCROLL_UP,   "", std::bind(FireZoomOrScrollWheel, true));
    registerShortcut(ShortcutId::kViewZoomScrollDown, STR_SHORTCUT_ZOOM_OR_SCROLL_DOWN, "", std::bind(FireZoomOrScrollWheel, false));
    registerShortcut(ShortcutId::kViewGeneralRotateClockwise, STR_SHORTCUT_ROTATE_VIEW_CLOCKWISE, "RETURN", "MOUSE 6", std::bind(RotateCamera, 1));
    registerShortcut(ShortcutId::kViewGeneralRotateAnticlockwise, STR_SHORTCUT_ROTATE_VIEW_ANTICLOCKWISE, "SHIFT+RETURN", "MOUSE 5", std::bind(RotateCamera, -1));
    registerShortcut(ShortcutId::kViewScrollUp, STR_SHORTCUT_SCROLL_MAP_UP, "UP", []() {});
    registerShortcut(ShortcutId::kViewScrollLeft, STR_SHORTCUT_SCROLL_MAP_LEFT, "LEFT", []() {});
    registerShortcut(ShortcutId::kViewScrollRight, STR_SHORTCUT_SCROLL_MAP_RIGHT, "RIGHT", []() {});
    registerShortcut(ShortcutId::kViewScrollDown, STR_SHORTCUT_SCROLL_MAP_DOWN, "DOWN", []() {});
    registerShortcut(ShortcutId::kViewToggleUnderground, STR_SHORTCUT_UNDERGROUND_VIEW_TOGGLE, "1", std::bind(ToggleViewFlag, VIEWPORT_FLAG_UNDERGROUND_INSIDE));
    registerShortcut(ShortcutId::kViewToggleTransparentWater, STR_VIEWPORT_TRANSPARENT_WATER, "2", ShortcutToggleTransparentWater);
    registerShortcut(ShortcutId::kViewToggleBaseLand, STR_SHORTCUT_REMOVE_BASE_LAND_TOGGLE, "H", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_BASE));
    registerShortcut(ShortcutId::kViewToggleVerticalLand, STR_SHORTCUT_REMOVE_VERTICAL_LAND_TOGGLE, "V", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_VERTICAL));
    registerShortcut(ShortcutId::kViewToggleRides, STR_SHORTCUT_SEE_THROUGH_RIDES_TOGGLE, "3", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_RIDES));
    registerShortcut(ShortcutId::kViewToggleVehicles, STR_SHORTCUT_SEE_THROUGH_VEHICLES_TOGGLE, std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_VEHICLES));
    registerShortcut(ShortcutId::kViewToggleVegetation, STR_SHORTCUT_SEE_THROUGH_VEGETATION_TOGGLE, std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_VEGETATION));
    registerShortcut(ShortcutId::kViewToggleScenery, STR_SHORTCUT_SEE_THROUGH_SCENERY_TOGGLE, "4", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_SCENERY));
    registerShortcut(ShortcutId::kViewToggleFootpaths, STR_SHORTCUT_SEE_THROUGH_PATHS_TOGGLE, std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_PATHS));
    registerShortcut(ShortcutId::kViewToggleSupports, STR_SHORTCUT_INVISIBLE_SUPPORTS_TOGGLE, "5", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_SUPPORTS));
    registerShortcut(ShortcutId::kViewToggleGuests, STR_SHORTCUT_SEE_THROUGH_GUESTS_TOGGLE, "6", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_GUESTS));
    registerShortcut(ShortcutId::kViewToggleStaff, STR_SHORTCUT_SEE_THROUGH_STAFF_TOGGLE, std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIDE_STAFF));
    registerShortcut(ShortcutId::kViewToggleLandHeightMarkers, STR_SHORTCUT_HEIGHT_MARKS_ON_LAND_TOGGLE, "8", std::bind(ToggleViewFlag, VIEWPORT_FLAG_LAND_HEIGHTS));
    registerShortcut(ShortcutId::kViewToggleTrackHeightMarkers, STR_SHORTCUT_HEIGHT_MARKS_ON_RIDE_TRACKS_TOGGLE, "9", std::bind(ToggleViewFlag, VIEWPORT_FLAG_TRACK_HEIGHTS));
    registerShortcut(ShortcutId::kViewToggleFootpathHeightMarkers, STR_SHORTCUT_HEIGHT_MARKS_ON_PATHS_TOGGLE, "0", std::bind(ToggleViewFlag, VIEWPORT_FLAG_PATH_HEIGHTS));
    registerShortcut(ShortcutId::kViewToggleGridlines, STR_SHORTCUT_GRIDLINES_DISPLAY_TOGGLE, "7", std::bind(ToggleViewFlag, VIEWPORT_FLAG_GRIDLINES));
    registerShortcut(ShortcutId::kViewToggleCutAway, STR_SHORTCUT_VIEW_CLIPPING, std::bind(OpenWindow, WindowClass::viewClipping));
    registerShortcut(ShortcutId::kViewToggleFootpathIssues, STR_SHORTCUT_HIGHLIGHT_PATH_ISSUES_TOGGLE, "I", std::bind(ToggleViewFlag, VIEWPORT_FLAG_HIGHLIGHT_PATH_ISSUES));

    // Window
    registerShortcut(ShortcutId::kWindowRideConstructionTurnLeft, STR_SHORTCUT_CONSTRUCTION_TURN_LEFT, "NUMPAD 4", ShortcutConstructionTurnLeft);
    registerShortcut(ShortcutId::kWindowRideConstructionTurnRight, STR_SHORTCUT_CONSTRUCTION_TURN_RIGHT, "NUMPAD 6", ShortcutConstructionTurnRight);
    registerShortcut(ShortcutId::kWindowRideConstructionDefault, STR_SHORTCUT_CONSTRUCTION_USE_TRACK_DEFAULT, "NUMPAD 5", WindowRideConstructionKeyboardShortcutUseTrackDefault);
    registerShortcut(ShortcutId::kWindowRideConstructionSlopeDown, STR_SHORTCUT_CONSTRUCTION_SLOPE_DOWN, "NUMPAD 2", ShortcutConstructionSlopeDown);
    registerShortcut(ShortcutId::kWindowRideConstructionSlopeUp, STR_SHORTCUT_CONSTRUCTION_SLOPE_UP, "NUMPAD 8", ShortcutConstructionSlopeUp);
    registerShortcut(ShortcutId::kWindowRideConstructionChainLift, STR_SHORTCUT_CONSTRUCTION_CHAIN_LIFT_TOGGLE, "NUMPAD +", WindowRideConstructionKeyboardShortcutChainLiftToggle);
    registerShortcut(ShortcutId::kWindowRideConstructionBankLeft, STR_SHORTCUT_CONSTRUCTION_BANK_LEFT, "NUMPAD 1", WindowRideConstructionKeyboardShortcutBankLeft);
    registerShortcut(ShortcutId::kWindowRideConstructionBankRight, STR_SHORTCUT_CONSTRUCTION_BANK_RIGHT, "NUMPAD 3", WindowRideConstructionKeyboardShortcutBankRight);
    registerShortcut(ShortcutId::kWindowRideConstructionPrevious, STR_SHORTCUT_CONSTRUCTION_PREVIOUS_TRACK, "NUMPAD 7", WindowRideConstructionKeyboardShortcutPreviousTrack);
    registerShortcut(ShortcutId::kWindowRideConstructionNext, STR_SHORTCUT_CONSTRUCTION_NEXT_TRACK, "NUMPAD 9", WindowRideConstructionKeyboardShortcutNextTrack);
    registerShortcut(ShortcutId::kWindowRideConstructionBuild, STR_SHORTCUT_CONSTRUCTION_BUILD_CURRENT, "NUMPAD 0", ShortcutConstructionBuildCurrent);
    registerShortcut(ShortcutId::kWindowRideConstructionDemolish, STR_SHORTCUT_CONSTRUCTION_DEMOLISH_CURRENT, "NUMPAD -", ShortcutConstructionDemolishCurrent);
    registerShortcut(ShortcutId::kWindowTileInspectorToggleInvisibility, STR_SHORTCUT_TOGGLE_INVISIBILITY, WindowTileInspectorKeyboardShortcutToggleInvisibility);
    registerShortcut(ShortcutId::kWindowTileInspectorCopy, STR_SHORTCUT_COPY_ELEMENT, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_COPY));
    registerShortcut(ShortcutId::kWindowTileInspectorPaste, STR_SHORTCUT_PASTE_ELEMENT, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_PASTE));
    registerShortcut(ShortcutId::kWindowTileInspectorSort, STR_SHORTCUT_SORT_ELEMENTS, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_SORT));
    registerShortcut(ShortcutId::kWindowTileInspectorRemove, STR_SHORTCUT_REMOVE_ELEMENT, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_REMOVE));
    registerShortcut(ShortcutId::kWindowTileInspectorMoveUp, STR_SHORTCUT_MOVE_ELEMENT_UP, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_MOVE_UP));
    registerShortcut(ShortcutId::kWindowTileInspectorMoveDown, STR_SHORTCUT_MOVE_ELEMENT_DOWN, std::bind(TileInspectorMouseUp, WC_TILE_INSPECTOR__WIDX_BUTTON_MOVE_DOWN));
    registerShortcut(ShortcutId::kWindowTileInspectorIncreaseX, STR_SHORTCUT_INCREASE_X_COORD, std::bind(TileInspectorMouseDown, WC_TILE_INSPECTOR__WIDX_SPINNER_X_INCREASE));
    registerShortcut(ShortcutId::kWindowTileInspectorDecreaseX, STR_SHORTCUT_DECREASE_X_COORD, std::bind(TileInspectorMouseDown, WC_TILE_INSPECTOR__WIDX_SPINNER_X_DECREASE));
    registerShortcut(ShortcutId::kWindowTileInspectorIncreaseY, STR_SHORTCUT_INCREASE_Y_COORD, std::bind(TileInspectorMouseDown, WC_TILE_INSPECTOR__WIDX_SPINNER_Y_INCREASE));
    registerShortcut(ShortcutId::kWindowTileInspectorDecreaseY, STR_SHORTCUT_DECREASE_Y_COORD, std::bind(TileInspectorMouseDown, WC_TILE_INSPECTOR__WIDX_SPINNER_Y_DECREASE));
    registerShortcut(ShortcutId::kWindowTileInspectorIncreaseHeight, STR_SHORTCUT_INCREASE_ELEM_HEIGHT, ShortcutIncreaseElementHeight);
    registerShortcut(ShortcutId::kWindowTileInspectorDecreaseHeight, STR_SHORTCUT_DECREASE_ELEM_HEIGHT, ShortcutDecreaseElementHeight);
    registerShortcut(ShortcutId::kWindowTileInspectorChangeWallSlope, STR_SHORTCUT_TOGGLE_WALL_SLOPE, ShortcutToggleWallSlope);

    // Debug
    registerShortcut(ShortcutId::kDebugToggleConsole, STR_CONSOLE, "`", ShortcutToggleConsole);
    registerShortcut(ShortcutId::kDebugAdvanceTick, STR_SHORTCUT_ADVANCE_TO_NEXT_TICK, []() {
        if (gLegacyScene != LegacyScene::titleSequence && gLegacyScene != LegacyScene::scenarioEditor && gLegacyScene != LegacyScene::trackDesignsManager)
        {
            gDoSingleUpdate = true;
        }
    });
    registerShortcut(ShortcutId::kDebugTogglePaintDebugWindow, STR_SHORTCUT_DEBUG_PAINT_TOGGLE, []() {
        if (gLegacyScene != LegacyScene::titleSequence)
        {
            auto* windowMgr = GetWindowManager();
            auto window = windowMgr->FindByClass(WindowClass::debugPaint);
            if (window != nullptr)
            {
                windowMgr->Close(*window);
            }
            else
            {
                ContextOpenWindow(WindowClass::debugPaint);
            }
        }
    });

    // OPENRCT2MINI gamepad-plan 1.5a: virtual cursor + chord shortcuts.
    // These IDs are net-new — they migrate the _vKb*-driven cursor /
    // click / shade / Z-lock behaviour out of UiContext and into the
    // ShortcutManager pipeline. In 1.5a we only REGISTER the IDs with
    // stub no-op actions so they show up in the rebind UI (and can be
    // reassigned by the user) without changing any in-game behaviour
    // yet. The actions get wired in 1.5b-f, then the legacy _vKb*
    // block is deleted in 1.6.
    //
    // Default keyboard bindings are passed for the rebind UI's "reset
    // to default" path. cut 44 strips them via [[maybe_unused]] in the
    // RegisteredShortcut constructor — the device's vendor SDL2 emits
    // fake keyboard events for D-pad / face buttons, so live keyboard
    // defaults would double-fire — but the strings still show in the
    // rebind UI as the canonical "what this is" hint.
    //
    // Default PAD bindings come from registerPadDefault below. Per the
    // migration table in gamepad-plan.md §1.5: cursor motion → DPAD
    // directions; click → south face (SDL "A"); cancel → east face
    // (SDL "B"); shade-window → north face (SDL "Y"); chord shortcuts
    // get explicit "PAD MOD+ACTION" strings.
    registerShortcut(ShortcutId::kCursorUp,           STR_SHORTCUT_CURSOR_UP,           "UP",      []() {});
    registerShortcut(ShortcutId::kCursorDown,         STR_SHORTCUT_CURSOR_DOWN,         "DOWN",    []() {});
    registerShortcut(ShortcutId::kCursorLeft,         STR_SHORTCUT_CURSOR_LEFT,         "LEFT",    []() {});
    registerShortcut(ShortcutId::kCursorRight,        STR_SHORTCUT_CURSOR_RIGHT,        "RIGHT",   []() {});
    // OPENRCT2MINI focus-mode-plan §F.9: focus-ring direction moves.
    // Separate namespace from cursor.* so the user can rebind each
    // side independently — see ShortcutIds.h kFocusUp comment for
    // the rationale. Action lambdas are empty stubs (like cursor.*);
    // the actual focus-ring movement runs from
    // WidgetFocusContextImpl::onShortcut, which dispatches its
    // findNearestInDirection / setFocus calls on these ids.
    // Defaults overlap with cursor.* keyboard arrows + D-pad on
    // purpose: first-launch behaviour stays "D-pad navigates the
    // focus ring", and users who want non-overlap (e.g. D-pad
    // moves only cursor, never focus) can drop the focus.* binding
    // through the Input Bindings window.
    registerShortcut(ShortcutId::kFocusUp,    STR_SHORTCUT_FOCUS_UP,    "UP",    []() {});
    registerShortcut(ShortcutId::kFocusDown,  STR_SHORTCUT_FOCUS_DOWN,  "DOWN",  []() {});
    registerShortcut(ShortcutId::kFocusLeft,  STR_SHORTCUT_FOCUS_LEFT,  "LEFT",  []() {});
    registerShortcut(ShortcutId::kFocusRight, STR_SHORTCUT_FOCUS_RIGHT, "RIGHT", []() {});
    // OPENRCT2MINI cursor-selector-modal-plan §CS-R1: explicit
    // show-cursor escape hatch. No default binding; users on the
    // Mini bind to a face button if they want a way out of the
    // always-on selector. Host users with widgetFocusAlwaysOn=true
    // get the same affordance — they can bind a key if mouse motion
    // isn't sufficient (rare).
    registerShortcut(ShortcutId::kInterfaceShowCursor, STR_SHORTCUT_SHOW_CURSOR, "", []() {
        GetInputManager().onTransitionEvent(InputManager::SelectorTransitionSource::wakeCursorRequested);
    });
    registerShortcut(ShortcutId::kCursorClick,        STR_SHORTCUT_CURSOR_CLICK,        "Z",       []() {});
    registerShortcut(ShortcutId::kCursorCancel,       STR_SHORTCUT_CURSOR_CANCEL,       "X",       []() {});
    registerShortcut(ShortcutId::kCursorFastModifier, STR_SHORTCUT_CURSOR_FAST_MODIFIER, "LSHIFT", []() {});
    registerShortcut(ShortcutId::kInterfaceConstructionZLock,
                     STR_SHORTCUT_CONSTRUCTION_Z_LOCK,        "LCTRL",  []() {});
    // OPENRCT2MINI hold-binding refactor: shade-window fires on tap-
    // release (binding holdMs == 0) of the bound input; shade-all fires
    // on hold-elapsed (binding holdMs == 500, set by the "HOLD " prefix
    // on its default bindings further down). ShortcutManager's
    // _holdPending mechanism coordinates the two so the same physical
    // input drives tap → shade-window and 500ms-hold → shade-all
    // without double-firing.
    registerShortcut(ShortcutId::kInterfaceShadeWindowUnderCursor,
                     STR_SHORTCUT_SHADE_WINDOW_UNDER_CURSOR,  "C",      []() {
        FireShadeWindowUnderCursor();
    });
    registerShortcut(ShortcutId::kInterfaceToggleShadeAllWindows,
                     STR_SHORTCUT_TOGGLE_SHADE_ALL_WINDOWS,   "SHIFT+C", []() {
        FireToggleShadeAll();
    });
    registerShortcut(ShortcutId::kInterfaceCloseWindowUnderCursor,
                     STR_SHORTCUT_CLOSE_WINDOW_UNDER_CURSOR,  "ALT+C",  []() {
        FireCloseWindowUnderCursor();
    });
    // OPENRCT2MINI gamepad-plan 1.5g: shift modifier — held-state-only
    // (queried by handleModifiers); action lambda is a stub. Default
    // keyboard binding is empty because SDL's real Shift mod state
    // already drives these behaviours; this shortcut is for host
    // gamepad users who want to bind PAD R3 / PAD L3 / etc. to access
    // OpenRCT2's KMOD_SHIFT-driven construction features.
    registerShortcut(ShortcutId::kInterfaceShiftModifier,
                     STR_SHORTCUT_SHIFT_MODIFIER,             "",       []() {});
    // OPENRCT2MINI gamepad-plan 1.6c.1: dismiss / confirm — fire the
    // active modal's registered ModalHooks callbacks. Action lambdas
    // are STUBS at this stage (1.6c.1 ships only the IDs + defaults).
    // 1.6c.2 wires the InputManager modal-gate dispatch that consults
    // these shortcuts via shortcutManager.matches() ahead of the per-
    // context hardcoded checks. After 1.6c.7 the matching gate is the
    // sole dispatcher; the per-modal callbacks installed via
    // setModalHooks do the actual work.
    //
    // Default ESCAPE / RETURN keyboard bindings overlap with the
    // existing kInterfaceCancelConstruction (ESCAPE) and
    // kViewGeneralRotateClockwise (RETURN) bindings — both shortcuts
    // fire on the same key, action lambdas run independently, no
    // regression for world-context users.
    registerShortcut(ShortcutId::kInterfaceDismiss,
                     STR_SHORTCUT_DISMISS,                    "ESCAPE", []() {});
    registerShortcut(ShortcutId::kInterfaceConfirm,
                     STR_SHORTCUT_CONFIRM,                    "RETURN", []() {});
    // OPENRCT2MINI mouse-input refactor: camera drag shortcut. Held →
    // camera pans with cursor motion. Tap-and-release < 500ms → fires
    // the context-sensitive right-click action at the cursor position
    // (delete tile element, etc.). Default mouse binding RMB and
    // default gamepad binding PAD B added below; the action lambda is
    // a stub because the per-frame poll in UiContext::ProcessWorld-
    // Cursor drives both Begin and End directly off the bound input's
    // held state.
    registerShortcut(ShortcutId::kInterfaceCameraDrag,
                     STR_SHORTCUT_CAMERA_DRAG,                "",       []() {});
    // clang-format on

    // OPENRCT2MINI gamepad-plan 1.4: ship a conservative set of default
    // gamepad bindings against existing shortcut IDs that have a
    // straightforward action mapping. Cursor / click / cancel / shade /
    // Z-lock / fast-cursor are deferred to 1.5 — those need new
    // shortcut IDs because the existing _vKb*-driven behavior is not
    // exposed via ShortcutManager today.
    //
    // Mini physical layout reminder: A is east face (cancel/back), B is
    // south (click/confirm), Y is north (shade), X is west (swap).
    // SDL_GameController normalises face buttons by physical position
    // (A=south, B=east, X=west, Y=north), so the Mini's "A" maps to
    // SDL_CONTROLLER_BUTTON_B, etc. Phase 2's vendor SDL2 mapping puts
    // the Mini bits in the right SDL slots so the same defaults work
    // there. For now (Phase 1) only host pads see these bindings since
    // the device's joystick driver is still a stub.
    //
    // View transforms — these are the cleanest mappings since the
    // existing shortcut IDs do exactly what we want:
    registerPadDefault(ShortcutId::kViewGeneralRotateAnticlockwise, "PAD L1");
    registerPadDefault(ShortcutId::kViewGeneralRotateClockwise,     "PAD R1");
    registerPadDefault(ShortcutId::kViewGeneralZoomOut,             "PAD L2");
    registerPadDefault(ShortcutId::kViewGeneralZoomIn,              "PAD R2");

    // OPENRCT2MINI gamepad-plan 1.5h: device-friendly keyboard
    // defaults for the W/S scancodes that the device's vendor SDL2
    // emits for L2 / R2. The SCANCODE_W/_S intercepts in UiContext
    // were dropped — these keyboard bindings now drive view-rotate
    // unconditionally on the device, matching cut-162's L2-CCW /
    // R2-CW polish. Loses the _vGamepadMod swap-to-zoom behaviour;
    // user binds a chord shortcut if they miss it.
    registerKeyboardDefault(ShortcutId::kViewGeneralRotateAnticlockwise, "W");
    registerKeyboardDefault(ShortcutId::kViewGeneralRotateClockwise,     "S");

    // OPENRCT2MINI gamepad-plan 1.5i: device-friendly keyboard default
    // for the V scancode (face-Y on the device's vendor SDL2 mapping).
    // The SCANCODE_V intercept in UiContext was dropped in favour of
    // ShortcutManager dispatch.
    registerKeyboardDefault(ShortcutId::kInterfaceRotateConstruction, "V");

    // OPENRCT2MINI gamepad-plan 1.5j: the device's R1 maps to LALT
    // through vendor SDL2. With the legacy _vKbShift / _vGamepadMod
    // setters in the SCANCODE_LALT handler stripped down to just the
    // R1+C chord prerequisite, fast-cursor needs an explicit binding
    // path. LALT as a keyboard default for cursor.fast_modifier means
    // R1 on the device drives fast-cursor through the new held-state
    // poll. Host users can rebind to LSHIFT or whatever they prefer.
    registerKeyboardDefault(ShortcutId::kCursorFastModifier, "LALT");

    // OPENRCT2MINI gamepad-plan 1.6: device-friendly keyboard defaults
    // for cursor motion / click / cancel. The device's vendor SDL2
    // emits arrow scancodes for the D-pad and Z / X for face buttons
    // (cut 38b's mapping). Without these defaults, the legacy _vKb*
    // arrow / Z / X latches in InterceptVirtualCursorKey would have
    // been the only things driving cursor motion / click / cancel on
    // the device — and 1.6 deletes those latches entirely. Adding
    // the bindings explicitly here means scancode → shortcut fall-
    // through covers the device's needs unconditionally, and the user
    // can additionally rebind to anything they want via the rebind UI.
    registerKeyboardDefault(ShortcutId::kCursorUp,     "UP");
    registerKeyboardDefault(ShortcutId::kCursorDown,   "DOWN");
    registerKeyboardDefault(ShortcutId::kCursorLeft,   "LEFT");
    registerKeyboardDefault(ShortcutId::kCursorRight,  "RIGHT");
    registerKeyboardDefault(ShortcutId::kCursorClick,  "Z");
    registerKeyboardDefault(ShortcutId::kCursorCancel, "X");

    // OPENRCT2MINI mouse-input refactor: real mouse buttons used to be
    // hardcoded in the SDL_MOUSEBUTTONDOWN/UP handler — SDL_BUTTON_LEFT
    // synthesised StoreMouseInput(leftPress) directly. That path is gone
    // now; mouse buttons are bindable through the same shortcut system
    // as keyboard / gamepad. cursor.click / cursor.cancel get LMB / RMB
    // defaults so the user-facing behaviour matches what was hardcoded
    // before — but the user can rebind to any input.
    //
    // The polling loop in ProcessWorldCursor::handleButton picks up the
    // shortcut held-state (via getState(), which consults
    // SDL_GetMouseState's bitmask for mouse-kind bindings) and
    // synthesises StoreMouseInput on transitions.
    registerMouseDefault(ShortcutId::kCursorClick,  "LMB");
    registerMouseDefault(ShortcutId::kCursorCancel, "RMB");

    // OPENRCT2MINI mouse-input refactor: scroll-wheel defaults bind
    // to the CONTEXT-SENSITIVE Zoom+Scroll shortcuts (not the
    // unconditional kViewGeneralZoomIn/Out). The action lambdas
    // dispatch on cursor position: zoom over viewport, no-op over
    // scroll/spinner widgets so the existing _cursorState.wheel feed
    // handles widget-local scroll without double-firing zoom.
    registerMouseDefault(ShortcutId::kViewZoomScrollUp,   "MOUSE WHEEL UP");
    registerMouseDefault(ShortcutId::kViewZoomScrollDown, "MOUSE WHEEL DOWN");

    // OPENRCT2MINI cursor-cancel-drag split: kInterfaceCameraDrag is
    // the single binding that drives ALL three right-click drag
    // gestures (camera-pan over viewport, scroll-drag over overflowing
    // lists, window-position drag over draggable dialogs). Default
    // RMB and PAD B. cursor.cancel is co-bound to the same inputs by
    // default (so a single RMB tap still feels like one gesture to
    // the user) but no longer carries any drag side-effects — its
    // rightPress handler in MouseInput.cpp only closes tooltips and
    // brings the under-cursor window to front. The drag dispatch is
    // owned by InputContextDragBeginAtCursor, called from the
    // kInterfaceCameraDrag held-state poll in UiContext::Process-
    // WorldCursor.
    registerMouseDefault(ShortcutId::kInterfaceCameraDrag, "RMB");
    // shade-window keeps the C scancode as its default (the device
    // emits C for face-X). The SCANCODE_C handler in UiContext keeps
    // the R1+C chord-close special case as the only remaining hard-
    // coded device intercept until Phase 2 lands real joybutton events.
    registerKeyboardDefault(ShortcutId::kInterfaceShadeWindowUnderCursor, "C");

    // OPENRCT2MINI gamepad-plan 1.6: device L1 dual-emits Q + LSHIFT
    // through the vendor SDL2's set_key patch. The legacy SCANCODE_Q
    // intercept that injected KMOD_LSHIFT into SDL's mod state was
    // dropped in 1.6 — Q is now the default keyboard binding for the
    // shift modifier shortcut, which ORs into ModifierKey::shift via
    // handleModifiers. Same downstream effect on the device, but
    // cleaner: shift modifier doesn't pollute SDL chord-shortcut
    // matching (CTRL+SHIFT+S etc. only see real Shift). Host users
    // can rebind to whatever physical control they prefer.
    registerKeyboardDefault(ShortcutId::kInterfaceShiftModifier, "Q");

    // OPENRCT2MINI shift/ctrl-modifier refactor: bind the real Shift /
    // Ctrl keys to their respective modifier shortcuts as defaults. The
    // SDL_GetModState() hardcoded path in handleModifiers used to set
    // ModifierKey::shift / ::ctrl unconditionally on real Shift / Ctrl
    // press; that path is gone, so out-of-the-box Shift / Ctrl behaviour
    // depends on these bindings now. Both LSHIFT and RSHIFT (and likewise
    // for Ctrl) are bound so either side of the keyboard works.
    //
    // Standalone modifier-key tokens are recognised by ShortcutInput's
    // parser (ShortcutInput.cpp; previously they tried to parse as
    // chord prefixes and silently produced a 0-button binding).
    // InputManager::getState recognises bare-modifier keyboard
    // bindings and reads the keyboard scancode bitmap directly,
    // bypassing the SDL mod-state comparison that would otherwise
    // always mismatch.
    registerKeyboardDefault(ShortcutId::kInterfaceShiftModifier,    "LSHIFT");
    registerKeyboardDefault(ShortcutId::kInterfaceShiftModifier,    "RSHIFT");
    registerKeyboardDefault(ShortcutId::kInterfaceConstructionZLock, "LCTRL");
    registerKeyboardDefault(ShortcutId::kInterfaceConstructionZLock, "RCTRL");

    // System buttons:
    registerPadDefault(ShortcutId::kInterfacePause,        "PAD START");
    registerPadDefault(ShortcutId::kDebugToggleConsole,    "PAD GUIDE");

    // Face buttons (A / B / X / Y, plus the four DPAD directions) are
    // intentionally NOT bound here. They're currently captured by the
    // _vKb*-driven virtual cursor system in UiContext::Process-
    // VirtualGamepadCursor (lines 1338-1356) for left-click / right-click /
    // shade / cursor motion. Adding a shortcut binding for any of these
    // alongside the existing direct-poll behaviour produces double-fires:
    // pressing south face would both left-click AND fire the bound
    // shortcut. Phase 1.5 migrates the cursor / click / shade behaviours
    // to ShortcutManager, at which point face-button defaults can ship
    // without conflict.
    //
    // OPENRCT2MINI gamepad-plan 1.5a: pre-register PAD defaults for the
    // new cursor / chord shortcut IDs added above. The defaults LIVE in
    // shortcuts.json (and show up in the rebind UI), but the actions are
    // stub no-ops in 1.5a — pressing these buttons does nothing yet.
    // The legacy _vKb* direct-poll path is still authoritative for
    // cursor / click / shade until 1.6 deletes it.
    //
    // Why ship PAD defaults now even though they're inert: it lets the
    // user see + test the rebind UI in 1.5a, and means 1.5b-f can wire
    // actions one at a time without also having to plumb defaults each
    // time.
    registerPadDefault(ShortcutId::kCursorUp,                          "PAD DPAD_UP");
    registerPadDefault(ShortcutId::kCursorDown,                        "PAD DPAD_DOWN");
    registerPadDefault(ShortcutId::kCursorLeft,                        "PAD DPAD_LEFT");
    registerPadDefault(ShortcutId::kCursorRight,                       "PAD DPAD_RIGHT");
    // OPENRCT2MINI focus-mode-plan §F.9: D-pad defaults for focus
    // direction. Overlaps with cursor.* on purpose (both fire on
    // D-pad press) so first-launch behaviour matches what users
    // saw before this split. The two ids are separate in the
    // rebind UI, so users can drop one without affecting the
    // other.
    registerPadDefault(ShortcutId::kFocusUp,                           "PAD DPAD_UP");
    registerPadDefault(ShortcutId::kFocusDown,                         "PAD DPAD_DOWN");
    registerPadDefault(ShortcutId::kFocusLeft,                         "PAD DPAD_LEFT");
    registerPadDefault(ShortcutId::kFocusRight,                        "PAD DPAD_RIGHT");
    // OPENRCT2MINI gamepad-plan 1.9 follow-on: also bind the left
    // stick to cursor motion. Each cursor.* shortcut now has BOTH
    // a digital DPAD binding AND an analog STICK_L axis binding —
    // ProcessWorldCursor's digital block handles the dpad, and its
    // new analog block handles the stick via getAnalogState. User
    // can rebind either side via the Input Bindings window.
    registerPadDefault(ShortcutId::kCursorUp,                          "PAD STICK_L UP");
    registerPadDefault(ShortcutId::kCursorDown,                        "PAD STICK_L DOWN");
    registerPadDefault(ShortcutId::kCursorLeft,                        "PAD STICK_L LEFT");
    registerPadDefault(ShortcutId::kCursorRight,                       "PAD STICK_L RIGHT");
    registerPadDefault(ShortcutId::kCursorClick,                       "PAD A");
    registerPadDefault(ShortcutId::kCursorCancel,                      "PAD B");
    // OPENRCT2MINI mouse-input refactor: gamepad camera drag default.
    // PAD B is also bound to cursor.cancel (right-click). Holding it
    // over the viewport pans the camera via the kInterfaceCameraDrag
    // poll; releasing within ~500ms additionally fires the
    // context-sensitive right-click action (delete tile element).
    registerPadDefault(ShortcutId::kInterfaceCameraDrag,               "PAD B");
    // OPENRCT2MINI gamepad-plan 1.9 follow-on: bind the right stick
    // to view-scroll. The kViewScroll* shortcuts already exist with
    // arrow-key keyboard defaults; PAD STICK_R bindings layer on
    // alongside so the right stick pans the camera through the
    // bindable shortcut system rather than the previous hardcoded
    // SDL_CONTROLLER_AXIS_RIGHTX/Y poll. processAnalogueInput reads
    // these via getAnalogState.
    registerPadDefault(ShortcutId::kViewScrollUp,                      "PAD STICK_R UP");
    registerPadDefault(ShortcutId::kViewScrollDown,                    "PAD STICK_R DOWN");
    registerPadDefault(ShortcutId::kViewScrollLeft,                    "PAD STICK_R LEFT");
    registerPadDefault(ShortcutId::kViewScrollRight,                   "PAD STICK_R RIGHT");
    // OPENRCT2MINI gamepad-plan 1.6b step 0: PAD R1 default for fast-
    // cursor modifier. kViewGeneralRotateClockwise also defaults to
    // PAD R1 — that's intentional per gamepad-plan §1.4. They live in
    // different shortcut buckets and the dispatcher fires them via
    // independent paths: rotate-cw is a one-shot action lambda on
    // press transition, fast-modifier is a per-frame held-state poll.
    // Pressing R1 once rotates the view AND briefly speeds the cursor
    // — pretty much imperceptible on a tap. Holding R1 keeps the
    // cursor fast (the design intent) and the rotate-cw action fires
    // exactly once on the press edge. No double-fire issue.
    //
    // This binding replaces the legacy
    // SDL_GameControllerGetButton(BUTTON_RIGHTSHOULDER) direct poll in
    // UiContext::ProcessVirtualGamepadCursor that 1.6b step 0 deletes.
    // Without this default, host gamepad users would silently lose
    // fast-cursor when the direct poll goes away (the device path is
    // unaffected — vendor SDL2 emits LALT scancode for R1, picked up
    // via the kCursorFastModifier keyboard default at line ~1031).
    registerPadDefault(ShortcutId::kCursorFastModifier,                "PAD R1");
    // kInterfaceConstructionZLock: same reasoning, no PAD default —
    // most physical buttons that make sense here already have view
    // bindings. 1.5d wires the held-state query and the user can
    // rebind to whatever they prefer (likely L1+R1 chord).
    registerPadDefault(ShortcutId::kInterfaceShadeWindowUnderCursor,   "PAD Y");
    // OPENRCT2MINI hold-binding refactor: shade-all is the hold-
    // variant of the same physical button as shade-window. The
    // dispatcher's _holdPending mechanism coordinates the two: a
    // tap of PAD Y / C fires shade-window (the bindings on
    // kInterfaceShadeWindowUnderCursor have holdMs == 0); holding
    // for 500 ms fires shade-all (these HOLD bindings have
    // holdMs == 500 via the "HOLD " parser prefix).
    registerPadDefault(ShortcutId::kInterfaceToggleShadeAllWindows,    "HOLD PAD Y");
    registerKeyboardDefault(ShortcutId::kInterfaceToggleShadeAllWindows, "HOLD C");
    registerPadDefault(ShortcutId::kInterfaceCloseWindowUnderCursor,   "PAD R1+Y");

    // OPENRCT2MINI gamepad-plan 1.6c.1: dismiss / confirm PAD defaults.
    // BACK = the small side button (Mini SELECT, DS4 Share, Xbox View);
    // START = the small centre-right button (Mini START, DS4 Options,
    // Xbox Menu). Mirrors the SNES / Switch convention where Select
    // backs out and Start commits or pauses. Pause itself defaults to
    // PAD START (line 1067) — same physical button. Inside a modal the
    // 1.6c dispatch fires Confirm; outside any modal it falls through
    // to kInterfacePause's action lambda. No conflict.
    registerPadDefault(ShortcutId::kInterfaceDismiss,                  "PAD BACK");
    registerPadDefault(ShortcutId::kInterfaceConfirm,                  "PAD START");
}
