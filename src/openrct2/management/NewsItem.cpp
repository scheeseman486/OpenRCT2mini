/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "NewsItem.h"

#include "../Context.h"
#include "../Diagnostic.h"
#include "../GameState.h"
#include "../Input.h"
#include "../OpenRCT2.h"
#include "../audio/Audio.h"
#include "../config/Config.h"
#include "../entity/EntityRegistry.h"
#include "../entity/Peep.h"
#include "../haptic/HapticEvent.h"
#include "../haptic/LedEvent.h"
#include "../localisation/Formatter.h"
#include "../localisation/Formatting.h"
#include "../localisation/StringIds.h"
#include "../management/Award.h"
#include "../management/Research.h"
#include "../profiling/Profiling.h"
#include "../ride/Ride.h"
#include "../ride/Vehicle.h"
#include "../ui/WindowManager.h"
#include "../windows/Intent.h"
#include "../world/Location.hpp"
#include "../world/Map.h"

#include <cassert>

using namespace OpenRCT2;

News::Item& News::ItemQueues::current()
{
    return _recent.front();
}

const News::Item& News::ItemQueues::current() const
{
    return _recent.front();
}

bool News::IsValidIndex(int32_t index)
{
    if (index >= MaxItems)
    {
        LOG_ERROR("Tried to get news item past MAX_NEWS.");
        return false;
    }
    return true;
}

News::Item* News::GetItem(int32_t index)
{
    return getGameState().newsItems.at(index);
}

News::Item& News::ItemQueues::operator[](size_t index)
{
    return const_cast<Item&>(const_cast<const ItemQueues&>(*this)[index]);
}

const News::Item& News::ItemQueues::operator[](size_t index) const
{
    if (index < _recent.capacity())
        return _recent[index];

    return _archived[index - _recent.capacity()];
}

News::Item* News::ItemQueues::at(int32_t index)
{
    return const_cast<Item*>(const_cast<const ItemQueues&>(*this).at(index));
}

const News::Item* News::ItemQueues::at(int32_t index) const
{
    if (IsValidIndex(index))
    {
        return &(*this)[index];
    }

    return nullptr;
}

bool News::IsQueueEmpty()
{
    return getGameState().newsItems.isEmpty();
}

bool News::ItemQueues::isEmpty() const
{
    return _recent.empty();
}

/**
 *
 *  rct2: 0x0066DF32
 */
void News::ItemQueues::clear()
{
    _recent.clear();
    _archived.clear();
    // OPENRCT2MINI gamepad-plan 1.13: the queue is being wiped (new
    // park load, scenario reset, game shutdown). Force the lightbar
    // to off immediately rather than running a fade — the on-screen
    // ticker is also wiped synchronously, so a trailing fade would
    // outlive the visual cue it's anchored to.
    OpenRCT2::Led::forceClear();
}

void News::InitQueue(GameState_t& gameState)
{
    gameState.newsItems.clear();
    assert(gameState.newsItems.isEmpty());

    // Throttles for warning types (PEEP_*_WARNING)
    for (auto& warningThrottle : gameState.park.peepWarningThrottle)
    {
        warningThrottle = 0;
    }

    auto intent = Intent(INTENT_ACTION_INVALIDATE_TICKER_NEWS);
    ContextBroadcastIntent(&intent);
}

uint16_t News::ItemQueues::incrementTicks()
{
    return ++current().ticks;
}

