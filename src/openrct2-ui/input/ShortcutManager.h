/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "InputManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <openrct2/core/FileSystem.hpp>
#include <openrct2/localisation/StringIdType.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OpenRCT2
{
    struct IPlatformEnvironment;
}

namespace OpenRCT2::Ui
{
    // OPENRCT2MINI gamepad-plan 1.2: axis-as-button encoding used by the
    // shortcut binding model and the InputManager held-set. SDL only
    // defines ~21 controller buttons (indices 0..20 inclusive in modern
    // SDL2 headers); kPadAxisAsButtonBase = 64 is comfortably past that,
    // leaves room for future SDL additions, and is small enough that
    // std::set element values stay in single-byte hash territory.
    //
    //   axis-as-button-index = kPadAxisAsButtonBase + (axis * 2)
    //                                               + (direction == +1 ? 0 : 1)
    //
    // where `axis` is the SDL_CONTROLLER_AXIS_* index (0..5) and
    // `direction` is +1 (positive) or -1 (negative). LEFTTRIGGER
    // (axis 4, +1 dir only) and RIGHTTRIGGER (axis 5, +1 dir only)
    // take indices 72 and 74. Stick directions also use the negative-
    // direction slot: e.g. STICK_L UP is (axis 1 LEFTY, direction -1)
    // → 64 + 2 + 1 = 67.
    //
    // Single source of truth — every site that converts an SDL axis +
    // direction to the encoded held-set value goes through this header.
    // Previously this constant and the formula were duplicated across
    // ShortcutInput.cpp, InputManager.cpp (twice), and ShortcutManager.cpp;
    // drift between sites would have silently broken trigger / stick-
    // direction chord matching.
    inline constexpr uint32_t kPadAxisAsButtonBase = 64;

    inline constexpr uint32_t encodeAxisAsButton(int32_t axis, int8_t direction) noexcept
    {
        return kPadAxisAsButtonBase + (static_cast<uint32_t>(axis) * 2u)
            + (direction == 1 ? 0u : 1u);
    }

    struct ShortcutInput
    {
    public:
        InputDeviceKind kind{};
        uint32_t modifiers{};
        uint32_t button{};

        // OPENRCT2MINI gamepad-plan 1.2: chord-modifier prerequisites for
        // joyButton / joyAxis bindings. Empty for non-chord bindings (single
        // button, single trigger, single keyboard chord). For chord bindings
        // ("PAD L1+B"), this holds the held-modifier buttons (in this case
        // {LEFTSHOULDER}); the `button` field carries the action button (B).
        // Indices follow SDL_CONTROLLER_BUTTON_* for buttons, plus the
        // axis-as-button encoding from ShortcutInput.cpp for trigger /
        // stick-direction modifiers.
        std::vector<uint32_t> chordModifiers;

        // OPENRCT2MINI gamepad-plan 1.2: axis matching parameters for
        // joyAxis kind (triggers + stick directions). axisDirection is
        // -1 / +1 / 0; axisThreshold is the signed magnitude past which
        // the axis is considered "pressed". For triggers (range 0-32767)
        // direction is +1 and threshold is around 16384 (50%); for stick
        // directions, direction is ±1 depending on which way the binding
        // points. Both unused for non-axis kinds.
        int32_t axisThreshold{};
        int8_t axisDirection{};

        // OPENRCT2MINI hold-binding refactor: when non-zero, this binding
        // fires its action only after the input has been continuously
        // held for `holdMs` milliseconds. Tap bindings (holdMs == 0) on
        // the same input are deferred until release: if released before
        // any co-bound hold threshold, the tap fires; if released after,
        // the tap is suppressed (the hold already fired). Default 0 =
        // legacy fire-on-press behaviour. Token: "HOLD " prefix on the
        // binding string sets holdMs = 500 (the only value supported
        // for now). The dispatch lives in ShortcutManager::processEvent
        // + tickHoldShortcuts; per-frame timer ticking happens in
        // InputManager::process.
        uint16_t holdMs{};

        ShortcutInput() = default;
        ShortcutInput(std::string_view value);
        std::string toString() const;
        std::string toLocalisedString() const;

