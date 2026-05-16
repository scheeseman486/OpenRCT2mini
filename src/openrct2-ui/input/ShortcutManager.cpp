/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ShortcutManager.h"

#include "../UiContext.h" // OPENRCT2MINI cursor-selector-modal-plan §CS.4: GetInputManager()
#include "InputManager.h"
#include "ShortcutIds.h"

#include <SDL.h>
#include <algorithm>
#include <openrct2-ui/UiStringIds.h>
#include <openrct2/Context.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/core/Console.hpp>
#include <openrct2/core/DataSerialiser.h>
#include <openrct2/core/FileStream.h>
#include <openrct2/core/FileSystem.hpp>
#include <openrct2/core/Json.hpp>
#include <openrct2/core/String.hpp>
#include <openrct2/Input.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/ui/WindowManager.h>

using namespace OpenRCT2::Ui;

// OPENRCT2MINI shift/ctrl-modifier refactor + chord-click fix:
// Cardinality of a binding is "how specific is it" — the bigger, the
// stronger its claim to fire when multiple bindings match the same
// event. Pre-fix this counted only chord (gamepad) modifiers + the
// action button (giving "Z" and "Shift+Z" the same card=1). With the
// HasModifier relaxation, both bindings now match Shift+Z, and we
// need the cardinality to differentiate so the explicit Shift+Z
// binding outranks plain Z. We add the keyboard modifier group count
// (ctrl/shift/alt/gui — one point per group regardless of left/right).
static size_t bindingCardinality(const ShortcutInput& binding) noexcept
{
    size_t card = binding.chordModifiers.size() + 1;
    if (binding.modifiers & (KMOD_LCTRL | KMOD_RCTRL))
        card++;
    if (binding.modifiers & (KMOD_LSHIFT | KMOD_RSHIFT))
        card++;
    if (binding.modifiers & (KMOD_LALT | KMOD_RALT))
        card++;
    if (binding.modifiers & (KMOD_LGUI | KMOD_RGUI))
        card++;
    return card;
}

std::string_view RegisteredShortcut::getTopLevelGroup() const
{
    auto fullstopIndex = id.find('.');
    if (fullstopIndex != std::string::npos)
    {
        return std::string_view(id.c_str(), fullstopIndex);
    }
    return {};
}

std::string_view RegisteredShortcut::getGroup() const
{
    auto fullstopIndex = id.find_last_of('.');
    if (fullstopIndex != std::string::npos)
    {
        return std::string_view(id.c_str(), fullstopIndex);
    }
    return {};
}

bool RegisteredShortcut::matches(const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons) const
{
    if (isSuitableInputEvent(e))
    {
        auto result = std::find_if(
            current.begin(), current.end(),
            [&](const ShortcutInput& _action) { return _action.matches(e, heldGamepadButtons); });
        return result != current.end();
    }
    return false;
}

bool RegisteredShortcut::isSuitableInputEvent(const InputEvent& e) const
{
    // Do not intercept button releases
    if (e.state == InputEventState::release)
    {
        return false;
    }

    // OPENRCT2MINI gamepad-plan mouse-column work: previously this
    // gate rejected mouse buttons 0 (LMB) and 1 (RMB), so even if a
    // user bound `MOUSE 1` / `MOUSE 2` to a shortcut the dispatcher
    // would silently drop the event. With the new Mouse column in
    // the rebind UI, those button events DO need to fire shortcuts —
    // so the mouse-button rejection is removed. The engine's
    // existing widget-click pipeline (StoreMouseInput → GameHandle-
    // InputMouse → InputWidgetLeft) is independent of the shortcut
    // dispatch, so a real LMB still clicks widgets normally; the
    // shortcut dispatch only fires if the user has actually bound
    // a shortcut to MOUSE 1.
    if (e.deviceKind == InputDeviceKind::keyboard)
    {
        // Do not allow modifier keys alone
        switch (e.button)
        {
            case SDLK_LCTRL:
            case SDLK_RCTRL:
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
            case SDLK_LALT:
            case SDLK_RALT:
            case SDLK_LGUI:
            case SDLK_RGUI:
                return false;
        }
    }

    return true;
}

std::string RegisteredShortcut::getDisplayString() const
{
    std::string result;
    auto numChords = current.size();
    for (size_t i = 0; i < numChords; i++)
    {
        const auto& kc = current[i];
        result += kc.toLocalisedString();
        if (i < numChords - 1)
        {
            result += " ";
            result += LanguageGetString(STR_OR);
            result += " ";
        }
    }
    return result;
}

namespace
{
    // OPENRCT2MINI gamepad-plan 1.7a: filter a `current` binding list
    // by predicate and concatenate with " or " separators. Shared
    // between getKeyboardDisplayString and getGamepadDisplayString.
    // Qualified with OpenRCT2:: because the unnamed namespace lifts
    // this template out of the surrounding OpenRCT2::Ui namespace.
    template<typename Pred>
    std::string buildFilteredDisplayString(const std::vector<OpenRCT2::Ui::ShortcutInput>& current, Pred pred)
    {
        std::string result;
        bool first = true;
        for (const auto& kc : current)
        {
            if (!pred(kc))
                continue;
            if (!first)
            {
                result += " ";
                result += OpenRCT2::LanguageGetString(OpenRCT2::STR_OR);
                result += " ";
            }
            result += kc.toLocalisedString();
            first = false;
        }
        return result;
    }
} // namespace

std::string RegisteredShortcut::getKeyboardDisplayString() const
{
    return buildFilteredDisplayString(current, [](const ShortcutInput& kc) {
        return kc.kind == InputDeviceKind::keyboard;
    });
}

std::string RegisteredShortcut::getMouseDisplayString() const
{
    return buildFilteredDisplayString(current, [](const ShortcutInput& kc) {
        return kc.kind == InputDeviceKind::mouse;
    });
}

std::string RegisteredShortcut::getGamepadDisplayString() const
{
    return buildFilteredDisplayString(current, [](const ShortcutInput& kc) {
        return kc.kind == InputDeviceKind::joyButton || kc.kind == InputDeviceKind::joyHat
            || kc.kind == InputDeviceKind::joyAxis;
    });
}

ShortcutManager::ShortcutManager(IPlatformEnvironment& env)
    : _env(env)
{
    registerDefaultShortcuts();
}

void ShortcutManager::registerShortcut(RegisteredShortcut&& shortcut)
{
    if (!shortcut.id.empty() && getShortcut(shortcut.id) == nullptr)
    {
        auto id = std::make_unique<std::string>(shortcut.id);
        auto idView = std::string_view(*id);
        _ids.push_back(std::move(id));
        shortcut.orderIndex = shortcuts.size();
        shortcuts[idView] = shortcut;
    }
}

void ShortcutManager::registerPadDefault(std::string_view id, std::string_view padChord)
{
    auto* sc = getShortcut(id);
    if (sc == nullptr)
        return;
    ShortcutInput parsed{ padChord };
    if (parsed.kind == InputDeviceKind::joyButton || parsed.kind == InputDeviceKind::joyAxis)
    {
        sc->standard.push_back(parsed);
        sc->current.push_back(std::move(parsed));
    }
}

