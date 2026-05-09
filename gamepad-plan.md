# Native Gamepad Support — Plan

**Status:** drafted, post-source-audit. Not yet started.
**Owner:** OpenRCT2mini fork.
**Goal:** real first-class gamepad as an input device on both host and Miyoo Mini, with keyboard and mouse remaining fully usable in parallel. No keyboard-translation kludges.

## Motivation

Today the gamepad story is fragmented across **three** parallel input feeds that all converge on the virtual-cursor's `_vKb*` flags in `UiContext.cpp`:

1. **Real keyboard scancodes** from `SDL_KEYDOWN` (host).
2. **Synthesised keyboard scancodes** from the Mini's vendor SDL2 (D-pad → arrows, A → SPACE, Q → SHIFT, A → ALT, etc.) — see `dist/vendor/sdl2_miyoo-main/sdl2/src/video/mmiyoo/SDL_event_mmiyoo.c`. The vendored joystick driver `SDL_joystick_mmiyoo.c` is a stub that registers but produces no events.
3. **Direct `SDL_GameControllerGetButton()` polling** in `UiContext::Update` (host with a real pad).

All three OR-merge into `_vKbUp/Down/Left/Right/Z/X/Shift/Ctrl` plus `_vGamepadMod` (the existing single-button gamepad-modifier flag). These flags drive the virtual cursor, fast-cursor mode, click state, and a small chord state machine for behaviors like "R1+X = close window under cursor". The chord behaviors are wired through scancode handling, NOT through `ShortcutManager`.

`ShortcutManager` itself supports keyboard chords (modifier + key, exact-match) and mouse buttons. It also accepts `joyButton` and `joyHat` device kinds in its data model, but: (a) it has no held-button-set concept for non-keyboard devices, (b) `joyHat` is dead for SDL_GameController (which emits `BUTTON_DPAD_*` not hat events), (c) `joyAxis` events flow into the queue from `InputManager` but `ShortcutInput::matches()` doesn't handle that kind, (d) there is no chord support for joystick at all — modifiers are read exclusively from `SDL_GetModState()` (keyboard).

This plan replaces the three feeds with one unified design: gamepad is a peer input device alongside keyboard and mouse, all three driving the game through one extended `ShortcutManager` that gains a held-button-set concept for cross-device chord matching. End state is platform-agnostic — the same input path runs on host and Mini.

## End-state design

```
        Host (existing SDL2)              Mini (vendored SDL2)
        --------------------              --------------------
        Real Xbox/PS pad                  Hardware buttons
                |                                |
                v                                v
        SDL_CONTROLLER* events            SDL_CONTROLLER* events
                                          (synthesised by Phase 2
                                          vendor-SDL2 changes)
                |                                |
                +----------+----------+----------+
                           |
                           v
                   ShortcutManager
                  /        |        \
            keyboard      mouse     gamepad
            bindings      events    bindings
                           |
                           +-- per-frame held-set tracking
                           |   (NEW: needed for gamepad chords)
                           |
                           v
                  fire game action
```

A user can bind `OPEN_PARK_WINDOW` to keyboard `P`, mouse `MOUSE 5`, and gamepad `PAD BACK` simultaneously. Any of those three fires the same action. Defaults ship with sensible bindings on every input type.

## Phase 1 — Host: native SDL_GameController as a first-class input

All testing on host. No changes to vendor SDL2 in this phase. Mini build remains on the existing keyboard-translation path until Phase 2.

### 1.1 Audit (deliverable: `gamepad-audit.md`)