        // OPENRCT2MINI gamepad-plan 1.2: held-set parameter for chord
        // matching. Pass nullptr (default) when calling from a context
        // that doesn't have access to InputManager (e.g. from
        // RegisteredShortcut::matches' fast path during keyboard event
        // processing); chord bindings simply won't match in that case.
        // Pass a real held-set when InputManager is dispatching a
        // joyButton/joyAxis event so chord prerequisites can be checked.
        bool matches(const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons = nullptr) const;

        static std::optional<ShortcutInput> fromInputEvent(const InputEvent& e);

    private:
        bool appendModifier(std::string& s, uint32_t left, uint32_t right, bool localised) const;
        static std::string_view getModifierName(uint32_t key, bool localised);
        static std::string_view getLocalisedKeyName(uint32_t key);
        std::string toString(bool localised) const;
    };

    class RegisteredShortcut
    {
    public:
        std::string id;
        StringId localisedName = kStringIdNone;
        std::string customName;
        std::vector<ShortcutInput> standard;
        std::vector<ShortcutInput> current;
        std::function<void()> action;
        size_t orderIndex = static_cast<size_t>(-1);

        RegisteredShortcut() = default;
        RegisteredShortcut(std::string_view _id, std::string_view _name, const std::function<void()>& _action)
            : id(_id)
            , customName(_name)
            , action(_action)
        {
        }

        RegisteredShortcut(std::string_view _id, StringId _localisedName, const std::function<void()>& _action)
            : id(_id)
            , localisedName(_localisedName)
            , action(_action)
        {
        }

        RegisteredShortcut(
            std::string_view _id, StringId _localisedName,
            [[maybe_unused]] std::string_view _defaultChord, const std::function<void()>& _action)
            : id(_id)
            , localisedName(_localisedName)
            // OPENRCT2MINI cut 44: drop the default keybind. The Miyoo Mini
            // has no keyboard, the device's D-pad / face buttons are wired
            // to virtual-cursor handling (cut 38b / 43), and the default
            // bindings (arrows / Z / X / C / etc) collide with that. The
            // shortcut stays registered so it shows up in the rebind UI;
            // the user can map it to whatever button they want.
            , action(_action)
        {
        }

        RegisteredShortcut(
            std::string_view _id, StringId _localisedName,
            [[maybe_unused]] std::string_view _defaultChordA,
            [[maybe_unused]] std::string_view _defaultChordB,
            const std::function<void()>& _action)
            : id(_id)
            , localisedName(_localisedName)
            // OPENRCT2MINI cut 44: as above, no default keybinds.
            , action(_action)
        {
        }

        std::string_view getTopLevelGroup() const;
        std::string_view getGroup() const;
        // OPENRCT2MINI gamepad-plan 1.2: held-set parameter forwarded to
        // ShortcutInput::matches for chord prerequisite checking. Pass
        // nullptr from keyboard / mouse callers; pass the live
        // _heldGamepadButtons from InputManager when dispatching joyButton
        // / joyAxis events.
        bool matches(const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons = nullptr) const;
        bool isSuitableInputEvent(const InputEvent& e) const;
        std::string getDisplayString() const;

        // OPENRCT2MINI gamepad-plan 1.7a: per-column display strings.
        // The Input Bindings window splits each row into three columns
        // (Keyboard / Mouse / Gamepad) — each column shows only the
        // bindings whose InputDeviceKind matches.
        //
        // Keyboard column: kind == keyboard.
        // Mouse column:    kind == mouse (added in the mouse-column
        //                  refactor — previously mouse bindings were
        //                  grouped with keyboard).
        // Gamepad column:  kind == joyButton, joyHat, or joyAxis.
        std::string getKeyboardDisplayString() const;
        std::string getMouseDisplayString() const;
        std::string getGamepadDisplayString() const;

    private:
    };

    // OPENRCT2MINI gamepad-plan 1.7b: per-column capture mode. Tells
    // the capture state machine in ShortcutManager::processEvent
    // which device kind the user clicked to rebind, so:
    //   * keyboard mode: accept keyboard + mouse-button events,
    //     reject joyButton / joyHat / joyAxis. Replace only the
    //     keyboard/mouse bindings in `current`.
    //   * gamepad mode: accept joyButton / joyHat / joyAxis, reject
    //     keyboard / mouse. Replace only the gamepad bindings.
    //   * any: legacy behaviour — accept any event, replace ALL
    //     bindings (used by the existing keyboard-only code paths
    //     that haven't yet been routed through the per-column UI).
    enum class PendingShortcutKind : uint8_t
    {
        any,
        keyboard,
        mouse,
        gamepad,
    };

    // OPENRCT2MINI gamepad-plan 1.7e: replace-vs-append capture mode.
    // Default replace blows away any existing same-kind bindings on
    // the shortcut (legacy "rebind" semantics). Append keeps existing
    // bindings and tacks the new one onto current[]. Selected via
    // a checkbox in the rebind modal.
    enum class PendingShortcutMode : uint8_t
    {
        replace,
        append,
    };

    class ShortcutManager
    {
    private:
        IPlatformEnvironment& _env;
        std::string _pendingShortcutChange;
        // OPENRCT2MINI gamepad-plan 1.7b: which kind the active capture
        // session is filtering for. Defaults to `any` for callers that
        // don't yet know about per-column capture; the per-column
        // ShortcutKeys window passes an explicit value.
        PendingShortcutKind _pendingShortcutKind = PendingShortcutKind::any;
        // OPENRCT2MINI gamepad-plan 1.7e: replace vs append.
        PendingShortcutMode _pendingShortcutMode = PendingShortcutMode::replace;

        // OPENRCT2MINI gamepad-plan 1.7c: chord capture state. While a
        // gamepad capture session is active we accumulate the held
        // gamepad-button set on each DOWN event. Commit rules:
        //
        //   1 button (no chord):  commit on release-all, same as the
        //                         original single-button capture.
        //   2 buttons:            commit when the 5-second hold timer
        //                         expires (counts down from the most
        //                         recent DOWN) — the user can't
        //                         realistically release both at the
        //                         exact same instant, and "press +
        //                         hold to lock" is a clearer UX than
        //                         "press, release everything fast".
        //   3 buttons:            commit immediately on the third DOWN
        //                         (cardinality cap is 3 per the plan
        //                         §1.7.1 — additional buttons would
        //                         have nowhere to fit).
        //   ESC / right-click:    cancel (1.7d).
        //
        // _captureMaxSize: largest |heldGamepadButtons| seen so far.
        // _captureMaxActionEvent: the DOWN event that grew the held-set
        //   to that maximum — its kind / button / axis fields become
        //   the action button of the committed binding.
        // _captureMaxModifiers: the other entries in the held-set at
        //   that moment (encoded as in InputManager::_heldGamepadButtons,
        //   which matches the format ShortcutInput::matches consults).
        // _captureLastDownMs: SDL_GetTicks() at the most recent DOWN
        //   event. Used to drive the 5-second 2-button-chord countdown
        //   evaluated by updatePendingCapture(). Zero means no countdown
        //   active (size 0 or 1 — wait for release-all).
        //
        // All reset to empty by setPendingShortcutChange() and by the
        // commit path. Keyboard captures don't use any of this — the
        // existing single-event flow handles keyboard chords via SDL's
        // modifier bits.
        size_t _captureMaxSize = 0;
        InputEvent _captureMaxActionEvent{};
        std::vector<uint32_t> _captureMaxModifiers;
        uint32_t _captureLastDownMs = 0;
        // OPENRCT2MINI gamepad-plan 1.7d: timestamps for the two
        // capture timeouts.
        //   * idle (8s): cancel a session that received no DOWN
        //     events at all — protects users who clicked the column
        //     by mistake.
        //   * hold (5s): commit whatever's currently held. Lets the
        //     user bind a single trigger by holding it without ever
        //     adding a second input.
        // _captureSessionStartMs is set in setPendingShortcutChange,
        // _captureLastDownMs is reset on each accepted DOWN event.
        uint32_t _captureSessionStartMs = 0;
        // OPENRCT2MINI gamepad-plan 1.7g: when a conflict is detected
        // during commit, this holds the localised name of the
        // shortcut the new binding collided with. ChangeShortcutWindow
        // displays it under the prompt; cleared on next DOWN.
        std::string _captureRejectedConflictName;

