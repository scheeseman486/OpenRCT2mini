/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TextComposition.h"

#include "UiContext.h"
#include "UiStringIds.h"
#include "interface/InGameConsole.h"
#include "interface/Window.h"

#include <SDL.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/String.hpp>
#include <openrct2/core/UTF8.h>
#include <openrct2/ui/UiContext.h>

// OPENRCT2MINI text-editing-de-hardcode: KB_PRIMARY_MODIFIER macro
// removed. Every Ctrl-modified caret / clipboard action used to consult
// this macro directly to decide whether to fire the word-jump / copy /
// cut / paste variant. Those decisions are now made by the binding
// system: each modifier+key combination is a separate shortcut ID with
// its own default chord (CTRL+LEFT, CTRL+BACKSPACE, CTRL+C, etc.). On
// macOS the user is free to rebind the shortcuts to CMD+* via the
// Input Bindings UI — no platform-specific code path here.

using namespace OpenRCT2;
using namespace OpenRCT2::Ui;

bool TextComposition::IsActive()
{
    return SDL_IsTextInputActive() && _session.Buffer != nullptr;
}

TextInputSession* TextComposition::Start(u8string& buffer, size_t maxLength)
{
    SDL_StartTextInput();
    _session.Buffer = &buffer;
    _session.MaxLength = maxLength;
    _session.SelectionStart = buffer.size();
    _session.SelectionSize = 0;
    _session.ImeBuffer = _imeBuffer;
    RecalculateLength();
    return &_session;
}

void TextComposition::Stop()
{
    SDL_StopTextInput();
    _session.Buffer = nullptr;
    _session.ImeBuffer = nullptr;
    _imeActive = false;
}

/**
 * Remaps keypad enter keypress to normal enter and the numpad keys that can be used for navigation when num lock is off.
 * @return A pair with the remapped keycode and scancode.
 */
static std::pair<SDL_Keycode, SDL_Scancode> ProcessKeyPress(SDL_Keycode key, SDL_Scancode scancode)
{
    if (key == SDLK_KP_ENTER)
    {
        key = SDLK_RETURN;
        scancode = SDL_SCANCODE_RETURN;
    }
    else if (!(SDL_GetModState() & KMOD_NUM))
    {
        switch (key)
        {
            case SDLK_KP_1:
            {
                key = SDLK_END;
                scancode = SDL_SCANCODE_END;
                break;
            }
            case SDLK_KP_4:
            {
                key = SDLK_LEFT;
                scancode = SDL_SCANCODE_LEFT;
                break;
            }
            case SDLK_KP_6:
            {
                key = SDLK_RIGHT;
                scancode = SDL_SCANCODE_RIGHT;
                break;
            }
            case SDLK_KP_7:
            {
                key = SDLK_HOME;
                scancode = SDL_SCANCODE_HOME;
                break;
            }
            case SDLK_KP_PERIOD:
            {
                key = SDLK_DELETE;
                scancode = SDL_SCANCODE_DELETE;
                break;
            }
        }
    }
    return { key, scancode };
}

void TextComposition::HandleMessage(const SDL_Event* e)
{
    auto& console = GetInGameConsole();

    // OPENRCT2MINI osk-overhaul §6 / C5: when the OSK is up it owns
    // text editing — the OSK feeds characters via widget activations,
    // so SDL_TEXTINPUT must be gated off to prevent double-type when
    // a hardware key (or scancode-emitting gamepad button) also lands
    // in the SDL_TEXTINPUT path. SDL_KEYDOWN no longer needs gating
    // here: every edit-key dispatch (caret movement / backspace /
    // delete / clipboard) is now handled by bindable shortcuts whose
    // action lambdas check TextComposition::IsActive() before
    // mutating the buffer — and shortcuts are themselves gated by
    // the InputContext allow-lists in InputManager.cpp.
    const bool oskOpen = Windows::OskIsActive();

    switch (e->type)
    {
        case SDL_TEXTEDITING:
            // When inputting Korean characters, `edit.length` is always zero
            String::set(_imeBuffer, sizeof(_imeBuffer), e->edit.text);
            _imeStart = e->edit.start;
            _imeLength = e->edit.length;
            _imeActive = ((e->edit.length != 0 || String::sizeOf(e->edit.text) != 0) && _imeBuffer[0] != '\0');
            break;
        case SDL_TEXTINPUT:
            // will receive an `SDL_TEXTINPUT` event when a composition is committed
            _imeActive = false;
            _imeBuffer[0] = '\0';
            // OPENRCT2MINI osk-overhaul §6 / C5: skip when the OSK
            // owns text editing — the OSK feeds characters via its
            // widget activations and would double-type otherwise.
            if (_session.Buffer != nullptr && !oskOpen)
            {
                // HACK ` will close console, so don't input any text
                if (e->text.text[0] == '`' && console.IsOpen())
                {
                    break;
                }

                Insert(e->text.text);

                console.RefreshCaret(_session.SelectionStart);
                Windows::WindowUpdateTextbox();
            }
            break;
        case SDL_KEYDOWN:
        {
            if (_imeActive)
            {
                break;
            }

            SDL_Keycode rawKey = e->key.keysym.sym;
            SDL_Scancode rawScancode = e->key.keysym.scancode;

            auto [key, scancode] = ProcessKeyPress(rawKey, rawScancode);

            GetContext()->GetUiContext().SetKeysPressed(key, scancode);

            // OPENRCT2MINI text-editing-de-hardcode: SDL_KEYDOWN switch
            // for BACKSPACE / HOME / END / DELETE / LEFT / RIGHT /
            // CTRL+C / CTRL+V / CTRL+X removed. Each is now its own
            // bindable shortcut (interface.textediting.*) whose action
            // lambda dispatches the relevant TextComposition method.
            // RETURN was already removed earlier (gamepad-plan 1.6c.6)
            // and is handled by kInterfaceConfirm + textbox ModalHooks.
            // The remaining work in SDL_KEYDOWN is just the SetKeys-
            // Pressed cache update above — that still happens here so
            // the legacy keyboard-state polling code keeps working.
        }
    }
}

void TextComposition::CaretMoveToStart()
{
    _session.SelectionStart = 0;
}

void TextComposition::CaretMoveToEnd()
{
    size_t selectionOffset = _session.Buffer->size();
    const utf8* ch = _session.Buffer->c_str() + _session.SelectionStart;
    while (!UTF8IsCodepointStart(ch) && selectionOffset > 0)
    {
        ch--;
        selectionOffset--;
    }

    _session.SelectionStart = selectionOffset;
}

void TextComposition::CaretMoveLeft()
{
    size_t selectionOffset = _session.SelectionStart;
    if (selectionOffset == 0)
        return;

    const utf8* ch = _session.Buffer->c_str() + selectionOffset;
    do
    {
        ch--;
        selectionOffset--;
    } while (!UTF8IsCodepointStart(ch) && selectionOffset > 0);

    _session.SelectionStart = selectionOffset;
}

void TextComposition::CaretMoveRight()
{
    size_t selectionOffset = _session.SelectionStart;
    size_t selectionMaxOffset = _session.Buffer->size();
    if (selectionOffset >= selectionMaxOffset)
        return;

    const utf8* ch = _session.Buffer->c_str() + _session.SelectionStart;
    do
    {
        ch++;
        selectionOffset++;
    } while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset);

    _session.SelectionSize = std::max<size_t>(0, _session.SelectionSize - (selectionOffset - _session.SelectionStart));
    _session.SelectionStart = selectionOffset;
}

static bool isWhitespace(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

void TextComposition::CaretMoveToLeftToken()
{
    if (_session.SelectionStart == 0)
        return;

    size_t selectionOffset = _session.SelectionStart - 1;
    size_t lastChar = selectionOffset;

    const utf8* ch = _session.Buffer->c_str() + selectionOffset;

    // Read until first non-whitespace.
    while (selectionOffset > 0)
    {
        while (!UTF8IsCodepointStart(ch) && selectionOffset > 0)
        {
            ch--;
            selectionOffset--;
        }

        auto cp = UTF8GetNext(ch, nullptr);
        if (!isWhitespace(cp))
        {
            lastChar = selectionOffset;
            break;
        }
        if (selectionOffset == 0)
            break;
        ch--;
        selectionOffset--;
    }

    // Skip white spaces.
    while (selectionOffset > 0)
    {
        while (!UTF8IsCodepointStart(ch) && selectionOffset > 0)
        {
            ch--;
            selectionOffset--;
        }

        auto cp = UTF8GetNext(ch, nullptr);
        if (isWhitespace(cp))
            break;

        lastChar = selectionOffset;
        if (selectionOffset == 0)
            break;
        ch--;
        selectionOffset--;
    }

    _session.SelectionSize = _session.SelectionSize - (selectionOffset - _session.SelectionStart);
    _session.SelectionStart = selectionOffset == 0 ? 0 : lastChar;
}

void TextComposition::CaretMoveToRightToken()
{
    size_t selectionOffset = _session.SelectionStart;
    size_t selectionMaxOffset = _session.Buffer->size();

    if (selectionOffset >= selectionMaxOffset)
        return;

    const utf8* ch = _session.Buffer->c_str() + selectionOffset;

    // Find a valid codepoint start.
    while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset)
    {
        ch++;
        selectionOffset++;
    }
    auto cp = UTF8GetNext(ch, nullptr);

    if (isWhitespace(cp))
    {
        // Read until first non-whitespace.
        while (selectionOffset < selectionMaxOffset)
        {
            do
            {
                ch++;
                selectionOffset++;
            } while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset);

            cp = UTF8GetNext(ch, nullptr);
            if (!isWhitespace(cp))
                break;
        }
    }
    else
    {
        // Read until first non-whitespace.
        while (selectionOffset < selectionMaxOffset)
        {
            do
            {
                ch++;
                selectionOffset++;
            } while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset);

            cp = UTF8GetNext(ch, nullptr);
            if (isWhitespace(cp))
                break;
        }

        // Skip white spaces.
        while (selectionOffset < selectionMaxOffset)
        {
            // Read until first non-whitespace.
            do
            {
                ch++;
                selectionOffset++;
            } while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset);

            cp = UTF8GetNext(ch, nullptr);
            if (!isWhitespace(cp))
                break;
        }
    }

    _session.SelectionSize = std::max<size_t>(0, _session.SelectionSize - (selectionOffset - _session.SelectionStart));
    _session.SelectionStart = selectionOffset;
}

void TextComposition::Insert(const utf8* text)
{
    const utf8* ch = text;
    uint32_t codepoint;
    while ((codepoint = UTF8GetNext(ch, &ch)) != 0)
    {
        InsertCodepoint(codepoint);
    }
}

void TextComposition::InsertCodepoint(codepoint_t codepoint)
{
    size_t codepointLength = UTF8GetCodepointLength(codepoint);
    size_t remainingSize = _session.MaxLength - _session.Length;
    if (remainingSize > 0)
    {
        const auto bufSize = _session.Buffer->size();
        _session.Buffer->resize(_session.Buffer->size() + codepointLength);

        // FIXME: Just insert the codepoint into the string, don't use memmove

        utf8* buffer = _session.Buffer->data();
        utf8* insertPtr = buffer + _session.SelectionStart;
        if (_session.SelectionStart < bufSize)
        {
            // Shift bytes to the right to make room for new codepoint
            utf8* targetShiftPtr = insertPtr + codepointLength;
            size_t shiftSize = bufSize - _session.SelectionStart;
            memmove(targetShiftPtr, insertPtr, shiftSize);
        }

        UTF8WriteCodepoint(insertPtr, codepoint);
        _session.SelectionStart += codepointLength;
        _session.Length++;
    }
}

void TextComposition::Clear()
{
    _session.Buffer->clear();
    _session.Length = 0;
    _session.SelectionStart = 0;
    _session.SelectionSize = 0;
}

void TextComposition::Delete()
{
    size_t selectionOffset = _session.SelectionStart;
    size_t selectionMaxOffset = std::min(_session.SelectionStart + _session.SelectionSize, _session.Buffer->size());
    if (selectionOffset >= selectionMaxOffset)
        return;

    // Find out how many bytes to delete.
    const utf8* ch = _session.Buffer->c_str() + _session.SelectionStart;
    while (selectionOffset < selectionMaxOffset)
    {
        do
        {
            ch++;
            selectionOffset++;
        } while (!UTF8IsCodepointStart(ch) && selectionOffset < selectionMaxOffset);
    }

    size_t bytesToSkip = selectionOffset - _session.SelectionStart;
    if (bytesToSkip == 0)
        return;

    _session.Buffer->erase(
        _session.Buffer->begin() + _session.SelectionStart, _session.Buffer->begin() + _session.SelectionStart + bytesToSkip);
    _session.SelectionSize = 0;

    RecalculateLength();
}

