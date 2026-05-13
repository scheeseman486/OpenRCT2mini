/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/ride/Construction.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Editor.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/String.hpp>
#include <openrct2/core/UnitConversion.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.String.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/ride/RideConstruction.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/TrackDesign.h>
#include <openrct2/ride/TrackDesignRepository.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <vector>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    static constexpr StringId kWindowTitle = STR_SELECT_DESIGN;
    // OPENRCT2MINI: shaved 32 px off the height (441 → 409) to fit the
    // 480 px panel with toolbar headroom. The ride-statistics text below
    // the preview moved into a vertical scroll widget (WIDX_STATS_SCROLL)
    // so a coaster with the full G-force / drops / inversions list no
    // longer overflows the bottom of the now-shorter window. The rotate
    // / scenery buttons stay vertically stacked in the right column so
    // the scroll widget can take the full width to their left.
    static constexpr ScreenSize kWindowSize = { 600, 409 };
    static constexpr int32_t kDebugPathHeight = 12;
    static constexpr int32_t kRotateAndSceneryButtonSize = 24;
    static constexpr int32_t kWindowPadding = 5;

    enum
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_BACK,
        WIDX_FILTER_STRING,
        WIDX_FILTER_CLEAR,
        WIDX_TRACK_LIST,
        WIDX_TRACK_PREVIEW,
        WIDX_ROTATE,
        WIDX_TOGGLE_SCENERY,
        // OPENRCT2MINI: stats text moved into a scrollable widget so a coaster
        // with G-forces + drops + inversions doesn't overflow the bottom of
        // the (now-shorter) window. Sits below the preview, left of the
        // rotate / scenery buttons (which now share a horizontal row in the
        // bottom-right corner).
        WIDX_STATS_SCROLL,
    };

    VALIDATE_GLOBAL_WIDX(WC_TRACK_DESIGN_LIST, WIDX_ROTATE);

    // clang-format off
    static constexpr auto kFlatBtnSize = ScreenSize{kRotateAndSceneryButtonSize, kRotateAndSceneryButtonSize};
    static const auto _trackListWidgets = makeWidgets(
        makeWindowShim(kWindowTitle, kWindowSize),
        makeWidget({  4,  18}, {218,  13},   WidgetType::tableHeader,  WindowColour::primary, STR_SELECT_OTHER_RIDE                                       ),
        makeWidget({  4,  32}, {124,  13},   WidgetType::textBox,      WindowColour::secondary                                                            ),
        makeWidget({130,  32}, { 92,  13},   WidgetType::button,       WindowColour::primary, STR_OBJECT_SEARCH_CLEAR                                     ),
        // OPENRCT2MINI: scroll list shrunk 381 → 349 to track the 32 px window crop.
        makeWidget({  4,  46}, {218, 349},   WidgetType::scroll,       WindowColour::primary, SCROLL_VERTICAL,         STR_CLICK_ON_DESIGN_TO_BUILD_IT_TIP),
        makeWidget({224,  18}, {372, 219},   WidgetType::flatBtn,      WindowColour::primary                                                              ),
        // OPENRCT2MINI: rotate (lower) + scenery (just above) stacked
        // vertically in the bottom-right corner. y is set in onPrepareDraw.
        makeWidget({572, 380}, kFlatBtnSize, WidgetType::flatBtn,      WindowColour::primary, ImageId(SPR_ROTATE_ARROW),        STR_ROTATE_90_TIP                  ),
        makeWidget({572, 356}, kFlatBtnSize, WidgetType::flatBtn,      WindowColour::primary, ImageId(SPR_SCENERY),             STR_TOGGLE_SCENERY_TIP             ),
        // OPENRCT2MINI: stats scroll box. left+top fixed; right+bottom set
        // dynamically in onPrepareDraw to track the button column and the
        // window's bottom margin (debugging-tools mode shrinks bottom).
        makeWidget({224, 240}, {344, 164},   WidgetType::scroll,       WindowColour::secondary, SCROLL_VERTICAL                                            )
    );
    // clang-format on

    constexpr uint16_t kTrackDesignIndexUnloaded = UINT16_MAX;

    RideSelection _window_track_list_item;

    class TrackListWindow final : public Window
    {
    private:
        std::vector<TrackDesignFileRef> _trackDesigns;
        utf8 _filterString[kUserStringMaxLength]{};
        std::vector<uint16_t> _filteredTrackIds;
        uint16_t _loadedTrackDesignIndex;
        std::unique_ptr<TrackDesign> _loadedTrackDesign;
        TrackDesignPreviewBuffer _trackDesignPreviewPixels{};
        bool _selectedItemIsBeingUpdated;
        bool _reloadTrackDesigns;
        u8string _windowTitle;

        void filterList()
        {
            _filteredTrackIds.clear();

            // Nothing to filter, so fill the list with all indices
            if (String::lengthOf(_filterString) == 0)
            {
                for (uint16_t i = 0; i < _trackDesigns.size(); i++)
                    _filteredTrackIds.push_back(i);

                return;
            }

            // Convert filter to uppercase
            const auto filterStringUpper = String::toUpper(_filterString);

            // Fill the set with indices for tracks that match the filter
            for (uint16_t i = 0; i < _trackDesigns.size(); i++)
            {
                const auto trackNameUpper = String::toUpper(_trackDesigns[i].name);
                if (trackNameUpper.find(filterStringUpper) != std::string::npos)
                {
                    _filteredTrackIds.push_back(i);
                }
            }

            // Ensure that the selected item is still in the list.
            if (static_cast<size_t>(selectedListItem) >= _filteredTrackIds.size())
            {
                selectedListItem = 0;
            }
        }

        void selectFromList(int32_t listIndex)
        {
            Audio::Play(Audio::SoundId::click1, 0, this->windowPos.x + (this->width / 2));
            if (gLegacyScene != LegacyScene::trackDesignsManager)
            {
                if (listIndex == 0)
                {
                    close();
                    RideConstructNew(_window_track_list_item);
                    return;
                }
                listIndex--;
            }

            // Displays a message if the ride can't load, fix #4080
            if (_loadedTrackDesign == nullptr)
            {
                ContextShowError(STR_CANT_BUILD_THIS_HERE, STR_TRACK_LOAD_FAILED_ERROR, {});
                return;
            }

            if (_loadedTrackDesign->gameStateData.hasFlag(TrackDesignGameStateFlag::SceneryUnavailable))
            {
                gTrackDesignSceneryToggle = true;
            }

            uint16_t trackDesignIndex = _filteredTrackIds[listIndex];
            TrackDesignFileRef* tdRef = &_trackDesigns[trackDesignIndex];
            if (gLegacyScene == LegacyScene::trackDesignsManager)
            {
                auto intent = Intent(WindowClass::manageTrackDesign);
                intent.PutExtra(INTENT_EXTRA_TRACK_DESIGN, tdRef);
                ContextOpenIntent(&intent);
            }
            else
            {
                if (_loadedTrackDesignIndex != kTrackDesignIndexUnloaded
                    && (_loadedTrackDesign->gameStateData.hasFlag(TrackDesignGameStateFlag::VehicleUnavailable)))
                {
                    ContextShowError(STR_THIS_DESIGN_WILL_BE_BUILT_WITH_AN_ALTERNATIVE_VEHICLE_TYPE, kStringIdNone, {});
                }

                auto intent = Intent(WindowClass::trackDesignPlace);
                intent.PutExtra(INTENT_EXTRA_TRACK_DESIGN, tdRef);
                ContextOpenIntent(&intent);
            }
        }

        int32_t getListItemFromPosition(const ScreenCoordsXY& screenCoords)
        {
            size_t maxItems = _filteredTrackIds.size();
            if (gLegacyScene != LegacyScene::trackDesignsManager)
            {
                // Extra item: custom design
                maxItems++;
            }

            int32_t index = screenCoords.y / kScrollableRowHeight;
            if (index < 0 || static_cast<uint32_t>(index) >= maxItems)
            {
                index = -1;
            }
            return index;
        }

        void loadDesignsList(RideSelection item)
        {
            auto repo = GetContext()->GetTrackDesignRepository();
            std::string entryName;
            if (item.Type < 0x80)
            {
                if (GetRideTypeDescriptor(item.Type).flags.has(RtdFlag::listVehiclesSeparately))
                {
                    entryName = GetRideEntryName(item.EntryIndex);
                }
            }
            _trackDesigns = repo->GetItemsForObjectEntry(item.Type, entryName);

            filterList();
        }

        bool loadDesignPreview(const u8string& path)
        {
            _loadedTrackDesign = TrackDesignImport(path.c_str());
            if (_loadedTrackDesign != nullptr)
            {
                TrackDesignDrawPreview(*_loadedTrackDesign, _trackDesignPreviewPixels, !gTrackDesignSceneryToggle);
                return true;
            }
            return false;
        }

    public:
        TrackListWindow(const RideSelection item)
        {
            _window_track_list_item = item;
        }

        void onOpen() override
        {
            String::set(_filterString, sizeof(_filterString), "");
            setWidgets(_trackListWidgets);
            widgets[WIDX_FILTER_STRING].string = _filterString;

            loadDesignsList(_window_track_list_item);

            WindowInitScrollWidgets(*this);
            _selectedItemIsBeingUpdated = false;
            _reloadTrackDesigns = false;
            // Start with first track highlighted
            selectedListItem = 0;
            if (!_trackDesigns.empty() && gLegacyScene != LegacyScene::trackDesignsManager)
            {
                selectedListItem = 1;
            }
            gTrackDesignSceneryToggle = false;
            WindowPushOthersRight(*this);
            _currentTrackPieceDirection = 2;
            std::fill(_trackDesignPreviewPixels.begin(), _trackDesignPreviewPixels.end(), PaletteIndex::transparent);

            _loadedTrackDesign = nullptr;
            _loadedTrackDesignIndex = kTrackDesignIndexUnloaded;
        }

        void reopenTrackManager()
        {
            auto* windowMgr = GetWindowManager();
            windowMgr->CloseByNumber(WindowClass::manageTrackDesign, number);
            windowMgr->CloseByNumber(WindowClass::trackDeletePrompt, number);
            Editor::LoadTrackManager();
        }

        void onClose() override
        {
            // Dispose track design and preview
            _loadedTrackDesign = nullptr;

            // Dispose track list
            _trackDesigns.clear();

            // If gScreenAge is zero, we're already in the process
            // of loading the track manager, so we shouldn't try
            // to do it again. Otherwise, this window will get
            // another close signal from the track manager load function,
            // try to load the track manager again, and an infinite loop will result.
            if ((gLegacyScene == LegacyScene::trackDesignsManager) && gScreenAge != 0)
            {
                reopenTrackManager();
            }
        }

        void onMouseUp(const WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    break;
                case WIDX_ROTATE:
                    _currentTrackPieceDirection++;
                    _currentTrackPieceDirection %= 4;
                    invalidate();
                    break;
                case WIDX_TOGGLE_SCENERY:
                    gTrackDesignSceneryToggle = !gTrackDesignSceneryToggle;
                    _loadedTrackDesignIndex = kTrackDesignIndexUnloaded;
                    invalidate();
                    break;
                case WIDX_BACK:
                    close();
                    if (gLegacyScene != LegacyScene::trackDesignsManager)
                    {
                        ContextOpenWindow(WindowClass::constructRide);
                    }
                    else
                    {
                        reopenTrackManager();
                    }
                    break;
                case WIDX_FILTER_STRING:
                    WindowStartTextbox(*this, widgetIndex, _filterString, sizeof(_filterString));
                    break;
                case WIDX_FILTER_CLEAR:
                    // Keep the highlighted item selected
                    if (gLegacyScene == LegacyScene::trackDesignsManager)
                    {
                        if (selectedListItem != -1 && _filteredTrackIds.size() > static_cast<size_t>(selectedListItem))
                            selectedListItem = _filteredTrackIds[selectedListItem];
                        else
                            selectedListItem = -1;
                    }
                    else
                    {
                        if (selectedListItem != 0)
                            selectedListItem = _filteredTrackIds[selectedListItem - 1] + 1;
                    }

                    String::set(_filterString, sizeof(_filterString), "");
                    filterList();
                    invalidate();
                    break;
            }
        }

        ScreenSize onScrollGetSize(const int32_t scrollIndex) override
        {
            // OPENRCT2MINI: scroll widget 1 is the stats text. Its content
            // size depends on which ride flags the loaded design exposes.
            if (scrollIndex == 1)
            {
                return { widgets[WIDX_STATS_SCROLL].width(), computeStatsHeight() };
            }

            size_t numItems = _filteredTrackIds.size();
            if (gLegacyScene != LegacyScene::trackDesignsManager)
            {
                // Extra item: custom design
                numItems++;
            }
            int32_t scrollHeight = static_cast<int32_t>(numItems * kScrollableRowHeight);

            return { width, scrollHeight };
        }

        void onScrollMouseDown(const int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            // OPENRCT2MINI: stats scroll is read-only.
            if (scrollIndex != 0)
                return;

            if (!_selectedItemIsBeingUpdated)
            {
                int32_t i = getListItemFromPosition(screenCoords);
                if (i != -1)
                {
                    selectFromList(i);
                }
            }
        }

        void onScrollMouseOver(const int32_t scrollIndex, const ScreenCoordsXY& screenCoords) override
        {
            // OPENRCT2MINI: stats scroll is read-only.
            if (scrollIndex != 0)
                return;

            if (!_selectedItemIsBeingUpdated)
            {
                int32_t i = getListItemFromPosition(screenCoords);
                if (i != -1 && selectedListItem != i)
                {
                    selectedListItem = i;
                    invalidate();
                }
            }
        }

        // OPENRCT2MINI list-focus-plan §3.8: 1D list opt-in for the
        // main design list ONLY (scrollIndex == 0). The stats scroll
        // (scrollIndex == 1) is purely visual / read-only — it stays
        // a non-focus stop. Items count matches onScrollGetSize for
        // scroll 0: filtered designs + one extra "custom design" row
        // outside the track-designs-manager flow.
        int32_t scrollFocusGetItemCount(int32_t scrollIndex) override
        {
            if (scrollIndex != 0)
                return 0;
            size_t numItems = _filteredTrackIds.size();
            if (gLegacyScene != LegacyScene::trackDesignsManager)
                numItems++;
            return static_cast<int32_t>(numItems);
        }

        ScreenRect scrollFocusGetItemRect(int32_t scrollIndex, int32_t itemIndex) override
        {
            if (scrollIndex != 0 || itemIndex < 0)
                return {};
            const int32_t count = scrollFocusGetItemCount(scrollIndex);
            if (itemIndex >= count)
                return {};
            const int32_t y = itemIndex * kScrollableRowHeight;
            return ScreenRect{ { 0, y }, { width - 1, y + kScrollableRowHeight - 1 } };
        }

        void onTextInput(const WidgetIndex widgetIndex, std::string_view text) override
        {
            if (widgetIndex != WIDX_FILTER_STRING)
                return;

            if (String::equals(_filterString, std::string(text).c_str()))
                return;

            String::set(_filterString, sizeof(_filterString), std::string(text).c_str());

            filterList();

            scrolls->contentOffsetY = 0;

            invalidate();
        }

        void onPrepareDraw() override
        {
            StringId stringId = kStringIdNone;
            const auto* entry = GetRideEntryByIndex(_window_track_list_item.EntryIndex);

            if (entry != nullptr)
            {
                RideNaming rideName = GetRideNaming(_window_track_list_item.Type, entry);
                stringId = rideName.Name;
            }

            StringId titleFormat;
            if (gLegacyScene == LegacyScene::trackDesignsManager)
            {
                titleFormat = STR_TRACK_DESIGNS;
                widgets[WIDX_TRACK_LIST].tooltip = STR_CLICK_ON_DESIGN_TO_RENAME_OR_DELETE_IT;
            }
            else
            {
                titleFormat = STR_SELECT_DESIGN;
                widgets[WIDX_TRACK_LIST].tooltip = STR_CLICK_ON_DESIGN_TO_BUILD_IT_TIP;
            }
            _windowTitle = FormatStringID(titleFormat, stringId);
            widgets[WIDX_TITLE].setString(_windowTitle.c_str());

            const bool showPreview = (gLegacyScene == LegacyScene::trackDesignsManager) || selectedListItem != 0;
            setWidgetPressed(WIDX_TRACK_PREVIEW, showPreview);
            setWidgetDisabled(WIDX_TRACK_PREVIEW, !showPreview);
            if (showPreview)
            {
                widgets[WIDX_ROTATE].type = WidgetType::flatBtn;
                widgets[WIDX_TOGGLE_SCENERY].type = WidgetType::flatBtn;
                widgets[WIDX_STATS_SCROLL].type = WidgetType::scroll;
                setWidgetPressed(WIDX_TOGGLE_SCENERY, !gTrackDesignSceneryToggle);
            }
            else
            {
                widgets[WIDX_ROTATE].type = WidgetType::empty;
                widgets[WIDX_TOGGLE_SCENERY].type = WidgetType::empty;
                widgets[WIDX_STATS_SCROLL].type = WidgetType::empty;
            }

            // When debugging tools are on, shift everything up a bit to make room for displaying the path.
            const int32_t bottomMargin = Config::Get().general.debuggingTools ? (kWindowPadding + kDebugPathHeight)
                                                                              : kWindowPadding;
            widgets[WIDX_TRACK_LIST].bottom = height - bottomMargin;

            // OPENRCT2MINI: rotate + scenery stacked vertically at the right
            // edge — rotate at the bottom, scenery directly above it. The
            // narrow right column lets the stats scroll claim the rest of
            // the horizontal real estate below the preview.
            widgets[WIDX_ROTATE].bottom = height - bottomMargin;
            widgets[WIDX_ROTATE].top = widgets[WIDX_ROTATE].bottom - kRotateAndSceneryButtonSize;
            widgets[WIDX_TOGGLE_SCENERY].bottom = widgets[WIDX_ROTATE].top;
            widgets[WIDX_TOGGLE_SCENERY].top = widgets[WIDX_TOGGLE_SCENERY].bottom - kRotateAndSceneryButtonSize;

            // OPENRCT2MINI: stats scroll fills the space below the preview,
            // stopping 4 px short of the (vertically-stacked) right button
            // column. Bottom tracks the same anchor as the buttons.
            widgets[WIDX_STATS_SCROLL].bottom = height - bottomMargin;
            widgets[WIDX_STATS_SCROLL].right = widgets[WIDX_ROTATE].left - 4;

            // OPENRCT2MINI: load the selected track design here (was in onDraw)
            // so _loadedTrackDesign is populated before drawWidgets recurses
            // into onScrollDraw for WIDX_STATS_SCROLL.
            if (showPreview)
            {
                int32_t listItemIndex = selectedListItem;
                if (gLegacyScene != LegacyScene::trackDesignsManager)
                    listItemIndex--;
                if (!_filteredTrackIds.empty() && listItemIndex >= 0
                    && static_cast<size_t>(listItemIndex) < _filteredTrackIds.size())
                {
                    const int32_t trackIndex = _filteredTrackIds[listItemIndex];
                    if (_loadedTrackDesignIndex != trackIndex)
                    {
                        const u8string& path = _trackDesigns[trackIndex].path;
                        if (loadDesignPreview(path))
                            _loadedTrackDesignIndex = trackIndex;
                        else
                            _loadedTrackDesignIndex = kTrackDesignIndexUnloaded;
                    }
                }
            }
        }

        void onUpdate() override
        {
            if (GetCurrentTextBox().window.classification == classification && GetCurrentTextBox().window.number == number)
            {
                WindowUpdateTextboxCaret();
                invalidateWidget(WIDX_FILTER_STRING); // TODO Check this
            }

            if (_reloadTrackDesigns)
            {
                loadDesignsList(_window_track_list_item);
                selectedListItem = 0;
                invalidate();
                _reloadTrackDesigns = false;
            }
        }

        void onDraw(RenderTarget& rt) override
        {
            drawWidgets(rt);

            int32_t listItemIndex = selectedListItem;
            if ((gLegacyScene == LegacyScene::trackDesignsManager) == 0)
            {
                // Because the first item in the list is "Build a custom design", lower the index by one
                listItemIndex--;
            }

            if (_filteredTrackIds.empty() || listItemIndex == -1)
                return;

            int32_t trackIndex = _filteredTrackIds[listItemIndex];

            // Track preview
            auto& tdWidget = widgets[WIDX_TRACK_PREVIEW];
            auto colour = getColourMap(colours[0].colour).darkest;
            u8string path = _trackDesigns[trackIndex].path;

            // Show track file path (in debug mode)
            if (Config::Get().general.debuggingTools)
            {
                const auto shortPath = shortenPath(path, width, FontStyle::medium);
                drawText(
                    rt, windowPos + ScreenCoordsXY{ 0, height - kDebugPathHeight - 3 }, shortPath,
                    { colours[1] }); // TODO Check dpi
            }

            auto screenPos = windowPos + ScreenCoordsXY{ tdWidget.left + 1, tdWidget.top + 1 };
            Rectangle::fill(rt, { screenPos, screenPos + ScreenCoordsXY{ 369, 216 } }, colour); // TODO Check dpi

            // OPENRCT2MINI: design load moved to onPrepareDraw so the stats
            // scroll widget has _loadedTrackDesign populated by the time
            // drawWidgets recurses into onScrollDraw.
            if (!_loadedTrackDesign)
            {
                return;
            }

            auto trackPreview = screenPos;
            screenPos = windowPos + ScreenCoordsXY{ tdWidget.midX(), tdWidget.midY() };

            G1Element g1temp = {};
            g1temp.offset = reinterpret_cast<uint8_t*>(
                _trackDesignPreviewPixels.data() + (_currentTrackPieceDirection * kTrackPreviewImageSize));
            g1temp.width = 370;
            g1temp.height = 217;
            g1temp.flags = { G1Flag::hasTransparency };
            GfxSetG1Element(SPR_TEMP_TRACK_LIST, &g1temp);
            DrawingEngineInvalidateImage(SPR_TEMP_TRACK_LIST);
            GfxDrawSprite(rt, ImageId(SPR_TEMP_TRACK_LIST), trackPreview);

            screenPos.y = windowPos.y + tdWidget.bottom - 12;

            // Warnings
            if (_loadedTrackDesign->gameStateData.hasFlag(TrackDesignGameStateFlag::VehicleUnavailable)
                && gLegacyScene != LegacyScene::trackDesignsManager)
            {
                // Vehicle design not available
                drawTextEllipsised(rt, screenPos, 368, STR_VEHICLE_DESIGN_UNAVAILABLE, { TextAlignment::centre });
                screenPos.y -= kScrollableRowHeight;
            }

            if (_loadedTrackDesign->gameStateData.hasFlag(TrackDesignGameStateFlag::SceneryUnavailable))
            {
                if (!gTrackDesignSceneryToggle)
                {
                    // Scenery not available
                    drawTextEllipsised(
                        rt, screenPos, 368, STR_DESIGN_INCLUDES_SCENERY_WHICH_IS_UNAVAILABLE, { TextAlignment::centre });
                    screenPos.y -= kScrollableRowHeight;
                }
            }

            // Track design name
            auto ft = Formatter();
            ft.Add<const utf8*>(_trackDesigns[trackIndex].name.c_str());
            drawTextEllipsised(rt, screenPos, 368, STR_TRACK_PREVIEW_NAME_FORMAT, ft, { TextAlignment::centre });

            // OPENRCT2MINI: ride statistics now live in the WIDX_STATS_SCROLL
            // scroll widget; see drawStats() / computeStatsHeight() and
            // onScrollDraw / onScrollGetSize for that path.
        }

        // OPENRCT2MINI: Helper that draws the ride statistics into a render
        // target at the given relative origin. Used by onScrollDraw to
        // populate WIDX_STATS_SCROLL.
        void drawStats(RenderTarget& rt, ScreenCoordsXY origin) const
        {
            if (!_loadedTrackDesign)
                return;

            ScreenCoordsXY screenPos = origin;

            auto ft = Formatter();
            ft.Add<fixed32_2dp>(_loadedTrackDesign->statistics.ratings.excitement);
            drawText(rt, screenPos, STR_TRACK_LIST_EXCITEMENT_RATING, ft);
            screenPos.y += kListRowHeight;

            ft = Formatter();
            ft.Add<fixed32_2dp>(_loadedTrackDesign->statistics.ratings.intensity);
            drawText(rt, screenPos, STR_TRACK_LIST_INTENSITY_RATING, ft);
            screenPos.y += kListRowHeight;

            ft = Formatter();
            ft.Add<fixed32_2dp>(_loadedTrackDesign->statistics.ratings.nausea);
            drawText(rt, screenPos, STR_TRACK_LIST_NAUSEA_RATING, ft);
            screenPos.y += kListRowHeight + 4;

            const auto& rtd = GetRideTypeDescriptor(_loadedTrackDesign->trackAndVehicle.rtdIndex);
            if (rtd.flags.has(RtdFlag::hasTrack))
            {
                if (rtd.specialType != RtdSpecialType::maze)
                {
                    if (rtd.specialType == RtdSpecialType::miniGolf)
                    {
                        ft = Formatter();
                        ft.Add<uint16_t>(_loadedTrackDesign->statistics.holes);
                        drawText(rt, screenPos, STR_HOLES, ft);
                        screenPos.y += kListRowHeight;
                    }
                    else
                    {
                        ft = Formatter();
                        ft.Add<uint16_t>(ToHumanReadableSpeed(_loadedTrackDesign->statistics.maxSpeed << 16));
                        drawText(rt, screenPos, STR_MAX_SPEED, ft);
                        screenPos.y += kListRowHeight;

                        ft = Formatter();
                        ft.Add<uint16_t>(ToHumanReadableSpeed(_loadedTrackDesign->statistics.averageSpeed << 16));
                        drawText(rt, screenPos, STR_AVERAGE_SPEED, ft);
                        screenPos.y += kListRowHeight;
                    }

                    ft = Formatter();
                    ft.Add<StringId>(STR_RIDE_LENGTH_ENTRY);
                    ft.Add<uint16_t>(_loadedTrackDesign->statistics.rideLength);
                    drawTextEllipsised(rt, screenPos, 214, STR_TRACK_LIST_RIDE_LENGTH, ft);
                    screenPos.y += kListRowHeight;
                }

                if (rtd.flags.has(RtdFlag::hasGForces))
                {
                    ft = Formatter();
                    ft.Add<int32_t>(_loadedTrackDesign->statistics.maxPositiveVerticalG);
                    drawText(rt, screenPos, STR_MAX_POSITIVE_VERTICAL_G, ft);
                    screenPos.y += kListRowHeight;

                    ft = Formatter();
                    ft.Add<int32_t>(_loadedTrackDesign->statistics.maxNegativeVerticalG);
                    drawText(rt, screenPos, STR_MAX_NEGATIVE_VERTICAL_G, ft);
                    screenPos.y += kListRowHeight;

                    ft = Formatter();
                    ft.Add<int32_t>(_loadedTrackDesign->statistics.maxLateralG);
                    drawText(rt, screenPos, STR_MAX_LATERAL_G, ft);
                    screenPos.y += kListRowHeight;

                    if (_loadedTrackDesign->statistics.totalAirTime != 0)
                    {
                        ft = Formatter();
                        ft.Add<int32_t>(ToHumanReadableAirTime(_loadedTrackDesign->statistics.totalAirTime));
                        drawText(rt, screenPos, STR_TOTAL_AIR_TIME, ft);
                        screenPos.y += kListRowHeight;
                    }
                }

                if (rtd.flags.has(RtdFlag::hasDrops))
                {
                    ft = Formatter();
                    ft.Add<uint16_t>(_loadedTrackDesign->statistics.drops);
                    drawText(rt, screenPos, STR_DROPS, ft);
                    screenPos.y += kListRowHeight;

                    ft = Formatter();
                    ft.Add<uint16_t>((_loadedTrackDesign->statistics.highestDropHeight * 3) / 4);
                    drawText(rt, screenPos, STR_HIGHEST_DROP_HEIGHT, ft);
                    screenPos.y += kListRowHeight;
                }

                if (_loadedTrackDesign->statistics.inversions != 0)
                {
                    ft = Formatter();
                    ft.Add<uint16_t>(_loadedTrackDesign->statistics.inversions);
                    drawText(rt, screenPos, STR_INVERSIONS, ft);
                    screenPos.y += kListRowHeight;
                }

                screenPos.y += 4;
            }

            if (!_loadedTrackDesign->statistics.spaceRequired.IsNull())
            {
                ft = Formatter();
                ft.Add<uint16_t>(_loadedTrackDesign->statistics.spaceRequired.x);
                ft.Add<uint16_t>(_loadedTrackDesign->statistics.spaceRequired.y);
                drawText(rt, screenPos, STR_TRACK_LIST_SPACE_REQUIRED, ft);
                screenPos.y += kListRowHeight;
            }

            if (_loadedTrackDesign->gameStateData.cost != 0)
            {
                ft = Formatter();
                ft.Add<uint32_t>(_loadedTrackDesign->gameStateData.cost);
                drawText(rt, screenPos, STR_TRACK_LIST_COST_AROUND, ft);
            }
        }

        // OPENRCT2MINI: Mirrors drawStats's conditional structure exactly,
        // accumulating heights instead of drawing. Used by onScrollGetSize so
        // the scroll bar reflects the actual content size for the loaded
        // design.
        int32_t computeStatsHeight() const
        {
            if (!_loadedTrackDesign)
                return 0;

            int32_t h = 0;
            h += kListRowHeight * 3 + 4; // excitement, intensity, nausea + gap

            const auto& rtd = GetRideTypeDescriptor(_loadedTrackDesign->trackAndVehicle.rtdIndex);
            if (rtd.flags.has(RtdFlag::hasTrack))
            {
                if (rtd.specialType != RtdSpecialType::maze)
                {
                    if (rtd.specialType == RtdSpecialType::miniGolf)
                        h += kListRowHeight; // holes
                    else
                        h += kListRowHeight * 2; // max speed + avg speed
                    h += kListRowHeight; // ride length
                }
                if (rtd.flags.has(RtdFlag::hasGForces))
                {
                    h += kListRowHeight * 3; // max+, max-, max lateral
                    if (_loadedTrackDesign->statistics.totalAirTime != 0)
                        h += kListRowHeight;
                }
                if (rtd.flags.has(RtdFlag::hasDrops))
                    h += kListRowHeight * 2; // drops + drop height
                if (_loadedTrackDesign->statistics.inversions != 0)
                    h += kListRowHeight;
                h += 4;
            }

            if (!_loadedTrackDesign->statistics.spaceRequired.IsNull())
                h += kListRowHeight; // space required
            if (_loadedTrackDesign->gameStateData.cost != 0)
                h += kListRowHeight; // cost

            return h;
        }

        void onScrollDraw(const int32_t scrollIndex, RenderTarget& rt) override
        {
            auto paletteIndex = getColourMap(colours[0].colour).midLight;
            GfxClear(rt, paletteIndex);

            // OPENRCT2MINI: scroll widget 1 hosts the ride statistics text.
            if (scrollIndex == 1)
            {
                drawStats(rt, ScreenCoordsXY{ 2, 1 });
                return;
            }

            auto screenCoords = ScreenCoordsXY{ 0, 0 };
            size_t listIndex = 0;
            if (gLegacyScene == LegacyScene::trackDesignsManager)
            {
                if (_trackDesigns.empty())
                {
                    // No track designs
                    drawText(rt, screenCoords - ScreenCoordsXY{ 0, 1 }, STR_NO_TRACK_DESIGNS_OF_THIS_TYPE);
                    return;
                }
            }
            else
            {
                // Build custom track item
                StringId stringId;
                if (listIndex == static_cast<size_t>(selectedListItem))
                {
                    // Highlight
                    Rectangle::filter(
                        rt, { screenCoords, { width, screenCoords.y + kScrollableRowHeight - 1 } },
                        FilterPaletteID::paletteDarken1);
                    stringId = STR_WINDOW_COLOUR_2_STRINGID;
                }
                else
                {
                    stringId = STR_BLACK_STRING;
                }

                auto ft = Formatter();
                ft.Add<StringId>(STR_BUILD_CUSTOM_DESIGN);
                drawText(rt, screenCoords - ScreenCoordsXY{ 0, 1 }, stringId, ft);
                screenCoords.y += kScrollableRowHeight;
                listIndex++;
            }

            for (auto i : _filteredTrackIds)
            {
                if (screenCoords.y + kScrollableRowHeight >= rt.y && screenCoords.y < rt.y + rt.height)
                {
                    StringId stringId;
                    if (listIndex == static_cast<size_t>(selectedListItem))
                    {
                        // Highlight
                        Rectangle::filter(
                            rt, { screenCoords, { width, screenCoords.y + kScrollableRowHeight - 1 } },
                            FilterPaletteID::paletteDarken1);
                        stringId = STR_WINDOW_COLOUR_2_STRINGID;
                    }
                    else
                    {
                        stringId = STR_BLACK_STRING;
                    }

                    // Draw track name
                    auto ft = Formatter();
                    ft.Add<StringId>(STR_TRACK_LIST_NAME_FORMAT);
                    ft.Add<const utf8*>(_trackDesigns[i].name.c_str());
                    drawText(rt, screenCoords - ScreenCoordsXY{ 0, 1 }, stringId, ft);
                }

                screenCoords.y += kScrollableRowHeight;
                listIndex++;
            }
        }

        void setIsBeingUpdated(const bool beingUpdated)
        {
            _selectedItemIsBeingUpdated = beingUpdated;
        }

        void reloadTrackDesigns()
        {
            _reloadTrackDesigns = true;
        }
    };

    WindowBase* TrackListOpen(const RideSelection item)
    {
        auto* windowMgr = GetWindowManager();
        windowMgr->CloseConstructionWindows();

        WindowFlags flags = {};
        ScreenCoordsXY screenPos{};
        if (gLegacyScene == LegacyScene::trackDesignsManager)
        {
            flags = { WindowFlag::autoPosition, WindowFlag::centreScreen };
        }
        else
        {
            screenPos = { 0, kTopToolbarHeight + 2 };
        }

        return windowMgr->Create<TrackListWindow>(WindowClass::trackDesignList, screenPos, kWindowSize, flags, item);
    }

    void WindowTrackDesignListReloadTracks()
    {
        auto* windowMgr = GetWindowManager();
        auto* trackListWindow = static_cast<TrackListWindow*>(windowMgr->FindByClass(WindowClass::trackDesignList));
        if (trackListWindow != nullptr)
        {
            trackListWindow->reloadTrackDesigns();
        }
    }

    void WindowTrackDesignListSetBeingUpdated(const bool beingUpdated)
    {
        auto* windowMgr = GetWindowManager();
        auto* trackListWindow = static_cast<TrackListWindow*>(windowMgr->FindByClass(WindowClass::trackDesignList));
        if (trackListWindow != nullptr)
        {
            trackListWindow->setIsBeingUpdated(beingUpdated);
        }
    }
} // namespace OpenRCT2::Ui::Windows
