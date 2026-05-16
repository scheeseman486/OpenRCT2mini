/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI per-binding Modifier mode: per-binding options window.
// Reached from the wrench button next to each binding cell in the
// Input Bindings list. Targets a single binding identified by
// (shortcutId, kind, indexWithinKind) — keyboard / mouse / gamepad
// bucket + which of that bucket's entries (in current[] order).
//
// Single control: a "Modifier Key" checkbox controlling the binding's
// `is_modifier` flag. Toggling saves immediately (no Apply button) —
// mirrors Haptics / LED.

#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/UiStringIds.h>
#include <openrct2-ui/input/ShortcutManager.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/ui/WindowManager.h>
#include <string>

namespace OpenRCT2::Ui::Windows
{
    // Small window: title bar + one checkbox row. 220 wide gives the
    // localised "Modifier Key" label some breathing room; 60 tall fits
    // title + checkbox + padding.
    static constexpr ScreenSize kBindingOptionsWindowSize = { 220, 60 };

    // ODR-unique tag — LTO whole-program check flags `enum WidgetIdx`
    // clashes across TUs sharing this namespace (Haptics, LED, etc.).
    enum BindingOptionsWidgetIdx : WidgetIndex
    {
        WIDX_BACKGROUND,
        WIDX_TITLE,
        WIDX_CLOSE,
        WIDX_MODIFIER_CHECKBOX,
    };

    // clang-format off
    static const auto kBindingOptionsWidgets = makeWidgets(
        makeWindowShim(STR_BINDING_OPTIONS, kBindingOptionsWindowSize),
        makeWidget   ({ 10, 25 }, { 200, 12 }, WidgetType::checkbox, WindowColour::secondary,
                      STR_BINDING_MODIFIER_KEY, STR_BINDING_MODIFIER_KEY_TIP)
    );
    // clang-format on

    class BindingOptionsWindow final : public Window
    {
        std::string _shortcutId;
        BindingOptionsKind _kind = BindingOptionsKind::keyboard;
        int32_t _indexWithinKind = 0;

    public:
        void Configure(std::string_view shortcutId, BindingOptionsKind kind, int32_t indexWithinKind)
        {
            _shortcutId = shortcutId;
            _kind = kind;
            _indexWithinKind = indexWithinKind;
        }

        void onOpen() override
        {
            setWidgets(kBindingOptionsWidgets);
            WindowInitScrollWidgets(*this);
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_CLOSE:
                    close();
                    return;

                case WIDX_MODIFIER_CHECKBOX:
                {
                    auto* binding = findBinding();
                    if (binding == nullptr)
                        return;
                    binding->isModifier = !binding->isModifier;
                    // Save through the user-bindings round-trip so the
                    // change persists across launches. Same path used
                    // by clearBindingsOfKind / commitPendingCapture.
                    GetShortcutManager().saveUserBindings();
                    // Invalidate the parent Input Bindings window so
                    // any future flag-driven display tweak picks up
                    // the change immediately (today none does, but
                    // the invalidate hook is cheap).
                    if (auto* mgr = GetWindowManager(); mgr != nullptr)
                    {
                        if (auto* w = mgr->FindByClass(WindowClass::keyboardShortcutList); w != nullptr)
                            w->invalidate();
                    }
                    invalidate();
                    return;
                }
            }
        }

        void onPrepareDraw() override
        {
            auto* binding = findBinding();
            const bool live = (binding != nullptr) && bindingHasModifierPortion(*binding);
            setCheckboxValue(WIDX_MODIFIER_CHECKBOX, (binding != nullptr) && binding->isModifier);
            // Single-key bindings have no modifier portion — the flag
            // is N/A, so grey the checkbox out to make that clear.
            setWidgetDisabled(WIDX_MODIFIER_CHECKBOX, !live);
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            drawWidgets(rt);
        }

    private:
        bool bindingHasModifierPortion(const ShortcutInput& binding) const
        {
            // Modifier portion exists iff the chord has 2+ keys total.
            // Keyboard / mouse: 1 trigger + N modifier-mask bits.
            //   "modifiers != 0" implies ≥ 2 keys total.
            // Gamepad: 1 trigger + N chord modifiers.
            //   "chordModifiers non-empty" implies ≥ 2 keys total.
            // joyHat / single-input axis: always 1 key — N/A.
            return binding.modifiers != 0 || !binding.chordModifiers.empty();
        }

        ShortcutInput* findBinding()
        {
            auto& mgr = GetShortcutManager();
            auto* shortcut = mgr.getShortcut(_shortcutId);
            if (shortcut == nullptr)
                return nullptr;
            int32_t seen = 0;
            for (auto& b : shortcut->current)
            {
                if (!kindMatches(b.kind))
                    continue;
                if (seen == _indexWithinKind)
                    return &b;
                seen++;
            }
            return nullptr;
        }

        bool kindMatches(InputDeviceKind k) const
        {
            switch (_kind)
            {
                case BindingOptionsKind::keyboard:
                    return k == InputDeviceKind::keyboard;
                case BindingOptionsKind::mouse:
                    return k == InputDeviceKind::mouse;
                case BindingOptionsKind::gamepad:
                    return k == InputDeviceKind::joyButton
                        || k == InputDeviceKind::joyHat
                        || k == InputDeviceKind::joyAxis;
            }
            return false;
        }
    };

    WindowBase* BindingOptionsOpen(
        std::string_view shortcutId, BindingOptionsKind kind, int32_t indexWithinKind)
    {
        auto* windowMgr = GetWindowManager();
        // Always (re)open: switching to a different binding from another
        // wrench should refresh the contents, not bring forward a stale
        // window pointing at the previous binding.
        windowMgr->CloseByClass(WindowClass::bindingOptions);
        auto* window = windowMgr->Create<BindingOptionsWindow>(
            WindowClass::bindingOptions, kBindingOptionsWindowSize,
            WindowFlags{ WindowFlag::centreScreen });
        if (window != nullptr)
        {
            window->Configure(shortcutId, kind, indexWithinKind);
            window->invalidate();
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows
