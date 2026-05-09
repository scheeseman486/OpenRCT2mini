# Gamepad Input Audit

**Status:** Phase 1.1 deliverable for `gamepad-plan.md`. No-code analysis of the current input architecture.
**Sources:** OpenRCT2mini revision at the time of writing (post-rev-95f bench work).

## Executive summary

Input handling today is **three parallel feeds** that converge on a set of latched flags in `UiContext`, plus a separate event-driven path through `ShortcutManager` for keyboard-style game shortcuts. Each feed exists for historical reasons and was added incrementally:

1. **Real keyboard scancodes** from host SDL2 — fully wired through `ShortcutManager`. This is the upstream-original path.
2. **Synthesised keyboard scancodes** from the Mini's vendor SDL2 — physical buttons emit fake `SDL_KEYDOWN`/`SDL_KEYUP` events with hardcoded scancodes (D-pad → arrows, A → SPACE, Q → SHIFT, A → ALT, etc.). Gets to the same code paths as feed 1.
3. **Direct `SDL_GameControllerGetButton` polling** in `UiContext::Update` — host with a real pad. Reads SDL_GameController state every frame; the resulting flags are OR'd with feeds 1 and 2 to drive the virtual cursor.

The convergence point is `UiContext::ProcessVirtualGamepadCursor()` (around line 1303), which OR-merges all three sources into local `dpadUp/Down/Left/Right`, `btnA/B`, and `fastModifier` flags every frame, then feeds them into the virtual-cursor velocity logic and into `StoreMouseInput` for click events.

`ShortcutManager` does NOT participate in any of the cursor / chord behaviors. Every cursor-related action and every chord behavior on the device (R1+X close window, X tap-vs-hold for shade-one-vs-shade-all, L1+R1 = Z-lock modifier, R1 = fast-cursor modifier) is implemented inside `UiContext` directly, on top of synthesised keyboard scancodes. The shortcut system handles only "discrete keyboard combos to game actions" (Ctrl+S = screenshot, etc.).

The plan's job is to consolidate all three feeds into one canonical SDL_GameController-based path, run through an extended `ShortcutManager` that gains held-button-set tracking for chords, and delete the entire `_vKb*` block once each behavior has a shortcut equivalent.

## Architecture diagram

```
                     ┌─────────────────────────────────────┐
                     │            HOST                      │
                     └─────────────────────────────────────┘

          Real keyboard           Real Xbox/PS pad
                │                         │
                │                         ├─── SDL_CONTROLLERBUTTONDOWN/UP
                │                         │     SDL_CONTROLLERAXISMOTION
                │                         │     │
                │                         │     v
                │                         │   InputManager::queueInputEvent
                │                         │   ┌─────────────────────────────┐
                │                         │   │ joyAxis  → InputEvent queue │
                │                         │   │ joyButton→ InputEvent queue │
                │                         │   │ joyHat   → (dead for SDL_GC)│
                │                         │   └─────────────────────────────┘
                │                         │     │
                │                         │     v
                │                         │   ShortcutManager::processEvent
                │                         │   (no held-button-set; cannot
                │                         │    match chords; joyAxis kind
                │                         │    has no `matches()` arm)
                │                         │
                │                         └─── SDL_GameControllerGetAxis
                │                         │     (left stick only — direct
                │                         │      poll in processAnalogueInput
                │                         │      → _analogueScroll → viewport
                │                         │      scroll)
                │                         │
                v                         └─── SDL_GameControllerGetButton
        SDL_KEYDOWN/UP                          (direct poll in
                │                                ProcessVirtualGamepadCursor)
                │                                  │
                v                                  v
        InterceptVirtualCursorKey                  │
        (UiContext.cpp:257)                        │
                │                                  │
                ├── arrows  → _vKbUp/Down/L/R     │
                ├── Z       → _vKbZ                │
                ├── X       → _vKbX                │
                ├── Q       → _vKbShift +          │
                │             SDL mod KMOD_LSHIFT  │
                ├── A       → _vKbShift +          │
                │             _vGamepadMod         │
                ├── L/R Alt → _vKbShift +          │
                │             _vGamepadMod         │
                ├── L/R Ctrl→ _vKbCtrl             │
                │             (keep propagating)   │
                ├── L/R Shift → no swallow         │
                │              (propagate to       │
                │               shortcut system)   │
                │                                  │
                └── otherwise: pass through  ──┐  │
                                               v  v
                                  ProcessVirtualGamepadCursor
                                  (UiContext.cpp:1303)
                                               │
                                               │  OR-merge of:
                                               │  • _vKb* keyboard latches
                                               │  • Direct GC poll
                                               │  • _vGamepadMod
                                               │  → cursor velocity
                                               │  → StoreMouseInput on
                                               │    A/B button transitions
                                               │  → R1+X close-window chord
                                               │  → X tap-vs-hold for
                                               │    shade / shade-all chord
                                               v
                                       virtual cursor moves,
                                       mouse buttons fire,
                                       chord actions execute

                       (and meanwhile, the keys NOT swallowed by
                        InterceptVirtualCursorKey continue down to
                        InputManager → ShortcutManager → game action)


                     ┌─────────────────────────────────────┐
                     │            MINI                      │
                     └─────────────────────────────────────┘

          Hardware buttons (evdev /dev/input/event0)
                  │
                  v
          MMIYOO_PumpEvents (vendor SDL2)
                  │
                  ├── reads keypad bitmap
                  ├── code[] table maps each bit to a fixed scancode
                  │   UP/DOWN/LEFT/RIGHT → SDLK_UP/DOWN/LEFT/RIGHT
                  │   A → SPACE                B → LCTRL
                  │   X → LSHIFT               Y → LALT
                  │   L1 → e (also LSHIFT, see joystick driver patches)
                  │   R1 → t (also LALT)       L2 → TAB    R2 → BACKSPACE
                  │   START → RETURN           SELECT → RCTRL
                  │   MENU → HOME
                  └── synthesises SDL_KEYDOWN / SDL_KEYUP via
                      SDL_SendKeyboardKey
                  │
                  v
          Same SDL_KEYDOWN path as host → InterceptVirtualCursorKey
          → _vKb* / _vGamepadMod / _vKbCtrl → ProcessVirtualGamepadCursor

          The vendored SDL_joystick_mmiyoo.c is registered but its
          Open / Update / GetGamepadMapping are stubs — produces no
          SDL_CONTROLLER* events. SDL_IsGameController(0) returns
          SDL_FALSE on Mini.
```

