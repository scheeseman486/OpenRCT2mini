/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2-ui/interface/Window.h>
#include <openrct2/Identifiers.h>
#include <openrct2/interface/Window.h>
#include <openrct2/world/Location.hpp>
#include <openrct2/world/ScenerySelection.h>
#include <optional>
#include <string_view>

struct StringWithArgs;
struct Ride;
struct RideSelection;
struct TrackDesign;
struct Vehicle;

enum class GuestListFilterType : int32_t;
enum class ScatterToolDensity : uint8_t;

using LoadSaveCallback = void (*)(ModalResult result, const utf8* path);
using ScenarioSelectCallback = void (*)(const utf8* path);

namespace OpenRCT2
{
    class Formatter;
    struct ObjectEntryDescriptor;
    struct Peep;
    struct TileElement;
} // namespace OpenRCT2

namespace OpenRCT2::Ui::Windows
{
    constexpr int32_t kTabBarHeight = 43;

    extern bool gWindowSceneryScatterEnabled;
    extern uint16_t gWindowSceneryScatterSize;
    extern ScatterToolDensity gWindowSceneryScatterDensity;
    extern uint8_t gWindowSceneryRotation;
    extern bool gWindowSceneryEyedropperEnabled;
    extern bool gDisableErrorWindowSound;

    // About
    WindowBase* AboutOpen();

    // AssetPacks
    WindowBase* AssetPacksOpen();

    // Banner
    WindowBase* BannerOpen(WindowNumber number);

    // Changelog
    WindowBase* ChangelogOpen(WindowView personality);

    // Cheats
    WindowBase* CheatsOpen();

    // OPENRCT2MINI P1: Performance Profiler. See profiler-plan.md.
#ifdef ENABLE_PERFORMANCE_PROFILER
    WindowBase* PerformanceProfilerOpen();
#endif

    // OPENRCT2MINI gamepad-plan 1.11: Haptics control window.
    // Reachable from the Cheats / Debug dropdown (under Performance
    // Profiler when debuggingTools is on). Toggles rumble globally,
    // adjusts intensity, and exposes a Test Rumble button so the
    // user can verify the pad actually buzzes.
    WindowBase* HapticsOpen();

    // OPENRCT2MINI gamepad-plan 1.11b: Rumble Editor window.
    // Per-SoundId envelope editor reachable from the same dropdown.
    WindowBase* RumbleEditorOpen();

    // OPENRCT2MINI input-plan Track 2 §4.2: LED Options window.
    // Reachable from the same Cheats / Debug dropdown. Toggles the
    // DualShock-style lightbar globally, adjusts brightness, and
    // exposes a Test LED button so the user can verify the pad's
    // lightbar responds.
    WindowBase* LedOpen();

    // OPENRCT2MINI per-binding Modifier mode: BindingOptions window.
    // Opened by the wrench button next to each binding cell in the
    // Input Bindings list. Targets a single binding identified by
    // (shortcutId, kind, indexWithinKind). Hosts a single "Modifier
    // Key" checkbox controlling that binding's `is_modifier` flag.
    enum class BindingOptionsKind : uint8_t
    {
        keyboard = 0,
        mouse,
        gamepad,
    };
    WindowBase* BindingOptionsOpen(
        std::string_view shortcutId, BindingOptionsKind kind, int32_t indexWithinKind);

    // ClearScenery
    WindowBase* ClearSceneryOpen();
    void ToggleClearSceneryWindow();
    // OPENRCT2MINI grid-cursor-plan §11.9 / §18.C (2026-05-24):
    // grid-cursor dispatch + cost preview for the ClearScenery tool.
    // Apply runs ClearAction (with an error-popup guard that mirrors
    // the mouse onToolDrag); RefreshCost runs the Query path and
    // writes the result to the window's cost member so the cost line
    // stays current as the user steps the brush or resizes it.
    void WindowClearSceneryAtCursor();
    void WindowClearSceneryRefreshCost();

    // CustomCurrency
    WindowBase* CustomCurrencyOpen();

    // DebugPaint
    WindowBase* DebugPaintOpen();

    // DemolishRidePrompt
    WindowBase* RideDemolishPromptOpen(const Ride& ride);

    // EditorInventionsList
    WindowBase* EditorInventionsListOpen();

