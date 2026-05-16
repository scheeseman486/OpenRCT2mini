/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <string_view>

namespace OpenRCT2::Ui::ShortcutId
{
    // Interface / general
    constexpr std::string_view kInterfaceCloseTop = "interface.general.close_top";
    constexpr std::string_view kInterfaceCloseAll = "interface.general.close_all";
    constexpr std::string_view kInterfaceRotateConstruction = "interface.general.rotate_construction";
    constexpr std::string_view kInterfaceCancelConstruction = "interface.general.cancel_construction";
    constexpr std::string_view kInterfacePause = "interface.general.pause";

    // OPENRCT2MINI focus-mode-plan / Phase F.1: focus-mode activator
    // (formerly "caret browse"). When the user fires this shortcut,
    // and a window with focusable widgets is on top, the InputManager
    // promotes the active context to `widgetFocus`. D-pad then steps
    // spatially between buttons / checkboxes / textboxes / scroll
    // widgets; A presses the focused widget; B exits. Only relevant
    // when Config::General::widgetFocusAlwaysOn is false — when it's
    // true (default), focus mode auto-activates without this gesture.
    // Default keyboard binding empty; default gamepad binding PAD BACK
    // 500ms hold (via task #373's HOLD mechanism). Mini-compatible.
    // See focus-mode-plan.md for the full design.
    constexpr std::string_view kInterfaceEnterFocusMode = "interface.general.enter_focus_mode";
    // OPENRCT2MINI cursor-selector-modal-plan §CS-R1: explicit
    // "show cursor" shortcut. Wakes the mouse cursor regardless of
    // current SelectorMode. The Mini handheld never sees
    // SDL_MOUSEMOTION (the virtual cursor writes _cursorState
    // directly, not through SDL events) and the §5 modal-switch
    // filter drops shared-binding cursor.* events while in
    // SelectorMode::active — so Mini users with widgetFocusAlwaysOn
    // = true would otherwise be stuck in selector mode. This
    // shortcut is the documented escape hatch. Default keyboard
    // binding empty (host PC users can leave widgetFocusAlwaysOn
    // = false if they prefer cursor-first); Mini users can bind it
    // through the rebind UI to whatever they like.
    constexpr std::string_view kInterfaceShowCursor = "interface.general.show_cursor";
    // OPENRCT2MINI focus-mode-plan §F.8: cycle the focus ring between
    // windows that have focusable widgets. The bootstrap auto-picks
    // the topmost qualifying window; these two shortcuts let the
    // user walk forward/backward through the rest of the stack
    // (z-order, wraps around). Default SHIFT+TAB for forward, CTRL+
    // TAB for backward — keyboard-conventional even if not the most
    // common Tab-as-next convention. Gamepad bindings TBD.
    constexpr std::string_view kInterfaceCycleNextWindow = "interface.general.cycle_next_window";
    constexpr std::string_view kInterfaceCyclePreviousWindow = "interface.general.cycle_previous_window";
    // OPENRCT2MINI focus-mode-plan §F.9: focus-ring directional moves,
    // namespaced separately from cursor.* so the two concepts (mouse
    // cursor position vs. yellow focus selector) can be rebound
    // independently. User report: the original cursor.* bindings
    // drove both, which made it impossible to have D-pad move only
    // focus or only cursor. The strategy's onShortcut now matches
    // focus.up/down/left/right instead of cursor.up/down/left/right.
    // Defaults overlap with cursor.* on purpose so first-launch
    // behaviour is preserved (D-pad / arrows still navigate the
    // focus ring); users who want non-overlapping behaviour can
    // unbind one side via the Input Bindings window.
    constexpr std::string_view kFocusUp = "focus.up";
    constexpr std::string_view kFocusDown = "focus.down";
    constexpr std::string_view kFocusLeft = "focus.left";
    constexpr std::string_view kFocusRight = "focus.right";
    constexpr std::string_view kInterfaceDecreaseSpeed = "interface.misc.decrease_speed";
    constexpr std::string_view kInterfaceIncreaseSpeed = "interface.misc.increase_speed";
    constexpr std::string_view kInterfaceToggleToolbars = "interface.misc.toggle_toolbars";
    constexpr std::string_view kInterfaceScreenshot = "interface.misc.screenshot";
    constexpr std::string_view kInterfaceGiantScreenshot = "interface.misc.giant_screenshot";
    constexpr std::string_view kInterfaceLoadGame = "interface.misc.load_game";
    constexpr std::string_view kInterfaceSaveGame = "interface.misc.save_game";
    constexpr std::string_view kInterfaceMute = "interface.misc.mute";
    constexpr std::string_view kInterfaceSceneryPicker = "interface.misc.scenery_picker";
    constexpr std::string_view kInterfaceDisableClearance = "interface.misc.disable_clearance";
    constexpr std::string_view kInterfaceMultiplayerChat = "interface.misc.multiplayer_chat";