## 1. SDL event entry points (UiContext::ProcessMessages)

Walking `UiContext.cpp::ProcessMessages` (around line 738):

| SDL event | Handler arm | What it does |
|---|---|---|
| `SDL_QUIT` | `ContextQuit()` | Calls `gFinish = true` |
| `SDL_WINDOWEVENT` | resize / move / focus changes | Updates `_width`, `_height`, default-display config, audio focus volume |
| `SDL_MOUSEMOTION` | sets `_cursorState.position` | scaled by `windowScale` |
| `SDL_MOUSEWHEEL` | console scroll OR `_cursorState.wheel` | early-return when `_inGameConsole.IsOpen()` |
| `SDL_MOUSEBUTTONDOWN` | StoreMouseInput + InputEvent enqueue | three buttons mapped to `MouseState::leftPress` etc.; queues an `InputEvent{deviceKind=mouse}` for ShortcutManager |
| `SDL_MOUSEBUTTONUP` | symmetric to down | |
| `SDL_FINGERMOTION/DOWN/UP` | non-macOS | Touch → mouse single/double-tap heuristic |
| `SDL_KEYDOWN` | calls `_textComposition.HandleMessage` then `InputManager::queueInputEvent` | InterceptVirtualCursorKey runs **before** anything else and may swallow the event |
| `SDL_KEYUP` | symmetric | |
| `SDL_TEXTINPUT/EDITING` | `_textComposition.HandleMessage` | for chat / OSK / textinput widgets |
| `SDL_DROPFILE` | drag-and-drop park load | |
| `SDL_CONTROLLERBUTTONDOWN/UP` | falls into default (no specific arm in ProcessMessages) | flows to InputManager::queueInputEvent through the generic dispatch — see §2 |
| `SDL_CONTROLLERAXISMOTION` | same | |
| `SDL_CONTROLLERDEVICEADDED/REMOVED` | same | InputManager refreshes its enumeration on next `checkJoysticks()` tick |

**Notable** — `SDL_KEYDOWN` is the entry point for both real keyboard scancodes AND the Mini's vendor-SDL2 synthesised scancodes. They're indistinguishable at this layer.

`InterceptVirtualCursorKey` (lines 257-401) is called as the very first thing for every SDL_KEYDOWN/UP. If it returns true, the event is swallowed entirely and never reaches `_textComposition` or `InputManager`. This is how arrow keys can drive the cursor without also panning the camera.

## 2. InputManager: event queue + analog poll

`src/openrct2-ui/input/InputManager.cpp`:

### `queueInputEvent(const SDL_Event&)` (line 40)

Six event types arrive from `UiContext::ProcessMessages` via the generic SDL dispatch:

| SDL event | Translates to | Notes |
|---|---|---|
| `SDL_CONTROLLERAXISMOTION` (axes 0-3 only) | `InputEvent{kind=joyAxis, button=axis_idx, axisValue=value, state=down}` | Ignores triggers (axes 4-5). One-shot DOWN, no UP. |
| `SDL_JOYHATMOTION` (non-centred) | `InputEvent{kind=joyHat, button=hat_value, state=down}` | Dead for SDL_GameController inputs |
| `SDL_CONTROLLERBUTTONDOWN` / `SDL_JOYBUTTONDOWN` | `InputEvent{kind=joyButton, button=cbutton.button, state=down}` | Modifiers field set to `SDL_GetModState()` (KEYBOARD modifiers, not gamepad held-buttons) |
| `SDL_CONTROLLERBUTTONUP` / `SDL_JOYBUTTONUP` | `InputEvent{kind=joyButton, ..., state=release}` | |
| `SDL_CONTROLLERDEVICEADDED/REMOVED` / joystick equivalents | resets `_lastJoystickCheck = 0` | forces a re-enumeration on next `checkJoysticks()` |

### `checkJoysticks()` (line 115)

Called every frame from `process()`. Throttled to 5-second intervals — only re-enumerates if `_lastJoystickCheck` is older than 5000 ms or has been forcibly cleared by a device add/remove. Walks `SDL_NumJoysticks()`, opens each via `SDL_GameControllerOpen` if `SDL_IsGameController` returns true, stores in `_gameControllers`.

### `processAnalogueInput()` (line 140)

Direct-poll, not event-driven. Each frame:

- Reads `SDL_CONTROLLER_AXIS_LEFTX/LEFTY` from each enumerated controller via `SDL_GameControllerGetAxis`.
- Computes magnitude, applies `Config::Get().general.gamepadDeadzone` (default 8000 of 32767) and `gamepadSensitivity` (default 1.0, quadratic-curve).
- Accumulates into `_analogueScrollAccumX/Y` (sub-pixel float), extracts integer pixel delta into `_analogueScroll`.

**Important:** the right stick (RIGHTX/RIGHTY) is **not read** by this function. It is, however, queued as joyAxis events into the InputEvent queue (see queueInputEvent above). Those queued events go into ShortcutManager and match nothing — `ShortcutInput::matches` does not handle `joyAxis` kind.

### `handleViewScrolling()` (line 210)

Three scroll sources, each with its own conditions:

1. **Gamepad analog scroll** (`_analogueScroll`): only fires if not on title sequence and console not open. Targets the viewport under the **cursor** (via `ViewportFindFromPoint`), with main-window fallback. Calls `WindowUnfollowSprite` if scrolling main / viewport-class window. Ignored otherwise — including ride-following windows, which keep tracking their vehicle.
2. **Keyboard scroll** (`_viewScroll - _analogueScroll`): edge-based, applied to main window, unfollows main-window-sprite first.
3. **Mouse edge scroll**: only when `Config::general.edgeScrolling` is on, no modifiers held, normal input state.

### `process(const InputEvent&)` (line 326)

Delegates to either:
- **Console**, when `_inGameConsole.IsOpen()`: only `ShortcutId::kDebugToggleConsole` is checked; everything else routed to `processInGameConsole`. **Note:** this gating only triggers for `deviceKind == keyboard`. Gamepad InputEvents fall through to `shortcutManager.processEvent(e)` even with the console open.
- **Chat**, when `gChatOpen`: keyboard-only routing to `processChat`.
- **TextInput / LoadSave / OverwritePrompt windows**: keyboard-only routing.
- **WidgetTextBox active**: keyboard-only swallow.
- **Fallback**: `shortcutManager.processEvent(e)` — fires matching shortcut.

**Bug-relevant:** the keyboard-only gating means gamepad button presses can fire shortcuts while the console is open, which is wrong. This is captured under Plan 1.10 (context gating policy).

### Other paths

- `_inputManager.process()` is called once per frame by `UiContext::Tick()`. Processing order: `checkJoysticks → processAnalogueInput → handleModifiers → processEvents → processHoldEvents → handleViewScrolling`.
- `processHoldEvents()` re-fires shortcuts every N ms while their binding is held — used for cursor direction, hold-to-rotate-construction, etc. Currently only consults keyboard state via `SDL_GetKeyboardState()`; gamepad hold-fire is via `processAnalogueInput`'s direct stick poll, not this function.

## 3. ShortcutManager and ShortcutInput

`src/openrct2-ui/input/ShortcutManager.{h,cpp}` and `ShortcutInput.cpp`:

### Data model

Each shortcut is a `RegisteredShortcut`:

```cpp
class RegisteredShortcut {
    std::string id;            // e.g. "shortcut.openpark_window"
    StringId localisedName;    // STR_SHORTCUT_OPEN_PARK
    std::vector<ShortcutInput> standard;  // default bindings
    std::vector<ShortcutInput> current;   // after user rebinds
    std::function<void()> action;
};
```

