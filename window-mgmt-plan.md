# Window-Management Features Plan

OpenRCT2mini revisions adding two window-management features for handheld use:

* **A.** A held shortcut that drags whatever window is under the cursor, mimicking title-bar drag from anywhere inside the window.
* **B.** A "shade" toggle on title-barred windows that collapses the body to title-bar height while preserving game state.

Both are framework-level changes; neither requires per-window code.

## 0. Reference points in the existing codebase

For implementation:

* `WindowBase` — `src/openrct2/interface/WindowBase.h` (height, widgets, flags)
* `WindowBase::resizeFrame()` — `src/openrct2/interface/WindowBase.cpp:74` (the layout choke point every window goes through)
* `repositionCloseButton()` — `WindowBase.cpp:53` (already swaps sides via `windowButtonsOnTheLeft`)
* `WidgetType` / `WidgetFlag` — `src/openrct2/interface/Widget.h` (`isHidden = 5` exists; flag enum is `uint8_t` with bit 7 free)
* `Widget::isVisible()` — `Widget.h:179`
* `WindowDrawWidgets()` — `src/openrct2-ui/interface/Window.cpp:1036` (consults `isVisible()`)
* `FindWidgetFromPoint()` — `src/openrct2-ui/WindowManager.cpp:1217` (also consults `isVisible()`)
* `InputWindowPositionBegin/Continue/End` — `src/openrct2-ui/input/MouseInput.cpp:474, 483, 490`
* `InputStateWidgetPressed` left-release dispatch to `onMouseUp` — `MouseInput.cpp:1424`
* Shortcut registration — `src/openrct2-ui/input/Shortcuts.cpp:757+`
* Held-shortcut polling pattern — `src/openrct2-ui/input/InputManager.cpp:480` (`processViewScrollEvent`)

## 1. Feature A — Drag-window hotkey

### 1.1 Goal

Player parks the cursor over any non-toolbar window, holds the bound button (configurable shortcut), moves the cursor — the window follows. Releasing the button drops the window. Equivalent to grabbing the title bar.

On the Miyoo Mini target, this binds to the **X face button**, which is currently the game-speed cycle button. Cycling game speed is moved entirely to the toolbar dropdown (already merged in Cut 31), freeing X for the more useful window-management role on a tiny screen.

### 1.2 Why this is straightforward

`InputWindowPositionBegin(w, widgetIndex, screenCoords)` is already a public function and is already called from a non-caption widget at `EditorInventionsList.cpp:679`. The widgetIndex argument is only used for tooltip restoration on release; it doesn't have to point at a caption. The continuation path in `GameHandleInputMouse` (`MouseInput.cpp:339`) drives the window from cursor deltas without caring how the drag started.

The only missing piece is a held-shortcut hook that begins the drag on press and ends it on release.

### 1.3 Implementation

**Rev W0 — Keymap rework (Miyoo gamepad → accessible keyboard keys).** Prerequisite for both this feature and any future cross-platform shortcut testing. The cut-59 design dumped the device's face X/Y and L2/R2 onto F14-F17 because those F-keys "are basically never used by humans"; the cost is that PC keyboards rarely have those keys, making host testing hard. Move all device buttons onto common WASD-cluster letters with modifier dual-emission for the shoulder buttons.

Target mapping:

| Device | Current scancode | New scancode | Notes |
|---|---|---|---|
| D-pad UP/DOWN/LEFT/RIGHT | arrow keys | (unchanged) | already accessible |
| Face A | Z | Z | unchanged |
| Face B | X | X | unchanged |
| Face X | F16 | **C** | reverts to pre-cut-59 |
| Face Y | F17 | **V** | reverts to pre-cut-59 |
| L1 | LSHIFT | **Q + LSHIFT** | dual-emit, see below |
| R1 | LALT | **A + LALT** | dual-emit, see below |
| L1+R1 chord | LCTRL | **LCTRL** (unchanged chord swap) | suppresses LSHIFT+LALT |
| L2 | F14 | **W** | |
| R2 | F15 | **S** | |
| Start | RETURN | RETURN | unchanged (= ENTER) |
| Select | ESCAPE | ESCAPE | unchanged — ESCAPE is universally accessible on PC keyboards and preserves the existing `kInterfaceCancelConstruction` semantic (cancel active tool / ride-build) |

