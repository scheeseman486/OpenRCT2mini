/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "../UiStringIds.h"
#include "ShortcutManager.h"

#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <cstring>
#include <openrct2/core/String.hpp>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/localisation/StringIds.h>
#include <unordered_map>

using namespace OpenRCT2;
using namespace OpenRCT2::Ui;

// OPENRCT2MINI gamepad-plan 1.2: kPadAxisAsButtonBase + encodeAxisAsButton
// now live in ShortcutManager.h as inline constexpr / inline. See that
// header for the encoding contract.

// OPENRCT2MINI gamepad-plan 1.2: name<->index tables for SDL_GameController
// buttons and axes, used by the PAD-token string parser/serializer.
struct PadButtonEntry
{
    std::string_view name;
    int32_t button; // SDL_CONTROLLER_BUTTON_*
};

static const PadButtonEntry kPadButtonTable[] = {
    { "A",            SDL_CONTROLLER_BUTTON_A },
    { "B",            SDL_CONTROLLER_BUTTON_B },
    { "X",            SDL_CONTROLLER_BUTTON_X },
    { "Y",            SDL_CONTROLLER_BUTTON_Y },
    { "BACK",         SDL_CONTROLLER_BUTTON_BACK },
    { "GUIDE",        SDL_CONTROLLER_BUTTON_GUIDE },
    { "START",        SDL_CONTROLLER_BUTTON_START },
    { "L3",           SDL_CONTROLLER_BUTTON_LEFTSTICK },
    { "R3",           SDL_CONTROLLER_BUTTON_RIGHTSTICK },
    { "L1",           SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
    { "R1",           SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { "DPAD_UP",      SDL_CONTROLLER_BUTTON_DPAD_UP },
    { "DPAD_DOWN",    SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { "DPAD_LEFT",    SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { "DPAD_RIGHT",   SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
};

// Axis-as-button names. Tuple: (token, axis, direction). Triggers fire
// only on the positive-direction crossing; stick directions cover both.
struct PadAxisEntry
{
    std::string_view name;
    int32_t axis;       // SDL_CONTROLLER_AXIS_*
    int8_t direction;   // +1 or -1
};

static const PadAxisEntry kPadAxisTable[] = {
    { "L2",          SDL_CONTROLLER_AXIS_TRIGGERLEFT,  +1 },
    { "R2",          SDL_CONTROLLER_AXIS_TRIGGERRIGHT, +1 },
    { "STICK_L UP",    SDL_CONTROLLER_AXIS_LEFTY,  -1 },
    { "STICK_L DOWN",  SDL_CONTROLLER_AXIS_LEFTY,  +1 },
    { "STICK_L LEFT",  SDL_CONTROLLER_AXIS_LEFTX,  -1 },
    { "STICK_L RIGHT", SDL_CONTROLLER_AXIS_LEFTX,  +1 },
    { "STICK_R UP",    SDL_CONTROLLER_AXIS_RIGHTY, -1 },
    { "STICK_R DOWN",  SDL_CONTROLLER_AXIS_RIGHTY, +1 },
    { "STICK_R LEFT",  SDL_CONTROLLER_AXIS_RIGHTX, -1 },
    { "STICK_R RIGHT", SDL_CONTROLLER_AXIS_RIGHTX, +1 },
};

// 50% of full-scale axis range. Triggers and sticks both share this
// press threshold for shortcut binding purposes; the trigger hysteresis
// (50% press / 30% release) is applied in InputManager when computing
// the joyAxis state-transition events that feed into matches.
constexpr int32_t kPadAxisPressThreshold = 16384;

// OPENRCT2MINI mouse-input refactor: scroll-wheel events ride on the
// existing mouse kind with magic button values past SDL's defined
// range. SDL_BUTTON_LEFT..SDL_BUTTON_X2 are 1..5 (storage 0..4); 8 / 9
// are guaranteed clear and easy to remember as "wheel up / wheel down".
constexpr uint32_t kMouseWheelUpButton = 8;
constexpr uint32_t kMouseWheelDownButton = 9;

// Try to parse a "PAD ..." token (without the leading "PAD " prefix
// already stripped by the caller). Returns true on match and fills the
// out-params with kind/button/threshold/direction. Returns false if the
// token doesn't match any known PAD name — caller should treat as
// invalid binding.
static bool parsePadToken(std::string_view tok, InputDeviceKind& outKind,
                          uint32_t& outButton, int32_t& outThreshold, int8_t& outDirection)
{
    for (const auto& e : kPadButtonTable)
    {
        if (String::iequals(tok, e.name))
        {
            outKind = InputDeviceKind::joyButton;
            outButton = static_cast<uint32_t>(e.button);
            outThreshold = 0;
            outDirection = 0;
            return true;
        }
    }
    for (const auto& e : kPadAxisTable)
    {
        if (String::iequals(tok, e.name))
        {
            outKind = InputDeviceKind::joyAxis;
            outButton = static_cast<uint32_t>(e.axis);
            outThreshold = kPadAxisPressThreshold;
            outDirection = e.direction;
            return true;
        }
    }
    return false;
}

// Serialize a parsed PAD reference back to its canonical token name.
// Used by toString to produce the "PAD ..." suffix. Returns empty if
// no name matches (shouldn't happen for valid bindings).
static std::string_view padButtonName(uint32_t button)
{
    for (const auto& e : kPadButtonTable)
    {
        if (static_cast<uint32_t>(e.button) == button)
            return e.name;
    }
    return {};
}

static std::string_view padAxisName(int32_t axis, int8_t direction)
{
    for (const auto& e : kPadAxisTable)
    {
        if (e.axis == axis && e.direction == direction)
            return e.name;
    }
    return {};
}

// Encode a chord-modifier ShortcutInput parse step: given a parsed
// kind/button/axis-direction, emit the held-set encoded uint32 to
// add to chordModifiers. Buttons go in directly; axis-as-button uses
// the encoding helper.
static uint32_t encodeChordModifier(InputDeviceKind kind, uint32_t button, int32_t axis, int8_t direction)
{
    if (kind == InputDeviceKind::joyAxis)
        return encodeAxisAsButton(axis, direction);
    return button;
}

constexpr uint32_t kUsefulModifiers = KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI;

static uint32_t ParseModifier(std::string_view text)
{
    if (String::iequals(text, "CTRL"))
    {
        return KMOD_CTRL;
    }
    if (String::iequals(text, "LCTRL"))
    {
        return KMOD_LCTRL;
    }
    if (String::iequals(text, "RCTRL"))
    {
        return KMOD_RCTRL;
    }
    if (String::iequals(text, "SHIFT"))
    {
        return KMOD_SHIFT;
    }
    if (String::iequals(text, "LSHIFT"))
    {
        return KMOD_LSHIFT;
    }
    if (String::iequals(text, "RSHIFT"))
    {
        return KMOD_RSHIFT;
    }
    if (String::iequals(text, "ALT"))
    {
        return KMOD_ALT;
    }
    if (String::iequals(text, "LALT"))
    {
        return KMOD_LALT;
    }
    if (String::iequals(text, "RALT"))
    {
        return KMOD_RALT;
    }
    if (String::iequals(text, "GUI"))
    {
        return KMOD_GUI;
    }
    if (String::iequals(text, "LCTRL"))
    {
        return KMOD_LGUI;
    }
    if (String::iequals(text, "RGUI"))
    {
        return KMOD_RGUI;
    }

    return 0;
}

static uint32_t ParseKey(std::string_view text)
{
    char buffer[128]{};
    std::strncpy(buffer, text.data(), sizeof(buffer) - 1);
    auto keyCode = SDL_GetKeyFromName(buffer);
    if (keyCode != SDLK_UNKNOWN)
    {
        return keyCode;
    }
    return 0;
}

static size_t FindPlus(std::string_view s, size_t index)
{
    while (true)
    {
        index = s.find('+', index);
        if (index != std::string::npos && index != 0 && s[index - 1] == ' ')
        {
            index++;
            continue;
        }

        break;
    }
    return index;
}

ShortcutInput::ShortcutInput(std::string_view value)
{
    // OPENRCT2MINI hold-binding refactor: "HOLD " prefix marks the
    // binding as fire-after-500ms-held instead of fire-on-press.
    // Strip the prefix, set holdMs, and parse the remainder as a
    // normal binding string. Composes with PAD-prefix below — the
    // user can write "HOLD PAD Y" or "HOLD C". Default 500 ms; we
    // don't yet support custom hold times.
    if (String::startsWith(value, "HOLD ", true))
    {
        holdMs = 500;
        value = value.substr(5);
    }

    // OPENRCT2MINI gamepad-plan 1.2: PAD-token branch. If the binding
    // starts with "PAD ", treat the entire remainder as a gamepad
    // chord — multiple `+`-joined PAD tokens, where the LAST one is the
    // action (button or axis-as-button) and the prior ones are
    // chord-modifier prerequisites that must be held.
    //
    // Parsed tokens are looked up via parsePadToken; each non-final
    // token contributes to chordModifiers via encodeChordModifier; the
    // final token sets kind/button/axisThreshold/axisDirection.
    if (String::startsWith(value, "PAD ", true))
    {
        // Strip the "PAD " prefix, then split on `+` (using the existing
        // FindPlus helper which already skips "+ " separators).
        auto remainder = value.substr(4);
        std::vector<std::string_view> tokens;
        size_t tokIdx = 0;
        size_t tokSep = FindPlus(remainder, tokIdx);
        while (tokSep != std::string::npos)
        {
            tokens.push_back(remainder.substr(tokIdx, tokSep - tokIdx));
            tokIdx = tokSep + 1;
            tokSep = FindPlus(remainder, tokIdx);
        }
        tokens.push_back(remainder.substr(tokIdx));

        if (tokens.empty())
            return; // invalid; ShortcutInput stays default-constructed

        // Modifiers: every token except the last.
        for (size_t i = 0; i + 1 < tokens.size(); i++)
        {
            InputDeviceKind k{};
            uint32_t b{};
            int32_t t{};
            int8_t d{};
            if (parsePadToken(tokens[i], k, b, t, d))
            {
                chordModifiers.push_back(encodeChordModifier(k, b, static_cast<int32_t>(b), d));
            }
            // Unrecognised modifier token: silently skip (defensive — a
            // future SDL version might add controller buttons we don't
            // know yet, and we'd rather drop the chord-modifier than
            // refuse to load the entire binding).
        }

        // Action: the last token.
        InputDeviceKind k{};
        uint32_t b{};
        int32_t t{};
        int8_t d{};
        if (parsePadToken(tokens.back(), k, b, t, d))
        {
            kind = k;
            button = b;
            axisThreshold = t;
            axisDirection = d;
        }
        return;
    }

    uint32_t newModifiers = 0;
    size_t index = 0;
    auto sepIndex = FindPlus(value, index);
    while (sepIndex != std::string::npos)
    {
        auto text = value.substr(index, sepIndex - index);
        auto mod = ParseModifier(text);
        newModifiers |= mod;
        index = sepIndex + 1;
        sepIndex = FindPlus(value, index);
    }
    auto rem = value.substr(index);

    if (String::startsWith(rem, "JOY ", true))
    {
        rem = rem.substr(4);
        if (String::equals(rem, "LEFT"))
        {
            kind = InputDeviceKind::joyHat;
            modifiers = newModifiers;
            button = SDL_HAT_LEFT;
        }
        else if (String::equals(rem, "RIGHT"))
        {
            kind = InputDeviceKind::joyHat;
            modifiers = newModifiers;
            button = SDL_HAT_RIGHT;
        }
        else if (String::equals(rem, "UP"))
        {
            kind = InputDeviceKind::joyHat;
            modifiers = newModifiers;
            button = SDL_HAT_UP;
        }
        else if (String::equals(rem, "DOWN"))
        {
            kind = InputDeviceKind::joyHat;
            modifiers = newModifiers;
            button = SDL_HAT_DOWN;
        }
        else
        {
            auto number = String::tryParse<int32_t>(rem);
            if (number.has_value())
            {
                kind = InputDeviceKind::joyButton;
                modifiers = newModifiers;
                button = number.value() - 1;
            }
        }
    }
    // OPENRCT2MINI mouse-input refactor: scroll wheel as a bindable
    // input. Tokens "MOUSE WHEEL UP" / "MOUSE WHEEL DOWN" map onto the
    // mouse kind with magic button values 8 (up) and 9 (down) — comfortably
    // past the SDL_BUTTON_X1/X2 range (4/5), so they don't collide with
    // any real mouse button. getState's `_mouseState & (1 << button)` for
    // button=8 / 9 returns false (those bits are never set in
    // SDL_GetMouseState's bitmask), which is exactly the behaviour we
    // want: wheel events fire once on press, with no held-state.
    else if (String::iequals(rem, "MOUSE WHEEL UP"))
    {
        kind = InputDeviceKind::mouse;
        modifiers = newModifiers;
        button = kMouseWheelUpButton;
    }
    else if (String::iequals(rem, "MOUSE WHEEL DOWN"))
    {
        kind = InputDeviceKind::mouse;
        modifiers = newModifiers;
        button = kMouseWheelDownButton;
    }
    else if (String::startsWith(rem, "MOUSE ", true))
    {
        rem = rem.substr(6);
        auto number = String::tryParse<int32_t>(rem);
        if (number)
        {
            kind = InputDeviceKind::mouse;
            modifiers = newModifiers;
            button = *number - 1;
        }
    }
    else if (String::iequals(rem, "LMB"))
    {
        kind = InputDeviceKind::mouse;
        modifiers = newModifiers;
        button = 0;
    }
    // OPENRCT2MINI mouse-input refactor: RMB numbering bug-fix. SDL's
    // SDL_BUTTON_RIGHT is 3 (1-based); after the SDL handler converts
    // events to 0-based we expect button=2 for RMB. The legacy code
    // stored RMB as button=1, which would have collided with the
    // middle-button slot — but mouse bindings weren't actually firing
    // before the loosened isSuitableInputEvent filter, so the
    // mismatch was inert.
    else if (String::iequals(rem, "RMB"))
    {
        kind = InputDeviceKind::mouse;
        modifiers = newModifiers;
        button = 2;
    }
    else
    {
        kind = InputDeviceKind::keyboard;
        modifiers = newModifiers;
        button = ParseKey(rem);

        // OPENRCT2MINI shift/ctrl-modifier refactor: standalone
        // modifier-key tokens ("LSHIFT", "RSHIFT", "LCTRL", "RCTRL",
        // "LALT", "RALT") with no following key are now bindable as
        // KEYBOARD-KEY shortcuts on their own, not chord prefixes.
        // SDL_GetKeyFromName doesn't recognise the abbreviated form,
        // so ParseKey would otherwise return 0 and the binding would
        // silently no-op. The chord syntax ("SHIFT+P") still routes
        // through the modifier loop above and is unaffected — that
        // branch hits a value of `rem` that's the trailing key
        // ("P"), so this fallback only fires when the modifier is
        // the entire token.
        if (button == 0 && newModifiers == 0)
        {
            if (String::iequals(rem, "LSHIFT"))
                button = SDLK_LSHIFT;
            else if (String::iequals(rem, "RSHIFT"))
                button = SDLK_RSHIFT;
            else if (String::iequals(rem, "LCTRL"))
                button = SDLK_LCTRL;
            else if (String::iequals(rem, "RCTRL"))
                button = SDLK_RCTRL;
            else if (String::iequals(rem, "LALT"))
                button = SDLK_LALT;
            else if (String::iequals(rem, "RALT"))
                button = SDLK_RALT;
        }
    }
}

std::string_view ShortcutInput::getModifierName(uint32_t key, bool localised)
{
    static std::unordered_map<uint32_t, std::pair<const char*, StringId>> _keys{
        { KMOD_SHIFT, { "SHIFT", STR_SHORTCUT_MOD_SHIFT } },    { KMOD_LSHIFT, { "LSHIFT", STR_SHORTCUT_MOD_LSHIFT } },
        { KMOD_RSHIFT, { "RSHIFT", STR_SHORTCUT_MOD_RSHIFT } }, { KMOD_CTRL, { "CTRL", STR_SHORTCUT_MOD_CTRL } },
        { KMOD_LCTRL, { "LCTRL", STR_SHORTCUT_MOD_LCTRL } },    { KMOD_RCTRL, { "RCTRL", STR_SHORTCUT_MOD_RCTRL } },
        { KMOD_ALT, { "ALT", STR_SHORTCUT_MOD_ALT } },          { KMOD_LALT, { "LALT", STR_SHORTCUT_MOD_LALT } },
        { KMOD_RALT, { "RALT", STR_SHORTCUT_MOD_RALT } },       { KMOD_GUI, { "GUI", STR_SHORTCUT_MOD_GUI } },
        { KMOD_LGUI, { "LGUI", STR_SHORTCUT_MOD_LGUI } },       { KMOD_RGUI, { "RGUI", STR_SHORTCUT_MOD_RGUI } },
    };

    auto r = _keys.find(key);
    if (r != _keys.end())
    {
        if (localised && r->second.second != kStringIdNone)
        {
            return LanguageGetString(r->second.second);
        }

        return r->second.first;
    }

    return {};
}

std::string_view ShortcutInput::getLocalisedKeyName(uint32_t key)
{
    static std::unordered_map<uint32_t, StringId> _keys{
        { SDLK_LEFT, STR_SHORTCUT_LEFT },
        { SDLK_RIGHT, STR_SHORTCUT_RIGHT },
        { SDLK_UP, STR_SHORTCUT_UP },
        { SDLK_DOWN, STR_SHORTCUT_DOWN },
        { SDLK_BACKSPACE, STR_SHORTCUT_BACKSPACE },
        { SDLK_ESCAPE, STR_SHORTCUT_ESCAPE },
        { SDLK_SPACE, STR_SHORTCUT_SPACEBAR },
        { SDLK_TAB, STR_SHORTCUT_TAB },
        { SDLK_RETURN, STR_SHORTCUT_RETURN },
        { SDLK_PAGEUP, STR_SHORTCUT_PGUP },
        { SDLK_PAGEDOWN, STR_SHORTCUT_PGDN },
        { SDLK_INSERT, STR_SHORTCUT_INSERT },
        { SDLK_DELETE, STR_SHORTCUT_DELETE },
        { SDLK_KP_DIVIDE, STR_SHORTCUT_NUMPAD_DIVIDE },
        { SDLK_KP_MULTIPLY, STR_SHORTCUT_NUMPAD_MULTIPLY },
        { SDLK_KP_MINUS, STR_SHORTCUT_NUMPAD_MINUS },
        { SDLK_KP_PLUS, STR_SHORTCUT_NUMPAD_PLUS },
        { SDLK_KP_ENTER, STR_SHORTCUT_NUMPAD_RETURN },
        { SDLK_KP_1, STR_SHORTCUT_NUMPAD_1 },
        { SDLK_KP_2, STR_SHORTCUT_NUMPAD_2 },
        { SDLK_KP_3, STR_SHORTCUT_NUMPAD_3 },
        { SDLK_KP_4, STR_SHORTCUT_NUMPAD_4 },
        { SDLK_KP_5, STR_SHORTCUT_NUMPAD_5 },
        { SDLK_KP_6, STR_SHORTCUT_NUMPAD_6 },
        { SDLK_KP_7, STR_SHORTCUT_NUMPAD_7 },
        { SDLK_KP_8, STR_SHORTCUT_NUMPAD_8 },
        { SDLK_KP_9, STR_SHORTCUT_NUMPAD_9 },
        { SDLK_KP_0, STR_SHORTCUT_NUMPAD_0 },
        { SDLK_KP_PERIOD, STR_SHORTCUT_NUMPAD_PERIOD },
        { SDLK_CAPSLOCK, STR_SHORTCUT_CAPSLOCK },
        { SDLK_CLEAR, STR_SHORTCUT_CLEAR },
        { SDLK_PAUSE, STR_SHORTCUT_PAUSE },
        { SDLK_END, STR_SHORTCUT_END },
        { SDLK_HOME, STR_SHORTCUT_HOME },
        { SDLK_SELECT, STR_SHORTCUT_SELECT },
        { SDLK_PRINTSCREEN, STR_SHORTCUT_PRINT },
        { SDLK_EXECUTE, STR_SHORTCUT_EXECUTE },
        { SDLK_SYSREQ, STR_SHORTCUT_SNAPSHOT },
        { SDLK_HELP, STR_SHORTCUT_HELP },
        { SDLK_MENU, STR_SHORTCUT_MENU },
        { SDLK_NUMLOCKCLEAR, STR_SHORTCUT_NUMLOCK },
        { SDLK_SCROLLLOCK, STR_SHORTCUT_SCROLL },
    };

    auto r = _keys.find(key);
    if (r != _keys.end())
    {
        return LanguageGetString(r->second);
    }

    return {};
}

std::string ShortcutInput::toString() const
{
    return toString(false);
}

std::string ShortcutInput::toLocalisedString() const
{
    return toString(true);
}

std::string ShortcutInput::toString(bool localised) const
{
    std::string result;

    // OPENRCT2MINI hold-binding refactor: prepend "HOLD " for hold
    // bindings so shortcuts.json round-trips and the rebind UI
    // displays the binding as "HOLD C" / "HOLD PAD Y" etc. Goes
    // before everything else — composes with PAD / chord prefixes.
    if (holdMs > 0)
    {
        result += "HOLD ";
    }

    // OPENRCT2MINI gamepad-plan 1.2: PAD bindings serialise to a single
    // "PAD <chord>" string with chord modifiers `+`-joined before the
    // action token. Sample outputs:
    //   "PAD B"
    //   "PAD L1+B"
    //   "PAD L2"
    //   "PAD L1+R1"  (modifier-only chord)
    //   "PAD STICK_L UP"
    if (kind == InputDeviceKind::joyButton || kind == InputDeviceKind::joyAxis)
    {
        // Heuristic: classify as PAD-encoded if the action button name
        // is found in our PAD tables. Legacy "JOY n" bindings (numeric
        // button indices) fall through to the existing joyButton path
        // below for backward compatibility.
        std::string_view actionName;
        if (kind == InputDeviceKind::joyButton)
            actionName = padButtonName(button);
        else
            actionName = padAxisName(static_cast<int32_t>(button), axisDirection);

        if (!actionName.empty())
        {
            result += "PAD ";

            // Decode each chord-modifier back to its name.
            for (uint32_t modEncoded : chordModifiers)
            {
                std::string_view modName;
                if (modEncoded < kPadAxisAsButtonBase)
                {
                    modName = padButtonName(modEncoded);
                }
                else
                {
                    uint32_t rel = modEncoded - kPadAxisAsButtonBase;
                    int32_t axisIdx = static_cast<int32_t>(rel / 2);
                    int8_t dir = (rel & 1u) == 0u ? +1 : -1;
                    modName = padAxisName(axisIdx, dir);
                }
                if (!modName.empty())
                {
                    result += modName;
                    result += "+";
                }
            }
            result += actionName;
            return result;
        }
        // Fall through to legacy JOY / hat formatting below if the
        // action wasn't a recognised PAD entry.
    }

    appendModifier(result, KMOD_LSHIFT, KMOD_RSHIFT, localised);
    appendModifier(result, KMOD_LCTRL, KMOD_RCTRL, localised);
    appendModifier(result, KMOD_LALT, KMOD_RALT, localised);
    appendModifier(result, KMOD_LGUI, KMOD_RGUI, localised);

    if (kind == InputDeviceKind::keyboard)
    {
        if (button != 0)
        {
            // OPENRCT2MINI shift/ctrl-modifier refactor: round-trip
            // standalone modifier-key bindings to their canonical
            // tokens ("LSHIFT" etc.) instead of SDL's localised
            // names ("Left Shift") — the parser only recognises
            // the abbreviated form, and writing them in the long
            // form would not parse back. Keeps shortcuts.json
            // serialise-then-load idempotent.
            const char* modName = nullptr;
            switch (button)
            {
                case SDLK_LSHIFT: modName = "LSHIFT"; break;
                case SDLK_RSHIFT: modName = "RSHIFT"; break;
                case SDLK_LCTRL:  modName = "LCTRL";  break;
                case SDLK_RCTRL:  modName = "RCTRL";  break;
                case SDLK_LALT:   modName = "LALT";   break;
                case SDLK_RALT:   modName = "RALT";   break;
            }
            if (modName != nullptr && modifiers == 0 && !localised)
            {
                result += modName;
            }
            else if (localised)
            {
                auto name = getLocalisedKeyName(button);
                if (!name.empty())
                {
                    result += name;
                }
                else
                {
                    result += SDL_GetKeyName(button);
                }
            }
            else
            {
                result += SDL_GetKeyName(button);
            }
        }
    }
    else if (kind == InputDeviceKind::mouse)
    {
        switch (button)
        {
            case 0:
                result += localised ? FormatStringID(STR_SHORTCUT_MOUSE_LEFT, button + 1) : "LMB";
                break;
            // OPENRCT2MINI mouse-input refactor: RMB now maps to
            // button = 2 (== SDL_BUTTON_RIGHT - 1). Was 1 in legacy
            // code; bug was inert before the binding filter loosened.
            case 2:
                result += localised ? FormatStringID(STR_SHORTCUT_MOUSE_RIGHT, button + 1) : "RMB";
                break;
            // OPENRCT2MINI mouse-input refactor: scroll-wheel tokens.
            case kMouseWheelUpButton:
                result += "MOUSE WHEEL UP";
                break;
            case kMouseWheelDownButton:
                result += "MOUSE WHEEL DOWN";
                break;
            default:
                result += localised ? FormatStringID(STR_SHORTCUT_MOUSE_NUMBER, button + 1)
                                    : "MOUSE " + std::to_string(button + 1);
                break;
        }
    }
    else if (kind == InputDeviceKind::joyButton)
    {
        result += localised ? FormatStringID(STR_SHORTCUT_JOY_NUMBER, button + 1) : "JOY " + std::to_string(button + 1);
    }
    else if (kind == InputDeviceKind::joyHat)
    {
        if (button & SDL_HAT_LEFT)
            result += localised ? LanguageGetString(STR_SHORTCUT_JOY_LEFT) : "JOY LEFT";
        else if (button & SDL_HAT_RIGHT)
            result += localised ? LanguageGetString(STR_SHORTCUT_JOY_RIGHT) : "JOY RIGHT";
        else if (button & SDL_HAT_UP)
            result += localised ? LanguageGetString(STR_SHORTCUT_JOY_UP) : "JOY UP";
        else if (button & SDL_HAT_DOWN)
            result += localised ? LanguageGetString(STR_SHORTCUT_JOY_DOWN) : "JOY DOWN";
        else
            result += "JOY ?";
    }
    return result;
}

bool ShortcutInput::appendModifier(std::string& s, uint32_t left, uint32_t right, bool localised) const
{
    if ((modifiers & (left | right)) == (left | right))
    {
        s += getModifierName(left | right, localised);
        s += "+";
        return true;
    }
    if (modifiers & left)
    {
        s += getModifierName(left, localised);
        s += "+";
        return true;
    }
    if (modifiers & right)
    {
        s += getModifierName(right, localised);
        s += "+";
        return true;
    }
    return false;
}

// OPENRCT2MINI shift/ctrl-modifier refactor + chord-click fix:
//
// When the shortcut REQUIRES this modifier (left|right), accept only if
// the corresponding side is actually held — that part is unchanged.
//
// When the shortcut does NOT require this modifier, the legacy upstream
// behaviour was to REJECT if the modifier was held anyway (so plain `Z`
// would not fire while Shift was down — the user "must" have meant a
// Shift+Z chord). That worked in the old hardcoded-modifier world, but
// it breaks our bindable modifier-shortcut model: kInterfaceShift-
// Modifier and kInterfaceConstructionZLock have keyboard defaults
// LSHIFT/RSHIFT and LCTRL/RCTRL, so holding the bound input sets
// KMOD_SHIFT / KMOD_CTRL in SDL_GetModState — which would then block
// the bare cursor.click binding (modifiers=0) from matching, making
// modifier+click silently no-op. The user sees vertical placement on
// hover, but clicking does nothing because the click event itself is
// filtered out at matches() time.
//
// New behaviour: when the shortcut doesn't require this modifier, ACCEPT
// even if the actual state has it. Differentiation between "plain Z"
// and "Shift+Z" as separate shortcuts is now done by the largest-match-
// wins cardinality scan in ShortcutManager::processEvent, which adds
// the keyboard-modifier count into the cardinality so an explicit
// Shift+Z binding (card 2) outranks plain Z (card 1) when both match.
static bool HasModifier(uint32_t shortcut, uint32_t actual, uint32_t left, uint32_t right)
{
    if (shortcut & (left | right))
    {
        if ((shortcut & left) && (actual & left))
        {
            return true;
        }
        if ((shortcut & right) && (actual & right))
        {
            return true;
        }
        return false;
    }
    return true;
}

static bool CompareModifiers(uint32_t shortcut, uint32_t actual)
{
    shortcut &= kUsefulModifiers;
    return HasModifier(shortcut, actual, KMOD_LCTRL, KMOD_RCTRL) && HasModifier(shortcut, actual, KMOD_LSHIFT, KMOD_RSHIFT)
        && HasModifier(shortcut, actual, KMOD_LALT, KMOD_RALT) && HasModifier(shortcut, actual, KMOD_LGUI, KMOD_RGUI);
}

bool ShortcutInput::matches(const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons) const
{
    // Existing keyboard / mouse / joyButton-without-chord / joyHat path:
    // exact button match plus exact keyboard modifier match.
    if (kind == InputDeviceKind::keyboard || kind == InputDeviceKind::mouse
        || kind == InputDeviceKind::joyHat
        || (kind == InputDeviceKind::joyButton && chordModifiers.empty()))
    {
        if (CompareModifiers(modifiers, e.modifiers))
        {
            if (e.deviceKind == kind && button == e.button)
            {
                return true;
            }
        }
        return false;
    }

    // OPENRCT2MINI gamepad-plan 1.2: joyButton with chord modifiers OR
    // joyAxis (with optional chord modifiers). The action-button match
    // is the same — incoming event's deviceKind/button must agree —
    // but we additionally require every chord-modifier in our binding
    // to be present in the held-set.
    if (kind == InputDeviceKind::joyButton && !chordModifiers.empty())
    {
        if (e.deviceKind != InputDeviceKind::joyButton || e.button != button)
            return false;

        // Chord-modifier prerequisite: every entry in chordModifiers
        // must be in the held-set (or the event itself, which is the
        // newly-pressed button — but a button can't be its own chord
        // modifier, so we just consult heldGamepadButtons).
        if (heldGamepadButtons == nullptr)
            return false;
        for (uint32_t mod : chordModifiers)
        {
            if (heldGamepadButtons->find(mod) == heldGamepadButtons->end())
                return false;
        }
        return true;
    }

    if (kind == InputDeviceKind::joyAxis)
    {
        if (e.deviceKind != InputDeviceKind::joyAxis || e.button != button)
            return false;

        // axisDirection is +1 or -1; we match if the event's axisValue
        // is past the threshold in that direction.
        if (axisDirection > 0)
        {
            if (e.axisValue < axisThreshold)
                return false;
        }
        else if (axisDirection < 0)
        {
            if (e.axisValue > -axisThreshold)
                return false;
        }
        else
        {
            return false; // shouldn't happen — axis bindings always have a direction
        }

        // Chord prerequisites (if any).
        if (!chordModifiers.empty())
        {
            if (heldGamepadButtons == nullptr)
                return false;
            for (uint32_t mod : chordModifiers)
            {
                if (heldGamepadButtons->find(mod) == heldGamepadButtons->end())
                    return false;
            }
        }
        return true;
    }

    return false;
}

std::optional<ShortcutInput> ShortcutInput::fromInputEvent(const InputEvent& e)
{
    // Assume any side modifier (more specific configurations can be done by manually editing config file)
    auto modifiers = e.modifiers & kUsefulModifiers;
    for (auto mod : { KMOD_CTRL, KMOD_SHIFT, KMOD_ALT, KMOD_GUI })
    {
        if (modifiers & mod)
        {
            modifiers |= mod;
        }
    }

    ShortcutInput result;
    result.kind = e.deviceKind;
    result.modifiers = modifiers;
    result.button = e.button;

    // OPENRCT2MINI shift/ctrl-modifier refactor: when the captured
    // key IS a bare modifier (LSHIFT/RSHIFT/LCTRL/RCTRL/LALT/RALT),
    // strip the modifier flags from the binding. Otherwise the
    // SDL_KEYDOWN for Shift would store {kind=keyboard,
    // modifiers=KMOD_SHIFT, button=SDLK_LSHIFT} — a self-modified
    // binding that toString would render as "SHIFT+LSHIFT" and
    // InputManager::getState's bare-modifier fast path wouldn't
    // recognise (because modifiers != 0). Clearing modifiers
    // produces a clean bare-modifier binding instead.
    if (e.deviceKind == InputDeviceKind::keyboard)
    {
        const auto b = e.button;
        const bool isBareModifierKey
            = (b == SDLK_LSHIFT || b == SDLK_RSHIFT || b == SDLK_LCTRL || b == SDLK_RCTRL
               || b == SDLK_LALT || b == SDLK_RALT);
        if (isBareModifierKey)
            result.modifiers = 0;
    }
    // OPENRCT2MINI gamepad-plan 1.7c: joyAxis (trigger / stick-direction)
    // bindings need axisDirection and axisThreshold for matches() to fire
    // and for toString() to serialise as "PAD L2" / "PAD STICK_L UP" /
    // etc. InputManager synthesises e.axisValue as ±kPadAxisPressThreshold
    // on threshold-cross — so we can derive both fields directly: sign of
    // axisValue gives direction, magnitude is the press threshold.
    // Without this, captured trigger bindings end up with axisDirection=0
    // and matches() returns false unconditionally; the binding also
    // serialises as an empty string because padAxisName(idx, 0) misses
    // the table.
    if (e.deviceKind == InputDeviceKind::joyAxis)
    {
        result.axisDirection = (e.axisValue >= 0) ? +1 : -1;
        result.axisThreshold = kPadAxisPressThreshold;
    }
    return result;
}