A `ShortcutInput` is:

```cpp
struct ShortcutInput {
    InputDeviceKind kind;      // keyboard | mouse | joyButton | joyHat
    uint32_t modifiers;        // KMOD_* bitmask (KEYBOARD ONLY)
    uint32_t button;           // SDLK_* | mouse btn | joystick btn | hat dir
};
```

**Crucial limitations:**

- `modifiers` is only ever set from `SDL_GetModState()` (keyboard modifier-keys). There is no gamepad-side held-button-set.
- `matches(const InputEvent& e)` (line 400) does an exact compare: `CompareModifiers(modifiers, e.modifiers) && e.deviceKind == kind && button == e.button`. Modifier match is **exact** — `Ctrl+P` does NOT also match `P` unless Ctrl is not held. There is no largest-subset matching.
- `joyAxis` device kind has no arm in `matches()`. Trigger-axis events flow into the queue from `InputManager` but match nothing. Right-stick axis events likewise unused.
- `joyHat` is dead for SDL_GameController hardware. SDL_GameController emits `BUTTON_DPAD_*` (joyButton kind), not hat motion. The `JOY UP/DOWN/LEFT/RIGHT` parser tokens accept hat directions but no modern pad will ever fire them.

### String binding format

Storage in `shortcuts.json` is per-shortcut-id keyed strings (or arrays of strings):

```json
{
  "shortcut.openpark_window": ["P"],
  "shortcut.cancel_construction": ["ESCAPE"],
  "shortcut.scale_to_fit": "CTRL+0",
  "shortcut.giant_screenshot": "CTRL+SHIFT+S"
}
```

`ShortcutInput::ShortcutInput(string_view value)` parses by splitting on `+`, accumulating modifier tokens (`CTRL`, `SHIFT`, `LSHIFT`, `RCTRL`, `ALT`, `GUI`, etc.), then interpreting the trailing token as one of:

- `MOUSE n` / `LMB` / `RMB` → mouse button
- `JOY n` → joyButton, button index `n-1`
- `JOY UP/DOWN/LEFT/RIGHT` → joyHat with direction
- otherwise → keyboard scancode via `SDL_GetKeyFromName(text.c_str())`

`toString()` (line 273) is the inverse — produces `MODS+KEY`-style strings. Localisation only affects modifier names (`CTRL` vs translated `STR_SHORTCUT_MOD_CTRL`); button values stay as SDL key names.

**No version field on `shortcuts.json`.** Detection of "new format" must be by content (presence of new tokens) or by adding an explicit version key on save.

### Default bindings

`Shortcuts.cpp::registerDefaultShortcuts` (line 757) is the catalog. Each call passes either no chord, one chord string, or two chord strings. Examples:

```cpp
registerShortcut(ShortcutId::kInterfaceCloseTop, STR_SHORTCUT_CLOSE_TOP_MOST_WINDOW, "BACKSPACE", []() { ... });
registerShortcut(ShortcutId::kInterfaceCancelConstruction, STR_SHORTCUT_CANCEL_CONSTRUCTION_MODE, "ESCAPE", []() { ... });
registerShortcut(ShortcutId::kInterfacePause, STR_SHORTCUT_PAUSE_GAME, "PAUSE", []() { ... });
registerShortcut(ShortcutId::kInterfaceScreenshot, STR_SHORTCUT_SCREENSHOT, "CTRL+S", []() { gScreenshotCountdown = 2; });
registerShortcut(ShortcutId::kInterfaceGiantScreenshot, STR_SHORTCUT_GIANT_SCREENSHOT, "CTRL+SHIFT+S", ScreenshotGiant);
registerShortcut(ShortcutId::kInterfaceLoadGame, STR_LOAD_GAME, "CTRL+L", ShortcutLoadGame);
registerShortcut(ShortcutId::kInterfaceSaveGame, STR_SAVE_GAME, "CTRL+F10", ShortcutQuickSaveGame);
registerShortcut(ShortcutId::kInterfaceMute, STR_SHORTCUT_MUTE_SOUND, Audio::ToggleAllSounds);
registerShortcut(ShortcutId::kInterfaceScaleToggleWindowMode, STR_SHORTCUT_WINDOWED_MODE_TOGGLE, "ALT+RETURN", ToggleWindowedMode);
```

**OPENRCT2MINI cut 44 has stripped the default keybind** for several shortcuts (the `RegisteredShortcut(_id, _name, _defaultChord, _action)` constructor at ShortcutManager.h:82 marks the chord parameter as `[[maybe_unused]]` — the default is silently discarded). This was done because the device's faked scancodes for D-pad / face buttons would collide with the default arrow / Z / X keybinds. The shortcut still exists in the catalog; it's just unbound until the user (or our defaults) explicitly maps it.

