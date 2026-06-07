/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ViewportInteraction.h"

#include "../ProvisionalElements.h"
#include "../UiStringIds.h"
#include "../windows/Windows.h"
#include "Viewport.h"
#include "Window.h"

#include <openrct2/Context.h>
#include <openrct2/Editor.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/footpath/FootpathAdditionRemoveAction.h>
#include <openrct2/actions/footpath/FootpathRemoveAction.h>
#include <openrct2/actions/general/BalloonPressAction.h>
#include <openrct2/actions/park/ParkEntranceRemoveAction.h>
#include <openrct2/actions/scenery/LargeSceneryRemoveAction.h>
#include <openrct2/actions/scenery/SmallSceneryRemoveAction.h>
#include <openrct2/actions/scenery/WallRemoveAction.h>
#include <openrct2/entity/Balloon.h>
#include <openrct2/entity/Duck.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/object/BannerSceneryEntry.h>
#include <openrct2/object/LargeSceneryEntry.h>
#include <openrct2/object/ObjectEntryManager.h>
#include <openrct2/object/PathAdditionEntry.h>
#include <openrct2/object/SmallSceneryEntry.h>
#include <openrct2/object/WallSceneryEntry.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideConstruction.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/Track.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <openrct2/world/Banner.h>
#include <openrct2/world/Footpath.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/Scenery.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/world/tile_element/BannerElement.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/LargeSceneryElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/WallElement.h>

#include <array>
#include <optional>

using namespace OpenRCT2;
using namespace OpenRCT2::Ui::Windows;

namespace OpenRCT2::Ui
{
    static void ViewportInteractionRemoveScenery(const SmallSceneryElement& smallSceneryElement, const CoordsXY& mapCoords);
    static void ViewportInteractionRemoveFootpath(const PathElement& pathElement, const CoordsXY& mapCoords);
    static void ViewportInteractionRemovePathAddition(const PathElement& pathElement, const CoordsXY& mapCoords);
    static void ViewportInteractionRemoveParkWall(const WallElement& wallElement, const CoordsXY& mapCoords);
    static void ViewportInteractionRemoveLargeScenery(
        const LargeSceneryElement& largeSceneryElement, const CoordsXY& mapCoords);
    static void ViewportInteractionRemoveParkEntrance(const EntranceElement& entranceElement, CoordsXY mapCoords);
    static Peep* ViewportInteractionGetClosestPeep(ScreenCoordsXY screenCoords, int32_t maxDistance);

    /**
     *
     *  rct2: 0x006ED9D0
     */
    static InteractionInfo ViewportInteractionGetItemLeft(const ScreenCoordsXY& screenCoords)
    {
        InteractionInfo info{};
        // No click input for scenario editor or track manager
        if (gLegacyScene == LegacyScene::scenarioEditor || gLegacyScene == LegacyScene::trackDesignsManager)
            return info;

        //
        if (gLegacyScene == LegacyScene::trackDesigner && getGameState().editorStep != EditorStep::rollerCoasterDesigner)
            return info;

        info = GetMapCoordinatesFromPos(
            screenCoords,
            EnumsToFlags(
                ViewportInteractionItem::entity, ViewportInteractionItem::ride, ViewportInteractionItem::parkEntrance));
        auto tileElement = info.interactionType != ViewportInteractionItem::entity ? info.Element : nullptr;
        // Only valid when info.interactionType == ViewportInteractionItem::entity, but can't assign nullptr without compiler
        // complaining
        auto sprite = info.Entity;

        // Allows only balloons to be popped and ducks to be quacked in title screen
        if (gLegacyScene == LegacyScene::titleSequence)
        {
            if (info.interactionType == ViewportInteractionItem::entity && (sprite->is<Balloon>() || sprite->is<Duck>()))
                return info;

            info.interactionType = ViewportInteractionItem::none;
            return info;
        }

        switch (info.interactionType)
        {
            case ViewportInteractionItem::entity:
                switch (sprite->type)
                {
                    case EntityType::vehicle:
                    {
                        auto vehicle = sprite->as<Vehicle>();
                        if (vehicle != nullptr && !vehicle->IsCableLift())
                            vehicle->SetMapToolbar();
                        else
                            info.interactionType = ViewportInteractionItem::none;
                    }
                    break;
                    case EntityType::guest:
                    case EntityType::staff:
                    {
                        auto peep = sprite->as<Peep>();
                        if (peep != nullptr)
                        {
                            PeepSetMapTooltip(peep);
                        }
                        else
                        {
                            info.interactionType = ViewportInteractionItem::none;
                        }
                    }
                    break;
                    default:
                        break;
                }
                break;
            case ViewportInteractionItem::ride:
                Guard::ArgumentNotNull(tileElement);
                RideSetMapTooltip(*tileElement);
                break;
            case ViewportInteractionItem::parkEntrance:
            {
                auto& gameState = getGameState();
                auto parkName = gameState.park.name.c_str();

                auto ft = Formatter();
                ft.Add<StringId>(STR_STRING);
                ft.Add<const char*>(parkName);
                SetMapTooltip(ft);
                break;
            }
            default:
                info.interactionType = ViewportInteractionItem::none;
                break;
        }

        // If nothing is under cursor, find a close by peep
        if (info.interactionType == ViewportInteractionItem::none)
        {
            auto peep = ViewportInteractionGetClosestPeep(screenCoords, 32);
            if (peep != nullptr)
            {
                info.Entity = peep;
                info.interactionType = ViewportInteractionItem::entity;
                info.Loc.x = peep->x;
                info.Loc.y = peep->y;
                PeepSetMapTooltip(peep);
            }
        }

        return info;
    }