    constexpr std::string_view kInterfaceScaleToggleWindowMode = "interface.graphics.toggle_window_mode";
    constexpr std::string_view kInterfaceScaleIncrease = "interface.graphics.increase";
    constexpr std::string_view kInterfaceScaleDecrease = "interface.graphics.decrease";

    // Interface / open
    constexpr std::string_view kInterfaceOpenLand = "interface.open.land";
    constexpr std::string_view kInterfaceOpenWater = "interface.open.water";
    constexpr std::string_view kInterfaceClearScenery = "interface.open.clear_scenery";
    constexpr std::string_view kInterfaceOpenScenery = "interface.open.scenery";
    constexpr std::string_view kInterfaceOpenFootpaths = "interface.open.footpaths";
    constexpr std::string_view kInterfaceOpenNewRide = "interface.open.new_ride";
    constexpr std::string_view kInterfaceOpenFinances = "interface.open.finances";
    constexpr std::string_view kInterfaceOpenResearch = "interface.open.research";
    constexpr std::string_view kInterfaceOpenRides = "interface.open.rides";
    constexpr std::string_view kInterfaceOpenPark = "interface.open.park";
    constexpr std::string_view kInterfaceOpenGuests = "interface.open.guests";
    constexpr std::string_view kInterfaceOpenStaff = "interface.open.staff";
    constexpr std::string_view kInterfaceOpenMessages = "interface.open.messages";
    constexpr std::string_view kInterfaceOpenMap = "interface.open.map";
    constexpr std::string_view kInterfaceShowOptions = "interface.open.options";
    constexpr std::string_view kInterfaceOpenKeyboardShortcuts = "interface.open.keyboard_shortcuts";
    constexpr std::string_view kInterfaceOpenTransparencyOptions = "interface.open.transparency_options";
    constexpr std::string_view kInterfaceOpenCheats = "interface.open.cheats";
    constexpr std::string_view kInterfaceOpenTileInspector = "interface.open.tileinspector";
    constexpr std::string_view kInterfaceMultiplayerShow = "interface.open.multiplayer";

    // View / general
    constexpr std::string_view kViewGeneralZoomOut = "view.general.zoom_out";
    constexpr std::string_view kViewGeneralZoomIn = "view.general.zoom_in";
    constexpr std::string_view kViewGeneralRotateClockwise = "view.general.rotate_clockwise";
    constexpr std::string_view kViewGeneralRotateAnticlockwise = "view.general.rotate_anticlockwise";

    // OPENRCT2MINI mouse-input refactor: context-sensitive wheel
    // shortcuts. Default-bound to MOUSE WHEEL UP / DOWN. Their action
    // lambdas zoom the main viewport ONLY when the cursor is over a
    // viewport-class window. When the cursor is over a scroll widget
    // or another wheel-handling widget, these no-op so the existing
    // _cursorState.wheel-driven WindowAllWheelInput dispatch can
    // handle the widget-local scroll without doubling up with zoom.
    // kViewGeneralZoomIn/Out remain unconditional zoom shortcuts —
    // the user can still bind them to any input for force-zoom.
    constexpr std::string_view kViewZoomScrollUp = "view.general.zoom_scroll_up";
    constexpr std::string_view kViewZoomScrollDown = "view.general.zoom_scroll_down";