        // OPENRCT2MINI hold-binding capture: set true by the 5-second
        // hold-timeout path in updatePendingCapture; consumed by
        // commitPendingCapture which copies it to the captured
        // ShortcutInput's holdMs (500). Reset to false on every
        // session start and after each commit. Lets the same capture
        // flow yield either a tap binding (commit on release-before-
        // 5s) or a hold binding (commit on 5s-elapsed).
        bool _captureCommitAsHold = false;

        // OPENRCT2MINI gamepad-plan 1.7c: shared commit path for the
        // gamepad chord state machine — used by the "3-button cap",
        // "release-all", and "5s timer expiry" paths. Builds the
        // ShortcutInput from _captureMaxActionEvent + _captureMax-
        // Modifiers, applies the kind-filtered replace from 1.7b,
        // saves the user bindings, closes the change modal, and
        // resets all the capture state.
        void commitPendingCapture();

        // OPENRCT2MINI hold-binding refactor: pending-press tracking
        // for inputs that have at least one HOLD binding co-bound to
        // them. On press, processEvent searches all bindings matching
        // the input and notes the maximum holdMs across the matches;
        // if any is non-zero, firing is deferred and an entry is added
        // here. tickHoldShortcuts polls each entry per-frame and fires
        // hold actions when the threshold elapses (latching holdFired).
        // On release, processEvent looks up the entry by (kind, button)
        // and, if !holdFired, fires the queued tap actions before
        // erasing the entry.
        struct HoldPendingState
        {
            // Identifies the input for release-side matching. Modifiers
            // are intentionally NOT part of the key — once a press is
            // tracked, releasing under a different modifier set still
            // ends the tracking (e.g. user presses C, presses Shift,
            // releases C — should still fire the C tap).
            InputDeviceKind kind{};
            uint32_t button{};

            uint32_t pressStartMs = 0;
            uint32_t holdMs = 0;
            bool holdFired = false;

            // Shortcut IDs to fire on release-before-threshold (taps)
            // and on threshold-elapsed (holds). Strings are stored by
            // value to avoid dangling string_views if a shortcut's
            // backing storage moves; the lookup at fire time goes
            // through getShortcut() to translate to the action.
            std::vector<std::string> tapShortcutIds;
            std::vector<std::string> holdShortcutIds;
        };
        std::vector<HoldPendingState> _holdPending;

        static std::optional<ShortcutInput> convertLegacyBinding(uint16_t binding);
        void loadLegacyBindings(const fs::path& path);
        void loadUserBindings(const fs::path& path);
        void saveUserBindings(const fs::path& path);

        // We store the IDs separately so that we can safely use them for string_view in the map
        std::vector<std::unique_ptr<std::string>> _ids;

    public:
        std::unordered_map<std::string_view, RegisteredShortcut> shortcuts;

        ShortcutManager(IPlatformEnvironment& env);
        ShortcutManager(const ShortcutManager&) = delete;

        void loadUserBindings();
        void saveUserBindings();

        void registerShortcut(RegisteredShortcut&& shortcut);
        template<typename... Args>
        void registerShortcut(Args&&... args)
        {
            registerShortcut(RegisteredShortcut(std::forward<Args>(args)...));
        }
        void registerDefaultShortcuts();

        // OPENRCT2MINI gamepad-plan 1.4: append a "PAD ..." default
        // binding to an already-registered shortcut. Populates BOTH
        // `standard` (so reset-to-default in the rebind UI restores it)
        // and `current` (so the binding is live without requiring the
        // user to rebind on first run). On subsequent runs `current` is
        // overwritten by loadUserBindings if a user file exists, so
        // user customisations take precedence over our defaults — same
        // semantics as keyboard defaults.
        //
        // Why a separate helper instead of extending the constructor:
        // OPENRCT2MINI cut 44 strips keyboard chord defaults via
        // [[maybe_unused]] in the constructors to prevent the device's
        // faked-scancode collisions. We don't want to undo that — the
        // collision is real on Mini. Gamepad PAD-tokens are a NEW
        // channel that doesn't collide with the device's keyboard
        // synthesis (vendor SDL2 emits keys, not joyButton events), so
        // they can be populated unconditionally.
        void registerPadDefault(std::string_view id, std::string_view padChord);