**Why dual-emit on L1/R1.** OpenRCT2's "Shift modifier" path (raise placement Z, vertical stack on track) and OpenRCT2mini's "fast cursor / gamepad mod" path both check SDL's mod state via `KMOD_LSHIFT` / `KMOD_LALT`. Only the actual modifier keys set those flags. If L1 sends only Q and R1 only A, the modifier semantics break. The fix is to send the user-facing letter for testability AND the modifier key for behavior. The chord-swap logic (cut 60) already handles the LSHIFT/LALT → LCTRL transition when L1+R1 are both held; extend it to also suppress the dual-emitted Q/A while the chord is engaged so chord state is unambiguous.

**Files touched in W0:**

* `Packaging/miyoo_mini/build-deps.sh` — rewrite the `set_key` switch block (~10 lines of patch text). Each shoulder press now sends two scancodes; each shoulder release sends two releases. The `l1_emitted_sc` static (cut 60) tracks whether L1 was emitted as LSHIFT or LCTRL, extend with a similar `r1_emitted_sc` for R1. Also remove the dual-emit while chord is engaged (Q/A get suppressed when L1+R1 chord into LCTRL).
* `src/openrct2-ui/UiContext.cpp:255-275` — the F14/F15/F16/F17 intercepts switch to `SDL_SCANCODE_W` / `SDL_SCANCODE_S` / `SDL_SCANCODE_C` / `SDL_SCANCODE_V`. Letter keys conflict with text input; the existing intercepts already short-circuit when text input is active (`hasTextInputFocus()` check would need to be added; check whether the F-key intercept currently handles this — if not, that was latent bug masked by F-keys not being used in text). For W (rotate view CCW or zoom out with R1), S (rotate view CW or zoom in with R1), V (rotate construction object 1× or 3× with R1) the intercept stays. For C (was cycle-speed), the intercept is **removed entirely** — see W1a below.
* `src/openrct2-ui/windows/Osk.cpp:925, 963` — F16 → space mapping changes to C → space. OSK is a per-window keyboard override that takes precedence over global intercepts, so the OSK window claims C while open. Update Osk.cpp:424 comment too.
* `Packaging/miyoo_mini/package.sh:666` — CONTROLS section updates: list the new test-friendly mapping under each device button so users (and devs) know what to press on a real keyboard.
* `launch.sh` shortcuts.json wipe (cut 44b pattern) — bump the wipe sentinel so existing installs invalidate any user-customised bindings that referenced the old F-keys.

**Rev W1a — Repurpose face X (now keyboard C).** Drop the cycle-game-speed intercept; the toolbar dropdown (Cut 31) is the only path for changing speed via a button. Keyboard `-`/`=` (`kInterfaceDecreaseSpeed`/`kInterfaceIncreaseSpeed`) remain for incremental steps on host PC.

* `src/openrct2-ui/UiContext.cpp` — the `case SDL_SCANCODE_C:` (formerly F16) branch is simply not added during W0's intercept rewrite. Face X now passes through to the shortcut system.
* `Packaging/miyoo_mini/package.sh` CONTROLS line for face X (already touched in W0): `X (C)        Drag window under cursor`.

**Rev W1b — Register the window-drag shortcut with C as default:**

* `src/openrct2-ui/input/ShortcutIds.h` — add `kInterfaceWindowDrag = "interface.general.window_drag"`.
* `src/openrct2-ui/input/Shortcuts.cpp` — register with default binding `"C"` and an empty lambda (we poll held-state instead). Pattern matches `kViewScrollUp`'s empty-lambda registration. Example:
  ```cpp
  registerShortcut(ShortcutId::kInterfaceWindowDrag, STR_SHORTCUT_DRAG_WINDOW, "C", []() {});
  ```
  Users can rebind via Options > Controls > Keyboard Shortcuts. Both device and host see the same default.
* `data/language/en-GB.txt` + the 14 other shipped languages — add `STR_SHORTCUT_DRAG_WINDOW` "Drag window under cursor".

**Rev W2 — Hook held-state polling** in `InputManager::processHoldEvents` (or a sibling). On rising edge:

1. Bail if `_inputState` is not `Normal` or `Reset` (prevents stomping on resize, scroll, dropdown, viewport drag, widget-press states).
2. Bail if there's an active text-input session (`hasTextInputFocus()`).
3. Resolve `WindowBase* w = windowMgr->FindFromPoint(ContextGetCursorPosition())`.
4. Bail if `w == nullptr` or `w->flags.hasAny(WindowFlag::stickToBack, WindowFlag::stickToFront)` or `w->flags.has(WindowFlag::noBackground)` — that excludes the top/bottom toolbars and the title-screen viewport. (`FlagHolder::has` takes a single flag; multi-flag tests use `hasAny`/`hasAll`.)
5. `windowMgr->BringToFront(*w)` so the dragged window comes up in z-order, matching title-bar behavior.
6. Call `InputWindowPositionBegin(*w, 0, ContextGetCursorPosition())` — index 0 is the frame widget, harmless for tooltip restoration.

