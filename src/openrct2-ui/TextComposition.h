/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <openrct2/Input.h>

union SDL_Event;

namespace OpenRCT2::Ui
{
    class TextComposition
    {
    private:
        TextInputSession _session = {};

        bool _imeActive = false;
        int32_t _imeStart = 0;
        int32_t _imeLength = 0;
        utf8 _imeBuffer[32] = {};

    public:
        bool IsActive();
        TextInputSession* Start(u8string& buffer, size_t maxLength);
        void Stop();
        void HandleMessage(const SDL_Event* e);

        // OPENRCT2MINI text-editing-de-hardcode: caret + clipboard
        // operations promoted to the public API. These used to be
        // private helpers called from the hardcoded SDL_KEYDOWN switch
        // in HandleMessage; the switch is gone — each operation is
        // now driven by an action lambda in Shortcuts.cpp that
        // resolves the live TextComposition via GetTextComposition()
        // and invokes the relevant method.
        void CaretMoveToStart();
        void CaretMoveToEnd();
        void CaretMoveLeft();
        void CaretMoveRight();
        void CaretMoveToLeftToken();
        void CaretMoveToRightToken();
        void Insert(const utf8* text);
        void Delete();
        // OPENRCT2MINI text-editing-de-hardcode: clipboard operations
        // were previously inline in the SDLK_c / SDLK_v / SDLK_x cases
        // of HandleMessage. Now exposed so the bindable shortcuts can
        // call them.
        void ClipboardCopy();
        void ClipboardCut();
        void ClipboardPaste();
        // OPENRCT2MINI text-editing-de-hardcode: backspace / delete
        // sequences live here as composite methods because they need
        // to touch the private _session SelectionStart / SelectionSize
        // fields between the caret move and the Delete call. The
        // shortcut action lambdas in Shortcuts.cpp call these instead
        // of reaching into the session directly.
        void BackspaceCharacter();
        void BackspaceWord();
        void DeleteCharacter();
        void DeleteWord();
        // OPENRCT2MINI text-editing-de-hardcode: caret-position getter
        // for the in-game console RefreshCaret call. Used by the
        // CaretMoveLeft/Right shortcut action lambdas after they fire
        // a movement method.
        size_t GetCaretPosition() const;

    private:
        void InsertCodepoint(codepoint_t codepoint);
        void Clear();
        void RecalculateLength();
    };
} // namespace OpenRCT2::Ui
