/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI: On-Screen Keyboard. See osk-plan.md for the full design.
//
// 640×240 strip pinned to the bottom of the screen. Top 24 px is an
// edit strip; the bottom 216 px is a 5-row × 13-column key grid in
// either full-QWERTY layout or a 3×4 numpad. Selection is a contrasting
// outline drawn in onDrawWidget *after* the standard widgetDraw, so the
// widget's own pressed flag stays free for Caps-Lock-on rendering.

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
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
        constexpr int32_t kOskWidth = 640;
        constexpr int32_t kOskHeight = 240;
        constexpr int32_t kEditStripH = 26;
        constexpr int16_t kCellW = 49;
        constexpr int16_t kRowH = 42;
        constexpr int16_t kKeyMargin = 2;

        enum class OskAction : uint8_t
        {
            Insert,
            Caps,
            Return,
            Space,
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
        const OskKeyDef kFullRow0[] = {
            { '`', '~', 1, OskAction::Insert, nullptr }, { '1', '!', 1, OskAction::Insert, nullptr },
            { '2', '@', 1, OskAction::Insert, nullptr }, { '3', '#', 1, OskAction::Insert, nullptr },
            { '4', '$', 1, OskAction::Insert, nullptr }, { '5', '%', 1, OskAction::Insert, nullptr },
            { '6', '^', 1, OskAction::Insert, nullptr }, { '7', '&', 1, OskAction::Insert, nullptr },
            { '8', '*', 1, OskAction::Insert, nullptr }, { '9', '(', 1, OskAction::Insert, nullptr },
            { '0', ')', 1, OskAction::Insert, nullptr }, { '-', '_', 1, OskAction::Insert, nullptr },
            { '=', '+', 1, OskAction::Insert, nullptr },
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
            { '\'', '"', 1, OskAction::Insert, nullptr },
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
        const OskRowSpan kNumpadLayout[] = {
            { kNumRow0, std::size(kNumRow0) }, { kNumRow1, std::size(kNumRow1) },
            { kNumRow2, std::size(kNumRow2) }, { kNumRow3, std::size(kNumRow3) },
        };

        struct OskRuntimeKey
        {
            WidgetIndex widgetIdx;
            int16_t centreX;
            OskAction action;
            char glyph;
            char shifted;
            uint8_t span;
        };

        // Per-key label backing store. Single chars need a 2-byte
        // null-terminated buffer for setString(); keep them inline so
        // pointers stay valid for the OSK's lifetime.
        struct GlyphBuffer
        {
            char data[8] = {};
        };

        // Repeat-on-hold cadence (matches the cursor stack's tunables).
        constexpr uint32_t kRepeatInitialMs = 250;
        constexpr uint32_t kRepeatIntervalMs = 60;

        constexpr uint8_t kPressFlashFrames = 4;

        // Which kind of caller spawned the OSK. The two paths differ
        // only in how Commit dispatches the typed text back.
        enum class OskTarget : uint8_t
        {
            TextInputWindow, // Modal TextInputWindow
            Textbox,         // Inline WindowStartTextbox widget
        };

        class OskWindow final : public Window
        {
        private:
            OskMode _mode = OskMode::full;
            OskTarget _target = OskTarget::TextInputWindow;
            WidgetIndex _parentWidgetIdx = 0;
            WindowIdentifier _parentId{};
            std::vector<std::vector<OskRuntimeKey>> _keys;
            std::vector<GlyphBuffer> _labels;
            int _selRow = 0;
            int _selKey = 0;
            bool _caps = false;
            int _pressFlashFrames = 0;
            WidgetIndex _pressFlashIdx = kWidgetIndexNull;
            // Edit-strip state.
            u8string _editBuffer;
            size_t _caret = 0;     // byte offset
            size_t _maxLength = 0; // codepoints; 0 = unlimited
            int _cursorBlink = 0;
            // Repeat-on-hold tracking. SDL doesn't fire auto-repeat on
            // device, so we synthesise it ourselves.
            int32_t _heldScancode = 0;
            uint32_t _heldSinceMs = 0;
            uint32_t _lastFireMs = 0;

        public:
            void setMode(OskMode mode)
            {
                if (_mode == mode && !_keys.empty())
                    return;
                _mode = mode;
                // OnOpen already built the layout using the default
                // mode; rebuild now that the caller has overridden it.
                // Skipped if the layout hasn't been built yet (i.e.
                // setMode called BEFORE onOpen — currently never, but
                // the guard means it'll work either way).
                if (!widgets.empty())
                {
                    BuildLayout();
                    // Reset selection to the right starting cell for
                    // the new layout. Per §3.8b: home row leftmost for
                    // full QWERTY, top-left for numpad.
                    if (_mode == OskMode::full)
                    {
                        _selRow = 2;
                        _selKey = 0;
                    }
                    else
                    {
                        _selRow = 0;
                        _selKey = 0;
                    }
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
                // Sensible initial selection per §3.8b: home row leftmost
                // for the full keyboard, top-left '1' for the numpad.
                if (_mode == OskMode::full)
                {
                    _selRow = 2;
                    _selKey = 0;
                }
                else
                {
                    _selRow = 0;
                    _selKey = 0;
                }
            }

            void onClose() override
            {
                gGamePaused &= ~GAME_PAUSED_MODAL;
            }

            void onUpdate() override
            {
                _cursorBlink = (_cursorBlink + 1) % 30;
                if (_pressFlashFrames > 0)
                {
                    --_pressFlashFrames;
                    if (_pressFlashFrames == 0 && _pressFlashIdx != kWidgetIndexNull)
                    {
                        // Restore baseline pressed state once the flash
                        // ends. CAPS keeps its toggle; everything else
                        // goes back to unpressed.
                        SyncPressedFlag(_pressFlashIdx);
                        invalidateWidget(_pressFlashIdx);
                        _pressFlashIdx = kWidgetIndexNull;
                    }
                }
                ProcessRepeats();
                invalidate();
            }

            void onPrepareDraw() override
            {
                // Refresh shifted-glyph labels when caps changes.
                RefreshLabels();
                SyncCapsPressed();
            }

            void onDraw(RenderTarget& rt) override
            {
                // Background + standard widget pass.
                drawWidgets(rt);
                DrawEditStrip(rt);
            }

            void onDrawWidget(WidgetIndex widgetIndex, RenderTarget& rt) override
            {
                // Default rendering (frame, button face, label).
                Window::onDrawWidget(widgetIndex, rt);

                // Selection outline — drawn AFTER widgetDraw so it sits
                // on top. Colour intentionally distinct from the
                // inherited theme so it's legible against any parent.
                if (widgetIndex == GetSelectedWidgetIdx())
                {
                    const auto& widget = widgets[widgetIndex];
                    const auto rect = ScreenRect{
                        { windowPos.x + widget.left, windowPos.y + widget.top },
                        { windowPos.x + widget.right, windowPos.y + widget.bottom },
                    };
                    Rectangle::fillInset(
                        rt, rect, ColourWithFlags{ Colour::brightYellow },
                        Rectangle::BorderStyle::outset, Rectangle::FillBrightness::light, Rectangle::FillMode::none);
                }
            }

            // Returns true if event consumed.
            bool handleKey(int32_t scancode, bool down)
            {
                // Always swallow these scancodes while the OSK is up so
                // shortcut chord handlers further down the pipeline
                // don't fire (e.g. F16 = X = game-speed cycle).
                const bool consumed = IsOskScancode(scancode);
                if (!consumed)
                    return false;
                if (!down)
                {
                    // KEY-UP: clear repeat state if it matches.
                    if (_heldScancode == scancode)
                    {
                        _heldScancode = 0;
                        _heldSinceMs = 0;
                    }
                    return true;
                }
                // KEY-DOWN: fire once, then start the repeat clock.
                FireScancode(scancode);
                _heldScancode = scancode;
                _heldSinceMs = SDL_GetTicks();
                _lastFireMs = _heldSinceMs;
                return true;
            }

        private:
            void BuildLayout()
            {
                widgets.clear();
                _keys.clear();
                _labels.clear();

                // First widget = background frame.
                widgets.push_back(makeWidget(
                    { 0, 0 }, { kOskWidth, kOskHeight }, WidgetType::frame, WindowColour::primary));

                const OskRowSpan* layout = (_mode == OskMode::full) ? kFullLayout : kNumpadLayout;
                const size_t rowCount = (_mode == OskMode::full) ? std::size(kFullLayout)
                                                                 : std::size(kNumpadLayout);
                _keys.resize(rowCount);

                // Total potential keys for the label backing store.
                size_t total = 0;
                for (size_t r = 0; r < rowCount; ++r)
                    total += layout[r].count;
                _labels.resize(total);

                const int16_t gridY0 = static_cast<int16_t>(kEditStripH);
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
                        // Centre the 3-column numpad horizontally.
                        cellWThisRow = 64;
                        const int16_t gridW = static_cast<int16_t>(cellWThisRow * row.count);
                        cursorX = static_cast<int16_t>((kOskWidth - gridW) / 2);
                    }
                    const int16_t y0 = static_cast<int16_t>(gridY0 + cellH * static_cast<int16_t>(r));

                    _keys[r].reserve(row.count);
                    for (size_t k = 0; k < row.count; ++k)
                    {
                        const OskKeyDef& def = row.keys[k];
                        const int16_t spanW = static_cast<int16_t>(cellWThisRow * def.span);
                        const int16_t left = static_cast<int16_t>(cursorX + kKeyMargin);
                        const int16_t right = static_cast<int16_t>(cursorX + spanW - kKeyMargin - 1);
                        const int16_t top = static_cast<int16_t>(y0 + kKeyMargin);
                        const int16_t bottom = static_cast<int16_t>(y0 + cellH - kKeyMargin - 1);

                        // Build the label string into our backing store.
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
                        rt.centreX = static_cast<int16_t>((left + right) / 2);
                        rt.action = def.action;
                        rt.glyph = def.glyph;
                        rt.shifted = def.shifted ? def.shifted : def.glyph;
                        rt.span = def.span;
                        _keys[r].push_back(rt);

                        cursorX = static_cast<int16_t>(cursorX + spanW);
                        ++labelIdx;
                    }
                }
            }

            void RefreshLabels()
            {
                size_t labelIdx = 0;
                for (size_t r = 0; r < _keys.size(); ++r)
                {
                    for (size_t k = 0; k < _keys[r].size(); ++k)
                    {
                        const auto& rt = _keys[r][k];
                        if (rt.action == OskAction::Insert)
                        {
                            const char ch = _caps ? rt.shifted : rt.glyph;
                            _labels[labelIdx].data[0] = ch;
                            _labels[labelIdx].data[1] = '\0';
                            // Re-bind the widget to the same backing
                            // pointer (it's cached in widget.string but
                            // the byte we changed is already visible).
                            widgets[rt.widgetIdx].string = _labels[labelIdx].data;
                        }
                        ++labelIdx;
                    }
                }
            }

            void SyncCapsPressed()
            {
                // CAPS key reflects toggle state via isPressed.
                for (auto& row : _keys)
                {
                    for (auto& key : row)
                    {
                        if (key.action == OskAction::Caps)
                        {
                            if (_caps)
                                widgets[key.widgetIdx].flags.set(WidgetFlag::isPressed);
                            else
                                widgets[key.widgetIdx].flags.unset(WidgetFlag::isPressed);
                        }
                    }
                }
                if (_pressFlashIdx != kWidgetIndexNull && _pressFlashFrames > 0)
                    widgets[_pressFlashIdx].flags.set(WidgetFlag::isPressed);
            }

            void SyncPressedFlag(WidgetIndex idx)
            {
                // Restore baseline pressed state for one widget after a
                // flash ends. Most keys are unpressed; CAPS reflects
                // _caps.
                widgets[idx].flags.unset(WidgetFlag::isPressed);
                for (auto& row : _keys)
                {
                    for (auto& key : row)
                    {
                        if (key.widgetIdx == idx && key.action == OskAction::Caps && _caps)
                        {
                            widgets[idx].flags.set(WidgetFlag::isPressed);
                            return;
                        }
                    }
                }
            }

            WidgetIndex GetSelectedWidgetIdx() const
            {
                if (_selRow < 0 || _selRow >= static_cast<int>(_keys.size()))
                    return kWidgetIndexNull;
                if (_selKey < 0 || _selKey >= static_cast<int>(_keys[_selRow].size()))
                    return kWidgetIndexNull;
                return _keys[_selRow][_selKey].widgetIdx;
            }

            const OskRuntimeKey& GetSelectedKey() const
            {
                return _keys[_selRow][_selKey];
            }

            void DrawEditStrip(RenderTarget& rt)
            {
                const auto stripRect = ScreenRect{
                    { windowPos.x + 4, windowPos.y + 3 },
                    { windowPos.x + kOskWidth - 5, windowPos.y + kEditStripH - 4 },
                };
                Rectangle::fillInset(
                    rt, stripRect, colours[1], Rectangle::BorderStyle::inset, Rectangle::FillBrightness::light,
                    Rectangle::FillMode::dontLightenWhenInset);

                // Render the buffer text. Single line — if the buffer is
                // longer than the strip can show, scroll horizontally so
                // the caret stays visible. Plain ASCII assumption is
                // safe for the OSK.
                const int32_t textY = stripRect.GetTop() + 4;
                const int32_t textXLeft = stripRect.GetLeft() + 6;
                const int32_t textXRight = stripRect.GetRight() - 6;
                const int32_t stripWidth = textXRight - textXLeft;

                // Caret-visible scroll: measure the prefix up to the
                // caret to figure out where it lands, then offset so the
                // caret is in view.
                const auto fontStyle = FontStyle::medium;
                const auto preCaret = u8string_view{ _editBuffer.data(), _caret };
                const int32_t preWidth = getStringWidth(preCaret, fontStyle, true);
                int32_t scrollX = 0;
                if (preWidth > stripWidth - 8)
                    scrollX = preWidth - (stripWidth - 8);

                drawText(
                    rt, ScreenCoordsXY{ textXLeft - scrollX, textY }, _editBuffer.data(),
                    { colours[1], fontStyle, { TextPaintFlag::noFormatting }, TextAlignment::left });

                // Caret blink — visible for 15 of every 30 frames.
                if (_cursorBlink < 15)
                {
                    const int32_t caretX = textXLeft + preWidth - scrollX;
                    const auto caretColour = getColourMap(colours[1].colour).midLight;
                    Rectangle::fill(
                        rt, ScreenRect{ { caretX, textY }, { caretX + 1, textY + 9 } },
                        static_cast<PaletteIndex>(EnumValue(caretColour) + 5));
                }
            }

            // Insert one char at the caret.
            void InsertChar(char ch)
            {
                if (_maxLength != 0)
                {
                    // Trim rule: codepoint count < max.
                    const size_t codepoints = String::lengthOf(_editBuffer.c_str());
                    if (codepoints >= _maxLength)
                        return;
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

            void CaretLeft()
            {
                if (_caret == 0)
                    return;
                const char* base = _editBuffer.data();
                const char* p = base + _caret;
                do
                {
                    --p;
                } while (p > base && (static_cast<uint8_t>(*p) & 0xC0) == 0x80);
                _caret = static_cast<size_t>(p - base);
            }

            void CaretRight()
            {
                if (_caret >= _editBuffer.size())
                    return;
                size_t step = 1;
                while (_caret + step < _editBuffer.size()
                       && (static_cast<uint8_t>(_editBuffer[_caret + step]) & 0xC0) == 0x80)
                    ++step;
                _caret += step;
            }

            // Numpad-only: flip the sign of the current value. Concretely
            // toggle a leading '-' on the buffer. Caret tracks the
            // shift so the user's logical position is preserved.
            void ToggleSign()
            {
                if (!_editBuffer.empty() && _editBuffer.front() == '-')
                {
                    _editBuffer.erase(0, 1);
                    if (_caret > 0)
                        --_caret;
                }
                else
                {
                    _editBuffer.insert(0, 1, '-');
                    ++_caret;
                }
            }

            void StartFlash(WidgetIndex idx)
            {
                _pressFlashIdx = idx;
                _pressFlashFrames = kPressFlashFrames;
                widgets[idx].flags.set(WidgetFlag::isPressed);
                invalidateWidget(idx);
            }

            void ActivateSelectedKey()
            {
                const auto& key = GetSelectedKey();
                StartFlash(key.widgetIdx);
                switch (key.action)
                {
                    case OskAction::Insert:
                        InsertChar(_caps ? key.shifted : key.glyph);
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
                }
            }

            void MoveSelectionLeft()
            {
                if (_keys[_selRow].empty())
                    return;
                _selKey = (_selKey - 1 + static_cast<int>(_keys[_selRow].size())) % static_cast<int>(_keys[_selRow].size());
            }

            void MoveSelectionRight()
            {
                if (_keys[_selRow].empty())
                    return;
                _selKey = (_selKey + 1) % static_cast<int>(_keys[_selRow].size());
            }

            void MoveSelectionVertical(int dir)
            {
                const int newRow = _selRow + dir;
                if (newRow < 0 || newRow >= static_cast<int>(_keys.size()))
                    return; // clamp
                if (_keys[newRow].empty())
                    return;
                const int16_t targetX = _keys[_selRow][_selKey].centreX;
                int bestKey = 0;
                int bestDist = std::numeric_limits<int>::max();
                for (size_t k = 0; k < _keys[newRow].size(); ++k)
                {
                    const int dist = std::abs(_keys[newRow][k].centreX - targetX);
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestKey = static_cast<int>(k);
                    }
                }
                _selRow = newRow;
                _selKey = bestKey;
            }

            void Commit()
            {
                auto* windowMgr = GetWindowManager();
                auto* parent = (_parentId.classification == WindowClass::null)
                    ? nullptr
                    : windowMgr->FindByNumber(_parentId.classification, _parentId.number);
                std::string buffer = _editBuffer;
                const auto target = _target;
                const auto widgetIdx = _parentWidgetIdx;
                // Close OSK first; then dispatch to parent. The parent's
                // own onClose path calls OskClose() too, which is a
                // no-op once we're already gone.
                close();
                if (parent == nullptr)
                    return;
                switch (target)
                {
                    case OskTarget::TextInputWindow:
                        TextInputCommitFromOsk(parent, buffer);
                        break;
                    case OskTarget::Textbox:
                        // Push the buffer to the parent widget the same
                        // way WindowUpdateTextbox does, then end the
                        // engine's textbox session.
                        parent->onTextInput(widgetIdx, buffer);
                        WindowCancelTextbox();
                        break;
                }
            }

            void Cancel()
            {
                auto* windowMgr = GetWindowManager();
                auto* parent = (_parentId.classification == WindowClass::null)
                    ? nullptr
                    : windowMgr->FindByNumber(_parentId.classification, _parentId.number);
                const auto target = _target;
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
                }
            }

            // What scancodes belong to the OSK while it's active.
            static bool IsOskScancode(int32_t scancode)
            {
                switch (scancode)
                {
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_LEFT:
                    case SDL_SCANCODE_RIGHT:
                    case SDL_SCANCODE_Z:      // A button — insert
                    case SDL_SCANCODE_X:      // B button — backspace
                    case SDL_SCANCODE_F16:    // X button — space
                    case SDL_SCANCODE_F17:    // Y button — caps
                    case SDL_SCANCODE_LSHIFT: // L1 — caret left
                    case SDL_SCANCODE_RSHIFT:
                    case SDL_SCANCODE_LALT:   // R1 — caret right
                    case SDL_SCANCODE_RALT:
                    case SDL_SCANCODE_RETURN: // Start — commit
                    case SDL_SCANCODE_ESCAPE: // Select — cancel
                    case SDL_SCANCODE_F14:    // L2 — swallow (no-op)
                    case SDL_SCANCODE_F15:    // R2 — swallow (no-op)
                        return true;
                    default:
                        return false;
                }
            }

            void FireScancode(int32_t scancode)
            {
                switch (scancode)
                {
                    case SDL_SCANCODE_UP:
                        MoveSelectionVertical(-1);
                        break;
                    case SDL_SCANCODE_DOWN:
                        MoveSelectionVertical(+1);
                        break;
                    case SDL_SCANCODE_LEFT:
                        MoveSelectionLeft();
                        break;
                    case SDL_SCANCODE_RIGHT:
                        MoveSelectionRight();
                        break;
                    case SDL_SCANCODE_Z:
                        ActivateSelectedKey();
                        break;
                    case SDL_SCANCODE_X:
                        Backspace();
                        break;
                    case SDL_SCANCODE_F16:
                        if (_mode == OskMode::numpad)
                            ToggleSign();
                        else
                            InsertChar(' ');
                        break;
                    case SDL_SCANCODE_F17:
                        if (_mode == OskMode::full)
                            _caps = !_caps;
                        // Numpad: Y is unused — see §6.3.
                        break;
                    case SDL_SCANCODE_LSHIFT:
                    case SDL_SCANCODE_RSHIFT:
                        CaretLeft();
                        break;
                    case SDL_SCANCODE_LALT:
                    case SDL_SCANCODE_RALT:
                        CaretRight();
                        break;
                    case SDL_SCANCODE_RETURN:
                        Commit();
                        break;
                    case SDL_SCANCODE_ESCAPE:
                        Cancel();
                        break;
                    default:
                        break;
                }
            }

            void ProcessRepeats()
            {
                if (_heldScancode == 0)
                    return;
                // Only nav / backspace / caret keys repeat — see
                // §3.8b. Don't repeat A (insert), X (space), Y (caps),
                // Start (commit), Select (cancel).
                if (!ScancodeShouldRepeat(_heldScancode))
                    return;
                const uint32_t now = SDL_GetTicks();
                if (now - _heldSinceMs < kRepeatInitialMs)
                    return;
                if (now - _lastFireMs < kRepeatIntervalMs)
                    return;
                _lastFireMs = now;
                FireScancode(_heldScancode);
            }

            static bool ScancodeShouldRepeat(int32_t sc)
            {
                switch (sc)
                {
                    case SDL_SCANCODE_UP:
                    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_LEFT:
                    case SDL_SCANCODE_RIGHT:
                    case SDL_SCANCODE_X:      // backspace
                    case SDL_SCANCODE_LSHIFT:
                    case SDL_SCANCODE_RSHIFT:
                    case SDL_SCANCODE_LALT:
                    case SDL_SCANCODE_RALT:
                        return true;
                    default:
                        return false;
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
            const int32_t w = std::min<int32_t>(screenW, kOskWidth);
            const auto pos = ScreenCoordsXY{ (screenW - w) / 2, screenH - kOskHeight };

            auto* wnd = windowMgr->Create<OskWindow>(
                WindowClass::osk, pos, ScreenSize{ w, kOskHeight }, WindowFlag::stickToFront);
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

    bool OskHandleKey(int32_t sdlScancode, bool down)
    {
        auto* osk = FindOsk();
        if (osk == nullptr)
            return false;
        return osk->handleKey(sdlScancode, down);
    }
} // namespace OpenRCT2::Ui::Windows
