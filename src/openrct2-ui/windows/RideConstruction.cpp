/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <limits>
#include <openrct2-ui/ProvisionalElements.h>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/interface/Viewport.h>
#include <openrct2-ui/interface/ViewportInteraction.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/ride/Construction.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Cheats.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/Input.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/ResultWithMessage.h>
#include <openrct2/actions/ride/MazeSetTrackAction.h>
#include <openrct2/actions/ride/RideDemolishAction.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideSetStatusAction.h>
#include <openrct2/actions/track/TrackPlaceAction.h>
#include <openrct2/actions/track/TrackRemoveAction.h>
#include <openrct2/actions/track/TrackSetBrakeSpeedAction.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Numerics.hpp>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/network/Network.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/paint/VirtualFloor.h>
#include <openrct2/paint/tile_element/Paint.TileElement.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideConstruction.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/Track.h>
#include <openrct2/ride/TrackData.h>
#include <openrct2/ride/TrackIteration.h>
#include <openrct2/ride/ted/TrackElementDescriptor.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/windows/Intent.h>
#include <openrct2/world/ConstructionClearance.h>
#include <openrct2/world/Entrance.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/MapSelection.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/world/tile_element/TrackElement.h>

constexpr int8_t kDefaultSpeedIncrement = 2;
constexpr int8_t kDefaultMinimumSpeed = 2;

constexpr uint8_t kVerticalDropButtonStart = 6;

using namespace OpenRCT2::Numerics;
using namespace OpenRCT2::TrackMetadata;
using OpenRCT2::GameActions::CommandFlag;
using OpenRCT2::GameActions::CommandFlags;
using OpenRCT2::GameActions::MazeBuildMode;

namespace OpenRCT2::Ui::Windows
{
    bool gDisableErrorWindowSound = false;

    static RideConstructionState _rideConstructionState2;

    static bool WindowRideConstructionUpdateState(
        TrackElemType* trackType, int32_t* trackDirection, RideId* rideIndex,
        SelectedLiftAndInverted* _liftHillAndAlternativeState, CoordsXYZ* trackPos, int32_t* properties);
    money64 PlaceProvisionalTrackPiece(
        RideId rideIndex, TrackElemType trackType, int32_t trackDirection, SelectedLiftAndInverted liftHillAndAlternativeState,
        const CoordsXYZ& trackPos);
    static std::pair<bool, TrackElemType> WindowRideConstructionUpdateStateGetTrackElement();

    static constexpr StringId kWindowTitle = kStringIdNone;
    static constexpr ScreenSize kWindowSize = { 210, 394 };

    static constexpr uint16_t kArrowPulseDuration = 200;
    // Width of the group boxes, e.g. “Banking”
    static constexpr int32_t kGroupWidth = kWindowSize.width - 6;

#pragma region Widgets

    enum WindowRideConstructionWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_DIRECTION_GROUPBOX,
        WIDX_SLOPE_GROUPBOX,
        WIDX_BANKING_GROUPBOX,
        WIDX_LEFT_CURVE_VERY_SMALL,
        WIDX_LEFT_CURVE_SMALL,
        WIDX_LEFT_CURVE,
        WIDX_LEFT_CURVE_LARGE,
        WIDX_STRAIGHT,
        WIDX_RIGHT_CURVE_LARGE,
        WIDX_RIGHT_CURVE,
        WIDX_RIGHT_CURVE_SMALL,
        WIDX_RIGHT_CURVE_VERY_SMALL,
        WIDX_SPECIAL_TRACK_DROPDOWN,
        WIDX_SLOPE_DOWN_VERTICAL,
        WIDX_SLOPE_DOWN_STEEP,
        WIDX_SLOPE_DOWN,
        WIDX_LEVEL,
        WIDX_SLOPE_UP,
        WIDX_SLOPE_UP_STEEP,
        WIDX_SLOPE_UP_VERTICAL,
        WIDX_CHAIN_LIFT,
        WIDX_BANK_LEFT,
        WIDX_BANK_STRAIGHT,
        WIDX_BANK_RIGHT,
        WIDX_CONSTRUCT,
        WIDX_DEMOLISH,
        WIDX_PREVIOUS_SECTION,
        WIDX_NEXT_SECTION,
        WIDX_ENTRANCE_EXIT_GROUPBOX,
        WIDX_ENTRANCE,
        WIDX_EXIT,
        WIDX_ROTATE,
        WIDX_U_TRACK,
        WIDX_O_TRACK,
        WIDX_SEAT_ROTATION_GROUPBOX,
        WIDX_SEAT_ROTATION_ANGLE_SPINNER,
        WIDX_SEAT_ROTATION_ANGLE_SPINNER_UP,
        WIDX_SEAT_ROTATION_ANGLE_SPINNER_DOWN,
        WIDX_SIMULATE,
        WIDX_SPEED_GROUPBOX = WIDX_BANKING_GROUPBOX,
        WIDX_SPEED_SETTING_SPINNER = WIDX_BANK_LEFT,
        WIDX_SPEED_SETTING_SPINNER_UP = WIDX_BANK_STRAIGHT,
        WIDX_SPEED_SETTING_SPINNER_DOWN = WIDX_BANK_RIGHT,
    };

    VALIDATE_GLOBAL_WIDX(WC_RIDE_CONSTRUCTION, WIDX_CONSTRUCT);
    VALIDATE_GLOBAL_WIDX(WC_RIDE_CONSTRUCTION, WIDX_ENTRANCE);
    VALIDATE_GLOBAL_WIDX(WC_RIDE_CONSTRUCTION, WIDX_EXIT);
    VALIDATE_GLOBAL_WIDX(WC_RIDE_CONSTRUCTION, WIDX_ROTATE);

    // clang-format off
    static const auto kRideConstructionWidgets = makeWidgets(
        makeWindowShim(kWindowTitle, kWindowSize),
        makeWidget        ({  3,  17}, {     kGroupWidth,  57}, WidgetType::groupbox, WindowColour::primary  , STR_RIDE_CONSTRUCTION_DIRECTION                                                                       ),
        makeWidget        ({  3,  76}, {     kGroupWidth,  41}, WidgetType::groupbox, WindowColour::primary  , STR_RIDE_CONSTRUCTION_SLOPE                                                                           ),
        makeWidget        ({  3, 120}, {     kGroupWidth,  41}, WidgetType::groupbox, WindowColour::primary  , STR_RIDE_CONSTRUCTION_ROLL_BANKING                                                                    ),
        makeWidget        ({  6,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_LEFT_CURVE_SMALL),  STR_RIDE_CONSTRUCTION_LEFT_CURVE_VERY_SMALL_TIP     ),
        makeWidget        ({ 28,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_G2_ICON_MEDIUM_CURVE_LEFT),           STR_RIDE_CONSTRUCTION_LEFT_CURVE_SMALL_TIP          ),
        makeWidget        ({ 50,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_LEFT_CURVE),        STR_RIDE_CONSTRUCTION_LEFT_CURVE_TIP                ),
        makeWidget        ({ 72,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_LEFT_CURVE_LARGE),  STR_RIDE_CONSTRUCTION_LEFT_CURVE_LARGE_TIP          ),
        makeWidget        ({ 94,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_STRAIGHT),          STR_RIDE_CONSTRUCTION_STRAIGHT_TIP                  ),
        makeWidget        ({116,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_RIGHT_CURVE_LARGE), STR_RIDE_CONSTRUCTION_RIGHT_CURVE_LARGE_TIP         ),
        makeWidget        ({138,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_RIGHT_CURVE),       STR_RIDE_CONSTRUCTION_RIGHT_CURVE_TIP               ),
        makeWidget        ({160,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_G2_ICON_MEDIUM_CURVE_RIGHT),          STR_RIDE_CONSTRUCTION_RIGHT_CURVE_SMALL_TIP         ),
        makeWidget        ({182,  29}, {              22,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_RIGHT_CURVE_SMALL), STR_RIDE_CONSTRUCTION_RIGHT_CURVE_VERY_SMALL_TIP    ),
        makeWidget        ({  6,  55}, { kGroupWidth - 6,  14}, WidgetType::button,   WindowColour::secondary, kStringIdEmpty,                                   STR_RIDE_CONSTRUCTION_OTHER_TRACK_CONFIGURATIONS_TIP),
        makeWidget        ({  6,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_VERTICAL_DROP),     STR_RIDE_CONSTRUCTION_VERTICAL_DROP_TIP             ),
        makeWidget        ({ 30,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_DOWN_STEEP),  STR_RIDE_CONSTRUCTION_STEEP_SLOPE_DOWN_TIP          ),
        makeWidget        ({ 54,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_DOWN),        STR_RIDE_CONSTRUCTION_SLOPE_DOWN_TIP                ),
        makeWidget        ({ 78,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_LEVEL),       STR_RIDE_CONSTRUCTION_LEVEL_TIP                     ),
        makeWidget        ({102,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_UP),          STR_RIDE_CONSTRUCTION_SLOPE_UP_TIP                  ),
        makeWidget        ({126,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_UP_STEEP),    STR_RIDE_CONSTRUCTION_STEEP_SLOPE_UP_TIP            ),
        makeWidget        ({150,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_VERTICAL_RISE),     STR_RIDE_CONSTRUCTION_VERTICAL_RISE_TIP             ),
        makeWidget        ({178,  88}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_CHAIN_LIFT),                          STR_RIDE_CONSTRUCTION_CHAIN_LIFT_TIP                ),
        makeWidget        ({ 69, 132}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_LEFT_BANK),         STR_RIDE_CONSTRUCTION_ROLL_FOR_LEFT_CURVE_TIP       ),
        makeWidget        ({ 93, 132}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_NO_BANK),           STR_RIDE_CONSTRUCTION_NO_ROLL_TIP                   ),
        makeWidget        ({117, 132}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_RIGHT_BANK),        STR_RIDE_CONSTRUCTION_ROLL_FOR_RIGHT_CURVE_TIP      ),
        makeWidget        ({  3, 164}, {     kGroupWidth, 170}, WidgetType::imgBtn,   WindowColour::secondary, 0xFFFFFFFF,                                       STR_RIDE_CONSTRUCTION_CONSTRUCT_SELECTED_SECTION_TIP),
        makeWidget        ({ 82, 338}, {              46,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_DEMOLISH_CURRENT_SECTION),            STR_RIDE_CONSTRUCTION_REMOVE_HIGHLIGHTED_SECTION_TIP),
        makeWidget        ({ 52, 338}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_PREVIOUS),                            STR_RIDE_CONSTRUCTION_MOVE_TO_PREVIOUS_SECTION_TIP  ),
        makeWidget        ({134, 338}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_NEXT),                                STR_RIDE_CONSTRUCTION_MOVE_TO_NEXT_SECTION_TIP      ),
        makeWidget        ({  3, 362}, {     kGroupWidth,  28}, WidgetType::groupbox, WindowColour::primary                                                                                                          ),
        makeWidget        ({ 31, 372}, {              70,  12}, WidgetType::button,   WindowColour::secondary, STR_RIDE_CONSTRUCTION_ENTRANCE,                   STR_RIDE_CONSTRUCTION_ENTRANCE_TIP                  ),
        makeWidget        ({109, 372}, {              70,  12}, WidgetType::button,   WindowColour::secondary, STR_RIDE_CONSTRUCTION_EXIT,                       STR_RIDE_CONSTRUCTION_EXIT_TIP                      ),
        makeWidget        ({ 94, 338}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_ROTATE_ARROW),                        STR_ROTATE_90_TIP                                   ),
        makeWidget        ({ 41, 132}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_U_SHAPED_TRACK),    STR_RIDE_CONSTRUCTION_U_SHAPED_OPEN_TRACK_TIP       ),
        makeWidget        ({144, 132}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_RIDE_CONSTRUCTION_O_SHAPED_TRACK),    STR_RIDE_CONSTRUCTION_O_SHAPED_ENCLOSED_TRACK_TIP   ),
        makeWidget        ({118, 120}, {              89,  41}, WidgetType::groupbox, WindowColour::primary  , STR_RIDE_CONSTRUCTION_SEAT_ROT                                                                        ),
        makeSpinnerWidgets({123, 138}, {              58,  12}, WidgetType::spinner,  WindowColour::secondary, 0,                                                STR_RIDE_CONSTRUCTION_SELECT_SEAT_ROTATION_ANGLE_TIP),
        makeWidget        ({161, 338}, {              24,  24}, WidgetType::flatBtn,  WindowColour::secondary, ImageId(SPR_G2_SIMULATE),                         STR_SIMULATE_RIDE_TIP                               )
    );
    // clang-format on

