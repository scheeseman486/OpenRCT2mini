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

// OPENRCT2MINI gamepad-plan 1.2: encoding for axis-as-button entries in
// the held-set and as ShortcutInput::chordModifiers entries. SDL only
// defines ~21 controller buttons (indices 0-20 inclusive in modern SDL2
// headers); kPadAxisAsButtonBase = 64 is comfortably past that, leaves
// room for future SDL additions, and is small enough that std::set
// element values stay in single-byte hash territory. Encoding:
//
//   axis-as-button-index = kPadAxisAsButtonBase + (axis * 2) + (direction == +1 ? 0 : 1)
//
// where `axis` is the SDL_CONTROLLER_AXIS_* index (0-5) and `direction`
// is +1 (positive) or -1 (negative). LEFTTRIGGER (axis 4, +1 dir only)
// and RIGHTTRIGGER (axis 5, +1 dir only) take indices 72 and 74. Stick
// directions use the negative-direction slot too: e.g. STICK_L UP is
// (axis 1 LEFTY, direction -1) → 64 + 2 + 1 = 67.
constexpr uint32_t kPadAxisAsButtonBase = 64;

static uint32_t encodeAxisAsButton(int32_t axis, int8_t direction)
{
    return kPadAxisAsButtonBase + (static_cast<uint32_t>(axis) * 2) + (direction == 1 ? 0u : 1u);
}

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
    else if (String::iequals(rem, "RMB"))
    {
        kind = InputDeviceKind::mouse;
        modifiers = newModifiers;
        button = 1;
    }
    else
    {
        kind = InputDeviceKind::keyboard;
        modifiers = newModifiers;
        button = ParseKey(rem);
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
            if (localised)
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
            case 1:
                result += localised ? FormatStringID(STR_SHORTCUT_MOUSE_RIGHT, button + 1) : "RMB";
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
    if (actual & (left | right))
    {
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
    return result;
}
