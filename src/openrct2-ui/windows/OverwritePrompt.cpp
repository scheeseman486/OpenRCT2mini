/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <SDL_keycode.h>
#include <openrct2-ui/UiContext.h>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/interface/FileBrowser.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Text.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/ui/WindowManager.h>
#include <string>

struct TrackDesign;

namespace OpenRCT2::Ui::Windows
{
    static constexpr ScreenSize kWindowSize = { 200, 100 };

    enum
    {
        WIDX_OVERWRITE_BACKGROUND,
        WIDX_OVERWRITE_TITLE,
        WIDX_OVERWRITE_CLOSE,
        WIDX_OVERWRITE_OVERWRITE,
        WIDX_OVERWRITE_CANCEL
    };

    // clang-format off
    static const auto window_overwrite_prompt_widgets = makeWidgets(
        makeWindowShim(STR_FILEBROWSER_OVERWRITE_TITLE, kWindowSize),
        makeWidget({                     10, kWindowSize.height - 20 }, { 84, 11 }, WidgetType::button, WindowColour::primary, STR_FILEBROWSER_OVERWRITE_TITLE),
        makeWidget({ kWindowSize.width - 95, kWindowSize.height - 20 }, { 85, 11 }, WidgetType::button, WindowColour::primary, STR_SAVE_PROMPT_CANCEL)
    );
    // clang-format on

    class OverwritePromptWindow final : public Window
    {
        std::string _name;
        std::string _path;
        LoadSaveAction _action;
        LoadSaveType _type;
        TrackDesign* _trackDesign;

    public:
        OverwritePromptWindow(
            const std::string_view name, const std::string_view path, LoadSaveAction action, LoadSaveType type,
            TrackDesign* trackDesignPtr)
            : _name(name)
            , _path(path)
            , _action(action)
            , _type(type)
            , _trackDesign(trackDesignPtr)
        {
        }

        void onOpen() override
        {
            setWidgets(window_overwrite_prompt_widgets);
            // OPENRCT2MINI gamepad-plan 1.6c.4: ESC / RETURN /
            // PAD BACK / PAD START all route through the modal hooks
            // now. Replaces the hardcoded keycode switch in
            // WindowLoadSaveOverwritePromptInputKey below (kept as a
            // legacy entry point for callers that still pass keycodes
            // directly, but in practice unreachable while hooks are
            // installed — the InputManager dispatch fires first).
            _modalHooksToken = OpenRCT2::Ui::GetInputManager().pushModalHooks({
                /*dismiss=*/ [this](const OpenRCT2::Ui::InputEvent&) {
                    onMouseUp(WIDX_OVERWRITE_CANCEL);
                    return true;
                },
                /*confirm=*/ [this](const OpenRCT2::Ui::InputEvent&) {
                    onMouseUp(WIDX_OVERWRITE_OVERWRITE);
                    return true;
                },
            });
        }

        void onClose() override
        {
            OpenRCT2::Ui::GetInputManager().popModalHooks(_modalHooksToken);
        }

    private:
        // OPENRCT2MINI gamepad-plan 1.6c.4: token from pushModalHooks.
        OpenRCT2::Ui::InputManager::ModalHooksToken _modalHooksToken{};

    public:

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_OVERWRITE_OVERWRITE:
                {
                    FileBrowser::Select(_path.c_str(), _action, _type, _trackDesign);

                    // As the LoadSaveWindow::Select function can change the order of the
                    // windows we can't use WindowClose(w).
                    auto* windowMgr = GetWindowManager();
                    windowMgr->CloseByClass(WindowClass::loadsaveOverwritePrompt);
                    break;
                }

                case WIDX_OVERWRITE_CANCEL:
                case WIDX_OVERWRITE_CLOSE:
                    close();
                    break;
            }
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            drawWidgets(rt);

            auto ft = Formatter();
            ft.Add<StringId>(STR_STRING);
            ft.Add<char*>(_name.c_str());

            ScreenCoordsXY stringCoords(windowPos.x + width / 2, windowPos.y + (height / 2) - 3);
            drawTextWrapped(rt, stringCoords, width - 4, STR_FILEBROWSER_OVERWRITE_PROMPT, ft, { TextAlignment::centre });
        }
    };

    WindowBase* WindowOverwritePromptOpen(
        const std::string_view name, const std::string_view path, LoadSaveAction action, LoadSaveType type,
        TrackDesign* trackDesignPtr)
    {
        auto* windowMgr = GetWindowManager();
        windowMgr->CloseByClass(WindowClass::loadsaveOverwritePrompt);

        return windowMgr->Create<OverwritePromptWindow>(
            WindowClass::loadsaveOverwritePrompt, kWindowSize,
            { WindowFlag::transparent, WindowFlag::stickToFront, WindowFlag::centreScreen }, name, path, action, type,
            trackDesignPtr);
    }

    void WindowLoadSaveOverwritePromptInputKey(WindowBase* w, uint32_t keycode)
    {
        // OPENRCT2MINI gamepad-plan 1.6c.7: ESC/RETURN/KP_ENTER cases
        // deleted. Both keys now route through InputManager's
        // ModalHooks dispatch (installed by onOpen, popped by
        // onClose) and fire WIDX_OVERWRITE_OVERWRITE / _CANCEL via
        // the registered callbacks before reaching this entry point.
        // The InputManager keyboard-branch gate that calls this
        // function still early-returns on any keyboard event while
        // the prompt is up, swallowing non-confirm/dismiss keys
        // (preventing e.g. RETURN-bound camera rotate from firing).
        // Function body becomes a no-op.
        (void)w;
        (void)keycode;
    }
} // namespace OpenRCT2::Ui::Windows