    // EditorBottomToolbar
    WindowBase* EditorBottomToolbarOpen();

    // EditorObjectSelection
    WindowBase* EditorObjectSelectionOpen();
    bool EditorObjectSelectionWindowCheck();
    void EditorObjectSelectionClose();

    // EditorParkEntrance
    WindowBase* EditorParkEntranceOpen();

    // EditorScenarioOptions
    WindowBase* EditorScenarioOptionsOpen();

    // Error
    WindowBase* ErrorOpen(StringId title, StringId message, const class Formatter& formatter, bool autoClose = false);
    WindowBase* ErrorOpen(std::string_view title, std::string_view message, bool autoClose = false);

    // Finances
    WindowBase* FinancesOpen();
    WindowBase* FinancesResearchOpen();
    WindowBase* FinancesMarketingOpen();

    // Footpath
    WindowBase* FootpathOpen();
    void WindowFootpathResetSelectedPath();
    void ToggleFootpathWindow();
    void WindowFootpathKeyboardShortcutTurnLeft();
    void WindowFootpathKeyboardShortcutTurnRight();
    void WindowFootpathKeyboardShortcutSlopeDown();
    void WindowFootpathKeyboardShortcutSlopeUp();
    void WindowFootpathKeyboardShortcutBuildCurrent();
    void WindowFootpathKeyboardShortcutDemolishCurrent();
    bool WindowFootpathSelectDefault();
    // OPENRCT2MINI grid-cursor-plan §14.2: bridge helpers the tool
    // context calls from outside Footpath.cpp. WindowFootpathRemove
    // is declared as a free function below for the same reason.
    void WindowFootpathRemove();
    // Place a footpath at a specific tile coord at the given base Z,
    // bypassing the screen-pos / mouse-cursor lookup. Used by the
    // gamepad-driven FootpathContext.
    void WindowFootpathPlaceAtTile(const TileCoordsXY& tile, int32_t baseZ);
    // Adjust the placement Z by ±step (±1 = ±kPathHeightStep).
    void WindowFootpathAdjustPlacementZ(int32_t step);
    // Set / clear the provisional ("ghost") footpath at a tile. The
    // grid cursor calls these from onActivate / onStep / onDeactivate
    // so the ghost tile renders alongside the highlight (mirrors the
    // mouse path's hover behaviour).
    //
    // OPENRCT2MINI grid-cursor-plan §14.2 (amendment 2026-05-20 —
    // Shift+D-pad Z): zOffset is the grid cursor's accumulated Z
    // offset (kPathHeightStep multiples). Defaults to 0 so existing
    // callers (onActivate / onStep at surface Z) stay unchanged;
    // onRaise / onLower pass gridCursor().getZ() so the ghost +
    // VirtualFloor track the Shift+up/down gesture in lockstep with
    // the mouse Shift+drag-Z path.
    void WindowFootpathSetProvisionalAtTile(const TileCoordsXY& tile, int32_t zOffset = 0);
    void WindowFootpathClearProvisional();
    // OPENRCT2MINI grid-cursor-plan §14.4 (2026-05-20): clear the
    // Footpath window's sticky error latch (_footpathErrorOccured).
    // The mouse path clears it on onToolUp; the grid cursor's
    // popup-dismiss flow has to clear it explicitly via this bridge
    // because it never routes through onToolUp. No-op when the
    // window isn't open.
    void WindowFootpathClearErrorLatch();
    // Close the Footpath window outright — used by the grid-cursor
    // interface.confirm verb ("OK, I'm done with this tool").
    void WindowFootpathClose();

    // OPENRCT2MINI grid-cursor-plan §16: input-mode of the Footpath
    // window. FootpathContextImpl branches verb dispatch per mode —
    // direct (onLand), rectangle drag (dragArea), bridge-pick anchor
    // (bridgePick), or step-extend bridge construction (bridgeBuild).
    // Returns `none` when the Footpath window isn't open / no tool is
    // active.
    enum class FootpathInputMode : uint8_t
    {
        none,
        onLand,
        dragArea,
        bridgePick,
        bridgeBuild,
    };
    FootpathInputMode WindowFootpathGetInputMode();