#pragma endregion

    static bool _trackPlaceCtrlState;
    static int32_t _trackPlaceCtrlZ;
    static bool _trackPlaceShiftState;
    static ScreenCoordsXY _trackPlaceShiftStart;
    static int32_t _trackPlaceShiftZ;
    static int32_t _trackPlaceZ;
    static money64 _trackPlaceCost;
    static StringId _trackPlaceErrorMessage;
    static bool _autoRotatingShop;
    static bool _gotoStartPlacementMode = false;

    static constexpr StringId kSeatAngleRotationStrings[] = {
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_NEG_180, STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_NEG_135,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_NEG_90,  STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_NEG_45,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_0,       STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_45,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_90,      STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_135,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_180,     STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_225,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_270,     STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_315,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_360,     STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_405,
        STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_450,     STR_RIDE_CONSTRUCTION_SEAT_ROTATION_ANGLE_495,
    };

    static void WindowRideConstructionMouseUpDemolishNextPiece(const CoordsXYZD& piecePos, TrackElemType type);
    static void WindowRideConstructionUpdateActiveElements();

    /* move to ride.c */
    static void CloseRideWindowForConstruction(RideId rideId)
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByNumber(WindowClass::ride, rideId.ToUnderlying());
        if (w != nullptr && w->page == 1)
            windowMgr->Close(*w);
    }

    static void RideConstructPlacedForwardGameActionCallback(
        const GameActions::GameAction* ga, const GameActions::Result* result);
    static void RideConstructPlacedBackwardGameActionCallback(
        const GameActions::GameAction* ga, const GameActions::Result* result);
    static void CloseConstructWindowOnCompletion(const Ride& ride);

    class RideConstructionWindow final : public Window
    {
    private:
        uint8_t _currentlyShowingBrakeOrBoosterSpeed{};
        SpecialElementsDropdownState _specialElementDropdownState;
        bool _autoOpeningShop{};
        u8string _windowTitle{};
        u8string _specialDropdownText{};
        u8string _brakeSpeedText{};

    public:
        void onOpen() override
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            setWidgets(kRideConstructionWidgets);
            number = _currentRideIndex.ToUnderlying();

            initScrollWidgets();

            WindowPushOthersRight(*this);
            ShowGridlines();

            _currentTrackPrice = kMoney64Undefined;
            _currentBrakeSpeed = 8;
            _currentColourScheme = RideColourScheme::main;
            _currentSeatRotationAngle = 4;

            _currentlySelectedTrack = currentRide->getRideTypeDescriptor().StartTrackPiece;
            _currentTrackPitchEnd = TrackPitch::none;
            _currentTrackRollEnd = TrackRoll::none;
            _currentTrackHasLiftHill = false;
            _currentTrackAlternative.clearAll();

            if (currentRide->getRideTypeDescriptor().flags.has(RtdFlag::startConstructionInverted))
                _currentTrackAlternative.set(AlternativeTrackFlag::inverted);

            _previousTrackRollEnd = TrackRoll::none;
            _previousTrackPitchEnd = TrackPitch::none;

            _currentTrackPieceDirection = 0;
            _rideConstructionState = RideConstructionState::Place;
            _currentTrackSelectionFlags.clearAll();
            _autoOpeningShop = false;
            _autoRotatingShop = true;
            _trackPlaceCtrlState = false;
            _trackPlaceShiftState = false;

            UpdateTrackPieceWidgets();
        }

        void onClose() override
        {
            RideConstructionInvalidateCurrentTrack();
            ViewportSetVisibility(ViewportVisibility::standard);

            gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
            gMapSelectFlags.unset(MapSelectFlag::enableArrow);

            // In order to cancel the yellow arrow correctly the
            // selection tool should be cancelled. Don't do a tool cancel if
            // another window has already taken control of tool.
            if (isToolActive(classification, number))
                ToolCancel();

            HideGridlines();

            // If we demolish a currentRide all windows will be closed including the construction window,
            // the currentRide at this point is already gone.
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            auto& gameState = getGameState();

            if (RideTryGetOriginElement(*currentRide, nullptr))
            {
                // Auto open shops if required.
                if (currentRide->mode == RideMode::shopStall && Config::Get().general.autoOpenShops)
                {
                    // HACK: Until we find a good a way to defer the game command for opening the shop, stop this
                    //       from getting stuck in an infinite loop as opening the currentRide will try to close this window
                    if (!_autoOpeningShop)
                    {
                        _autoOpeningShop = true;
                        auto gameAction = GameActions::RideSetStatusAction(currentRide->id, RideStatus::open);
                        GameActions::Execute(&gameAction, gameState);
                        _autoOpeningShop = false;
                    }
                }

                auto intent = Intent(WindowClass::ride);
                intent.PutExtra(INTENT_EXTRA_RIDE_ID, currentRide->id.ToUnderlying());
                ContextOpenIntent(&intent);
            }
            else
            {
                auto gameAction = GameActions::RideDemolishAction(currentRide->id, GameActions::RideModifyType::demolish);
                gameAction.SetFlags({ CommandFlag::allowDuringPaused });
                GameActions::Execute(&gameAction, gameState);
            }
        }

        void UpdateTrackPieceWidgets()
        {
            WindowRideConstructionUpdateEnabledTrackPieces();

            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }
            const auto& rtd = currentRide->getRideTypeDescriptor();
            const auto currentTrackDrawerDescriptor = getCurrentTrackDrawerDescriptor(rtd);

            uint64_t newDisabledWidgets = 0;

            if (_rideConstructionState == RideConstructionState::Place)
            {
                newDisabledWidgets |= (1uLL << WIDX_CONSTRUCT);
            }

            if (_currentlySelectedTrack.isTrackType)
            {
                newDisabledWidgets |= (1uLL << WIDX_SLOPE_GROUPBOX) | (1uLL << WIDX_BANKING_GROUPBOX)
                    | (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL)
                    | (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP) | (1uLL << WIDX_CHAIN_LIFT)
                    | (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_STRAIGHT) | (1uLL << WIDX_BANK_RIGHT);
            }

            // Disable large curves if the start or end of the track is sloped and large sloped curves are not available
            if ((_previousTrackPitchEnd != TrackPitch::none || _currentTrackPitchEnd != TrackPitch::none))
            {
                if (!IsTrackEnabled(TrackGroup::slopeCurveLarge)
                    || !(_previousTrackPitchEnd == TrackPitch::up25 || _previousTrackPitchEnd == TrackPitch::down25)
                    || !(_currentTrackPitchEnd == TrackPitch::up25 || _currentTrackPitchEnd == TrackPitch::down25))
                {
                    newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_RIGHT_CURVE_LARGE);
                }
            }
            if (IsTrackEnabled(TrackGroup::slopeCurve) && IsTrackEnabled(TrackGroup::curveVerySmall))
            {
                // Disable small curves if the start or end of the track is sloped.
                if (_previousTrackPitchEnd != TrackPitch::none || _currentTrackPitchEnd != TrackPitch::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
                }
            }
            if (!IsTrackEnabled(TrackGroup::slopeCurve))
            {
                if (IsTrackEnabled(TrackGroup::curveVertical))
                {
                    // Disable all curves only on vertical track
                    if (_previousTrackPitchEnd != TrackPitch::up90 || _currentTrackPitchEnd != TrackPitch::up90)
                    {
                        if (_previousTrackPitchEnd != TrackPitch::down90 || _currentTrackPitchEnd != TrackPitch::down90)
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL)
                                | (1uLL << WIDX_LEFT_CURVE) | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_SMALL)
                                | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
                        }
                    }
                }
                else
                {
                    // Disable all curves on sloped track
                    if (_previousTrackPitchEnd != TrackPitch::none || _currentTrackPitchEnd != TrackPitch::none)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL)
                            | (1uLL << WIDX_LEFT_CURVE) | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_SMALL)
                            | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
                    }
                }
            }
            if (!IsTrackEnabled(TrackGroup::flatRollBanking))
            {
                // Disable banking
                newDisabledWidgets |= (1uLL << WIDX_BANKING_GROUPBOX) | (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_STRAIGHT)
                    | (1uLL << WIDX_BANK_RIGHT);
            }
            // Disable banking if the start track is steep and the end of the track becomes flat.
            if ((_previousTrackPitchEnd == TrackPitch::down60 || _previousTrackPitchEnd == TrackPitch::up60)
                && _currentTrackPitchEnd == TrackPitch::none)
            {
                newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
            }
            if (!IsTrackEnabled(TrackGroup::slope) && !IsTrackEnabled(TrackGroup::slopeSteepDown)
                && !IsTrackEnabled(TrackGroup::slopeSteepUp))
            {
                if (!currentRide->getRideTypeDescriptor().SupportsTrackGroup(TrackGroup::reverseFreefall))
                {
                    // Disable all slopes
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_GROUPBOX) | (1uLL << WIDX_SLOPE_DOWN_STEEP)
                        | (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP)
                        | (1uLL << WIDX_SLOPE_UP_STEEP);
                }
            }
            // If ride type does not have access to diagonal sloped turns, disallow simultaneous use of banked and sloped
            // diagonals
            if (!IsTrackEnabled(TrackGroup::slopeCurveLarge) && TrackPieceDirectionIsDiagonal(_currentTrackPieceDirection))
            {
                if (_currentTrackPitchEnd != TrackPitch::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                }
                else if (_currentTrackRollEnd != TrackRoll::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_SLOPE_UP);
                }
            }
            if (currentRide->getRideTypeDescriptor().flags.has(RtdFlag::upInclineRequiresLift)
                && !getGameState().cheats.enableAllDrawableTrackPieces)
            {
                // Disable lift hill toggle and banking if current track piece is uphill
                if (_previousTrackPitchEnd == TrackPitch::up25 || _previousTrackPitchEnd == TrackPitch::up60
                    || _currentTrackPitchEnd == TrackPitch::up25 || _currentTrackPitchEnd == TrackPitch::up60)
                    newDisabledWidgets |= 1uLL << WIDX_CHAIN_LIFT | (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                // Disable upward slope if current track piece is not flat
                if ((_previousTrackPitchEnd != TrackPitch::none || _previousTrackRollEnd != TrackRoll::none)
                    && !(_currentTrackHasLiftHill))
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP);
            }
            if (_rideConstructionState == RideConstructionState::State0)
            {
                newDisabledWidgets |= (1uLL << WIDX_CONSTRUCT) | (1uLL << WIDX_DEMOLISH) | (1uLL << WIDX_PREVIOUS_SECTION)
                    | (1uLL << WIDX_NEXT_SECTION);
            }
            if (!_currentlySelectedTrack.isTrackType)
            {
                switch (_currentlySelectedTrack.curve)
                {
                    case TrackCurve::leftVerySmall:
                    case TrackCurve::leftSmall:
                    case TrackCurve::left:
                    case TrackCurve::leftLarge:
                        newDisabledWidgets |= (1uLL << WIDX_BANK_RIGHT);
                        if (_previousTrackRollEnd == TrackRoll::none)
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT);
                        }
                        else
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_STRAIGHT);
                        }
                        break;
                    case TrackCurve::rightLarge:
                    case TrackCurve::right:
                    case TrackCurve::rightSmall:
                    case TrackCurve::rightVerySmall:
                        newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT);
                        if (_previousTrackRollEnd == TrackRoll::none)
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_RIGHT);
                        }
                        else
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_STRAIGHT);
                        }
                        break;
                    default:
                        break;
                }
            }
            if (!IsTrackEnabled(TrackGroup::slopeRollBanking))
            {
                if (_currentTrackRollEnd != TrackRoll::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_SLOPE_UP);
                }
            }
            if (_previousTrackPitchEnd == _currentTrackPitchEnd)
            {
                switch (_currentTrackPitchEnd)
                {
                    case TrackPitch::up60:
                    case TrackPitch::down60:
                        newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                            | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
                        if (!IsTrackEnabled(TrackGroup::slopeCurveSteep))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE_SMALL);
                        }
                        break;
                    case TrackPitch::up90:
                    case TrackPitch::down90:
                        newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                            | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
                        if (!IsTrackEnabled(TrackGroup::curveVertical))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE_SMALL);
                        }
                        break;
                    default:
                        break;
                }
            }
            else
            {
                // Disable all curves
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL)
                    | (1uLL << WIDX_LEFT_CURVE) | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_SMALL)
                    | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
            }

            if (TrackPieceDirectionIsDiagonal(_currentTrackPieceDirection))
            {
                switch (_previousTrackPitchEnd)
                {
                    case TrackPitch::none:
                        if (!IsTrackEnabled(TrackGroup::diagSlope))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_DOWN);
                        }
                        if (_currentlySelectedTrack != TrackCurve::none
                            || (!IsTrackEnabled(TrackGroup::flatToSteepSlope)
                                && !IsTrackEnabled(TrackGroup::diagSlopeSteepLong)))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        }
                        break;
                    case TrackPitch::down25:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        if (!IsTrackEnabled(TrackGroup::diagSlopeSteepDown))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP);
                        }
                        break;
                    case TrackPitch::down60:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        if (!IsTrackEnabled(TrackGroup::flatToSteepSlope) && !IsTrackEnabled(TrackGroup::diagSlopeSteepLong))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                        break;
                    case TrackPitch::up25:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN);
                        if (!IsTrackEnabled(TrackGroup::diagSlopeSteepUp))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP_STEEP);
                        }
                        break;
                    case TrackPitch::up60:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN);
                        if (!IsTrackEnabled(TrackGroup::flatToSteepSlope) && !IsTrackEnabled(TrackGroup::diagSlopeSteepLong))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                        break;
                    case TrackPitch::down90:
                    case TrackPitch::up90:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP);
                        break;
                }
            }
            else
            {
                switch (_previousTrackPitchEnd)
                {
                    case TrackPitch::none:
                        if (_currentlySelectedTrack != TrackCurve::none
                            || (!IsTrackEnabled(TrackGroup::flatToSteepSlope) && !IsTrackEnabled(TrackGroup::slopeSteepLong)))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        }
                        break;
                    case TrackPitch::down25:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        if (!IsTrackEnabled(TrackGroup::slopeSteepDown))
                        {
                            newDisabledWidgets |= 1uLL << WIDX_SLOPE_DOWN_STEEP;
                        }
                        break;
                    case TrackPitch::down60:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                        if (!IsTrackEnabled(TrackGroup::flatToSteepSlope) && !IsTrackEnabled(TrackGroup::slopeSteepLong))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                        break;
                    case TrackPitch::up25:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN);
                        if (!IsTrackEnabled(TrackGroup::slopeSteepUp))
                        {
                            newDisabledWidgets |= 1uLL << WIDX_SLOPE_UP_STEEP;
                        }
                        break;
                    case TrackPitch::up60:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN);
                        if (!IsTrackEnabled(TrackGroup::flatToSteepSlope) && !IsTrackEnabled(TrackGroup::slopeSteepLong))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                        break;
                    case TrackPitch::down90:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP)
                            | (1uLL << WIDX_SLOPE_UP_STEEP);
                        break;
                    case TrackPitch::up90:
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL)
                            | (1uLL << WIDX_SLOPE_UP);
                        break;
                }
                if (IsTrackEnabled(TrackGroup::slopeVertical))
                {
                    if (_previousTrackPitchEnd == TrackPitch::up60 && _currentTrackPieceDirection < 4)
                    {
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP_VERTICAL);
                    }
                    if (_previousTrackPitchEnd == TrackPitch::up90)
                    {
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP_VERTICAL);
                    }
                    if (_previousTrackPitchEnd == TrackPitch::down60 && _currentTrackPieceDirection < 4)
                    {
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN_VERTICAL);
                    }
                }
            }

            if (TrackPieceDirectionIsDiagonal(_currentTrackPieceDirection)
                && (_currentlySelectedTrack == TrackCurve::leftLarge || _currentlySelectedTrack == TrackCurve::rightLarge)
                && (_previousTrackPitchEnd == TrackPitch::up25 || _previousTrackPitchEnd == TrackPitch::down25)
                && !IsTrackEnabled(TrackGroup::diagSlope))
            {
                newDisabledWidgets |= 1uLL << WIDX_STRAIGHT;
            }

            if (_previousTrackRollEnd == TrackRoll::left)
            {
                newDisabledWidgets |= (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE)
                    | (1uLL << WIDX_RIGHT_CURVE_LARGE) | (1uLL << WIDX_BANK_RIGHT);
            }
            if (_previousTrackRollEnd == TrackRoll::right)
            {
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                    | (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_BANK_LEFT);
            }
            if (_currentTrackRollEnd != _previousTrackRollEnd)
            {
                newDisabledWidgets |= (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE)
                    | (1uLL << WIDX_RIGHT_CURVE_LARGE) | (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                    | (1uLL << WIDX_LEFT_CURVE_LARGE);
            }
            if (_currentTrackPitchEnd != TrackPitch::none)
            {
                if (IsTrackEnabled(TrackGroup::slopeRollBanking))
                {
                    if (_previousTrackPitchEnd == TrackPitch::none)
                    {
                        if (_currentTrackPitchEnd != TrackPitch::up25 && _currentTrackPitchEnd != TrackPitch::down25)
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                        }
                    }
                    else
                    {
                        if (_currentTrackPitchEnd != _previousTrackPitchEnd)
                        {
                            newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                        }
                        else
                        {
                            if (_currentTrackPitchEnd != TrackPitch::up25 && _currentTrackPitchEnd != TrackPitch::down25)
                            {
                                newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                            }
                        }
                    }
                }
                else
                {
                    newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                }
            }
            if (_currentTrackRollEnd != TrackRoll::none || _previousTrackRollEnd != TrackRoll::none)
            {
                newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_UP_STEEP)
                    | (1uLL << WIDX_CHAIN_LIFT);
            }
            if (_currentlySelectedTrack != TrackCurve::none)
            {
                if (!IsTrackEnabled(TrackGroup::liftHillCurve))
                {
                    newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
                }
                if (_currentTrackPitchEnd == TrackPitch::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
                }
                if (_currentTrackPitchEnd == TrackPitch::up60)
                {
                    newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
                }
                if (_currentTrackPitchEnd == TrackPitch::down60)
                {
                    newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
                }
                // Ensures that you can build an "unbanking turn", but not select a turn that start banked and
                // turns to level (as that does not exist).
                if (_currentTrackRollEnd != TrackRoll::none && _currentTrackPitchEnd != TrackPitch::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                }
            }
            if (_currentTrackPitchEnd == TrackPitch::up90 || _previousTrackPitchEnd == TrackPitch::up90)
            {
                newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
            }
            if (!IsTrackEnabled(TrackGroup::liftHillSteep))
            {
                if (_previousTrackPitchEnd == TrackPitch::up60 || _currentTrackPitchEnd == TrackPitch::up60)
                {
                    newDisabledWidgets |= (1uLL << WIDX_CHAIN_LIFT);
                }
            }
            if (_previousTrackRollEnd == TrackRoll::upsideDown)
            {
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                    | (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_STRAIGHT) | (1uLL << WIDX_RIGHT_CURVE_SMALL)
                    | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_LARGE);
            }
            if (_currentlySelectedTrack != TrackCurve::none)
            {
                if (_currentTrackPitchEnd == TrackPitch::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_SLOPE_UP);
                }
                if (_currentTrackPitchEnd == _previousTrackPitchEnd)
                {
                    if (_currentTrackPitchEnd == TrackPitch::up25)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP_STEEP);
                        if (_currentlySelectedTrack == TrackCurve::left || _currentlySelectedTrack == TrackCurve::right
                            || _rideConstructionState != RideConstructionState::Back
                            || !IsTrackEnabled(TrackGroup::slopeCurveBanked))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                    }
                    if (_currentTrackPitchEnd == TrackPitch::down25)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP);
                        if (_currentlySelectedTrack == TrackCurve::left || _currentlySelectedTrack == TrackCurve::right
                            || _rideConstructionState != RideConstructionState::Front
                            || !IsTrackEnabled(TrackGroup::slopeCurveBanked))
                        {
                            newDisabledWidgets |= (1uLL << WIDX_LEVEL);
                        }
                    }
                }
                else if (IsTrackEnabled(TrackGroup::slopeCurveBanked))
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_UP_STEEP);
                    if (_currentTrackRollEnd == TrackRoll::left)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_BANK_STRAIGHT) | (1uLL << WIDX_BANK_RIGHT);
                        newDisabledWidgets &= ~(1uLL << WIDX_BANK_LEFT);
                    }
                    if (_currentTrackRollEnd == TrackRoll::right)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_STRAIGHT);
                        newDisabledWidgets &= ~(1uLL << WIDX_BANK_RIGHT);
                    }
                    if (_currentTrackRollEnd == TrackRoll::none)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_RIGHT);
                        newDisabledWidgets &= ~(1uLL << WIDX_BANK_STRAIGHT);
                    }
                    if (_currentTrackPitchEnd == TrackPitch::none)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_SLOPE_UP);
                        newDisabledWidgets &= ~(1uLL << WIDX_LEVEL);
                    }
                    if (_currentTrackPitchEnd == TrackPitch::up25)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN) | (1uLL << WIDX_LEVEL);
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP);
                    }
                    if (_currentTrackPitchEnd == TrackPitch::down25)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP);
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN);
                    }
                    if (_currentlySelectedTrack == TrackCurve::leftSmall)
                    {
                        newDisabledWidgets &= ~(1uLL << WIDX_LEFT_CURVE_SMALL);
                    }
                    if (_currentlySelectedTrack == TrackCurve::rightSmall)
                    {
                        newDisabledWidgets &= ~(1uLL << WIDX_RIGHT_CURVE_SMALL);
                    }
                }
            }
            if (_currentlySelectedTrack != TrackCurve::none && _currentTrackPitchEnd == TrackPitch::up60)
            {
                newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP);
            }
            if (_currentlySelectedTrack != TrackCurve::none && _currentTrackPitchEnd == TrackPitch::down60)
            {
                newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN);
            }
            if ((_currentTrackHasLiftHill) && !getGameState().cheats.enableChainLiftOnAllTrack)
            {
                if (_currentTrackPitchEnd != TrackPitch::none && !IsTrackEnabled(TrackGroup::liftHillCurve))
                {
                    newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE)
                        | (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE)
                        | (1uLL << WIDX_RIGHT_CURVE_LARGE);
                }
                if (!IsTrackEnabled(TrackGroup::liftHillSteep))
                {
                    if (widgets[WIDX_SLOPE_UP_STEEP].tooltip == STR_RIDE_CONSTRUCTION_STEEP_SLOPE_UP_TIP)
                    {
                        newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP_STEEP);
                    }
                }
            }
            if (_previousTrackPitchEnd == TrackPitch::up60 && _currentlySelectedTrack != TrackCurve::none)
            {
                newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_LEVEL);
            }
            if (_previousTrackPitchEnd == TrackPitch::down60 && _currentlySelectedTrack != TrackCurve::none)
            {
                newDisabledWidgets |= (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP_STEEP);
            }
            if (_currentTrackPitchEnd == TrackPitch::up90 || _previousTrackPitchEnd == TrackPitch::up90)
            {
                if (_currentlySelectedTrack != TrackCurve::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_UP_STEEP);
                }
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_RIGHT_CURVE_LARGE)
                    | (1uLL << WIDX_SLOPE_DOWN_STEEP);
                if (currentRide->getRideTypeDescriptor().SupportsTrackGroup(TrackGroup::reverseFreefall))
                {
                    newDisabledWidgets |= (1uLL << WIDX_STRAIGHT) | (1uLL << WIDX_RIGHT_CURVE)
                        | (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE);
                }
            }
            else if (_currentTrackPitchEnd == TrackPitch::down90 || _previousTrackPitchEnd == TrackPitch::down90)
            {
                if (_currentlySelectedTrack != TrackCurve::none)
                {
                    newDisabledWidgets |= (1uLL << WIDX_SLOPE_DOWN_STEEP);
                }
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_RIGHT_CURVE_LARGE)
                    | (1uLL << WIDX_SLOPE_UP_STEEP);
                if (currentRide->getRideTypeDescriptor().SupportsTrackGroup(TrackGroup::reverseFreefall))
                {
                    newDisabledWidgets |= (1uLL << WIDX_STRAIGHT) | (1uLL << WIDX_RIGHT_CURVE)
                        | (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL) | (1uLL << WIDX_LEFT_CURVE);
                }
            }
            // If the previous track is flat and the next track is flat, attempt to show buttons for helixes
            if (_currentTrackPitchEnd == TrackPitch::none && _currentTrackPitchEnd == _previousTrackPitchEnd)
            {
                // If the bank is none, attempt to show unbanked quarter helixes
                if (_currentTrackRollEnd == TrackRoll::none
                    && (_currentlySelectedTrack == TrackCurve::left || _currentlySelectedTrack == TrackCurve::right))
                {
                    if (IsTrackEnabled(TrackGroup::helixDownUnbankedQuarter))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN_STEEP);
                    if (IsTrackEnabled(TrackGroup::helixUpUnbankedQuarter))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP_STEEP);
                }
                // If the track is banked left or right and curvature is standard size (2.5 tile radius), attempt to show
                // buttons for half or quarter helixes
                else if (
                    (_currentTrackRollEnd == TrackRoll::left || _currentTrackRollEnd == TrackRoll::right)
                    && (_currentlySelectedTrack == TrackCurve::left || _currentlySelectedTrack == TrackCurve::right))
                {
                    if (IsTrackEnabled(TrackGroup::helixDownBankedHalf) || IsTrackEnabled(TrackGroup::helixDownBankedQuarter))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN_STEEP);
                    if (IsTrackEnabled(TrackGroup::helixUpBankedHalf) || IsTrackEnabled(TrackGroup::helixUpBankedQuarter))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP_STEEP);
                }
                // If the track is banked left or right and curvature is small size (1.5 tile radius), attempt to show buttons
                // for half helixes
                else if (
                    (_currentTrackRollEnd == TrackRoll::left || _currentTrackRollEnd == TrackRoll::right)
                    && (_currentlySelectedTrack == TrackCurve::leftSmall || _currentlySelectedTrack == TrackCurve::rightSmall))
                {
                    if (IsTrackEnabled(TrackGroup::helixDownBankedHalf))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN_STEEP);
                    if (IsTrackEnabled(TrackGroup::helixUpBankedHalf))
                        newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP_STEEP);
                }
            }
            if (IsTrackEnabled(TrackGroup::slopeCurveBanked))
            {
                if (_rideConstructionState == RideConstructionState::Front)
                {
                    if (_currentlySelectedTrack == TrackCurve::leftSmall || _currentlySelectedTrack == TrackCurve::rightSmall)
                    {
                        if (_currentTrackPitchEnd == TrackPitch::none && _previousTrackRollEnd != TrackRoll::none
                            && (!currentRide->getRideTypeDescriptor().flags.has(RtdFlag::upInclineRequiresLift)
                                || getGameState().cheats.enableAllDrawableTrackPieces))
                        {
                            newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_UP);
                        }
                    }
                }
                else if (_rideConstructionState == RideConstructionState::Back)
                {
                    if (_currentlySelectedTrack == TrackCurve::leftSmall || _currentlySelectedTrack == TrackCurve::rightSmall)
                    {
                        if (_currentTrackPitchEnd == TrackPitch::none && _previousTrackRollEnd != TrackRoll::none)
                        {
                            newDisabledWidgets &= ~(1uLL << WIDX_SLOPE_DOWN);
                        }
                    }
                }
            }
            if (_currentTrackPieceDirection >= 4)
            {
                newDisabledWidgets |= (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL)
                    | (1uLL << WIDX_LEFT_CURVE) | (1uLL << WIDX_RIGHT_CURVE) | (1uLL << WIDX_RIGHT_CURVE_SMALL)
                    | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL);
            }
            if (_rideConstructionState == RideConstructionState::Front)
            {
                newDisabledWidgets |= (1uLL << WIDX_NEXT_SECTION);
                if (WindowRideConstructionUpdateState(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
                {
                    newDisabledWidgets |= (1uLL << WIDX_CONSTRUCT);
                }
            }
            else if (_rideConstructionState == RideConstructionState::Back)
            {
                newDisabledWidgets |= (1uLL << WIDX_PREVIOUS_SECTION);
                if (WindowRideConstructionUpdateState(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
                {
                    newDisabledWidgets |= (1uLL << WIDX_CONSTRUCT);
                }
            }
            if (currentTrackDrawerDescriptor.HasCoveredPieces())
            {
                newDisabledWidgets &= ~(1uLL << WIDX_BANKING_GROUPBOX);
            }
            if (_rideConstructionState == RideConstructionState::EntranceExit
                || _rideConstructionState == RideConstructionState::Selected)
            {
                newDisabledWidgets |= (1uLL << WIDX_DIRECTION_GROUPBOX) | (1uLL << WIDX_SLOPE_GROUPBOX)
                    | (1uLL << WIDX_BANKING_GROUPBOX) | (1uLL << WIDX_LEFT_CURVE_VERY_SMALL) | (1uLL << WIDX_LEFT_CURVE_SMALL)
                    | (1uLL << WIDX_LEFT_CURVE) | (1uLL << WIDX_STRAIGHT) | (1uLL << WIDX_RIGHT_CURVE)
                    | (1uLL << WIDX_RIGHT_CURVE_SMALL) | (1uLL << WIDX_RIGHT_CURVE_VERY_SMALL)
                    | (1uLL << WIDX_SPECIAL_TRACK_DROPDOWN) | (1uLL << WIDX_SLOPE_DOWN_STEEP) | (1uLL << WIDX_SLOPE_DOWN)
                    | (1uLL << WIDX_LEVEL) | (1uLL << WIDX_SLOPE_UP) | (1uLL << WIDX_SLOPE_UP_STEEP) | (1uLL << WIDX_CHAIN_LIFT)
                    | (1uLL << WIDX_BANK_LEFT) | (1uLL << WIDX_BANK_STRAIGHT) | (1uLL << WIDX_BANK_RIGHT)
                    | (1uLL << WIDX_LEFT_CURVE_LARGE) | (1uLL << WIDX_RIGHT_CURVE_LARGE);
            }
            if (_currentlyShowingBrakeOrBoosterSpeed)
            {
                newDisabledWidgets &= ~(1uLL << WIDX_BANKING_GROUPBOX);
                newDisabledWidgets &= ~(1uLL << WIDX_BANK_LEFT);
                newDisabledWidgets &= ~(1uLL << WIDX_BANK_STRAIGHT);
                newDisabledWidgets &= ~(1uLL << WIDX_BANK_RIGHT);
            }

            // If chain lift cheat is enabled then show the chain lift widget no matter what
            if (getGameState().cheats.enableChainLiftOnAllTrack)
            {
                newDisabledWidgets &= ~(1uLL << WIDX_CHAIN_LIFT);
            }

            // Invalidate only widgets whose disabled state changes.
            const auto widgetCount = static_cast<WidgetIndex>(widgets.size());
            for (WidgetIndex i = 0; i < widgetCount && i < 64; i++)
            {
                const bool shouldBeDisabled = (newDisabledWidgets & (1uLL << i)) != 0;
                if (isWidgetDisabled(i) != shouldBeDisabled)
                {
                    setWidgetDisabled(i, shouldBeDisabled);
                    invalidateWidget(i);
                }
            }
        }

        void onUpdate() override
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            // Close construction window if currentRide is not closed,
            // editing currentRide while open will cause many issues until properly handled
            if (currentRide->status != RideStatus::closed && currentRide->status != RideStatus::simulating)
            {
                close();
                return;
            }

            if (_currentlySelectedTrack.isTrackType)
            {
                switch (_currentlySelectedTrack.trackType)
                {
                    case TrackElemType::spinningTunnel:
                    case TrackElemType::whirlpool:
                    case TrackElemType::rapids:
                    case TrackElemType::waterfall:
                        invalidateWidget(WIDX_CONSTRUCT);
                        break;
                    default:
                        break;
                }
            }

            if (_rideConstructionState == RideConstructionState::Place)
            {
                if (!isToolActive(*this, WIDX_CONSTRUCT))
                {
                    close();
                    return;
                }
            }

            if (_rideConstructionState == RideConstructionState::EntranceExit)
            {
                if (!isToolActive(*this, WIDX_ENTRANCE) && !isToolActive(*this, WIDX_EXIT))
                {
                    _rideConstructionState = gRideEntranceExitPlacePreviousRideConstructionState;
                    WindowRideConstructionUpdateActiveElements();
                }
            }

            switch (_rideConstructionState)
            {
                case RideConstructionState::Front:
                case RideConstructionState::Back:
                case RideConstructionState::Selected:
                    if (isToolActive(WindowClass::rideConstruction))
                    {
                        // OPENRCT2MINI ride-construction-grid-cursor-plan
                        // §9 follow-up (2026-05-25): upstream cancels the
                        // construct tool every frame in Front/Back/Selected
                        // because the mouse path doesn't need the map tool
                        // armed in those states (user extends track via the
                        // WIDX_CONSTRUCT button click, not via map clicks).
                        // For the gamepad path we DO need the tool armed so
                        // resolveActiveContext can route to toolRideConstruction
                        // for grid-cursor mode. Gate the cancel on selector
                        // mode: only cancel when the user is mouse-driving
                        // (selector hidden); skip when in Focus / grid cursor
                        // mode (selector active) so the gamepad engage
                        // sticks. Diagnostic from user 2026-05-25 confirmed
                        // engage block fires correctly but this onUpdate
                        // cancel immediately undoes it, leaving the user
                        // stuck in widgetFocus.
                        if (OpenRCT2::Ui::GetInputManager().getSelectorMode()
                            == OpenRCT2::Ui::InputManager::SelectorMode::hidden)
                        {
                            ToolCancel();
                        }
                    }
                    break;
                default:
                    break;
            }

            UpdateGhostTrackAndArrow();

            UpdateTrackPieceWidgets();
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            WindowRideConstructionUpdateEnabledTrackPieces();
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    break;
                case WIDX_ROTATE:
                    Rotate();
                    break;
                case WIDX_ENTRANCE:
                    EntranceClick();
                    break;
                case WIDX_EXIT:
                    ExitClick();
                    break;
                case WIDX_SIMULATE:
                {
                    auto currentRide = GetRide(_currentRideIndex);
                    if (currentRide != nullptr)
                    {
                        auto status = currentRide->status == RideStatus::simulating ? RideStatus::closed
                                                                                    : RideStatus::simulating;
                        auto gameAction = GameActions::RideSetStatusAction(currentRide->id, status);
                        GameActions::Execute(&gameAction, getGameState());
                    }
                    break;
                }
            }
        }

        void onMouseDown(WidgetIndex widgetIndex) override
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            WindowRideConstructionUpdateEnabledTrackPieces();
            switch (widgetIndex)
            {
                case WIDX_CONSTRUCT:
                {
                    Construct();
                    // Force any footpath construction to recheck the area.
                    FootpathRecheckProvisional();
                    break;
                }
                case WIDX_DEMOLISH:
                    MouseUpDemolish();
                    break;
                case WIDX_NEXT_SECTION:
                    RideSelectNextSection();
                    if (!gMapSelectFlags.has(MapSelectFlag::enable))
                        VirtualFloorSetHeight(_currentTrackBegin.z);
                    break;
                case WIDX_PREVIOUS_SECTION:
                    RideSelectPreviousSection();
                    if (!gMapSelectFlags.has(MapSelectFlag::enable))
                        VirtualFloorSetHeight(_currentTrackBegin.z);
                    break;
                case WIDX_LEFT_CURVE:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::left;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_RIGHT_CURVE:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::right;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_LEFT_CURVE_SMALL:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::leftSmall;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_RIGHT_CURVE_SMALL:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::rightSmall;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_LEFT_CURVE_VERY_SMALL:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::leftVerySmall;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_RIGHT_CURVE_VERY_SMALL:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::rightVerySmall;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_LEFT_CURVE_LARGE:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::leftLarge;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_RIGHT_CURVE_LARGE:
                    RideConstructionInvalidateCurrentTrack();
                    _currentlySelectedTrack = TrackCurve::rightLarge;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_STRAIGHT:
                    RideConstructionInvalidateCurrentTrack();
                    if (_currentlySelectedTrack != TrackCurve::none)
                        _currentTrackRollEnd = TrackRoll::none;
                    _currentlySelectedTrack = TrackCurve::none;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_SLOPE_DOWN_VERTICAL:
                    RideConstructionInvalidateCurrentTrack();
                    UpdateLiftHillSelected(TrackPitch::down90);
                    break;
                case WIDX_SLOPE_DOWN_STEEP:
                    RideConstructionInvalidateCurrentTrack();
                    if (IsTrackEnabled(TrackGroup::helixDownBankedHalf) || IsTrackEnabled(TrackGroup::helixUpBankedHalf))
                    {
                        if (_currentlySelectedTrack == TrackCurve::left && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftHalfBankedHelixDownLarge;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::right && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightHalfBankedHelixDownLarge;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::leftSmall && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftHalfBankedHelixDownSmall;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::rightSmall && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightHalfBankedHelixDownSmall;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                    }
                    if (IsTrackEnabled(TrackGroup::helixDownBankedQuarter) || IsTrackEnabled(TrackGroup::helixUpBankedQuarter))
                    {
                        if (_currentlySelectedTrack == TrackCurve::left && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftQuarterBankedHelixLargeDown;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::right && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightQuarterBankedHelixLargeDown;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                    }
                    if (IsTrackEnabled(TrackGroup::helixDownUnbankedQuarter)
                        || IsTrackEnabled(TrackGroup::helixUpUnbankedQuarter))
                    {
                        if (_currentTrackRollEnd == TrackRoll::none)
                        {
                            if (_currentlySelectedTrack == TrackCurve::left)
                            {
                                _currentlySelectedTrack = TrackElemType::leftQuarterHelixLargeDown;
                                _currentTrackPrice = kMoney64Undefined;
                                WindowRideConstructionUpdateActiveElements();
                                break;
                            }
                            if (_currentlySelectedTrack == TrackCurve::right)
                            {
                                _currentlySelectedTrack = TrackElemType::rightQuarterHelixLargeDown;
                                _currentTrackPrice = kMoney64Undefined;
                                WindowRideConstructionUpdateActiveElements();
                                break;
                            }
                        }
                    }

                    UpdateLiftHillSelected(TrackPitch::down60);
                    break;
                case WIDX_SLOPE_DOWN:
                    RideConstructionInvalidateCurrentTrack();
                    if (_rideConstructionState == RideConstructionState::Back && _currentTrackRollEnd != TrackRoll::none)
                    {
                        _currentTrackRollEnd = TrackRoll::none;
                    }
                    UpdateLiftHillSelected(TrackPitch::down25);
                    break;
                case WIDX_LEVEL:
                    RideConstructionInvalidateCurrentTrack();
                    if (_rideConstructionState == RideConstructionState::Front && _previousTrackPitchEnd == TrackPitch::down25)
                    {
                        if (_currentlySelectedTrack == TrackCurve::leftSmall)
                        {
                            _currentTrackRollEnd = TrackRoll::left;
                        }
                        else if (_currentlySelectedTrack == TrackCurve::rightSmall)
                        {
                            _currentTrackRollEnd = TrackRoll::right;
                        }
                    }
                    else if (
                        _rideConstructionState == RideConstructionState::Back && _previousTrackPitchEnd == TrackPitch::up25)
                    {
                        if (_currentlySelectedTrack == TrackCurve::leftSmall)
                        {
                            _currentTrackRollEnd = TrackRoll::left;
                        }
                        else if (_currentlySelectedTrack == TrackCurve::rightSmall)
                        {
                            _currentTrackRollEnd = TrackRoll::right;
                        }
                    }
                    UpdateLiftHillSelected(TrackPitch::none);
                    break;
                case WIDX_SLOPE_UP:
                    RideConstructionInvalidateCurrentTrack();
                    if (_rideConstructionState == RideConstructionState::Front && _currentTrackRollEnd != TrackRoll::none)
                    {
                        _currentTrackRollEnd = TrackRoll::none;
                    }
                    if (currentRide->getRideTypeDescriptor().SupportsTrackGroup(TrackGroup::reverseFreefall))
                    {
                        if (_rideConstructionState == RideConstructionState::Front
                            && _currentlySelectedTrack == TrackCurve::none)
                        {
                            _currentlySelectedTrack = TrackElemType::reverseFreefallSlope;
                            WindowRideConstructionUpdateActiveElements();
                        }
                    }
                    else
                    {
                        UpdateLiftHillSelected(TrackPitch::up25);
                    }
                    break;
                case WIDX_SLOPE_UP_STEEP:
                    RideConstructionInvalidateCurrentTrack();
                    if (IsTrackEnabled(TrackGroup::helixDownBankedHalf) || IsTrackEnabled(TrackGroup::helixUpBankedHalf))
                    {
                        if (_currentlySelectedTrack == TrackCurve::left && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftHalfBankedHelixUpLarge;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::right && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightHalfBankedHelixUpLarge;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::leftSmall && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftHalfBankedHelixUpSmall;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::rightSmall && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightHalfBankedHelixUpSmall;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                    }
                    if (IsTrackEnabled(TrackGroup::helixDownBankedQuarter) || IsTrackEnabled(TrackGroup::helixUpBankedQuarter))
                    {
                        if (_currentlySelectedTrack == TrackCurve::left && _currentTrackRollEnd == TrackRoll::left)
                        {
                            _currentlySelectedTrack = TrackElemType::leftQuarterBankedHelixLargeUp;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                        if (_currentlySelectedTrack == TrackCurve::right && _currentTrackRollEnd == TrackRoll::right)
                        {
                            _currentlySelectedTrack = TrackElemType::rightQuarterBankedHelixLargeUp;
                            _currentTrackPrice = kMoney64Undefined;
                            WindowRideConstructionUpdateActiveElements();
                            break;
                        }
                    }
                    if (IsTrackEnabled(TrackGroup::helixDownUnbankedQuarter)
                        || IsTrackEnabled(TrackGroup::helixUpUnbankedQuarter))
                    {
                        if (_currentTrackRollEnd == TrackRoll::none)
                        {
                            if (_currentlySelectedTrack == TrackCurve::left)
                            {
                                _currentlySelectedTrack = TrackElemType::leftQuarterHelixLargeUp;
                                _currentTrackPrice = kMoney64Undefined;
                                WindowRideConstructionUpdateActiveElements();
                                break;
                            }
                            if (_currentlySelectedTrack == TrackCurve::right)
                            {
                                _currentlySelectedTrack = TrackElemType::rightQuarterHelixLargeUp;
                                _currentTrackPrice = kMoney64Undefined;
                                WindowRideConstructionUpdateActiveElements();
                                break;
                            }
                        }
                    }

                    UpdateLiftHillSelected(TrackPitch::up60);
                    break;
                case WIDX_SLOPE_UP_VERTICAL:
                    RideConstructionInvalidateCurrentTrack();
                    UpdateLiftHillSelected(TrackPitch::up90);
                    break;
                case WIDX_CHAIN_LIFT:
                    RideConstructionInvalidateCurrentTrack();
                    _currentTrackHasLiftHill = !_currentTrackHasLiftHill;
                    if ((_currentTrackHasLiftHill) && !getGameState().cheats.enableChainLiftOnAllTrack)
                        _currentTrackAlternative.unset(AlternativeTrackFlag::alternativePieces);
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_BANK_LEFT:
                    RideConstructionInvalidateCurrentTrack();
                    if (!_currentlyShowingBrakeOrBoosterSpeed)
                    {
                        _currentTrackRollEnd = TrackRoll::left;
                        _currentTrackPrice = kMoney64Undefined;
                        WindowRideConstructionUpdateActiveElements();
                    }
                    break;
                case WIDX_BANK_STRAIGHT:
                    RideConstructionInvalidateCurrentTrack();
                    if (!_currentlyShowingBrakeOrBoosterSpeed)
                    {
                        _currentTrackRollEnd = TrackRoll::none;
                        _currentTrackPrice = kMoney64Undefined;
                        WindowRideConstructionUpdateActiveElements();
                    }
                    else
                    {
                        auto trackSpeedMaximum = kMaximumTrackSpeed;
                        auto trackSpeedIncrement = kDefaultSpeedIncrement;
                        uint8_t brakesSpeed = std::min<int16_t>(trackSpeedMaximum, _currentBrakeSpeed + trackSpeedIncrement);
                        if (brakesSpeed != _currentBrakeSpeed)
                        {
                            if (_rideConstructionState == RideConstructionState::Selected)
                            {
                                SetBrakeSpeed(brakesSpeed);
                            }
                            else
                            {
                                _currentBrakeSpeed = brakesSpeed;
                                WindowRideConstructionUpdateActiveElements();
                            }
                        }
                    }
                    break;
                case WIDX_BANK_RIGHT:
                    RideConstructionInvalidateCurrentTrack();
                    if (!_currentlyShowingBrakeOrBoosterSpeed)
                    {
                        _currentTrackRollEnd = TrackRoll::right;
                        _currentTrackPrice = kMoney64Undefined;
                        WindowRideConstructionUpdateActiveElements();
                    }
                    else
                    {
                        auto trackSpeedIncrement = kDefaultSpeedIncrement;
                        auto trackSpeedMinimum = kDefaultMinimumSpeed;
                        if (getGameState().cheats.unlockOperatingLimits)
                        {
                            trackSpeedMinimum = 0;
                        }
                        uint8_t brakesSpeed = std::max<int16_t>(trackSpeedMinimum, _currentBrakeSpeed - trackSpeedIncrement);
                        if (brakesSpeed != _currentBrakeSpeed)
                        {
                            if (_rideConstructionState == RideConstructionState::Selected)
                            {
                                SetBrakeSpeed(brakesSpeed);
                            }
                            else
                            {
                                _currentBrakeSpeed = brakesSpeed;
                                WindowRideConstructionUpdateActiveElements();
                            }
                        }
                    }
                    break;
                case WIDX_SPECIAL_TRACK_DROPDOWN:
                    ShowSpecialTrackDropdown(&widgets[widgetIndex]);
                    break;
                case WIDX_U_TRACK:
                    RideConstructionInvalidateCurrentTrack();
                    _currentTrackAlternative.unset(AlternativeTrackFlag::alternativePieces);
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_O_TRACK:
                    RideConstructionInvalidateCurrentTrack();
                    _currentTrackAlternative.set(AlternativeTrackFlag::alternativePieces);
                    if (!getGameState().cheats.enableChainLiftOnAllTrack)
                        _currentTrackHasLiftHill = false;
                    _currentTrackPrice = kMoney64Undefined;
                    WindowRideConstructionUpdateActiveElements();
                    break;
                case WIDX_SEAT_ROTATION_ANGLE_SPINNER_UP:
                    if (_currentSeatRotationAngle < 15)
                    {
                        if (_rideConstructionState == RideConstructionState::Selected)
                        {
                            RideSelectedTrackSetSeatRotation(_currentSeatRotationAngle + 1);
                        }
                        else
                        {
                            _currentSeatRotationAngle++;
                            WindowRideConstructionUpdateActiveElements();
                        }
                    }
                    break;
                case WIDX_SEAT_ROTATION_ANGLE_SPINNER_DOWN:
                    if (_currentSeatRotationAngle > 0)
                    {
                        if (_rideConstructionState == RideConstructionState::Selected)
                        {
                            RideSelectedTrackSetSeatRotation(_currentSeatRotationAngle - 1);
                        }
                        else
                        {
                            _currentSeatRotationAngle--;
                            WindowRideConstructionUpdateActiveElements();
                        }
                    }
                    break;
            }
        }

        void onDropdown(WidgetIndex widgetIndex, int32_t selectedIndex) override
        {
            if (widgetIndex != WIDX_SPECIAL_TRACK_DROPDOWN)
                return;
            if (selectedIndex == -1)
                return;

            RideConstructionInvalidateCurrentTrack();
            _currentTrackPrice = kMoney64Undefined;
            TrackElemType trackPiece = _specialElementDropdownState.Elements[selectedIndex].TrackType;
            switch (trackPiece)
            {
                case TrackElemType::endStation:
                case TrackElemType::sBendLeft:
                case TrackElemType::sBendRight:
                    _currentTrackPitchEnd = TrackPitch::none;
                    break;
                case TrackElemType::leftVerticalLoop:
                case TrackElemType::rightVerticalLoop:
                    _currentTrackRollEnd = TrackRoll::none;
                    _currentTrackHasLiftHill = false;
                    break;
                case TrackElemType::blockBrakes:
                case TrackElemType::diagBlockBrakes:
                    _currentBrakeSpeed = kRCT2DefaultBlockBrakeSpeed;
                    break;
                default:
                    break;
            }
            _currentlySelectedTrack = trackPiece;
            WindowRideConstructionUpdateActiveElements();
        }

        void onToolUpdate(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            switch (widgetIndex)
            {
                case WIDX_CONSTRUCT:
                    RideConstructionToolupdateConstruct(screenCoords);
                    break;
                case WIDX_ENTRANCE:
                case WIDX_EXIT:
                    RideConstructionToolupdateEntranceExit(screenCoords);
                    break;
            }
        }

        void onToolDown(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            switch (widgetIndex)
            {
                case WIDX_CONSTRUCT:
                    RideConstructionTooldownConstruct(screenCoords);
                    break;
                case WIDX_ENTRANCE:
                case WIDX_EXIT:
                    ToolDownEntranceExit(screenCoords);
                    break;
            }
        }

        void onPrepareDraw() override
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            StringId stringId = STR_RIDE_CONSTRUCTION_SPECIAL;
            if (_currentlySelectedTrack.isTrackType)
            {
                const auto& rtd = currentRide->getRideTypeDescriptor();
                const auto& ted = GetTrackElementDescriptor(_currentlySelectedTrack.trackType);
                stringId = ted.description;
                if (stringId == STR_RAPIDS && rtd.Category != RideCategory::water)
                {
                    stringId = STR_LOG_BUMPS;
                }
            }
            _specialDropdownText = FormatStringID(STR_YELLOW_STRING, stringId);
            widgets[WIDX_SPECIAL_TRACK_DROPDOWN].setString(_specialDropdownText.c_str());

            if (_currentlyShowingBrakeOrBoosterSpeed)
            {
                uint16_t brakeSpeed2 = ((_currentBrakeSpeed * 9) >> 2) & 0xFFFF;
                if (trackTypeIsBooster(_selectedTrackType) || trackTypeIsBooster(_currentlySelectedTrack.trackType))
                {
                    brakeSpeed2 = GetUnifiedBoosterSpeed(currentRide->type, brakeSpeed2);
                }
                _brakeSpeedText = FormatStringID(STR_VELOCITY, brakeSpeed2);
                widgets[WIDX_SPEED_SETTING_SPINNER].setString(_brakeSpeedText.c_str());
            }

            widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER].text = kSeatAngleRotationStrings[_currentSeatRotationAngle];

            // Simulate button
            auto& simulateWidget = widgets[WIDX_SIMULATE];
            simulateWidget.type = WidgetType::empty;
            if (currentRide->supportsStatus(RideStatus::simulating))
            {
                simulateWidget.type = WidgetType::flatBtn;
                setWidgetPressed(WIDX_SIMULATE, currentRide->status == RideStatus::simulating);
            }
            _windowTitle = FormatStringID(STR_RIDE_CONSTRUCTION_WINDOW_TITLE, currentRide->getName().c_str());
            widgets[WIDX_TITLE].setString(_windowTitle.c_str());
        }

        static void onDrawUpdateCoveredPieces(const TrackDrawerDescriptor& trackDrawerDescriptor, std::span<Widget> widgets)
        {
            widgets[WIDX_U_TRACK].type = WidgetType::empty;
            widgets[WIDX_O_TRACK].type = WidgetType::empty;

            if (!trackDrawerDescriptor.HasCoveredPieces())
                return;

            auto trackElementResult = WindowRideConstructionUpdateStateGetTrackElement();
            if (!std::get<0>(trackElementResult))
                return;

            auto selectedTrackType = std::get<1>(trackElementResult);
            const auto& ted = GetTrackElementDescriptor(selectedTrackType);
            auto availableGroups = trackDrawerDescriptor.Covered.enabledTrackGroups;

            if (!availableGroups.get(EnumValue(ted.definition.group)))
                return;

            widgets[WIDX_BANKING_GROUPBOX].text = STR_RIDE_CONSTRUCTION_TRACK_STYLE;
            widgets[WIDX_U_TRACK].type = WidgetType::flatBtn;
            widgets[WIDX_O_TRACK].type = WidgetType::flatBtn;

            widgets[WIDX_U_TRACK].image = ImageId(trackDrawerDescriptor.Regular.icon);
            widgets[WIDX_O_TRACK].image = ImageId(trackDrawerDescriptor.Covered.icon);
            widgets[WIDX_U_TRACK].tooltip = trackDrawerDescriptor.Regular.tooltip;
            widgets[WIDX_O_TRACK].tooltip = trackDrawerDescriptor.Covered.tooltip;
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            Drawing::RenderTarget clippedRT;
            Widget* widget;
            int32_t widgetWidth, widgetHeight;

            drawWidgets(rt);

            widget = &widgets[WIDX_CONSTRUCT];
            if (widget->type == WidgetType::empty)
                return;

            RideId rideIndex;
            TrackElemType trackType;
            int32_t trackDirection;
            SelectedLiftAndInverted liftHillAndInvertedState{};
            if (WindowRideConstructionUpdateState(
                    &trackType, &trackDirection, &rideIndex, &liftHillAndInvertedState, nullptr, nullptr))
                return;

            // Draw track piece
            auto screenCoords = ScreenCoordsXY{ windowPos.x + widget->left + 1, windowPos.y + widget->top + 1 };
            widgetWidth = widget->width() - 2;
            widgetHeight = widget->height() - 2;
            if (ClipRenderTarget(clippedRT, rt, screenCoords, widgetWidth, widgetHeight))
            {
                DrawTrackPiece(
                    clippedRT, rideIndex, trackType, trackDirection, liftHillAndInvertedState, widgetWidth, widgetHeight);
            }

            // Draw cost
            screenCoords = { windowPos.x + widget->midX(), windowPos.y + widget->bottom - 23 };
            if (_rideConstructionState != RideConstructionState::Place)
                drawText(rt, screenCoords, STR_BUILD_THIS, { TextAlignment::centre });

            screenCoords.y += 11;
            if (_currentTrackPrice != kMoney64Undefined && !(getGameState().park.flags & PARK_FLAGS_NO_MONEY))
            {
                auto ft = Formatter();
                ft.Add<money64>(_currentTrackPrice);
                drawText(rt, screenCoords, STR_COST_LABEL, ft, { TextAlignment::centre });
            }
        }

        void updateWidgets()
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            const auto& rtd = GetRideTypeDescriptor(currentRide->type);
            auto trackDrawerDescriptor = getCurrentTrackDrawerDescriptor(rtd);

            widgetsSetHoldable(*this, { WIDX_CONSTRUCT, WIDX_DEMOLISH, WIDX_NEXT_SECTION, WIDX_PREVIOUS_SECTION });
            if (rtd.flags.has(RtdFlag::isShopOrFacility) || !currentRide->hasStation())
            {
                widgets[WIDX_ENTRANCE_EXIT_GROUPBOX].type = WidgetType::empty;
                widgets[WIDX_ENTRANCE].type = WidgetType::empty;
                widgets[WIDX_EXIT].type = WidgetType::empty;
            }
            else
            {
                widgets[WIDX_ENTRANCE_EXIT_GROUPBOX].type = WidgetType::groupbox;
                widgets[WIDX_ENTRANCE].type = WidgetType::button;
                widgets[WIDX_EXIT].type = WidgetType::button;
            }

            if (_specialElementDropdownState.HasActiveElements)
            {
                widgets[WIDX_SPECIAL_TRACK_DROPDOWN].type = WidgetType::button;
            }
            else
            {
                widgets[WIDX_SPECIAL_TRACK_DROPDOWN].type = WidgetType::empty;
            }

            if (IsTrackEnabled(TrackGroup::straight))
            {
                widgets[WIDX_STRAIGHT].type = WidgetType::flatBtn;
            }
            else
            {
                widgets[WIDX_STRAIGHT].type = WidgetType::empty;
            }

            if (IsTrackEnabled(TrackGroup::curveLarge))
            {
                widgets[WIDX_LEFT_CURVE_LARGE].type = WidgetType::flatBtn;
                widgets[WIDX_RIGHT_CURVE_LARGE].type = WidgetType::flatBtn;
            }
            else
            {
                widgets[WIDX_LEFT_CURVE_LARGE].type = WidgetType::empty;
                widgets[WIDX_RIGHT_CURVE_LARGE].type = WidgetType::empty;
            }

            widgets[WIDX_LEFT_CURVE].type = WidgetType::empty;
            widgets[WIDX_RIGHT_CURVE].type = WidgetType::empty;
            widgets[WIDX_LEFT_CURVE_SMALL].type = WidgetType::empty;
            widgets[WIDX_RIGHT_CURVE_SMALL].type = WidgetType::empty;
            widgets[WIDX_LEFT_CURVE_VERY_SMALL].type = WidgetType::empty;
            widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type = WidgetType::empty;
            if (IsTrackEnabled(TrackGroup::curveVertical))
            {
                widgets[WIDX_LEFT_CURVE_SMALL].type = WidgetType::flatBtn;
                widgets[WIDX_RIGHT_CURVE_SMALL].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::curve))
            {
                widgets[WIDX_LEFT_CURVE].type = WidgetType::flatBtn;
                widgets[WIDX_RIGHT_CURVE].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::curveSmall))
            {
                widgets[WIDX_LEFT_CURVE_SMALL].type = WidgetType::flatBtn;
                widgets[WIDX_RIGHT_CURVE_SMALL].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::curveVerySmall))
            {
                widgets[WIDX_LEFT_CURVE_VERY_SMALL].type = WidgetType::flatBtn;
                widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type = WidgetType::flatBtn;
            }

            widgets[WIDX_SLOPE_DOWN_STEEP].type = WidgetType::empty;
            widgets[WIDX_SLOPE_DOWN].type = WidgetType::empty;
            widgets[WIDX_LEVEL].type = WidgetType::empty;
            widgets[WIDX_SLOPE_UP].type = WidgetType::empty;
            widgets[WIDX_SLOPE_UP_STEEP].type = WidgetType::empty;
            widgets[WIDX_SLOPE_DOWN_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_DOWN_STEEP);
            widgets[WIDX_SLOPE_DOWN_STEEP].tooltip = STR_RIDE_CONSTRUCTION_STEEP_SLOPE_DOWN_TIP;
            widgets[WIDX_SLOPE_UP_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_SLOPE_UP_STEEP);
            widgets[WIDX_SLOPE_UP_STEEP].tooltip = STR_RIDE_CONSTRUCTION_STEEP_SLOPE_UP_TIP;
            if (trackDrawerDescriptor.Regular.SupportsTrackGroup(TrackGroup::reverseFreefall))
            {
                widgets[WIDX_LEVEL].type = WidgetType::flatBtn;
                widgets[WIDX_SLOPE_UP].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::slope) || IsTrackEnabled(TrackGroup::slopeSteepDown)
                || IsTrackEnabled(TrackGroup::slopeSteepUp))
            {
                widgets[WIDX_LEVEL].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::slope))
            {
                widgets[WIDX_SLOPE_DOWN].type = WidgetType::flatBtn;
                widgets[WIDX_SLOPE_UP].type = WidgetType::flatBtn;
            }
            if ((IsTrackEnabled(TrackGroup::helixDownBankedHalf) || IsTrackEnabled(TrackGroup::helixUpBankedHalf))
                && _currentTrackRollEnd != TrackRoll::none && _currentTrackPitchEnd == TrackPitch::none)
            {
                const bool hasHelixEquivalent = _currentlySelectedTrack == TrackCurve::left
                    || _currentlySelectedTrack == TrackCurve::right || _currentlySelectedTrack == TrackCurve::leftSmall
                    || _currentlySelectedTrack == TrackCurve::rightSmall;
                if (hasHelixEquivalent)
                {
                    // Enable helix
                    widgets[WIDX_SLOPE_DOWN_STEEP].type = WidgetType::flatBtn;
                    if (IsTrackEnabled(TrackGroup::helixUpBankedHalf))
                        widgets[WIDX_SLOPE_UP_STEEP].type = WidgetType::flatBtn;
                }
            }

            if (IsTrackEnabled(TrackGroup::slopeSteepDown) || IsTrackEnabled(TrackGroup::diagSlopeSteepDown))
            {
                widgets[WIDX_SLOPE_DOWN_STEEP].type = WidgetType::flatBtn;
            }
            if (IsTrackEnabled(TrackGroup::slopeSteepUp) || IsTrackEnabled(TrackGroup::diagSlopeSteepUp))
            {
                widgets[WIDX_SLOPE_UP_STEEP].type = WidgetType::flatBtn;
            }

            const auto& gameState = getGameState();
            if (currentRide->getRideTypeDescriptor().flags.has(RtdFlag::upInclineRequiresLift)
                && (_currentTrackPitchEnd == TrackPitch::up25 || _currentTrackPitchEnd == TrackPitch::up60)
                && !gameState.cheats.enableAllDrawableTrackPieces)
            {
                _currentTrackHasLiftHill = true;
            }

            if ((IsTrackEnabled(TrackGroup::liftHill) && !_currentlySelectedTrack.isTrackType)
                || (gameState.cheats.enableChainLiftOnAllTrack
                    && currentRide->getRideTypeDescriptor().flags.has(RtdFlag::hasTrack)))
            {
                widgets[WIDX_CHAIN_LIFT].type = WidgetType::flatBtn;
            }
            else
            {
                widgets[WIDX_CHAIN_LIFT].type = WidgetType::empty;
            }

            int32_t x = kVerticalDropButtonStart;
            for (int32_t i = WIDX_SLOPE_DOWN_VERTICAL; i <= WIDX_SLOPE_UP_VERTICAL; i++)
            {
                widgets[i].moveToX(x);
                x += 24;
            }

            widgets[WIDX_SLOPE_DOWN_VERTICAL].type = WidgetType::empty;
            widgets[WIDX_SLOPE_UP_VERTICAL].type = WidgetType::empty;

            if (IsTrackEnabled(TrackGroup::slopeVertical) && !TrackPieceDirectionIsDiagonal(_currentTrackPieceDirection))
            {
                if (_previousTrackPitchEnd == TrackPitch::up60 || _previousTrackPitchEnd == TrackPitch::up90)
                {
                    widgets[WIDX_SLOPE_UP_VERTICAL].type = WidgetType::flatBtn;
                }
                else if (_previousTrackPitchEnd == TrackPitch::down60 || _previousTrackPitchEnd == TrackPitch::down90)
                {
                    widgets[WIDX_SLOPE_DOWN_VERTICAL].type = WidgetType::flatBtn;
                }
            }

            if ((IsTrackEnabled(TrackGroup::helixDownUnbankedQuarter) || IsTrackEnabled(TrackGroup::helixUpUnbankedQuarter))
                && _currentTrackPitchEnd == TrackPitch::none && _currentTrackRollEnd == TrackRoll::none
                && (_currentlySelectedTrack == TrackCurve::left || _currentlySelectedTrack == TrackCurve::right))
            {
                widgets[WIDX_SLOPE_DOWN_STEEP].type = WidgetType::flatBtn;
                widgets[WIDX_SLOPE_DOWN_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_HELIX_DOWN);
                widgets[WIDX_SLOPE_DOWN_STEEP].tooltip = STR_RIDE_CONSTRUCTION_HELIX_DOWN_TIP;
                widgets[WIDX_SLOPE_UP_STEEP].type = WidgetType::flatBtn;
                widgets[WIDX_SLOPE_UP_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_HELIX_UP);
                widgets[WIDX_SLOPE_UP_STEEP].tooltip = STR_RIDE_CONSTRUCTION_HELIX_UP_TIP;

                int32_t tmp = widgets[WIDX_SLOPE_DOWN_STEEP].left;
                widgets[WIDX_SLOPE_DOWN_STEEP].left = widgets[WIDX_SLOPE_DOWN].left;
                widgets[WIDX_SLOPE_DOWN].left = tmp;

                tmp = widgets[WIDX_SLOPE_DOWN_STEEP].right;
                widgets[WIDX_SLOPE_DOWN_STEEP].right = widgets[WIDX_SLOPE_DOWN].right;
                widgets[WIDX_SLOPE_DOWN].right = tmp;

                tmp = widgets[WIDX_SLOPE_UP_STEEP].left;
                widgets[WIDX_SLOPE_UP_STEEP].left = widgets[WIDX_SLOPE_UP].left;
                widgets[WIDX_SLOPE_UP].left = tmp;

                tmp = widgets[WIDX_SLOPE_UP_STEEP].right;
                widgets[WIDX_SLOPE_UP_STEEP].right = widgets[WIDX_SLOPE_UP].right;
                widgets[WIDX_SLOPE_UP].right = tmp;
            }

            if (IsTrackEnabled(TrackGroup::helixDownBankedQuarter) || IsTrackEnabled(TrackGroup::helixUpBankedQuarter)
                || IsTrackEnabled(TrackGroup::helixDownBankedHalf) || IsTrackEnabled(TrackGroup::helixUpBankedHalf))
            {
                const bool hasHelixEquivalent = _currentlySelectedTrack == TrackCurve::left
                    || _currentlySelectedTrack == TrackCurve::right || _currentlySelectedTrack == TrackCurve::leftSmall
                    || _currentlySelectedTrack == TrackCurve::rightSmall;

                if (hasHelixEquivalent && _currentTrackPitchEnd == TrackPitch::none && _currentTrackRollEnd != TrackRoll::none)
                {
                    widgets[WIDX_SLOPE_DOWN_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_HELIX_DOWN);
                    widgets[WIDX_SLOPE_DOWN_STEEP].tooltip = STR_RIDE_CONSTRUCTION_HELIX_DOWN_TIP;
                    widgets[WIDX_SLOPE_UP_STEEP].image = ImageId(SPR_RIDE_CONSTRUCTION_HELIX_UP);
                    widgets[WIDX_SLOPE_UP_STEEP].tooltip = STR_RIDE_CONSTRUCTION_HELIX_UP_TIP;

                    int32_t tmp = widgets[WIDX_SLOPE_DOWN_STEEP].left;
                    widgets[WIDX_SLOPE_DOWN_STEEP].left = widgets[WIDX_SLOPE_DOWN].left;
                    widgets[WIDX_SLOPE_DOWN].left = tmp;

                    tmp = widgets[WIDX_SLOPE_DOWN_STEEP].right;
                    widgets[WIDX_SLOPE_DOWN_STEEP].right = widgets[WIDX_SLOPE_DOWN].right;
                    widgets[WIDX_SLOPE_DOWN].right = tmp;

                    tmp = widgets[WIDX_SLOPE_UP_STEEP].left;
                    widgets[WIDX_SLOPE_UP_STEEP].left = widgets[WIDX_SLOPE_UP].left;
                    widgets[WIDX_SLOPE_UP].left = tmp;

                    tmp = widgets[WIDX_SLOPE_UP_STEEP].right;
                    widgets[WIDX_SLOPE_UP_STEEP].right = widgets[WIDX_SLOPE_UP].right;
                    widgets[WIDX_SLOPE_UP].right = tmp;
                }
            }

            auto top = 118 + widgets[WIDX_TITLE].bottom;
            auto bottom = top + 23;
            widgets[WIDX_BANKING_GROUPBOX].image = ImageId(STR_RIDE_CONSTRUCTION_ROLL_BANKING);
            widgets[WIDX_BANK_LEFT].image = ImageId(SPR_RIDE_CONSTRUCTION_LEFT_BANK);
            widgets[WIDX_BANK_LEFT].tooltip = STR_RIDE_CONSTRUCTION_ROLL_FOR_LEFT_CURVE_TIP;
            widgets[WIDX_BANK_LEFT].left = 69;
            widgets[WIDX_BANK_LEFT].right = 92;
            widgets[WIDX_BANK_LEFT].top = top;
            widgets[WIDX_BANK_LEFT].bottom = bottom;
            widgets[WIDX_BANK_STRAIGHT].image = ImageId(SPR_RIDE_CONSTRUCTION_NO_BANK);
            widgets[WIDX_BANK_STRAIGHT].tooltip = STR_RIDE_CONSTRUCTION_NO_ROLL_TIP;
            widgets[WIDX_BANK_STRAIGHT].left = 93;
            widgets[WIDX_BANK_STRAIGHT].right = 116;
            widgets[WIDX_BANK_STRAIGHT].top = top;
            widgets[WIDX_BANK_STRAIGHT].bottom = bottom;
            widgets[WIDX_BANK_RIGHT].image = ImageId(SPR_RIDE_CONSTRUCTION_RIGHT_BANK);
            widgets[WIDX_BANK_RIGHT].tooltip = STR_RIDE_CONSTRUCTION_ROLL_FOR_RIGHT_CURVE_TIP;
            widgets[WIDX_BANK_RIGHT].left = 117;
            widgets[WIDX_BANK_RIGHT].right = 140;
            widgets[WIDX_BANK_RIGHT].top = top;
            widgets[WIDX_BANK_RIGHT].bottom = bottom;
            widgets[WIDX_BANK_LEFT].type = WidgetType::empty;
            widgets[WIDX_BANK_STRAIGHT].type = WidgetType::empty;
            widgets[WIDX_BANK_RIGHT].type = WidgetType::empty;
            widgets[WIDX_U_TRACK].type = WidgetType::empty;
            widgets[WIDX_O_TRACK].type = WidgetType::empty;

            bool trackHasSpeedSetting = trackTypeHasSpeedSetting(_selectedTrackType)
                || trackTypeHasSpeedSetting(_currentlySelectedTrack.trackType);
            bool boosterTrackSelected = trackTypeIsBooster(_selectedTrackType)
                || trackTypeIsBooster(_currentlySelectedTrack.trackType);

            // only necessary because TD6 writes speed and seat rotation to the same bits. Remove for new track design format.
            bool trackHasSpeedAndSeatRotation = _selectedTrackType == TrackElemType::blockBrakes
                || _currentlySelectedTrack == TrackElemType::blockBrakes || _selectedTrackType > TrackElemType::highestAlias
                || _currentlySelectedTrack.trackType > TrackElemType::highestAlias;

            bool rideHasSeatRotation = rtd.flags.has(RtdFlag::hasSeatRotation);

            if (!trackHasSpeedSetting)
            {
                if (IsTrackEnabled(TrackGroup::flatRollBanking))
                {
                    widgets[WIDX_BANK_LEFT].type = WidgetType::flatBtn;
                    widgets[WIDX_BANK_STRAIGHT].type = WidgetType::flatBtn;
                    widgets[WIDX_BANK_RIGHT].type = WidgetType::flatBtn;
                }
                onDrawUpdateCoveredPieces(trackDrawerDescriptor, widgets);
            }
            else
            {
                if (!boosterTrackSelected)
                {
                    widgets[WIDX_SPEED_GROUPBOX].text = STR_RIDE_CONSTRUCTION_BRAKE_SPEED;
                    widgets[WIDX_SPEED_SETTING_SPINNER].tooltip = STR_RIDE_CONSTRUCTION_BRAKE_SPEED_LIMIT_TIP;
                    widgets[WIDX_SPEED_SETTING_SPINNER_UP].tooltip = STR_RIDE_CONSTRUCTION_BRAKE_SPEED_LIMIT_TIP;
                    widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].tooltip = STR_RIDE_CONSTRUCTION_BRAKE_SPEED_LIMIT_TIP;
                }
                else
                {
                    widgets[WIDX_SPEED_GROUPBOX].text = STR_RIDE_CONSTRUCTION_BOOSTER_SPEED;
                    widgets[WIDX_SPEED_SETTING_SPINNER].tooltip = STR_RIDE_CONSTRUCTION_BOOSTER_SPEED_LIMIT_TIP;
                    widgets[WIDX_SPEED_SETTING_SPINNER_UP].tooltip = STR_RIDE_CONSTRUCTION_BOOSTER_SPEED_LIMIT_TIP;
                    widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].tooltip = STR_RIDE_CONSTRUCTION_BOOSTER_SPEED_LIMIT_TIP;
                }

                _currentlyShowingBrakeOrBoosterSpeed = true;

                widgets[WIDX_SPEED_SETTING_SPINNER].type = WidgetType::spinner;
                widgets[WIDX_SPEED_SETTING_SPINNER_UP].type = WidgetType::button;
                widgets[WIDX_SPEED_SETTING_SPINNER_UP].text = STR_NUMERIC_UP;
                widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].type = WidgetType::button;
                widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].text = STR_NUMERIC_DOWN;

                auto spinnerStart = 124 + widgets[WIDX_TITLE].bottom;
                resizeSpinner(WIDX_SPEED_SETTING_SPINNER, { 12, spinnerStart }, { 85, kSpinnerHeight });

                widgetsSetHoldable(*this, { WIDX_SPEED_SETTING_SPINNER_UP, WIDX_SPEED_SETTING_SPINNER_DOWN });
            }

            static constexpr int16_t bankingGroupboxRightNoSeatRotation = kGroupWidth;
            static constexpr int16_t bankingGroupboxRightWithSeatRotation = 114;

            widgets[WIDX_BANKING_GROUPBOX].right = bankingGroupboxRightNoSeatRotation;
            widgets[WIDX_SEAT_ROTATION_GROUPBOX].type = WidgetType::empty;
            widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER].type = WidgetType::empty;
            widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER_UP].type = WidgetType::empty;
            widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER_DOWN].type = WidgetType::empty;

            // Simplify this condition to "rideHasSeatRotation" for new track design format
            if ((rideHasSeatRotation && !trackHasSpeedSetting)
                || (rideHasSeatRotation && trackHasSpeedSetting && trackHasSpeedAndSeatRotation))
            {
                widgets[WIDX_SEAT_ROTATION_GROUPBOX].type = WidgetType::groupbox;
                widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER].type = WidgetType::spinner;
                widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER_UP].type = WidgetType::button;
                widgets[WIDX_SEAT_ROTATION_ANGLE_SPINNER_DOWN].type = WidgetType::button;
                widgets[WIDX_BANKING_GROUPBOX].right = bankingGroupboxRightWithSeatRotation;

                // squishes the track speed spinner slightly to make room for the seat rotation widgets
                if (trackHasSpeedSetting)
                {
                    widgets[WIDX_SPEED_SETTING_SPINNER].left -= 4;
                    widgets[WIDX_SPEED_SETTING_SPINNER].right -= 8;
                    widgets[WIDX_SPEED_SETTING_SPINNER_UP].right -= 8;
                    widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].right -= 8;
                    widgets[WIDX_SPEED_SETTING_SPINNER_UP].left -= 8;
                    widgets[WIDX_SPEED_SETTING_SPINNER_DOWN].left -= 8;
                }
                // moves banking buttons to the left to make room for the seat rotation widgets
                else if (IsTrackEnabled(TrackGroup::flatRollBanking))
                {
                    for (int32_t i = WIDX_BANK_LEFT; i <= WIDX_BANK_RIGHT; i++)
                    {
                        widgets[i].left -= 36;
                        widgets[i].right -= 36;
                    }
                }
            }

            uint64_t newPressedWidgets = 0;
            for (auto preservedIndex : { WIDX_BACKGROUND, WIDX_TITLE, WIDX_CLOSE, WIDX_DIRECTION_GROUPBOX, WIDX_SLOPE_GROUPBOX,
                                         WIDX_BANKING_GROUPBOX, WIDX_CONSTRUCT, WIDX_DEMOLISH, WIDX_PREVIOUS_SECTION,
                                         WIDX_NEXT_SECTION, WIDX_ENTRANCE_EXIT_GROUPBOX, WIDX_ENTRANCE, WIDX_EXIT })
            {
                if (widgets[preservedIndex].flags.has(WidgetFlag::isPressed))
                    newPressedWidgets |= (1uLL << preservedIndex);
            }

            widgets[WIDX_CONSTRUCT].type = WidgetType::empty;
            widgets[WIDX_DEMOLISH].type = WidgetType::flatBtn;
            widgets[WIDX_ROTATE].type = WidgetType::empty;
            if (rtd.flags.has(RtdFlag::cannotHaveGaps))
            {
                widgets[WIDX_PREVIOUS_SECTION].type = WidgetType::empty;
                widgets[WIDX_NEXT_SECTION].type = WidgetType::empty;
            }
            else
            {
                widgets[WIDX_PREVIOUS_SECTION].type = WidgetType::flatBtn;
                widgets[WIDX_NEXT_SECTION].type = WidgetType::flatBtn;
            }

            switch (_rideConstructionState)
            {
                case RideConstructionState::Front:
                    widgets[WIDX_CONSTRUCT].type = WidgetType::imgBtn;
                    widgets[WIDX_NEXT_SECTION].type = WidgetType::empty;
                    break;
                case RideConstructionState::Back:
                    widgets[WIDX_CONSTRUCT].type = WidgetType::imgBtn;
                    widgets[WIDX_PREVIOUS_SECTION].type = WidgetType::empty;
                    break;
                case RideConstructionState::Place:
                    widgets[WIDX_CONSTRUCT].type = WidgetType::imgBtn;
                    widgets[WIDX_DEMOLISH].type = WidgetType::empty;
                    widgets[WIDX_NEXT_SECTION].type = WidgetType::empty;
                    widgets[WIDX_PREVIOUS_SECTION].type = WidgetType::empty;
                    widgets[WIDX_ROTATE].type = WidgetType::flatBtn;
                    break;
                case RideConstructionState::EntranceExit:
                    widgets[WIDX_DEMOLISH].type = WidgetType::empty;
                    widgets[WIDX_NEXT_SECTION].type = WidgetType::empty;
                    widgets[WIDX_PREVIOUS_SECTION].type = WidgetType::empty;
                    break;
                default:
                    applyPressedBits(newPressedWidgets);
                    invalidate();
                    return;
            }

            WidgetIndex widgetIndex = WIDX_SPECIAL_TRACK_DROPDOWN;
            if (!_currentlySelectedTrack.isTrackType)
            {
                switch (_currentlySelectedTrack.curve)
                {
                    case TrackCurve::none:
                        widgetIndex = WIDX_STRAIGHT;
                        break;
                    case TrackCurve::left:
                        widgetIndex = WIDX_LEFT_CURVE;
                        break;
                    case TrackCurve::right:
                        widgetIndex = WIDX_RIGHT_CURVE;
                        break;
                    case TrackCurve::leftSmall:
                        widgetIndex = WIDX_LEFT_CURVE_SMALL;
                        break;
                    case TrackCurve::rightSmall:
                        widgetIndex = WIDX_RIGHT_CURVE_SMALL;
                        break;
                    case TrackCurve::leftVerySmall:
                        widgetIndex = WIDX_LEFT_CURVE_VERY_SMALL;
                        break;
                    case TrackCurve::rightVerySmall:
                        widgetIndex = WIDX_RIGHT_CURVE_VERY_SMALL;
                        break;
                    case TrackCurve::leftLarge:
                        widgetIndex = WIDX_LEFT_CURVE_LARGE;
                        break;
                    case TrackCurve::rightLarge:
                        widgetIndex = WIDX_RIGHT_CURVE_LARGE;
                        break;
                }
            }

            newPressedWidgets |= (1uLL << widgetIndex);

            switch (_currentTrackPitchEnd)
            {
                case TrackPitch::down90:
                    widgetIndex = WIDX_SLOPE_DOWN_VERTICAL;
                    break;
                case TrackPitch::down60:
                    widgetIndex = WIDX_SLOPE_DOWN_STEEP;
                    break;
                case TrackPitch::down25:
                    widgetIndex = WIDX_SLOPE_DOWN;
                    break;
                case TrackPitch::up25:
                    widgetIndex = WIDX_SLOPE_UP;
                    break;
                case TrackPitch::up60:
                    widgetIndex = WIDX_SLOPE_UP_STEEP;
                    break;
                case TrackPitch::up90:
                    widgetIndex = WIDX_SLOPE_UP_VERTICAL;
                    break;
                default:
                    widgetIndex = WIDX_LEVEL;
                    break;
            }
            newPressedWidgets |= (1uLL << widgetIndex);

            if (!_currentlyShowingBrakeOrBoosterSpeed)
            {
                if (trackDrawerDescriptor.HasCoveredPieces())
                {
                    if (_currentTrackAlternative.has(AlternativeTrackFlag::alternativePieces))
                    {
                        newPressedWidgets |= (1uLL << WIDX_O_TRACK);
                    }
                    else
                    {
                        newPressedWidgets |= (1uLL << WIDX_U_TRACK);
                    }
                }
                switch (_currentTrackRollEnd)
                {
                    case TrackRoll::left:
                        widgetIndex = WIDX_BANK_LEFT;
                        break;
                    case TrackRoll::none:
                        widgetIndex = WIDX_BANK_STRAIGHT;
                        break;
                    default:
                        widgetIndex = WIDX_BANK_RIGHT;
                        break;
                }
                newPressedWidgets |= (1uLL << widgetIndex);
            }

            if (_currentTrackHasLiftHill)
                newPressedWidgets |= (1uLL << WIDX_CHAIN_LIFT);

            applyPressedBits(newPressedWidgets);
            invalidate();
        }

        void applyPressedBits(uint64_t bits)
        {
            const auto widgetCount = static_cast<WidgetIndex>(widgets.size());
            for (WidgetIndex i = 0; i < widgetCount && i < 64; i++)
                setWidgetPressed(i, (bits & (1uLL << i)) != 0);
        }

        void updatePossibleRideConfigurations()
        {
            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }
            _specialElementDropdownState = BuildSpecialElementsList(
                *currentRide, _currentTrackPieceDirection, _previousTrackPitchEnd, _previousTrackRollEnd,
                _rideConstructionState);
            _currentlyShowingBrakeOrBoosterSpeed = false;
        }

        void updateMapSelection()
        {
            TrackElemType trackType;
            int32_t trackDirection;
            CoordsXYZ trackPos{};

            gMapSelectFlags.set(MapSelectFlag::enableConstruct);
            gMapSelectFlags.set(MapSelectFlag::green);

            switch (_rideConstructionState)
            {
                case RideConstructionState::State0:
                    trackDirection = _currentTrackPieceDirection;
                    trackType = TrackElemType::flat;
                    trackPos = _currentTrackBegin;
                    break;
                case RideConstructionState::Selected:
                    trackDirection = _currentTrackPieceDirection;
                    trackType = _currentTrackPieceType;
                    trackPos = _currentTrackBegin;
                    break;
                case RideConstructionState::EntranceExit:
                    MapSelection::clearSelectedTiles();
                    return;
                default:
                    if (WindowRideConstructionUpdateState(&trackType, &trackDirection, nullptr, nullptr, &trackPos, nullptr))
                    {
                        trackDirection = _currentTrackPieceDirection;
                        trackType = TrackElemType::flat;
                        trackPos = _currentTrackBegin;
                    }
                    break;
            }

            if (GetRide(_currentRideIndex))
            {
                selectMapTiles(trackType, trackDirection, trackPos);
            }
        }

        void selectMapTiles(TrackElemType trackType, int32_t trackDirection, const CoordsXY& tileCoords)
        {
            // If the scenery tool is active, we do not display our tiles as it
            // will conflict with larger scenery objects selecting tiles
            if (isToolActive(WindowClass::scenery))
            {
                return;
            }

            const auto& ted = GetTrackElementDescriptor(trackType);
            trackDirection &= 3;
            MapSelection::clearSelectedTiles();
            for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
            {
                CoordsXY offsets = { ted.sequenceData.sequences[i].clearance.x, ted.sequenceData.sequences[i].clearance.y };
                CoordsXY currentTileCoords = tileCoords + offsets.Rotate(trackDirection);

                MapSelection::addSelectedTile(currentTileCoords);
            }
        }

    private:
        void Construct()
        {
            RideId rideIndex;
            TrackElemType trackType;
            int32_t trackDirection, properties;
            SelectedLiftAndInverted liftHillAndAlternativeState{};
            CoordsXYZ trackPos{};

            _currentTrackPrice = kMoney64Undefined;
            _trackPlaceCost = kMoney64Undefined;
            _trackPlaceErrorMessage = kStringIdNone;
            RideConstructionInvalidateCurrentTrack();
            if (WindowRideConstructionUpdateState(
                    &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, &trackPos, &properties))
            {
                WindowRideConstructionUpdateActiveElements();
                return;
            }

            auto currentRide = GetRide(_currentRideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            auto trackPlaceAction = GameActions::TrackPlaceAction(
                rideIndex, trackType, currentRide->type, { trackPos, static_cast<uint8_t>(trackDirection) }, properties & 0xFF,
                (properties >> 8) & 0x0F, (properties >> 12) & 0x0F, liftHillAndAlternativeState, false);
            if (_rideConstructionState == RideConstructionState::Back)
            {
                trackPlaceAction.SetCallback(RideConstructPlacedBackwardGameActionCallback);
            }
            else if (_rideConstructionState == RideConstructionState::Front)
            {
                trackPlaceAction.SetCallback(RideConstructPlacedForwardGameActionCallback);
            }

            auto res = GameActions::Execute(&trackPlaceAction, getGameState());
            if (res.error != GameActions::Status::ok)
            {
                _trackPlaceCost = kMoney64Undefined;
                _trackPlaceErrorMessage = std::get<StringId>(res.errorMessage);
            }
            else
            {
                _trackPlaceCost = res.cost;
                _trackPlaceErrorMessage = kStringIdNone;
            }

            if (res.error != GameActions::Status::ok)
            {
                return;
            }

            Audio::Play3D(Audio::SoundId::placeItem, trackPos);

            if (Network::GetMode() != Network::Mode::none)
            {
                _currentTrackSelectionFlags.set(TrackSelectionFlag::trackPlaceActionQueued);
            }

            const auto resultData = res.getData<GameActions::TrackPlaceActionResult>();
            if (resultData.GroundFlags & ELEMENT_IS_UNDERGROUND)
            {
                ViewportSetVisibility(ViewportVisibility::undergroundViewOn);
            }

            const bool helixSelected = (_currentlySelectedTrack.isTrackType)
                && trackTypeIsHelix(_currentlySelectedTrack.trackType);
            if (helixSelected || (_currentTrackPitchEnd != TrackPitch::none))
            {
                ViewportSetVisibility(ViewportVisibility::trackHeights);
            }
        }

        void MouseUpDemolish()
        {
            int32_t direction;
            TileElement* tileElement;
            CoordsXYE inputElement, outputElement;
            TrackBeginEnd trackBeginEnd;

            _currentTrackPrice = kMoney64Undefined;
            RideConstructionInvalidateCurrentTrack();

            // Select the track element that is to be deleted
            _rideConstructionState2 = RideConstructionState::Selected;
            if (_rideConstructionState == RideConstructionState::Front)
            {
                if (!RideSelectBackwardsFromFront())
                {
                    WindowRideConstructionUpdateActiveElements();
                    return;
                }
                _rideConstructionState2 = RideConstructionState::Front;
            }
            else if (_rideConstructionState == RideConstructionState::Back)
            {
                if (!RideSelectForwardsFromBack())
                {
                    WindowRideConstructionUpdateActiveElements();
                    return;
                }
                _rideConstructionState2 = RideConstructionState::Back;
            }

            // Invalidate the selected track element or make sure it's at origin???
            direction = _currentTrackPieceDirection;
            // The direction is reset by ride_initialise_construction_window(), but we need it to remove flat rides properly.
            Direction currentDirection = _currentTrackPieceDirection;
            TrackElemType type = _currentTrackPieceType;
            auto newCoords = GetTrackElementOriginAndApplyChanges(
                { _currentTrackBegin, static_cast<Direction>(direction & 3) }, type, 0, &tileElement, {});
            if (!newCoords.has_value())
            {
                WindowRideConstructionUpdateActiveElements();
                return;
            }

            // Get the previous track element to go to after the selected track element is deleted
            inputElement.x = newCoords->x;
            inputElement.y = newCoords->y;
            inputElement.element = tileElement;
            if (trackBlockGetPrevious({ *newCoords, tileElement }, &trackBeginEnd))
            {
                *newCoords = { trackBeginEnd.begin_x, trackBeginEnd.begin_y, trackBeginEnd.begin_z };
                direction = trackBeginEnd.begin_direction;
                type = trackBeginEnd.begin_element->AsTrack()->GetTrackType();
                _gotoStartPlacementMode = false;
            }
            else if (trackBlockGetNext(&inputElement, &outputElement, &newCoords->z, &direction))
            {
                newCoords->x = outputElement.x;
                newCoords->y = outputElement.y;
                direction = outputElement.element->GetDirection();
                type = outputElement.element->AsTrack()->GetTrackType();
                _gotoStartPlacementMode = false;
            }
            else
            {
                direction = _currentTrackPieceDirection;
                type = _currentTrackPieceType;
                newCoords = GetTrackElementOriginAndApplyChanges(
                    { _currentTrackBegin, static_cast<Direction>(direction & 3) }, type, 0, &tileElement, {});

                if (!newCoords.has_value())
                {
                    WindowRideConstructionUpdateActiveElements();
                    return;
                }

                const auto& ted = GetTrackElementDescriptor(tileElement->AsTrack()->GetTrackType());
                newCoords->z = (tileElement->GetBaseZ()) - ted.sequenceData.sequences[0].clearance.z;
                _gotoStartPlacementMode = true;
            }

            auto trackRemoveAction = GameActions::TrackRemoveAction(
                _currentTrackPieceType, 0,
                { _currentTrackBegin.x, _currentTrackBegin.y, _currentTrackBegin.z, currentDirection });

            trackRemoveAction.SetCallback([=](const GameActions::GameAction* ga, const GameActions::Result* result) {
                if (result->error != GameActions::Status::ok)
                {
                    WindowRideConstructionUpdateActiveElements();
                }
                else
                {
                    auto currentRide = GetRide(_currentRideIndex);
                    if (currentRide != nullptr)
                    {
                        // When flat rides are deleted, the window should be reset so the currentRide can be placed again.
                        const auto& rtd = currentRide->getRideTypeDescriptor();
                        if (rtd.flags.has(RtdFlag::isFlatRide) && !rtd.flags.has(RtdFlag::isShopOrFacility))
                        {
                            RideInitialiseConstructionWindow(*currentRide);
                        }

                        WindowRideConstructionMouseUpDemolishNextPiece({ *newCoords, static_cast<Direction>(direction) }, type);
                    }
                }
            });

            GameActions::Execute(&trackRemoveAction, getGameState());
        }

        void Rotate()
        {
            _autoRotatingShop = false;
            _currentTrackPieceDirection = (_currentTrackPieceDirection + 1) & 3;
            RideConstructionInvalidateCurrentTrack();
            _currentTrackPrice = kMoney64Undefined;
            WindowRideConstructionUpdateActiveElements();
        }

        void EntranceClick()
        {
            if (ToolSet(*this, WIDX_ENTRANCE, Tool::crosshair))
            {
                auto currentRide = GetRide(_currentRideIndex);
                if (currentRide != nullptr && !RideTryGetOriginElement(*currentRide, nullptr))
                {
                    RideInitialiseConstructionWindow(*currentRide);
                }
            }
            else
            {
                gRideEntranceExitPlaceType = ENTRANCE_TYPE_RIDE_ENTRANCE;
                gRideEntranceExitPlaceRideIndex = _currentRideIndex;
                gRideEntranceExitPlaceStationIndex = StationIndex::FromUnderlying(0);
                gInputFlags.set(InputFlag::allowRightMouseRemoval);
                RideConstructionInvalidateCurrentTrack();
                if (_rideConstructionState != RideConstructionState::EntranceExit)
                {
                    gRideEntranceExitPlacePreviousRideConstructionState = _rideConstructionState;
                    _rideConstructionState = RideConstructionState::EntranceExit;
                }
                WindowRideConstructionUpdateActiveElements();
                // OPENRCT2MINI ride-construction-grid-cursor-plan §16
                // (2026-05-28): when the user clicks Entrance from focus
                // mode (or any non-tool context), engage the grid cursor
                // so the same gamepad placement UX kicks in as after the
                // auto-flip from the last track piece. Without this, the
                // user has to manually press Start after clicking the
                // button. setToolFocusSelected + setSelectorMode flip the
                // resolveActiveContext gates so the next frame routes to
                // RideConstructionContextImpl, whose processFrame then
                // detects the entranceExit transition and auto-positions
                // the cursor + spawns the ghost.
                {
                    auto& inputMgr = OpenRCT2::Ui::GetInputManager();
                    inputMgr.setToolFocusSelected(
                        true,
                        OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                    inputMgr.setSelectorMode(
                        OpenRCT2::Ui::InputManager::SelectorMode::active);
                }
            }
        }

        void ExitClick()
        {
            if (ToolSet(*this, WIDX_EXIT, Tool::crosshair))
            {
                auto currentRide = GetRide(_currentRideIndex);
                if (!RideTryGetOriginElement(*currentRide, nullptr))
                {
                    RideInitialiseConstructionWindow(*currentRide);
                }
            }
            else
            {
                gRideEntranceExitPlaceType = ENTRANCE_TYPE_RIDE_EXIT;
                gRideEntranceExitPlaceRideIndex = _currentRideIndex;
                gRideEntranceExitPlaceStationIndex = StationIndex::FromUnderlying(0);
                gInputFlags.set(InputFlag::allowRightMouseRemoval);
                RideConstructionInvalidateCurrentTrack();
                if (_rideConstructionState != RideConstructionState::EntranceExit)
                {
                    gRideEntranceExitPlacePreviousRideConstructionState = _rideConstructionState;
                    _rideConstructionState = RideConstructionState::EntranceExit;
                }
                WindowRideConstructionUpdateActiveElements();
                // OPENRCT2MINI ride-construction-grid-cursor-plan §16
                // (2026-05-28): mirror EntranceClick — engage grid cursor
                // so focus-mode click → entranceExit gamepad placement.
                {
                    auto& inputMgr = OpenRCT2::Ui::GetInputManager();
                    inputMgr.setToolFocusSelected(
                        true,
                        OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                    inputMgr.setSelectorMode(
                        OpenRCT2::Ui::InputManager::SelectorMode::active);
                }
            }
        }

        void UpdateLiftHillSelected(TrackPitch slope)
        {
            _currentTrackPitchEnd = slope;
            _currentTrackPrice = kMoney64Undefined;
            if (_rideConstructionState == RideConstructionState::Front && !getGameState().cheats.enableChainLiftOnAllTrack)
            {
                switch (slope)
                {
                    case TrackPitch::none:
                    case TrackPitch::up25:
                    case TrackPitch::up60:
                        break;
                    default:
                        _currentTrackHasLiftHill = false;
                        break;
                }
            }
            WindowRideConstructionUpdateActiveElements();
        }

        void SetBrakeSpeed(int32_t brakesSpeed)
        {
            TileElement* tileElement;

            if (GetTrackElementOriginAndApplyChanges(
                    { _currentTrackBegin, static_cast<Direction>(_currentTrackPieceDirection & 3) }, _currentTrackPieceType, 0,
                    &tileElement, {})
                != std::nullopt)
            {
                auto trackSetBrakeSpeed = GameActions::TrackSetBrakeSpeedAction(
                    _currentTrackBegin, tileElement->AsTrack()->GetTrackType(), brakesSpeed);
                trackSetBrakeSpeed.SetCallback([](const GameActions::GameAction* ga, const GameActions::Result* result) {
                    WindowRideConstructionUpdateActiveElements();
                });
                GameActions::Execute(&trackSetBrakeSpeed, getGameState());
                return;
            }
            WindowRideConstructionUpdateActiveElements();
        }

        void ShowSpecialTrackDropdown(Widget* widget)
        {
            auto& elements = _specialElementDropdownState.Elements;

            int32_t defaultIndex = -1;
            int32_t i = 0;
            for (auto& element : elements)
            {
                TrackElemType trackPiece = element.TrackType;

                // Separate elements logically
                if (trackPiece == TrackElemType::none)
                {
                    gDropdown.items[i++] = Dropdown::Separator();
                    continue;
                }

                const auto& ted = GetTrackElementDescriptor(trackPiece);
                StringId trackPieceStringId = ted.description;

                // TODO: this should probably be done elsewhere
                if (trackPieceStringId == STR_RAPIDS)
                {
                    auto currentRide = GetRide(_currentRideIndex);
                    if (currentRide != nullptr)
                    {
                        const auto& rtd = currentRide->getRideTypeDescriptor();
                        if (rtd.Category != RideCategory::water)
                            trackPieceStringId = STR_LOG_BUMPS;
                    }
                }

                gDropdown.items[i] = Dropdown::PlainMenuLabel(trackPieceStringId);
                if (_currentlySelectedTrack == trackPiece)
                    defaultIndex = i;

                i++;
            }

            // Tune dropdown to the elements it contains
            auto ddWidth = widget->width() - 1;
            auto targetColumnSize = _specialElementDropdownState.PreferredNumRows;
            if (targetColumnSize < _specialElementDropdownState.Elements.size())
                ddWidth -= 30;

            WindowDropdownShowTextCustomWidth(
                { windowPos.x + widget->left, windowPos.y + widget->top }, widget->height(), colours[1], 0,
                Dropdown::Flag::StayOpen, elements.size(), ddWidth, targetColumnSize);

            for (size_t j = 0; j < elements.size(); j++)
            {
                gDropdown.items[static_cast<int32_t>(j)].setDisabled(_specialElementDropdownState.Elements[j].Disabled);
            }
            gDropdown.defaultIndex = defaultIndex;
        }

        void RideSelectedTrackSetSeatRotation(int32_t seatRotation)
        {
            GetTrackElementOriginAndApplyChanges(
                { _currentTrackBegin, static_cast<Direction>(_currentTrackPieceDirection & 3) }, _currentTrackPieceType,
                seatRotation, nullptr, { TrackElementSetFlag::seatRotation });
            WindowRideConstructionUpdateActiveElements();
        }

        void ToolDownEntranceExit(const ScreenCoordsXY& screenCoords)
        {
            RideConstructionInvalidateCurrentTrack();
            gMapSelectFlags.unset(MapSelectFlag::enable);
            gMapSelectFlags.unset(MapSelectFlag::enableArrow);

            CoordsXYZD entranceOrExitCoords = RideGetEntranceOrExitPositionFromScreenPosition(screenCoords);
            if (gRideEntranceExitPlaceDirection == kInvalidDirection)
                return;

            auto rideEntranceExitPlaceAction = GameActions::RideEntranceExitPlaceAction(
                entranceOrExitCoords, DirectionReverse(gRideEntranceExitPlaceDirection), gRideEntranceExitPlaceRideIndex,
                gRideEntranceExitPlaceStationIndex, gRideEntranceExitPlaceType == ENTRANCE_TYPE_RIDE_EXIT);

            rideEntranceExitPlaceAction.SetCallback(
                [=](const GameActions::GameAction* ga, const GameActions::Result* result) {
                    if (result->error != GameActions::Status::ok)
                        return;

                    Audio::Play3D(Audio::SoundId::placeItem, result->position);

                    auto* windowMgr = GetWindowManager();

                    auto currentRide = GetRide(gRideEntranceExitPlaceRideIndex);
                    if (currentRide != nullptr && RideAreAllPossibleEntrancesAndExitsBuilt(*currentRide).Successful)
                    {
                        ToolCancel();
                        if (!currentRide->getRideTypeDescriptor().flags.has(RtdFlag::hasTrack))
                        {
                            windowMgr->CloseByClass(WindowClass::rideConstruction);
                        }
                    }
                    else
                    {
                        gRideEntranceExitPlaceType = gRideEntranceExitPlaceType ^ 1;
                        windowMgr->InvalidateByClass(WindowClass::rideConstruction);

                        auto newToolWidgetIndex = (gRideEntranceExitPlaceType == ENTRANCE_TYPE_RIDE_ENTRANCE)
                            ? WC_RIDE_CONSTRUCTION__WIDX_ENTRANCE
                            : WC_RIDE_CONSTRUCTION__WIDX_EXIT;

                        ToolCancel();
                        ToolSet(*this, newToolWidgetIndex, Tool::crosshair);
                    }
                });
            auto res = GameActions::Execute(&rideEntranceExitPlaceAction, getGameState());
        }

        void DrawTrackPiece(
            Drawing::RenderTarget& rt, RideId rideIndex, TrackElemType trackType, int32_t trackDirection,
            SelectedLiftAndInverted liftHillAndInvertedState, int32_t widgetWidth, int32_t widgetHeight)
        {
            auto currentRide = GetRide(rideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            const auto& ted = GetTrackElementDescriptor(trackType);
            const auto trackBlock = ted.sequenceData.sequences[ted.sequenceData.numSequences - 1].clearance;
            CoordsXYZ mapCoords{ trackBlock.x, trackBlock.y, trackBlock.z };
            if (trackBlock.flags.has(ClearanceFlag::flag1))
            {
                mapCoords.x = 0;
                mapCoords.y = 0;
            }

            auto rotatedMapCoords = mapCoords.Rotate(trackDirection);
            // this is actually case 0, but the other cases all jump to it
            mapCoords.x = 4112 + (rotatedMapCoords.x / 2);
            mapCoords.y = 4112 + (rotatedMapCoords.y / 2);
            mapCoords.z = 1024 + mapCoords.z;

            auto previewZOffset = ted.definition.previewZOffset;
            mapCoords.z -= previewZOffset;

            const ScreenCoordsXY rotatedScreenCoords = Translate3DTo2DWithZ(GetCurrentRotation(), mapCoords);

            rt.x += rotatedScreenCoords.x - widgetWidth / 2;
            rt.y += rotatedScreenCoords.y - widgetHeight / 2 - 16;

            rt.cullingX = rt.x;
            rt.cullingY = rt.y;
            rt.cullingWidth = rt.width;
            rt.cullingHeight = rt.height;

            DrawTrackPieceHelper(rt, rideIndex, trackType, trackDirection, liftHillAndInvertedState, { 4096, 4096 }, 1024);
        }

        void DrawTrackPieceHelper(
            Drawing::RenderTarget& rt, RideId rideIndex, TrackElemType trackType, int32_t trackDirection,
            SelectedLiftAndInverted liftHillAndInvertedState, const CoordsXY& originCoords, int32_t originZ)
        {
            TileElement tempSideTrackTileElement{ 0x80, 0x8F, 128, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            TileElement tempTrackTileElement{};
            TileElement* backupTileElementArrays[5]{};
            PaintSession* session = PaintSessionAlloc(rt, 0, GetCurrentRotation());
            trackDirection &= 3;

            auto currentRide = GetRide(rideIndex);
            if (currentRide == nullptr)
            {
                return;
            }

            auto& gameState = getGameState();
            auto preserveMapSize = gameState.mapSize;

            gameState.mapSize = { kMaximumMapSizeTechnical, kMaximumMapSizeTechnical };

            // Setup non changing parts of the temporary track tile element
            tempTrackTileElement.SetType(TileElementType::Track);
            tempTrackTileElement.SetDirection(trackDirection);
            tempTrackTileElement.AsTrack()->SetHasChain(liftHillAndInvertedState.has(LiftHillAndInverted::liftHill));
            tempTrackTileElement.SetLastForTile(true);
            tempTrackTileElement.AsTrack()->SetTrackType(trackType);
            tempTrackTileElement.AsTrack()->SetRideType(currentRide->type);
            tempTrackTileElement.AsTrack()->SetHasCableLift(false);
            tempTrackTileElement.AsTrack()->SetInverted(liftHillAndInvertedState.has(LiftHillAndInverted::inverted));
            tempTrackTileElement.AsTrack()->SetColourScheme(_currentColourScheme);
            // Skipping seat rotation, should not be necessary for a temporary piece.
            tempTrackTileElement.AsTrack()->SetRideIndex(rideIndex);

            const auto& ted = GetTrackElementDescriptor(trackType);
            const auto* rideEntry = currentRide->getRideEntry();
            auto clearanceHeight = (rideEntry != nullptr) ? rideEntry->Clearance
                                                          : currentRide->getRideTypeDescriptor().Heights.ClearanceHeight;

            for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
            {
                const auto& trackBlock = ted.sequenceData.sequences[i].clearance;

                auto quarterTile = trackBlock.quarterTile.Rotate(trackDirection);
                CoordsXY offsets = { trackBlock.x, trackBlock.y };
                CoordsXY coords = originCoords + offsets.Rotate(trackDirection);

                int32_t baseZ = originZ + trackBlock.z;
                int32_t clearanceZ = trackBlock.clearanceZ + clearanceHeight + baseZ + (4 * kCoordsZStep);

                auto centreTileCoords = TileCoordsXY{ coords };
                auto eastTileCoords = centreTileCoords + TileDirectionDelta[TILE_ELEMENT_DIRECTION_EAST];
                auto westTileCoords = centreTileCoords + TileDirectionDelta[TILE_ELEMENT_DIRECTION_WEST];
                auto northTileCoords = centreTileCoords + TileDirectionDelta[TILE_ELEMENT_DIRECTION_NORTH];
                auto southTileCoords = centreTileCoords + TileDirectionDelta[TILE_ELEMENT_DIRECTION_SOUTH];

                // Replace map elements with temporary ones containing track
                backupTileElementArrays[0] = MapGetFirstElementAt(centreTileCoords);
                backupTileElementArrays[1] = MapGetFirstElementAt(eastTileCoords);
                backupTileElementArrays[2] = MapGetFirstElementAt(westTileCoords);
                backupTileElementArrays[3] = MapGetFirstElementAt(northTileCoords);
                backupTileElementArrays[4] = MapGetFirstElementAt(southTileCoords);
                MapSetTileElement(centreTileCoords, &tempTrackTileElement);
                MapSetTileElement(eastTileCoords, &tempSideTrackTileElement);
                MapSetTileElement(westTileCoords, &tempSideTrackTileElement);
                MapSetTileElement(northTileCoords, &tempSideTrackTileElement);
                MapSetTileElement(southTileCoords, &tempSideTrackTileElement);

                // Set the temporary track element
                tempTrackTileElement.SetOccupiedQuadrants(quarterTile.GetBaseQuarterOccupied());
                tempTrackTileElement.SetBaseZ(baseZ);
                tempTrackTileElement.SetClearanceZ(clearanceZ);
                tempTrackTileElement.AsTrack()->SetSequenceIndex(i);

                // Draw this map tile
                TileElementPaintSetup(*session, coords, true);

                // Restore map elements
                MapSetTileElement(centreTileCoords, backupTileElementArrays[0]);
                MapSetTileElement(eastTileCoords, backupTileElementArrays[1]);
                MapSetTileElement(westTileCoords, backupTileElementArrays[2]);
                MapSetTileElement(northTileCoords, backupTileElementArrays[3]);
                MapSetTileElement(southTileCoords, backupTileElementArrays[4]);
            }

            gameState.mapSize = preserveMapSize;

            PaintSessionArrange(*session);
            PaintDrawStructs(*session);
            PaintSessionFree(session);
        }
    };

    static void WindowRideConstructionUpdateDisabledPieces(ride_type_t rideType)
    {
        RideTrackGroups disabledGroups{};
        const auto& rtd = GetRideTypeDescriptor(rideType);
        if (rtd.flags.has(RtdFlag::hasTrack))
        {
            // Set all pieces as “disabled”. When looping over the ride entries,
            // pieces will be re-enabled as soon as a single entry supports it.
            disabledGroups.flip();

            auto& objManager = GetContext()->GetObjectManager();
            auto& rideEntries = objManager.GetAllRideEntries(rideType);
            // If there are no vehicles for this ride type, enable all the track pieces.
            if (rideEntries.empty())
            {
                disabledGroups.reset();
            }

            for (auto rideEntryIndex : rideEntries)
            {
                const auto* currentRideEntry = GetRideEntryByIndex(rideEntryIndex);
                if (currentRideEntry == nullptr)
                    continue;

                // Non-default vehicle visuals do not use this system, so we have to assume it supports all the track pieces.
                auto& firstCar = currentRideEntry->Cars[0];
                if ((firstCar.PaintStyle != VEHICLE_VISUAL_DEFAULT && firstCar.PaintStyle != VEHICLE_VISUAL_SPINNING_CARS)
                    || firstCar.flags.hasAny(CarEntryFlag::isChairlift, CarEntryFlag::useSlideSwing))
                {
                    disabledGroups.reset();
                    break;
                }

                // Any pieces that this ride entry supports must be taken out of the array.
                auto supportedPieces = RideEntryGetSupportedTrackPieces(*currentRideEntry);
                disabledGroups &= supportedPieces.flip();
            }
        }

        UpdateDisabledRideGroups(disabledGroups);
    }

    /**
     *
     *  rct2: 0x006CB481
     */
    WindowBase* RideConstructionOpen()
    {
        RideId rideIndex = _currentRideIndex;
        CloseRideWindowForConstruction(rideIndex);

        auto currentRide = GetRide(rideIndex);
        if (currentRide == nullptr)
        {
            return nullptr;
        }

        WindowRideConstructionUpdateDisabledPieces(currentRide->type);

        auto* windowMgr = GetWindowManager();

        const auto& rtd = currentRide->getRideTypeDescriptor();
        switch (rtd.ConstructionWindowContext)
        {
            case RideConstructionWindowContext::Maze:
                return ContextOpenWindowView(WindowView::mazeConstruction);
            case RideConstructionWindowContext::Default:
                return windowMgr->Create<RideConstructionWindow>(
                    WindowClass::rideConstruction, ScreenCoordsXY(0, 29), kWindowSize, WindowFlag::noAutoClose);
        }
        return windowMgr->Create<RideConstructionWindow>(
            WindowClass::rideConstruction, ScreenCoordsXY(0, 29), kWindowSize, WindowFlag::noAutoClose);
    }

    static void CloseConstructWindowOnCompletion(const Ride& ride)
    {
        if (_rideConstructionState == RideConstructionState::State0)
        {
            auto* windowMgr = GetWindowManager();
            auto w = windowMgr->FindByClass(WindowClass::rideConstruction);
            if (w != nullptr)
            {
                if (RideAreAllPossibleEntrancesAndExitsBuilt(ride).Successful)
                {
                    windowMgr->Close(*w);
                }
                else
                {
                    w->onMouseUp(WIDX_ENTRANCE);
                }
            }
        }
    }

    static void WindowRideConstructionDoEntranceExitCheck()
    {
        auto* windowMgr = GetWindowManager();
        auto w = windowMgr->FindByClass(WindowClass::rideConstruction);
        auto ride = GetRide(_currentRideIndex);
        if (w == nullptr || ride == nullptr)
        {
            return;
        }

        if (_rideConstructionState == RideConstructionState::State0)
        {
            w = windowMgr->FindByClass(WindowClass::rideConstruction);
            if (w != nullptr)
            {
                if (!RideAreAllPossibleEntrancesAndExitsBuilt(*ride).Successful)
                {
                    w->onMouseUp(WC_RIDE_CONSTRUCTION__WIDX_ENTRANCE);
                }
            }
        }
    }

    static void RideConstructPlacedForwardGameActionCallback(
        const GameActions::GameAction* ga, const GameActions::Result* result)
    {
        if (result->error != GameActions::Status::ok)
        {
            WindowRideConstructionUpdateActiveElements();
            return;
        }
        auto ride = GetRide(_currentRideIndex);
        if (ride != nullptr)
        {
            int32_t trackDirection = _currentTrackPieceDirection;
            auto trackPos = _currentTrackBegin;
            if (!(trackDirection & 4))
            {
                trackPos -= CoordsDirectionDelta[trackDirection];
            }

            CoordsXYE next_track;
            if (trackBlockGetNextFromZero(trackPos, *ride, trackDirection, &next_track, &trackPos.z, &trackDirection, false))
            {
                _currentTrackBegin.x = next_track.x;
                _currentTrackBegin.y = next_track.y;
                _currentTrackBegin.z = trackPos.z;
                _currentTrackPieceDirection = next_track.element->GetDirection();
                _currentTrackPieceType = next_track.element->AsTrack()->GetTrackType();
                _currentTrackSelectionFlags.clearAll();
                _rideConstructionState = RideConstructionState::Selected;
                _rideConstructionNextArrowPulse = 0;
                gMapSelectFlags.unset(MapSelectFlag::enableArrow);
                RideSelectNextSection();
            }
            else
            {
                _rideConstructionState = RideConstructionState::State0;
            }

            WindowRideConstructionDoEntranceExitCheck();
            WindowRideConstructionUpdateActiveElements();
        }

        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::error);
        if (ride != nullptr)
            CloseConstructWindowOnCompletion(*ride);
    }

    static void RideConstructPlacedBackwardGameActionCallback(
        const GameActions::GameAction* ga, const GameActions::Result* result)
    {
        if (result->error != GameActions::Status::ok)
        {
            WindowRideConstructionUpdateActiveElements();
            return;
        }
        auto ride = GetRide(_currentRideIndex);
        if (ride != nullptr)
        {
            auto trackDirection = DirectionReverse(_currentTrackPieceDirection);
            auto trackPos = _currentTrackBegin;
            if (!(trackDirection & 4))
            {
                trackPos += CoordsDirectionDelta[trackDirection];
            }

            TrackBeginEnd trackBeginEnd;
            if (trackBlockGetPreviousFromZero(trackPos, *ride, trackDirection, &trackBeginEnd))
            {
                _currentTrackBegin.x = trackBeginEnd.begin_x;
                _currentTrackBegin.y = trackBeginEnd.begin_y;
                _currentTrackBegin.z = trackBeginEnd.begin_z;
                _currentTrackPieceDirection = trackBeginEnd.begin_direction;
                _currentTrackPieceType = trackBeginEnd.begin_element->AsTrack()->GetTrackType();
                _currentTrackSelectionFlags.clearAll();
                _rideConstructionState = RideConstructionState::Selected;
                _rideConstructionNextArrowPulse = 0;
                gMapSelectFlags.unset(MapSelectFlag::enableArrow);
                RideSelectPreviousSection();
            }
            else
            {
                _rideConstructionState = RideConstructionState::State0;
            }

            WindowRideConstructionUpdateActiveElements();
        }

        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::error);
        if (ride != nullptr)
            CloseConstructWindowOnCompletion(*ride);
    }

    /**
     *
     *  rct2: 0x006CC538
     */
    static std::optional<CoordsXY> RideGetPlacePositionFromScreenPosition(ScreenCoordsXY screenCoords)
    {
        CoordsXY mapCoords;
        auto& im = GetInputManager();

        if (!_trackPlaceCtrlState)
        {
            if (im.isModifierKeyPressed(ModifierKey::ctrl))
            {
                constexpr auto interactionFlags = EnumsToFlags(
                    ViewportInteractionItem::terrain, ViewportInteractionItem::ride, ViewportInteractionItem::footpath,
                    ViewportInteractionItem::pathAddition, ViewportInteractionItem::largeScenery,
                    ViewportInteractionItem::label, ViewportInteractionItem::banner);

                auto info = GetMapCoordinatesFromPos(screenCoords, interactionFlags);
                if (info.interactionType != ViewportInteractionItem::none)
                {
                    _trackPlaceCtrlZ = info.Element->GetBaseZ();
                    _trackPlaceCtrlState = true;
                }
            }
        }
        else
        {
            if (!(im.isModifierKeyPressed(ModifierKey::ctrl)))
            {
                _trackPlaceCtrlState = false;
            }
        }

        if (!_trackPlaceShiftState)
        {
            if (im.isModifierKeyPressed(ModifierKey::shift))
            {
                _trackPlaceShiftState = true;
                _trackPlaceShiftStart = screenCoords;
                _trackPlaceShiftZ = 0;
            }
        }
        else
        {
            if (im.isModifierKeyPressed(ModifierKey::shift))
            {
                uint16_t maxHeight = ZoomLevel::max().ApplyTo(
                    std::numeric_limits<decltype(TileElement::BaseHeight)>::max() - 32);

                _trackPlaceShiftZ = _trackPlaceShiftStart.y - screenCoords.y + 4;
                // Scale delta by zoom to match mouse position.
                auto* mainWnd = WindowGetMain();
                if (mainWnd != nullptr && mainWnd->viewport != nullptr)
                {
                    _trackPlaceShiftZ = mainWnd->viewport->zoom.ApplyTo(_trackPlaceShiftZ);
                }
                _trackPlaceShiftZ = floor2(_trackPlaceShiftZ, 8);

                // Clamp to maximum possible value of BaseHeight can offer.
                _trackPlaceShiftZ = std::min<int16_t>(_trackPlaceShiftZ, maxHeight);

                screenCoords = _trackPlaceShiftStart;
            }
            else
            {
                _trackPlaceShiftState = false;
            }
        }

        if (!_trackPlaceCtrlState)
        {
            mapCoords = ViewportInteractionGetTileStartAtCursor(screenCoords);
            if (mapCoords.IsNull())
                return std::nullopt;

            _trackPlaceZ = 0;
            if (_trackPlaceShiftState)
            {
                auto surfaceElement = MapGetSurfaceElementAt(mapCoords);
                if (surfaceElement == nullptr)
                    return std::nullopt;
                auto mapZ = floor2(surfaceElement->GetBaseZ(), 16);
                mapZ += _trackPlaceShiftZ;
                mapZ = std::max<int16_t>(mapZ, 16);
                _trackPlaceZ = mapZ;
            }
        }
        else
        {
            auto mapZ = _trackPlaceCtrlZ;
            auto mapXYCoords = ScreenGetMapXYWithZ(screenCoords, mapZ);
            if (mapXYCoords.has_value())
            {
                mapCoords = mapXYCoords.value();
            }
            else
            {
                return std::nullopt;
            }

            if (_trackPlaceShiftState != 0)
            {
                mapZ += _trackPlaceShiftZ;
            }
            _trackPlaceZ = std::max<int32_t>(mapZ, 16);
        }

        if (mapCoords.x == kLocationNull)
            return std::nullopt;

        return mapCoords.ToTileStart();
    }

    /**
     *
     *  rct2: 0x006C84CE
     */
    void WindowRideConstructionUpdateActiveElementsImpl()
    {
        WindowRideConstructionUpdateEnabledTrackPieces();
        if (auto currentRide = GetRide(_currentRideIndex);
            !currentRide || currentRide->getRideTypeDescriptor().specialType == RtdSpecialType::maze)
        {
            return;
        }
        auto* windowMgr = GetWindowManager();
        auto window = static_cast<RideConstructionWindow*>(windowMgr->FindByClass(WindowClass::rideConstruction));
        if (!window)
        {
            return;
        }

        window->updateMapSelection();

        _selectedTrackType = TrackElemType::none;
        if (_rideConstructionState == RideConstructionState::Selected)
        {
            TileElement* tileElement;
            if (GetTrackElementOriginAndApplyChanges(
                    { _currentTrackBegin, static_cast<Direction>(_currentTrackPieceDirection & 3) }, _currentTrackPieceType, 0,
                    &tileElement, {})
                != std::nullopt)
            {
                _selectedTrackType = tileElement->AsTrack()->GetTrackType();
                if (trackTypeHasSpeedSetting(tileElement->AsTrack()->GetTrackType()))
                    _currentBrakeSpeed = tileElement->AsTrack()->GetBrakeBoosterSpeed();
                _currentColourScheme = static_cast<RideColourScheme>(tileElement->AsTrack()->GetColourScheme());
                _currentSeatRotationAngle = tileElement->AsTrack()->GetSeatRotation();
            }
        }

        window->updatePossibleRideConfigurations();
        window->updateWidgets();
    }

    /**
     *
     *  rct2: 0x006C6A77
     */
    void WindowRideConstructionUpdateEnabledTrackPieces()
    {
        auto ride = GetRide(_currentRideIndex);
        if (ride == nullptr)
            return;

        auto rideEntry = ride->getRideEntry();
        if (rideEntry == nullptr)
            return;

        auto trackDrawerDescriptor = getCurrentTrackDrawerDescriptor(ride->getRideTypeDescriptor());
        UpdateEnabledRideGroups(trackDrawerDescriptor);
    }

    /**
     *
     *  rct2: 0x006C94D8
     */
    void UpdateGhostTrackAndArrow()
    {
        RideId rideIndex;
        int32_t direction;
        TrackElemType type;
        SelectedLiftAndInverted liftHillAndAlternativeState{};
        CoordsXYZ trackPos{};

        if (_currentTrackSelectionFlags.has(TrackSelectionFlag::trackPlaceActionQueued))
        {
            return;
        }

        // Recheck if area is fine for new track.
        // Set by footpath placement
        if (_currentTrackSelectionFlags.has(TrackSelectionFlag::recheck))
        {
            RideConstructionInvalidateCurrentTrack();
            _currentTrackSelectionFlags.unset(TrackSelectionFlag::recheck);
        }

        switch (_rideConstructionState)
        {
            case RideConstructionState::Front:
            case RideConstructionState::Back:
            {
                // place ghost piece
                if (!_currentTrackSelectionFlags.has(TrackSelectionFlag::track))
                {
                    if (WindowRideConstructionUpdateState(
                            &type, &direction, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr))
                    {
                        RideConstructionRemoveGhosts();
                    }
                    else
                    {
                        _currentTrackPrice = PlaceProvisionalTrackPiece(
                            rideIndex, type, direction, liftHillAndAlternativeState, trackPos);
                        WindowRideConstructionUpdateActiveElements();

                        gMapSelectArrowPosition = _currentTrackBegin;
                        Direction arrowDirection = _currentTrackPieceDirection;
                        // diagonal pieces trigger this
                        if (arrowDirection >= 4)
                            arrowDirection += 4;
                        if (_rideConstructionState == RideConstructionState::Back)
                            arrowDirection = DirectionReverse(arrowDirection);
                        gMapSelectArrowDirection = arrowDirection;

                        if (!gMapSelectFlags.has(MapSelectFlag::enable))
                        {
                            // Set height to where the next track piece would begin
                            VirtualFloorSetHeight(_currentTrackBegin.z);
                        }
                    }
                }
                // update flashing arrow
                auto curTime = Platform::GetTicks();
                if (_rideConstructionNextArrowPulse >= curTime)
                    break;
                _rideConstructionNextArrowPulse = curTime + kArrowPulseDuration;

                _currentTrackSelectionFlags.flip(TrackSelectionFlag::arrow);

                gMapSelectFlags.unset(MapSelectFlag::enableArrow);
                if (_currentTrackSelectionFlags.has(TrackSelectionFlag::arrow))
                    gMapSelectFlags.set(MapSelectFlag::enableArrow);
                break;
            }
            case RideConstructionState::Selected:
            {
                auto curTime = Platform::GetTicks();
                if (_rideConstructionNextArrowPulse >= curTime)
                    break;
                _rideConstructionNextArrowPulse = curTime + kArrowPulseDuration;

                _currentTrackSelectionFlags.flip(TrackSelectionFlag::arrow);
                direction = _currentTrackPieceDirection & 3;
                type = _currentTrackPieceType;
                auto flag = _currentTrackSelectionFlags.has(TrackSelectionFlag::arrow) ? TrackElementSetFlag::highlightOn
                                                                                       : TrackElementSetFlag::highlightOff;
                auto newCoords = GetTrackElementOriginAndApplyChanges(
                    { _currentTrackBegin, static_cast<Direction>(direction) }, type, 0, nullptr, { flag });
                if (!newCoords.has_value())
                {
                    RideConstructionRemoveGhosts();
                    _rideConstructionState = RideConstructionState::State0;
                }
                break;
            }
            case RideConstructionState::MazeBuild:
            case RideConstructionState::MazeMove:
            case RideConstructionState::MazeFill:
            {
                auto curTime = Platform::GetTicks();
                if (_rideConstructionNextArrowPulse >= curTime)
                    break;
                _rideConstructionNextArrowPulse = curTime + kArrowPulseDuration;

                _currentTrackSelectionFlags.flip(TrackSelectionFlag::arrow);
                trackPos = CoordsXYZ{ _currentTrackBegin.x & 0xFFE0, _currentTrackBegin.y & 0xFFE0, _currentTrackBegin.z + 15 };
                gMapSelectArrowPosition = trackPos;
                gMapSelectArrowDirection = 4;
                if (((_currentTrackBegin.x & 0x1F) | (_currentTrackBegin.y & 0x1F)) != 0)
                {
                    gMapSelectArrowDirection = 6;
                    if (((_currentTrackBegin.x & 0x1F) & (_currentTrackBegin.y & 0x1F)) == 0)
                    {
                        gMapSelectArrowDirection = 5;
                        if ((_currentTrackBegin.y & 0x1F) == 0)
                            gMapSelectArrowDirection = 7;
                    }
                }
                gMapSelectFlags.unset(MapSelectFlag::enableArrow);
                if (_currentTrackSelectionFlags.has(TrackSelectionFlag::arrow))
                    gMapSelectFlags.set(MapSelectFlag::enableArrow);
                break;
            }
            default:
                break;
        }
    }

    /**
     *
     *  rct2: 0x006CC6A8
     */
    void RideConstructionToolupdateConstruct(const ScreenCoordsXY& screenCoords)
    {
        int32_t z;

        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        auto mapCoords = RideGetPlacePositionFromScreenPosition(screenCoords);
        if (!mapCoords)
        {
            RideConstructionInvalidateCurrentTrack();
            return;
        }

        z = _trackPlaceZ;
        if (z == 0)
            z = MapGetHighestZ(*mapCoords);

        gMapSelectFlags.set(MapSelectFlag::enableConstruct);
        gMapSelectFlags.set(MapSelectFlag::enableArrow);
        gMapSelectFlags.unset(MapSelectFlag::green);
        gMapSelectArrowPosition = CoordsXYZ{ *mapCoords, z };
        gMapSelectArrowDirection = _currentTrackPieceDirection;
        MapSelection::clearSelectedTiles();
        MapSelection::addSelectedTile(*mapCoords);

        RideId rideIndex;
        TrackElemType trackType;
        int32_t trackDirection;
        SelectedLiftAndInverted liftHillAndAlternativeState{};
        if (WindowRideConstructionUpdateState(
                &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, nullptr, nullptr))
        {
            RideConstructionInvalidateCurrentTrack();
            return;
        }
        _currentTrackPieceType = trackType;
        auto ride = GetRide(_currentRideIndex);
        if (!ride)
        {
            return;
        }

        const auto& rtd = ride->getRideTypeDescriptor();
        if (rtd.specialType != RtdSpecialType::maze)
        {
            auto* windowMgr = GetWindowManager();
            auto window = static_cast<RideConstructionWindow*>(windowMgr->FindByClass(WindowClass::rideConstruction));
            if (!window)
            {
                return;
            }
            // Re-using this other code, very slight difference from original
            //   - Original code checks for MSB mask instead of 255 on trackPart->var_00
            //   - Original code checks this first as its already set origin tile, probably just a micro optimisation
            window->selectMapTiles(trackType, trackDirection, *mapCoords);
        }

        gMapSelectArrowPosition.z = z;
        if (_trackPlaceZ == 0)
        {
            // Raise z above all slopes and water
            if (gMapSelectFlags.has(MapSelectFlag::enableConstruct))
            {
                int32_t highestZ = 0;
                for (const auto& selectedTile : MapSelection::getSelectedTiles())
                {
                    if (MapIsLocationValid(selectedTile))
                    {
                        z = MapGetHighestZ(selectedTile);
                        if (z > highestZ)
                            highestZ = z;
                    }
                }
            }
            // Loc6CC8BF:
            // z = MapGetHighestZ(x >> 5, y >> 5);
        }
        // Loc6CC91B:
        const auto& ted = GetTrackElementDescriptor(trackType);
        int32_t bx = 0;
        for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
        {
            bx = std::min<int32_t>(bx, ted.sequenceData.sequences[i].clearance.z);
        }
        z -= bx;

        gMapSelectArrowPosition.z = z;
        _currentTrackBegin.x = mapCoords->x;
        _currentTrackBegin.y = mapCoords->y;
        _currentTrackBegin.z = z;
        if (_currentTrackSelectionFlags.has(TrackSelectionFlag::track) && _currentTrackBegin == _previousTrackPiece)
        {
            return;
        }

        _previousTrackPiece = _currentTrackBegin;
        // search for appropriate z value for ghost, up to max ride height
        int numAttempts = (z <= kMaximumTrackHeight ? ((kMaximumTrackHeight - z) / kCoordsZStep + 1) : 2);

        if (rtd.specialType == RtdSpecialType::maze)
        {
            for (int zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
            {
                CoordsXYZ trackPos{};
                WindowRideConstructionUpdateState(
                    &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr);
                _currentTrackPrice = PlaceProvisionalTrackPiece(
                    rideIndex, trackType, trackDirection, liftHillAndAlternativeState, trackPos);
                if (_currentTrackPrice != kMoney64Undefined)
                    break;

                _currentTrackBegin.z -= 8;
                if (_currentTrackBegin.z < 0)
                    break;

                _currentTrackBegin.z += 16;
            }

            auto intent = Intent(INTENT_ACTION_UPDATE_MAZE_CONSTRUCTION);
            ContextBroadcastIntent(&intent);
            return;
        }

        for (int zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
        {
            CoordsXYZ trackPos{};
            WindowRideConstructionUpdateState(
                &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr);
            _currentTrackPrice = PlaceProvisionalTrackPiece(
                rideIndex, trackType, trackDirection, liftHillAndAlternativeState, trackPos);
            mapCoords = trackPos;
            z = trackPos.z;
            if (_currentTrackPrice != kMoney64Undefined)
                break;

            _currentTrackBegin.z -= 8;
            if (_currentTrackBegin.z < 0)
                break;

            _currentTrackBegin.z += 16;
        }

        if (_autoRotatingShop && _rideConstructionState == RideConstructionState::Place
            && ride->getRideTypeDescriptor().flags.has(RtdFlag::isShopOrFacility))
        {
            PathElement* pathsByDir[kNumOrthogonalDirections];

            bool keepOrientation = false;
            for (int8_t i = 0; i < kNumOrthogonalDirections; i++)
            {
                const auto testLoc = CoordsXYZ{ *mapCoords + CoordsDirectionDelta[i], z };
                if (!MapIsLocationOwned(testLoc))
                {
                    pathsByDir[i] = nullptr;
                    continue;
                }

                pathsByDir[i] = MapGetFootpathElement(testLoc);

                if (pathsByDir[i] != nullptr && pathsByDir[i]->IsSloped() && pathsByDir[i]->GetSlopeDirection() != i)
                {
                    pathsByDir[i] = nullptr;
                }

                // Sloped path on the level below
                if (pathsByDir[i] == nullptr)
                {
                    pathsByDir[i] = MapGetFootpathElement({ *mapCoords + CoordsDirectionDelta[i], z - kPathHeightStep });

                    if (pathsByDir[i] != nullptr
                        && (!pathsByDir[i]->IsSloped() || pathsByDir[i]->GetSlopeDirection() != DirectionReverse(i)))
                    {
                        pathsByDir[i] = nullptr;
                    }
                }

                if (pathsByDir[i] != nullptr && pathsByDir[i]->IsQueue())
                {
                    pathsByDir[i] = nullptr;
                }

                if (pathsByDir[i] != nullptr && i == _currentTrackPieceDirection)
                {
                    keepOrientation = true;
                    break;
                }
            }

            if (!keepOrientation)
            {
                for (int8_t i = 0; i < kNumOrthogonalDirections; i++)
                {
                    if (pathsByDir[i] != nullptr)
                    {
                        _currentTrackPieceDirection = i;

                        CoordsXYZ trackPos{};
                        WindowRideConstructionUpdateState(
                            &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr);
                        PlaceProvisionalTrackPiece(rideIndex, trackType, trackDirection, liftHillAndAlternativeState, trackPos);
                        gMapSelectArrowDirection = _currentTrackPieceDirection;
                        break;
                    }
                }
            }
        }

        WindowRideConstructionUpdateActiveElements();
    }

    /**
     *
     *  rct2: 0x006CD354
     */
    void RideConstructionToolupdateEntranceExit(const ScreenCoordsXY& screenCoords)
    {
        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        CoordsXYZD entranceOrExitCoords = RideGetEntranceOrExitPositionFromScreenPosition(screenCoords);
        if (gRideEntranceExitPlaceDirection == kInvalidDirection)
        {
            RideConstructionInvalidateCurrentTrack();
            return;
        }
        gMapSelectFlags.set(MapSelectFlag::enable);
        gMapSelectFlags.set(MapSelectFlag::enableArrow);
        gMapSelectType = MapSelectType::full;
        gMapSelectPositionA = entranceOrExitCoords;
        gMapSelectPositionB = entranceOrExitCoords;
        gMapSelectArrowPosition = entranceOrExitCoords;
        gMapSelectArrowDirection = DirectionReverse(entranceOrExitCoords.direction);

        entranceOrExitCoords.direction = DirectionReverse(gRideEntranceExitPlaceDirection);
        StationIndex stationNum = gRideEntranceExitPlaceStationIndex;
        if (!_currentTrackSelectionFlags.has(TrackSelectionFlag::entranceOrExit)
            || entranceOrExitCoords != gRideEntranceExitGhostPosition || stationNum != gRideEntranceExitGhostStationIndex)
        {
            auto ride = GetRide(_currentRideIndex);
            if (ride != nullptr)
            {
                _currentTrackPrice = RideEntranceExitPlaceGhost(
                    *ride, entranceOrExitCoords, entranceOrExitCoords.direction, gRideEntranceExitPlaceType, stationNum);
            }
            WindowRideConstructionUpdateActiveElements();
            VirtualFloorSetHeight(entranceOrExitCoords.z);
        }
    }

    /**
     *
     *  rct2: 0x006CCA73
     */
    void RideConstructionTooldownConstruct(const ScreenCoordsXY& screenCoords)
    {
        const CursorState* state = ContextGetCursorState();

        WindowBase* w;

        RideConstructionInvalidateCurrentTrack();

        CoordsXYZ mapCoords{};
        TrackElemType trackType;
        int32_t z, highestZ;

        if (WindowRideConstructionUpdateState(&trackType, nullptr, nullptr, nullptr, nullptr, nullptr))
            return;

        z = mapCoords.z;
        _currentTrackPieceType = trackType;

        // Raise z above all slopes and water
        highestZ = 0;
        if (gMapSelectFlags.has(MapSelectFlag::enableConstruct))
        {
            for (const auto& selectedTile : MapSelection::getSelectedTiles())
            {
                if (!MapIsLocationValid(selectedTile))
                    continue;

                z = MapGetHighestZ(selectedTile);
                if (z > highestZ)
                    highestZ = z;
            }
        }

        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        auto ridePlacePosition = RideGetPlacePositionFromScreenPosition(screenCoords);
        if (!ridePlacePosition)
            return;

        mapCoords = { *ridePlacePosition, z };

        z = _trackPlaceZ;
        if (z == 0)
            z = MapGetHighestZ(mapCoords);

        ToolCancel();

        auto ride = GetRide(_currentRideIndex);
        if (ride == nullptr)
            return;

        if (_trackPlaceZ == 0)
        {
            const auto& ted = GetTrackElementDescriptor(_currentTrackPieceType);
            int32_t bx = 0;
            for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
            {
                bx = std::min<int32_t>(bx, ted.sequenceData.sequences[i].clearance.z);
            }
            z -= bx;

            // FIX not sure exactly why it starts trial and error place from a lower Z, but it causes issues with disable
            // clearance
            if (!getGameState().cheats.disableClearanceChecks && z > kMinimumLandZ)
            {
                z -= kLandHeightStep;
            }
        }
        else
        {
            z = _trackPlaceZ;
        }

        // search for z value to build at, up to max ride height
        int numAttempts = (z <= kMaximumTrackHeight ? ((kMaximumTrackHeight - z) / kCoordsZStep + 1) : 2);

        const auto& rtd = ride->getRideTypeDescriptor();
        if (rtd.specialType == RtdSpecialType::maze)
        {
            for (int32_t zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
            {
                _rideConstructionState = RideConstructionState::MazeBuild;
                _currentTrackBegin.x = mapCoords.x;
                _currentTrackBegin.y = mapCoords.y;
                _currentTrackBegin.z = z;
                _currentTrackSelectionFlags.clearAll();
                auto intent = Intent(INTENT_ACTION_UPDATE_MAZE_CONSTRUCTION);
                ContextBroadcastIntent(&intent);

                auto* windowMgr = GetWindowManager();
                w = windowMgr->FindByClass(WindowClass::rideConstruction);
                if (w == nullptr)
                    break;

                gDisableErrorWindowSound = true;

                auto gameAction = GameActions::MazeSetTrackAction(
                    CoordsXYZD{ _currentTrackBegin, 0 }, true, _currentRideIndex, MazeBuildMode::build);
                auto mazeSetTrackResult = GameActions::Execute(&gameAction, getGameState());
                if (mazeSetTrackResult.error == GameActions::Status::ok)
                {
                    _trackPlaceCost = mazeSetTrackResult.cost;
                    _trackPlaceErrorMessage = kStringIdNone;
                }
                else
                {
                    _trackPlaceCost = kMoney64Undefined;
                    _trackPlaceErrorMessage = std::get<StringId>(mazeSetTrackResult.errorMessage);
                }

                gDisableErrorWindowSound = false;

                if (mazeSetTrackResult.error != GameActions::Status::ok)
                {
                    _rideConstructionState = RideConstructionState::Place;
                    StringId errorText = std::get<StringId>(mazeSetTrackResult.errorMessage);
                    z -= 8;
                    if (errorText == STR_NOT_ENOUGH_CASH_REQUIRES || errorText == STR_CAN_ONLY_BUILD_THIS_UNDERWATER
                        || errorText == STR_CAN_ONLY_BUILD_THIS_ON_WATER || errorText == STR_RIDE_CANT_BUILD_THIS_UNDERWATER
                        || errorText == STR_CAN_ONLY_BUILD_THIS_ABOVE_GROUND || errorText == STR_TOO_HIGH_FOR_SUPPORTS
                        || zAttempts == (numAttempts - 1) || z < 0)
                    {
                        Audio::Play(Audio::SoundId::error, 0, state->position.x);
                        w = windowMgr->FindByClass(WindowClass::rideConstruction);
                        if (w != nullptr)
                        {
                            ToolSet(*w, WIDX_CONSTRUCT, Tool::crosshair);
                            gInputFlags.set(InputFlag::allowRightMouseRemoval);
                            _trackPlaceCtrlState = false;
                            _trackPlaceShiftState = false;
                        }
                        auto intent2 = Intent(INTENT_ACTION_UPDATE_MAZE_CONSTRUCTION);
                        ContextBroadcastIntent(&intent2);
                        break;
                    }
                    z += 16;
                }
                else
                {
                    windowMgr->CloseByClass(WindowClass::error);
                    Audio::Play3D(Audio::SoundId::placeItem, _currentTrackBegin);
                    break;
                }
            }
            return;
        }

        for (int32_t zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
        {
            _rideConstructionState = RideConstructionState::Front;
            _currentTrackBegin.x = mapCoords.x;
            _currentTrackBegin.y = mapCoords.y;
            _currentTrackBegin.z = z;
            _currentTrackSelectionFlags.clearAll();
            WindowRideConstructionUpdateActiveElements();

            auto* windowMgr = GetWindowManager();
            w = windowMgr->FindByClass(WindowClass::rideConstruction);
            if (w == nullptr)
                break;

            gDisableErrorWindowSound = true;
            w->onMouseDown(WIDX_CONSTRUCT);
            gDisableErrorWindowSound = false;

            if (_trackPlaceCost == kMoney64Undefined)
            {
                StringId errorText = _trackPlaceErrorMessage;
                z -= 8;
                if (errorText == STR_NOT_ENOUGH_CASH_REQUIRES || errorText == STR_CAN_ONLY_BUILD_THIS_UNDERWATER
                    || errorText == STR_CAN_ONLY_BUILD_THIS_ON_WATER || errorText == STR_CAN_ONLY_BUILD_THIS_ABOVE_GROUND
                    || errorText == STR_TOO_HIGH_FOR_SUPPORTS || errorText == STR_TOO_HIGH
                    || errorText == STR_LOCAL_AUTHORITY_WONT_ALLOW_CONSTRUCTION_ABOVE_TREE_HEIGHT
                    || zAttempts == (numAttempts - 1) || z < 0)
                {
                    int32_t saveTrackDirection = _currentTrackPieceDirection;
                    auto saveCurrentTrackCurve = _currentlySelectedTrack;
                    auto savePreviousTrackPitchEnd = _previousTrackPitchEnd;
                    auto saveCurrentTrackPitchEnd = _currentTrackPitchEnd;
                    auto savePreviousTrackRollEnd = _previousTrackRollEnd;
                    auto saveCurrentTrackRollEnd = _currentTrackRollEnd;
                    auto savedCurrentTrackAlternative = _currentTrackAlternative;
                    auto savedCurrentTrackLiftHill = _currentTrackHasLiftHill;

                    RideInitialiseConstructionWindow(*ride);

                    _currentTrackPieceDirection = saveTrackDirection;
                    _currentlySelectedTrack = saveCurrentTrackCurve;
                    _previousTrackPitchEnd = savePreviousTrackPitchEnd;
                    _currentTrackPitchEnd = saveCurrentTrackPitchEnd;
                    _previousTrackRollEnd = savePreviousTrackRollEnd;
                    _currentTrackRollEnd = saveCurrentTrackRollEnd;
                    _currentTrackAlternative = savedCurrentTrackAlternative;
                    _currentTrackHasLiftHill = savedCurrentTrackLiftHill;

                    Audio::Play(Audio::SoundId::error, 0, state->position.x);
                    break;
                }

                z += 16;
            }
            else
            {
                break;
            }
        }
    }

    void WindowRideConstructionKeyboardShortcutTurnLeft()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_STRAIGHT) || w->widgets[WIDX_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        if (_currentlySelectedTrack.isTrackType)
            return;

        switch (_currentlySelectedTrack.curve)
        {
            case TrackCurve::leftSmall:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                break;
            case TrackCurve::left:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::leftLarge:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::none:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::rightLarge:
                if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::right:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::rightSmall:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::rightVerySmall:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_LEFT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::leftVerySmall:
                break;
        }
    }

    void WindowRideConstructionKeyboardShortcutTurnRight()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_STRAIGHT) || w->widgets[WIDX_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        if (_currentlySelectedTrack.isTrackType)
            return;

        switch (_currentlySelectedTrack.curve)
        {
            case TrackCurve::rightSmall:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                break;
            case TrackCurve::right:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::rightLarge:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::none:
                if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::leftLarge:
                if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::left:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::leftSmall:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::leftVerySmall:
                if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE_SMALL) && w->widgets[WIDX_LEFT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_SMALL);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEFT_CURVE) && w->widgets[WIDX_LEFT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_LEFT_CURVE_LARGE) && w->widgets[WIDX_LEFT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEFT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_STRAIGHT);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_LARGE)
                    && w->widgets[WIDX_RIGHT_CURVE_LARGE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_LARGE);
                }
                else if (!widgetIsDisabled(*w, WIDX_RIGHT_CURVE) && w->widgets[WIDX_RIGHT_CURVE].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_SMALL);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_RIGHT_CURVE_VERY_SMALL)
                    && w->widgets[WIDX_RIGHT_CURVE_VERY_SMALL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_RIGHT_CURVE_VERY_SMALL);
                }
                else
                {
                    return;
                }
                break;
            case TrackCurve::rightVerySmall:
                break;
        }
    }

    void WindowRideConstructionKeyboardShortcutUseTrackDefault()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_STRAIGHT) || w->widgets[WIDX_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        if (!widgetIsDisabled(*w, WIDX_STRAIGHT) && w->widgets[WIDX_STRAIGHT].type != WidgetType::empty)
        {
            w->onMouseDown(WIDX_STRAIGHT);
        }

        if (!widgetIsDisabled(*w, WIDX_LEVEL) && w->widgets[WIDX_LEVEL].type != WidgetType::empty)
        {
            w->onMouseDown(WIDX_LEVEL);
        }

        if (!widgetIsDisabled(*w, WIDX_CHAIN_LIFT) && w->widgets[WIDX_CHAIN_LIFT].type != WidgetType::empty
            && _currentTrackHasLiftHill)
        {
            w->onMouseDown(WIDX_CHAIN_LIFT);
        }

        if (!widgetIsDisabled(*w, WIDX_BANK_STRAIGHT) && w->widgets[WIDX_BANK_STRAIGHT].type != WidgetType::empty)
        {
            w->onMouseDown(WIDX_BANK_STRAIGHT);
        }
    }

    void WindowRideConstructionKeyboardShortcutSlopeDown()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_STRAIGHT) || w->widgets[WIDX_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        switch (_currentTrackPitchEnd)
        {
            case TrackPitch::down60:
                if (IsTrackEnabled(TrackGroup::slopeVertical) && !widgetIsDisabled(*w, WIDX_SLOPE_DOWN_VERTICAL)
                    && w->widgets[WIDX_SLOPE_DOWN_VERTICAL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_VERTICAL);
                }
                break;
            case TrackPitch::down25:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_DOWN_STEEP) && w->widgets[WIDX_SLOPE_DOWN_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_STEEP);
                }
                break;
            case TrackPitch::none:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_DOWN) && w->widgets[WIDX_SLOPE_DOWN].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN);
                }
                else if (IsTrackEnabled(TrackGroup::slopeVertical))
                {
                    return;
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_DOWN_STEEP) && w->widgets[WIDX_SLOPE_DOWN_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::up25:
                if (!widgetIsDisabled(*w, WIDX_LEVEL) && w->widgets[WIDX_LEVEL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEVEL);
                }
                else if (!widgetIsDisabled(*w, WIDX_SLOPE_DOWN) && w->widgets[WIDX_SLOPE_DOWN].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_DOWN_STEEP) && w->widgets[WIDX_SLOPE_DOWN_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::up60:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_UP) && w->widgets[WIDX_SLOPE_UP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEVEL) && w->widgets[WIDX_LEVEL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEVEL);
                }
                else if (!widgetIsDisabled(*w, WIDX_SLOPE_DOWN) && w->widgets[WIDX_SLOPE_DOWN].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN);
                }
                else if (IsTrackEnabled(TrackGroup::slopeVertical))
                {
                    return;
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_DOWN_STEEP) && w->widgets[WIDX_SLOPE_DOWN_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::up90:
                if (IsTrackEnabled(TrackGroup::slopeVertical) && !widgetIsDisabled(*w, WIDX_SLOPE_UP_STEEP)
                    && w->widgets[WIDX_SLOPE_UP_VERTICAL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_STEEP);
                }
                break;
            default:
                return;
        }
    }

    void WindowRideConstructionKeyboardShortcutSlopeUp()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_STRAIGHT) || w->widgets[WIDX_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        switch (_currentTrackPitchEnd)
        {
            case TrackPitch::up60:
                if (IsTrackEnabled(TrackGroup::slopeVertical) && !widgetIsDisabled(*w, WIDX_SLOPE_UP_VERTICAL)
                    && w->widgets[WIDX_SLOPE_UP_VERTICAL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_VERTICAL);
                }
                break;
            case TrackPitch::up25:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_UP_STEEP) && w->widgets[WIDX_SLOPE_UP_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_STEEP);
                }
                break;
            case TrackPitch::none:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_UP) && w->widgets[WIDX_SLOPE_UP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP);
                }
                else if (IsTrackEnabled(TrackGroup::slopeVertical))
                {
                    return;
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_UP_STEEP) && w->widgets[WIDX_SLOPE_UP_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::down25:
                if (!widgetIsDisabled(*w, WIDX_LEVEL) && w->widgets[WIDX_LEVEL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEVEL);
                }
                else if (!widgetIsDisabled(*w, WIDX_SLOPE_UP) && w->widgets[WIDX_SLOPE_UP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP);
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_UP_STEEP) && w->widgets[WIDX_SLOPE_UP_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::down60:
                if (!widgetIsDisabled(*w, WIDX_SLOPE_DOWN) && w->widgets[WIDX_SLOPE_DOWN].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN);
                }
                else if (!widgetIsDisabled(*w, WIDX_LEVEL) && w->widgets[WIDX_LEVEL].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_LEVEL);
                }
                else if (!widgetIsDisabled(*w, WIDX_SLOPE_UP) && w->widgets[WIDX_SLOPE_UP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP);
                }
                else if (IsTrackEnabled(TrackGroup::slopeVertical))
                {
                    return;
                }
                else if (
                    !widgetIsDisabled(*w, WIDX_SLOPE_UP_STEEP) && w->widgets[WIDX_SLOPE_UP_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_UP_STEEP);
                }
                else
                {
                    return;
                }
                break;
            case TrackPitch::down90:
                if (IsTrackEnabled(TrackGroup::slopeVertical) && !widgetIsDisabled(*w, WIDX_SLOPE_DOWN_STEEP)
                    && w->widgets[WIDX_SLOPE_DOWN_STEEP].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_SLOPE_DOWN_STEEP);
                }
                break;
            default:
                return;
        }
    }

    void WindowRideConstructionKeyboardShortcutChainLiftToggle()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_CHAIN_LIFT) || w->widgets[WIDX_CHAIN_LIFT].type == WidgetType::empty)
        {
            return;
        }

        w->onMouseDown(WIDX_CHAIN_LIFT);
    }

    void WindowRideConstructionKeyboardShortcutBankLeft()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_BANK_STRAIGHT)
            || w->widgets[WIDX_BANK_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        switch (_currentTrackRollEnd)
        {
            case TrackRoll::none:
                if (!widgetIsDisabled(*w, WIDX_BANK_LEFT) && w->widgets[WIDX_BANK_LEFT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_LEFT);
                }
                break;
            case TrackRoll::right:
                if (!widgetIsDisabled(*w, WIDX_BANK_STRAIGHT) && w->widgets[WIDX_BANK_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_STRAIGHT);
                }
                else if (!widgetIsDisabled(*w, WIDX_BANK_LEFT) && w->widgets[WIDX_BANK_LEFT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_LEFT);
                }
                else
                {
                    return;
                }
                break;
            default:
                return;
        }
    }

    void WindowRideConstructionKeyboardShortcutBankRight()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_BANK_STRAIGHT)
            || w->widgets[WIDX_BANK_STRAIGHT].type == WidgetType::empty)
        {
            return;
        }

        switch (_currentTrackRollEnd)
        {
            case TrackRoll::none:
                if (!widgetIsDisabled(*w, WIDX_BANK_RIGHT) && w->widgets[WIDX_BANK_RIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_RIGHT);
                }
                break;
            case TrackRoll::left:
                if (!widgetIsDisabled(*w, WIDX_BANK_STRAIGHT) && w->widgets[WIDX_BANK_STRAIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_STRAIGHT);
                }
                else if (!widgetIsDisabled(*w, WIDX_BANK_RIGHT) && w->widgets[WIDX_BANK_RIGHT].type != WidgetType::empty)
                {
                    w->onMouseDown(WIDX_BANK_RIGHT);
                }
                else
                {
                    return;
                }
                break;
            default:
                return;
        }
    }

    void WindowRideConstructionKeyboardShortcutPreviousTrack()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_PREVIOUS_SECTION)
            || w->widgets[WIDX_PREVIOUS_SECTION].type == WidgetType::empty)
        {
            return;
        }

        w->onMouseDown(WIDX_PREVIOUS_SECTION);
    }

    void WindowRideConstructionKeyboardShortcutNextTrack()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_NEXT_SECTION) || w->widgets[WIDX_NEXT_SECTION].type == WidgetType::empty)
        {
            return;
        }

        w->onMouseDown(WIDX_NEXT_SECTION);
    }

    void WindowRideConstructionKeyboardShortcutBuildCurrent()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_CONSTRUCT) || w->widgets[WIDX_CONSTRUCT].type == WidgetType::empty)
        {
            return;
        }

        w->onMouseDown(WIDX_CONSTRUCT);
    }

    void WindowRideConstructionKeyboardShortcutDemolishCurrent()
    {
        auto* windowMgr = GetWindowManager();
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || widgetIsDisabled(*w, WIDX_DEMOLISH) || w->widgets[WIDX_DEMOLISH].type == WidgetType::empty)
        {
            return;
        }

        w->onMouseDown(WIDX_DEMOLISH);
    }

    static void WindowRideConstructionMouseUpDemolishNextPiece(const CoordsXYZD& piecePos, TrackElemType type)
    {
        if (_gotoStartPlacementMode)
        {
            _currentTrackBegin.z = floor2(piecePos.z, kCoordsZStep);
            _rideConstructionState = RideConstructionState::Front;
            _currentTrackSelectionFlags.clearAll();
            _currentTrackPieceDirection = piecePos.direction & 3;
            auto savedSelectedTrack = _currentlySelectedTrack;
            auto savedPreviousTrackPitchEnd = _previousTrackPitchEnd;
            auto savedCurrentTrackPitchEnd = _currentTrackPitchEnd;
            auto savedPreviousTrackRollEnd = _previousTrackRollEnd;
            auto savedCurrentTrackRollEnd = _currentTrackRollEnd;
            auto savedCurrentTrackAlternative = _currentTrackAlternative;
            auto savedCurrentTrackLiftHill = _currentTrackHasLiftHill;
            RideConstructionSetDefaultNextPiece();
            WindowRideConstructionUpdateActiveElements();
            auto ride = GetRide(_currentRideIndex);
            if (!RideTryGetOriginElement(*ride, nullptr))
            {
                RideInitialiseConstructionWindow(*ride);
                _currentTrackPieceDirection = piecePos.direction & 3;
                if (!savedSelectedTrack.isTrackType)
                {
                    _currentlySelectedTrack = savedSelectedTrack;
                    _previousTrackPitchEnd = savedPreviousTrackPitchEnd;
                    _currentTrackPitchEnd = savedCurrentTrackPitchEnd;
                    _previousTrackRollEnd = savedPreviousTrackRollEnd;
                    _currentTrackRollEnd = savedCurrentTrackRollEnd;
                    _currentTrackAlternative = savedCurrentTrackAlternative;
                    _currentTrackHasLiftHill = savedCurrentTrackLiftHill;
                    WindowRideConstructionUpdateActiveElements();
                }
            }
        }
        else
        {
            if (_rideConstructionState2 == RideConstructionState::Selected
                || _rideConstructionState2 == RideConstructionState::Front)
            {
                if (type == TrackElemType::middleStation || type == TrackElemType::beginStation)
                {
                    type = TrackElemType::endStation;
                }
            }
            if (_rideConstructionState2 == RideConstructionState::Back)
            {
                if (type == TrackElemType::middleStation)
                {
                    type = TrackElemType::beginStation;
                }
            }
            if (Network::GetMode() == Network::Mode::client)
            {
                // rideConstructionState needs to be set again to the proper value, this only affects the client
                _rideConstructionState = RideConstructionState::Selected;
            }
            _currentTrackBegin = piecePos;
            _currentTrackPieceDirection = piecePos.direction;
            _currentTrackPieceType = type;
            _currentTrackSelectionFlags.clearAll();
            if (_rideConstructionState2 == RideConstructionState::Front)
            {
                RideSelectNextSection();
            }
            else if (_rideConstructionState2 == RideConstructionState::Back)
            {
                RideSelectPreviousSection();
            }
            WindowRideConstructionUpdateActiveElements();
        }
    }

    static void WindowRideConstructionUpdateActiveElements()
    {
        auto intent = Intent(INTENT_ACTION_RIDE_CONSTRUCTION_UPDATE_ACTIVE_ELEMENTS);
        ContextBroadcastIntent(&intent);
    }

    /**
     *
     *  rct2: 0x006CA162
     */
    money64 PlaceProvisionalTrackPiece(
        RideId rideIndex, TrackElemType trackType, int32_t trackDirection, SelectedLiftAndInverted liftHillAndAlternativeState,
        const CoordsXYZ& trackPos)
    {
        auto ride = GetRide(rideIndex);
        if (ride == nullptr)
            return kMoney64Undefined;

        RideConstructionRemoveGhosts();
        const auto& rtd = ride->getRideTypeDescriptor();
        if (rtd.specialType == RtdSpecialType::maze)
        {
            CommandFlags flags = { CommandFlag::allowDuringPaused, CommandFlag::noSpend, CommandFlag::ghost };
            auto gameAction = GameActions::MazeSetTrackAction(CoordsXYZD{ trackPos, 0 }, true, rideIndex, MazeBuildMode::build);
            gameAction.SetFlags(flags);
            auto result = GameActions::Execute(&gameAction, getGameState());

            if (result.error != GameActions::Status::ok)
                return kMoney64Undefined;

            _unkF440C5 = { trackPos, static_cast<Direction>(trackDirection) };
            _currentTrackSelectionFlags.set(TrackSelectionFlag::track);
            ViewportSetVisibility(ViewportVisibility::undergroundViewOff);
            if (_currentTrackPitchEnd != TrackPitch::none)
                ViewportSetVisibility(ViewportVisibility::trackHeights);

            return result.cost;
        }

        auto trackPlaceAction = GameActions::TrackPlaceAction(
            rideIndex, trackType, ride->type, { trackPos, static_cast<uint8_t>(trackDirection) }, 0, 0, 0,
            liftHillAndAlternativeState, false);
        trackPlaceAction.SetFlags({ CommandFlag::allowDuringPaused, CommandFlag::noSpend, CommandFlag::ghost });
        // This command must not be sent over the network
        auto res = GameActions::Execute(&trackPlaceAction, getGameState());
        if (res.error != GameActions::Status::ok)
            return kMoney64Undefined;

        int16_t zBegin{}, zEnd{};
        const auto& ted = GetTrackElementDescriptor(trackType);
        const TrackCoordinates& coords = ted.coordinates;
        if (ride->getRideTypeDescriptor().flags.has(RtdFlag::hasTrack))
        {
            zBegin = coords.zBegin;
            zEnd = coords.zEnd;
        }
        else
        {
            zEnd = zBegin = coords.zBegin;
        }

        _unkF440C5 = { trackPos.x, trackPos.y, trackPos.z + zBegin, static_cast<Direction>(trackDirection) };
        _currentTrackSelectionFlags.set(TrackSelectionFlag::track);

        const auto resultData = res.getData<GameActions::TrackPlaceActionResult>();
        const auto visiblity = (resultData.GroundFlags & ELEMENT_IS_UNDERGROUND) ? ViewportVisibility::undergroundViewOn
                                                                                 : ViewportVisibility::undergroundViewOff;
        ViewportSetVisibility(visiblity);
        if (_currentTrackPitchEnd != TrackPitch::none)
            ViewportSetVisibility(ViewportVisibility::trackHeights);

        if (!gMapSelectFlags.has(MapSelectFlag::enable))
        {
            // Set height to where the next track piece would begin
            VirtualFloorSetHeight(trackPos.z - zBegin + zEnd);
        }

        return res.cost;
    }

    static std::pair<bool, TrackElemType> WindowRideConstructionUpdateStateGetTrackElement()
    {
        auto startSlope = _previousTrackPitchEnd;
        auto endSlope = _currentTrackPitchEnd;
        auto startBank = _previousTrackRollEnd;
        auto endBank = _currentTrackRollEnd;

        if (_rideConstructionState == RideConstructionState::Back)
        {
            startSlope = _currentTrackPitchEnd;
            endSlope = _previousTrackPitchEnd;
            startBank = _currentTrackRollEnd;
            endBank = _previousTrackRollEnd;
        }

        auto selectedTrack = _currentlySelectedTrack;
        if (selectedTrack == TrackElemType::none)
        {
            return std::make_pair(false, TrackElemType::flat);
        }

        bool startsDiagonal = (_currentTrackPieceDirection & (1 << 2)) != 0;
        if (selectedTrack == TrackCurve::leftLarge || selectedTrack == TrackCurve::rightLarge)
        {
            if (_rideConstructionState == RideConstructionState::Back)
            {
                startsDiagonal = !startsDiagonal;
            }
        }

        if (!selectedTrack.isTrackType)
        {
            auto trackPiece = GetTrackTypeFromCurve(
                selectedTrack.curve, startsDiagonal, startSlope, endSlope, startBank, endBank);
            if (trackPiece != TrackElemType::none)
                return std::make_pair(true, trackPiece);
            else
                return std::make_pair(false, TrackElemType::flat);
        }

        auto asTrackType = selectedTrack.trackType;
        switch (asTrackType)
        {
            case TrackElemType::endStation:
            case TrackElemType::sBendLeft:
            case TrackElemType::sBendRight:
                if (startSlope != TrackPitch::none || endSlope != TrackPitch::none)
                {
                    return std::make_pair(false, TrackElemType::flat);
                }

                if (startBank != TrackRoll::none || endBank != TrackRoll::none)
                {
                    return std::make_pair(false, TrackElemType::flat);
                }

                return std::make_pair(true, asTrackType);

            case TrackElemType::leftVerticalLoop:
            case TrackElemType::rightVerticalLoop:
                if (startBank != TrackRoll::none || endBank != TrackRoll::none)
                {
                    return std::make_pair(false, TrackElemType::flat);
                }

                if (_rideConstructionState == RideConstructionState::Back)
                {
                    if (endSlope != TrackPitch::down25)
                    {
                        return std::make_pair(false, TrackElemType::flat);
                    }
                }
                else
                {
                    if (startSlope != TrackPitch::up25)
                    {
                        return std::make_pair(false, TrackElemType::flat);
                    }
                }

                return std::make_pair(true, asTrackType);

            default:
                return std::make_pair(true, asTrackType);
        }
    }

    /**
     * rct2: 0x006CA2DF
     *
     * @param[out] _trackType (dh)
     * @param[out] _trackDirection (bh)
     * @param[out] _rideIndex (dl)
     * @param[out] _liftHillAndInvertedState (liftHillAndInvertedState)
     * @param[out] _x (ax)
     * @param[out] _y (cx)
     * @param[out] _z (di)
     * @param[out] _properties (edirs16)
     * @return (CF)
     */
    static bool WindowRideConstructionUpdateState(
        TrackElemType* _trackType, int32_t* _trackDirection, RideId* _rideIndex,
        SelectedLiftAndInverted* _liftHillAndInvertedState, CoordsXYZ* _trackPos, int32_t* _properties)
    {
        RideId rideIndex;
        uint8_t trackDirection;
        uint16_t x, y, properties;
        SelectedLiftAndInverted liftHillAndInvertedState{};

        auto intent = Intent(INTENT_ACTION_RIDE_CONSTRUCTION_UPDATE_PIECES);
        ContextBroadcastIntent(&intent);
        auto updated_element = WindowRideConstructionUpdateStateGetTrackElement();
        if (!std::get<0>(updated_element))
        {
            return true;
        }

        TrackElemType trackType = std::get<1>(updated_element);
        rideIndex = _currentRideIndex;
        if (_currentTrackHasLiftHill)
        {
            liftHillAndInvertedState.set(LiftHillAndInverted::liftHill);
        }

        if (_currentTrackAlternative.has(AlternativeTrackFlag::inverted))
        {
            liftHillAndInvertedState.set(LiftHillAndInverted::inverted);
        }

        auto ride = GetRide(rideIndex);
        if (ride == nullptr)
            return true;

        if (IsTrackEnabled(TrackGroup::slopeSteepLong))
        {
            switch (trackType)
            {
                case TrackElemType::flatToUp60:
                    trackType = TrackElemType::flatToUp60LongBase;
                    break;

                case TrackElemType::up60ToFlat:
                    trackType = TrackElemType::up60ToFlatLongBase;
                    break;

                case TrackElemType::flatToDown60:
                    trackType = TrackElemType::flatToDown60LongBase;
                    break;

                case TrackElemType::down60ToFlat:
                    trackType = TrackElemType::down60ToFlatLongBase;
                    break;

                default:
                    break;
            }
        }

        if (IsTrackEnabled(TrackGroup::diagSlopeSteepLong))
        {
            switch (trackType)
            {
                case TrackElemType::diagFlatToUp60:
                    trackType = TrackElemType::diagFlatToUp60LongBase;
                    break;

                case TrackElemType::diagUp60ToFlat:
                    trackType = TrackElemType::diagUp60ToFlatLongBase;
                    break;

                case TrackElemType::diagFlatToDown60:
                    trackType = TrackElemType::diagFlatToDown60LongBase;
                    break;

                case TrackElemType::diagDown60ToFlat:
                    trackType = TrackElemType::diagDown60ToFlatLongBase;
                    break;

                default:
                    break;
            }
        }

        const auto& rtd = ride->getRideTypeDescriptor();
        const auto trackDrawerDescriptor = getCurrentTrackDrawerDescriptor(rtd);
        if (trackDrawerDescriptor.HasCoveredPieces() && _currentTrackAlternative.has(AlternativeTrackFlag::alternativePieces))
        {
            auto availableGroups = trackDrawerDescriptor.Covered.enabledTrackGroups;
            const auto& ted = GetTrackElementDescriptor(trackType);

            auto coveredVariant = ted.alternativeType;
            // this method limits the track element types that can be used
            if (coveredVariant != TrackElemType::none && (availableGroups.get(EnumValue(ted.definition.group))))
            {
                trackType = coveredVariant;
                if (!getGameState().cheats.enableChainLiftOnAllTrack)
                    liftHillAndInvertedState.unset(LiftHillAndInverted::liftHill);
            }
        }

        const auto& ted = GetTrackElementDescriptor(trackType);
        const TrackCoordinates& trackCoordinates = ted.coordinates;

        x = _currentTrackBegin.x;
        y = _currentTrackBegin.y;
        auto z = _currentTrackBegin.z;
        if (_rideConstructionState == RideConstructionState::Back)
        {
            z -= trackCoordinates.zEnd;
            trackDirection = _currentTrackPieceDirection ^ 0x02;
            trackDirection -= trackCoordinates.rotationEnd;
            trackDirection += trackCoordinates.rotationBegin;
            trackDirection &= 0x03;

            if (trackCoordinates.rotationBegin & (1 << 2))
            {
                trackDirection |= 0x04;
            }

            CoordsXY offsets = { trackCoordinates.x, trackCoordinates.y };
            CoordsXY coords = { x, y };
            coords += offsets.Rotate(DirectionReverse(trackDirection));
            x = static_cast<uint16_t>(coords.x);
            y = static_cast<uint16_t>(coords.y);
        }
        else
        {
            z -= trackCoordinates.zBegin;
            trackDirection = _currentTrackPieceDirection;
        }

        bool turnOffLiftHill = false;
        if (!IsTrackEnabled(TrackGroup::liftHillCurve))
        {
            if (ted.flags.has(TrackElementFlag::curveAllowsLift))
            {
                turnOffLiftHill = true;
            }
        }

        if (!ted.flags.has(TrackElementFlag::allowLiftHill))
        {
            turnOffLiftHill = true;
        }

        if (turnOffLiftHill && !getGameState().cheats.enableChainLiftOnAllTrack)
        {
            liftHillAndInvertedState.unset(LiftHillAndInverted::liftHill);
            _currentTrackHasLiftHill = false;

            if (trackType == TrackElemType::leftCurvedLiftHill || trackType == TrackElemType::rightCurvedLiftHill)
            {
                liftHillAndInvertedState.set(LiftHillAndInverted::liftHill);
            }
        }

        if (trackTypeHasSpeedSetting(trackType))
        {
            properties = _currentBrakeSpeed;
        }
        else
        {
            properties = _currentSeatRotationAngle << 12;
        }

        properties |= static_cast<uint8_t>(_currentColourScheme) << 8;

        if (_trackType != nullptr)
            *_trackType = trackType;
        if (_trackDirection != nullptr)
            *_trackDirection = trackDirection;
        if (_rideIndex != nullptr)
            *_rideIndex = rideIndex;
        if (_liftHillAndInvertedState != nullptr)
            *_liftHillAndInvertedState = liftHillAndInvertedState;
        if (_trackPos != nullptr)
            *_trackPos = { x, y, z };
        if (_properties != nullptr)
            *_properties = properties;

        return false;
    }

    void RideRestoreProvisionalTrackPiece()
    {
        if (_currentTrackSelectionFlags.has(TrackSelectionFlag::track))
        {
            RideId rideIndex;
            int32_t direction;
            TrackElemType type;
            SelectedLiftAndInverted liftHillAndAlternativeState{};
            CoordsXYZ trackPos;
            if (WindowRideConstructionUpdateState(
                    &type, &direction, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr))
            {
                RideConstructionRemoveGhosts();
            }
            else
            {
                _currentTrackPrice = PlaceProvisionalTrackPiece(
                    rideIndex, type, direction, liftHillAndAlternativeState, trackPos);
                WindowRideConstructionUpdateActiveElements();
            }
        }
    }

    void RideRemoveProvisionalTrackPiece()
    {
        auto rideIndex = _currentRideIndex;
        auto ride = GetRide(rideIndex);
        if (ride == nullptr || !_currentTrackSelectionFlags.has(TrackSelectionFlag::track))
        {
            return;
        }

        int32_t x = _unkF440C5.x;
        int32_t y = _unkF440C5.y;
        int32_t z = _unkF440C5.z;

        const auto& rtd = ride->getRideTypeDescriptor();
        if (rtd.specialType == RtdSpecialType::maze)
        {
            const CommandFlags flags = { CommandFlag::allowDuringPaused, CommandFlag::noSpend, CommandFlag::ghost };
            const CoordsXYZD quadrants[kNumOrthogonalDirections] = {
                { x, y, z, 0 },
                { x, y + 16, z, 1 },
                { x + 16, y + 16, z, 2 },
                { x + 16, y, z, 3 },
            };
            for (const auto& quadrant : quadrants)
            {
                auto gameAction = GameActions::MazeSetTrackAction(quadrant, false, rideIndex, MazeBuildMode::fill);
                gameAction.SetFlags(flags);
                auto res = GameActions::Execute(&gameAction, getGameState());
            }
        }
        else
        {
            int32_t direction = _unkF440C5.direction;
            if (!(direction & 4))
            {
                x -= CoordsDirectionDelta[direction].x;
                y -= CoordsDirectionDelta[direction].y;
            }
            CoordsXYE next_track;
            if (trackBlockGetNextFromZero({ x, y, z }, *ride, direction, &next_track, &z, &direction, true))
            {
                auto trackType = next_track.element->AsTrack()->GetTrackType();
                int32_t trackSequence = next_track.element->AsTrack()->GetSequenceIndex();
                auto trackRemoveAction = GameActions::TrackRemoveAction{
                    trackType, trackSequence, { next_track.x, next_track.y, z, static_cast<Direction>(direction) }
                };
                trackRemoveAction.SetFlags({ CommandFlag::allowDuringPaused, CommandFlag::noSpend, CommandFlag::ghost });
                GameActions::Execute(&trackRemoveAction, getGameState());
            }
        }
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan (2026-05-25) Phase 1:
    // window-side hooks for the gamepad-driven RideConstructionContextImpl.
    // Map RideConstructionState → RideInputMode discriminator (Maze states
    // collapse to `none` — out of v1 scope per plan §11).
    RideInputMode WindowRideConstructionGetInputMode()
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr || windowMgr->FindByClass(WindowClass::rideConstruction) == nullptr)
            return RideInputMode::none;
        switch (_rideConstructionState)
        {
            case RideConstructionState::Place:
                return RideInputMode::initialPlace;
            case RideConstructionState::Front:
                return RideInputMode::buildForward;
            case RideConstructionState::Back:
                return RideInputMode::buildBackward;
            case RideConstructionState::Selected:
                return RideInputMode::selected;
            case RideConstructionState::EntranceExit:
                return RideInputMode::entranceExit;
            case RideConstructionState::State0:
            case RideConstructionState::MazeBuild:
            case RideConstructionState::MazeMove:
            case RideConstructionState::MazeFill:
            default:
                return RideInputMode::none;
        }
    }

    // Head-tile accessor — returns _currentTrackBegin only in build states
    // (Front/Back) where the cursor chases the next-piece head. Place and
    // EntranceExit return nullopt so the cursor moves freely under D-pad;
    // the placement-position write goes the OTHER direction (cursor →
    // _currentTrackBegin) and lives in WindowRideConstructionSetInitialPlaceAt.
    std::optional<TileCoordsXY> WindowRideConstructionGetHeadTile()
    {
        if (!WindowRideConstructionIsInBuildState())
            return std::nullopt;
        return TileCoordsXY{ _currentTrackBegin };
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §6.3 (Phase 2,
    // 2026-05-25): write the cursor's tile + Z into _currentTrackBegin
    // before the user commits the first piece in Place state. The
    // Construct() path that BuildCurrent dispatches reads _currentTrackBegin
    // for the action's origin, so seeding it from the grid cursor here is
    // what makes "PAD A places at where I'm pointing" work.
    void WindowRideConstructionSetInitialPlaceAt(TileCoordsXY tile, int32_t z)
    {
        if (_rideConstructionState != RideConstructionState::Place)
            return;
        _currentTrackBegin = CoordsXYZ{ tile.ToCoordsXY(), z };
        // Refresh the active elements so the provisional ghost piece
        // re-renders at the new origin (PlaceProvisionalTrackPiece reads
        // _currentTrackBegin).
        WindowRideConstructionUpdateActiveElements();
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan (post-Phase 2 user
    // feedback, 2026-05-25): mirror of RideConstructionToolupdateConstruct
    // (the mouse per-frame tool update at line 3355) for the gamepad path.
    // The mouse path sets MapSelectFlag::enableConstruct + enableArrow,
    // populates the multi-tile selectedTiles via window->selectMapTiles,
    // and runs the Z trial-and-error PlaceProvisionalTrackPiece loop so the
    // ghost piece + directional arrow render at the hover position. Without
    // this the gamepad cursor shows the regular tile highlight with no piece
    // preview and no direction indicator. Called from onStep in initialPlace
    // state so the preview follows the cursor in real time.
    void WindowRideConstructionShowGhostAtTile(TileCoordsXY tile)
    {
        if (_rideConstructionState != RideConstructionState::Place)
            return;
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return;
        auto* w = static_cast<RideConstructionWindow*>(
            windowMgr->FindByClass(WindowClass::rideConstruction));
        if (w == nullptr)
            return;
        auto ride = GetRide(_currentRideIndex);
        if (ride == nullptr)
            return;

        const CoordsXY mapPos = tile.ToCoordsXY();
        int32_t z = _trackPlaceZ;
        if (z == 0)
            z = MapGetHighestZ(mapPos);

        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);
        gMapSelectFlags.set(MapSelectFlag::enableConstruct);
        gMapSelectFlags.set(MapSelectFlag::enableArrow);
        gMapSelectFlags.unset(MapSelectFlag::green);
        gMapSelectArrowPosition = CoordsXYZ{ mapPos, z };
        gMapSelectArrowDirection = _currentTrackPieceDirection;
        MapSelection::clearSelectedTiles();
        MapSelection::addSelectedTile(mapPos);

        // Resolve the staged track type from the curve/slope/bank state.
        RideId rideIndex;
        TrackElemType trackType;
        int32_t trackDirection;
        SelectedLiftAndInverted liftHillAndAlternativeState{};
        if (WindowRideConstructionUpdateState(
                &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, nullptr, nullptr))
        {
            RideConstructionInvalidateCurrentTrack();
            return;
        }
        _currentTrackPieceType = trackType;

        // Populate selected tiles for multi-tile pieces (long curves etc.).
        // Maze rides skip the multi-tile preview — out of v1 scope anyway.
        const auto& rtd = ride->getRideTypeDescriptor();
        if (rtd.specialType != RtdSpecialType::maze)
            w->selectMapTiles(trackType, trackDirection, mapPos);

        gMapSelectArrowPosition.z = z;

        // Raise Z above slopes/water using the selected-tile footprint.
        if (_trackPlaceZ == 0 && gMapSelectFlags.has(MapSelectFlag::enableConstruct))
        {
            for (const auto& selectedTile : MapSelection::getSelectedTiles())
            {
                if (MapIsLocationValid(selectedTile))
                {
                    const int32_t tileZ = MapGetHighestZ(selectedTile);
                    if (tileZ > z)
                        z = tileZ;
                }
            }
        }
        const auto& ted = GetTrackElementDescriptor(trackType);
        int32_t bx = 0;
        for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
            bx = std::min<int32_t>(bx, ted.sequenceData.sequences[i].clearance.z);
        z -= bx;
        gMapSelectArrowPosition.z = z;

        _currentTrackBegin.x = mapPos.x;
        _currentTrackBegin.y = mapPos.y;
        _currentTrackBegin.z = z;

        // Skip re-placing the ghost if it's already at this exact position
        // (mirrors the mouse path's _previousTrackPiece guard at :3446).
        if (_currentTrackSelectionFlags.has(TrackSelectionFlag::track)
            && _currentTrackBegin == _previousTrackPiece)
        {
            return;
        }
        _previousTrackPiece = _currentTrackBegin;

        // Z trial-and-error to find a Z where the ghost can render. Maze
        // path is excluded (handled separately upstream; out of v1 scope).
        if (rtd.specialType == RtdSpecialType::maze)
            return;

        const int numAttempts = (z <= kMaximumTrackHeight ? ((kMaximumTrackHeight - z) / kCoordsZStep + 1) : 2);
        for (int zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
        {
            CoordsXYZ trackPos{};
            WindowRideConstructionUpdateState(
                &trackType, &trackDirection, &rideIndex, &liftHillAndAlternativeState, &trackPos, nullptr);
            _currentTrackPrice = PlaceProvisionalTrackPiece(
                rideIndex, trackType, trackDirection, liftHillAndAlternativeState, trackPos);
            if (_currentTrackPrice != kMoney64Undefined)
                break;
            _currentTrackBegin.z -= 8;
            if (_currentTrackBegin.z < 0)
                break;
            _currentTrackBegin.z += 16;
        }
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §6.3 fix (post-Phase 2,
    // 2026-05-25): the original SetInitialPlaceAt + BuildCurrent path didn't
    // work because BuildCurrent bails when WIDX_CONSTRUCT is disabled, and
    // WIDX_CONSTRUCT IS disabled in Place state. The mouse path solves this
    // via RideConstructionTooldownConstruct (at line ~3609) which flips the
    // state Place → Front, sets _currentTrackBegin, THEN clicks WIDX_CONSTRUCT
    // (now enabled), with a Z trial-and-error loop on failure. This helper
    // mirrors that flow for the gamepad path. Maze handling deferred per
    // plan non-goals §11.
    void WindowRideConstructionPlaceInitialAtTile(TileCoordsXY tile)
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return;
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr || _rideConstructionState != RideConstructionState::Place)
            return;
        auto ride = GetRide(_currentRideIndex);
        if (ride == nullptr)
            return;
        // Maze is a different beast — out of v1 scope per plan §11.
        if (ride->getRideTypeDescriptor().specialType == RtdSpecialType::maze)
            return;

        // Compute the track type from current shape selection (writes
        // _currentTrackPieceType as a side effect). Bail if invalid.
        TrackElemType trackType;
        if (WindowRideConstructionUpdateState(&trackType, nullptr, nullptr, nullptr, nullptr, nullptr))
            return;
        _currentTrackPieceType = trackType;

        RideConstructionInvalidateCurrentTrack();
        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);

        const CoordsXY mapPos = tile.ToCoordsXY();
        int32_t z = _trackPlaceZ;
        if (z == 0)
            z = MapGetHighestZ(mapPos);

        ToolCancel();

        if (_trackPlaceZ == 0)
        {
            const auto& ted = GetTrackElementDescriptor(_currentTrackPieceType);
            int32_t bx = 0;
            for (uint8_t i = 0; i < ted.sequenceData.numSequences; i++)
                bx = std::min<int32_t>(bx, ted.sequenceData.sequences[i].clearance.z);
            z -= bx;
            if (!getGameState().cheats.disableClearanceChecks && z > kMinimumLandZ)
                z -= kLandHeightStep;
        }
        else
        {
            z = _trackPlaceZ;
        }

        // Z trial-and-error placement loop (mirror of RideConstructionTooldown-
        // Construct at line :3757). Each iteration flips state to Front, writes
        // _currentTrackBegin, and clicks WIDX_CONSTRUCT (now enabled in Front);
        // on failure, retries at Z+8.
        const int numAttempts = (z <= kMaximumTrackHeight ? ((kMaximumTrackHeight - z) / kCoordsZStep + 1) : 2);
        for (int32_t zAttempts = 0; zAttempts < numAttempts; ++zAttempts)
        {
            _rideConstructionState = RideConstructionState::Front;
            _currentTrackBegin.x = mapPos.x;
            _currentTrackBegin.y = mapPos.y;
            _currentTrackBegin.z = z;
            _currentTrackSelectionFlags.clearAll();
            WindowRideConstructionUpdateActiveElements();

            w = windowMgr->FindByClass(WindowClass::rideConstruction);
            if (w == nullptr)
                break;

            gDisableErrorWindowSound = true;
            w->onMouseDown(WIDX_CONSTRUCT);
            gDisableErrorWindowSound = false;

            if (_trackPlaceCost == kMoney64Undefined)
            {
                StringId errorText = _trackPlaceErrorMessage;
                z -= 8;
                if (errorText == STR_NOT_ENOUGH_CASH_REQUIRES || errorText == STR_CAN_ONLY_BUILD_THIS_UNDERWATER
                    || errorText == STR_CAN_ONLY_BUILD_THIS_ON_WATER || errorText == STR_CAN_ONLY_BUILD_THIS_ABOVE_GROUND
                    || errorText == STR_TOO_HIGH_FOR_SUPPORTS || errorText == STR_TOO_HIGH
                    || errorText == STR_LOCAL_AUTHORITY_WONT_ALLOW_CONSTRUCTION_ABOVE_TREE_HEIGHT
                    || zAttempts == (numAttempts - 1) || z < 0)
                {
                    // Hard failure: restore Place state + save/restore shape
                    // settings so the user can try again. Mirrors mouse path's
                    // save-restore + RideInitialiseConstructionWindow at :3785.
                    const auto saveTrackDirection = _currentTrackPieceDirection;
                    const auto saveCurrentTrackCurve = _currentlySelectedTrack;
                    const auto savePreviousTrackPitchEnd = _previousTrackPitchEnd;
                    const auto saveCurrentTrackPitchEnd = _currentTrackPitchEnd;
                    const auto savePreviousTrackRollEnd = _previousTrackRollEnd;
                    const auto saveCurrentTrackRollEnd = _currentTrackRollEnd;
                    const auto savedCurrentTrackAlternative = _currentTrackAlternative;
                    const auto savedCurrentTrackLiftHill = _currentTrackHasLiftHill;

                    RideInitialiseConstructionWindow(*ride);

                    _currentTrackPieceDirection = saveTrackDirection;
                    _currentlySelectedTrack = saveCurrentTrackCurve;
                    _previousTrackPitchEnd = savePreviousTrackPitchEnd;
                    _currentTrackPitchEnd = saveCurrentTrackPitchEnd;
                    _previousTrackRollEnd = savePreviousTrackRollEnd;
                    _currentTrackRollEnd = saveCurrentTrackRollEnd;
                    _currentTrackAlternative = savedCurrentTrackAlternative;
                    _currentTrackHasLiftHill = savedCurrentTrackLiftHill;

                    // Skip error audio (mouse path uses state->position.x from
                    // ContextGetCursorState which is screen-pointer relative;
                    // for gamepad we don't have a meaningful screen pos).
                    break;
                }
                z += 16;
            }
            else
            {
                // Success. Now the tricky part: the upstream forward callback
                // (RideConstructPlacedForwardGameActionCallback at :2957) has
                // already fired synchronously and progressed the state machine.
                // Possible post-callback states:
                //   - Selected: next-piece lookup found a track block (the
                //     placed piece). RideSelectNextSection inside the callback
                //     then either advances within Selected or flips to Front
                //     at end-of-track.
                //   - Front: end-of-track reached cleanly. Best case.
                //   - State0: lookup failed (common for single station-style
                //     first pieces). Then CloseConstructWindowOnCompletion
                //     (:2914) sees State0 + entrances-not-built → auto-clicks
                //     WIDX_ENTRANCE → state flips to EntranceExit and the
                //     WIDX_ENTRANCE tool gets armed. THIS is the auto-Entrance
                //     trap the user reported.
                //
                // For the gamepad path we want to land in Front so the user
                // can immediately extend via D-pad shape + PAD A. Undo any
                // auto-EntranceExit flip, then force state to Front + re-arm
                // WIDX_CONSTRUCT.
                if (_rideConstructionState == RideConstructionState::EntranceExit)
                {
                    // Undo the auto-flip from CloseConstructWindowOnCompletion.
                    // The previous state stored in gRideEntranceExitPlace-
                    // PreviousRideConstructionState was State0.
                    _rideConstructionState = gRideEntranceExitPlacePreviousRideConstructionState;
                    gRideEntranceExitPlaceDirection = kInvalidDirection;
                }
                if (_rideConstructionState == RideConstructionState::Selected)
                {
                    // Advance through the placed piece(s) until we hit Front
                    // (end-of-track). RideSelectNextSection in Selected state
                    // will either stay in Selected (more track ahead) or flip
                    // to Front (end-of-track). Bound the loop defensively.
                    for (int i = 0; i < 64 && _rideConstructionState == RideConstructionState::Selected; i++)
                        RideSelectNextSection();
                }
                if (_rideConstructionState != RideConstructionState::Front
                    && _rideConstructionState != RideConstructionState::Back)
                {
                    // State0 (or anything else): manually flip to Front and
                    // let RideConstructionSetDefaultNextPiece set up curve/
                    // slope/bank defaults. _currentTrackBegin / direction
                    // stay at the placed piece's position — best the gamepad
                    // path can do without re-deriving the piece's exit
                    // geometry (the user gets an "overlap" error if they try
                    // to extend without moving the cursor, which is honest).
                    _rideConstructionState = RideConstructionState::Front;
                    RideConstructionSetDefaultNextPiece();
                    WindowRideConstructionUpdateActiveElements();
                }
                // Re-arm WIDX_CONSTRUCT so the user can extend in Front state
                // via the gamepad onStep verbs. The auto-Entrance trigger
                // would otherwise have armed WIDX_ENTRANCE; explicitly cancel
                // the stale tool first so the new ToolSet activates cleanly
                // (otherwise ToolSet may detect the existing arm and no-op).
                ToolCancel();
                w = windowMgr->FindByClass(WindowClass::rideConstruction);
                if (w != nullptr)
                    ToolSet(*w, WIDX_CONSTRUCT, Tool::crosshair);
                gInputFlags.set(InputFlag::toolActive);
                // Defensive: re-assert grid cursor mode invariants post-placement.
                // The auto-Entrance trigger + ToolCancel round-trip can leave
                // these in inconsistent states; setting them explicitly here
                // means the user lands cleanly in toolRideConstruction context
                // on the next frame regardless of what intermediate paths ran.
                //
                // toolFocusSelected + selectorMode::active: required gates for
                // resolveActiveContext to route to the tool context.
                //
                // setFocus: aligns _focusedWindowClass with rideConstruction so
                // the §16 in-mode engage gate (focusedOnRide check) still
                // matches if the user later exits and re-engages grid mode.
                {
                    auto& inputMgr = OpenRCT2::Ui::GetInputManager();
                    inputMgr.setToolFocusSelected(
                        true,
                        OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                    inputMgr.setSelectorMode(
                        OpenRCT2::Ui::InputManager::SelectorMode::active);
                }
                if (w != nullptr)
                {
                    auto& inputMgr = OpenRCT2::Ui::GetInputManager();
                    inputMgr.setFocus(WindowClass::rideConstruction, WIDX_CONSTRUCT);
                }
                break;
            }
        }
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §16 (post-Phase 2,
    // 2026-05-26): adjust the placement Z (_trackPlaceZ) for the gamepad
    // path. The mouse path drives _trackPlaceZ via shift+drag-Y at line
    // 3120 (mapZ += _trackPlaceShiftZ); the gamepad has no continuous Y
    // axis so we step by a fixed amount on each shift+D-pad-up/down press.
    //
    // Step size = kLandHeightStep (16), NOT kCoordsZStep (8). Most rides
    // can only be placed at multiples of 16 — placing at an 8-offset Z
    // gets corrected by ShowGhostAtTile's trial-and-error loop (lines
    // 5360-5374), so an 8-step bump from a 16-multiple becomes a no-op
    // visually (loop rounds back to the same 16-multiple). User-facing
    // effect: every other tap does nothing. 16-step taps always produce
    // a visible Z change.
    //
    // Seed: floor2(MapGetHighestZ, 16) matches the mouse path's alignment
    // at line 3152 (mapZ = floor2(surfaceElement->GetBaseZ(), 16)).
    //
    // _trackPlaceZ == 0 is a sentinel that ShowGhostAtTile treats as
    // "follow ground" (line 5286-5288). We clamp to [kLandHeightStep,
    // kMaximumTrackHeight] so the sentinel is never re-entered.
    //
    // ShowGhostAtTile re-runs PlaceProvisionalTrackPiece with the new Z
    // (and its own trial-and-error loop) so the ghost preview moves to
    // match the user's adjustment.
    //
    // Future polish (out of v1): some ride types (water coasters,
    // submarines with half-tile pieces) accept 8-step Z. A query like
    // "does this ride support half-tile track Z?" could refine this —
    // but for the common case 16 is the right default.
    void WindowRideConstructionAdjustPlaceZ(TileCoordsXY tile, int32_t delta)
    {
        if (_rideConstructionState != RideConstructionState::Place)
            return;
        const CoordsXY mapPos = tile.ToCoordsXY();
        if (!MapIsLocationValid(mapPos))
            return;
        if (_trackPlaceZ == 0)
        {
            // Seed from ground level (16-aligned) so the first nudge
            // produces a visible step. Without seeding, +16 still leaves
            // _trackPlaceZ at a small value that ShowGhostAtTile re-falls-
            // back near ground anyway. floor2(..., 16) matches the mouse
            // path's alignment so ride-valid placement Zs line up.
            _trackPlaceZ = floor2(MapGetHighestZ(mapPos), kLandHeightStep);
        }
        _trackPlaceZ = std::min<int32_t>(
            std::max<int32_t>(_trackPlaceZ + delta, kLandHeightStep),
            kMaximumTrackHeight);
        // Refresh the ghost at the new Z so the user sees the piece move.
        // ShowGhostAtTile reads _trackPlaceZ at line 5286 and writes
        // _currentTrackBegin.z accordingly.
        WindowRideConstructionShowGhostAtTile(tile);
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §6.2 (Phase 2,
    // 2026-05-25): rotate the initial-piece direction in Place state via
    // PAD Y. Cycles 0 → 1 → 2 → 3. No-op outside Place state — in
    // Front/Back the direction is constrained by the previous piece's
    // exit, and TurnRight is the rotation verb (onRotate maps there).
    void WindowRideConstructionCycleInitialDirection()
    {
        if (_rideConstructionState != RideConstructionState::Place)
            return;
        _currentTrackPieceDirection = (_currentTrackPieceDirection + 1) & 3;
        WindowRideConstructionUpdateActiveElements();
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §6.2 follow-up
    // (post-Phase 2 user request, 2026-05-25): the precision-modifier +
    // cardinal D-pad gesture sets the initial-piece direction directly
    // (mirrors the Land tool's corner picker). PAD Y cycling stays as a
    // secondary path. dir is a world direction 0-3 (N/E/S/W per
    // TileDirectionDelta) computed in InputManager by the context impl
    // from the dpad + camera rotation.
    void WindowRideConstructionSetInitialDirection(uint8_t dir)
    {
        if (_rideConstructionState != RideConstructionState::Place)
            return;
        _currentTrackPieceDirection = dir & 3;
        WindowRideConstructionUpdateActiveElements();
        // Refresh the ghost so the arrow / piece preview re-renders with
        // the new direction at the current cursor tile. The tile is read
        // from _currentTrackBegin (last set by the per-step ShowGhostAtTile
        // call); if we haven't stepped yet, this is a no-op gracefully.
        if (_currentTrackBegin.x != 0 || _currentTrackBegin.y != 0)
            WindowRideConstructionShowGhostAtTile(TileCoordsXY{ _currentTrackBegin });
    }

    // OPENRCT2MINI ride-construction-grid-cursor-plan §7 (Phase 3, 2026-05-25,
    // revised 2026-05-28): entrance/exit placement for the gamepad path.
    // The mouse path derives tile + direction from one screen position via
    // RideGetEntranceOrExitPositionFromScreenPosition (Construction.cpp:370)
    // which searches around the screen-mapped tile for an adjacent station
    // track. The original gamepad path split tile from direction (cursor +
    // PAD Y cycle) — which let the user place anywhere with any direction,
    // bypassing the adjacency constraint. Fixed here by mirroring the
    // search: per-cursor-step, look for an adjacent station tile and set
    // gRideEntranceExitPlaceDirection to face away from the track. If no
    // station-edge is adjacent to the cursor, direction is kInvalidDirection
    // and PAD A refuses to place. Manual direction cycling is gone — the
    // cursor's tile position uniquely picks the valid edge.
    //
    // Internal predicate used by the auto-position search. Reports whether
    // `tile` is a valid entrance/exit placement tile for the currently-
    // staged station + ride + Z, without mutating any global state. The
    // public function below does the same check but additionally writes
    // gRideEntranceExitPlaceDirection / Station / etc. so the place action
    // can use them.
    static bool TileIsValidEntranceExitPlacement(TileCoordsXY tile)
    {
        auto ride = GetRide(gRideEntranceExitPlaceRideIndex);
        if (ride == nullptr)
            return false;
        auto stationStart = ride->getStation(gRideEntranceExitPlaceStationIndex).Start;
        if (stationStart.IsNull())
            return false;
        auto stationBaseZ = ride->getStation(gRideEntranceExitPlaceStationIndex).GetBaseZ();
        CoordsXYZD entranceExitCoords{ tile.ToCoordsXY(), stationBaseZ, kInvalidDirection };
        for (uint8_t startDirection = 0; startDirection < 4; startDirection++)
        {
            entranceExitCoords.direction = startDirection;
            auto nextLocation = entranceExitCoords;
            nextLocation += CoordsDirectionDelta[entranceExitCoords.direction];
            if (!MapIsLocationValid(nextLocation))
                continue;
            auto* tileElement = MapGetFirstElementAt(nextLocation);
            if (tileElement == nullptr)
                continue;
            do
            {
                if (tileElement->GetType() != TileElementType::Track)
                    continue;
                if (tileElement->GetBaseZ() != stationBaseZ)
                    continue;
                auto* trackElement = tileElement->AsTrack();
                if (trackElement->GetRideIndex() != gRideEntranceExitPlaceRideIndex)
                    continue;
                if (trackElement->GetTrackType() == TrackElemType::maze)
                    return true;
                if (trackElement->GetStationIndex() != gRideEntranceExitPlaceStationIndex)
                    continue;
                Direction relativeDirection
                    = (DirectionReverse(entranceExitCoords.direction) - tileElement->GetDirection()) & 3;
                const auto& ted = GetTrackElementDescriptor(trackElement->GetTrackType());
                auto connectionSides
                    = ted.sequenceData.sequences[trackElement->GetSequenceIndex()].getEntranceConnectionSides();
                if (connectionSides & (1 << relativeDirection))
                    return true;
            } while (!(tileElement++)->IsLastForTile());
        }
        return false;
    }

    // Searches all 4 cardinals from the cursor (no sub-tile quadrant
    // start direction since gamepad cursor is tile-snapped). Logic
    // otherwise identical to the mouse path's adjacency loop at lines
    // 442-491 of Construction.cpp.
    //
    // Enhancement 2 (2026-05-28): when the cursor is on a valid placement
    // tile, also fire the entrance/exit ghost preview (mirror of
    // RideConstructionToolupdateEntranceExit at :3590) so the user sees
    // where the entrance/exit will land. Cleared when invalid via
    // RideConstructionInvalidateCurrentTrack.
    bool WindowRideConstructionUpdateEntranceExitDirection(TileCoordsXY tile)
    {
        if (_rideConstructionState != RideConstructionState::EntranceExit)
            return false;
        auto ride = GetRide(gRideEntranceExitPlaceRideIndex);
        if (ride == nullptr)
        {
            gRideEntranceExitPlaceDirection = kInvalidDirection;
            return false;
        }
        auto stationStart = ride->getStation(gRideEntranceExitPlaceStationIndex).Start;
        if (stationStart.IsNull())
        {
            gRideEntranceExitPlaceDirection = kInvalidDirection;
            return false;
        }
        auto stationBaseZ = ride->getStation(gRideEntranceExitPlaceStationIndex).GetBaseZ();
        CoordsXYZD entranceExitCoords{ tile.ToCoordsXY(), stationBaseZ, kInvalidDirection };

        // Clear MapSelect flags / cancel existing ghost up-front; on hit we
        // re-set them. On miss this leaves the ghost cleared.
        gMapSelectFlags.unset(MapSelectFlag::enable);
        gMapSelectFlags.unset(MapSelectFlag::enableConstruct);
        gMapSelectFlags.unset(MapSelectFlag::enableArrow);

        for (uint8_t startDirection = 0; startDirection < 4; startDirection++)
        {
            entranceExitCoords.direction = startDirection;
            auto nextLocation = entranceExitCoords;
            nextLocation += CoordsDirectionDelta[entranceExitCoords.direction];
            if (!MapIsLocationValid(nextLocation))
                continue;
            auto* tileElement = MapGetFirstElementAt(nextLocation);
            if (tileElement == nullptr)
                continue;
            do
            {
                if (tileElement->GetType() != TileElementType::Track)
                    continue;
                if (tileElement->GetBaseZ() != stationBaseZ)
                    continue;
                auto* trackElement = tileElement->AsTrack();
                if (trackElement->GetRideIndex() != gRideEntranceExitPlaceRideIndex)
                    continue;
                bool isMaze = (trackElement->GetTrackType() == TrackElemType::maze);
                if (isMaze)
                {
                    gRideEntranceExitPlaceStationIndex = StationIndex::FromUnderlying(0);
                }
                else
                {
                    Direction relativeDirection
                        = (DirectionReverse(entranceExitCoords.direction) - tileElement->GetDirection()) & 3;
                    const auto& ted = GetTrackElementDescriptor(trackElement->GetTrackType());
                    auto connectionSides
                        = ted.sequenceData.sequences[trackElement->GetSequenceIndex()].getEntranceConnectionSides();
                    if (!(connectionSides & (1 << relativeDirection)))
                        continue;
                    gRideEntranceExitPlaceStationIndex = trackElement->GetStationIndex();
                }
                gRideEntranceExitPlaceDirection = DirectionReverse(entranceExitCoords.direction);

                // Mirror RideConstructionToolupdateEntranceExit (:3590):
                // light up the tile and dispatch the ghost preview action.
                gMapSelectFlags.set(MapSelectFlag::enable);
                gMapSelectFlags.set(MapSelectFlag::enableArrow);
                gMapSelectType = MapSelectType::full;
                gMapSelectPositionA = entranceExitCoords;
                gMapSelectPositionB = entranceExitCoords;
                gMapSelectArrowPosition = entranceExitCoords;
                gMapSelectArrowDirection = gRideEntranceExitPlaceDirection;

                // Re-dispatch the ghost only if the position/station
                // changed (matches mouse path's guard at :3611-3612).
                CoordsXYZD ghostCoords = entranceExitCoords;
                ghostCoords.direction = DirectionReverse(gRideEntranceExitPlaceDirection);
                if (!_currentTrackSelectionFlags.has(TrackSelectionFlag::entranceOrExit)
                    || ghostCoords != gRideEntranceExitGhostPosition
                    || gRideEntranceExitPlaceStationIndex != gRideEntranceExitGhostStationIndex)
                {
                    _currentTrackPrice = RideEntranceExitPlaceGhost(
                        *ride, ghostCoords, ghostCoords.direction,
                        gRideEntranceExitPlaceType, gRideEntranceExitPlaceStationIndex);
                    WindowRideConstructionUpdateActiveElements();
                    VirtualFloorSetHeight(ghostCoords.z);
                }
                return true;
            } while (!(tileElement++)->IsLastForTile());
        }

        gRideEntranceExitPlaceDirection = kInvalidDirection;
        // No valid edge — clear any stale ghost from a prior frame.
        RideConstructionInvalidateCurrentTrack();
        return false;
    }

    // Enhancement 1 (2026-05-28): scan a bounding box around the station
    // for valid placement tiles, pick the one closest to the viewport
    // centre. Called when entering entranceExit mode so the cursor lands
    // somewhere immediately usable (don't make the user pan to find a
    // valid edge). Bounding box radius = kMaxStationPlatformLength (32) so
    // the search covers the entire maximum-length station + its
    // immediately-adjacent placement tiles.
    std::optional<TileCoordsXY> WindowRideConstructionFindBestEntranceExitTile()
    {
        if (_rideConstructionState != RideConstructionState::EntranceExit)
            return std::nullopt;
        auto ride = GetRide(gRideEntranceExitPlaceRideIndex);
        if (ride == nullptr)
            return std::nullopt;
        auto stationStart = ride->getStation(gRideEntranceExitPlaceStationIndex).Start;
        if (stationStart.IsNull())
            return std::nullopt;
        const TileCoordsXY centreTile{ stationStart };

        // Resolve a reference point — the viewport centre projected to
        // map XY (matches the user's intent "closest to view centre").
        // Fall back to the station's start if the viewport isn't ready.
        CoordsXY referencePos{ stationStart.x, stationStart.y };
        if (auto* mainWnd = WindowGetMain(); mainWnd != nullptr && mainWnd->viewport != nullptr)
        {
            const auto* vp = mainWnd->viewport;
            ScreenCoordsXY viewportCentreScreen{
                vp->pos.x + vp->width / 2,
                vp->pos.y + vp->height / 2,
            };
            // Project the viewport-centre screen pixel back to a map XY at
            // the station's base Z so the projection accounts for camera
            // rotation + zoom.
            auto stationBaseZ = ride->getStation(gRideEntranceExitPlaceStationIndex).GetBaseZ();
            if (auto projected = ScreenGetMapXYWithZ(viewportCentreScreen, stationBaseZ); projected.has_value())
            {
                referencePos = *projected;
            }
        }

        std::optional<TileCoordsXY> best;
        int64_t bestDistSq = std::numeric_limits<int64_t>::max();
        constexpr int kSearchRadius = kMaxStationPlatformLength;
        for (int dx = -kSearchRadius; dx <= kSearchRadius; dx++)
        {
            for (int dy = -kSearchRadius; dy <= kSearchRadius; dy++)
            {
                TileCoordsXY candidate{ centreTile.x + dx, centreTile.y + dy };
                if (!TileIsValidEntranceExitPlacement(candidate))
                    continue;
                const auto candidateCoords = candidate.ToCoordsXY();
                const int64_t ddx = candidateCoords.x - referencePos.x;
                const int64_t ddy = candidateCoords.y - referencePos.y;
                const int64_t distSq = ddx * ddx + ddy * ddy;
                if (distSq < bestDistSq)
                {
                    bestDistSq = distSq;
                    best = candidate;
                }
            }
        }
        return best;
    }

    void WindowRideConstructionPlaceEntranceExit(TileCoordsXY tile)
    {
        if (_rideConstructionState != RideConstructionState::EntranceExit)
            return;
        // Re-run the adjacency search at the current cursor (defence-in-
        // depth — onStep should have set this already, but in case the
        // user pressed PAD A on a non-adjacent tile we still bail rather
        // than placing into a void).
        if (!WindowRideConstructionUpdateEntranceExitDirection(tile))
        {
            // No adjacent station edge → invalid placement. Play the error
            // sound via a no-op game action call would be cleanest, but
            // for v1 just silently refuse. The direction preview in the
            // ride window will show "Invalid" as user feedback.
            return;
        }

        auto ride = GetRide(gRideEntranceExitPlaceRideIndex);
        if (ride == nullptr)
            return;
        // Use the station's base Z, not the cursor's Z — entrances must
        // be at the same height as the station they serve.
        const auto stationBaseZ = ride->getStation(gRideEntranceExitPlaceStationIndex).GetBaseZ();
        const CoordsXYZD entranceCoords{
            tile.ToCoordsXY(), stationBaseZ, static_cast<uint8_t>(0)
        };
        auto action = GameActions::RideEntranceExitPlaceAction(
            entranceCoords, DirectionReverse(gRideEntranceExitPlaceDirection),
            gRideEntranceExitPlaceRideIndex, gRideEntranceExitPlaceStationIndex,
            gRideEntranceExitPlaceType == ENTRANCE_TYPE_RIDE_EXIT);

        action.SetCallback([](const GameActions::GameAction*, const GameActions::Result* result) {
            if (result->error != GameActions::Status::ok)
                return;
            Audio::Play3D(Audio::SoundId::placeItem, result->position);
            auto* windowMgr = GetWindowManager();
            auto currentRide = GetRide(gRideEntranceExitPlaceRideIndex);
            if (currentRide != nullptr && RideAreAllPossibleEntrancesAndExitsBuilt(*currentRide).Successful)
            {
                ToolCancel();
                if (!currentRide->getRideTypeDescriptor().flags.has(RtdFlag::hasTrack))
                {
                    windowMgr->CloseByClass(WindowClass::rideConstruction);
                }
            }
            else
            {
                // Flip to placing the other (entrance→exit or exit→entrance)
                // — mirrors the mouse path's behaviour at :2692.
                gRideEntranceExitPlaceType = gRideEntranceExitPlaceType ^ 1;
                if (windowMgr != nullptr)
                    windowMgr->InvalidateByClass(WindowClass::rideConstruction);
            }
        });
        GameActions::Execute(&action, getGameState());
    }

    void WindowRideConstructionCancelEntranceExitMode()
    {
        if (_rideConstructionState != RideConstructionState::EntranceExit)
            return;
        // OPENRCT2MINI ride-construction-grid-cursor-plan §16 (2026-05-28):
        // clear the entrance/exit ghost BEFORE swapping state away from
        // EntranceExit. RideConstructionInvalidateCurrentTrack switches on
        // _rideConstructionState — the EntranceExit branch hits
        // RideConstructionRemoveGhosts which removes the ghost tile element
        // and clears _currentTrackSelectionFlags::entranceOrExit. Flipping
        // state first routes through the Front/Back branch which doesn't
        // know about the entrance ghost, leaving it stuck in the world
        // (user-reported: PAD B + PAD Start + close window = persistent
        // ghost entrance).
        RideConstructionInvalidateCurrentTrack();
        // Restore the construction state to whatever was active before the
        // user clicked Entrance/Exit. Mirrors the mouse path's behaviour at
        // RideConstruction.cpp:1069.
        _rideConstructionState = gRideEntranceExitPlacePreviousRideConstructionState;
        gRideEntranceExitPlaceDirection = kInvalidDirection;
        auto* windowMgr = GetWindowManager();
        if (windowMgr != nullptr)
            windowMgr->InvalidateByClass(WindowClass::rideConstruction);
    }

    bool WindowRideConstructionIsInBuildState()
    {
        return _rideConstructionState == RideConstructionState::Front
            || _rideConstructionState == RideConstructionState::Back;
    }

    // Re-arm the WIDX_CONSTRUCT tool when entering focus / cursor mode and the
    // window has live build state but the tool widget has dropped (e.g. after
    // a focus-mode roundtrip). Mirrors WindowFootpathReArmForCurrentMode.
    // Guarded against toggle-cancel: only re-issue ToolSet when not already
    // active for our window.
    void WindowRideConstructionReArmForCurrentMode()
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr)
            return;
        WindowBase* w = windowMgr->FindByClass(WindowClass::rideConstruction);
        if (w == nullptr)
            return;
        // OPENRCT2MINI 2026-05-25 follow-up #2: removed the GetInputMode()==none
        // guard. State0 (the idle "between pieces" state) returns none too,
        // but we still want to arm WIDX_CONSTRUCT so the user can engage grid
        // cursor mode and start D-padding to extend / place pieces. Maze
        // construction uses a different window class entirely (mazeConstruction)
        // so we don't hit it here.
        if (isToolActive(*w, WIDX_CONSTRUCT))
            return;
        ToolSet(*w, WIDX_CONSTRUCT, Tool::crosshair);
        gInputFlags.set(InputFlag::toolActive);
    }

    // Cycle to the next valid special-track piece for the current ride /
    // direction / pitch / roll context. Filtered via BuildSpecialElementsList
    // (the same helper the dropdown picker uses). Wraps to the first entry
    // past the end. No-op silently if the list is empty (rare — most ride
    // types have at least one special piece, even if just a station).
    //
    // OPENRCT2MINI §16.11 fix (2026-05-25): the list returned by
    // BuildSpecialElementsList ALSO contains separator entries
    // (TrackElemType::none = kSeparator at Construction.cpp:31) — visual
    // dividers between piece groups in the dropdown UI. Separators can
    // have Disabled=false in the data (their pitchStart/End default to
    // zero and match the build slope when it's TrackPitch::none), so the
    // original implementation that only skipped Disabled entries would
    // land on a separator and the user saw "nothing" rendered. Worse, the
    // separator's TrackType is TrackElemType::none which is also the
    // "no curve selected" sentinel — picking it scrambled
    // _currentlySelectedTrack and the next cycle restarted from index 0,
    // looping between the first two real specials and the separator.
    // Fix: skip both Disabled AND TrackElemType::none entries.
    void WindowRideConstructionKeyboardShortcutCycleSpecialNext()
    {
        auto* windowMgr = GetWindowManager();
        if (windowMgr == nullptr || windowMgr->FindByClass(WindowClass::rideConstruction) == nullptr)
            return;
        const auto ride = GetRide(_currentRideIndex);
        if (ride == nullptr)
            return;

        const auto list = BuildSpecialElementsList(
            *ride, _currentTrackPieceDirection, _currentTrackPitchEnd, _currentTrackRollEnd, _rideConstructionState);
        if (!list.HasActiveElements || list.Elements.empty())
            return;

        // Predicate: an entry is selectable iff it's not disabled AND it's
        // not a separator placeholder. Kept inline so the find-current and
        // step-next loops use the exact same filter.
        const auto isSelectable = [](const auto& entry) {
            return !entry.Disabled && entry.TrackType != TrackElemType::none;
        };

        // Find current pick (if it matches a selectable entry) and step to
        // the next selectable one with wrap. If current isn't in the list
        // (we're on a plain curve, not a special, OR we're on a stale entry
        // that got filtered out by a state change), start from the beginning.
        const auto& current = _currentlySelectedTrack;
        size_t startIdx = 0;
        if (current.isTrackType)
        {
            const auto currentType = current.trackType;
            for (size_t i = 0; i < list.Elements.size(); ++i)
            {
                if (list.Elements[i].TrackType == currentType && isSelectable(list.Elements[i]))
                {
                    startIdx = i + 1; // start search past current
                    break;
                }
            }
        }
        for (size_t step = 0; step < list.Elements.size(); ++step)
        {
            const size_t idx = (startIdx + step) % list.Elements.size();
            if (!isSelectable(list.Elements[idx]))
                continue;
            const TrackElemType picked = list.Elements[idx].TrackType;
            RideConstructionInvalidateCurrentTrack();
            // Mirror the dropdown picker's side-effects (RideConstruction.cpp
            // :1546-1577): set the track + clear cached price + refresh.
            _currentlySelectedTrack = picked;
            _currentTrackPrice = kMoney64Undefined;
            WindowRideConstructionUpdateActiveElements();
            return;
        }
    }
} // namespace OpenRCT2::Ui::Windows