### Save / load

`saveUserBindings` (line 331): writes a `json_t` keyed by shortcut id; value is either a single string (one binding) or array of strings (multiple bindings). Always rewrites the entire file.

`loadUserBindings` (line 288): reads each key, looks up the shortcut, replaces `current` with parsed `ShortcutInput` objects.

## 4. UiContext virtual cursor system (the entire `_vKb*` block)

This is the single largest piece of input code in the project — about 250 lines of the UiContext class. Lines 110-256 declare state and helpers; lines 257-431 implement `InterceptVirtualCursorKey`; lines 1303-1530 implement `ProcessVirtualGamepadCursor`. All of it is gated on `#ifdef ENABLE_SOFTWARE_CURSOR` (cut 45 made that the host default too, so this code runs on host as well).

### State (lines 110-160)

```cpp
float _vcursorX, _vcursorY;          // sub-pixel cursor position
bool _vcursorInitialised;            // lazy-init at first poll
uint32_t _vdpadHeldSinceMs;          // for ramp-up acceleration
uint32_t _vcursorLastTickMs;         // for dt-based velocity (cut 44c)
int32_t _vcursorLastIntX, _vcursorLastIntY;  // for resync detection (cut 47)
bool _vprevA, _vprevB;               // for click edge detection

// Latched keyboard state (set by InterceptVirtualCursorKey,
// not by SDL itself):
bool _vKbUp, _vKbDown, _vKbLeft, _vKbRight;  // arrow keys
bool _vKbZ, _vKbX;                            // click buttons
bool _vKbShift;                               // fast modifier (Q / Alt / device A)
bool _vGamepadMod;                            // R1 modifier (cut 59)
bool _vKbCtrl;                                // Z-lock (cut 60)
bool _vKbCPressed;                            // X press latch
bool _vKbCHoldFired;                          // X hold-fire latch
uint32_t _vKbCPressedAtMs;                    // X press timestamp (W9)
bool _vKbCSuppressShade;                      // R1+X chord suppress (W*)
SDL_Scancode _oskClosingSwallowKey;           // OSK closing swallow (OSK)
```

### `InterceptVirtualCursorKey(SDL_Scancode sc, bool down)` (line 257-401)

Called from `ProcessMessages` for every SDL_KEYDOWN/UP before anything else. Returns `true` if the key should be swallowed.

The body is a giant switch on `sc`:

| Scancode | What happens | Swallowed? |
|---|---|---|
| `UP/DOWN/LEFT/RIGHT` | sets `_vKb{Up,Down,Left,Right}` | yes |
| `Z` | sets `_vKbZ` | yes |
| `X` | sets `_vKbX` AND fires the X-press / X-release tap-vs-hold state machine for shade-window-under-cursor + R1+X close | yes |
| `Q` | sets `KMOD_LSHIFT` in SDL mod state (synthesises a Shift) | yes (unless text-input active, then propagate as literal Q) |
| `A` | sets `_vKbShift` AND `_vGamepadMod` (mirrors L/R Alt) | yes (unless text-input active) |
| `LALT` / `RALT` | sets `_vKbShift` AND `_vGamepadMod`; clears `KMOD_LALT/RALT` so it doesn't propagate | yes |
| `LSHIFT` / `RSHIFT` | no swallow — propagates the modifier | no (passes through to ShortcutManager) |
| `LCTRL` / `RCTRL` | sets `_vKbCtrl` for fast-cursor suppression; lets it propagate | no |
| anything else | no | no |

Plus a sequence at the top for OSK handling: if `Windows::OskIsActive()` and `OskHandleKey(sc, down)` returns true, the event is consumed by the OSK; if the OSK closed during the event, a swallow-on-keyup latch is set.

### `ProcessVirtualGamepadCursor()` (line 1303-1530)

Called once per frame from `UiContext::Tick`. Lines 1338-1356 are the **direct controller polling** block:

```cpp
const auto& controllers = _inputManager.getGameControllers();
bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
bool btnA = false, btnB = false, fastModifier = false;
for (auto* gc : controllers) {
    dpadUp |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    dpadDown |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    dpadLeft |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    dpadRight |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
    btnA |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
    btnB |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0;
    fastModifier |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
}
```

Then OR-merges the keyboard latches:

```cpp
dpadUp |= _vKbUp;  dpadDown |= _vKbDown;
dpadLeft |= _vKbLeft;  dpadRight |= _vKbRight;
btnA |= _vKbZ;  btnB |= _vKbX;
fastModifier |= _vKbShift;
if (_vKbCtrl) fastModifier = false;  // Z-lock cancels fast cursor
```