    // OPENRCT2MINI grid-cursor-plan §16: drag-area mode at-tile
    // helpers. The mouse path uses screen-coord versions that project
    // and call into the same window state these touch — this is the
    // gamepad equivalent for FootpathContextImpl::onPlace /
    // onCancel / onStep when in drag-area mode.
    //   Anchor: store _dragStartPos at this tile, light the map
    //           selection, set the provisional ghost.
    //   Preview: update the rectangle from anchor to this tile (live
    //            during cursor stepping after anchor).
    //   Commit: like Preview followed by WindowFootpathPlacePath() —
    //           dispatches FootpathPlaceAction for each tile in the
    //           rectangle, then clears the anchor.
    //   Clear: cancel — clear anchor + provisional, back to "no
    //          anchor" state.
    //   HasAnchor: true iff an anchor has been set since the last
    //              Clear / Commit. FootpathContextImpl::onPlace
    //              branches on this to choose Anchor vs Commit.
    // §17 (2026-05-23): all three APIs accept a Z offset (grid
    // cursor's CURRENT accumulated Z) so the whole rectangle
    // places at the offset Z. Each call recomputes the placement
    // Z from the anchor's stored natural baseZ + zOffset, so
    // mid-drag Z adjustments are honoured (the preview updates
    // and the eventual commit places at the user's chosen Z).
    void WindowFootpathDragAreaAnchorAtTile(const TileCoordsXY& tile, int32_t zOffset = 0);
    void WindowFootpathDragAreaPreviewAtTile(const TileCoordsXY& tile, int32_t zOffset = 0);
    void WindowFootpathDragAreaCommitAtTile(const TileCoordsXY& tile, int32_t zOffset = 0);
    void WindowFootpathDragAreaClear();
    bool WindowFootpathDragAreaHasAnchor();

    // OPENRCT2MINI grid-cursor-plan §16: bridge-pick → bridge-build
    // transition at a tile coord. Equivalent of the mouse path's
    // WindowFootpathStartBridgeAtPoint but with the direction picked
    // from the screen "up" → world direction (so D-pad up = forward
    // in user vision until they rotate). After this call, the
    // FootpathWindow is in PathConstructionMode::bridgeOrTunnel and
    // _footpathConstructFromPosition is set at this tile. No-op
    // when the window isn't in PathConstructionMode::bridgeOrTunnelPick.
    //
    // zOffset is the grid cursor's accumulated Z (kPathHeightStep
    // multiples). Lets the user raise the grid cursor above ground
    // before picking, so a bridge can be started from an existing
    // raised path / bridge end. The implementation scans tile
    // elements at this XY for a Path element whose Z matches the
    // requested elevation and snaps the anchor to it (matching the
    // mouse path's tileElement-based behaviour); otherwise it uses
    // surface Z + zOffset directly. Defaults to 0.
    void WindowFootpathStartBridgeAtTile(const TileCoordsXY& tile, int32_t zOffset = 0);

    // OPENRCT2MINI grid-cursor-plan §16: bridge head position
    // accessor — returns the tile of _footpathConstructFromPosition
    // when the window is in bridgeOrTunnel build mode, or nullopt
    // otherwise (window closed, in OnLand / dragArea / pick). Used
    // by FootpathContextImpl in bridgeBuild to keep the grid cursor
    // highlight in sync with the bridge head as segments are
    // extended / retracted / slope-adjusted.
    std::optional<TileCoordsXY> WindowFootpathGetBridgeHeadTile();

    // OPENRCT2MINI bug 2026-05-22: re-arm the tool widget appropriate
    // for the Footpath window's current _footpathConstructionMode.
    // The mouse path's StartBridgeAtPoint calls ToolCancel after
    // picking an anchor (the mouse user drives bridge via window
    // widgets, not the map tool) — so when the user transitions
    // controller-driven cursor → mouse-driven bridge build → press
    // Start to switch back to grid cursor, toolActive is false and
    // the InputManager's enter-focus-mode auto-engage branch never
    // fires. Calling this re-arms the right tool widget for the
    // current mode so the auto-engage works. No-op when the window
    // isn't open or the mode is `none`.
    void WindowFootpathReArmForCurrentMode();

    // GameBottomToolbar
    extern uint8_t gToolbarDirtyFlags;
    WindowBase* GameBottomToolbarOpen();
    void WindowGameBottomToolbarInvalidateNewsItem();