static void TickCurrent()
{
    auto& current = getGameState().newsItems.current();
    int32_t ticks = getGameState().newsItems.incrementTicks();
    // Only play news item sound when in normal playing mode
    if (ticks == 1 && (gLegacyScene == LegacyScene::playing))
    {
        // Play sound
        OpenRCT2::Audio::Play(Audio::SoundId::newsItem, 0, ContextGetWidth() / 2);

        // OPENRCT2MINI gamepad-plan 1.13: first frame as current —
        // light the DualShock lightbar to the item's cached severity.
        // Held until ArchiveCurrent() retires the item, so the LED
        // tracks the on-screen ticker exactly (incl. game-speed
        // changes — Current().ticks counts game ticks, not realtime,
        // and so does the on-screen ticker). setActive(info) is a
        // no-op at the engine layer so unclassified items go dark.
        //
        // OPENRCT2MINI input-plan Track 2 §4.3: per-severity LED
        // gating. When a bucket is disabled the engine treats it like
        // an info-class item — no colour, but the news ticker still
        // shows (the gate only kills the haptic / LED feedback). Test
        // LED in the LED Options window calls setActive directly
        // without going through this gate so it stays usable.
        const auto& gpCfg = Config::Get().general;
        bool ledAllowed = true;
        switch (current.severity)
        {
            case News::Severity::critical: ledAllowed = gpCfg.gamepadLedOnCritical; break;
            case News::Severity::warning:  ledAllowed = gpCfg.gamepadLedOnWarning;  break;
            case News::Severity::money:    ledAllowed = gpCfg.gamepadLedOnMoney;    break;
            case News::Severity::info:     ledAllowed = false;                      break;
        }
        if (ledAllowed)
            OpenRCT2::Led::setActive(current.severity);

        // OPENRCT2MINI input-plan Track 2 §4.1: parallel rumble pulse
        // on critical news only. Matches the LED red flash duration
        // (200 ms / ~60% intensity). Lower buckets stay quiet — yellow
        // LED alone is enough for warning-class news, and rumble on
        // every queue-too-long ticker would be exhausting. Crash /
        // drowning / vandalism / 1-week-remaining / park-closure are
        // the four hits classified as critical (NewsItem.cpp::Get-
        // Severity StringId table).
        //
        // §4.3 gate: gamepadRumbleOnCriticalNews flips this off
        // independently of the master gamepadRumbleEnabled switch so
        // the user can keep crash / construction rumble while muting
        // the news pulse specifically.
        if (current.severity == News::Severity::critical && gpCfg.gamepadRumbleOnCriticalNews)
        {
            OpenRCT2::Haptic::pulse(0.6f, 200);
        }
    }
}

int32_t News::ItemQueues::removeTime() const
{
    if (!_recent[5].isEmpty() && !_recent[4].isEmpty() && !_recent[3].isEmpty() && !_recent[2].isEmpty())
    {
        return 256;
    }
    return 320;
}

bool News::ItemQueues::currentShouldBeArchived() const
{
    return current().ticks >= removeTime();
}

/**
 *
 *  rct2: 0x0066E252
 */
void News::UpdateCurrentItem()
{
    PROFILED_FUNCTION();

    auto& gameState = getGameState();
    // Check if there is a current news item
    if (gameState.newsItems.isEmpty())
        return;

    auto intent = Intent(INTENT_ACTION_INVALIDATE_TICKER_NEWS);
    ContextBroadcastIntent(&intent);

    // Update the current news item
    TickCurrent();

    // Removal of current news item
    if (gameState.newsItems.currentShouldBeArchived())
        gameState.newsItems.archiveCurrent();
}

/**
 *
 *  rct2: 0x0066E377
 */
void News::CloseCurrentItem()
{
    getGameState().newsItems.archiveCurrent();
}

void News::ItemQueues::archiveCurrent()
{
    // Check if there is a current message
    if (isEmpty())
        return;

    _archived.push_back(current());

    // Invalidate the news window
    auto* windowMgr = Ui::GetWindowManager();
    windowMgr->InvalidateByClass(WindowClass::recentNews);

    // Dequeue the current news item, shift news up
    _recent.pop_front();

    // Invalidate current news item bar
    auto intent = Intent(INTENT_ACTION_INVALIDATE_TICKER_NEWS);
    ContextBroadcastIntent(&intent);

    // OPENRCT2MINI gamepad-plan 1.13: the on-screen ticker is now
    // displaying either the next queued item (still at ticks=0 — its
    // LED will fire next UpdateCurrentItem call when TickCurrent
    // increments to 1) or nothing. Either way, clear the lightbar
    // here so the previous severity colour fades out cleanly during
    // the inter-item gap. If a next item is queued, its setActive()
    // on first tick will hot-replace whatever's left of this fade.
    OpenRCT2::Led::Clear();
}