Then runs the **velocity logic** (lines 1377-1485) — sub-pixel float accumulator, ramp-up acceleration based on `gamepadDeadzone` config, framerate-independent dt, fast-modifier 2.5× boost, diagonal 0.7071 normalization, clamp to canvas bounds, write rounded integer to `_cursorState.position`.

Finally, **A/B click edge detection** (lines 1486-1528) — when `btnA` transitions, calls `StoreMouseInput(MouseState::leftPress|leftRelease, _cursorState.position)`; same for `btnB` → right click.

### Shade-window state machine (W9, refined repeatedly)

Ties together:
- X press → set `_vKbCPressed`, record `_vKbCPressedAtMs`
- X release within 500ms → tap → call `ShadeWindowUnderCursor`
- X release after 500ms → no-op (hold fired earlier)
- `ProcessVirtualGamepadCursor`'s frame-poll: if `_vKbCPressed && !_vKbCHoldFired && !_vKbCSuppressShade` and held >= 500ms → fire `ToggleShadeAll`, set `_vKbCHoldFired`
- R1+X chord (frame after R1 already held) → `CloseWindowUnderCursor`, set `_vKbCSuppressShade` so the matching X-release skips the tap-shade

### OSK suspend

When `Windows::OskIsActive()`, `ProcessVirtualGamepadCursor` early-returns after clearing all the latches — D-pad is delivered to the OSK instead of moving the cursor. This branch lives at lines 1310-1321.

## 5. Pre-existing chord behaviors (must be preserved during 1.5/1.6 migration)

Comprehensive list of every input behavior currently driven by `_vKb*` / direct controller poll / scancode synthesis. Each MUST have an equivalent `ShortcutManager` binding before deletion.

| # | Behavior | Today's path | Target shortcut id (to be coined) |
|---|---|---|---|
| 1 | Cursor up/down/left/right (held, with ramp-up acceleration) | `_vKb{Up,Down,Left,Right}` + direct GC `BUTTON_DPAD_*` poll | `kCursorUp/Down/Left/Right`, hold-style |
| 2 | Click | `_vKbZ` + direct GC `BUTTON_A` poll → `MouseState::leftPress` | `kCursorClick` |
| 3 | Cancel | `_vKbX` + direct GC `BUTTON_B` poll → `MouseState::rightPress` | `kCursorCancel` |
| 4 | Fast-cursor modifier (R1 held) | `_vKbShift` + `_vGamepadMod` + direct GC `BUTTON_RIGHTSHOULDER` poll | `kCursorFastModifier`, hold-style modifier |
| 5 | Z-lock (Ctrl held / L1+R1 chord on device) | `_vKbCtrl` from LCTRL scancode | `kInterfaceConstructionZLock`, hold-style modifier |
| 6 | Shade-one window (X tap < 500ms) | X-release within 500ms after press → `ShadeWindowUnderCursor` | `kInterfaceShadeWindowUnderCursor`, tap-style |
| 7 | Shade-all toggle (X hold >= 500ms) | per-frame check → `ToggleShadeAll` | `kInterfaceToggleShadeAllWindows`, hold-fire-once style |
| 8 | Close-window-under-cursor (R1+X chord) | scancode chord state machine in InterceptVirtualCursorKey + frame poll | `kInterfaceCloseWindowUnderCursor`, chord-shortcut |
| 9 | Right-click drag (mouse-only, unchanged) | mouse path | unchanged — not a gamepad migration |
| 10 | Window drag via right-click (rev W*-cuts 308-309) | `ProcessVirtualGamepadCursor` writes `_cursorState.position` while right-button held | unchanged — uses click events from migrated #3 |
| 11 | Q → Shift modifier synthesis | InterceptVirtualCursorKey synthesises `KMOD_LSHIFT` into SDL state | DELETE — host devs use real keyboard Shift; no need to fake it |
| 12 | A → Alt + fast-cursor synthesis | InterceptVirtualCursorKey | DELETE — replaced by direct gamepad shortcut binding |

Behaviors 11 and 12 are PURELY developer-test-on-host conveniences from the era when the host build had no real gamepad path. With native SDL_GameController bindings (Plan Phase 1.2-1.4), host devs press the actual gamepad button (or a real keyboard key bound to the same shortcut), and the synthesis layer is unnecessary.

## 6. Vendor SDL2 state

`dist/vendor/sdl2_miyoo-main/sdl2/`:

### `src/joystick/mmiyoo/SDL_joystick_mmiyoo.c`

A driver shell — registered in `src/joystick/SDL_joystick.c:102` (`&SDL_MMIYOO_JoystickDriver`) and built when `SDL_JOYSTICK_MMIYOO=1` (configured in `SDL_config.h:484`). Implements the `SDL_JoystickDriver` vtable but every real method is a stub:

- `MMIYOO_JoystickInit` returns 1.
- `MMIYOO_JoystickGetCount` returns 1 (one device hardcoded).
- `MMIYOO_JoystickOpen` sets `nbuttons=14, naxes=2, nhats=0` and returns. Doesn't store the joystick handle.
- `MMIYOO_JoystickUpdate` does nothing.
- `MMIYOO_JoystickGetGamepadMapping` returns `SDL_FALSE` — meaning `SDL_IsGameController` returns false on Mini.
- `MMIYOO_JoystickRumble` / `RumbleTriggers` / `SetLED` / `SendEffect` / `SetSensorsEnabled` all return `SDL_Unsupported()`.

### `src/video/mmiyoo/SDL_event_mmiyoo.c`

The actual event-emitter for the Mini. Spawns a worker thread (`EventUpdate`) that reads `/dev/input/event0` evdev events, maps each `EV_KEY` press to a `MYKEY_*` bit in `evt.keypad.bitmaps`. `MMIYOO_PumpEvents` is called by SDL's pump tick; it diffs the bitmap against the previous frame and calls `SDL_SendKeyboardKey(state, SDL_GetScancodeFromKey(code[idx]))` for each changed bit, where `code[]` is the hardcoded keyboard-scancode translation table.

The `code[]` table:

```c
const SDL_Scancode code[]={
    SDLK_UP,            // UP
    SDLK_DOWN,          // DOWN
    SDLK_LEFT,          // LEFT
    SDLK_RIGHT,         // RIGHT
    SDLK_SPACE,         // A
    SDLK_LCTRL,         // B
    SDLK_LSHIFT,        // X
    SDLK_LALT,          // Y
    SDLK_e,             // L1
    SDLK_t,             // R1
    SDLK_TAB,           // L2
    SDLK_BACKSPACE,     // R2
    SDLK_RCTRL,         // SELECT
    SDLK_RETURN,        // START
    SDLK_HOME,          // MENU
    ...
};
```

This is what makes the device's "feel" entirely keyboard-driven: SDL never knows there's a gamepad attached.

### Vendor SDL2 capability headers

- `SDL_GameControllerHasRumble` declared at `include/SDL_gamecontroller.h:905`. Available API; vendor stub just returns `SDL_Unsupported`.
- `SDL_GameControllerHasLED` declared similarly. Same situation.
- `SDL_GameControllerSetLED` available.
- `SDL_GameControllerRumble` / `RumbleTriggers` available.

So the SDL_GameController capability-API is fully present in the vendor SDL2 — it just hits the stub on Mini and does nothing.

### Vendor `SDL_GamepadMapping` divergence

`src/joystick/SDL_sysjoystick.h:192` declares:

```c
SDL_bool(*GetGamepadMapping)(int device_index, SDL_GamepadMapping *out);
```

This is an out-parameter struct. **Upstream SDL2** uses `char* mapping_string` returning a controller-mapping-database string. The vendor diverged to a struct. Phase 2.3 implementations must match the vendor's struct, not write a mapping string.

## 7. Adjacent systems

### News severity (referenced by Plan 1.12-1.13)

`src/openrct2/management/NewsItem.h`:

```cpp
enum class ItemType : uint8_t {
    null, ride, peepOnRide, peep, money, blank,
    research, peeps, award, graph, campaign, count
};
```

These are **categorisation labels**, not severity levels. There is no existing classification of "guest drowning" (critical) vs "guest happy" (info) — both are `peep`.

