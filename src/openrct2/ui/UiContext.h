/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Context.h"
#include "../config/ConfigTypes.h"
#include "../interface/Cursors.h"

#include <memory>
#include <string>
#include <vector>

struct ScreenCoordsXY;
struct ITitleSequencePlayer;

namespace OpenRCT2
{
    namespace Drawing
    {
        struct IDrawingEngineFactory;
        struct IWeatherDrawer;
        struct RenderTarget;
        using DrawWeatherFunc = void (*)(
            RenderTarget& rt, IWeatherDrawer* weatherDrawer, int32_t left, int32_t top, int32_t width, int32_t height);
    } // namespace Drawing

    namespace Ui
    {
        struct IWindowManager;

        enum class FullscreenMode
        {
            windowed,
            fullscreen,
            fullscreenDesktop,
        };

        struct Resolution
        {
            int32_t Width;
            int32_t Height;
        };

        inline bool operator<(const Resolution& lhs, const Resolution& rhs)
        {
            int32_t areaA = lhs.Width * lhs.Height;
            int32_t areaB = rhs.Width * rhs.Height;
            if (areaA == areaB)
            {
                return lhs.Width < rhs.Width;
            }
            return areaA < areaB;
        }

        inline bool operator==(const Resolution& lhs, const Resolution& rhs)
        {
            return lhs.Width == rhs.Width && lhs.Height == rhs.Height;
        }

        inline bool operator!=(const Resolution& lhs, const Resolution& rhs)
        {
            return !(lhs == rhs);
        }

        enum class FileDialogType : uint8_t
        {
            Open,
            Save
        };

        struct FileDialogDesc
        {
            struct Filter
            {
                u8string Name;    // E.g. "Image Files"
                u8string Pattern; // E.g. "*.png;*.jpg;*.gif"

                Filter(u8string_view name, u8string_view pattern)
                    : Name(name)
                    , Pattern(pattern)
                {
                }
            };

            FileDialogType Type = FileDialogType::Open;
            u8string Title;
            u8string InitialDirectory;
            u8string DefaultFilename;
            std::vector<Filter> Filters;
        };

        /**
         * Represents the window or screen that OpenRCT2 is presented on.
         */
        struct IUiContext
        {
            virtual ~IUiContext() = default;

            virtual void InitialiseScriptExtensions() = 0;
            virtual void Tick() = 0;
            virtual void Draw(Drawing::RenderTarget& rt) = 0;

            // Window
            virtual void CreateWindow() = 0;
            virtual void CloseWindow() = 0;
            virtual void RecreateWindow() = 0;
            virtual void* GetWindow() = 0;
            virtual int32_t GetWidth() = 0;
            virtual int32_t GetHeight() = 0;
            virtual ScaleQuality GetScaleQuality() = 0;
            virtual void SetFullscreenMode(FullscreenMode mode) = 0;
            virtual const std::vector<Resolution>& GetFullscreenResolutions() = 0;
            virtual bool HasFocus() = 0;
            virtual bool IsMinimised() = 0;
            virtual bool IsSteamOverlayActive() = 0;
            virtual void ProcessMessages() = 0;
            virtual void TriggerResize() = 0;

            virtual void ShowMessageBox(const std::string& message) = 0;
            virtual int32_t ShowMessageBox(
                const std::string& title, const std::string& message, const std::vector<std::string>& options)
                = 0;

            virtual bool HasMenuSupport() = 0;
            // Creates a menu with a series of options, returns the index of the selected option
            virtual int32_t ShowMenuDialog(
                const std::vector<std::string>& options, const std::string& title, const std::string& text)
                = 0;
            virtual void OpenFolder(const std::string& path) = 0;
            virtual void OpenURL(const std::string& url) = 0;
            virtual std::string ShowFileDialog(const FileDialogDesc& desc) = 0;
            virtual std::string ShowDirectoryDialog(const std::string& title) = 0;
            virtual bool HasFilePicker() const = 0;

            // Input
            virtual const CursorState* GetCursorState() = 0;
            virtual CursorID GetCursor() = 0;
            virtual void SetCursor(CursorID cursor) = 0;
            virtual void SetCursorScale(uint8_t scale) = 0;
            virtual void SetCursorVisible(bool value) = 0;
            virtual ScreenCoordsXY GetCursorPosition() = 0;
            virtual void SetCursorPosition(const ScreenCoordsXY& cursorPosition) = 0;
            virtual void SetCursorTrap(bool value) = 0;
            virtual const uint8_t* GetKeysState() = 0;
            virtual const uint8_t* GetKeysPressed() = 0;
            virtual void SetKeysPressed(uint32_t keysym, uint8_t scancode) = 0;

            // OPENRCT2MINI gamepad-plan 1.11: haptic feedback. Walks every
            // connected SDL game controller and submits a rumble pulse via
            // SDL_GameControllerRumble. `low` and `high` are 0.0–1.0 motor
            // intensities (low-frequency / high-frequency). `durationMs`
            // is the maximum runtime — SDL clears the rumble automatically
            // when the timer expires; an in-progress rumble is overwritten
            // by a fresh call. Globally scaled by Config::Get().general.
            // gamepadRumbleIntensity and globally gated by gamepadRumble-
            // Enabled before the SDL call. Silent no-op on pads SDL
            // reports as having no rumble (SDL_GameControllerHasRumble),
            // and a complete no-op on the dummy UiContext used by headless
            // runs / tests / the Mini build pre-2.6.
            virtual void RumbleControllers(float low, float high, uint32_t durationMs) = 0;

            // OPENRCT2MINI gamepad-plan 1.13: DualShock-style RGB
            // lightbar control. SetControllerLED walks every connected
            // SDL game controller and submits an `SDL_GameControllerSet-
            // LED(r, g, b)` colour. Globally gated by Config::Get().
            // general.gamepadLedEnabled and scaled by gamepadLedBright-
            // ness before the SDL call, so users can dim the bright
            // DualShock 4/5 lightbars without re-authoring per-severity
            // colours. Pads SDL reports as having no LED (Xbox, the
            // Mini panel button-board, etc.) are silently skipped.
            //   r / g / b — raw 0..255 colour as defined by the
            //               severity → colour map in Led::tickEngine
            //               BEFORE brightness scaling. Implementation
            //               applies cfg.gamepadLedBrightness internally
            //               so callers don't have to know the global.
            // ControllerHasLED returns true if *any* currently-attached
            // pad reports an LED — used by Led::tickEngine to avoid
            // queueing a fade timer on no-LED setups.
            virtual void SetControllerLED(uint8_t r, uint8_t g, uint8_t b) = 0;
            virtual bool ControllerHasLED() = 0;

            // Drawing
            [[nodiscard]] virtual std::shared_ptr<Drawing::IDrawingEngineFactory> GetDrawingEngineFactory() = 0;
            virtual void DrawWeatherAnimation(
                Drawing::IWeatherDrawer* weatherDrawer, Drawing::RenderTarget& rt, Drawing::DrawWeatherFunc drawFunc)
                = 0;

            // Text input
            virtual bool IsTextInputActive() = 0;
            virtual TextInputSession* StartTextInput(u8string& buffer, size_t maxLength) = 0;
            virtual void StopTextInput() = 0;

            // In-game UI
            virtual IWindowManager* GetWindowManager() = 0;

            // Clipboard
            virtual bool SetClipboardText(const utf8* target) = 0;

            // HACK Until all title logic is moved to libopenrct2ui, we will need to provide some services
            virtual ITitleSequencePlayer* GetTitleSequencePlayer() = 0;
        };

        [[nodiscard]] std::unique_ptr<IUiContext> CreateDummyUiContext();
    } // namespace Ui
} // namespace OpenRCT2