/**
 * Get the (x,y,z) coordinates of the subject of a news item.
 * If the new item is no longer valid, return LOCATION_NULL in the x-coordinate
 *
 *  rct2: 0x0066BA74
 */
std::optional<CoordsXYZ> News::GetSubjectLocation(ItemType type, int32_t subject)
{
    std::optional<CoordsXYZ> subjectLoc{ std::nullopt };

    auto& gameState = getGameState();

    switch (type)
    {
        case ItemType::ride:
        {
            Ride* ride = GetRide(RideId::FromUnderlying(subject));
            if (ride == nullptr || ride->overallView.IsNull())
            {
                break;
            }
            auto rideViewCentre = ride->overallView.ToTileCentre();
            subjectLoc = CoordsXYZ{ rideViewCentre, TileElementHeight(rideViewCentre) };
            break;
        }
        case ItemType::peepOnRide:
        {
            auto peep = gameState.entities.TryGetEntity<Peep>(EntityId::FromUnderlying(subject));
            if (peep == nullptr)
                break;

            subjectLoc = peep->getLocation();
            if (subjectLoc->x != kLocationNull)
                break;

            if (peep->State != PeepState::onRide && peep->State != PeepState::enteringRide)
            {
                subjectLoc = std::nullopt;
                break;
            }

            // Find which ride peep is on
            Ride* ride = GetRide(peep->CurrentRide);
            if (ride == nullptr || !ride->flags.has(RideFlag::onTrack))
            {
                subjectLoc = std::nullopt;
                break;
            }

            // Find the first car of the train peep is on
            auto sprite = gameState.entities.TryGetEntity<Vehicle>(ride->vehicles[peep->CurrentTrain]);
            // Find the actual car peep is on
            for (int32_t i = 0; i < peep->CurrentCar && sprite != nullptr; i++)
            {
                sprite = gameState.entities.TryGetEntity<Vehicle>(sprite->next_vehicle_on_train);
            }
            if (sprite != nullptr)
            {
                subjectLoc = sprite->getLocation();
            }
            break;
        }
        case ItemType::peep:
        {
            auto peep = gameState.entities.TryGetEntity<Peep>(EntityId::FromUnderlying(subject));
            if (peep != nullptr)
            {
                subjectLoc = peep->getLocation();
            }
            break;
        }
        case ItemType::blank:
        {
            auto subjectUnsigned = static_cast<uint32_t>(subject);
            auto subjectXY = CoordsXY{ static_cast<int16_t>(subjectUnsigned & 0xFFFF),
                                       static_cast<int16_t>(subjectUnsigned >> 16) };
            if (!subjectXY.IsNull())
            {
                subjectLoc = CoordsXYZ{ subjectXY, TileElementHeight(subjectXY) };
            }
            break;
        }
        default:
            break;
    }
    return subjectLoc;
}

News::Item* News::ItemQueues::firstOpenOrNewSlot()
{
    for (auto emptySlots = _recent.capacity() - _recent.size(); emptySlots < 2; ++emptySlots)
    {
        archiveCurrent();
    }

    auto res = _recent.end();
    // The for loop above guarantees there is always an extra element to use
    assert(_recent.capacity() - _recent.size() >= 2);
    auto newsItem = res + 1;
    newsItem->type = ItemType::null;

    return &*res;
}