Continuing: nothing to do. The existing `InputState::PositioningWindow` branch in `GameHandleInputMouse` runs every frame the cursor moves.

On falling edge:

1. If `_inputState == InputState::PositioningWindow`, look up the window via `_dragWidget.windowClassification/.windowNumber` (same as the existing `MouseState::leftRelease` branch at `MouseInput.cpp:348`).
2. If found, call `InputWindowPositionEnd(*w, cursor)`. If gone, just clear `_inputState = InputState::Normal`.

### 1.4 Edge cases

* User triggers the shortcut while a different drag/scroll is active: skipped via the `_inputState` guard.
* Window closes mid-drag: `InputState::PositioningWindow` continuation already handles this (`MouseInput.cpp:341` sets state to Reset on null).
* User binds the shortcut to the same key as the left mouse: shortcut polling and mouse-button polling are independent. Edge cases are rare on a handheld with no mouse, but on PC the drag will start and a mouse-up ends it normally.
* Cursor over toolbar: excluded via `noBackground` flag. The main viewport is excluded via `stickToBack`.
* `WindowSnap` proximity: respected. The continuation calls `WindowMoveAndSnap` with the user's configured proximity (`Config::Get().general.windowSnapProximity`).

### 1.5 Test plan

Host build (after W0 keymap rework, before W2):