    // View / scroll
    constexpr std::string_view kViewScrollUp = "view.scroll.up";
    constexpr std::string_view kViewScrollLeft = "view.scroll.left";
    constexpr std::string_view kViewScrollRight = "view.scroll.right";
    constexpr std::string_view kViewScrollDown = "view.scroll.down";

    // View / toggle
    constexpr std::string_view kViewToggleUnderground = "view.toggle.show_underground";
    constexpr std::string_view kViewToggleTransparentWater = "view.toggle.transparent_water";
    constexpr std::string_view kViewToggleBaseLand = "view.toggle.hide_base_land";
    constexpr std::string_view kViewToggleVerticalLand = "view.toggle.hide_vertical_land";
    constexpr std::string_view kViewToggleRides = "view.toggle.transparent_rides";
    constexpr std::string_view kViewToggleVehicles = "view.toggle.transparent_vehicles";
    constexpr std::string_view kViewToggleVegetation = "view.toggle.transparent_vegetation";
    constexpr std::string_view kViewToggleScenery = "view.toggle.transparent_scenery";
    constexpr std::string_view kViewToggleFootpaths = "view.toggle.transparent_footpaths";
    constexpr std::string_view kViewToggleSupports = "view.toggle.hide_supports";
    constexpr std::string_view kViewToggleGuests = "view.toggle.hide_guests";
    constexpr std::string_view kViewToggleStaff = "view.toggle.hide_staff";
    constexpr std::string_view kViewToggleLandHeightMarkers = "view.toggle.show_land_height";
    constexpr std::string_view kViewToggleTrackHeightMarkers = "view.toggle.show_track_height";
    constexpr std::string_view kViewToggleFootpathHeightMarkers = "view.toggle.show_footpath_height";
    constexpr std::string_view kViewToggleGridlines = "view.toggle.show_gridlines";
    constexpr std::string_view kViewToggleCutAway = "view.toggle.toggle_cut_away";
    constexpr std::string_view kViewToggleFootpathIssues = "view.toggle.highlight_path_issues";

    // Window / ride construction
    constexpr std::string_view kWindowRideConstructionTurnLeft = "window.rideconstruction.turn_left";
    constexpr std::string_view kWindowRideConstructionTurnRight = "window.rideconstruction.turn_right";
    constexpr std::string_view kWindowRideConstructionDefault = "window.rideconstruction.default";
    constexpr std::string_view kWindowRideConstructionSlopeDown = "window.rideconstruction.slope_down";
    constexpr std::string_view kWindowRideConstructionSlopeUp = "window.rideconstruction.slope_up";
    constexpr std::string_view kWindowRideConstructionChainLift = "window.rideconstruction.chain_lift";
    constexpr std::string_view kWindowRideConstructionBankLeft = "window.rideconstruction.bank_left";
    constexpr std::string_view kWindowRideConstructionBankRight = "window.rideconstruction.bank_right";
    constexpr std::string_view kWindowRideConstructionPrevious = "window.rideconstruction.previous";
    constexpr std::string_view kWindowRideConstructionNext = "window.rideconstruction.next";
    constexpr std::string_view kWindowRideConstructionBuild = "window.rideconstruction.build";
    constexpr std::string_view kWindowRideConstructionDemolish = "window.rideconstruction.demolish";