// OPENRCT2MINI gamepad-plan 1.12: classify a news item by severity.
// Used by the DualShock LED indicator (1.13) and the deferred
// critical-news rumble pulse hook from 1.11.
//
// Strategy is StringId-first, ItemType-fallback:
//   1) Explicit StringId table — every known critical/warning string
//      is matched here, regardless of which ItemType the producer
//      tagged it with. This matters because ItemType is chosen by
//      the producer for camera-pan / icon / clickability reasons
//      that don't correlate with severity. For example:
//        * Drowning news is fired as ItemType::blank (the location is
//          a raw x|y bitfield, not a peep id, so the news system
//          uses the no-subject "blank" variant for camera-pan).
//        * STR_PEEPS_COMPLAINING_ABOUT_QUEUE_LENGTH_WARNING is fired
//          as ItemType::ride (it opens the ride window on click)
//          despite "PEEPS" in the name.
//      Bucketing on ItemType alone misses both.
//
//   2) If the StringId isn't in the table, fall through to the
//      ItemType default:
//        * money / campaign → green
//        * award → AwardIsPositive(assoc) ? green : yellow
//        * everything else → info (no LED, no rumble)
//
// The table is intentionally permissive: any new StringId added by
// future contributors gets the safe info-default until someone
// explicitly classifies it. No need to remember to update the table
// when adding new news strings — silence is the right starting
// behaviour for an unknown event.
News::Severity News::GetSeverity(ItemType type, StringId stringId, uint32_t assoc)
{
    // ---- StringId-first table -------------------------------------------
    // Matches regardless of which ItemType bucket the producer used.
    switch (stringId)
    {
        // Critical — emergency requiring immediate attention.
        case STR_RIDE_HAS_CRASHED:               // ride
        case STR_X_PERSON_DIED_ON_X:             // ride
        case STR_X_PEOPLE_DIED_ON_X:             // ride
        case STR_NEWS_ITEM_GUEST_DROWNED:        // blank (NOT peep — uses
                                                 //   x|y bitfield assoc)
        case STR_PARK_HAS_BEEN_CLOSED_DOWN:      // peeps
        case STR_PARK_RATING_WARNING_1_WEEK_REMAINING: // peeps
        case STR_PEEPS_DISLIKE_VANDALISM:        // peeps
            return Severity::critical;

        // Warning — actionable problem the player should address.
        case STR_RIDE_IS_BROKEN_DOWN:            // ride
        case STR_RIDE_IS_STILL_NOT_FIXED:        // ride
        case STR_NEWS_VEHICLE_HAS_STALLED:       // ride
        case STR_ENTRANCE_NOT_CONNECTED:         // ride
        case STR_EXIT_NOT_CONNECTED:             // ride
        case STR_GUESTS_GETTING_STUCK_ON_RIDE:   // ride (post-edit issue)
        case STR_PARK_RATING_WARNING_2_WEEKS_REMAINING: // peeps
        case STR_PARK_RATING_WARNING_3_WEEKS_REMAINING: // peeps
        case STR_PARK_RATING_WARNING_4_WEEKS_REMAINING: // peeps
        case STR_PEEPS_COMPLAINING_ABOUT_QUEUE_LENGTH_WARNING: // ride
        case STR_PEEPS_ARE_HUNGRY:               // peeps
        case STR_PEEPS_ARE_THIRSTY:              // peeps
        case STR_PEEPS_CANT_FIND_TOILET:         // peeps
        case STR_PEEPS_DISLIKE_LITTER:           // peeps
        case STR_PEEPS_DISGUSTED_BY_PATHS:       // peeps
        case STR_PEEPS_GETTING_LOST_OR_STUCK:    // peeps
        case STR_ENTRANCE_FEE_TOO_HI:            // peeps
            return Severity::warning;

        default:
            break;
    }

    // ---- ItemType-fallback ----------------------------------------------
    switch (type)
    {
        case ItemType::money:
        case ItemType::campaign:
            // Finance + marketing campaign milestones are positive
            // progression events — green.
            return Severity::money;

        case ItemType::award:
        {
            // Positive awards: money/green. Negative awards: warning.
            // assoc encodes the AwardType for award items.
            const auto awardType = static_cast<AwardType>(assoc);
            return AwardIsPositive(awardType) ? Severity::money : Severity::warning;
        }

        case ItemType::null:
        case ItemType::count:
        case ItemType::blank:
        case ItemType::peepOnRide:
        case ItemType::graph:
        case ItemType::research:
        case ItemType::ride:
        case ItemType::peep:
        case ItemType::peeps:
        default:
            // Cosmetic / informational by default. The explicit
            // StringId table above promotes known criticals + warnings
            // to the right severity; anything left over (research
            // unlocks, guest tracking, generic blank text from
            // ReplayManager etc.) is correctly info.
            return Severity::info;
    }
}