        // OPENRCT2MINI gamepad-plan 1.5h: append a device-friendly
        // keyboard default. Constructor-supplied keyboard defaults are
        // stripped (cut 44) to avoid the device's vendor-SDL2 faked-
        // scancode collisions — but specific scancodes that the device
        // emits for face/shoulder buttons (W/S = L2/R2, V = face Y)
        // need to drive shortcuts unconditionally. This helper opts in
        // those specific bindings without touching the global cut-44
        // strip.
        void registerKeyboardDefault(std::string_view id, std::string_view kbChord);

        // OPENRCT2MINI mouse-input refactor: append a mouse default.
        // Used for migrating the previously-hardcoded SDL_BUTTON_LEFT /
        // SDL_BUTTON_RIGHT / SDL_MOUSEWHEEL paths into the shortcut
        // system — e.g. cursor.click ← LMB, cursor.cancel ← RMB,
        // kViewGeneralZoomIn ← MOUSE WHEEL UP, etc.
        void registerMouseDefault(std::string_view id, std::string_view mouseChord);

        RegisteredShortcut* getShortcut(std::string_view id);
        void removeShortcut(std::string_view id);
        bool isPendingShortcutChange() const;
        // OPENRCT2MINI gamepad-plan 1.7b/1.7e: kind defaults to `any`
        // and mode defaults to `replace` so existing callers don't
        // need to be updated. The per-column Input Bindings window
        // passes keyboard / gamepad explicitly depending on which
        // column was clicked, and replace / append based on the
        // append-toggle in the change modal.
        void setPendingShortcutChange(
            std::string_view id, PendingShortcutKind kind = PendingShortcutKind::any,
            PendingShortcutMode mode = PendingShortcutMode::replace);

        // OPENRCT2MINI input-bindings-rework §2.3: clear all bindings
        // of a given device kind for a single shortcut. The per-cell
        // bin button in the Input Bindings list drives this (one
        // call per kind cell). Buckets joyButton / joyHat / joyAxis
        // together under PendingShortcutKind::gamepad — same grouping
        // the rebind UI uses. Saves user bindings on success. No-op
        // if the shortcut id is unknown.
        void clearBindingsOfKind(std::string_view id, PendingShortcutKind kind);

        // OPENRCT2MINI gamepad-plan 1.7e: live-update the append flag
        // while a session is active (called when the user toggles the
        // append checkbox in the rebind modal).
        void setPendingShortcutMode(PendingShortcutMode mode);
        PendingShortcutMode getPendingShortcutMode() const
        {
            return _pendingShortcutMode;
        }

        // OPENRCT2MINI gamepad-plan 1.7g: live-update for the rebind
        // modal — query the conflict-rejected message (empty if no
        // conflict has been rejected yet in this session).
        const std::string& getCaptureRejectedConflictName() const
        {
            return _captureRejectedConflictName;
        }
        // OPENRCT2MINI gamepad-plan 1.2: held-set parameter for chord
        // matching. InputManager passes its _heldGamepadButtons when
        // dispatching joyButton / joyAxis events; nullptr from keyboard
        // / mouse paths means chord-binding ShortcutInputs simply won't
        // match (defensive — they shouldn't be checked at all from those
        // paths since the deviceKind disagrees).
        //
        // OPENRCT2MINI gamepad-plan 1.10: optional `isAllowed` filter
        // (the ShortcutFilter alias). Called once per matching shortcut
        // before its action fires. Returning false suppresses the
        // shortcut for this event — used by InputManager to apply the
        // per-context allow-list (`InputManager::isShortcutAllowedIn-
        // ActiveContext`) uniformly across keyboard / mouse / joyButton /
        // joyHat / joyAxis. Default null filter means "fire everything
        // that matches" (preserves the legacy world-context behaviour
        // for any caller that doesn't want the gate). The filter is
        // also consulted on the release-path tap-fire for hold-pending
        // entries so a hold->tap resolution can't sneak past the gate
        // either.
        using ShortcutFilter = std::function<bool(const InputEvent&, std::string_view shortcutId)>;

