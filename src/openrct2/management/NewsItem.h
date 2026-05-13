/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Identifiers.h"
#include "../core/StringTypes.h"
#include "../localisation/StringIdType.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include "../core/Span.hpp"
#include <string>

struct CoordsXYZ;

namespace OpenRCT2
{
    class Formatter;
    struct GameState_t;
} // namespace OpenRCT2

namespace OpenRCT2::News
{
    enum class ItemType : uint8_t
    {
        null,
        ride,
        peepOnRide,
        peep,
        money,
        blank,
        research,
        peeps,
        award,
        graph,
        campaign,
        count
    };

    // OPENRCT2MINI gamepad-plan 1.12: severity classification for
    // news items. ItemType groups by CATEGORY (peep / money /
    // research / …); a single category can span multiple severities
    // ("guest drowned" and "guest joined queue" are both peep). This
    // enum is the orthogonal severity axis, used by the DualShock
    // LED indicator (1.13) and the critical-news rumble pulse hook
    // deferred from 1.11.
    //   info     — generic / cosmetic notifications, no signal needed
    //   money    — financial / progression-positive events
    //   warning  — actionable problem (mechanic en route, queue jam,
    //              cash flow warning, multi-week rating countdown)
    //   critical — emergency requiring immediate attention (ride
    //              crash, guest drowned, vandalism, park closure /
    //              1-week-remaining countdown)
    enum class Severity : uint8_t
    {
        info,
        money,
        warning,
        critical,
    };

    // Classify a news item by (ItemType, StringId, assoc).
    //   - Most ItemTypes bucket cleanly (money → money, research →
    //     info, etc.); the StringId lookup is only consulted for the
    //     ride / peep / peeps types where the same category spans
    //     multiple severities.
    //   - `assoc` carries award type / ride id; used to disambiguate
    //     positive vs negative awards via AwardIsPositive. Defaults
    //     to 0 so callers that only have type + stringId don't have
    //     to pass it.
    // Unknown StringIds fall through to the category's default. New
    // strings added later don't break the function — they just get
    // the default until someone explicitly classifies them.
    Severity GetSeverity(ItemType type, StringId stringId, uint32_t assoc = 0);

    constexpr size_t ItemTypeCount = static_cast<size_t>(ItemType::count);

    enum ItemTypeProperty : uint8_t
    {
        hasLocation = 1,
        hasSubject = 2,
    };

    enum ItemFlags : uint8_t
    {
        hasButton = 1 << 0,
    };

    /**
     * A single news item / message.
     */
    struct Item
    {
        ItemType type = ItemType::null;
        uint8_t flags{};
        uint32_t assoc{};
        uint16_t ticks{};
        uint16_t monthYear{};
        uint8_t day{};
        // OPENRCT2MINI gamepad-plan 1.13: classification cached at
        // ingestion time so UpdateCurrentItem can drive the DualShock
        // lightbar across the news item's full on-screen lifetime
        // without re-running GetSeverity each frame and without
        // storing the original StringId (which is lost after
        // FormatStringLegacy bakes the text). Defaults to `info` so
        // legacy items deserialised from older saves / network sync
        // simply produce no LED flash rather than a wrong colour.
        Severity severity = Severity::info;
        std::string text{};

        constexpr bool isEmpty() const noexcept
        {
            return type == ItemType::null;
        }

        constexpr uint8_t getTypeProperties() const
        {
            switch (type)
            {
                case ItemType::blank:
                    return hasLocation;
                case ItemType::money:
                case ItemType::research:
                case ItemType::peeps:
                case ItemType::award:
                case ItemType::graph:
                case ItemType::campaign:
                    return hasSubject;
                case ItemType::ride:
                case ItemType::peepOnRide:
                case ItemType::peep:
                    return hasLocation | hasSubject;
                case ItemType::null:
                case ItemType::count:
                default:
                    return 0;
            }
        }

        void setFlags(uint8_t flag)
        {
            flags |= flag;
        }

        constexpr bool typeHasSubject() const
        {
            return this->getTypeProperties() & hasSubject;
        }

        constexpr bool typeHasLocation() const
        {
            return this->getTypeProperties() & hasLocation;
        }

        constexpr bool hasButton() const noexcept
        {
            return flags & ItemFlags::hasButton;
        }
    };

    constexpr int32_t ItemHistoryStart = 11;
    constexpr int32_t MaxItemsArchive = 50;
    constexpr int32_t MaxItems = ItemHistoryStart + MaxItemsArchive;

    template<std::size_t N>
    class ItemQueue
    {
    public:
        static_assert(N > 0, "Cannot instantiate News::ItemQueue with size=0");