    // Guest
    WindowBase* GuestOpen(Peep* peep);

    // GuestList
    WindowBase* GuestListOpen();
    WindowBase* GuestListOpenWithFilter(GuestListFilterType type, int32_t index);
    void WindowGuestListRefreshList();

    // InstallTrack
    WindowBase* InstallTrackOpen(const utf8* path);

    // Land
    WindowBase* LandOpen();
    void ToggleLandWindow();
    // OPENRCT2MINI grid-cursor-plan §11.2: gamepad raise/lower for the
    // Land tool. Reads gMapSelectPositionA/B and gMapSelectType (set by
    // the grid cursor's WriteGridCursorSelection); dispatches the
    // canonical LandRaise/LandLower game action.
    //
    // §11.2 follow-up (2026-05-24, Land tool modes): when the user has
    // pressed the mountain-mode button, raise/lower swap to LandSmooth
    // (mirrors the mouse path's _landToolMountainMode branch). The
    // helpers read the LandWindow state via the IsMountainMode getter.
    void WindowLandRaiseAtCursor();
    void WindowLandLowerAtCursor();
    // OPENRCT2MINI grid-cursor-plan §11.2 follow-up (2026-05-24): Land
    // tool paint mode. When the user has pressed the paint-landscape
    // button, cursor.click dispatches SurfaceSetStyleAction over the
    // current map selection instead of LandRaise.
    void WindowLandPaintAtCursor();
    // Mode getters for TerrainContextImpl to branch verb dispatch on.
    // Both return false when the Land window isn't open.
    bool WindowLandIsMountainMode();
    bool WindowLandIsPaintMode();

    // LandRights
    WindowBase* LandRightsOpen();
    // OPENRCT2MINI grid-cursor-plan §11.5 / §18.C (2026-05-24):
    // grid-cursor dispatch + cost preview for the LandRights tool.
    // WindowLandRightsApplyAtCursor mirrors mouse onToolDown and
    // branches on IsOwnershipMode() to pick LandSetRights vs
    // LandBuyRights. WindowLandRightsRefreshCost runs the Query
    // path of the same action and writes the result back to the
    // window's display-cost member so the cost line stays current
    // as the user steps the brush or resizes it.
    void WindowLandRightsApplyAtCursor();
    void WindowLandRightsRefreshCost();

    // LoadSave
    WindowBase* LoadsaveOpen(
        LoadSaveAction action, LoadSaveType type, std::string_view defaultPath,
        std::function<void(ModalResult result, std::string_view)> callback, bool isJsCallback, TrackDesign* trackDesign);
    void WindowLoadSaveInputKey(WindowBase* w, uint32_t keycode);

    // Main
    WindowBase* MainOpen();

    // Map
    WindowBase* MapOpen();
    void WindowMapReset();

    // MapGen
    WindowBase* MapgenOpen();

    // MapTooltip
    void SetMapTooltip(Formatter& ft);
    const Formatter& GetMapTooltip();
    void WindowMapTooltipUpdateVisibility();

    // MazeConstruction
    WindowBase* MazeConstructionOpen();
    void WindowMazeConstructionUpdatePressedWidgets();

    // Multiplatyer
    WindowBase* MultiplayerOpen();

    // NewCampaign
    WindowBase* NewCampaignOpen(int16_t campaignType);
    void WindowCampaignRefreshRides();

    // NewRide
    WindowBase* NewRideOpen();
    WindowBase* NewRideOpenResearch();
    void WindowNewRideInitVars();
    void WindowNewRideFocus(RideSelection rideItem);

    // News
    WindowBase* NewsOpen();

    // NetworkStatus
    WindowBase* NetworkStatusOpen(const std::string& text, CloseCallback onClose);
    WindowBase* NetworkStatusOpenPassword();
    void WindowNetworkStatusClose();

    // ObjectLoadError
    WindowBase* ObjectLoadErrorOpen(utf8* path, size_t numMissingObjects, const ObjectEntryDescriptor* missingObjects);

    // Options
    WindowBase* OptionsOpen();

    // OverwritePrompt
    WindowBase* WindowOverwritePromptOpen(
        std::string_view name, std::string_view path, LoadSaveAction action, LoadSaveType type, TrackDesign* trackDesignPtr);
    void WindowLoadSaveOverwritePromptInputKey(WindowBase* w, uint32_t keycode);