        // OPENRCT2MINI input-plan Track 3 / Phase 3.A: routing
        // interpose. Called once per shortcut fire site (legacy
        // press path, deferred tap on release, hold-fire after
        // threshold) ahead of the action lambda. Returning true
        // suppresses the lambda — the active context strategy
        // handled the shortcut itself. Returning false (or a null
        // filter) lets the lambda fire normally.
        //
        // Default null filter preserves the pre-Phase-3.A behaviour
        // for any caller that doesn't want strategy routing. The
        // InputManager wraps its `shouldSuppressAction` in this
        // filter when calling processEvent.
        using RouteFilter = std::function<bool(const InputEvent&, std::string_view shortcutId)>;
        void processEvent(
            const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons = nullptr,
            const ShortcutFilter& isAllowed = {}, const RouteFilter& shouldSuppress = {});
        bool processEventForSpecificShortcut(
            const InputEvent& e, std::string_view id,
            const std::set<uint32_t>* heldGamepadButtons = nullptr);

        // OPENRCT2MINI gamepad-plan 1.7c: per-frame tick for the chord
        // capture countdown. InputManager::process calls this once at
        // the top of every frame with the current SDL_GetTicks() value
        // and the live held-set. When a 2-button chord has been
        // accumulating for 5 seconds without further DOWN events, the
        // chord is committed at the next tick. No-op when no capture
        // is pending or the held-set is empty / size 1 (those paths
        // commit via the event-driven release-all flow).
        void updatePendingCapture(uint32_t nowMs, const std::set<uint32_t>* heldGamepadButtons);

        // OPENRCT2MINI hold-binding refactor: per-frame tick for the
        // hold-shortcut threshold. InputManager::process calls this
        // each frame; for every entry in _holdPending, if the input
        // has been held continuously for >= holdMs and the hold action
        // hasn't fired yet, fire all matched hold shortcuts and latch
        // holdFired=true. Tap shortcuts on the same input then become
        // "swallowed" — the subsequent release event sees holdFired
        // and skips the deferred-tap fire.
        void tickHoldShortcuts(uint32_t nowMs);

        // OPENRCT2MINI hold-binding refactor: cancel any pending hold
        // tracking for the input identified by (kind, button). Called
        // from action lambdas that "consume" a chord press the
        // dispatcher would otherwise leave deferred — e.g. the
        // ALT+C / R1+C close-window chord, where letting the C tap
        // fire on release would also shade an arbitrary window. The
        // shortcut firing the consumer calls this to drop the entry
        // so the release does nothing. No-op if no entry matches.
        void cancelPendingHoldForInput(InputDeviceKind kind, uint32_t button);

        // OPENRCT2MINI overlap-warn: returns true if any OTHER
        // registered shortcut has a binding with the same fire-time
        // identity as `binding` (same kind + button + modifiers +
        // chord + holdMs). The caller's own shortcut is excluded by
        // `excludeShortcutId`, so a shortcut bound to the same input
        // twice in its own current[] is NOT flagged. Used by the
        // rebind window to colour competing bindings yellow as a
        // soft warning that two shortcuts will fire on the same
        // press; the user can still bind whatever they want.
        bool isBindingOverlapping(std::string_view excludeShortcutId, const ShortcutInput& binding) const;

        // OPENRCT2MINI gamepad-plan 1.7c: live capture preview. The
        // ChangeShortcutWindow polls these on every frame to render
        // feedback in its prompt area: which buttons are currently
        // held, what the captured max chord is so far, and how much
        // time is left on the 2-button countdown.
        //
        // - getCaptureMaxChordPreview returns the largest chord
        //   captured during this press cycle, formatted as e.g.
        //   "PAD L1+R1". Empty string when nothing yet.
        // - getCaptureCountdownMsRemaining returns ms until the
        //   2-button-chord auto-commit fires; 0 if not armed.
        bool isCaptureChordTimerArmed() const;
        uint32_t getCaptureCountdownMsRemaining(uint32_t nowMs) const;
        std::string getCaptureMaxChordPreview() const;

        static std::string_view getLegacyShortcutId(size_t index);
    };
} // namespace OpenRCT2::Ui