/**
 *
 *  rct2: 0x0066DF55
 */
News::Item* News::AddItemToQueue(ItemType type, StringId string_id, uint32_t assoc, const Formatter& formatter)
{
    utf8 buffer[256];

    // overflows possible?
    FormatStringLegacy(buffer, 256, string_id, formatter.Data());

    auto* newsItem = AddItemToQueue(type, buffer, assoc);

    // OPENRCT2MINI gamepad-plan 1.13: classify the news item by
    // (type, stringId, assoc) and cache the severity on the Item
    // so UpdateCurrentItem / ArchiveCurrent can drive the DualShock
    // lightbar across the item's full on-screen lifetime. The flash
    // fires once the item actually becomes current (ticks == 1, same
    // gate as the newsItem audio cue) — queueing it here would light
    // up the LED for items that haven't surfaced yet.
    if (newsItem != nullptr)
        newsItem->severity = GetSeverity(type, string_id, assoc);

    return newsItem;
}

// TODO: Use variant for assoc, requires strong type for each possible input.
News::Item* News::AddItemToQueue(ItemType type, StringId string_id, EntityId assoc, const Formatter& formatter)
{
    return AddItemToQueue(type, string_id, assoc.ToUnderlying(), formatter);
}

News::Item* News::AddItemToQueue(ItemType type, const utf8* text, uint32_t assoc)
{
    auto& date = GetDate();
    Item* newsItem = getGameState().newsItems.firstOpenOrNewSlot();
    newsItem->type = type;
    newsItem->flags = 0;
    newsItem->assoc = assoc; // Make optional for Award, Money, Graph and Null
    newsItem->ticks = 0;
    newsItem->monthYear = static_cast<uint16_t>(date.GetMonthsElapsed());
    newsItem->day = date.GetDay() + 1;
    newsItem->text = text;

    return newsItem;
}

/**
 * Checks if News::ItemType requires an assoc
 * @return A boolean if assoc is required.
 */

bool News::CheckIfItemRequiresAssoc(ItemType type)
{
    switch (type)
    {
        case ItemType::null:
        case ItemType::award:
        case ItemType::money:
        case ItemType::graph:
            return false;
        default:
            return true; // Everything else requires assoc
    }
}

/**
 * Opens the window/tab for the subject of the news item
 *
 *  rct2: 0x0066EBE6
 *
 */