    // Park
    WindowBase* ParkAwardsOpen();
    WindowBase* ParkEntranceOpen();
    WindowBase* ParkGuestsOpen();
    WindowBase* ParkObjectiveOpen();
    WindowBase* ParkRatingOpen();

    // Player
    WindowBase* PlayerOpen(uint8_t id);

    // ProgressWindow
    WindowBase* ProgressWindowOpen(const std::string& text, CloseCallback onClose = nullptr);
    void ProgressWindowSet(uint32_t currentProgress, uint32_t totalCount, StringId format = kStringIdNone);
    void ProgressWindowClose();

    // PatrolArea
    WindowBase* PatrolAreaOpen(EntityId staffId);
    EntityId WindowPatrolAreaGetCurrentStaffId();

    // RefurbishRidePrompt
    WindowBase* RideRefurbishPromptOpen(const Ride& ride);

    // Research
    WindowBase* ResearchOpen();
    void WindowResearchDevelopmentMouseUp(WidgetIndex widgetIndex, WidgetIndex baseWidgetIndex);
    void WindowResearchDevelopmentPrepareDraw(WindowBase* w, WidgetIndex baseWidgetIndex);
    void WindowResearchDevelopmentDraw(WindowBase* w, Drawing::RenderTarget& rt, WidgetIndex baseWidgetIndex);
    void WindowResearchFundingMouseDown(WindowBase* w, WidgetIndex widgetIndex, WidgetIndex baseWidgetIndex);
    void WindowResearchFundingMouseUp(WidgetIndex widgetIndex, WidgetIndex baseWidgetIndex);
    void WindowResearchFundingDropdown(WidgetIndex widgetIndex, int32_t selectedIndex, WidgetIndex baseWidgetIndex);
    void WindowResearchFundingPrepareDraw(WindowBase* w, WidgetIndex baseWidgetIndex);
    void WindowResearchFundingDraw(WindowBase* w, Drawing::RenderTarget& rt);

    // Ride
    WindowBase* RideMainOpen(const Ride& ride);
    WindowBase* RideOpenTrack(TileElement* tileElement);
    WindowBase* RideOpenVehicle(Vehicle* vehicle);
    void WindowRideInvalidateVehicle(const Vehicle& vehicle);
    void WindowRidePaintResetVehicle(RideId rideIndex);
    void WindowRideMeasurementsDesignCancel();

    // RideConstruction
    WindowBase* RideConstructionOpen();
    void WindowRideConstructionUpdateActiveElementsImpl();
    void WindowRideConstructionUpdateEnabledTrackPieces();
    void RideRestoreProvisionalTrackPiece();
    void RideRemoveProvisionalTrackPiece();
    void RideConstructionToolupdateEntranceExit(const ScreenCoordsXY& screenCoords);
    void RideConstructionToolupdateConstruct(const ScreenCoordsXY& screenCoords);
    void RideConstructionTooldownConstruct(const ScreenCoordsXY& screenCoords);
    void UpdateGhostTrackAndArrow();
    void WindowRideConstructionKeyboardShortcutTurnLeft();
    void WindowRideConstructionKeyboardShortcutTurnRight();
    void WindowRideConstructionKeyboardShortcutUseTrackDefault();
    void WindowRideConstructionKeyboardShortcutSlopeDown();
    void WindowRideConstructionKeyboardShortcutSlopeUp();
    void WindowRideConstructionKeyboardShortcutChainLiftToggle();
    void WindowRideConstructionKeyboardShortcutBankLeft();
    void WindowRideConstructionKeyboardShortcutBankRight();
    void WindowRideConstructionKeyboardShortcutPreviousTrack();
    void WindowRideConstructionKeyboardShortcutNextTrack();
    void WindowRideConstructionKeyboardShortcutBuildCurrent();
    void WindowRideConstructionKeyboardShortcutDemolishCurrent();

    // RideList
    WindowBase* RideListOpen();
    void WindowRideListRefreshList(WindowBase* w);

    // SavePrompt
    WindowBase* SavePromptOpen();

    // ScenarioSelect
    WindowBase* ScenarioselectOpen(ScenarioSelectCallback callback);
    WindowBase* ScenarioselectOpen(std::function<void(std::string_view)> callback);