    bool ViewportInteractionLeftOver(const ScreenCoordsXY& screenCoords)
    {
        auto info = ViewportInteractionGetItemLeft(screenCoords);

        switch (info.interactionType)
        {
            case ViewportInteractionItem::entity:
            case ViewportInteractionItem::ride:
            case ViewportInteractionItem::parkEntrance:
                return true;
            default:
                return false;
        }
    }

    bool ViewportInteractionLeftClick(const ScreenCoordsXY& screenCoords)
    {
        auto info = ViewportInteractionGetItemLeft(screenCoords);
        auto& gameState = getGameState();

        switch (info.interactionType)
        {
            case ViewportInteractionItem::entity:
            {
                auto entity = info.Entity;
                switch (entity->type)
                {
                    case EntityType::vehicle:
                    {
                        auto intent = Intent(WindowDetail::vehicle);
                        intent.PutExtra(INTENT_EXTRA_VEHICLE, entity);
                        ContextOpenIntent(&intent);
                        break;
                    }
                    case EntityType::guest:
                    case EntityType::staff:
                    {
                        auto intent = Intent(WindowClass::peep);
                        intent.PutExtra(INTENT_EXTRA_PEEP, entity);
                        ContextOpenIntent(&intent);
                        break;
                    }
                    case EntityType::balloon:
                    {
                        if (GameIsNotPaused())
                        {
                            auto balloonPress = GameActions::BalloonPressAction(entity->id);
                            GameActions::Execute(&balloonPress, gameState);
                        }
                    }
                    break;
                    case EntityType::duck:
                    {
                        if (GameIsNotPaused())
                        {
                            auto duck = entity->as<Duck>();
                            if (duck != nullptr)
                            {
                                duck->Press();
                            }
                        }
                    }
                    break;
                    default:
                        break;
                }
                return true;
            }
            case ViewportInteractionItem::ride:
            {
                auto intent = Intent(WindowDetail::track);
                intent.PutExtra(INTENT_EXTRA_TILE_ELEMENT, info.Element);
                ContextOpenIntent(&intent);
                return true;
            }
            case ViewportInteractionItem::parkEntrance:
                ContextOpenWindow(WindowClass::parkInformation);
                return true;
            default:
                return false;
        }
    }

