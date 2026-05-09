/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2/world/Location.hpp>
#include <queue>
#include <set>
#include <string_view>

typedef struct _SDL_GameController SDL_GameController;
typedef union SDL_Event SDL_Event;

namespace OpenRCT2::Ui
{
    class RegisteredShortcut;
    struct ShortcutInput;

    enum class InputDeviceKind
    {
        mouse,
        keyboard,
        joyButton,
        joyHat,
        joyAxis,
    };

    enum class InputEventState
    {
        down,
        release,
    };

    struct InputEvent
    {
        InputDeviceKind deviceKind;
        uint32_t modifiers;
        uint32_t button;
        InputEventState state;
        int16_t axisValue{}; // For analogue stick values (-32768 to 32767)
    };

    enum class ModifierKey : uint8_t
    {
        none = 0,
        shift = 1 << 0,
        ctrl = 1 << 1,
        alt = 1 << 2,
        cmd = 1 << 3,
    };

    class InputManager
    {
    private:
        uint32_t _lastJoystickCheck{};
        std::vector<SDL_GameController*> _gameControllers;
        std::queue<InputEvent> _events;
        ScreenCoordsXY _viewScroll;
        ScreenCoordsXY _analogueScroll;     // analogue stick scroll values
        float _analogueScrollAccumX = 0.0f; // Fractional accumulator for X axis
        float _analogueScrollAccumY = 0.0f; // Fractional accumulator for Y axis
        uint32_t _mouseState{};
        std::vector<uint8_t> _keyboardState;
        uint8_t _modifierKeyState;

        // OPENRCT2MINI gamepad-plan 1.2: held gamepad-button set, used as
        // the chord-modifier source when matching ShortcutInput against an
        // incoming joyButton / joyAxis event. Updated in queueInputEvent
        // before the InputEvent is enqueued, so by the time the event
        // reaches process() the held-set already reflects this transition.
        // Triggers (LEFTTRIGGER / RIGHTTRIGGER axes) participate in the
        // held-set after their press-threshold crossing — see queueInputEvent.
        // Stick-direction-as-button entries (PAD STICK_L UP etc.) likewise
        // enter and leave the set on threshold crossings. Indices follow
        // SDL_CONTROLLER_BUTTON_* for buttons; for axis-as-button entries
        // we encode them in the high range (kAxisAsButtonBase + axis*4 +
        // direction) to avoid collision with real button indices. See
        // ShortcutInput.cpp for the encoding helpers.
        std::set<uint32_t> _heldGamepadButtons;

        void checkJoysticks();
        void processAnalogueInput();
        void updateAnalogueScroll();

        void handleViewScrolling();
        void handleModifiers();
        void processEvents();
        void process(const InputEvent& e);
        void processInGameConsole(const InputEvent& e);
        void processChat(const InputEvent& e);
        void processHoldEvents();
        void processViewScrollEvent(std::string_view shortcutId, const ScreenCoordsXY& delta);

        bool getState(const RegisteredShortcut& shortcut) const;
        bool getState(const ShortcutInput& shortcut) const;

        bool hasTextInputFocus() const;

    public:
        InputManager();

        bool isModifierKeyPressed(ModifierKey modifier) const;
        void queueInputEvent(const SDL_Event& e);
        void queueInputEvent(InputEvent&& e);
        void process();

        // OPENRCT2MINI: cut 38. Expose connected controllers so the virtual
        // gamepad cursor (in UiContext, behind ENABLE_SOFTWARE_CURSOR) can
        // poll D-pad and face-button state without opening a second copy.
        const std::vector<SDL_GameController*>& getGameControllers() const
        {
            return _gameControllers;
        }

        // OPENRCT2MINI gamepad-plan 1.2: read access for shortcut chord
        // matching. ShortcutInput::matches consults this when checking
        // whether a binding's chord-modifier prerequisites are satisfied.
        const std::set<uint32_t>& getHeldGamepadButtons() const
        {
            return _heldGamepadButtons;
        }
    };
} // namespace OpenRCT2::Ui