* Press W, S — should rotate view CCW / CW (replacing F14/F15 behavior).
* Press SHIFT+W, SHIFT+S — wait, these are LSHIFT+W which is L1+L2 combined. Shouldn't fire anything special (L1 modifier doesn't modify L2 alone). Skip this case.
* Press ALT+W, ALT+S — should zoom out / zoom in (R1+L2/R2 chord on device).
* Press V — should rotate construction object CW; ALT+V → 3× (CCW direction).
* Press C — should NOT cycle game speed (intercept removed). Should fire window-drag shortcut once W2 is in.
* Verify keyboard `-` and `=` still decrement/increment speed (those shortcut bindings are untouched).
* Verify SHIFT held during ride placement still raises Z (L1 modifier semantics intact via L1's LSHIFT dual-emission — but on host PC the dev presses LSHIFT directly, which works because that's what OpenRCT2 was always checking).
* Verify ALT held still triggers fast-cursor / gamepad-mod path (R1's LALT dual-emission, same reasoning).
* SHIFT+ALT chord: on device this is L1+R1 → LCTRL via cut 60. On host PC dev presses LCTRL directly. Confirm Ctrl-modifier (placement Z lock) still works.

Host build (after W2 — drag shortcut active):

* Open Park, Map, Finance windows. Hover over each body, hold C, move cursor. Window should follow. Release — window stays.
* Repeat over a tab strip (Park window), over a viewport widget (Park entrance).
* Open dropdown, hold C — should be ignored.
* Begin a normal title-bar drag with mouse, then press C — should remain the existing drag, not double-trigger.
* During text-input (rename a peep, press C) — ignored. Sanity-check: a typed C in the textbox should appear, not trigger the shortcut.
* C inside the OSK — should type a space (OSK's per-window override survives the F16→C migration).

Device build (after W0 + W1b + W2):

* Same window-drag scenarios via the X face button.
* Confirm L1 still raises placement Z during ride construction (LSHIFT dual-emit working).
* Confirm R1 still triggers fast cursor (LALT dual-emit working).
* Confirm L1+R1 chord still locks placement Z onto a clicked element (LCTRL chord-swap working).
* Confirm L2/R2 still rotate view (with R1 chord = zoom).
* Confirm Y face still rotates construction (with R1 chord = 3×).
* Confirm Start opens the in-game menu (RETURN).
* Confirm Select still cancels active construction / dismisses (ESCAPE → kInterfaceCancelConstruction, unchanged from today).
* Open the toolbar speed dropdown explicitly to confirm speed changes still possible without a dedicated button.
* Open the OSK in a textbox; press X face — should type a space.

### 1.6 Files touched

W0 (keymap rework):
* `Packaging/miyoo_mini/build-deps.sh` — rewrite set_key switch (~30 lines) with dual-emit for L1/R1 and chord suppression
* `src/openrct2-ui/UiContext.cpp` — rewire F14/F15/F17 intercepts to W/S/V scancodes; remove F16 case entirely (~25 lines net)
* `src/openrct2-ui/windows/Osk.cpp` — F16→C in two switch cases + comment (~3 lines)
* `Packaging/miyoo_mini/package.sh` — CONTROLS section: annotate each device button with its host keyboard equivalent
* `launch.sh` — bump shortcuts.json wipe sentinel (1 line)

W1a (drop cycle-speed): folded into W0's UiContext rewrite (the C case just doesn't get added back).

W1b (drag shortcut registration):
* `src/openrct2-ui/input/ShortcutIds.h` — 1 line (new shortcut id)
* `src/openrct2-ui/input/Shortcuts.cpp` — 1 line registration with C default
* `src/openrct2-ui/UiStringIds.h` — 1 string id
* `data/language/*.txt` — 1 string each in 15 files

W2 (held-state poll):
* `src/openrct2-ui/input/InputManager.h` — track previous-frame held state for edge detection
* `src/openrct2-ui/input/InputManager.cpp` — ~30 lines for poll + begin/end calls

## 2. Feature B — Window shade

### 2.1 Goal

Title-barred windows gain a "shade" button on the side opposite the close button. Clicking it collapses the body to title-bar height; clicking again restores the original height. While shaded:

* Tabs, body widgets, scroll regions, and the resize handle are hidden and not hit-testable.
* Game state held by the window (selected ride, active tool, current scenery, selected tab, viewport focus, etc.) is preserved. The window's `onClose` does NOT run.
* The window is still draggable and the close button still closes.

### 2.2 Design decision: track shade with a new widget flag

Two viable approaches:

* **(A)** Mutate `WidgetFlag::isHidden` on body widgets when shading; restore on un-shading.
* **(B)** Add `WidgetFlag::shadeHidden = 7` (one of the two free bits). Both `isHidden` and `shadeHidden` gate `Widget::isVisible()`. Shade only ever sets `shadeHidden`; the window's own `setWidgetHidden` calls in `onPrepareDraw` continue to work on `isHidden` independently.

Pick **(B)**. Rationale:

* Most windows rebuild widget visibility every `onPrepareDraw` based on tab/page state. Mixing shade-imposed and window-imposed hidden state on the same flag means we'd lose the per-window decisions when un-shading.
* The flag is one bit; cheap.
* `isVisible()` is the single chokepoint for both rendering and hit-testing.

### 2.3 The shade widget

Add `WidgetType::shadeBox` (or reuse `closeBox` with a content-string distinguisher; new type is cleaner). It draws like the close box but with a glyph that toggles between expanded (▾) and shaded (▴) states.

**No new graphic asset needed.** The close box uses Unicode glyphs through the bitmap font (`u8"{BLACK}❌"` for `kCloseBoxStringBlackNormal`). The same path is available for shade — confirmed in `src/openrct2/core/UnicodeChar.h:206-210`:

* `UnicodeChar::up = 0x25B2` (▲, large up-pointing triangle)
* `UnicodeChar::small_up = 0x25B4` (▴, small up-pointing triangle)
* `UnicodeChar::down = 0x25BC` (▼)
* `UnicodeChar::small_down = 0x25BE` (▾)

All four are explicitly registered in the sprite-font glyph map at `src/openrct2/drawing/Font.cpp:228-233`, mapped to existing CSChar sprite slots. They render in the same path as `❌` and don't require any g2.dat work.

Mirror the existing close-box constants:

```cpp
constexpr const char* kShadeBoxStringExpandBlackNormal = u8"{BLACK}▾";
constexpr const char* kShadeBoxStringExpandWhiteNormal = u8"{WHITE}▾";
constexpr const char* kShadeBoxStringExpandBlackLarge  = u8"{BLACK}▼";
constexpr const char* kShadeBoxStringExpandWhiteLarge  = u8"{WHITE}▼";
constexpr const char* kShadeBoxStringCollapseBlackNormal = u8"{BLACK}▴";
constexpr const char* kShadeBoxStringCollapseWhiteNormal = u8"{WHITE}▴";
constexpr const char* kShadeBoxStringCollapseBlackLarge  = u8"{BLACK}▲";
constexpr const char* kShadeBoxStringCollapseWhiteLarge  = u8"{WHITE}▲";
```

`repositionCloseButton` (or its successor) sets the appropriate string on the shade widget based on `isShaded` and `enlargedUi`/`translucent`, the same way it already chooses the close-box string.

Other options considered and rejected:

* `SPR_G2_ARROW_UP` / `SPR_G2_ARROW_DOWN`: exist (resources/g2/icons/arrow_up.png and arrow_down.png), but they're 11×11 bright-red sprites built for `flatBtn` priority controls in AssetPacks. Wrong colour, wrong style for the close-button slot.
* New PNGs in resources/g2/icons/ + sprites.json + new `SPR_G2_*` constants: works but requires regenerating g2.dat; unnecessary given the font already has the glyphs.
* ASCII underscore-style minimize button (`_`/`‾`): Win9x-feel, but `‾` (overline U+203E) isn't in the registered glyph set — would need to fall through to TTF if compiled in, otherwise miss-glyph squares. Skip.

Position: the inverse side of the close box. `repositionCloseButton` already places the close box based on `windowButtonsOnTheLeft`. Extend that helper (or split into `repositionTitleButtons`) so:

* If `windowButtonsOnTheLeft`: close is at left edge, shade is `2 + closeButtonSize + 2` to the right of close.
* Else: close is at right edge, shade is `closeButtonSize + 2` to the left of close.

Both buttons use the same `kCloseButtonSize` / `kCloseButtonSizeTouch` (enlargedUi-aware) sizing.

Title-text width: `WidgetCaptionDraw` (`Widget.cpp:575-582`) currently subtracts `kCloseButtonSize` once or twice for adjacent close boxes. Extend the loop to also subtract for `shadeBox`. The shadeBox needs to be positioned adjacent to (or at index 3 just after) the close box for the existing trim logic to find it; document this layout invariant.

### 2.4 Where the shade widget gets inserted

Two options:

* **(B1)** Every standard window's `makeWindowShim()` returns 3 widgets (frame, caption, close). Extend it to return 4 when shade is enabled — but the indices in every window's `WIDX_*` enum start at `WIDX_BACKGROUND=0, WIDX_TITLE=1, WIDX_CLOSE=2, WIDX_PAGE_BACKGROUND=3, ...`. Inserting a widget at index 3 shifts every index in every window — unacceptable.
* **(B2)** Insert at index 2 *if* close box is at index 2, but place visually next to it. Doesn't help — same shifting problem.
* **(B3)** Append a virtual shade widget at the end of the array inside `resizeFrame()` when the window is shade-eligible. No fixed index. Drawing and hit-test loops walk all widgets, so position in the array doesn't matter. The caption text-trim loop needs to scan for shadeBox anywhere in the array, not just adjacent.

Pick **(B3)**. Implementation: `resizeFrame()` checks if the window has caption + closeBox prefix and lacks a shadeBox at the array tail. If so, push a `Widget` of type `shadeBox` onto `widgets`. Position it via the modified `repositionCloseButton`. This makes shading opt-in only for windows that pass the prefix check, which is what we want — chromeless windows (toolbars, dropdowns, tooltips, OSK) skip it automatically.

Caveat: `setWidgets()` clears and re-inserts from the static template, then calls `resizeFrame()`. So the appended shade widget is rebuilt every time, which is fine — `resizeFrame()` handles the rebuild every call.

### 2.5 The toggle

Add to `WindowBase`:

```cpp
bool isShaded = false;
int16_t shadeRestoreHeight = 0;
int16_t shadeRestoreMinHeight = 0;
int16_t shadeRestoreMaxHeight = 0;

void toggleShade();
```

`toggleShade()` flips `isShaded`. On shade, save the three height fields. On un-shade, restore them.

The actual widget hiding and height collapse happens inside `resizeFrame()`. After the existing layout work, if `isShaded`:

* For every widget whose type is NOT `frame`, `caption`, `closeBox`, or `shadeBox`: set `WidgetFlag::shadeHidden`. Done by widget type rather than index because the shadeBox is appended at the array tail (§2.4) and the array length varies per window.
* Set `height = getTitleBarCurrentHeight() + 1`.
* Set `minHeight = maxHeight = height`.
* Set frame widget's `bottom = height - 1`.
* The shadeBox's own position via `repositionCloseButton` handles the new height.

If not `isShaded` and we previously were: clear `shadeHidden` from all widgets. Height fields are restored by the toggle path before `resizeFrame` runs.

### 2.6 The click hook

In `InputStateWidgetPressed` at the `MouseState::leftRelease` branch, just before the `w->onMouseUp(widgetIndex)` call at `MouseInput.cpp:1424`:

```cpp
if (w->widgets[widgetIndex].type == WidgetType::shadeBox) {
    w->toggleShade();
    return;
}
```

Same pattern as the closeBox modifier-tracking already done at `MouseInput.cpp:1256`. No per-window code needed because shade is purely framework state.

### 2.7 Tab and tool interaction

A common worry is that hiding tab buttons while shaded loses the selected tab. It doesn't:

* `selectedTab` is a member of `WindowBase` (`WindowBase.h:110`), updated only by per-window tab-click handlers. Hiding the tab buttons just means they can't be clicked while shaded — by design.
* Active tool tracking lives in `gCurrentToolWidget` and `gCurrentToolId` (`Window.h:276`). Independent of widget visibility. Shading doesn't run `ToolCancel`.
* Selected scenery, ride construction state, current peep being viewed: all live in window members or globals. None of these touch widget flags.
* Ride construction's `onClose` (`RideConstruction.cpp:296`) is what runs `ToolCancel`, removes ghosts, and clears map-select flags. Shading does NOT close the window — `onClose` does not fire.

### 2.8 Edge cases

* **Resizable windows**: while shaded, `minHeight == maxHeight == titleBar+1`. The resize-handle widget at the bottom-right is in `widgets[]` somewhere and gets `shadeHidden`. Hit-test for resize at `MouseInput.cpp:1071` checks the bottom-right 19×19 area of the window — when height shrinks, that area collapses into the title bar. Add a guard: don't enter `InputWindowResizeBegin` if `w->isShaded`. One-line check.
* **Viewport widgets**: ride/peep/staff windows have a viewport widget in their body. Hiding it via `shadeHidden` skips drawing (`Window.cpp:1052` checks visibility). The viewport itself is not deallocated; on un-shade it draws again. The viewport's per-tick update still runs (it's driven by `WindowDispatchUpdateAll`, not draw).
* **Cursor over a shaded window**: hit-test gets the title bar widgets, viewport beneath gets clicks back since the window's bbox shrunk. Confirm via `FindFromPoint` (`WindowManager.cpp:1195`) which uses `width`/`height` — these are correctly reduced.
* **Shade persistence**: don't persist across sessions or even across window-close/reopen. `WindowBase::isShaded` defaults to false; new windows always open expanded.
* **Multiple windows shaded**: each has independent `isShaded` state — fine. Stacking many shaded windows produces a column of title bars, which is the desired UX.
* **Button-side swap mid-game**: if user toggles `windowButtonsOnTheLeft` in Options, every window's `resizeFrame` runs on the next invalidate (Options.cpp invalidates after toggling). The shade button repositions automatically.
* **Non-standard close-box placements**: the EditorInventionsList window calls `InputWindowPositionBegin(*this, 0, gTooltipCursor)` from a body widget — it still has the standard prefix. Spot-check by grepping for windows whose `widgets[2]` isn't a closeBox; if any exist, they simply don't get a shade button (the prefix check fails).
* **Cursor type over title bar of shaded resizable window**: cosmetic. The existing cursor logic returns the resize cursor when in the bottom-right corner. With the resize-begin guarded by `isShaded`, the click is a no-op but the cursor sprite still flashes. Acceptable; can patch later by adding a `isShaded` check in `onCursor` virtual.

### 2.9 Implementation sequence

* **Rev W3:** `WidgetType::shadeBox` enum + draw path that mirrors closeBox draw. Just rendering, no behavior. Append to the rendering switch in `Widget.cpp:115`.
* **Rev W4:** `WidgetFlag::shadeHidden = 7`. Modify `Widget::isVisible()` to gate on both flags.
* **Rev W5:** `WindowBase::isShaded`, `shadeRestoreHeight/Min/Max`, and `toggleShade()`. Modify `resizeFrame()` to append shadeBox widget when prefix matches, and to apply collapse/expand logic at end of layout. Modify `repositionCloseButton` (or split it) to position both buttons.
* **Rev W6:** Hook `InputStateWidgetPressed` to call `toggleShade` for shadeBox left-release. Guard `InputWindowResizeBegin` against `isShaded`.
* **Rev W7:** Extend `WidgetCaptionDraw` text-width loop (`Widget.cpp:575`) to scan the full widget array for a shadeBox and subtract another `kCloseButtonSize`.
* **Rev W8:** Tooltip string for the shade button (`STR_SHADE_WINDOW_TIP`) in en-GB and the 14 other languages. (No glyph assets needed — see §2.3.)

### 2.10 Test plan

Host build:

* Open Park window, click shade, click un-shade. Geometry restores exactly.
* Open Park, switch to Awards tab. Shade. Un-shade. Awards tab is still selected.
* Open Ride Construction, start placing track (tool active). Shade. Un-shade. Tool is still active, ghosts intact.
* Open Scenery, select an item. Shade. Un-shade. Selection preserved.
* Open Map (resizable). Resize to 400×300. Shade. Un-shade. Window is back at 400×300.
* Open Map. Shade. Try to resize via bottom-right corner. Should be blocked.
* Toggle `windowButtonsOnTheLeft` in Options while a window is shaded. Buttons should swap sides.
* Drag a shaded window via title bar. Works. Drag via Feature A held shortcut. Works.
* Cursor click on the area where the body used to be (now empty space) goes to the viewport beneath.
* Open the Options window (which calls `setPage` → `setWidgets` on tab change). Shade. Click a tab — but tabs are hidden, so click goes through. Un-shade — Options is still on the previously-selected tab.
* Repeat key tests on device build with enlargedUi enabled.