No code. Document the state of input handling end-to-end. Output is **a multi-page markdown** (audit will not fit in two pages — three platforms' input feeds, three parallel paths into `_vKb*`, four event types in InputManager, console/chat/OSK exception modes, and the existing Mini-style chord behaviors via scancode synthesis all need walking).

Cover:

- Every entry point in `UiContext::ProcessMessages` for SDL events: which event types are processed, what each one does, what state it mutates (`_cursorState`, `_inputManager` queue, etc.).
- `InputManager::queueInputEvent(const SDL_Event&)` — the four arms (`CONTROLLERAXISMOTION`, `JOYHATMOTION`, `CONTROLLERBUTTONDOWN/UP` + `JOYBUTTONDOWN/UP`, `CONTROLLERDEVICEADDED/REMOVED`). What's queued, who consumes it.
- `InputManager::processAnalogueInput()` direct-poll path — currently reads LEFTX/LEFTY only; outputs `_analogueScroll` consumed by `handleViewScrolling()`.
- `InputManager::handleViewScrolling()` — three scroll sources (gamepad analog, keyboard, mouse-edge) with title-sequence and console-open early-returns.
- `InputManager::process(const InputEvent&)` — keyboard-only routes for console/chat/textinput/loadsave/loadsaveOverwritePrompt/widgetTextBox; everything else falls through to `shortcutManager.processEvent(e)`. **Note** that gamepad events skip these routes today, falling straight to ShortcutManager — meaning when console is open, controller buttons hit ShortcutManager but only `kDebugToggleConsole` is checked (line 332-339).
- `ShortcutManager::processEvent` and `RegisteredShortcut::matches` — exact-modifier match logic.
- `ShortcutInput` — string-format parser/serializer (NOT JSON-typed). Tokens: `MODS+KEY`, `MOUSE n`, `LMB`, `RMB`, `JOY n`, `JOY UP/DOWN/LEFT/RIGHT` (hat). `joyAxis` kind has no string form today.
- `UiContext::Update`'s direct `SDL_GameControllerGetButton()` polling for virtual cursor — every button it reads, what state it sets.
- `_vKb*` flags + `_vGamepadMod` + `_vKbCPressedAtMs` + `_vKbCHoldFired` + `_vKbCSuppressShade` — every place set or read; existing chord state machine (`R1+X` close window, `X` tap-vs-hold for shade-one-vs-shade-all, etc.).
- Q→SHIFT and A→ALT keyboard-scancode synthesis path that mirrors the Mini's vendor SDL2 keyboard-translation in the host scancode handler (UiContext.cpp:303-348).
- `SDL_GameControllerHasRumble` / `SDL_GameControllerHasLED` availability — confirmed present in vendor SDL2 headers; usable on both host and Mini SDL2 builds.
- The existing `Config::general.gamepadDeadzone` / `gamepadSensitivity` consumed by `processAnalogueInput()`.
- News item `ItemType` taxonomy in `NewsItem.h` — note: types are CATEGORIES (peep / money / ride / award / etc.), NOT severity levels. Severity classification is missing and will need to be added under 1.11.

Output as `gamepad-audit.md` with a graph of the three feeds and a numbered list of "kludges to migrate during Phase 1.5–1.6".

### 1.2 Extend Shortcut bindings to support gamepad

This is **net-new infrastructure**, not a free data-model extension. The existing `ShortcutInput` matches on (deviceKind, button, keyboard-modifier-bits). For gamepad chords we need a held-button set tracked across events.

Two changes:

**(a) Extend `ShortcutInput` in-memory model:**

```cpp
struct ShortcutInput {
    InputDeviceKind kind{};         // existing
    uint32_t modifiers{};            // existing — keyboard only
    uint32_t button{};               // existing — primary button/key
    // NEW:
    std::vector<uint32_t> chordModifiers;  // gamepad-side: buttons that must
                                            // be held while `button` fires.
                                            // Empty for non-chord bindings.
    int32_t axisThreshold{};         // for joyAxis kind: ±value threshold
                                     // (signed — supports stick directions
                                     // as well as triggers)
    int8_t axisDirection{};          // -1 / +1 for joyAxis sign
};
```

**(b) Track held gamepad-button state in `InputManager`:**

```cpp
class InputManager {
    // NEW:
    std::set<uint32_t> _heldGamepadButtons;  // updated on every
                                              // CONTROLLERBUTTONDOWN/UP and
                                              // CONTROLLERAXISMOTION threshold
                                              // crossing.
};
```

`RegisteredShortcut::matches(const InputEvent&)` is extended: when the event is `joyButton` or `joyAxis`, also check that every entry in `chordModifiers` is currently in `_heldGamepadButtons`.

**(c) Extend `ShortcutInput` string format:**

Storage format **stays as plain strings inside `shortcuts.json`** — there's no typed-JSON migration. New tokens added to the string parser:

| Token | Meaning |
|---|---|
| `PAD A` / `PAD B` / `PAD X` / `PAD Y` | Face buttons (south / east / west / north — SDL convention) |
| `PAD DPAD_UP` / `DPAD_DOWN` / `DPAD_LEFT` / `DPAD_RIGHT` | D-pad as buttons (NOT hats — SDL_GameController doesn't emit hats) |
| `PAD L1` / `PAD R1` | Shoulders |
| `PAD L2` / `PAD R2` | Triggers (pressed, threshold-crossed) |
| `PAD BACK` / `PAD START` / `PAD GUIDE` | System buttons |
| `PAD L3` / `PAD R3` | Stick clicks |
| `PAD L1+B` | Chord — first token(s) are held-modifier buttons, last is the action button |
| `PAD STICK_L UP` / `STICK_R LEFT` etc. | Stick direction-as-button (axis with threshold) — bindable but defaults will not use this |

The existing legacy `JOY n` and `JOY UP/DOWN/LEFT/RIGHT` (hat) tokens **stay in the parser for backward compat** but are deprecated. New defaults all use `PAD ...`. The `JOY UP/DOWN/LEFT/RIGHT` (hat) tokens are effectively dead on SDL_GameController hardware; documented but not used in defaults.

**(d) Extend `ShortcutInput::matches(const InputEvent&)`** to handle `joyAxis` (currently it doesn't — this is a documented bug fix). Match condition: same axis index + sign-of-value past `axisThreshold`. Used for trigger bindings and stick-direction bindings.

### 1.3 Plumb controller events into ShortcutManager

Three things to wire:

- `SDL_CONTROLLERBUTTONDOWN/UP` already arrive at `InputManager::queueInputEvent` and become `joyButton` `InputEvent`s. **Add: update `_heldGamepadButtons` first** (before queueing), so the held-set is correct when the corresponding event is later processed by `ShortcutManager`.
- `SDL_CONTROLLERAXISMOTION` for triggers: track per-axis press/release transitions with hysteresis (50% press, 30% release). On a transition, queue a synthesized `joyAxis` `InputEvent` with `state = down` or `release` and the signed threshold value. Update `_heldGamepadButtons` so triggers can act as chord modifiers (`L2+A` etc.).
- Stick axes (LEFTX/LEFTY/RIGHTX/RIGHTY) **stay direct-polled** by `processAnalogueInput()` (1.9). They also queue `joyAxis` events for shortcut-bindable stick-direction tokens, but with the same hysteresis to avoid event spam at deadzone edge.

Console / chat / OSK gating (1.10): when console is open, `InputManager::process(const InputEvent&)` currently early-returns for keyboard events except `kDebugToggleConsole`. Mirror this for gamepad — when console is open, only console-mode shortcuts (toggle console, scroll up/down via D-pad, enter via south face) are fired; everything else goes to console text input. Same policy in chat and OSK contexts.

### 1.4 Define default gamepad bindings

Default bindings, expressed in the `PAD ...` string format:

```
PAD DPAD_UP / DPAD_DOWN / DPAD_LEFT / DPAD_RIGHT  → cursor up/down/left/right (held)
PAD A (south face)                                 → click / confirm           // Mini "B" position
PAD B (east face)                                  → cancel / back             // Mini "A" position
PAD Y (north face)                                 → toggle window shade       // Mini "X" position
PAD X (west face)                                  → swap window               // Mini "Y" position
PAD L1                                             → rotate view ccw           // Mini L1
PAD R1                                             → rotate view cw            // Mini R1
PAD L2                                             → zoom out                  // Mini L2
PAD R2                                             → zoom in                   // Mini R2
PAD BACK                                           → open menu (secondary)     // Mini SELECT
PAD START                                          → toolbar / pause           // Mini START
PAD GUIDE                                          → toggle console            // Mini MENU

// Existing Mini-style chord behaviors migrated from _vKb-based scancode handling:
PAD R1+Y                                           → close window under cursor // existing rev W*; Mini "X" press = SDL Y
PAD L1+R1                                          → construction Z-lock modifier (held)
PAD R1                                             → fast-cursor modifier (held; see 1.5)

// Host-only (Mini has neither stick) — direct-polled, not in shortcut table:
LEFTSTICK_AXIS                                     → analog cursor motion      // see 1.9
RIGHTSTICK_AXIS                                    → analog camera pan         // see 1.9
```

SDL canonicalises face buttons by physical position:
`BUTTON_A` = south (Xbox A / PS Cross / Nintendo B);
`BUTTON_B` = east  (Xbox B / PS Circle / Nintendo A);
`BUTTON_X` = west  (Xbox X / PS Square / Nintendo Y);
`BUTTON_Y` = north (Xbox Y / PS Triangle / Nintendo X).
The Mini's button labelling follows Nintendo convention — Mini "A" is right-side (east), Mini "B" is bottom (south) — so the plan's mapping above ties Mini "A" → `PAD B` and Mini "B" → `PAD A`. Phase 2's vendor SDL2 mapping does the same translation in the joystick driver so the same shortcut bindings work identically on both platforms.

**Phase ordering caveat for face buttons:** binding `PAD A` / `PAD B` / `PAD X` / `PAD Y` in 1.4 produces double-fires while the `_vKb*`-driven virtual cursor system still polls `SDL_CONTROLLER_BUTTON_A` / `_B` / `_RIGHTSHOULDER` directly (UiContext::ProcessVirtualGamepadCursor lines 1338-1356). Pressing south face would both left-click AND fire the new shortcut. Face-button bindings therefore ship in **1.5** alongside the cursor / click / shade migration, not in 1.4. Only the conflict-free buttons (L1, R1, L2, R2, START, GUIDE, BACK) ship as defaults in 1.4.

**1.4 also includes a conflict audit:** for each existing default keyboard binding, verify the new default gamepad binding fires the same shortcut ID. E.g. `kInterfaceCancelConstruction` defaults to ESCAPE; in 1.5 we add `PAD B` → same shortcut. Two bindings on one shortcut, both fire it — that's the model.

### 1.5 Migrate `_vKb*`-based behaviors to ShortcutManager

The existing `_vKb*` system isn't just shadow flags — it carries **real chord behaviors** built on top of synthesized scancodes (revs W0/W2/cuts 38b/43/44/60/61). These have to be migrated, not just deleted.

Behaviors to enumerate during 1.1 audit and migrate before deleting `_vKb*`:

| Behavior | Today's path | Migrated to |
|---|---|---|
| Cursor up/down/left/right (held) | `_vKbUp/Down/Left/Right` flags + `dpadUp/Down/Left/Right` direct poll | held shortcut bound to `PAD DPAD_*` |
| Click | `_vKbZ` + `dpadA` (BUTTON_A poll = south face) | shortcut fired on `PAD A` |
| Cancel | `_vKbX` + `dpadB` (BUTTON_B poll = east face) | shortcut fired on `PAD B` |
| Fast cursor (R1 held) | `_vGamepadMod` + RIGHTSHOULDER poll | held shortcut `PAD R1` (modifier-only) |
| Z-lock (Ctrl held / L1+R1 chord) | `_vKbCtrl` from LCTRL scancode | held shortcut `PAD L1+R1` |
| Shade window (X tap) / Shade all (X hold) | `_vKbCPressedAtMs` + `_vKbCHoldFired` state machine on Y-scancode | tap-vs-hold variant of `PAD Y` shortcut (existing tap/hold model in shortcut catalog) |
| Close window under cursor (R1+X) | scancode chord state machine | chord shortcut `PAD R1+Y` |
| Right-click drag (right-click button held during cursor motion) | mouse path, unchanged by this plan | unchanged |

Once **all** behaviors above have a shortcut equivalent and the shortcut path fires correctly, then 1.6 can delete the `_vKb*` flags + the direct-poll block + the Q/A scancode synthesis.

The cursor velocity logic in `UiContext::Update` (lines around 1377-1450) stays — it's just fed from "shortcut-is-currently-held" state queried via `InputManager` instead of from the OR'd flag soup.

### 1.6 Drop `_vKb*` shadow flags and Q/A scancode synthesis

After 1.5 migrations, the `_vKb*` block is fully redundant. Delete:
- All `_vKbUp/Down/Left/Right/Z/X/Shift/Ctrl/CPressedAtMs/CHoldFired/CSuppressShade` member declarations.
- The `OskHandleKey` block in UiContext.cpp:279-348 that sets them.
- The Q→KMOD_LSHIFT and A→`_vKbShift`/`_vGamepadMod` synthesis (lines 303-316) — host devs use real keyboard chords through the shortcut system instead.
- The `_vGamepadMod` flag itself.
- The direct `SDL_GameControllerGetButton()` poll block (UiContext.cpp:1338-1356).

The `OskHandleKey` block does have a separate purpose unrelated to virtual cursor (`_oskClosingSwallowKey` etc.) — keep that part, just drop the cursor-state assignments.

### 1.7 UI: gamepad shortcut binding in Options > Controls > Shortcut Keys

The Shortcut Keys window already has one column per shortcut showing the keyboard binding plus a clickable area to capture a new key. We add a second column on the right with the same affordance for gamepad. Both columns can hold zero, one, or many bindings per shortcut — a shortcut's "binding list" is the union, and any single binding firing fires the action.

The user-facing complexity is concentrated in two things: (a) representing chords compactly and unambiguously in the row, and (b) capturing chord input cleanly without confusing single-button and chord intent. The rest of this section nails those down.

#### 1.7.1 Chord model

A **gamepad binding** is one of:

- **Single button:** one `SDL_GameControllerButton` (face / D-pad / shoulder / start / back / guide / stick-click). Always digital.
- **Single trigger:** one `SDL_GameControllerAxis` for `TRIGGERLEFT`/`TRIGGERRIGHT` with a fixed press-threshold of 50% of the axis range and 30% release-threshold (hysteresis prevents flicker near the edge).
- **Modifier-only chord:** 2-3 modifier buttons held simultaneously, no action button (e.g. `L1+R1` for the construction Z-lock). Fires on the last button-down completing the chord; releases on any button-up.
- **Action chord:** 1-2 modifier buttons + exactly one action button. Fires on action-button-down while modifiers are held.
- **Cardinality cap:** 3 total. Beyond that the user is fighting their thumbs and most pads can't N-key rollover anyway. Soft limit during capture (UI hints "too many buttons" on 4th press), hard limit in the data model.

Canonicalisation at storage time uses a fixed ordering — modifiers first in priority order, then the optional action button. **Modifier buttons** (in priority order):

1. `L1`, `R1`
2. `L2`, `R2`
3. `BACK`, `START`, `GUIDE`
4. `L3`, `R3` (stick clicks)

**Non-modifiers (action buttons):** D-pad, face buttons. At most one per chord.

Stored canonical form examples:
- `{B, L1}` → string `"PAD L1+B"`
- `{L1, R1}` → string `"PAD L1+R1"` (modifier-only)
- `{R1, R2, A}` → string `"PAD R1+R2+A"`
- `{A, B}` → **invalid** (two action buttons), rejected at capture time.

#### 1.7.2 String storage format inside `shortcuts.json`

`shortcuts.json` already stores bindings as plain strings, e.g. `"shortcut.id": "CTRL+P"` or `"shortcut.id": ["CTRL+P", "MOUSE 5"]`. **We extend the string vocabulary, not the JSON schema.** Old entries continue parsing unchanged; new entries use `PAD ...` tokens.

Concrete example file:

```json
{
  "shortcut.openpark_window": [
    "CTRL+P",
    "PAD BACK",
    "PAD L1+B",
    "PAD L2"
  ]
}
```

Backward compat: a file without any `PAD ...` strings is "old format" — the loader fills in default gamepad bindings on first save (1.8). Detection uses presence/absence of `PAD ...` strings rather than a version field, since `shortcuts.json` has no version field today.

Optional: to make migration explicit, add a top-level `"_version": 2` key on save. Files without it are treated as v1 and gamepad-defaults filled in. The OPENRCT2MINI cut-44b launch.sh stale-shortcuts.json wipe stays as a belt-and-braces measure for the device.

#### 1.7.3 Display format in the row

Text labels with the `PAD ` prefix stripped for compactness, joined by `+` in canonical (storage) order:

- Single: `B`, `DPAD_UP`, `L1`, `L2`
- Chord: `L1+B`, `BACK+DPAD_LEFT`, `L1+R1+A`
- Empty: `—`

Multiple bindings on the same shortcut: comma-separated list, same as the keyboard column today (`BACK, L1+B`).

The row width budget is the existing keyboard column's. We don't lengthen the window. Long chord strings ellipsis at column edge with a tooltip showing the full string.

Localisation: button names are not translated (they're SDL identifiers — translating "B" or "L1" loses meaning). The "+" joiner and the "—" empty marker come from the localisation table.

(Glyph artwork — actual face-button icons rendered to bitmap — is post-Phase-2 polish. Text labels carry the meaning unambiguously and ship with the first release.)

#### 1.7.4 Capture flow

Click on the gamepad column of a row. The cell enters **capture mode** and renders `[Press buttons…]`. State machine while in capture:

```
       (enter capture)
              │
              v
   ┌─────────────────────┐
   │   IDLE              │ no buttons currently held; waiting
   │                     │ for the user to start a press
   └─────────────────────┘
       │ first DOWN event
       v
   ┌─────────────────────┐
   │   ACCUMULATING      │ at least one button held; record every
   │   (held-set grows)  │ DOWN into a held-set. Triggers count when
   │                     │ they cross the press-threshold. Cardinality
   │                     │ ≥ 2 enforces "at most one action button":
   │                     │ if held-set already contains an action
   │                     │ button and another action button is added,
   │                     │ "invalid chord" hint, stay in this state
   │                     │ until release. Modifier-only chords are
   │                     │ allowed.
   └─────────────────────┘
       │ all buttons released
       v
   ┌─────────────────────┐
   │   CONFIRMED         │ the largest-cardinality held-set we observed
   │                     │ during the press is the binding. If it was
   │                     │ a valid chord (1.7.1), commit. If invalid,
   │                     │ discard and re-enter IDLE with a hint.
   └─────────────────────┘
       │
       v
   exit capture, write binding
```

**Why "release all to confirm"?** Committing on first button-down would prevent chords — `B` would commit before `L1` could be added. Waiting until release lets the user assemble the chord by holding extra buttons, then commit by letting go.

**Cancel:** ESC on keyboard at any state exits capture and leaves the existing binding untouched. Right-mouse-click on the row also cancels (matches existing keyboard-rebinding cancel behavior).

**Idle timeout:** if 8 seconds pass in IDLE with no input, capture exits and the existing binding is preserved. Prevents a user who clicked accidentally from being stuck.

**Hold timeout:** if 8 seconds pass in ACCUMULATING (something is held the whole time), commit whatever is held now. This handles the trigger-only case where the user holds `L2` to bind it but never adds a second input.

**Replace vs append:** a configurable on the row's right-click context menu — `Replace existing` (default) blows away the current bindings and installs the new one, `Add to existing` keeps existing and appends.

**Capturing while game running:** capture is modal at the rebind cell. The rest of the game continues to receive its own input. A click on the cell suppresses gamepad-shortcut firing (so pressing buttons during capture doesn't trigger game actions) but allows mouse and keyboard to operate the rebind UI itself. Cleaner alternative considered: pause the game during capture. Rejected — the user might want to bind something they'll see in context.

#### 1.7.5 Conflict resolution at fire time (NEW logic)

This is **net-new logic**, not "the same as keyboard". The existing keyboard side does exact-modifier match — `Ctrl+P` and `P` are distinct shortcuts; pressing Ctrl+P does not also fire `P`. There's no existing largest-subset-match algorithm to reuse.

For gamepad, we implement largest-match-wins per-event against the live held-set:

```
on each gamepad button/trigger DOWN event:
    H = current held-set ∪ {newly-pressed button}
    candidates = { binding B for any shortcut S : B.buttons ⊆ H AND B contains the newly-pressed button as action button }
    if candidates non-empty:
        fire shortcut whose binding has largest |B.buttons|
        ties broken by definition order in shortcuts catalog
        suppress lower-cardinality bindings whose buttons are subsumed by a fired chord (held-shortcut release event fires for any smaller match that was previously firing)
```

So if `PAD B` is bound to "click" and `PAD L1+B` is bound to "rotate ccw", and the user presses `L1` then `B`:

1. After `L1` down: H = {L1}. No binding has `L1` as action button. Nothing fires.
2. After `B` down: H = {L1, B}. Two bindings have `B` as action button — `{B}` (cardinality 1) and `{L1, B}` (cardinality 2). The larger wins: rotate ccw fires. The single-`B` "click" binding is suppressed.
3. After `B` up (L1 still held): chord broken. H = {L1}. Rotate-ccw release event fires.
4. After `L1` up: H = {}. Nothing happens.

**Press-order edge case.** If the user presses `B` first (firing "click"), then `L1` (forming the chord retroactively):
- "click" is a tap shortcut and already fired. We don't try to "unfire" it — that would require a delay buffer that breaks input feel.
- Holding `L1` after the fact does NOT retroactively fire "rotate ccw" — the rule above says "newly-pressed button must be the action button". `L1` is not an action button.
- Tap shortcuts that already fired stay fired; the user gets only "click" in this case. To get "rotate ccw" the user must press modifiers first (the same pattern as keyboard chords).

Held shortcuts (cursor direction, fast-cursor-while-R1-held) do honor mid-chord upgrades: when held-set grows and a larger-cardinality match exists, the smaller match's release event fires immediately and the larger one's press event fires.

**Modifier-only chords** (`L1+R1`): fire on the last modifier-down completing the chord. Release on any modifier-up.

**Trigger jitter near threshold:** the 50% press / 30% release hysteresis from 1.7.1 covers this. A trigger that drifts between 45% and 55% won't oscillate.

**Conflicting chord bindings on multiple shortcuts:** disallowed by the rebinding UI — when the user tries to bind a chord that another shortcut already has, show a hint and refuse, same as keyboard today.

#### 1.7.6 What ships in 1.7

- Gamepad column added to Shortcut Keys window.
- Single-button capture working end-to-end.
- Chord capture (cardinality 2-3, including modifier-only) working end-to-end.
- Trigger capture working with threshold + hysteresis.
- Multiple bindings per shortcut (replace/append modes).
- Cancel (ESC, right-click), idle timeout, hold timeout.
- Largest-match-wins fire-time resolution (newly written for gamepad — keyboard side untouched).
- Conflict-detection at bind time.

Out of 1.7's scope, deferred to follow-up:
- Glyph artwork for buttons (text labels suffice).
- Per-controller binding profiles (one binding set applies to all enumerated controllers).
- Hold-vs-tap differentiation specifically for gamepad inputs (existing keyboard hold/tap model carries over via the shortcut definition itself, so this is implicit).

### 1.8 Backward compat

Existing users have `shortcuts.json` with only string-formatted keyboard bindings (no `PAD ...` strings). On first load with the new code:

- Detection: presence/absence of any `PAD ...` string in any binding entry. Optionally, presence of the new `"_version": 2` top-level key.
- v1 file loaded: keep all keyboard bindings as-is, fill in default `PAD ...` binding for each known shortcut (from 1.4), save back at v2.
- v2 file loaded: load both as-is.
- Custom keyboard rebindings the user did pre-gamepad survive untouched.
- The existing OPENRCT2MINI cut-44b launch.sh wipe of stale `shortcuts.json` on the device stays as a belt-and-braces fallback for the device — Mini-side users don't have gamepad rebindings worth preserving anyway, since Phase 2's vendor-SDL2 update will produce different button event types from before.

### 1.9 Analog sticks: cursor and camera

Host pads (DualShock, Xbox, etc.) have two analog sticks. Mini has none. The defaults:

- **Left stick → cursor motion.** Replaces the previous OR-of-keyboard-and-controller cursor input. Magnitude proportional to deflection past deadzone, applied as smooth pixels-per-frame velocity. D-pad (held shortcut) still works in parallel for users who prefer digital.
- **Right stick → camera pan.** The viewport already has an analog-scroll path used by `InputManager::processAnalogueInput()` for the LEFT stick — this sub-phase **adds** right-stick polling alongside, with separate accumulator and config knobs. Left stick is REPOINTED at the cursor system, leaving right-stick to handle camera. Both sticks are direct-polled (read each frame from `SDL_GameControllerGetAxis`) — they don't go through the queued `joyAxis` events. The queued events are for shortcut-bindable stick-direction tokens (1.2c) only.

Configuration knobs in `Config::general`:

- `gamepadDeadzone` (existing — repurposed as cursor deadzone, default ~24%)
- `gamepadSensitivity` (existing — repurposed as cursor sensitivity, default 1.0)
- `gamepadCameraDeadzone` (NEW, default 8000 / 32767)
- `gamepadCameraSensitivity` (NEW, default 1.0)
- `gamepadInvertCameraY` (NEW, default false)

Stick-click buttons (`PAD L3`, `PAD R3`) are bindable via the normal shortcut system from 1.7 — left-click cursor-reset-to-centre, right-click camera-reset-to-followed-entity, etc., as user choice.

### 1.10 Console / chat / OSK gating policy

The existing `InputManager::process(const InputEvent&)` early-returns for keyboard events when console / chat / textinput / loadsave / loadsaveOverwritePrompt / widgetTextBox is active. **Gamepad events skip this path today** and fall through to `ShortcutManager::processEvent` directly — meaning controller buttons can fire game shortcuts WHILE console is open, which is wrong.

Add a parallel gating policy for `joyButton` and `joyAxis` events:

| Active context | Allowed gamepad shortcuts | Other gamepad input goes to |
|---|---|---|
| Console open | `kDebugToggleConsole`, OSK-mode console scroll (D-pad up/down), enter (south face) | Console text input |
| Chat open | None (close-only — START or ESC equivalent) | Chat text input |
| OSK active | OSK navigation (D-pad, A=press, B=cancel) | OSK |
| Text-input widget focused | None | Widget text input |
| LoadSave / OverwritePrompt | Up/Down/Enter/Escape only | Dialog navigation |
| Normal | All gamepad shortcuts | n/a |

Implementation: extend `InputManager::process(const InputEvent&)` to apply the same context-mode gating for gamepad as it does for keyboard.

### 1.11 Haptic feedback (rumble)

This sub-phase has two parts: the API/plumbing (cheap), and per-game-event integration (expensive — needs new data plumbed through the engine).

**Plumbing (the easy part):**

A thin `HapticEvent` API added to libopenrct2:

```cpp
namespace HapticEvent {
    void pulse(float intensity, uint32_t duration_ms);
    void rumble(float lowFreq, float highFreq, uint32_t duration_ms);
}
```

Routes to `UiContext::RumbleControllers(low, high, duration)`. UiContext walks `_inputManager.getGameControllers()` and calls `SDL_GameControllerRumble` on each. Capability detection via `SDL_GameControllerHasRumble()`; silent no-op when absent.

Config knobs:
- `gamepadRumbleIntensity` (0.0 - 1.0, default 1.0)
- Per-event toggles in Options > Controls > Haptics: coaster, crash, construction, news.

**Per-game-event integration (the harder part):**

| Event | Data path |
|---|---|
| Crash | hook into existing `RideCrash` notification — already a discrete event; one-shot 500 ms full-intensity pulse. Shippable. |
| Construction tick / refusal flash | hook the existing OSK rejection-flash signal — already a discrete event. Shippable. |
| Critical news pulse | hook the news-add path; needs the news severity classification from 1.12 to gate "critical" vs other types. **Depends on 1.12.** |
| Coaster ride (vertical-G proportional) | requires sampling a vehicle entity's per-frame state. Today: `WindowFollowSprite` exists and sets a `viewport_target_sprite` on a window, but there is NO existing "current G-force on ridden vehicle" signal. Need new code: detect that the main window's followed sprite is a `Vehicle` entity, sample its `velocity` / `acceleration` fields per frame, derive a vertical-G estimate from track-piece data, feed into `HapticEvent::rumble`. **This is a non-trivial sub-project of its own.** Documented as a stretch goal of 1.11; ship without it if it slips. |

The plumbing ships first (crash + construction work immediately). Coaster-G can land in a later revision.

Hard-disabled when `SDL_GameControllerHasRumble()` returns false for all enumerated pads — silent no-op. The haptic API stays callable so game code doesn't need branches.

Mini support: vibration motor wired through in Phase 2.6.

### 1.12 News severity classification (foundation for 1.13 LED)

`NewsItem.h::ItemType` enumerates CATEGORIES (peep / money / research / peeps / award / graph / campaign / peepOnRide / blank / ride). These are NOT severity levels — "guest drowning" and "guest happy" are both `peep`.

LED colour mapping (1.13) needs severity, not category. Two implementation options:

(a) **Severity lookup table by news StringId:** for each `STR_NEWS_*` string, classify it as `critical` / `warning` / `money` / `info`. Static const table; ~50-100 entries.

(b) **Add a severity field at item creation:** every `News::AddItemToQueue` callsite passes a severity. More invasive (touches every news producer) but cleaner long-term.

Plan picks (a) for Phase 1 (less invasive, ships faster); (b) is a possible follow-up cleanup.

Severity buckets:
- **Critical (red):** ride breakdown with stranded passengers, guest drowning, vandalism spree, park rating critical.
- **Warning (yellow):** mechanic on the way, ride queue overflowing, financial warning.
- **Money (green):** loan repaid, scenario goal milestones, year-end profit.
- **Info (off):** generic notifications (guest spawned, ride built, etc.) — no LED flash, no rumble.

This severity field also gates the "critical news rumble pulse" from 1.11.

### 1.13 Player LED (DualShock lightbar)

DualShock 4 / 5 expose a controllable RGB LED via `SDL_GameControllerSetLED(r, g, b)`. Wire 1.12's severity buckets to LED colours:

```
critical → red    (255, 0, 0)
warning  → yellow (255, 200, 0)
money    → green  (0, 255, 0)
info     → no flash; LED returns to idle off after current flash decays
```

Implementation: `LedEvent::flash(severity, duration_ms)` API in libopenrct2; routes to `UiContext::SetControllerLED`. Capability detection via `SDL_GameControllerHasLED()` — silently no-op when absent (Xbox pads, the Mini, etc.).

Config knobs:
- `gamepadLedEnabled` (default true)
- `gamepadLedBrightness` (0.0 - 1.0, default 0.5 — DualShocks are bright)

### Phase 1 acceptance criteria

- Plug a DualShock 4 (primary test target) or Xbox controller into the host build. D-pad moves cursor digitally; left stick moves cursor analogue; right stick pans camera; face buttons click / cancel / shade / swap; shoulders rotate; triggers zoom. Controller is fully functional.
- Same binary still responds to keyboard and mouse identically to before.
- Console/chat/OSK contexts gate gamepad input correctly per 1.10.
- All `_vKb*`-driven chord behaviors from rev W*/cuts 38b/43/44/60/61 (cursor velocity, fast-cursor, Z-lock, shade-tap-vs-hold, R1+X close window) work via the shortcut system, not via scancode synthesis.
- Options > Controls > Shortcut Keys shows current gamepad bindings, allows rebinding (single button, chord, modifier-only chord, trigger).
- `shortcuts.json` has both keyboard and gamepad bindings persisted in the extended string format. Old files migrate cleanly via 1.8.
- News items have severity classification (1.12). Crash and construction haptics work. DualShock lightbar flashes by news severity.
- Coaster ride rumble: shippable WITHOUT it if the vehicle-G data path slips; otherwise present.
- Options > Controls > Haptics and Options > Controls > LED expose intensity / per-event toggles.
- Mini binary build is unchanged in behavior (still uses keyboard-translation in vendor SDL2 — Phase 2 territory).
- Audit document exists at `gamepad-audit.md`.

## Phase 2 — Mini: real SDL controller events from the vendor SDL2 driver

Once Phase 1 is shipped and tested, modify `dist/vendor/sdl2_miyoo-main/sdl2/src/joystick/mmiyoo/SDL_joystick_mmiyoo.c` and `SDL_event_mmiyoo.c` so the device produces real `SDL_CONTROLLER*` events. The Phase-1 binary, recompiled against the updated vendor SDL2, then runs on Mini with controller input identical to host with a plugged-in pad.

### 2.1 Make `MMIYOO_JoystickOpen` allocate state

Currently sets `nbuttons=14, naxes=2, nhats=0` and returns. Needs to remember the `SDL_Joystick*` handle so `MMIYOO_JoystickUpdate` can target it. Wire a static pointer (single device, single instance is fine) plus track previous-frame button state for diff computation.

### 2.2 Make `MMIYOO_JoystickUpdate` actually deliver events

Move the keypad-bitmap polling — currently in `MMIYOO_PumpEvents` of `SDL_event_mmiyoo.c` — into `MMIYOO_JoystickUpdate`. On each call:

- Read the current keypad bitmap (existing `evt.keypad.bitmaps`).
- Diff against the previous frame's bitmap.
- For each changed bit, call `SDL_PrivateJoystickButton(joystick, button_idx, SDL_PRESSED|SDL_RELEASED)`.
- For triggers (if mapped to axes): `SDL_PrivateJoystickAxis(joystick, axis_idx, value)`.
- Save current bitmap as previous.

### 2.3 Implement `MMIYOO_JoystickGetGamepadMapping` (vendor signature)

**Important:** the vendor SDL2 diverged from upstream on this function's signature. `SDL_sysjoystick.h` line 192 declares:

```c
SDL_bool(*GetGamepadMapping)(int device_index, SDL_GamepadMapping *out);
```

This is an out-parameter `SDL_GamepadMapping*`, NOT upstream's `char* mapping_string`. Before coding 2.3, **read the vendor's `SDL_GamepadMapping` struct definition** to learn its fields (likely a struct of indices for each controller button → joystick button index). The implementation fills the struct field-by-field rather than constructing a mapping string.

Canonical Mini → SDL_GameController mapping (the values to fill into the struct):

```
MYKEY_UP    → SDL_CONTROLLER_BUTTON_DPAD_UP
MYKEY_DOWN  → SDL_CONTROLLER_BUTTON_DPAD_DOWN
MYKEY_LEFT  → SDL_CONTROLLER_BUTTON_DPAD_LEFT
MYKEY_RIGHT → SDL_CONTROLLER_BUTTON_DPAD_RIGHT
MYKEY_A     → SDL_CONTROLLER_BUTTON_B    // east face — matches Phase 1.4
MYKEY_B     → SDL_CONTROLLER_BUTTON_A    // south face
MYKEY_X     → SDL_CONTROLLER_BUTTON_Y    // north face
MYKEY_Y     → SDL_CONTROLLER_BUTTON_X    // west face
MYKEY_L1    → SDL_CONTROLLER_BUTTON_LEFTSHOULDER
MYKEY_R1    → SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
MYKEY_L2    → SDL_CONTROLLER_AXIS_TRIGGERLEFT  (digital → axis 0/MAX)
MYKEY_R2    → SDL_CONTROLLER_AXIS_TRIGGERRIGHT (digital → axis 0/MAX)
MYKEY_SELECT→ SDL_CONTROLLER_BUTTON_BACK
MYKEY_START → SDL_CONTROLLER_BUTTON_START
MYKEY_MENU  → SDL_CONTROLLER_BUTTON_GUIDE
```

This is the canonical Mini → SDL_GameController mapping that anyone reading the gamepad code will reference.

### 2.4 Strip keyboard-translation from `MMIYOO_PumpEvents`

The `code[]` table and the `SDL_SendKeyboardKey()` loop in `MMIYOO_PumpEvents` go away. Keypad-bitmap polling moves to the joystick driver per 2.2. The `MMIYOO_KEYPAD_MODE` arm becomes an empty stub (or the function deletes its keypad path entirely, leaving only mouse-mode handling if anything still wants it).

`MMIYOO_PumpEvents` is still called by SDL's event-pump tick — it just becomes a no-op for keypad.

### 2.5 Verify identical behavior across platforms

Build the Phase-1 binary against the updated vendor SDL2. Deploy to Mini. The same input mappings, the same shortcut behavior, the same defaults — no platform-specific code path for input. That's the proof Phase 1's design was right.

### 2.6 Mini rumble: wire `MMIYOO_JoystickRumble` to the hardware vibration motor

Currently the vendor stub returns `SDL_Unsupported`. The Miyoo Mini has a vibration motor; the OnionUI source / community drivers already document the sysfs / ioctl interface. Implementation:

- Identify the control node (likely `/sys/class/...` or a specific `/dev/...` ioctl) by reading OnionUI's drivers and any community `rumble` utility on the device.
- Open the control handle once at `MMIYOO_JoystickInit` time, close at `MMIYOO_JoystickQuit`.
- `MMIYOO_JoystickRumble(joystick, low_freq, high_freq, duration_ms)` writes the requested intensity (mapped to whatever range the hardware accepts — typically 0-100 or 0-255) and schedules a deferred "stop" at `duration_ms` later.
- Mini hardware likely has only one motor (no dual eccentric like a DS4) so `low_freq` and `high_freq` are averaged before being written.
- If the control node is missing or open fails, fall back to `SDL_Unsupported` silently — same UX as host without rumble support.

Phase-1 game code calling `HapticEvent::pulse()` then drives the Mini motor without any code change in OpenRCT2mini.

LED control on Mini is **out of scope** — the device has no controllable colour LED. The `SDL_GameControllerHasLED()` check from 1.13 returns false on Mini and the LED path is silently skipped.

### Phase 2 acceptance criteria

- `MMIYOO_JoystickGetGamepadMapping` fills the vendor's `SDL_GamepadMapping` out-struct correctly; `SDL_IsGameController(0) == SDL_TRUE` on Mini.
- `SDL_GameControllerOpen(0)` succeeds; `SDL_GameControllerName` returns "MMiyoo Joystick" or similar.
- Pressing physical buttons on the Mini produces `SDL_CONTROLLERBUTTONDOWN/UP` events with the expected indices.
- L2/R2 produce `SDL_CONTROLLERAXISMOTION` events on the trigger axes.
- `MMIYOO_PumpEvents` no longer synthesises keyboard events for any keypad button.
- `MMIYOO_JoystickRumble` drives the device's vibration motor; coaster ride / crash haptics are felt on device.
- The OpenRCT2mini binary built in Phase 1 — without modification — runs on Mini and behaves identically to host with a plugged pad (modulo the Mini lacking sticks and LED).

## Sequencing & checkpoints

| # | Phase | Code touched | User-visible change | Build target |
|---|---|---|---|---|
| 1.1 | Audit | none — write `gamepad-audit.md` | none | none |
| 1.2 | Shortcut data model + held-set tracking | `ShortcutManager`, `ShortcutInput`, `InputManager` (new `_heldGamepadButtons`) | none yet | host |
| 1.3 | Wire controller events into shortcuts (incl. trigger threshold) | `UiContext::ProcessMessages`, `InputManager` | gamepad starts firing actions | host |
| 1.4 | Default bindings + conflict audit | shortcut defaults table | gamepad works out of the box | host |
| 1.5 | Migrate `_vKb*`-based behaviors to shortcut system | `UiContext::Update` | none (refactor) — chord behaviors preserved | host |
| 1.6 | Drop `_vKb*` flags + Q/A scancode synthesis + direct GC poll | `UiContext` | none (refactor) | host |
| 1.7 | Rebinding UI | Options > Controls > Shortcut Keys window | user can rebind gamepad (single, chord, trigger) | host |
| 1.8 | Backward compat | shortcuts.json loader | existing users get gamepad defaults filled in | host |
| 1.9 | Analog sticks | `InputManager::processAnalogueInput` (extended for 2nd stick), viewport scroll | left stick → cursor, right stick → camera | host |
| 1.10 | Console / chat / OSK gating | `InputManager::process(const InputEvent&)` | gamepad respects context | host |
| 1.11 | Haptic plumbing + crash/construction integration | new `HapticEvent` API + UiContext rumble routing | crash / construction rumble | host |
| 1.12 | News severity classification | new severity table in `NewsItem.cpp` | foundation only — not yet user-visible | host |
| 1.13 | LED indicator | new `LedEvent` API + UiContext LED routing | DualShock lightbar flashes by news severity | host |
| 1.11* | Coaster-G rumble | vehicle entity sampling | rumble proportional to coaster G — stretch | host |
| 2.1 | Vendor SDL2: joystick state | `SDL_joystick_mmiyoo.c` | none (build-only) | Mini SDL2 |
| 2.2 | Vendor SDL2: emit events | `SDL_joystick_mmiyoo.c` | Mini emits `SDL_CONTROLLER*` | Mini SDL2 |
| 2.3 | Vendor SDL2: gamepad mapping (out-struct) | `SDL_joystick_mmiyoo.c` | `SDL_IsGameController` true on Mini | Mini SDL2 |
| 2.4 | Vendor SDL2: strip keyboard translation | `SDL_event_mmiyoo.c` | Mini stops faking keyboard events | Mini SDL2 |
| 2.5 | End-to-end on device | none — deploy and test | Mini behaves exactly like host w/ pad | both |
| 2.6 | Vendor SDL2: rumble | `SDL_joystick_mmiyoo.c` + Mini sysfs/ioctl | Mini vibrates on coaster ride / crash | Mini SDL2 |

After each row: clean build, smoke test, commit. Build a deployable Mini package after 2.5.

## Out of scope

- **Touch input.** Mini has no touch. Host touchpad/tablet path stays as-is.
- **Pretty button icons / glyph artwork.** Use text labels (`DPAD_UP`, `B`) in 1.7. Bitmap glyphs can come post-Phase 2.
- **Multi-controller / split-screen multiplayer.** Plan supports all enumerated controllers receiving identical bindings (single-player, any pad fires the same shortcut). Designing per-pad-bound-to-player-N input — i.e. distinguishing "Player 1 pad" from "Player 2 pad" at fire time — is deferred. Architecture-wise, the shortcut firing pipeline routes events based on `SDL_JoystickInstanceID`; introducing a per-instance "owning player" routing key later is additive and doesn't require revisiting Phase 1's data model. Noted here so we don't paint ourselves into a corner.

## Resolved decisions

Decisions previously listed as open questions, now committed:

- **Storage format.** `shortcuts.json` stays as plain-string bindings (NOT typed JSON). New `PAD ...` token vocabulary added to the existing string parser. Backward compat by detection of `PAD ...` strings or optional `"_version": 2` top-level key.
- **Conflict resolution between keyboard and gamepad firing the same shortcut at the same instant.** Same shortcut firing twice in the same tick is idempotent — game actions consume one fire per tick regardless of how many bindings hit. The shortcut dispatcher dedupes by shortcut ID per frame.
- **Chord UX.** Resolved in 1.7.1–1.7.6: multiple bindings per shortcut, chord cardinality cap 3 (incl. modifier-only), at most one action button per chord, largest-match-wins fire-time resolution (NEW logic, not shared with keyboard side), release-all-to-confirm capture with idle/hold timeouts, 50% press / 30% release trigger hysteresis.
- **Trigger axis vs button mapping in vendor `SDL_GamepadMapping`.** Triggers are exposed as axes. SDL's standard mapping for Xbox / PS pads does the same — keeps L2/R2 as analog, OpenRCT2mini's internal threshold logic from 1.7.1 handles the digital interpretation. Triggers as digital buttons would lose the analog signal we'd want for graduated zoom-speed in a later polish pass.
- **Gamepad chord modifier infrastructure.** Net-new code: `InputManager::_heldGamepadButtons` set updated on every `joyButton` and `joyAxis` threshold transition; `RegisteredShortcut::matches(InputEvent)` extended to consult it for non-keyboard kinds. The `_vGamepadMod` flag from rev W* gets folded into this set during 1.5/1.6 cleanup.
- **News severity.** Static lookup table by news StringId for Phase 1 (less invasive than threading a severity field through every news producer). Optional follow-up cleanup adds an explicit severity field at item creation.

## Reference files

- Existing input architecture:
  - `src/openrct2-ui/UiContext.cpp` — SDL event pump, virtual cursor, controller polling, Q/A scancode synthesis
  - `src/openrct2-ui/input/InputManager.cpp` / `.h` — event queue, controller enumeration, analog stick polling
  - `src/openrct2-ui/input/ShortcutManager.cpp` / `.h` — registered shortcuts, JSON load/save (string format)
  - `src/openrct2-ui/input/ShortcutInput.cpp` — string parser / serialiser, modifier comparison, `matches()` (extends in 1.2)
  - `src/openrct2-ui/input/Shortcuts.cpp` — shortcut catalog with default bindings (extends in 1.4)
  - `src/openrct2-ui/input/ShortcutIds.h` — shortcut ID string constants
  - `src/openrct2-ui/windows/ShortcutKeys.cpp` — rebinding UI (extends in 1.7)
  - `src/openrct2/management/NewsItem.h` — `enum class ItemType`, classification source for 1.12
- Vendor SDL2 (Phase 2 only):
  - `dist/vendor/sdl2_miyoo-main/sdl2/src/joystick/mmiyoo/SDL_joystick_mmiyoo.c` — joystick driver stub
  - `dist/vendor/sdl2_miyoo-main/sdl2/src/joystick/SDL_sysjoystick.h` — vendor's `SDL_GamepadMapping` struct + signature divergence from upstream
  - `dist/vendor/sdl2_miyoo-main/sdl2/src/video/mmiyoo/SDL_event_mmiyoo.c` — keyboard-translation event pump
  - `dist/vendor/sdl2_miyoo-main/sdl2/include/SDL_gamecontroller.h` — `SDL_GameControllerHasRumble` / `HasLED` availability (confirmed present)
- Plan documents that this one builds on:
  - `OpenRCT2mini-Plan.md` — overall fork plan
  - `osk-plan.md` — on-screen keyboard plan (related, different concern)