        // OPENRCT2MINI: cut 33. typename added for C++17 dependent-name resolution.
        using value_type = typename std::array<Item, N>::value_type;
        using pointer = value_type*;
        using const_pointer = const value_type*;
        using reference = value_type&;
        using const_reference = const value_type&;
        using iterator = typename std::array<Item, N>::iterator;
        using const_iterator = typename std::array<Item, N>::const_iterator;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        ItemQueue()
        {
            std::get<0>(Queue).type = ItemType::null;
        }

        constexpr iterator begin() noexcept
        {
            return std::begin(Queue);
        }
        constexpr const_iterator begin() const noexcept
        {
            return cbegin();
        }
        constexpr const_iterator cbegin() const noexcept
        {
            return std::cbegin(Queue);
        }
        iterator end() noexcept
        {
            return std::find_if(std::begin(Queue), std::end(Queue), [](const_reference item) { return item.isEmpty(); });
        }
        const_iterator end() const noexcept
        {
            return cend();
        }
        const_iterator cend() const noexcept
        {
            return std::find_if(std::cbegin(Queue), std::cend(Queue), [](const_reference item) { return item.isEmpty(); });
        }

        constexpr bool empty() const noexcept
        {
            return std::get<0>(Queue).isEmpty();
        }

        size_type size() const noexcept
        {
            return std::distance(cbegin(), cend());
        }

        reference front() noexcept
        {
            return *begin();
        }
        const_reference front() const noexcept
        {
            return *cbegin();
        }
        reference back() noexcept
        {
            return *end();
        }
        const_reference back() const noexcept
        {
            return *cend();
        }

        void pop_front()
        {
            std::move(std::begin(Queue) + 1, std::end(Queue), std::begin(Queue));
            Queue[N - 1].type = ItemType::null;
        }

        void push_back(const_reference item)
        {
            auto it = end();
            if (!std::distance(it, std::end(Queue)))
            {
                // Reached queue max size, need to free some space
                pop_front();
                Queue[N - 1] = item;
            }
            else
            {
                *it = item;
                ++it;
                if (std::distance(it, std::end(Queue)))
                    it->type = ItemType::null;
            }
        }

        reference operator[](size_type n) noexcept
        {
            return Queue[n];
        }
        const_reference operator[](size_type n) const noexcept
        {
            return Queue[n];
        }

        constexpr size_type capacity() const noexcept
        {
            return N;
        }

        void clear() noexcept
        {
            std::fill(Queue.begin(), Queue.end(), Item{});
        }

    private:
        std::array<Item, N> Queue;
    };

    struct ItemQueues
    {
        Item& operator[](size_t index);
        const Item& operator[](size_t index) const;
        Item* At(int32_t index);
        const Item* At(int32_t index) const;
        bool IsEmpty() const;
        void Clear();
        uint16_t IncrementTicks();
        Item& Current();
        const Item& Current() const;
        bool CurrentShouldBeArchived() const;
        void ArchiveCurrent();
        Item* FirstOpenOrNewSlot();
        const auto& GetRecent() const
        {
            return Recent;
        }
        const auto& GetArchived() const
        {
            return Archived;
        }

        template<typename Predicate>
        void ForeachRecentNews(Predicate&& p)
        {
            for (auto& newsItem : Recent)
            {
                p(newsItem);
            }
        }

        template<typename Predicate>
        void ForeachArchivedNews(Predicate&& p)
        {
            for (auto& newsItem : Archived)
            {
                p(newsItem);
            }
        }

    private:
        int32_t RemoveTime() const;

        ItemQueue<ItemHistoryStart> Recent;
        ItemQueue<MaxItemsArchive> Archived;
    };

    void InitQueue(GameState_t& gameState);

    void UpdateCurrentItem();
    void CloseCurrentItem();

    std::optional<CoordsXYZ> GetSubjectLocation(ItemType type, int32_t subject);

    Item* AddItemToQueue(ItemType type, StringId string_id, uint32_t assoc, const Formatter& formatter);
    Item* AddItemToQueue(ItemType type, StringId string_id, EntityId assoc, const Formatter& formatter);
    Item* AddItemToQueue(ItemType type, const utf8* text, uint32_t assoc);

    bool CheckIfItemRequiresAssoc(ItemType type);

    void OpenSubject(ItemType type, int32_t subject);

    void DisableNewsItems(ItemType type, uint32_t assoc);

    Item* GetItem(int32_t index);

    bool IsQueueEmpty();

    bool IsValidIndex(int32_t index);

    void AddItemToQueue(Item* newNewsItem);
    void RemoveItem(int32_t index);

    void importNewsItems(GameState_t& gameState, std::span<const Item> recent, std::span<const Item> archived);
} // namespace OpenRCT2::News