    // Window / tile inspector
    constexpr std::string_view kWindowTileInspectorToggleInvisibility = "window.tileinspector.toggle_invisibility";
    constexpr std::string_view kWindowTileInspectorCopy = "window.tileinspector.copy";
    constexpr std::string_view kWindowTileInspectorPaste = "window.tileinspector.paste";
    constexpr std::string_view kWindowTileInspectorSort = "window.tileinspector.sort";
    constexpr std::string_view kWindowTileInspectorRemove = "window.tileinspector.remove";
    constexpr std::string_view kWindowTileInspectorMoveUp = "window.tileinspector.move_up";
    constexpr std::string_view kWindowTileInspectorMoveDown = "window.tileinspector.move_down";
    constexpr std::string_view kWindowTileInspectorIncreaseX = "window.tileinspector.increase_x";
    constexpr std::string_view kWindowTileInspectorDecreaseX = "window.tileinspector.decrease_x";
    constexpr std::string_view kWindowTileInspectorIncreaseY = "window.tileinspector.increase_y";
    constexpr std::string_view kWindowTileInspectorDecreaseY = "window.tileinspector.decrease_y";
    constexpr std::string_view kWindowTileInspectorIncreaseHeight = "window.tileinspector.increase_height";
    constexpr std::string_view kWindowTileInspectorDecreaseHeight = "window.tileinspector.decrease_height";
    constexpr std::string_view kWindowTileInspectorChangeWallSlope = "window.tileinspector.toggle_wall_slope";

    // Debug
    constexpr std::string_view kDebugToggleConsole = "debug.console";
    constexpr std::string_view kDebugTogglePaintDebugWindow = "debug.toggle_paint_debug_window";
    constexpr std::string_view kDebugAdvanceTick = "debug.advance_tick";

    // OPENRCT2MINI gamepad-plan 1.5: virtual cursor + click + chord
    // shortcuts. These migrate the existing _vKb*-driven behaviours
    // (rev W*/cuts 38b/43/44/60/61) from scancode synthesis + direct
    // controller polling to the unified ShortcutManager pipeline. Each
    // shortcut here has a corresponding entry in the migration table
    // in gamepad-plan.md §1.5.
    constexpr std::string_view kCursorUp           = "cursor.up";
    constexpr std::string_view kCursorDown         = "cursor.down";
    constexpr std::string_view kCursorLeft         = "cursor.left";
    constexpr std::string_view kCursorRight        = "cursor.right";
    constexpr std::string_view kCursorClick        = "cursor.click";          // primary, mouse-left equivalent
    constexpr std::string_view kCursorCancel       = "cursor.cancel";         // secondary, mouse-right equivalent
    constexpr std::string_view kCursorFastModifier = "cursor.fast_modifier";  // held → 2.5x cursor speed
    // OPENRCT2MINI grid-cursor-plan §5: precision modifier — held while
    // the active context is a tool, D-pad selects a sub-tile position
    // (corner / edge / quadrant per the tool's precisionSubset()) on
    // the current tile instead of stepping to a neighbour tile. Mirrors
    // the held-only modifier shape of kCursorFastModifier (empty action
    // lambda; per-frame polled by the tool context's processFrame).
    constexpr std::string_view kCursorPrecisionModifier = "cursor.precision_modifier";
    constexpr std::string_view kInterfaceConstructionZLock
        = "interface.general.construction_z_lock"; // held → ride/scenery Z-axis lock
    // OPENRCT2MINI grid-cursor-plan §14.2: tool-context Z raise / lower
    // verbs. Active context is a tool (footpath / terrain / water /
    // scenery / wall / land-rights / ride-construction / tile-
    // inspector); the dispatcher in ToolContext::onShortcut routes
    // these to the tool's onRaise / onLower virtual. Default keyboard
    // bindings PAGEUP / PAGEDOWN; no default pad binding (the chord-
    // via-modifier flow on PAD is a follow-up).
    constexpr std::string_view kInterfaceConstructionZRaise = "interface.general.construction_z_raise";
    constexpr std::string_view kInterfaceConstructionZLower = "interface.general.construction_z_lower";
    constexpr std::string_view kInterfaceShadeWindowUnderCursor
        = "interface.general.shade_window_under_cursor"; // tap → toggle shade on window under cursor
    constexpr std::string_view kInterfaceToggleShadeAllWindows
        = "interface.general.shade_all_windows"; // 500ms hold → shade-all / unshade-all toggle
    constexpr std::string_view kInterfaceCloseWindowUnderCursor
        = "interface.general.close_window_under_cursor"; // chord → close window at cursor