### 2.11 Files touched

* `src/openrct2/interface/Widget.h` — `shadeBox` enum, `shadeHidden` flag, `isVisible()` update
* `src/openrct2-ui/interface/Widget.cpp` — shadeBox draw path, caption text-width fix
* `src/openrct2/interface/WindowBase.h` — `isShaded`, restore fields, `toggleShade()` decl
* `src/openrct2/interface/WindowBase.cpp` — `toggleShade()` impl, `resizeFrame()` extension, `repositionCloseButton` (or split) for both buttons
* `src/openrct2-ui/input/MouseInput.cpp` — shadeBox click intercept, resize-begin guard
* `src/openrct2-ui/UiStringIds.h` — `STR_SHADE_WINDOW_TIP`
* `data/language/*.txt` — 1 string each in 15 files

## 3. Sequencing & risk

* **W0 (keymap rework)** is a prerequisite that delivers value beyond this plan — every future feature gets host-testable. Medium risk: touches the SDL2 patch, the UiContext intercept layer, and the OSK. The test plan in §1.5 covers it. Ship first.
* **Feature A (W1a + W1b + W2)** is shortcut + held-state polling on top of W0. Low risk once W0 is in.
* **Feature B (W3-W8)** touches the framework but keeps changes localized to `resizeFrame`, `Widget::isVisible`, and one mouse-input hook. Medium risk; the test plan in §2.10 catches the obvious failure modes.

Order: W0 → W1a → W1b → W2 → W3-W8. Each ships as its own rev with host build first, then ARM dist for device verification.

## 4. Open questions

* **Glyph choice for shadeBox** is resolved (see §2.3): use the existing UnicodeChar `small_down`/`small_up` (and `down`/`up` for enlarged UI), which are already in the OpenRCT2 sprite font.
* **Letter-key intercepts and text input.** UiContext's W/S/V intercepts run before any per-window key dispatch. Letter keys could collide with text input (typing "W" in a peep-rename dialog). The current F-key intercepts don't have this problem because nothing types F14-F17. The fix is to add a `hasTextInputFocus()` guard at the top of `processKeyboardSpecialKeys` (or wherever the intercept lives). Confirm this guard exists in the current code path; add it if not.

## 5. Sanity-check pass

Re-read of plan after first draft surfaced these concerns; resolutions noted inline above (and small corrections folded in):

* **`flags.has(stickToBack | stickToFront)` is wrong syntax**: `FlagHolder::has` takes a single flag value, not a bitmask. Corrected §1.3 to use `flags.hasAny(WindowFlag::stickToBack, WindowFlag::stickToFront)` — the same idiom already used in `WindowManager.cpp:885`.
* **Hiding body widgets by index is fragile**: the appended shadeBox makes index-based exemption ("widgets[3..end]") miss its own button. Corrected §2.5 to exempt by widget *type* (frame/caption/closeBox/shadeBox).
* **`InputState` enum location**: confirmed at `Input.h:34`, with `Reset, Normal, WidgetPressed, PositioningWindow, ViewportRight, DropdownActive, ViewportLeft, ScrollLeft, Resizing, ScrollRight`. The "guard against starting drag in a non-Normal state" check in §1.3 is straightforward — explicitly compare against `Reset` and `Normal`.
* **`_inputState` is a global `extern InputState _inputState;` in `Input.h:86`**: accessible from any TU, not file-local — fine to test from InputManager.cpp.
* **`BringToFront` signature confirmed**: `WindowBase* BringToFront(WindowBase& w)` on `IWindowManager` (`WindowManager.h:116`). Returns the (possibly-moved-in-list) window pointer.
* **Current keymap state confirmed**: `Packaging/miyoo_mini/build-deps.sh:347-435` is the SDL2 patch driving device → SDL scancode translation. Face X = F16, Face Y = F17, L2 = F14, R2 = F15, L1 = LSHIFT, R1 = LALT, L1+R1 = LCTRL (chord swap), Start = RETURN, Select = ESCAPE. UiContext at `UiContext.cpp:255-275` intercepts F14-F17 specifically to run gamepad-mod-aware special handling that bypasses the shortcut system.
* **Modifier-preservation is real**: cuts 58, 60, 61 explicitly chose LSHIFT/LALT/LCTRL for L1/R1/chord because OpenRCT2's modifier checks key SDL's `KMOD_*` mask, which only the actual modifier scancodes set. A naïve "L1 → Q only" rewrite breaks every Shift-modifier path. Dual-emission (Q + LSHIFT, A + LALT) preserves modifier semantics while exposing the test-friendly letter to host devs.
* **OSK F16=space binding migrates with the keymap**: `Osk.cpp:925, 963` use F16 for the spacebar inside the on-screen keyboard. With X face → C, those become SDL_SCANCODE_C. Per-window key dispatch fires before global shortcut polling, so when the OSK window is active, C types a space; with OSK closed, C drives the window-drag shortcut.
* **Cycle-speed UX preservation**: removed entirely from face X. Toolbar dropdown (Cut 31, with Pause folded in) is the only on-screen path. Keyboard `-`/`=` (`ShortcutReduceGameSpeed`/`ShortcutIncreaseGameSpeed` in `Shortcuts.cpp:799-800`) remain for incremental host PC steps.