// OPENRCT2MINI gamepad-plan 1.5h: append a device-friendly keyboard
// default to an already-registered shortcut. Same shape as
// registerPadDefault: populates both `standard` (reset-to-default in
// the rebind UI restores it) and `current` (live without requiring the
// user to rebind on first run).
//
// Why a separate helper instead of using the constructor's
// _defaultChord parameter: cut 44 strips constructor-supplied keyboard
// defaults via [[maybe_unused]] to prevent the device's vendor SDL2
// faked-scancode collisions (arrow keys / Q / A / etc clashed with
// the virtual cursor system). 1.5h needs to OPT IN specific keyboard
// scancodes (W / S / V) as defaults for shortcuts that the device's
// vendor SDL2 emits — those scancodes are the L2 / R2 / face Y
// buttons, and we want them to fire view-rotate / zoom / rotate-
// construction without requiring the user to manually rebind on
// first run. Constructor-supplied keyboard defaults remain stripped;
// per-shortcut opt-in via this helper is non-stripped.
void ShortcutManager::registerKeyboardDefault(std::string_view id, std::string_view kbChord)
{
    auto* sc = getShortcut(id);
    if (sc == nullptr)
        return;
    ShortcutInput parsed{ kbChord };
    if (parsed.kind == InputDeviceKind::keyboard)
    {
        sc->standard.push_back(parsed);
        sc->current.push_back(std::move(parsed));
    }
}

// OPENRCT2MINI mouse-input refactor: append a mouse default to an
// already-registered shortcut. Used for the LMB/RMB → click/cancel
// migration (cursor.click, cursor.cancel) and for the scroll-wheel
// → zoom default (kViewGeneralZoomIn/Out). Same shape as the keyboard
// helper: populates both `standard` and `current`.
void ShortcutManager::registerMouseDefault(std::string_view id, std::string_view mouseChord)
{
    auto* sc = getShortcut(id);
    if (sc == nullptr)
        return;
    ShortcutInput parsed{ mouseChord };
    if (parsed.kind == InputDeviceKind::mouse)
    {
        sc->standard.push_back(parsed);
        sc->current.push_back(std::move(parsed));
    }
}

RegisteredShortcut* ShortcutManager::getShortcut(std::string_view id)
{
    auto result = shortcuts.find(id);
    return result == shortcuts.end() ? nullptr : &result->second;
}

void ShortcutManager::removeShortcut(std::string_view id)
{
    shortcuts.erase(id);
    _ids.erase(
        std::remove_if(_ids.begin(), _ids.end(), [id](const std::unique_ptr<std::string>& x) { return *x == id; }), _ids.end());
}

bool ShortcutManager::isPendingShortcutChange() const
{
    return !_pendingShortcutChange.empty();
}

void ShortcutManager::setPendingShortcutChange(
    std::string_view id, PendingShortcutKind kind, PendingShortcutMode mode)
{
    _pendingShortcutChange = id;
    _pendingShortcutKind = kind;
    _pendingShortcutMode = mode;
    // OPENRCT2MINI gamepad-plan 1.7c/1.7d/1.7g: clear the chord-
    // accumulator at the start of every capture session, capture the
    // session start timestamp for the idle-timeout, and clear any
    // stale conflict-rejected message. If a previous session was
    // cancelled mid-press (e.g. the user closed the modal while
    // holding a button), this prevents stale state from contaminating
    // the next session.
    _captureMaxSize = 0;
    _captureMaxActionEvent = {};
    _captureMaxModifiers.clear();
    _captureLastDownMs = 0;
    _captureSessionStartMs = id.empty() ? 0 : SDL_GetTicks();
    _captureRejectedConflictName.clear();
    _captureCommitAsHold = false;
}

void ShortcutManager::setPendingShortcutMode(PendingShortcutMode mode)
{
    _pendingShortcutMode = mode;
}

void ShortcutManager::clearBindingsOfKind(std::string_view id, PendingShortcutKind kind)
{
    // OPENRCT2MINI input-bindings-rework §2.3: remove every binding on
    // shortcut `id` whose device-kind falls into the requested bucket.
    // Gamepad bucket = joyButton + joyHat + joyAxis (mirrors the rebind
    // UI's column → kind mapping).
    auto* shortcut = getShortcut(id);
    if (shortcut == nullptr)
        return;
    auto& bindings = shortcut->current;
    const auto matchesKind = [kind](InputDeviceKind k) -> bool {
        switch (kind)
        {
            case PendingShortcutKind::keyboard:
                return k == InputDeviceKind::keyboard;
            case PendingShortcutKind::mouse:
                return k == InputDeviceKind::mouse;
            case PendingShortcutKind::gamepad:
                return k == InputDeviceKind::joyButton || k == InputDeviceKind::joyHat
                    || k == InputDeviceKind::joyAxis;
            case PendingShortcutKind::any:
                return true;
        }
        return false;
    };
    const auto before = bindings.size();
    bindings.erase(
        std::remove_if(
            bindings.begin(), bindings.end(),
            [&](const ShortcutInput& b) { return matchesKind(b.kind); }),
        bindings.end());
    if (bindings.size() != before)
        saveUserBindings();
}

namespace
{
    // OPENRCT2MINI gamepad-plan 1.7b: classify an event's device kind
    // into the same buckets the capture filter uses.
    //
    // Mouse-column refactor: mouse events used to be lumped in with
    // keyboard (single per-row column showed both). The Input Bindings
    // window now has a dedicated Mouse column, so each helper here is
    // strictly its own kind.
    bool isKeyboardKind(OpenRCT2::Ui::InputDeviceKind k)
    {
        return k == OpenRCT2::Ui::InputDeviceKind::keyboard;
    }

    bool isMouseKind(OpenRCT2::Ui::InputDeviceKind k)
    {
        return k == OpenRCT2::Ui::InputDeviceKind::mouse;
    }

    bool isGamepadKind(OpenRCT2::Ui::InputDeviceKind k)
    {
        return k == OpenRCT2::Ui::InputDeviceKind::joyButton
            || k == OpenRCT2::Ui::InputDeviceKind::joyHat
            || k == OpenRCT2::Ui::InputDeviceKind::joyAxis;
    }
} // namespace