    // Scenery
    WindowBase* SceneryOpen();
    void WindowScenerySetSelectedItem(
        const ScenerySelection& sceneryconst, std::optional<Drawing::Colour> primary, std::optional<Drawing::Colour> secondary,
        std::optional<Drawing::Colour> tertiary, std::optional<uint8_t> rotation);
    void WindowScenerySetSelectedTab(ObjectEntryIndex sceneryGroupIndex);
    void WindowScenerySetDefaultPlacementConfiguration();
    void WindowSceneryInit();
    void WindowSceneryResetSelectedSceneryItems();
    const ScenerySelection WindowSceneryGetTabSelection();
    void ToggleSceneryWindow();

    // SceneryScatter
    WindowBase* SceneryScatterOpen();

#ifndef DISABLE_NETWORK
    // ServerList
    WindowBase* ServerListOpen();

    // ServerStart
    WindowBase* ServerStartOpen();
#endif

    // ShortcutKeys
    WindowBase* ShortcutKeysOpen();

    // Sign
    WindowBase* SignOpen(WindowNumber number);
    WindowBase* SignSmallOpen(WindowNumber number);

    // Staff
    WindowBase* StaffOpen(Peep* peep);

    // StaffFirePrompt
    WindowBase* StaffFirePromptOpen(Peep* peep);

    // StaffList
    WindowBase* StaffListOpen();
    void WindowStaffListRefresh();

    // OPENRCT2MINI: on-screen keyboard. See osk-plan.md for design.
    // Two layouts: full QWERTY and numpad. Spawned automatically when
    // a TextInputWindow opens (or the InGameConsole, see Rev 8).
    // OskMode enum is in interface/Window.h so default-args can use it.
    void OskOpen(WindowBase* parent, OskMode mode = OskMode::full);
    // OPENRCT2MINI: snapshot of the OSK's edit-strip buffer for parent
    // windows / textbox widgets to render in real time. Empty if no
    // OSK is up.
    std::string OskGetCurrentText();
    // Caret position within the OSK buffer, byte offset.
    size_t OskGetCaretByteOffset();
    // Caret blink state matching the OSK's edit-strip caret, so parent
    // textbox widgets can flash in sync.
    bool OskCaretIsFlashed();
    // OPENRCT2MINI: spawn the OSK for a `WindowStartTextbox`-style
    // inline textbox. Differs from the TextInputWindow path because
    // the parent isn't a modal dialog — it's a regular window with
    // an embedded textbox widget. On commit the OSK fires the
    // parent's onTextInput(widgetIdx, text) and ends the textbox
    // session via WindowCancelTextbox.
    void OskOpenForTextbox(
        WindowBase* parent, WidgetIndex widgetIdx, std::string_view initialText, size_t maxLength,
        OskMode mode = OskMode::full);
    // OPENRCT2MINI: spawn the OSK to drive the in-game console. Always
    // opens in full-keyboard mode. Commit (Start) submits the current
    // line and clears the OSK buffer (console stays open for the next
    // line). Cancel (Select) closes the console entirely.
    void OskOpenForConsole();
    void OskClose();
    bool OskIsActive();
    // OPENRCT2MINI: pixel height of the currently-open OSK window
    // (full mode = 240, console mode = 214). Returns 0 if no OSK.
    // Used by InGameConsole to clamp its draw region above the OSK.
    int32_t OskGetActiveHeight();
    // OPENRCT2MINI: called by the OSK at commit time. Replaces the
    // TextInputWindow's buffer with `text`, fires its OK callback and
    // closes it. No-op if `w` isn't a TextInputWindow.
    void TextInputCommitFromOsk(WindowBase* w, std::string_view text);
    // OPENRCT2MINI: called by the OSK on cancel. Closes the
    // TextInputWindow without firing its OK callback (cancel
    // callback will fire from the close path).
    void TextInputCancelFromOsk(WindowBase* w);
    // OPENRCT2MINI: read the current text buffer from a
    // TextInputWindow so the OSK can initialise its editor with what
    // the user is editing. Returns empty string if `w` isn't one.
    std::string TextInputReadBuffer(WindowBase* w);
    // OPENRCT2MINI osk-overhaul §1+§2: dispatched from OskContextImpl
    // (input/InputManager.cpp). cursor.click presses the focus-mode-
    // selected key; cursor.cancel becomes Backspace.
    void OskActivateFocusedKey();
    void OskActivateBackspace();
    // Suppress shortcut chord handlers (X = game speed, Y = rotate
    // construction, L2/R2 = view/zoom) while the OSK is up. Wraps
    // OskIsActive() for callers that want a more readable check.
    inline bool OskShouldSuppressShortcuts()
    {
        return OskIsActive();
    }

