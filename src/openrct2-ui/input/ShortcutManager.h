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

    private:
    };

    class ShortcutManager
    {
    private:
        IPlatformEnvironment& _env;
        std::string _pendingShortcutChange;

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
        RegisteredShortcut* getShortcut(std::string_view id);
        void removeShortcut(std::string_view id);
        bool isPendingShortcutChange() const;
        void setPendingShortcutChange(std::string_view id);
        // OPENRCT2MINI gamepad-plan 1.2: held-set parameter for chord
        // matching. InputManager passes its _heldGamepadButtons when
        // dispatching joyButton / joyAxis events; nullptr from keyboard
        // / mouse paths means chord-binding ShortcutInputs simply won't
        // match (defensive — they shouldn't be checked at all from those
        // paths since the deviceKind disagrees).
        void processEvent(const InputEvent& e, const std::set<uint32_t>* heldGamepadButtons = nullptr);
        bool processEventForSpecificShortcut(
            const InputEvent& e, std::string_view id,
            const std::set<uint32_t>* heldGamepadButtons = nullptr);

        static std::string_view getLegacyShortcutId(size_t index);
    };
} // namespace OpenRCT2::Ui
