/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI: On-Screen Keyboard. See osk-overhaul-plan.md for the
// current design. Post-rework summary:
// - No edit strip; the parent textbox / TextInput / console renders
//   the buffer above us.
// - No title bar → not draggable; pinned centre-bottom and repositions
//   on screen resize.
// - Navigation driven by Focus mode (kFocus* directional shortcuts +
//   kCursorClick activation + kCursorCancel mapped to Backspace).
// - SelectorMode::active is set on open and restored on close, so the
//   focus ring is the only selection indicator. No more cursor force-
//   off, no hardcoded scancode dispatch.
// - Key auto-repeat is driven by ShortcutManager held-state queries
//   for kCursorClick (re-fire focused key) and kCursorCancel (re-fire
//   Backspace), at the same 250 ms / 60 ms cadence as before.

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/input/ShortcutIds.h>
#include <openrct2-ui/input/ShortcutManager.h>
#include <openrct2-ui/input/WidgetFocus.h>
#include <openrct2-ui/interface/InGameConsole.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/Input.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/audio/AudioMixer.h>
#include <openrct2/core/String.hpp>
#include <openrct2/core/UTF8.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Drawing.String.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/interface/Widget.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/ui/WindowManager.h>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    namespace
    {
        // osk-overhaul §4: single height for every target. The edit
        // strip is gone, so console and textbox mode share geometry.
        // 5 rows × 42 px + 4 px top/bottom padding = 214.
        constexpr int32_t kOskFullWidth = 640;
        constexpr int32_t kOskHeight = 214;
        constexpr int16_t kCellW = 49;
        constexpr int16_t kRowH = 42;
        constexpr int16_t kKeyMargin = 2;
        constexpr int16_t kGridTopPad = 2;
        // osk-overhaul bug-fix §B: numpad is only 3 cells × 64 px wide
        // (no fill-the-screen need like QWERTY). Match the side buffer
        // to the existing top/bottom padding (kGridTopPad) so the
        // window's visible "frame margin" is uniform on all four sides.
        constexpr int16_t kNumpadCellW = 64;
        constexpr int16_t kNumpadCols = 3;
        constexpr int32_t kOskNumpadWidth = kNumpadCellW * kNumpadCols + kGridTopPad * 2;

        // Window width for a given mode. OpenSkeleton sizes the window
        // accordingly, BuildLayout fills it.
        constexpr int32_t modeWidth(OskMode mode)
        {
            return (mode == OskMode::full) ? kOskFullWidth : kOskNumpadWidth;
        }

        enum class OskAction : uint8_t
        {
            Insert,
            Caps,
            Return,
            Space,
            Backspace,
            Spacer, // visual gap, no widget, no selection slot
        };

        struct OskKeyDef
        {
            char glyph;        // 0 for action keys
            char shifted;      // 0 means same as glyph
            uint8_t span;      // in 1-cell units
            OskAction action;
            const char* label; // for action keys; nullptr for character keys
        };

        // Full QWERTY-ish layout. 5 rows, 13 cells per row max.
        // Backtick/tilde lives at the end of row 2 (right of '),
        // freeing the top-right slot for a Backspace key.
        const OskKeyDef kFullRow0[] = {
            { '1', '!', 1, OskAction::Insert, nullptr }, { '2', '@', 1, OskAction::Insert, nullptr },
            { '3', '#', 1, OskAction::Insert, nullptr }, { '4', '$', 1, OskAction::Insert, nullptr },
            { '5', '%', 1, OskAction::Insert, nullptr }, { '6', '^', 1, OskAction::Insert, nullptr },
            { '7', '&', 1, OskAction::Insert, nullptr }, { '8', '*', 1, OskAction::Insert, nullptr },
            { '9', '(', 1, OskAction::Insert, nullptr }, { '0', ')', 1, OskAction::Insert, nullptr },
            { '-', '_', 1, OskAction::Insert, nullptr }, { '=', '+', 1, OskAction::Insert, nullptr },
            { 0, 0, 1, OskAction::Backspace, "Bksp" },
        };
        const OskKeyDef kFullRow1[] = {
            { 'q', 'Q', 1, OskAction::Insert, nullptr }, { 'w', 'W', 1, OskAction::Insert, nullptr },
            { 'e', 'E', 1, OskAction::Insert, nullptr }, { 'r', 'R', 1, OskAction::Insert, nullptr },
            { 't', 'T', 1, OskAction::Insert, nullptr }, { 'y', 'Y', 1, OskAction::Insert, nullptr },
            { 'u', 'U', 1, OskAction::Insert, nullptr }, { 'i', 'I', 1, OskAction::Insert, nullptr },
            { 'o', 'O', 1, OskAction::Insert, nullptr }, { 'p', 'P', 1, OskAction::Insert, nullptr },
            { '[', '{', 1, OskAction::Insert, nullptr }, { ']', '}', 1, OskAction::Insert, nullptr },
        };
        const OskKeyDef kFullRow2[] = {
            { 'a', 'A', 1, OskAction::Insert, nullptr }, { 's', 'S', 1, OskAction::Insert, nullptr },
            { 'd', 'D', 1, OskAction::Insert, nullptr }, { 'f', 'F', 1, OskAction::Insert, nullptr },
            { 'g', 'G', 1, OskAction::Insert, nullptr }, { 'h', 'H', 1, OskAction::Insert, nullptr },
            { 'j', 'J', 1, OskAction::Insert, nullptr }, { 'k', 'K', 1, OskAction::Insert, nullptr },
            { 'l', 'L', 1, OskAction::Insert, nullptr }, { ';', ':', 1, OskAction::Insert, nullptr },
            { '\'', '"', 1, OskAction::Insert, nullptr }, { '`', '~', 1, OskAction::Insert, nullptr },
        };
        const OskKeyDef kFullRow3[] = {
            { 0, 0, 1, OskAction::Caps, "Cap" },
            { 'z', 'Z', 1, OskAction::Insert, nullptr }, { 'x', 'X', 1, OskAction::Insert, nullptr },
            { 'c', 'C', 1, OskAction::Insert, nullptr }, { 'v', 'V', 1, OskAction::Insert, nullptr },
            { 'b', 'B', 1, OskAction::Insert, nullptr }, { 'n', 'N', 1, OskAction::Insert, nullptr },
            { 'm', 'M', 1, OskAction::Insert, nullptr }, { ',', '<', 1, OskAction::Insert, nullptr },
            { '.', '>', 1, OskAction::Insert, nullptr }, { '/', '?', 1, OskAction::Insert, nullptr },
            { 0, 0, 2, OskAction::Return, "Enter" },
        };
        const OskKeyDef kFullRow4[] = {
            { 0, 0, 13, OskAction::Space, "Space" },
        };

        struct OskRowSpan
        {
            const OskKeyDef* keys;
            uint8_t count;
        };

        const OskRowSpan kFullLayout[] = {
            { kFullRow0, std::size(kFullRow0) }, { kFullRow1, std::size(kFullRow1) },
            { kFullRow2, std::size(kFullRow2) }, { kFullRow3, std::size(kFullRow3) },
            { kFullRow4, std::size(kFullRow4) },
        };

        // Numpad: 3 columns × 4 rows. Phone-style ordering. Cells are
        // larger here (no need to fit 13 wide) — we centre the grid.
        const OskKeyDef kNumRow0[] = {
            { '1', 0, 1, OskAction::Insert, nullptr }, { '2', 0, 1, OskAction::Insert, nullptr },
            { '3', 0, 1, OskAction::Insert, nullptr },
        };
        const OskKeyDef kNumRow1[] = {
            { '4', 0, 1, OskAction::Insert, nullptr }, { '5', 0, 1, OskAction::Insert, nullptr },
            { '6', 0, 1, OskAction::Insert, nullptr },
        };
        const OskKeyDef kNumRow2[] = {
            { '7', 0, 1, OskAction::Insert, nullptr }, { '8', 0, 1, OskAction::Insert, nullptr },
            { '9', 0, 1, OskAction::Insert, nullptr },
        };
        const OskKeyDef kNumRow3[] = {
            { '.', 0, 1, OskAction::Insert, nullptr }, { '0', 0, 1, OskAction::Insert, nullptr },
            { '-', 0, 1, OskAction::Insert, nullptr },
        };
        // Action row beneath the digit grid: Backspace on the left, an
        // empty middle cell, Return on the right.
        const OskKeyDef kNumRow4[] = {
            { 0, 0, 1, OskAction::Backspace, "Bksp" },
            { 0, 0, 1, OskAction::Spacer, nullptr },
            { 0, 0, 1, OskAction::Return, "Enter" },
        };
        const OskRowSpan kNumpadLayout[] = {
            { kNumRow0, std::size(kNumRow0) }, { kNumRow1, std::size(kNumRow1) },
            { kNumRow2, std::size(kNumRow2) }, { kNumRow3, std::size(kNumRow3) },
            { kNumRow4, std::size(kNumRow4) },
        };

        struct OskRuntimeKey
        {
            WidgetIndex widgetIdx;
            OskAction action;
            char glyph;
            char shifted;
        };

        // Per-key label backing store.
        struct GlyphBuffer
        {
            char data[8] = {};
        };

        // Repeat-on-hold cadence — kept identical to pre-rework values
        // so users feel no change in key-repeat tempo.
        constexpr uint32_t kRepeatInitialMs = 250;
        constexpr uint32_t kRepeatIntervalMs = 60;

        constexpr uint8_t kPressFlashFrames = 4;
        constexpr uint8_t kRejectFlashFrames = 8;

        // Which kind of caller spawned the OSK. The three paths differ
        // only in how Commit/Cancel dispatch.
        enum class OskTarget : uint8_t
        {
            TextInputWindow, // Modal TextInputWindow
            Textbox,         // Inline WindowStartTextbox widget
            Console,         // InGameConsole — Commit submits a line and stays open
        };

        class OskWindow final : public Window
        {
        private:
            OskMode _mode = OskMode::full;
            OskTarget _target = OskTarget::TextInputWindow;
            WidgetIndex _parentWidgetIdx = 0;
            WindowIdentifier _parentId{};
            std::vector<OskRuntimeKey> _runtimeKeys;
            std::vector<GlyphBuffer> _labels;
            WidgetIndex _firstKeyWidget = kWidgetIndexNull;
            bool _caps = false;
            int _pressFlashFrames = 0;
            // OPENRCT2MINI gamepad-plan 1.6c.3: modal-hooks token for
            // Confirm / Dismiss (Commit / Cancel) wiring. Popped in
            // onClose by token so console / textbox parent hooks below
            // are unaffected.
            OpenRCT2::Ui::InputManager::ModalHooksToken _modalHooksToken{};
            WidgetIndex _pressFlashIdx = kWidgetIndexNull;
            // Edit-buffer state. We still own the buffer to centralise
            // codepoint trimming on Insert / Backspace; the parent
            // textbox / TextInput / console renders the visible text.
            u8string _editBuffer;
            size_t _caret = 0;     // byte offset
            size_t _maxLength = 0; // codepoints; 0 = unlimited
            // Repeat-on-hold tracking. Updated in onUpdate via
            // ShortcutManager held-state queries.
            uint32_t _heldSinceMs = 0;
            uint32_t _lastFireMs = 0;
            std::string_view _heldShortcutId{};
            // OPENRCT2MINI: insertion-refusal flash. Re-rendered as a
            // brief red wash over the focused key (formerly painted the
            // edit strip — that's gone now).
            int _rejectFlashFrames = 0;

        public:
            void setMode(OskMode mode)
            {
                if (_mode == mode && !_runtimeKeys.empty())
                    return;
                _mode = mode;
                if (!widgets.empty())
                {
                    BuildLayout();
                    // osk-overhaul bug-fix §A2: clear focus on mode
                    // switch — the widget indices change and the
                    // bootstrap will re-snap to the first focusable
                    // on the next frame, just like for any window.
                    OpenRCT2::Ui::GetInputManager().clearFocus();
                    invalidate();
                }
            }

            void setTarget(OskTarget target, WidgetIndex widgetIdx)
            {
                _target = target;
                _parentWidgetIdx = widgetIdx;
            }

            void setParent(WindowBase* parent)
            {
                if (parent != nullptr)
                {
                    _parentId.classification = parent->classification;
                    _parentId.number = parent->number;
                    // Inherit the parent's secondary colour so the OSK
                    // visually belongs to the dialog that spawned it.
                    colours[0] = parent->colours[1];
                    colours[1] = parent->colours[1];
                    colours[2] = parent->colours[1];
                }
                else
                {
                    _parentId.classification = WindowClass::null;
                    _parentId.number = 0;
                    colours[0] = { Colour::grey };
                    colours[1] = { Colour::grey };
                    colours[2] = { Colour::grey };
                }
            }

            void setEditText(std::string_view text, size_t maxLength = 0)
            {
                _editBuffer = u8string{ text };
                _caret = _editBuffer.size();
                _maxLength = maxLength;
            }

            void onOpen() override
            {
                BuildLayout();
                gGamePaused |= GAME_PAUSED_MODAL;

                // OPENRCT2MINI osk-overhaul bug-fix §A2: don't touch
                // the selector mode here. The per-frame bootstrap
                // (InputManager::process) auto-wakes the selector
                // when a new topmost-focusable window appears AND the
                // user was last on virtual input (D-pad / keyboard);
                // it leaves the selector hidden if the user was last
                // on real mouse. That's exactly the modal behaviour
                // the rest of the UI gets — the OSK shouldn't be
                // special. Likewise focus snapping is auto-driven by
                // the bootstrap's snapFocusToTopmostFocusable, so
                // there's no need for a SnapInitialFocus call here.

                // Modal hooks for Confirm / Dismiss. The OSK keeps
                // owning these because Commit / Cancel needs to dispatch
                // to the parent (TextInputWindow / textbox / console)
                // before tearing down the OSK. PAD START / PAD BACK and
                // SDL RETURN / ESCAPE all funnel here through
                // ShortcutManager.
                auto& mgr = OpenRCT2::Ui::GetInputManager();
                _modalHooksToken = mgr.pushModalHooks({
                    /*dismiss=*/ [this](const OpenRCT2::Ui::InputEvent&) {
                        Cancel();
                        return true;
                    },
                    /*confirm=*/ [this](const OpenRCT2::Ui::InputEvent&) {
                        Commit();
                        return true;
                    },
                });
            }

            void onClose() override
            {
                gGamePaused &= ~GAME_PAUSED_MODAL;
                auto& mgr = OpenRCT2::Ui::GetInputManager();
                mgr.popModalHooks(_modalHooksToken);
                // OPENRCT2MINI osk-overhaul §6 / C14 / bug-fix §A: just
                // drop the focus claim. We no longer save/restore the
                // selector mode — that left the OSK as an outlier vs.
                // every other window, and forced a mode even after the
                // user moved the mouse. Letting the existing state
                // (whatever it transitioned to during the OSK's
                // lifetime) persist is the right behaviour.
                mgr.clearFocus();
            }

            void onUpdate() override
            {
                // OPENRCT2MINI osk-overhaul §5 / bug-fix §C: re-anchor
                // centre-bottom every frame. onResize on WindowBase
                // fires when the window itself is resized (e.g. user
                // drag), NOT on screen / SDL_WINDOWEVENT_SIZE_CHANGED;
                // the cheapest robust re-anchor is per-frame here.
                // Math matches OpenSkeleton so the OSK stays aligned
                // to whatever the screen size is right now.
                const int32_t screenW = ContextGetWidth();
                const int32_t screenH = ContextGetHeight();
                const int32_t desiredW = std::min<int32_t>(screenW, modeWidth(_mode));
                if (width != desiredW)
                {
                    width = desiredW;
                    BuildLayout();
                }
                const int16_t desiredX = static_cast<int16_t>(std::max(0, (screenW - desiredW) / 2));
                const int16_t desiredY = static_cast<int16_t>(std::max(0, screenH - height));
                if (windowPos.x != desiredX || windowPos.y != desiredY)
                {
                    windowPos.x = desiredX;
                    windowPos.y = desiredY;
                    invalidate();
                }

                if (_pressFlashFrames > 0)
                {
                    --_pressFlashFrames;
                    if (_pressFlashFrames == 0 && _pressFlashIdx != kWidgetIndexNull)
                    {
                        SyncPressedFlag(_pressFlashIdx);
                        invalidateWidget(_pressFlashIdx);
                        _pressFlashIdx = kWidgetIndexNull;
                    }
                }
                if (_rejectFlashFrames > 0)
                    --_rejectFlashFrames;
                if (_target == OskTarget::Console)
                {
                    OpenRCT2::Ui::GetInGameConsole().OskMirrorBuffer(_editBuffer, _caret);
                }
                ProcessRepeats();
                invalidate();
            }

            void onPrepareDraw() override
            {
                RefreshLabels();
                SyncCapsPressed();
            }

            void onDraw(RenderTarget& rt) override
            {
                // osk-overhaul §4: edit strip removed. The parent
                // window / console renders the typed text — drawing it
                // twice would be a confusing duplicate.
                drawWidgets(rt);
            }

            // OPENRCT2MINI osk-overhaul §1: replace `pressWidgetByIndex`
            // synthesis with a direct activation hook. Focus-mode dispatch
            // (InputManager → OskContextImpl::cursor.click) ends up here
            // via `pressWidgetByIndex` → onMouseDown; mouse clicks land
            // the same way via the normal hit-test path.
            void onMouseDown(WidgetIndex widgetIndex) override
            {
                ActivateKey(widgetIndex);
            }

            // Activate the key at `widgetIndex` (insert character / fire
            // backspace / commit / etc.). Public so OskContextImpl can
            // re-press the focused key during auto-repeat, and so the
            // free function OskActivateBackspace() can re-use the same
            // path for cursor.cancel → Backspace.
            void activateBackspace()
            {
                // Find the Backspace widget — used for cursor.cancel and
                // for the numpad-mode Backspace key. We don't always have
                // a Backspace widget (full QWERTY uses Cap+Z+Enter and
                // no separate Backspace), so fall back to direct buffer
                // edits when there's no widget to flash.
                for (const auto& rt : _runtimeKeys)
                {
                    if (rt.action == OskAction::Backspace)
                    {
                        ActivateKey(rt.widgetIdx);
                        return;
                    }
                }
                // No widget — fire backspace directly (no flash, no
                // sound). Full-QWERTY mode hits this path; the user is
                // intentionally rebinding cursor.cancel to backspace,
                // so silent behaviour is fine.
                Backspace();
                invalidate();
            }

            // Returns true if event consumed.
            // OPENRCT2MINI: snapshot accessors so the parent
            // TextInputWindow / inline textbox widgets can render the
            // OSK's edit buffer in real time.
            const u8string& getEditBuffer() const
            {
                return _editBuffer;
            }
            size_t getCaret() const
            {
                return _caret;
            }
            bool caretIsFlashed() const
            {
                // Visual blink driven off SDL_GetTicks so the parent
                // textbox / console caret renderer can sync without
                // sharing a frame counter.
                const uint32_t now = SDL_GetTicks();
                return (now / 500) & 1;
            }

        private:
            void BuildLayout()
            {
                widgets.clear();
                _runtimeKeys.clear();
                _labels.clear();
                _firstKeyWidget = kWidgetIndexNull;

                // First widget = background frame. Width tracks the
                // live window width (onUpdate keeps the window itself
                // sized correctly for the mode).
                const int16_t frameW = static_cast<int16_t>(width);
                widgets.push_back(makeWidget(
                    { 0, 0 }, { frameW, kOskHeight }, WidgetType::frame, WindowColour::primary));

                const OskRowSpan* layout = (_mode == OskMode::full) ? kFullLayout : kNumpadLayout;
                const size_t rowCount = (_mode == OskMode::full) ? std::size(kFullLayout)
                                                                 : std::size(kNumpadLayout);

                // Total potential keys for the label backing store.
                size_t total = 0;
                for (size_t r = 0; r < rowCount; ++r)
                    total += layout[r].count;
                _labels.resize(total);
                _runtimeKeys.reserve(total);

                const int16_t gridY0 = static_cast<int16_t>(kGridTopPad);
                const int16_t cellH = kRowH;

                size_t labelIdx = 0;
                for (size_t r = 0; r < rowCount; ++r)
                {
                    const auto& row = layout[r];
                    int16_t cursorX;
                    int16_t cellWThisRow;
                    if (_mode == OskMode::full)
                    {
                        cursorX = 0;
                        cellWThisRow = kCellW;
                    }
                    else
                    {
                        // Centre the 3-column numpad horizontally within
                        // the current frame width (numpad-mode window is
                        // already sized tight to the grid, so this is
                        // essentially just kGridTopPad on each side).
                        cellWThisRow = kNumpadCellW;
                        const int16_t gridW = static_cast<int16_t>(cellWThisRow * row.count);
                        cursorX = static_cast<int16_t>((width - gridW) / 2);
                    }
                    const int16_t y0 = static_cast<int16_t>(gridY0 + cellH * static_cast<int16_t>(r));

                    for (size_t k = 0; k < row.count; ++k)
                    {
                        const OskKeyDef& def = row.keys[k];
                        const int16_t spanW = static_cast<int16_t>(cellWThisRow * def.span);

                        if (def.action == OskAction::Spacer)
                        {
                            cursorX = static_cast<int16_t>(cursorX + spanW);
                            ++labelIdx;
                            continue;
                        }

                        const int16_t left = static_cast<int16_t>(cursorX + kKeyMargin);
                        const int16_t right = static_cast<int16_t>(cursorX + spanW - kKeyMargin - 1);
                        const int16_t top = static_cast<int16_t>(y0 + kKeyMargin);
                        const int16_t bottom = static_cast<int16_t>(y0 + cellH - kKeyMargin - 1);

                        if (def.label != nullptr)
                        {
                            std::strncpy(_labels[labelIdx].data, def.label, sizeof(_labels[labelIdx].data) - 1);
                        }
                        else
                        {
                            _labels[labelIdx].data[0] = def.glyph;
                            _labels[labelIdx].data[1] = '\0';
                        }

                        Widget w{};
                        w.type = WidgetType::button;
                        w.colour = static_cast<uint8_t>(WindowColour::secondary);
                        w.left = left;
                        w.right = right;
                        w.top = top;
                        w.bottom = bottom;
                        w.string = _labels[labelIdx].data;
                        w.flags.set(WidgetFlag::textIsString);
                        widgets.push_back(w);

                        OskRuntimeKey rt{};
                        rt.widgetIdx = static_cast<WidgetIndex>(widgets.size() - 1);
                        rt.action = def.action;
                        rt.glyph = def.glyph;
                        rt.shifted = def.shifted ? def.shifted : def.glyph;
                        _runtimeKeys.push_back(rt);

                        if (_firstKeyWidget == kWidgetIndexNull)
                            _firstKeyWidget = rt.widgetIdx;

                        cursorX = static_cast<int16_t>(cursorX + spanW);
                        ++labelIdx;
                    }
                }
            }

            void RefreshLabels()
            {
                // Label index advances per widget (skipping Spacers).
                size_t labelIdx = 0;
                for (const auto& rt : _runtimeKeys)
                {
                    if (rt.action == OskAction::Insert)
                    {
                        const char ch = _caps ? rt.shifted : rt.glyph;
                        _labels[labelIdx].data[0] = ch;
                        _labels[labelIdx].data[1] = '\0';
                        widgets[rt.widgetIdx].string = _labels[labelIdx].data;
                    }
                    ++labelIdx;
                }
            }

            void SyncCapsPressed()
            {
                for (const auto& key : _runtimeKeys)
                {
                    if (key.action == OskAction::Caps)
                    {
                        if (_caps)
                            widgets[key.widgetIdx].flags.set(WidgetFlag::isPressed);
                        else
                            widgets[key.widgetIdx].flags.unset(WidgetFlag::isPressed);
                    }
                }
                if (_pressFlashIdx != kWidgetIndexNull && _pressFlashFrames > 0)
                    widgets[_pressFlashIdx].flags.set(WidgetFlag::isPressed);
            }

            void SyncPressedFlag(WidgetIndex idx)
            {
                widgets[idx].flags.unset(WidgetFlag::isPressed);
                for (const auto& key : _runtimeKeys)
                {
                    if (key.widgetIdx == idx && key.action == OskAction::Caps && _caps)
                    {
                        widgets[idx].flags.set(WidgetFlag::isPressed);
                        return;
                    }
                }
            }

            const OskRuntimeKey* runtimeKeyFor(WidgetIndex idx) const
            {
                for (const auto& rt : _runtimeKeys)
                    if (rt.widgetIdx == idx)
                        return &rt;
                return nullptr;
            }

            void ActivateKey(WidgetIndex widgetIndex)
            {
                const auto* key = runtimeKeyFor(widgetIndex);
                if (key == nullptr)
                    return;
                StartFlash(key->widgetIdx);
                Audio::Play(Audio::SoundId::click1, 0, windowPos.x + (width / 2));
                switch (key->action)
                {
                    case OskAction::Insert:
                        InsertChar(_caps ? key->shifted : key->glyph);
                        break;
                    case OskAction::Space:
                        InsertChar(' ');
                        break;
                    case OskAction::Caps:
                        _caps = !_caps;
                        invalidate();
                        break;
                    case OskAction::Return:
                        Commit();
                        return;
                    case OskAction::Backspace:
                        Backspace();
                        break;
                    case OskAction::Spacer:
                        break;
                }
            }

            // Insert one char at the caret.
            void InsertChar(char ch)
            {
                if (_maxLength != 0)
                {
                    const size_t codepoints = String::lengthOf(_editBuffer.c_str());
                    if (codepoints >= _maxLength)
                    {
                        _rejectFlashFrames = kRejectFlashFrames;
                        invalidate();
                        return;
                    }
                }
                _editBuffer.insert(_caret, 1, ch);
                _caret++;
            }

            void Backspace()
            {
                if (_caret == 0)
                    return;
                // Walk back one codepoint (handles multi-byte UTF-8).
                const char* base = _editBuffer.data();
                const char* p = base + _caret;
                do
                {
                    --p;
                } while (p > base && (static_cast<uint8_t>(*p) & 0xC0) == 0x80);
                const size_t newCaret = static_cast<size_t>(p - base);
                _editBuffer.erase(newCaret, _caret - newCaret);
                _caret = newCaret;
            }

            void StartFlash(WidgetIndex idx)
            {
                _pressFlashIdx = idx;
                _pressFlashFrames = kPressFlashFrames;
                widgets[idx].flags.set(WidgetFlag::isPressed);
                invalidateWidget(idx);
            }

            // OPENRCT2MINI osk-overhaul §2.5: auto-repeat is driven by
            // ShortcutManager held-state queries, not SDL scancode
            // polling. cursor.click repeats activation of the currently
            // focused key (re-pressed); cursor.cancel repeats Backspace.
            // Same 250/60 ms cadence as before.
            void ProcessRepeats()
            {
                auto& mgr = OpenRCT2::Ui::GetInputManager();
                auto& shortcutMgr = OpenRCT2::Ui::GetShortcutManager();
                const uint32_t now = SDL_GetTicks();

                // Detect which (if any) repeat-eligible shortcut is held.
                std::string_view candidate{};
                auto checkHeld = [&](std::string_view id) -> bool {
                    if (auto* s = shortcutMgr.getShortcut(id); s != nullptr)
                        return mgr.getState(*s);
                    return false;
                };
                if (checkHeld(ShortcutId::kCursorCancel))
                    candidate = ShortcutId::kCursorCancel;
                else if (checkHeld(ShortcutId::kCursorClick))
                    candidate = ShortcutId::kCursorClick;

                if (candidate.empty())
                {
                    _heldShortcutId = {};
                    return;
                }
                if (_heldShortcutId != candidate)
                {
                    // New press — reset the clock; primary fire happens
                    // through normal shortcut dispatch, not from here.
                    _heldShortcutId = candidate;
                    _heldSinceMs = now;
                    _lastFireMs = now;
                    return;
                }
                // Should this key auto-repeat at all? Filter matches the
                // old ScancodeShouldRepeat semantics — Backspace yes,
                // Enter / Caps no.
                if (candidate == ShortcutId::kCursorClick)
                {
                    auto focused = mgr.getFocusedWidget();
                    const auto* key = runtimeKeyFor(focused);
                    if (key == nullptr || !ShouldRepeatAction(key->action))
                        return;
                }
                if (now - _heldSinceMs < kRepeatInitialMs)
                    return;
                if (now - _lastFireMs < kRepeatIntervalMs)
                    return;
                _lastFireMs = now;
                if (candidate == ShortcutId::kCursorCancel)
                {
                    activateBackspace();
                }
                else if (candidate == ShortcutId::kCursorClick)
                {
                    auto focused = mgr.getFocusedWidget();
                    ActivateKey(focused);
                }
            }

            static bool ShouldRepeatAction(OskAction action)
            {
                switch (action)
                {
                    case OskAction::Insert:
                    case OskAction::Space:
                    case OskAction::Backspace:
                        return true;
                    case OskAction::Caps:
                    case OskAction::Return:
                    case OskAction::Spacer:
                        return false;
                }
                return false;
            }

            void Commit()
            {
                std::string buffer = _editBuffer;
                const auto target = _target;
                if (target == OskTarget::Console)
                {
                    auto& console = OpenRCT2::Ui::GetInGameConsole();
                    console.OskSubmitLine(buffer);
                    _editBuffer.clear();
                    _caret = 0;
                    invalidate();
                    return;
                }
                auto* windowMgr = GetWindowManager();
                auto* parent = (_parentId.classification == WindowClass::null)
                    ? nullptr
                    : windowMgr->FindByNumber(_parentId.classification, _parentId.number);
                const auto widgetIdx = _parentWidgetIdx;
                close();
                if (parent == nullptr)
                    return;
                switch (target)
                {
                    case OskTarget::TextInputWindow:
                        TextInputCommitFromOsk(parent, buffer);
                        break;
                    case OskTarget::Textbox:
                        parent->onTextInput(widgetIdx, buffer);
                        WindowCancelTextbox();
                        break;
                    case OskTarget::Console:
                        break; // handled above
                }
            }

            void Cancel()
            {
                const auto target = _target;
                if (target == OskTarget::Console)
                {
                    OpenRCT2::Ui::GetInGameConsole().Close();
                    return;
                }
                auto* windowMgr = GetWindowManager();
                auto* parent = (_parentId.classification == WindowClass::null)
                    ? nullptr
                    : windowMgr->FindByNumber(_parentId.classification, _parentId.number);
                close();
                if (parent == nullptr)
                {
                    if (target == OskTarget::Textbox)
                        WindowCancelTextbox();
                    return;
                }
                switch (target)
                {
                    case OskTarget::TextInputWindow:
                        TextInputCancelFromOsk(parent);
                        break;
                    case OskTarget::Textbox:
                        WindowCancelTextbox();
                        break;
                    case OskTarget::Console:
                        break; // handled above
                }
            }
        };

        OskWindow* FindOsk()
        {
            auto* windowMgr = GetWindowManager();
            return static_cast<OskWindow*>(windowMgr->FindByClass(WindowClass::osk));
        }
    } // namespace

    namespace
    {
        OskWindow* OpenSkeleton(WindowBase* parent, OskMode mode)
        {
            auto* windowMgr = GetWindowManager();
            if (windowMgr->FindByClass(WindowClass::osk) != nullptr)
                return nullptr;

            const int32_t screenW = ContextGetWidth();
            const int32_t screenH = ContextGetHeight();
            // osk-overhaul bug-fix §B: numpad gets a tighter window
            // than full QWERTY. modeWidth() encapsulates the mapping.
            const int32_t w = std::min<int32_t>(screenW, modeWidth(mode));
            const auto pos = ScreenCoordsXY{ (screenW - w) / 2, screenH - kOskHeight };

            // OPENRCT2MINI osk-overhaul §5: noTitleBar removes the
            // caption widget and prevents the user from dragging the
            // window. WindowFlag::stickToFront keeps the OSK above
            // every other window class so the parent textbox /
            // TextInput / console below it stays interactive.
            auto* wnd = windowMgr->Create<OskWindow>(
                WindowClass::osk, pos, ScreenSize{ w, kOskHeight },
                { WindowFlag::stickToFront, WindowFlag::noTitleBar });
            if (wnd == nullptr)
                return nullptr;
            wnd->setMode(mode);
            wnd->setParent(parent);
            return wnd;
        }
    } // namespace

    void OskOpen(WindowBase* parent, OskMode mode)
    {
        auto* wnd = OpenSkeleton(parent, mode);
        if (wnd == nullptr)
            return;
        wnd->setTarget(OskTarget::TextInputWindow, 0);
        wnd->setEditText(TextInputReadBuffer(parent));
    }

    void OskOpenForTextbox(
        WindowBase* parent, WidgetIndex widgetIdx, std::string_view initialText, size_t maxLength, OskMode mode)
    {
        auto* wnd = OpenSkeleton(parent, mode);
        if (wnd == nullptr)
            return;
        wnd->setTarget(OskTarget::Textbox, widgetIdx);
        wnd->setEditText(initialText, maxLength);
    }

    void OskOpenForConsole()
    {
        // Console always uses the full keyboard. No parent window —
        // the console isn't a Window, it's drawn directly by
        // UiContext::Draw, so colour inheritance doesn't apply.
        auto* wnd = OpenSkeleton(nullptr, OskMode::full);
        if (wnd == nullptr)
            return;
        wnd->setTarget(OskTarget::Console, 0);
        wnd->setEditText("", 0);
    }

    int32_t OskGetActiveHeight()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return 0;
        return osk->height;
    }

    void OskClose()
    {
        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::osk);
    }

    bool OskIsActive()
    {
        auto* windowMgr = GetWindowManager();
        return windowMgr->FindByClass(WindowClass::osk) != nullptr;
    }

    // OPENRCT2MINI osk-overhaul §1: routed from OskContextImpl
    // cursor.cancel handler. Fires the OSK's Backspace key (with flash
    // + click sound when the active layout has a Backspace widget,
    // otherwise a silent buffer edit).
    void OskActivateBackspace()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return;
        osk->activateBackspace();
    }

    // OPENRCT2MINI osk-overhaul §1: invoked from OskContextImpl when
    // cursor.click is dispatched in the OSK context. Wraps
    // pressWidgetByIndex with the focused-widget lookup so the
    // strategy stays compact.
    void OskActivateFocusedKey()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return;
        auto& mgr = OpenRCT2::Ui::GetInputManager();
        const auto idx = mgr.getFocusedWidget();
        if (idx == kWidgetIndexNull)
            return;
        WidgetFocus::pressWidgetByIndex(*osk, idx);
    }

    std::string OskGetCurrentText()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return {};
        return std::string{ osk->getEditBuffer() };
    }

    size_t OskGetCaretByteOffset()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return 0;
        return osk->getCaret();
    }

    bool OskCaretIsFlashed()
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return false;
        return osk->caretIsFlashed();
    }
} // namespace OpenRCT2::Ui::Windows