`ItemTypeProperty` enum (line 51) classifies whether a type requires an `assoc` (e.g. ride/peep/peepOnRide require an entity id; money/blank don't). Not severity.

Plan 1.12 must add a severity classification — either a static StringId-keyed table or a severity field at item creation.

### Rebind UI

`src/openrct2-ui/windows/ShortcutKeys.cpp` is the Options > Controls > Shortcut Keys window. Structure:

- One row per registered shortcut, showing localised name + currently-bound chord(s).
- Click a row → enters "press a key" capture mode: next keyboard chord becomes the new binding.
- Right-click → context menu (clear binding, etc.).
- Renders ~16 rows visible, scrollable.

Plan 1.7 needs to:
- Add a second column for gamepad binding.
- Extend capture mode to handle gamepad button / chord / trigger inputs.
- Implement the IDLE → ACCUMULATING → CONFIRMED state machine described in Plan 1.7.4.

### Coaster-G data path (Plan 1.11 stretch goal)

`WindowFollowSprite(Window&)` exists in `src/openrct2/interface/Viewport.cpp` — sets the window's `viewport_target_sprite` to a specific sprite ID. The viewport then tracks that sprite each frame.

**Missing**: there is no "current G-force on the followed vehicle" derived signal. To implement coaster-ride rumble, a new helper would need to:
- Detect that the followed sprite is a `Vehicle` entity (via `EntityRegistry::Get<Vehicle>(spriteId)`).
- Sample the vehicle's `velocity` / `acceleration` per frame.
- Look up the current track piece (`Vehicle::TrackLocation` + `TrackTypeFromTrackElementType`) to get track-piece curvature data.
- Combine velocity + curvature into a scalar G-force estimate.
- Clamp / curve into rumble amplitude.

This is multi-day work. Plan 1.11 lists it as a stretch goal that ships separately if needed.

## 8. Catalogue of kludges to migrate (numbered for Plan 1.5)

Each item is a behavior currently implemented via `_vKb*` / direct GC poll / scancode synthesis that must be ported to a `ShortcutManager` binding:

1. **Cursor up/down/left/right (held)** — bind `kCursorUp/Down/Left/Right` to `PAD DPAD_*` (default) and arrow keys.
2. **Click button** — bind `kCursorClick` to `PAD B` and `Z`.
3. **Cancel button** — bind `kCursorCancel` to `PAD A` and `X`.
4. **Fast-cursor modifier (held)** — bind `kCursorFastModifier` to `PAD R1` and SHIFT.
5. **Construction Z-lock modifier (held)** — bind `kInterfaceConstructionZLock` to `PAD L1+R1` and CTRL.
6. **Shade window under cursor (tap)** — bind `kInterfaceShadeWindowUnderCursor` to `PAD Y` (tap) and X (tap).
7. **Shade all (hold)** — bind `kInterfaceToggleShadeAllWindows` to `PAD Y` (hold) and X (hold). Tap-vs-hold variant of #6.
8. **Close window under cursor (chord)** — bind `kInterfaceCloseWindowUnderCursor` to `PAD R1+Y` and an existing keyboard chord (CTRL+W or similar).
9. **(unchanged)** Right-click drag, window drag via right-click — these consume click events from migrated #3.
10. **OSK suspend** — keep `Windows::OskIsActive()` early-return in cursor logic; OSK consumes its own gamepad inputs.

Plus the synthesis layers to delete during 1.6:

11. **Q → KMOD_LSHIFT synthesis** — delete; host uses real Shift.
12. **A → `_vKbShift` + `_vGamepadMod` synthesis** — delete; replaced by `PAD R1` direct binding for fast-cursor.
13. **LALT/RALT → `_vKbShift` + `_vGamepadMod` synthesis** — delete; replaced by `PAD R1` direct binding.
14. **Direct `SDL_GameControllerGetButton` polling block (UiContext.cpp:1338-1356)** — delete; cursor reads its inputs from `InputManager::isShortcutHeld(kCursor*)`.
15. **All `_vKb*` member declarations** — delete; cursor state lives in shortcut held-set.
16. **`_vGamepadMod` flag** — delete; subsumed by `kCursorFastModifier` shortcut held-state.
17. **`_vKbCtrl` flag** — delete; subsumed by `kInterfaceConstructionZLock` shortcut held-state.

## 9. Findings summary

For the plan-design discussion in 1.2-1.13:

- **String-format storage works.** Don't migrate to typed JSON — extend the parser with `PAD ...` tokens. Backward compat is automatic.
- **Chord support is net-new.** No held-button-set exists for non-keyboard kinds today. Build it in `InputManager` and consume in `ShortcutInput::matches`.
- **Largest-match-wins is net-new.** Keyboard side does exact match. Don't break that; add the new logic for gamepad bindings only.
- **Trigger-as-button is net-new.** `joyAxis` kind isn't matched today. Add the threshold-with-hysteresis to `ShortcutInput::matches`.
- **D-pad uses joyButton, not joyHat.** Hat path is dead for SDL_GameController.
- **Console / chat / OSK gating is incomplete for gamepad.** Symmetric path needs adding to `InputManager::process(const InputEvent&)`.
- **`_vKb*` is a real input bus, not just shadow flags.** It carries 8+ chord behaviors. Each must be ported to a shortcut binding before deletion.
- **Direct stick polling for sticks → cursor / camera should stay direct-poll.** Don't rebuild that on top of queued joyAxis events; just extend `processAnalogueInput` to read RIGHTX/RIGHTY too with separate config knobs.
- **Vendor SDL2 GamepadMapping signature is divergent from upstream.** Phase 2.3 fills a struct, not constructs a string.
- **News severity classification is missing.** Plan 1.12 needs to add it before LED (1.13) can work.
- **Coaster-G rumble (1.11 stretch) needs new vehicle-state plumbing.** Ship the rest of 1.11 (crash + construction haptics) without it; coaster-G can land later.

This audit is the reference document for design decisions in 1.2-1.13. Any deviation in implementation should update this file alongside the plan.