    // TextInput
    void WindowTextInputKey(WindowBase* w, uint32_t keycode);
    void WindowTextInputOpen(
        WindowBase* call_w, WidgetIndex call_widget, StringId title, StringId description, const Formatter& descriptionArgs,
        StringId existing_text, uintptr_t existing_args, int32_t maxLength, OskMode oskMode = OskMode::full);
    void WindowTextInputRawOpen(
        WindowBase* call_w, WidgetIndex call_widget, StringId title, StringId description, const Formatter& descriptionArgs,
        const_utf8string existing_text, int32_t maxLength, OskMode oskMode = OskMode::full);
    void WindowTextInputOpen(
        std::string_view title, std::string_view description, std::string_view initialValue, size_t maxLength,
        std::function<void(std::string_view)> okCallback, std::function<void()> cancelCallback,
        OskMode oskMode = OskMode::full);

    // Themes
    WindowBase* ThemesOpen();

    // TileInspector
    WindowBase* TileInspectorOpen();
    void WindowTileInspectorClearClipboard();
    void WindowTileInspectorKeyboardShortcutToggleInvisibility();

    // TitleExit
    WindowBase* TitleExitOpen();

    // TitleLogo
    WindowBase* TitleLogoOpen();

    // TitleMenu
    WindowBase* TitleMenuOpen();

    // TitleOptions
    WindowBase* TitleOptionsOpen();

    // TitleVersion
    WindowBase* TitleVersionOpen();

    // Tooltip
    void WindowTooltipReset(const ScreenCoordsXY& screenCoords);
    void WindowTooltipShow(const StringWithArgs& message, ScreenCoordsXY screenCoords);
    void WindowTooltipOpen(WindowBase* widgetWindow, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
    void WindowTooltipClose();

    // TopToolbar
    WindowBase* TopToolbarOpen();

    // TrackDesignPlace
    WindowBase* TrackPlaceOpen(const struct TrackDesignFileRef* tdFileRef);
    void TrackPlaceClearProvisionalTemporarily();
    void TrackPlaceRestoreProvisional();

    // TrackDesignManage
    WindowBase* TrackManageOpen(struct TrackDesignFileRef* tdFileRef);

    // TrackList
    // rct2: 0x00F635EE
    extern RideSelection _window_track_list_item;
    WindowBase* TrackListOpen(RideSelection item);
    void WindowTrackDesignListReloadTracks();
    void WindowTrackDesignListSetBeingUpdated(bool beingUpdated);

    // Transparency
    WindowBase* TransparencyOpen();

    // ViewClipping
    WindowBase* ViewClippingOpen();

    // Viewport
    WindowBase* ViewportOpen();

    // Water
    WindowBase* WaterOpen();
    void ToggleWaterWindow();
    // OPENRCT2MINI grid-cursor-plan §18.C (2026-05-24): grid-cursor
    // raise/lower dispatch for the Water tool. Mirrors the Land helpers
    // (WindowLandRaiseAtCursor / WindowLandLowerAtCursor) — reads the
    // current map selection rect (set by WriteGridCursorSelection at
    // size 1 or by its multi-cell sibling at size > 1) and dispatches
    // WaterRaiseAction / WaterLowerAction, both of which accept a
    // MapRange natively.
    void WindowWaterRaiseAtCursor();
    void WindowWaterLowerAtCursor();
    // OPENRCT2MINI grid-cursor-plan §11.3 retroactive polish
    // (2026-05-24): live cost preview for the Water tool — Queries
    // both raise and lower actions, writes both costs to the window,
    // invalidates. Called from WaterContextImpl::onStep + onActivate.
    void WindowWaterRefreshCost();
} // namespace OpenRCT2::Ui::Windows
