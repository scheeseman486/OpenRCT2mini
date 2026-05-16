/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace OpenRCT2
{
    struct IContext;
    struct IPlatformEnvironment;
} // namespace OpenRCT2

namespace OpenRCT2::Ui
{
    struct FileDialogDesc;
    struct IUiContext;

    class InGameConsole;
    class InputManager;
    class ShortcutManager;
    class TextComposition;

    struct IPlatformUiContext
    {
        virtual ~IPlatformUiContext() = default;
        virtual void SetWindowIcon(SDL_Window* window) = 0;
        virtual bool IsSteamOverlayAttached() = 0;

        virtual void ShowMessageBox(SDL_Window* window, const std::string& message) = 0;
        virtual bool HasMenuSupport() = 0;
        virtual int32_t ShowMenuDialog(
            const std::vector<std::string>& options, const std::string& title, const std::string& text)
            = 0;
        virtual void OpenFolder(const std::string& path) = 0;

        virtual void OpenURL(const std::string& url) = 0;
        virtual std::string ShowFileDialog(SDL_Window* window, const FileDialogDesc& desc) = 0;
        virtual std::string ShowDirectoryDialog(SDL_Window* window, const std::string& title) = 0;

        virtual bool HasFilePicker() const = 0;
    };

    [[nodiscard]] std::unique_ptr<IUiContext> CreateUiContext(IPlatformEnvironment& env);
    [[nodiscard]] std::unique_ptr<IPlatformUiContext> CreatePlatformUiContext();

    [[nodiscard]] InGameConsole& GetInGameConsole();
    [[nodiscard]] InputManager& GetInputManager();
    [[nodiscard]] ShortcutManager& GetShortcutManager();
    // OPENRCT2MINI text-editing-de-hardcode: expose TextComposition so
    // the bindable caret/clipboard shortcut action lambdas in
    // Shortcuts.cpp can reach the live instance owned by UiContext.
    [[nodiscard]] TextComposition& GetTextComposition();

    // OPENRCT2MINI gamepad-plan 1.5c: action bridges for shade-related
    // shortcuts. Called from action lambdas in Shortcuts.cpp.
    void FireToggleShadeAll();
    void FireCloseWindowUnderCursor();
    // OPENRCT2MINI hold-binding refactor: bridge for shade-window
    // tap action. Was previously fired by the per-frame poll in
    // ProcessWorldCursor; now fires via the kInterfaceShadeWindowUnderCursor
    // action lambda when the hold-binding dispatch fires it on tap-
    // release (release-before-500ms-threshold).
    void FireShadeWindowUnderCursor();
} // namespace OpenRCT2::Ui