    // OPENRCT2MINI gamepad-plan 1.5g: generic shift-modifier shortcut.
    // Held-state ORs into InputManager's synthetic ModifierKey::shift
    // bit so OpenRCT2's KMOD_SHIFT-driven behaviours (vertical track
    // stack, scenery vertical-step, vertical track piece in maze, etc.)
    // become accessible to host gamepad users without a real keyboard.
    // Default keyboard binding is empty (real Shift already works
    // through SDL mod state); default PAD binding is empty (user picks).
    constexpr std::string_view kInterfaceShiftModifier
        = "interface.general.shift_modifier";

    // OPENRCT2MINI gamepad-plan 1.6c: generic dismiss / confirm
    // shortcuts. Each modal context (OSK, LoadSave, OverwritePrompt,
    // TextInput modal, in-game console, chat, in-place widget
    // textbox) registers a ModalHooks callback pair with
    // InputManager on activate. When dismiss / confirm fires, the
    // registered callback is invoked — replacing the seven separate
    // hardcoded SDLK_ESCAPE / SDLK_RETURN scancode checks the audit
    // documented. Defaults: ESCAPE + PAD BACK for dismiss; RETURN +
    // PAD START for confirm. Outside any modal these fire no-op
    // action lambdas, so world-context behaviour (rotate camera on
    // RETURN, cancel tool on ESCAPE via kInterfaceCancelConstruction)
    // is unchanged.
    constexpr std::string_view kInterfaceDismiss = "interface.general.dismiss";
    constexpr std::string_view kInterfaceConfirm = "interface.general.confirm";

    // OPENRCT2MINI text-editing-de-hardcode: bindable caret movement +
    // clipboard shortcuts for the text-input composition layer. These
    // replace the previously-hardcoded SDL_KEYDOWN dispatch in
    // TextComposition.cpp (BACKSPACE / HOME / END / DELETE / LEFT /
    // RIGHT / CTRL+C / CTRL+V / CTRL+X). Each modifier+key combination
    // is its own bindable action per the no-hardcoded-inputs principle
    // established by tasks #372, #399, #401. Allow-listed in both
    // OskContextImpl and WidgetTextBoxContextImpl (so caret movement
    // works whether the OSK is up or the user is typing directly with
    // a hardware keyboard). Ctrl-modified variants exploit the
    // existing chord-matching machinery — listing them on the
    // allow-list is sufficient, no special chord logic needed.
    constexpr std::string_view kInterfaceCaretLeft = "interface.textediting.caret_left";
    constexpr std::string_view kInterfaceCaretRight = "interface.textediting.caret_right";
    constexpr std::string_view kInterfaceCaretWordLeft = "interface.textediting.caret_word_left";
    constexpr std::string_view kInterfaceCaretWordRight = "interface.textediting.caret_word_right";
    constexpr std::string_view kInterfaceTextBackspace = "interface.textediting.backspace";
    constexpr std::string_view kInterfaceTextBackspaceWord = "interface.textediting.backspace_word";
    constexpr std::string_view kInterfaceTextDelete = "interface.textediting.delete";
    constexpr std::string_view kInterfaceTextDeleteWord = "interface.textediting.delete_word";
    constexpr std::string_view kInterfaceClipboardCopy = "interface.textediting.clipboard_copy";
    constexpr std::string_view kInterfaceClipboardCut = "interface.textediting.clipboard_cut";
    constexpr std::string_view kInterfaceClipboardPaste = "interface.textediting.clipboard_paste";

    // OPENRCT2MINI mouse-input refactor: camera drag.
    // Held over the main viewport → camera pans with cursor motion.
    // Released within ~500ms → fires the context-sensitive right-click
    // action (delete tile element / etc.) at the cursor position. Used
    // to be hardcoded to the right mouse button in MouseInput.cpp's
    // rightPress handler — now bindable like every other input.
    // Default keyboard binding empty, default mouse binding RMB,
    // default gamepad binding PAD B. cursor.cancel still owns the
    // "right-click action without drag" channel via its own polling;
    // this shortcut owns the press-and-hold camera-drag gesture.
    constexpr std::string_view kInterfaceCameraDrag = "interface.general.camera_drag";
} // namespace OpenRCT2::Ui::ShortcutId