void ShortcutManager::processEvent(
    const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons, const ShortcutFilter& isAllowed,
    const RouteFilter& shouldSuppress)
{
    if (!isPendingShortcutChange())
    {
        // OPENRCT2MINI gamepad-plan 1.7f: largest-match-wins fire-time
        // resolution.
        //
        // Without this, holding a chord modifier (L1) and then pressing
        // an action button (B) would fire BOTH "PAD L1+B" (chord) AND
        // "PAD B" (single) — they each match the event independently
        // because gamepad chord matching is "chord modifiers ⊆ held"
        // (a soft superset check), unlike keyboard chord matching
        // which is exact-modifier (strict equality).
        //
        // Resolution: find the largest cardinality among all matching
        // bindings, then fire only the shortcuts whose best matching
        // binding equals that max. Cardinality = chordModifiers.size()
        // + 1 (the action button itself). For max == 1 (no chords
        // involved), all matches fire — preserves the existing
        // multi-binding-on-same-key keyboard behaviour. For max >= 2
        // (a chord matched), single-button bindings subsumed by the
        // chord are suppressed.
        //
        // Two-pass: first pass finds the max, second pass fires the
        // matches at that max. Within a single shortcut with multiple
        // bindings (e.g. ["PAD B", "PAD L1+B"]), we use whichever of
        // its bindings has the highest cardinality at this event —
        // so the shortcut fires once if any of its bindings matches
        // at max cardinality.
        // OPENRCT2MINI gamepad-plan 1.7f fix: gate every binding match
        // through RegisteredShortcut::isSuitableInputEvent first, so
        // release events are rejected up-front (the original
        // ShortcutManager::processEvent dispatch went through
        // RegisteredShortcut::matches which had this gate; my
        // refactor was calling ShortcutInput::matches directly and
        // skipping it, causing actions to fire on both DOWN and UP
        // — visible as doubled rotate / zoom on every press).
        // OPENRCT2MINI hold-binding refactor: release-event handling.
        // isSuitableInputEvent rejects release events further down, so
        // intercept here BEFORE the largest-match-wins press logic to
        // resolve any pending hold entry. If a release event matches a
        // tracked input and the hold action hasn't fired yet, fire the
        // queued tap actions; remove the entry either way. Modifier
        // state is intentionally not part of the match — the user may
        // press shift after pressing the input but before releasing,
        // and the release should still resolve the original press.
        if (e.state == InputEventState::release)
        {
            for (auto it = _holdPending.begin(); it != _holdPending.end(); ++it)
            {
                if (it->kind == e.deviceKind && it->button == e.button)
                {
                    if (!it->holdFired)
                    {
                        for (const auto& shortcutId : it->tapShortcutIds)
                        {
                            // OPENRCT2MINI gamepad-plan 1.10: re-apply
                            // the context gate at release-time tap
                            // resolution. The press happened in some
                            // earlier frame's context (entry queued
                            // there), but the active context can
                            // change before release (modal opens
                            // mid-hold) — re-check so a deferred tap
                            // can't fire through a freshly-opened
                            // typing modal.
                            if (isAllowed && !isAllowed(e, shortcutId))
                                continue;
                            // OPENRCT2MINI input-plan Track 3 / Phase 3.A:
                            // routing interpose. Phase 3.A's WorldContextStub
                            // returns Passthrough so this never suppresses;
                            // Phase 3.B's real strategies can return Consumed
                            // for shortcuts they semantically own.
                            if (shouldSuppress && shouldSuppress(e, shortcutId))
                                continue;
                            if (auto* sc = getShortcut(shortcutId); sc != nullptr)
                                sc->action();
                        }
                    }
                    _holdPending.erase(it);
                    break;
                }
            }
            return;
        }

        size_t maxCardinality = 0;
        // OPENRCT2MINI cursor-selector-modal-plan §5 / shared-binding
        // pre-pass. When a single physical press matches BOTH a focus.*
        // and a cursor.* shortcut (i.e. the user kept the default
        // shared D-pad bindings), the modal-switch filter inside the
        // dispatch loop needs to pick a winner BEFORE either side
        // executes — otherwise the focus.* fire would auto-wake the
        // selector mid-event and the cursor.* fire (processed next)
        // would be suppressed by the just-changed state. Compute the
        // namespace booleans here so the per-shortcut filter below
        // can defer to them regardless of iteration order.
        bool eventMatchedFocusNamespace = false;
        bool eventMatchedCursorNamespace = false;
        for (const auto& sh : shortcuts)
        {
            if (!sh.second.isSuitableInputEvent(e))
                continue;
            for (const auto& binding : sh.second.current)
            {
                if (binding.matches(e, heldGamepadButtons))
                {
                    const size_t card = bindingCardinality(binding);
                    if (card > maxCardinality)
                        maxCardinality = card;
                    const auto sid = sh.second.id;
                    if (sid == ShortcutId::kFocusUp || sid == ShortcutId::kFocusDown
                        || sid == ShortcutId::kFocusLeft || sid == ShortcutId::kFocusRight)
                    {
                        eventMatchedFocusNamespace = true;
                    }
                    if (sid == ShortcutId::kCursorUp || sid == ShortcutId::kCursorDown
                        || sid == ShortcutId::kCursorLeft || sid == ShortcutId::kCursorRight)
                    {
                        eventMatchedCursorNamespace = true;
                    }
                }
            }
        }
        const bool sharedFocusCursorPress = eventMatchedFocusNamespace && eventMatchedCursorNamespace;
        if (maxCardinality > 0)
        {
            // OPENRCT2MINI hold-binding refactor: scan the matched
            // bindings at max cardinality for a non-zero holdMs. If
            // any match has a hold marker, the press is deferred —
            // collect tap shortcut IDs (holdMs == 0) and hold shortcut
            // IDs (holdMs > 0) into a new _holdPending entry. Else,
            // fire normally (legacy fire-on-press path).
            uint32_t maxHoldMs = 0;
            std::vector<std::string> tapIds;
            std::vector<std::string> holdIds;
            for (const auto& sh : shortcuts)
            {
                if (!sh.second.isSuitableInputEvent(e))
                    continue;
                // OPENRCT2MINI gamepad-plan 1.10: per-context gate.
                // Drop shortcuts disallowed by the active input
                // context (typing modal, console, etc.) before they
                // even enter the hold-pending bucket — the deferred
                // tap fire and immediate hold fire both need to be
                // suppressed, and starting from an empty bucket is
                // simpler than gating every fire site.
                if (isAllowed && !isAllowed(e, sh.second.id))
                    continue;
                for (const auto& binding : sh.second.current)
                {
                    if (!binding.matches(e, heldGamepadButtons))
                        continue;
                    const size_t card = bindingCardinality(binding);
                    if (card != maxCardinality)
                        continue;
                    if (binding.holdMs > 0)
                    {
                        if (binding.holdMs > maxHoldMs)
                            maxHoldMs = binding.holdMs;
                        holdIds.push_back(std::string(sh.second.id));
                    }
                    else
                    {
                        tapIds.push_back(std::string(sh.second.id));
                    }
                    break; // first matching binding per shortcut wins
                }
            }

            if (maxHoldMs > 0)
            {
                // De-duplicate against existing pending entries (SDL
                // key auto-repeat resends KEYDOWN; without this we'd
                // reset pressStartMs and never reach the threshold).
                bool alreadyTracked = false;
                for (const auto& entry : _holdPending)
                {
                    if (entry.kind == e.deviceKind && entry.button == e.button)
                    {
                        alreadyTracked = true;
                        break;
                    }
                }
                if (!alreadyTracked)
                {
                    HoldPendingState entry;
                    entry.kind = e.deviceKind;
                    entry.button = e.button;
                    entry.pressStartMs = SDL_GetTicks();
                    entry.holdMs = maxHoldMs;
                    entry.holdFired = false;
                    entry.tapShortcutIds = std::move(tapIds);
                    entry.holdShortcutIds = std::move(holdIds);
                    _holdPending.push_back(std::move(entry));
                }
            }
            else
            {
                // No hold binding on this press — legacy fire-on-press.
                for (const auto& sh : shortcuts)
                {
                    if (!sh.second.isSuitableInputEvent(e))
                        continue;
                    // OPENRCT2MINI gamepad-plan 1.10: per-context gate
                    // on the legacy fire path. Same allow-list as the
                    // hold-pending bucket above; all device kinds
                    // (keyboard, mouse, joyButton, joyHat, joyAxis)
                    // funnel through here so this is the single
                    // chokepoint for "is this shortcut permitted in
                    // the current modal context".
                    if (isAllowed && !isAllowed(e, sh.second.id))
                        continue;
                    for (const auto& binding : sh.second.current)
                    {
                        if (binding.matches(e, heldGamepadButtons))
                        {
                            const size_t card = bindingCardinality(binding);
                            if (card == maxCardinality)
                            {
                                // OPENRCT2MINI focus-mode follow-up
                                // (Phase F.7 bug-fix): suppress check
                                // moved INSIDE the binding-match
                                // block. Previously the check ran for
                                // every shortcut id ahead of binding
                                // matching, which meant any input
                                // event invoked the strategy's
                                // onShortcut for EVERY id — fine for
                                // pure pass/consume filters, but the
                                // widget-focus strategy has side
                                // effects (move focus, press widget,
                                // close box) in onShortcut and was
                                // firing them all per keypress. User
                                // reported D-pad presses opening
                                // windows and any keypress closing
                                // windows. Gating the suppress
                                // callback behind binding.matches
                                // restores per-shortcut isolation:
                                // onShortcut only fires for the
                                // actual matched shortcut now.
                                if (shouldSuppress && shouldSuppress(e, sh.second.id))
                                    break;
                                // OPENRCT2MINI cursor-selector-modal-plan v2:
                                // modal-switch filter for shared focus.*/
                                // cursor.* bindings, plus virtual-input
                                // tracking.
                                //
                                // Rules (uses pre-pass booleans so the
                                // decision doesn't depend on iteration
                                // order or live state):
                                //   active + shared    → keep focus.*, drop cursor.*
                                //   active + cursor-only → drop cursor.*
                                //   hidden + shared    → keep cursor.*, drop focus.* (no wake)
                                //   hidden + focus-only → keep focus.*, wake to active
                                //
                                // Any non-mouse event also marks
                                // _lastInputWasRealMouse=false so the
                                // bootstrap's new-window auto-wake fires
                                // appropriately.
                                {
                                    auto& im = OpenRCT2::Ui::GetInputManager();
                                    const auto smode = im.getSelectorMode();
                                    const auto sid = sh.second.id;
                                    const bool isFocusId
                                        = sid == ShortcutId::kFocusUp
                                        || sid == ShortcutId::kFocusDown
                                        || sid == ShortcutId::kFocusLeft
                                        || sid == ShortcutId::kFocusRight;
                                    const bool isCursorId
                                        = sid == ShortcutId::kCursorUp
                                        || sid == ShortcutId::kCursorDown
                                        || sid == ShortcutId::kCursorLeft
                                        || sid == ShortcutId::kCursorRight;
                                    if (smode == OpenRCT2::Ui::InputManager::SelectorMode::active
                                        && isCursorId)
                                    {
                                        break; // cursor.* suppressed in active
                                    }
                                    if (smode == OpenRCT2::Ui::InputManager::SelectorMode::hidden
                                        && isFocusId)
                                    {
                                        if (sharedFocusCursorPress)
                                            break; // cursor wins shared press
                                        // Unshared focus.* in hidden →
                                        // user explicitly bound a focus-
                                        // only key. Wake the selector.
                                        im.onTransitionEvent(
                                            OpenRCT2::Ui::InputManager::SelectorTransitionSource::enterFocusModeRequested);
                                        break;
                                    }
                                    // Non-mouse events update the virtual-
                                    // input flag so subsequent new-window
                                    // detection wakes the selector.
                                    if (e.deviceKind != OpenRCT2::Ui::InputDeviceKind::mouse)
                                    {
                                        im.onTransitionEvent(
                                            OpenRCT2::Ui::InputManager::SelectorTransitionSource::virtualUserInput);
                                    }
                                }
                                sh.second.action();
                                break; // fire the shortcut at most once per event
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        auto shortcut = getShortcut(_pendingShortcutChange);
        if (shortcut == nullptr)
            return;

        // OPENRCT2MINI gamepad-plan 1.7b: filter events by the active
        // capture mode. Keyboard column rejects gamepad / mouse events
        // (and vice versa) — the capture session stays open until the
        // user presses an event of the matching kind, presses ESC to
        // cancel, or the timeout fires (1.7d). Mouse-column refactor:
        // mouse events are now their own column (and so their own
        // capture kind), no longer grouped with keyboard.
        if (_pendingShortcutKind == PendingShortcutKind::keyboard && !isKeyboardKind(e.deviceKind))
            return;
        if (_pendingShortcutKind == PendingShortcutKind::mouse && !isMouseKind(e.deviceKind))
            return;
        if (_pendingShortcutKind == PendingShortcutKind::gamepad && !isGamepadKind(e.deviceKind))
            return;

        // OPENRCT2MINI: ignore mouse clicks (and wheel ticks) that land
        // ON the capture modal itself — the Remove button, the Append
        // checkbox, the close-X, and the title-bar are part of the
        // modal's own UI, not user-binding input. Without this filter,
        // a user clicking the Remove button to clear a binding would
        // see that exact click captured as the new binding instead of
        // firing the widget. The filter is positional: any mouse event
        // whose cursor position falls inside the ChangeShortcutWindow's
        // rect is skipped from capture. Other windows still capture
        // mouse clicks normally — only the rebind modal itself is
        // shielded, because it's the only modal where mouse + capture
        // overlap on the same screen real-estate.
        if (isMouseKind(e.deviceKind))
        {
            auto* windowMgr = GetWindowManager();
            if (windowMgr != nullptr)
            {
                auto* changeWnd = windowMgr->FindByClass(WindowClass::changeKeyboardShortcut);
                if (changeWnd != nullptr)
                {
                    const auto* cursorState = ContextGetCursorState();
                    if (cursorState != nullptr)
                    {
                        const auto pos = cursorState->position;
                        if (pos.x >= changeWnd->windowPos.x
                            && pos.x < changeWnd->windowPos.x + changeWnd->width
                            && pos.y >= changeWnd->windowPos.y
                            && pos.y < changeWnd->windowPos.y + changeWnd->height)
                        {
                            return;
                        }
                    }
                }
            }
        }

        const bool isGamepadSession = (_pendingShortcutKind == PendingShortcutKind::gamepad)
            || (_pendingShortcutKind == PendingShortcutKind::any && isGamepadKind(e.deviceKind));

        if (isGamepadSession && heldGamepadButtons != nullptr)
        {
            // OPENRCT2MINI gamepad-plan 1.7c: chord capture state
            // machine. On each DOWN, update the captured max if the
            // held-set just grew, and decide whether to commit:
            //   * 3 buttons → commit immediately (max chord size).
            //   * 2 buttons → arm the 5-second countdown via
            //                 _captureLastDownMs; updatePendingCapture
            //                 commits when it expires.
            //   * 1 button  → wait for release-all (the legacy single-
            //                 button capture path).
            // RELEASE events: commit on release-all if maxSize > 0
            //                 (covers the 1-button case and the user
            //                 letting go before the 2-button timer
            //                 expires).
            if (e.state == InputEventState::down)
            {
                if (!shortcut->isSuitableInputEvent(e))
                    return;
                const auto heldSize = heldGamepadButtons->size();
                if (heldSize > _captureMaxSize && heldSize <= 3)
                {
                    _captureMaxSize = heldSize;
                    _captureMaxActionEvent = e;
                    // Modifiers = held-set minus the action button.
                    // For joyButton, e.button is the canonical
                    // SDL_CONTROLLER_BUTTON_* enum value, which is
                    // the same encoding the held-set uses. For
                    // joyAxis (triggers as action), we need the
                    // axis-as-button encoded value — the held-set
                    // already stores triggers in encoded form.
                    _captureMaxModifiers.clear();
                    // Mirror the axis-as-button encoding used in
                    // InputManager::checkJoysticks (see encodeAxis
                    // AsButton in ShortcutManager.h).
                    const uint32_t actionEncoded = (e.deviceKind == InputDeviceKind::joyAxis)
                        ? encodeAxisAsButton(e.button, e.axisValue >= 0 ? 1 : -1)
                        : e.button;
                    for (auto b : *heldGamepadButtons)
                    {
                        if (b != actionEncoded)
                            _captureMaxModifiers.push_back(b);
                    }
                }
                _captureLastDownMs = SDL_GetTicks();

                // 3-button cap → commit immediately on the third
                // DOWN. We're sure of the user's intent at this
                // point — no further ambiguity.
                if (heldSize >= 3)
                {
                    commitPendingCapture();
                }
                return;
            }
            else
            {
                // RELEASE event. Commit as soon as the held-set
                // shrinks below the captured max — i.e. the user has
                // started to let go after assembling the full chord.
                // This covers the 1-button single-press case (max=1,
                // held=0 after release) AND the 2-button partial-
                // release case (max=2, held=1 after letting go of
                // either chord button) without forcing the user to
                // release both buttons simultaneously or wait out
                // the 5-second auto-commit timer.
                if (_captureMaxSize == 0)
                    return; // nothing captured yet — ignore
                if (heldGamepadButtons->size() >= _captureMaxSize)
                    return; // somehow not a shrink (defensive)
                commitPendingCapture();
                return;
            }
        }

        // Keyboard / mouse / `any`-mode session.
        //
        // OPENRCT2MINI hold-binding capture: defer commit until
        // either release (commit as tap, holdMs=0) or 5-second hold
        // elapsed (commit as hold, holdMs=500 — driven by
        // updatePendingCapture). This mirrors the existing gamepad
        // capture flow and lets the same press cycle yield either
        // binding type.
        //
        // OPENRCT2MINI shift/ctrl-modifier refactor: don't gate on
        // isSuitableInputEvent in this branch. That gate rejects
        // bare keyboard modifier keys (LSHIFT, LCTRL, etc.) —
        // appropriate for fire-time dispatch but wrong for capture
        // where binding a modifier key directly is the whole point.
        if (e.state == InputEventState::release)
        {
            // Release before 5s threshold → commit as tap. Match
            // by (kind, button) against the stored DOWN so a
            // modifier-key release for a non-captured key doesn't
            // commit (e.g. user presses C, presses Shift, releases
            // Shift — Shift release shouldn't end the session).
            if (_captureMaxSize > 0
                && e.deviceKind == _captureMaxActionEvent.deviceKind
                && e.button == _captureMaxActionEvent.button)
            {
                commitPendingCapture();
            }
            return;
        }
        _captureMaxActionEvent = e;
        _captureMaxModifiers.clear();
        _captureMaxSize = 1;
        _captureLastDownMs = SDL_GetTicks();
        // Don't commit yet — wait for release-before-5s or
        // updatePendingCapture's 5s hold-elapsed path.
    }
}

bool ShortcutManager::processEventForSpecificShortcut(
    const InputEvent& e, std::string_view id, const std::set<uint32_t>* heldGamepadButtons)
{
    auto shortcut = getShortcut(id);
    if (shortcut != nullptr && shortcut->matches(e, heldGamepadButtons))
    {
        shortcut->action();
        return true;
    }
    return false;
}

namespace
{
    // OPENRCT2MINI overlap detection: exact-match check for two
    // ShortcutInput entries. Two bindings collide if they would fire
    // at the same press transition AND on the same timing — same
    // (kind, button, modifiers, chord, holdMs). Tap (holdMs == 0)
    // and hold (holdMs > 0) on the same physical input are NOT
    // considered overlapping; they're complementary by design.
    // Used by the rebind-UI's overlap warning to colour competing
    // bindings yellow.
    bool shortcutInputsEqual(const OpenRCT2::Ui::ShortcutInput& a, const OpenRCT2::Ui::ShortcutInput& b)
    {
        if (a.kind != b.kind || a.button != b.button)
            return false;
        if (a.holdMs != b.holdMs)
            return false;
        if (a.kind == OpenRCT2::Ui::InputDeviceKind::keyboard)
        {
            constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;
            return (a.modifiers & kUsefulModifiers) == (b.modifiers & kUsefulModifiers);
        }
        if (a.kind == OpenRCT2::Ui::InputDeviceKind::joyButton
            || a.kind == OpenRCT2::Ui::InputDeviceKind::joyAxis)
        {
            // Compare chord modifiers as a set (order-independent).
            if (a.chordModifiers.size() != b.chordModifiers.size())
                return false;
            std::set<uint32_t> aSet(a.chordModifiers.begin(), a.chordModifiers.end());
            std::set<uint32_t> bSet(b.chordModifiers.begin(), b.chordModifiers.end());
            if (aSet != bSet)
                return false;
            if (a.kind == OpenRCT2::Ui::InputDeviceKind::joyAxis)
            {
                if (a.axisDirection != b.axisDirection)
                    return false;
            }
            return true;
        }
        // mouse, joyHat: button equality is enough.
        return true;
    }
} // namespace

// OPENRCT2MINI gamepad-plan 1.7c: shared commit path for the gamepad
// chord state machine. Builds a ShortcutInput from the saved
// _captureMaxActionEvent + _captureMaxModifiers, applies the kind-
// filtered replace (or append, per 1.7e) from 1.7b, runs the
// conflict scan from 1.7g, saves user bindings, closes the change
// modal, and resets all the capture state. Called by:
//   - the 3-button cap (immediate commit on third DOWN),
//   - the partial-release / release-all path (commit on first held
//     shrink),
//   - updatePendingCapture's 5s-hold-timeout path,
//   - the keyboard / mouse fallback in processEvent.
//
// On conflict (1.7g), this DOES NOT commit, DOES NOT close the
// modal, and stores a localised name on _captureRejectedConflictName
// so the modal can render an inline rejection message. Capture state
// for the chord stays cleared so the user can immediately try a
// different binding.
void ShortcutManager::commitPendingCapture()
{
    auto shortcut = getShortcut(_pendingShortcutChange);
    if (shortcut == nullptr || _captureMaxSize == 0)
    {
        // Nothing to commit. Reset state defensively so the next
        // session starts clean.
        _pendingShortcutChange.clear();
        _pendingShortcutKind = PendingShortcutKind::any;
        _pendingShortcutMode = PendingShortcutMode::replace;
        _captureMaxSize = 0;
        _captureMaxActionEvent = {};
        _captureMaxModifiers.clear();
        _captureLastDownMs = 0;
        _captureSessionStartMs = 0;
        return;
    }

    auto shortcutInput = ShortcutInput::fromInputEvent(_captureMaxActionEvent);
    if (!shortcutInput.has_value())
    {
        // Couldn't synthesise a binding — bail without committing.
        _captureMaxSize = 0;
        _captureMaxActionEvent = {};
        _captureMaxModifiers.clear();
        _captureLastDownMs = 0;
        _captureCommitAsHold = false;
        return;
    }
    shortcutInput->chordModifiers = _captureMaxModifiers;
    // OPENRCT2MINI per-binding Modifier mode: re-apply the chord-shape
    // default now that chordModifiers has been populated. fromInputEvent
    // sees only the keyboard modifier mask and can't know about the
    // gamepad chord modifiers that this commit path attaches.
    if (!shortcutInput->chordModifiers.empty())
        shortcutInput->isModifier = true;
    // OPENRCT2MINI hold-binding capture: the 5-second hold-elapsed
    // path in updatePendingCapture flips _captureCommitAsHold = true
    // before calling here, so the captured binding serialises with a
    // "HOLD " prefix and the dispatcher treats it as fire-after-500ms.
    if (_captureCommitAsHold)
    {
        shortcutInput->holdMs = 500;
    }

    // OPENRCT2MINI overlap-warn refactor: the conflict-rejection
    // scan that used to live here is gone. The user can now bind any
    // input to any shortcut, including duplicates of existing bindings
    // on other shortcuts. Overlapping bindings are surfaced visually
    // by the rebind window — see ShortcutKeysWindow::buildOverlap-
    // AwareDisplayString — which colours competing bindings yellow.

    // OPENRCT2MINI gamepad-plan 1.7e: replace vs append. Replace
    // erases existing same-kind bindings before pushing the new one
    // (the original "rebind" semantic). Append keeps existing
    // bindings and pushes alongside, allowing multiple bindings per
    // shortcut (e.g. PAD A + PAD START both fire confirm).
    const bool append = (_pendingShortcutMode == PendingShortcutMode::append);
    if (!append)
    {
        if (_pendingShortcutKind == PendingShortcutKind::any)
        {
            shortcut->current.clear();
        }
        else
        {
            // Mouse-column refactor: each capture-kind has its own
            // erase predicate now that mouse is its own column.
            auto kindFilter = isGamepadKind;
            switch (_pendingShortcutKind)
            {
                case PendingShortcutKind::keyboard:
                    kindFilter = isKeyboardKind;
                    break;
                case PendingShortcutKind::mouse:
                    kindFilter = isMouseKind;
                    break;
                case PendingShortcutKind::gamepad:
                    kindFilter = isGamepadKind;
                    break;
                case PendingShortcutKind::any:
                    // Unreachable — handled by the outer `if`.
                    break;
            }
            shortcut->current.erase(
                std::remove_if(
                    shortcut->current.begin(), shortcut->current.end(),
                    [&](const ShortcutInput& si) { return kindFilter(si.kind); }),
                shortcut->current.end());
        }
    }
    shortcut->current.push_back(std::move(shortcutInput.value()));

    _pendingShortcutChange.clear();
    _pendingShortcutKind = PendingShortcutKind::any;
    _pendingShortcutMode = PendingShortcutMode::replace;
    _captureMaxSize = 0;
    _captureMaxActionEvent = {};
    _captureMaxModifiers.clear();
    _captureLastDownMs = 0;
    _captureSessionStartMs = 0;
    _captureRejectedConflictName.clear();
    _captureCommitAsHold = false;

    auto* windowMgr = GetWindowManager();
    windowMgr->CloseByClass(WindowClass::changeKeyboardShortcut);
    saveUserBindings();
}

// OPENRCT2MINI gamepad-plan 1.7c/1.7d: per-frame tick. Two timeouts:
//   * IDLE (8s): if a session has received no DOWN events at all
//     since opening, cancel it. Protects users who clicked a column
//     by accident and don't want to commit anything.
//   * HOLD (5s): if 1 or more buttons have been held without further
//     DOWN events for 5 seconds, commit the captured max. Lets the
//     user bind a single trigger by holding it, and acts as a
//     fallback for the 2-button case.
// 3-button chords commit immediately on the third DOWN inside
// processEvent's gamepad branch — they don't need a timer.
// OPENRCT2MINI hold-binding refactor: per-frame hold-threshold check.
// Walks _holdPending and fires hold actions for entries whose
// pressStartMs + holdMs has elapsed (and !holdFired). The entry stays
// in _holdPending after firing — the release event is what removes
// it (so a subsequent release-event handler can see holdFired=true
// and skip the deferred-tap fire). InputManager::process calls this
// once per frame at the same point as updatePendingCapture.
void ShortcutManager::tickHoldShortcuts(uint32_t nowMs)
{
    for (auto& entry : _holdPending)
    {
        if (entry.holdFired)
            continue;
        if (nowMs - entry.pressStartMs < entry.holdMs)
            continue;
        for (const auto& shortcutId : entry.holdShortcutIds)
        {
            if (auto* sc = getShortcut(shortcutId); sc != nullptr)
                sc->action();
        }
        entry.holdFired = true;
    }
}

// OPENRCT2MINI hold-binding refactor: drop a tracked hold entry by
// (kind, button) without firing anything. Used by chord shortcut
// actions that want to "consume" the press the dispatcher would
// otherwise leave deferred. Example: ALT+C close-window — the
// keyboard chord matches at maxCardinality 2 (modifiers + key), so
// the plain "C" tap binding doesn't even enter _holdPending; but
// the device's R1+C path bypasses the chord matcher and so the
// plain C ends up tracked. The action lambda calls this to drop
// the tracking before the C release fires shade-window.
void ShortcutManager::cancelPendingHoldForInput(InputDeviceKind kind, uint32_t button)
{
    for (auto it = _holdPending.begin(); it != _holdPending.end(); ++it)
    {
        if (it->kind == kind && it->button == button)
        {
            _holdPending.erase(it);
            return;
        }
    }
}

bool ShortcutManager::isBindingOverlapping(std::string_view excludeShortcutId, const ShortcutInput& binding) const
{
    for (const auto& sh : shortcuts)
    {
        if (sh.first == excludeShortcutId)
            continue;
        for (const auto& other : sh.second.current)
        {
            if (shortcutInputsEqual(other, binding))
                return true;
        }
    }
    return false;
}

void ShortcutManager::updatePendingCapture(uint32_t nowMs, const std::set<uint32_t>* heldGamepadButtons)
{
    if (!isPendingShortcutChange())
        return;
    // OPENRCT2MINI hold-binding capture: this tick now runs for every
    // session kind (keyboard, mouse, gamepad, any). The 5-second
    // hold-elapsed branch is the path that captures the binding as a
    // HOLD binding — applies regardless of input device.

    // Idle timeout — only fires when no DOWN events have been
    // recorded at all (_captureLastDownMs == 0).
    constexpr uint32_t kIdleTimeoutMs = 8000;
    if (_captureLastDownMs == 0 && _captureSessionStartMs != 0)
    {
        if (nowMs - _captureSessionStartMs >= kIdleTimeoutMs)
        {
            // Cancel the session: clear pending state and close
            // the modal without committing.
            _pendingShortcutChange.clear();
            _pendingShortcutKind = PendingShortcutKind::any;
            _pendingShortcutMode = PendingShortcutMode::replace;
            _captureMaxSize = 0;
            _captureMaxActionEvent = {};
            _captureMaxModifiers.clear();
            _captureSessionStartMs = 0;
            _captureRejectedConflictName.clear();
            _captureCommitAsHold = false;
            auto* windowMgr = GetWindowManager();
            windowMgr->CloseByClass(WindowClass::changeKeyboardShortcut);
        }
        return;
    }

    // Hold timeout — fires while 1+ buttons are still held and the
    // user has stopped adding new ones. Skip if 3-button cap already
    // fired (commitPendingCapture would have cleared _captureMaxSize).
    if (_captureMaxSize == 0 || _captureMaxSize >= 3)
        return;
    if (_captureLastDownMs == 0)
        return;
    // Held-set check is gamepad-only (keyboard/mouse don't track a
    // held-set here — we instead trust that no release event has
    // fired yet, since release would have routed to commit-as-tap
    // in processEvent and cleared _captureMaxSize).
    const bool isGamepadSession = (_pendingShortcutKind == PendingShortcutKind::gamepad)
        || (_pendingShortcutKind == PendingShortcutKind::any && isGamepadKind(_captureMaxActionEvent.deviceKind));
    if (isGamepadSession)
    {
        if (heldGamepadButtons == nullptr || heldGamepadButtons->empty())
            return; // user already released — release path will fire.
    }

    constexpr uint32_t kHoldTimeoutMs = 5000;
    if (nowMs - _captureLastDownMs < kHoldTimeoutMs)
        return;

    // OPENRCT2MINI hold-binding capture: 5-second hold elapsed →
    // capture as a HOLD binding. commitPendingCapture reads this
    // flag and sets shortcutInput->holdMs = 500.
    _captureCommitAsHold = true;
    commitPendingCapture();
}

bool ShortcutManager::isCaptureChordTimerArmed() const
{
    // OPENRCT2MINI gamepad-plan 1.7d / hold-binding capture: timer
    // fires for size 1 or 2 (single-button hold-as-HOLD-binding or
    // 2-button chord). Size 3 commits immediately and clears state,
    // so it's never armed here. Kind-agnostic: the same 5-second
    // window applies to keyboard, mouse, and gamepad sessions —
    // letting the user capture HOLD bindings on any input device.
    return isPendingShortcutChange()
        && _captureMaxSize >= 1 && _captureMaxSize < 3
        && _captureLastDownMs != 0;
}

uint32_t ShortcutManager::getCaptureCountdownMsRemaining(uint32_t nowMs) const
{
    if (!isCaptureChordTimerArmed())
        return 0;
    constexpr uint32_t kHoldTimeoutMs = 5000;
    const uint32_t elapsed = nowMs - _captureLastDownMs;
    if (elapsed >= kHoldTimeoutMs)
        return 0;
    return kHoldTimeoutMs - elapsed;
}

std::string ShortcutManager::getCaptureMaxChordPreview() const
{
    if (_captureMaxSize == 0)
        return {};
    auto si = ShortcutInput::fromInputEvent(_captureMaxActionEvent);
    if (!si.has_value())
        return {};
    si->chordModifiers = _captureMaxModifiers;
    return si->toLocalisedString();
}

void ShortcutManager::loadUserBindings()
{
    try
    {
        auto path = fs::u8path(_env.GetFilePath(PathId::configShortcuts));
        if (fs::exists(path))
        {
            loadUserBindings(path);
        }
        else
        {
            try
            {
                Console::WriteLine("Importing legacy shortcuts...");
                auto legacyPath = fs::u8path(_env.GetFilePath(PathId::configShortcutsLegacy));
                if (fs::exists(legacyPath))
                {
                    loadLegacyBindings(legacyPath);
                    saveUserBindings();
                    Console::WriteLine("Legacy shortcuts imported");
                }
            }
            catch (const std::exception& e)
            {
                Console::Error::WriteLine("Unable to import legacy shortcut bindings: %s", e.what());
            }
        }
    }
    catch (const std::exception& e)
    {
        Console::Error::WriteLine("Unable to load shortcut bindings: %s", e.what());
    }
}

std::optional<ShortcutInput> ShortcutManager::convertLegacyBinding(uint16_t binding)
{
    constexpr uint16_t kNullBinding = 0xFFFF;
    constexpr uint16_t kShift = 0x100;
    constexpr uint16_t kCtrl = 0x200;
    constexpr uint16_t kAlt = 0x400;
    constexpr uint16_t kCmd = 0x800;

    if (binding == kNullBinding)
    {
        return std::nullopt;
    }

    ShortcutInput result;
    result.kind = InputDeviceKind::keyboard;
    if (binding & kShift)
        result.modifiers |= KMOD_SHIFT;
    if (binding & kCtrl)
        result.modifiers |= KMOD_CTRL;
    if (binding & kAlt)
        result.modifiers |= KMOD_ALT;
    if (binding & kCmd)
        result.modifiers |= KMOD_GUI;
    result.button = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(binding & 0xFF));
    return result;
}

void ShortcutManager::loadLegacyBindings(const fs::path& path)
{
    constexpr int32_t kSupportedFileVersion = 1;
    constexpr int32_t kMaxLegacyShortcuts = 85;

    auto fs = FileStream(path, FileMode::open);
    auto version = fs.ReadValue<uint16_t>();
    if (version == kSupportedFileVersion)
    {
        for (size_t i = 0; i < kMaxLegacyShortcuts; i++)
        {
            auto value = fs.ReadValue<uint16_t>();
            auto shortcutId = getLegacyShortcutId(i);
            if (!shortcutId.empty())
            {
                auto shortcut = getShortcut(shortcutId);
                if (shortcut != nullptr)
                {
                    shortcut->current.clear();
                    auto input = convertLegacyBinding(value);
                    if (input.has_value())
                    {
                        shortcut->current.push_back(std::move(input.value()));
                    }
                }
            }
        }
    }
}

// OPENRCT2MINI per-binding Modifier mode: parse one binding-entry from
// JSON. Accepts either a string (legacy form — defaults apply) or an
// object {"binding":"<chord>","is_modifier":<bool>} where the boolean
// overrides the auto-default derived by ShortcutInput's parser.
//
// Bogus entries (missing/empty "binding" key, non-string binding,
// non-bool is_modifier) fall back to defaults wherever possible:
// the chord-default isModifier from ShortcutInput is preserved if
// is_modifier is absent or malformed; an entirely-broken object
// yields no entry (caller skips).
static std::optional<OpenRCT2::Ui::ShortcutInput> parseShortcutEntry(const json_t& value)
{
    using OpenRCT2::Ui::ShortcutInput;
    if (value.is_string())
    {
        return ShortcutInput(value.get<std::string>());
    }
    if (value.is_object())
    {
        auto bindingIt = value.find("binding");
        if (bindingIt == value.end() || !bindingIt->is_string())
            return std::nullopt;
        ShortcutInput parsed(bindingIt->get<std::string>());
        auto modIt = value.find("is_modifier");
        if (modIt != value.end() && modIt->is_boolean())
        {
            parsed.isModifier = modIt->get<bool>();
        }
        return parsed;
    }
    return std::nullopt;
}

void ShortcutManager::loadUserBindings(const fs::path& path)
{
    auto root = Json::ReadFromFile(path.u8string());
    if (root.is_object())
    {
        for (auto it = root.begin(); it != root.end(); ++it)
        {
            const auto& key = it.key();
            const auto& value = it.value();

            const auto& shortcut = getShortcut(key);
            if (shortcut != nullptr)
            {
                shortcut->current.clear();
                if (value.is_array())
                {
                    for (auto& subValue : value)
                    {
                        if (auto parsed = parseShortcutEntry(subValue); parsed.has_value())
                            shortcut->current.push_back(std::move(parsed.value()));
                    }
                }
                else
                {
                    if (auto parsed = parseShortcutEntry(value); parsed.has_value())
                        shortcut->current.push_back(std::move(parsed.value()));
                }
            }
        }
    }
}

void ShortcutManager::saveUserBindings()
{
    try
    {
        auto path = fs::u8path(_env.GetFilePath(PathId::configShortcuts));
        saveUserBindings(path);
    }
    catch (const std::exception& e)
    {
        Console::Error::WriteLine("Unable to save shortcut bindings: %s", e.what());
    }
}

// OPENRCT2MINI per-binding Modifier mode: serialise one binding. If
// isModifier matches the chord-default (chord-shaped → true, single-
// input → false), emit the plain string form; otherwise emit the
// object form with both fields. Minimises diff against user shortcut
// files where the default behaviour applies.
static json_t serialiseShortcutEntry(const OpenRCT2::Ui::ShortcutInput& binding)
{
    using OpenRCT2::Ui::InputDeviceKind;
    const bool isKeyboardChord
        = (binding.kind == InputDeviceKind::keyboard) && (binding.modifiers != 0);
    const bool isPadChord
        = (binding.kind == InputDeviceKind::joyButton || binding.kind == InputDeviceKind::joyAxis)
        && !binding.chordModifiers.empty();
    const bool defaultIsModifier = isKeyboardChord || isPadChord;
    if (binding.isModifier == defaultIsModifier)
        return binding.toString();
    json_t obj = json_t::object();
    obj["binding"] = binding.toString();
    obj["is_modifier"] = binding.isModifier;
    return obj;
}

void ShortcutManager::saveUserBindings(const fs::path& path)
{
    json_t root;
    if (fs::exists(path))
    {
        root = Json::ReadFromFile(path.u8string());
    }

    for (const auto& shortcut : shortcuts)
    {
        auto& jShortcut = root[shortcut.second.id];
        if (shortcut.second.current.size() == 1)
        {
            jShortcut = serialiseShortcutEntry(shortcut.second.current[0]);
        }
        else
        {
            jShortcut = nlohmann::json::array();
            for (const auto& binding : shortcut.second.current)
            {
                jShortcut.push_back(serialiseShortcutEntry(binding));
            }
        }
    }

    Json::WriteToFile(path.u8string(), root);
}

std::string_view ShortcutManager::getLegacyShortcutId(size_t index)
{
    static constexpr std::string_view _legacyMap[] = {
        ShortcutId::kInterfaceCloseTop,
        ShortcutId::kInterfaceCloseAll,
        ShortcutId::kInterfaceCancelConstruction,
        ShortcutId::kInterfacePause,
        ShortcutId::kViewGeneralZoomOut,
        ShortcutId::kViewGeneralZoomIn,
        ShortcutId::kViewGeneralRotateClockwise,
        ShortcutId::kViewGeneralRotateAnticlockwise,
        ShortcutId::kInterfaceRotateConstruction,
        ShortcutId::kViewToggleUnderground,
        ShortcutId::kViewToggleBaseLand,
        ShortcutId::kViewToggleVerticalLand,
        ShortcutId::kViewToggleRides,
        ShortcutId::kViewToggleScenery,
        ShortcutId::kViewToggleSupports,
        ShortcutId::kViewToggleGuests,
        ShortcutId::kViewToggleLandHeightMarkers,
        ShortcutId::kViewToggleTrackHeightMarkers,
        ShortcutId::kViewToggleFootpathHeightMarkers,
        ShortcutId::kInterfaceOpenLand,
        ShortcutId::kInterfaceOpenWater,
        ShortcutId::kInterfaceOpenScenery,
        ShortcutId::kInterfaceOpenFootpaths,
        ShortcutId::kInterfaceOpenNewRide,
        ShortcutId::kInterfaceOpenFinances,
        ShortcutId::kInterfaceOpenResearch,
        ShortcutId::kInterfaceOpenRides,
        ShortcutId::kInterfaceOpenPark,
        ShortcutId::kInterfaceOpenGuests,
        ShortcutId::kInterfaceOpenStaff,
        ShortcutId::kInterfaceOpenMessages,
        ShortcutId::kInterfaceOpenMap,
        ShortcutId::kInterfaceScreenshot,
        ShortcutId::kInterfaceDecreaseSpeed,
        ShortcutId::kInterfaceIncreaseSpeed,
        ShortcutId::kInterfaceOpenCheats,
        ShortcutId::kInterfaceToggleToolbars,
        ShortcutId::kViewScrollUp,
        ShortcutId::kViewScrollLeft,
        ShortcutId::kViewScrollDown,
        ShortcutId::kViewScrollRight,
        ShortcutId::kInterfaceMultiplayerChat,
        ShortcutId::kInterfaceSaveGame,
        ShortcutId::kInterfaceShowOptions,
        ShortcutId::kInterfaceMute,
        ShortcutId::kInterfaceScaleToggleWindowMode,
        ShortcutId::kInterfaceMultiplayerShow,
        std::string_view(),
        ShortcutId::kDebugTogglePaintDebugWindow,
        ShortcutId::kViewToggleFootpaths,
        ShortcutId::kWindowRideConstructionTurnLeft,
        ShortcutId::kWindowRideConstructionTurnRight,
        ShortcutId::kWindowRideConstructionDefault,
        ShortcutId::kWindowRideConstructionSlopeDown,
        ShortcutId::kWindowRideConstructionSlopeUp,
        ShortcutId::kWindowRideConstructionChainLift,
        ShortcutId::kWindowRideConstructionBankLeft,
        ShortcutId::kWindowRideConstructionBankRight,
        ShortcutId::kWindowRideConstructionPrevious,
        ShortcutId::kWindowRideConstructionNext,
        ShortcutId::kWindowRideConstructionBuild,
        ShortcutId::kWindowRideConstructionDemolish,
        ShortcutId::kInterfaceLoadGame,
        ShortcutId::kInterfaceClearScenery,
        ShortcutId::kViewToggleGridlines,
        ShortcutId::kViewToggleCutAway,
        ShortcutId::kViewToggleFootpathIssues,
        ShortcutId::kInterfaceOpenTileInspector,
        ShortcutId::kDebugAdvanceTick,
        ShortcutId::kInterfaceSceneryPicker,
        ShortcutId::kInterfaceScaleIncrease,
        ShortcutId::kInterfaceScaleDecrease,
        ShortcutId::kWindowTileInspectorToggleInvisibility,
        ShortcutId::kWindowTileInspectorCopy,
        ShortcutId::kWindowTileInspectorPaste,
        ShortcutId::kWindowTileInspectorRemove,
        ShortcutId::kWindowTileInspectorMoveUp,
        ShortcutId::kWindowTileInspectorMoveDown,
        ShortcutId::kWindowTileInspectorIncreaseX,
        ShortcutId::kWindowTileInspectorDecreaseX,
        ShortcutId::kWindowTileInspectorIncreaseY,
        ShortcutId::kWindowTileInspectorDecreaseY,
        ShortcutId::kWindowTileInspectorIncreaseHeight,
        ShortcutId::kWindowTileInspectorDecreaseHeight,
        ShortcutId::kInterfaceDisableClearance,
    };
    return index < std::size(_legacyMap) ? _legacyMap[index] : std::string_view();
}