    /**
     *
     *  rct2: 0x006EDE88
     */
    static InteractionInfo ViewportInteractionGetItemRight(const ScreenCoordsXY& screenCoords)
    {
        Ride* ride;
        InteractionInfo info{};
        // No click input for title screen or track manager
        if (gLegacyScene == LegacyScene::titleSequence || gLegacyScene == LegacyScene::trackDesignsManager)
            return info;

        //
        if (gLegacyScene == LegacyScene::trackDesigner && getGameState().editorStep != EditorStep::rollerCoasterDesigner)
            return info;

        constexpr auto flags = static_cast<int32_t>(
            ~EnumsToFlags(ViewportInteractionItem::terrain, ViewportInteractionItem::water));
        info = GetMapCoordinatesFromPos(screenCoords, flags);
        auto tileElement = info.Element;

        switch (info.interactionType)
        {
            case ViewportInteractionItem::entity:
            {
                auto sprite = info.Entity;
                if (gLegacyScene == LegacyScene::scenarioEditor || sprite->type != EntityType::vehicle)
                {
                    info.interactionType = ViewportInteractionItem::none;
                    return info;
                }

                auto vehicle = sprite->as<Vehicle>();
                if (vehicle == nullptr)
                {
                    info.interactionType = ViewportInteractionItem::none;
                    return info;
                }
                ride = GetRide(vehicle->ride);
                if (ride != nullptr && ride->status == RideStatus::closed)
                {
                    auto ft = Formatter();
                    ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_MODIFY);
                    ride->formatNameTo(ft);
                    SetMapTooltip(ft);
                }
                return info;
            }
            case ViewportInteractionItem::ride:
            {
                if (gLegacyScene == LegacyScene::scenarioEditor)
                {
                    info.interactionType = ViewportInteractionItem::none;
                    return info;
                }
                if (tileElement->getType() == TileElementType::Path)
                {
                    info.interactionType = ViewportInteractionItem::none;
                    return info;
                }

                ride = GetRide(tileElement->GetRideIndex());
                if (ride == nullptr)
                {
                    info.interactionType = ViewportInteractionItem::none;
                    return info;
                }

                if (ride->status != RideStatus::closed)
                    return info;

                auto ft = Formatter();
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_MODIFY);

                if (tileElement->getType() == TileElementType::Entrance)
                {
                    StringId stringId;
                    if (tileElement->asEntrance()->GetEntranceType() == ENTRANCE_TYPE_RIDE_ENTRANCE)
                    {
                        if (ride->numStations > 1)
                        {
                            stringId = STR_RIDE_STATION_X_ENTRANCE;
                        }
                        else
                        {
                            stringId = STR_RIDE_ENTRANCE;
                        }
                    }
                    else
                    {
                        if (ride->numStations > 1)
                        {
                            stringId = STR_RIDE_STATION_X_EXIT;
                        }
                        else
                        {
                            stringId = STR_RIDE_EXIT;
                        }
                    }
                    ft.Add<StringId>(stringId);
                }
                else if (tileElement->asTrack()->IsStation())
                {
                    StringId stringId;
                    if (ride->numStations > 1)
                    {
                        stringId = STR_RIDE_STATION_X;
                    }
                    else
                    {
                        stringId = STR_RIDE_STATION;
                    }
                    ft.Add<StringId>(stringId);
                }
                else
                {
                    // FIXME: Why does it *2 the value?
                    if (!getGameState().cheats.sandboxMode && !MapIsLocationOwned({ info.Loc, tileElement->getBaseZ() * 2 }))
                    {
                        info.interactionType = ViewportInteractionItem::none;
                        return info;
                    }

                    ride->formatNameTo(ft);
                    return info;
                }

                ride->formatNameTo(ft);

                const auto& rtd = ride->getRideTypeDescriptor();
                ft.Add<StringId>(GetRideComponentName(rtd.NameConvention.station).capitalised);

                StationIndex::UnderlyingType stationIndex;
                if (tileElement->getType() == TileElementType::Entrance)
                    stationIndex = tileElement->asEntrance()->GetStationIndex().ToUnderlying();
                else
                    stationIndex = tileElement->asTrack()->GetStationIndex().ToUnderlying();

                for (int32_t i = stationIndex; i >= 0; i--)
                    if (ride->getStations()[i].Start.IsNull())
                        stationIndex--;
                stationIndex++;
                ft.Add<uint16_t>(stationIndex);
                SetMapTooltip(ft);
                return info;
            }
            case ViewportInteractionItem::wall:
            {
                auto* wallEntry = tileElement->asWall()->GetEntry();
                if (wallEntry->scrolling_mode != kScrollingModeNone)
                {
                    auto banner = tileElement->asWall()->GetBanner();
                    if (banner != nullptr)
                    {
                        auto ft = Formatter();
                        ft.Add<StringId>(STR_MAP_TOOLTIP_BANNER_STRINGID_STRINGID);
                        banner->formatTextTo(ft);
                        ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_MODIFY);
                        ft.Add<StringId>(wallEntry->name);
                        SetMapTooltip(ft);
                        return info;
                    }
                }
                break;
            }
            case ViewportInteractionItem::largeScenery:
            {
                auto* sceneryEntry = tileElement->asLargeScenery()->GetEntry();
                if (sceneryEntry->scrolling_mode != kScrollingModeNone)
                {
                    auto banner = tileElement->asLargeScenery()->GetBanner();
                    if (banner != nullptr)
                    {
                        auto ft = Formatter();
                        ft.Add<StringId>(STR_MAP_TOOLTIP_BANNER_STRINGID_STRINGID);
                        banner->formatTextTo(ft);
                        ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_MODIFY);
                        ft.Add<StringId>(sceneryEntry->name);
                        SetMapTooltip(ft);
                        return info;
                    }
                }
                break;
            }
            case ViewportInteractionItem::banner:
            {
                auto banner = tileElement->asBanner()->GetBanner();
                if (banner != nullptr)
                {
                    auto* bannerEntry = ObjectEntryManager::GetObjectEntry<BannerSceneryEntry>(banner->type);

                    auto ft = Formatter();
                    ft.Add<StringId>(STR_MAP_TOOLTIP_BANNER_STRINGID_STRINGID);
                    banner->formatTextWithColourTo(ft);
                    ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_MODIFY);
                    ft.Add<StringId>(bannerEntry->name);
                    SetMapTooltip(ft);
                    return info;
                }
                break;
            }
            default:
                break;
        }

        if (!gInputFlags.has(InputFlag::allowRightMouseRemoval) || !gInputFlags.has(InputFlag::toolActive))
        {
            auto* windowMgr = GetWindowManager();
            if (windowMgr->FindByClass(WindowClass::rideConstruction) == nullptr
                && windowMgr->FindByClass(WindowClass::footpath) == nullptr)
            {
                info.interactionType = ViewportInteractionItem::none;
                return info;
            }
        }

        auto ft = Formatter();
        switch (info.interactionType)
        {
            case ViewportInteractionItem::scenery:
            {
                auto* sceneryEntry = tileElement->asSmallScenery()->GetEntry();
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                ft.Add<StringId>(sceneryEntry->name);
                SetMapTooltip(ft);
                return info;
            }
            case ViewportInteractionItem::footpath:
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                if (tileElement->asPath()->IsQueue())
                    ft.Add<StringId>(STR_QUEUE_LINE_MAP_TIP);
                else
                    ft.Add<StringId>(STR_FOOTPATH_MAP_TIP);
                SetMapTooltip(ft);
                return info;

            case ViewportInteractionItem::pathAddition:
            {
                auto* pathAddEntry = tileElement->asPath()->GetAdditionEntry();
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                if (tileElement->asPath()->IsBroken())
                {
                    ft.Add<StringId>(STR_BROKEN);
                }
                ft.Add<StringId>(pathAddEntry != nullptr ? pathAddEntry->name : kStringIdNone);
                SetMapTooltip(ft);
                return info;
            }
            case ViewportInteractionItem::parkEntrance:
                if (gLegacyScene != LegacyScene::scenarioEditor && !getGameState().cheats.sandboxMode)
                    break;

                if (tileElement->getType() != TileElementType::Entrance)
                    break;

                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                ft.Add<StringId>(STR_OBJECT_SELECTION_PARK_ENTRANCE);
                SetMapTooltip(ft);
                return info;

            case ViewportInteractionItem::wall:
            {
                auto* wallEntry = tileElement->asWall()->GetEntry();
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                ft.Add<StringId>(wallEntry->name);
                SetMapTooltip(ft);
                return info;
            }
            case ViewportInteractionItem::largeScenery:
            {
                auto* sceneryEntry = tileElement->asLargeScenery()->GetEntry();
                ft.Add<StringId>(STR_MAP_TOOLTIP_STRINGID_CLICK_TO_REMOVE);
                ft.Add<StringId>(sceneryEntry->name);
                SetMapTooltip(ft);
                return info;
            }
            default:
                break;
        }

        info.interactionType = ViewportInteractionItem::none;
        return info;
    }

    bool ViewportInteractionRightOver(const ScreenCoordsXY& screenCoords)
    {
        auto info = ViewportInteractionGetItemRight(screenCoords);

        return info.interactionType != ViewportInteractionItem::none;
    }

    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.2 (Phase A):
    // pure dispatch on a resolved InteractionInfo. The screen-coord
    // entry (ViewportInteractionRightClick) and the map-coord entry
    // (ViewportInteractionRightClickAtMapPos) both feed their info
    // through this switch — keeps the per-element-type semantics
    // (delete footpath / open ride / open banner detail / etc.) in
    // one place regardless of how the cursor's tile was discovered.
    static bool ViewportInteractionDispatchRightClick(const InteractionInfo& info)
    {
        CoordsXYE tileElement;
        switch (info.interactionType)
        {
            case ViewportInteractionItem::none:
            case ViewportInteractionItem::terrain:
            case ViewportInteractionItem::water:
            case ViewportInteractionItem::label:
                return false;

            case ViewportInteractionItem::entity:
            {
                auto entity = info.Entity;
                if (entity->type == EntityType::vehicle)
                {
                    auto vehicle = entity->as<Vehicle>();
                    if (vehicle == nullptr)
                    {
                        break;
                    }
                    auto ride = GetRide(vehicle->ride);
                    if (ride != nullptr)
                    {
                        RideConstructionStart(*ride);
                    }
                }
            }
            break;
            case ViewportInteractionItem::ride:
                tileElement = { info.Loc, info.Element };
                RideModify(tileElement);
                break;
            case ViewportInteractionItem::scenery:
                ViewportInteractionRemoveScenery(*info.Element->asSmallScenery(), info.Loc);
                break;
            case ViewportInteractionItem::footpath:
                ViewportInteractionRemoveFootpath(*info.Element->asPath(), info.Loc);
                break;
            case ViewportInteractionItem::pathAddition:
                ViewportInteractionRemovePathAddition(*info.Element->asPath(), info.Loc);
                break;
            case ViewportInteractionItem::parkEntrance:
                ViewportInteractionRemoveParkEntrance(*info.Element->asEntrance(), info.Loc);
                break;
            case ViewportInteractionItem::wall:
                ViewportInteractionRemoveParkWall(*info.Element->asWall(), info.Loc);
                break;
            case ViewportInteractionItem::largeScenery:
                ViewportInteractionRemoveLargeScenery(*info.Element->asLargeScenery(), info.Loc);
                break;
            case ViewportInteractionItem::banner:
                ContextOpenDetailWindow(WindowDetail::banner, info.Element->asBanner()->GetIndex().ToUnderlying());
                break;
        }

        return true;
    }

    /**
     *
     *  rct2: 0x006E8A62
     */
    bool ViewportInteractionRightClick(const ScreenCoordsXY& screenCoords)
    {
        return ViewportInteractionDispatchRightClick(ViewportInteractionGetItemRight(screenCoords));
    }

    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.2 (Phase A):
    // tile-coord entry. Reprojects the tile's centre through the
    // main window's viewport to a screen pixel position, then
    // delegates to the screen-coord path so the existing
    // GetMapCoordinatesFromPos / allowRightMouseRemoval / priority
    // logic is reused verbatim (§7.1 — projection inverse of
    // Viewport::ScreenToViewportCoord at Viewport.cpp:1114).
    //
    // OPENRCT2MINI grid-cursor-deletion-plan §3.6 (2026-05-20):
    // superseded by ViewportInteractionRightClickAtGridCursor for
    // the tool-context onCancel path — the screen-pixel reprojection
    // here couldn't address sub-tile sceneries deterministically.
    // No current consumers; kept as a parking spot for future
    // map-pos right-click flows.
    bool ViewportInteractionRightClickAtMapPos(const CoordsXY& mapCoords)
    {
        const auto screen = ViewportInteractionMapToScreen(mapCoords);
        if (!screen.has_value())
            return false;
        return ViewportInteractionRightClick(*screen);
    }

    // OPENRCT2MINI grid-cursor-deletion-plan §3.1 (2026-05-20):
    // priority-band classifier for the grid-cursor right-click
    // dispatcher. Lower band number = higher priority (acted on
    // first). The ordering is:
    //
    //   0  sub-tile decoration  (SmallScenery, !occupiesFullTile)
    //   1  full-tile decoration (SmallScenery,  occupiesFullTile)
    //   2  large scenery         (LargeScenery)
    //   3  wall                  (Wall)
    //   4  banner                (Banner)            — opens detail
    //   5  path addition         (Path w/ addition)  — clears addition
    //   6  path                  (Path)              — removes path
    //   7  park entrance         (Entrance, park)    — opens park info
    //   8  ride / track / ride entrance & exit       — opens ride ctor
    //
    // Surface returns -1 (never actionable through this path). The
    // band order encodes the user's expectation: strip decoration
    // off first, work down to infrastructure last.
    static int classifyGridCursorPriority(const TileElement& el)
    {
        switch (el.getType())
        {
            case TileElementType::SmallScenery:
            {
                const auto* entry = el.asSmallScenery()->GetEntry();
                if (entry == nullptr)
                    return 0;  // unknown entry — treat as sub-tile
                return entry->flags.has(SmallSceneryFlag::occupiesFullTile) ? 1 : 0;
            }
            case TileElementType::LargeScenery:
                return 2;
            case TileElementType::Wall:
                return 3;
            case TileElementType::Banner:
                return 4;
            case TileElementType::Path:
                return el.asPath()->HasAddition() ? 5 : 6;
            case TileElementType::Entrance:
                // Park entrance and ride entrance/exit share the
                // same TileElementType — disambiguate by sub-type.
                // Park entrance opens the park information window
                // (§6.2); ride entrance / exit opens ride
                // construction.
                return el.asEntrance()->GetEntranceType() == ENTRANCE_TYPE_PARK_ENTRANCE ? 7 : 8;
            case TileElementType::Track:
                return 8;
            case TileElementType::Surface:
            default:
                return -1;
        }
    }

    // OPENRCT2MINI grid-cursor-deletion-plan §3.5 (Phase B,
    // 2026-05-20): per-element dispatch for the grid-cursor right-
    // click. Switches on the element's classified priority band
    // and calls the matching delete helper / window opener:
    //
    //   0,1 SmallScenery    → ViewportInteractionRemoveScenery
    //   2   LargeScenery    → ViewportInteractionRemoveLargeScenery
    //                         (the helper opens the sign-detail
    //                         window for scrolling-banner large
    //                         scenery instead — matches mouse path)
    //   3   Wall            → ViewportInteractionRemoveParkWall
    //                         (opens sign window for scrolling-
    //                         banner walls, delete otherwise —
    //                         matches mouse path)
    //   4   Banner          → ContextOpenDetailWindow(banner)
    //   5   Path addition   → ViewportInteractionRemovePathAddition
    //                         (clears addition only; path stays)
    //   6   Path            → ViewportInteractionRemoveFootpath
    //   7   Park entrance   → ContextOpenWindow(parkInformation)
    //                         — fork divergence from mouse path's
    //                         RemoveParkEntrance (§6.2)
    //   8   Ride / Track / ride entrance & exit → RideModify
    //                         (opens ride construction window)
    //
    // The classifier returns the band; we re-derive a few sub-
    // class distinctions (path-addition vs path bare; park entry
    // vs ride entry) directly here so we don't depend on the
    // classifier's exact int encoding.
    static bool dispatchGridCursorActionForElement(TileElement& el, const CoordsXY& tile)
    {
        switch (el.getType())
        {
            case TileElementType::SmallScenery:
                ViewportInteractionRemoveScenery(*el.asSmallScenery(), tile);
                return true;

            case TileElementType::LargeScenery:
                ViewportInteractionRemoveLargeScenery(*el.asLargeScenery(), tile);
                return true;

            case TileElementType::Wall:
                ViewportInteractionRemoveParkWall(*el.asWall(), tile);
                return true;

            case TileElementType::Banner:
                ContextOpenDetailWindow(WindowDetail::banner, el.asBanner()->GetIndex().ToUnderlying());
                return true;

            case TileElementType::Path:
                if (el.asPath()->HasAddition())
                    ViewportInteractionRemovePathAddition(*el.asPath(), tile);
                else
                    ViewportInteractionRemoveFootpath(*el.asPath(), tile);
                return true;

            case TileElementType::Entrance:
                if (el.asEntrance()->GetEntranceType() == ENTRANCE_TYPE_PARK_ENTRANCE)
                {
                    // §6.2 — grid cursor never deletes the park
                    // entrance; instead surface the park info
                    // window, mirroring the top-toolbar park-info
                    // button. Mouse RMB on a park entrance retains
                    // its upstream delete behaviour (unchanged).
                    ContextOpenWindow(WindowClass::parkInformation);
                }
                else
                {
                    // Ride entrance / exit — open ride construction.
                    CoordsXYE rideTileElement{ tile, &el };
                    RideModify(rideTileElement);
                }
                return true;

            case TileElementType::Track:
            {
                CoordsXYE rideTileElement{ tile, &el };
                RideModify(rideTileElement);
                return true;
            }

            case TileElementType::Surface:
            default:
                return false;
        }
    }

    // OPENRCT2MINI grid-cursor-deletion-plan §3.2 (2026-05-20):
    // grid-cursor right-click dispatcher. See header for full
    // contract. Walks the tile's element list (ascending Z, then
    // placement order at the same Z — guaranteed by TileElementInsert),
    // classifies each into one of nine priority bands, and acts on
    // the highest-priority candidate. Within a band the earliest-
    // in-list candidate wins (so lower-Z, then earlier-placed at
    // the same Z). Z-window filter is engaged only when the user
    // has raised the cursor's Z plane (cursorZ != surface Z); in
    // that mode elements outside ±kPathHeightStep of the cursor's
    // effective Z are rejected, so a lamp on a raised path is
    // addressable independently of a tree at surface Z.
    //
    bool ViewportInteractionRightClickAtGridCursor(const TileCoordsXY& tile, int32_t cursorZ)
    {
        constexpr int kPriorityBands = 9;
        struct Candidate
        {
            TileElement* el = nullptr;
            size_t order = 0;
        };
        std::array<std::optional<Candidate>, kPriorityBands> best{};

        // §3.4: the Z-window filter is engaged only when the user
        // has raised the cursor off the surface. At surface Z
        // (cursorZ == 0 when the caller has not added a Z offset)
        // every element on the tile is a candidate, matching the
        // mouse "click on the tile, act on whatever's there"
        // semantic.
        //
        // Caller passes the cursor's effective world Z (surface Z
        // + raised-plane offset). To decide whether the Z window is
        // active we compare to the tile's surface Z — if the cursor
        // sits exactly at surface Z the window is disabled.
        const auto centre = tile.ToCoordsXY() + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
        const int32_t surfaceZ = TileElementHeight(centre);
        const bool useZWindow = (cursorZ != surfaceZ);

        size_t order = 0;
        for (auto* el : TileElementsView<>(tile))
        {
            ++order;
            const int p = classifyGridCursorPriority(*el);
            if (p < 0)
                continue;
            if (useZWindow)
            {
                const int32_t elZ = el->getBaseZ();
                if (elZ < cursorZ - kPathHeightStep || elZ > cursorZ + kPathHeightStep)
                    continue;
            }
            // OPENRCT2MINI grid-cursor-deletion-plan §3.5 follow-up
            // (2026-06-03): walls delete TOP-DOWN. Tile element list
            // is in ascending Z; the default "earliest wins" picks
            // the lowest-Z wall, which feels unintuitive when the
            // user is standing at the top of a stack and presses
            // delete. Override for walls only: keep updating so we
            // end on the LAST wall in the list (highest Z + latest
            // placed at the same Z). Other bands keep first-wins.
            const bool isWall = el->getType() == TileElementType::Wall;
            if (!best[p].has_value() || isWall)
                best[p] = Candidate{ el, order };
            // else: earlier-in-list within band already chosen
        }

        for (auto& slot : best)
        {
            if (slot.has_value())
                return dispatchGridCursorActionForElement(*slot->el, tile.ToCoordsXY());
        }
        return false;
    }

    // OPENRCT2MINI cursor-sync (2026-05-17): standalone projection
    // helper (factored from ViewportInteractionRightClickAtMapPos).
    // Inverse of ScreenToViewportCoord: takes a world map XY,
    // resolves the tile-centre's surface height via TileElementHeight,
    // does the iso 3D-to-2D projection with the current viewport
    // rotation, then applies the inverse zoom to produce a screen
    // pixel coord. Returns nullopt if the main viewport doesn't
    // exist (pre-init, title scene without a main window).
    std::optional<ScreenCoordsXY> ViewportInteractionMapToScreen(const CoordsXY& mapCoords)
    {
        return ViewportInteractionMapToScreen(mapCoords, 0);
    }

    // OPENRCT2MINI grid-cursor-plan §14.2 polish 2 (2026-05-20): Z-
    // aware variant. zOffset (in world units, kPathHeightStep
    // multiples) is added on top of the resolved surface Z, so the
    // grid cursor's parked screen position rides up the iso
    // projection in lockstep with the raised placement Z. Used by
    // SyncHiddenCursorParking when the grid cursor is in raised-Z
    // mode so the software cursor sprite tracks the Z plane rather
    // than staying tethered to the surface.
    std::optional<ScreenCoordsXY> ViewportInteractionMapToScreen(const CoordsXY& mapCoords, int32_t zOffset)
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return std::nullopt;
        const auto& vp = *main->viewport;
        const auto centre = mapCoords + CoordsXY{ kCoordsXYHalfTile, kCoordsXYHalfTile };
        const int32_t z = TileElementHeight(centre) + zOffset;
        const auto world = Translate3DTo2DWithZ(vp.rotation, CoordsXYZ{ centre, z });
        return ScreenCoordsXY{
            vp.zoom.ApplyInversedTo(world.x - vp.viewPos.x) + vp.pos.x,
            vp.zoom.ApplyInversedTo(world.y - vp.viewPos.y) + vp.pos.y,
        };
    }

    /**
     *
     *  rct2: 0x006E08D2
     */
    static void ViewportInteractionRemoveScenery(const SmallSceneryElement& smallSceneryElement, const CoordsXY& mapCoords)
    {
        auto removeSceneryAction = GameActions::SmallSceneryRemoveAction(
            { mapCoords.x, mapCoords.y, smallSceneryElement.getBaseZ() }, smallSceneryElement.GetSceneryQuadrant(),
            smallSceneryElement.GetEntryIndex());

        GameActions::Execute(&removeSceneryAction, getGameState());
    }

    /**
     *
     *  rct2: 0x006A614A
     */
    static void ViewportInteractionRemoveFootpath(const PathElement& pathElement, const CoordsXY& mapCoords)
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::footpath);
        if (w != nullptr)
            FootpathUpdateProvisional();

        TileElement* tileElement2 = MapGetFirstElementAt(mapCoords);
        if (tileElement2 == nullptr)
            return;

        auto z = pathElement.getBaseZ();
        do
        {
            if (tileElement2->getType() == TileElementType::Path && tileElement2->getBaseZ() == z)
            {
                auto action = GameActions::FootpathRemoveAction({ mapCoords, z });
                GameActions::Execute(&action, getGameState());
                break;
            }
        } while (!(tileElement2++)->isLastForTile());
    }

    /**
     *
     *  rct2: 0x006A61AB
     */
    static void ViewportInteractionRemovePathAddition(const PathElement& pathElement, const CoordsXY& mapCoords)
    {
        auto footpathAdditionRemoveAction = GameActions::FootpathAdditionRemoveAction(
            { mapCoords.x, mapCoords.y, pathElement.getBaseZ() });
        GameActions::Execute(&footpathAdditionRemoveAction, getGameState());
    }

    /**
     *
     *  rct2: 0x00666C0E
     */
    void ViewportInteractionRemoveParkEntrance(const EntranceElement& entranceElement, CoordsXY mapCoords)
    {
        int32_t rotation = entranceElement.getDirectionWithOffset(1);
        switch (entranceElement.GetSequenceIndex())
        {
            case 1:
                mapCoords += CoordsDirectionDelta[rotation];
                break;
            case 2:
                mapCoords -= CoordsDirectionDelta[rotation];
                break;
        }
        auto parkEntranceRemoveAction = GameActions::ParkEntranceRemoveAction(
            { mapCoords.x, mapCoords.y, entranceElement.getBaseZ() });
        GameActions::Execute(&parkEntranceRemoveAction, getGameState());
    }

    /**
     *
     *  rct2: 0x006E57A9
     */
    static void ViewportInteractionRemoveParkWall(const WallElement& wallElement, const CoordsXY& mapCoords)
    {
        auto* wallEntry = wallElement.GetEntry();
        if (wallEntry->scrolling_mode != kScrollingModeNone)
        {
            ContextOpenDetailWindow(WindowDetail::signSmall, wallElement.GetBannerIndex().ToUnderlying());
        }
        else
        {
            CoordsXYZD wallLocation = { mapCoords.x, mapCoords.y, wallElement.getBaseZ(), wallElement.getDirection() };
            auto wallRemoveAction = GameActions::WallRemoveAction(wallLocation);
            GameActions::Execute(&wallRemoveAction, getGameState());
        }
    }

    /**
     *
     *  rct2: 0x006B88DC
     */
    static void ViewportInteractionRemoveLargeScenery(const LargeSceneryElement& largeSceneryElement, const CoordsXY& mapCoords)
    {
        auto* sceneryEntry = largeSceneryElement.GetEntry();

        if (sceneryEntry->scrolling_mode != kScrollingModeNone)
        {
            auto bannerIndex = largeSceneryElement.GetBannerIndex();
            ContextOpenDetailWindow(WindowDetail::sign, bannerIndex.ToUnderlying());
        }
        else
        {
            auto removeSceneryAction = GameActions::LargeSceneryRemoveAction(
                { mapCoords.x, mapCoords.y, largeSceneryElement.getBaseZ(), largeSceneryElement.getDirection() },
                largeSceneryElement.GetSequenceIndex());
            GameActions::Execute(&removeSceneryAction, getGameState());
        }
    }

    struct PeepDistance
    {
        Peep* peep = nullptr;
        int32_t distance = std::numeric_limits<decltype(distance)>::max();
    };

    template<typename T>
    static PeepDistance GetClosestPeep(
        const ScreenCoordsXY& viewportCoords, uint8_t rotation, const int32_t maxDistance, PeepDistance goal)
    {
        for (auto peep : EntityList<T>())
        {
            if (peep->x == kLocationNull)
                continue;

            auto screenCoords = Translate3DTo2DWithZ(rotation, peep->getLocation());
            auto spriteRect = ScreenRect(
                screenCoords - ScreenCoordsXY{ peep->spriteData.width, peep->spriteData.heightMin },
                screenCoords + ScreenCoordsXY{ peep->spriteData.width, peep->spriteData.heightMax });

            auto distance = abs(((spriteRect.GetLeft() + spriteRect.GetRight()) / 2) - viewportCoords.x)
                + abs(((spriteRect.GetTop() + spriteRect.GetBottom()) / 2) - viewportCoords.y);
            if (distance > maxDistance)
                continue;

            if (distance < goal.distance)
            {
                goal.peep = peep;
                goal.distance = distance;
            }
        }
        return goal;
    }

    static Peep* ViewportInteractionGetClosestPeep(ScreenCoordsXY screenCoords, int32_t maxDistance)
    {
        auto* windowMgr = GetWindowManager();
        auto* w = windowMgr->FindFromPoint(screenCoords);
        if (w == nullptr)
            return nullptr;

        auto* viewport = w->viewport;
        if (viewport == nullptr || viewport->zoom >= ZoomLevel{ 2 })
            return nullptr;

        auto viewportCoords = viewport->ScreenToViewportCoord(screenCoords);

        PeepDistance goal;
        if (!(viewport->flags & VIEWPORT_FLAG_HIDE_GUESTS))
            goal = GetClosestPeep<Guest>(viewportCoords, viewport->rotation, maxDistance, goal);
        if (!(viewport->flags & VIEWPORT_FLAG_HIDE_STAFF))
            goal = GetClosestPeep<Staff>(viewportCoords, viewport->rotation, maxDistance, goal);
        return goal.peep;
    }

    /**
     *
     *  rct2: 0x0068A15E
     */
    CoordsXY ViewportInteractionGetTileStartAtCursor(const ScreenCoordsXY& screenCoords)
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* window = windowMgr->FindFromPoint(screenCoords);
        if (window == nullptr || window->viewport == nullptr)
        {
            CoordsXY ret{};
            ret.SetNull();
            return ret;
        }
        auto viewport = window->viewport;
        auto info = GetMapCoordinatesFromPosWindow(
            window, screenCoords, EnumsToFlags(ViewportInteractionItem::terrain, ViewportInteractionItem::water));
        auto initialPos = info.Loc;

        if (info.interactionType == ViewportInteractionItem::none)
        {
            initialPos.SetNull();
            return initialPos;
        }

        int16_t waterHeight = 0;
        if (info.interactionType == ViewportInteractionItem::water)
        {
            waterHeight = info.Element->asSurface()->GetWaterHeight();
        }

        auto initialVPPos = viewport->ScreenToViewportCoord(screenCoords);
        CoordsXY mapPos = initialPos + CoordsXY{ 16, 16 };

        for (int32_t i = 0; i < 5; i++)
        {
            int16_t z = waterHeight;
            if (info.interactionType != ViewportInteractionItem::water)
            {
                z = TileElementHeight(mapPos);
            }
            mapPos = ViewportPosToMapPos(initialVPPos, z, viewport->rotation);
            mapPos.x = std::clamp(mapPos.x, initialPos.x, initialPos.x + 31);
            mapPos.y = std::clamp(mapPos.y, initialPos.y, initialPos.y + 31);
        }

        return mapPos.ToTileStart();
    }
} // namespace OpenRCT2::Ui