Other items already covered:

* **`resizeFrame` clobbering height**: original concern. Resolved by §2.5 — `resizeFrame` itself applies the shade collapse, so any rebuild path lands at the same final height.
* **Tab indices shifting if we insert a shadeBox at index 3**: real concern; every window has `WIDX_*` constants starting at 0,1,2,3 for frame/caption/close/page. Resolved in §2.4 by appending shadeBox at the array tail instead of inserting.
* **`setWidgets` clearing the appended shadeBox**: yes, every `setWidgets` call clears `widgets` and reinserts from the template. The shadeBox is reappended by the next `resizeFrame()` call (which `setWidgets` invokes). Net effect: shadeBox is rebuilt every tab/page change, no stale state.
* **`shadeHidden` flag bit availability**: confirmed, `WidgetFlag` is `uint8_t` with bits 0,2,3,4,5,6 used, bits 1 and 7 free.
* **`onMouseUp` per-window WIDX_CLOSE handlers**: irrelevant. shadeBox click is intercepted before dispatch (§2.6), windows never see it.
* **OSK and console windows**: the OSK has no caption or close box (it's a custom layout), so the prefix check in §2.4 fails and no shade button appears. Console window — needs a quick check; if it has the caption+close prefix it'll get a shade button, which is fine.
* **Tooltips/dropdowns**: ephemeral, no caption/close, not affected.
* **Modal save/load dialogs**: have caption + close. They'll get a shade button. Shading a modal save dialog is harmless — the modal is still active, just collapsed. Could exclude via a new `WindowFlag::noShade` if it bothers anyone, but no need to over-engineer.
* **Drag-window hotkey racing with the shade click**: drag-hotkey rising edge runs in `processHoldEvents`, mouse click runs in `GameHandleInputMouse`. They're processed in the same frame but the drag's `_inputState` guard ensures we don't begin a drag if a widget-pressed state is already active from a click in progress. Safe.
* **`WindowGetMain`/main viewport**: has `stickToBack`. Excluded from drag-hotkey by the §1.3 step 4 guard. Has no caption either, so excluded from shade.
* **`BringToFront` skipping `stickToFront`**: confirmed via existing call sites. The drag-hotkey doesn't need to special-case sticky windows because the FindFromPoint guard already excludes them.
* **Per-frame cost of polling the drag shortcut**: the held-shortcut poll is one bool check per frame, same as the existing scroll-key polls. Negligible.
* **Save/load compatibility**: neither feature touches any persistable state. `isShaded` is window-instance state, gone when the window closes.
* **Language string fallout**: the 15-language sweep is the tedious part of both features. Reuse the same translation pattern as the cursor-style strings shipped in v0.2.0.