void News::OpenSubject(ItemType type, int32_t subject)
{
    switch (type)
    {
        case ItemType::ride:
        {
            auto intent = Intent(WindowClass::ride);
            intent.PutExtra(INTENT_EXTRA_RIDE_ID, subject);
            ContextOpenIntent(&intent);
            break;
        }
        case ItemType::peepOnRide:
        case ItemType::peep:
        {
            auto peep = getGameState().entities.TryGetEntity<Peep>(EntityId::FromUnderlying(subject));
            if (peep != nullptr)
            {
                auto intent = Intent(WindowClass::peep);
                intent.PutExtra(INTENT_EXTRA_PEEP, peep);
                ContextOpenIntent(&intent);
            }
            break;
        }
        case ItemType::money:
            ContextOpenWindow(WindowClass::finances);
            break;
        case ItemType::campaign:
            ContextOpenWindowView(WindowView::financeMarketing);
            break;
        case ItemType::research:
        {
            auto item = ResearchItem(subject, ResearchCategory::transport, 0);
            if (item.type == Research::EntryType::ride)
            {
                auto intent = Intent(INTENT_ACTION_NEW_RIDE_OF_TYPE);
                intent.PutExtra(INTENT_EXTRA_RIDE_TYPE, item.baseRideType);
                intent.PutExtra(INTENT_EXTRA_RIDE_ENTRY_INDEX, item.entryIndex);
                ContextOpenIntent(&intent);
                break;
            }

            auto intent = Intent(INTENT_ACTION_NEW_SCENERY);
            intent.PutExtra(INTENT_EXTRA_SCENERY_GROUP_ENTRY_INDEX, item.entryIndex);
            ContextOpenIntent(&intent);
            break;
        }
        case ItemType::peeps:
        {
            auto intent = Intent(WindowClass::guestList);
            intent.PutExtra(INTENT_EXTRA_GUEST_LIST_FILTER, static_cast<int32_t>(GuestListFilterType::guestsThinkingX));
            intent.PutExtra(INTENT_EXTRA_RIDE_ID, subject);
            ContextOpenIntent(&intent);
            break;
        }
        case ItemType::award:
            ContextOpenWindowView(WindowView::parkAwards);
            break;
        case ItemType::graph:
            ContextOpenWindowView(WindowView::parkRating);
            break;
        case ItemType::null:
        case ItemType::blank:
        case ItemType::count:
            break;
    }
}

/**
 *
 *  rct2: 0x0066E407
 */
void News::DisableNewsItems(ItemType type, uint32_t assoc)
{
    auto& gameState = getGameState();
    // TODO: write test invalidating windows
    gameState.newsItems.foreachRecentNews([type, assoc, &gameState](auto& newsItem) {
        if (type == newsItem.type && assoc == newsItem.assoc)
        {
            newsItem.setFlags(ItemFlags::hasButton);
            if (&newsItem == &gameState.newsItems.current())
            {
                auto intent = Intent(INTENT_ACTION_INVALIDATE_TICKER_NEWS);
                ContextBroadcastIntent(&intent);
            }
        }
    });

    gameState.newsItems.foreachArchivedNews([type, assoc](auto& newsItem) {
        if (type == newsItem.type && assoc == newsItem.assoc)
        {
            newsItem.setFlags(ItemFlags::hasButton);
            auto* windowMgr = Ui::GetWindowManager();
            windowMgr->InvalidateByClass(WindowClass::recentNews);
        }
    });
}

void News::AddItemToQueue(Item* newNewsItem)
{
    Item* newsItem = getGameState().newsItems.firstOpenOrNewSlot();
    *newsItem = *newNewsItem;
}

void News::RemoveItem(int32_t index)
{
    if (index < 0 || index >= MaxItems)
        return;

    auto& gameState = getGameState();
    // News item is already null, no need to remove it
    if (gameState.newsItems[index].type == ItemType::null)
        return;

    size_t newsBoundary = index < ItemHistoryStart ? ItemHistoryStart : MaxItems;
    for (size_t i = index; i < newsBoundary - 1; i++)
    {
        gameState.newsItems[i] = gameState.newsItems[i + 1];
    }
    gameState.newsItems[newsBoundary - 1].type = ItemType::null;
}

void News::importNewsItems(GameState_t& gameState, const std::span<const Item> recent, const std::span<const Item> archived)
{
    gameState.newsItems.clear();

    for (size_t i = 0; i < std::min<size_t>(recent.size(), ItemHistoryStart); i++)
    {
        gameState.newsItems[i] = recent[i];
    }
    for (size_t i = 0; i < std::min<size_t>(archived.size(), MaxItemsArchive); i++)
    {
        gameState.newsItems[ItemHistoryStart + i] = archived[i];
    }
}