void TextComposition::RecalculateLength()
{
    _session.Length = String::lengthOf(_session.Buffer->c_str());
}

// OPENRCT2MINI text-editing-de-hardcode: clipboard operations
// promoted out of the SDL_KEYDOWN switch in HandleMessage. Each is
// now invoked by the matching bindable shortcut's action lambda in
// Shortcuts.cpp. Behaviour is byte-for-byte identical to the legacy
// SDLK_c / SDLK_v / SDLK_x cases.
void TextComposition::ClipboardCopy()
{
    if (_session.Buffer == nullptr || _session.Length == 0)
        return;

    GetContext()->GetUiContext().SetClipboardText(_session.Buffer->c_str());
    ContextShowError(STR_COPY_INPUT_TO_CLIPBOARD, kStringIdNone, {});
}

void TextComposition::ClipboardCut()
{
    if (_session.Buffer == nullptr || _session.Length == 0)
        return;

    GetContext()->GetUiContext().SetClipboardText(_session.Buffer->c_str());
    Clear();
    Windows::WindowUpdateTextbox();
    ContextShowError(STR_COPY_INPUT_TO_CLIPBOARD, kStringIdNone, {});
}

void TextComposition::ClipboardPaste()
{
    if (_session.Buffer == nullptr || !SDL_HasClipboardText())
        return;

    auto& console = GetInGameConsole();
    utf8* text = SDL_GetClipboardText();
    Insert(text);
    SDL_free(text);
    console.RefreshCaret(_session.SelectionStart);
    Windows::WindowUpdateTextbox();
}

// OPENRCT2MINI text-editing-de-hardcode: backspace + delete composite
// operations. Each runs the sequence that used to live inline in the
// SDL_KEYDOWN switch — caret move, set SelectionSize, Delete, refresh
// console caret + textbox. Keeping this inside the class lets us
// continue to mutate the private _session fields between the caret
// move and the Delete.
void TextComposition::BackspaceCharacter()
{
    if (_session.Buffer == nullptr || _session.SelectionStart == 0)
        return;

    size_t endOffset = _session.SelectionStart;
    CaretMoveLeft();
    _session.SelectionSize = endOffset - _session.SelectionStart;
    Delete();

    GetInGameConsole().RefreshCaret(_session.SelectionStart);
    Windows::WindowUpdateTextbox();
}

void TextComposition::BackspaceWord()
{
    if (_session.Buffer == nullptr || _session.SelectionStart == 0)
        return;

    size_t endOffset = _session.SelectionStart;
    CaretMoveToLeftToken();
    _session.SelectionSize = endOffset - _session.SelectionStart;
    Delete();

    GetInGameConsole().RefreshCaret(_session.SelectionStart);
    Windows::WindowUpdateTextbox();
}

void TextComposition::DeleteCharacter()
{
    if (_session.Buffer == nullptr)
        return;

    size_t startOffset = _session.SelectionStart;
    CaretMoveRight();
    _session.SelectionSize = _session.SelectionStart - startOffset;
    _session.SelectionStart = startOffset;
    Delete();

    GetInGameConsole().RefreshCaret(_session.SelectionStart);
    Windows::WindowUpdateTextbox();
}

void TextComposition::DeleteWord()
{
    if (_session.Buffer == nullptr)
        return;

    size_t startOffset = _session.SelectionStart;
    CaretMoveToRightToken();
    _session.SelectionSize = _session.SelectionStart - startOffset;
    _session.SelectionStart = startOffset;
    Delete();

    GetInGameConsole().RefreshCaret(_session.SelectionStart);
    Windows::WindowUpdateTextbox();
}

size_t TextComposition::GetCaretPosition() const
{
    return _session.SelectionStart;
}
