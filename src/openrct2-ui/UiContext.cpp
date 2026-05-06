/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "UiContext.h"

#include "CursorRepository.h"
#include "SDLException.h"
#include "TextComposition.h"
#include "UiStringIds.h"
#include "WindowManager.h"
#include "drawing/engines/DrawingEngineFactory.hpp"
#include "input/ShortcutManager.h"
#include "interface/InGameConsole.h"
#include "interface/Theme.h"
#include "interface/Viewport.h"
#include "scripting/UiExtensions.h"
#include "title/TitleSequencePlayer.h"

#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <openrct2-ui/input/InputManager.h>
#include <openrct2-ui/input/MouseInput.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>
#include <openrct2/Input.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/Version.h>
#include <openrct2/network/Network.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/audio/AudioMixer.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/String.hpp>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/interface/Chat.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/scenes/title/TitleSequencePlayer.h>
#include <openrct2/scripting/ScriptEngine.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/world/Location.hpp>
#include <vector>

#ifdef __EMSCRIPTEN__
    #include <emscripten.h>
    #include <emscripten/html5.h>
#endif

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

// OPENRCT2MINI cut 59: forward declaration for the rotate-construction-object
// helper. Definition is in input/Shortcuts.cpp; cut 59 dropped its `static`
// qualifier so we can reach it from UiContext to wire the gamepad Y / R1+Y
// bindings without going through the keyboard shortcut layer.
void ShortcutRotateConstructionObject();

#ifdef __MACOSX__
    // macOS uses COMMAND rather than CTRL for many keyboard shortcuts
    #define KB_PRIMARY_MODIFIER KMOD_GUI
#else
    #define KB_PRIMARY_MODIFIER KMOD_CTRL
#endif

class UiContext final : public IUiContext
{
private:
    constexpr static uint32_t kTouchDoubleTimeout = 300;

    const std::unique_ptr<IPlatformUiContext> _platformUiContext;
    const std::unique_ptr<IWindowManager> _windowManager;

    CursorRepository _cursorRepository;

    SDL_Window* _window = nullptr;
    int32_t _width = 0;
    int32_t _height = 0;
    ScaleQuality _scaleQuality = ScaleQuality::NearestNeighbour;

    std::vector<Resolution> _fsResolutions;

    bool _steamOverlayActive = false;

    // Input
    InputManager _inputManager;
    ShortcutManager _shortcutManager;
    TextComposition _textComposition;
    CursorState _cursorState = {};
    uint32_t _lastKeyPressed = 0;
    const uint8_t* _keysState = nullptr;
    uint8_t _keysPressed[256] = {};
    uint32_t _lastGestureTimestamp = 0;
    float _gestureRadius = 0;

#ifdef ENABLE_SOFTWARE_CURSOR
    // OPENRCT2MINI: cut 38. Virtual-cursor state — D-pad/arrow keys drive the
    // position; A/Z = left-click, B/X = right-click. Sub-pixel accumulators
    // so accelerated hold doesn't quantise to integer steps. Centre is
    // filled in lazily on first poll so we don't have to know the window
    // size at construction time.
    float _vcursorX = 0.0f;
    float _vcursorY = 0.0f;
    bool _vcursorInitialised = false;
    uint32_t _vdpadHeldSinceMs = 0; // tick when any D-pad direction first went down
    uint32_t _vcursorLastTickMs = 0; // OPENRCT2MINI cut 44c: previous-call ms for dt
    // OPENRCT2MINI cut 47: last int32 position WE wrote to _cursorState.
    // If on the next call _cursorState.position differs from this, a real
    // mouse-motion or another input source moved the cursor; we resync.
    // If it matches, we keep the float _vcursor* values unchanged so
    // sub-pixel deltas accumulate.
    int32_t _vcursorLastIntX = -1;
    int32_t _vcursorLastIntY = -1;
    bool _vprevA = false;
    bool _vprevB = false;
    // Keyboard pressed-state, latched from intercepted SDL_KEYDOWN / SDL_KEYUP
    // so the keys never reach the shortcut handler (arrows would otherwise
    // pan the camera and Z/X would trigger menu shortcuts).
    bool _vKbUp = false, _vKbDown = false, _vKbLeft = false, _vKbRight = false;
    bool _vKbZ = false, _vKbX = false, _vKbShift = false;
    // OPENRCT2MINI cut 59: R1 (F13) is also our "gamepad modifier" — when held,
    // L2/R2/Y change action (rotate view / rotate object anti-clockwise
    // instead of zoom / rotate clockwise).
    bool _vGamepadMod = false;
    // OPENRCT2MINI cut 60: latched LCTRL/RCTRL state. Set by L1+R1 chord (the
    // SDL driver swaps L1's emitted modifier from LSHIFT to LCTRL when R1 is
    // held) or by a real keyboard Ctrl. Used to suppress fast cursor while
    // Ctrl is held — Ctrl is the construction Z-lock modifier and fast
    // cursor defeats the precision it provides.
    bool _vKbCtrl = false;

    // Returns true if the scancode is one of our virtual-cursor keys; in that
    // case state is updated and the caller should NOT propagate the event to
    // the rest of the keyboard pipeline.
    //
    // OPENRCT2MINI cut 53b: removed the `_textComposition.IsActive()` early
    // return. Cut 38b yielded arrow keys to the text-input layer when a
    // filename / chat field was focused, but on the Miyoo Mini there's no
    // keyboard to type with — yielding cursor control during a text input
    // just freezes the cursor and leaves the user unable to click Cancel
    // or OK. Cursor movement is always more useful than within-text-field
    // arrow nav for these targets.
    bool InterceptVirtualCursorKey(SDL_Scancode sc, bool down)
    {
        // OPENRCT2MINI OSK: when the on-screen keyboard is up, route every
        // device button to it instead of running the cursor / shortcut
        // pipeline. The OSK consumes arrows for selection, A/B/X/Y for
        // edit actions, L1/R1 for caret motion, Start/Select for
        // commit/cancel. We forward the scancode + down state and trust
        // its return value.
        if (Windows::OskIsActive() && Windows::OskHandleKey(static_cast<int32_t>(sc), down))
            return true;
        switch (sc)
        {
            case SDL_SCANCODE_UP:    _vKbUp = down;    return true;
            case SDL_SCANCODE_DOWN:  _vKbDown = down;  return true;
            case SDL_SCANCODE_LEFT:  _vKbLeft = down;  return true;
            case SDL_SCANCODE_RIGHT: _vKbRight = down; return true;
            case SDL_SCANCODE_Z:     _vKbZ = down;     return true;
            case SDL_SCANCODE_X:     _vKbX = down;     return true;
            // OPENRCT2MINI cut 58/59/60/61: R1 = LALT (was RSHIFT in 58, F13
            // in 59-60). Alt is on every PC keyboard so the dev can test
            // fast-cursor / gamepad-mod natively. We swallow the event AND
            // clear KMOD_LALT/RALT from SDL's mod state — without the
            // clear, subsequent keypresses (e.g. Start = RETURN) would
            // carry KMOD_ALT into the shortcut matcher and trigger
            // ALT+RETURN (windowed-mode toggle). OpenRCT2's only default
            // Alt bindings are ALT+RETURN and CTRL+ALT+C; neither has any
            // role on the device, and on the host the dev can use Options
            // for fullscreen toggle.
            case SDL_SCANCODE_LALT:
            case SDL_SCANCODE_RALT:
                _vKbShift = down;       // fast cursor
                _vGamepadMod = down;    // gamepad modifier (L2/R2/Y semantics)
                SDL_SetModState(static_cast<SDL_Keymod>(
                    SDL_GetModState() & ~(KMOD_LALT | KMOD_RALT)));
                return true;
            // OPENRCT2MINI cut 59: L1 (LSHIFT) is the Shift modifier reach,
            // but it no longer activates fast cursor — only R1 (F13) does.
            // We don't swallow LSHIFT/RSHIFT so the modifier still
            // propagates to OpenRCT2's input pipeline (scenery
            // vertical-stack, track/footpath Z-raise, etc.).
            case SDL_SCANCODE_LSHIFT:
            case SDL_SCANCODE_RSHIFT: return false;
            // OPENRCT2MINI cut 60: latch Ctrl. The SDL driver emits LCTRL for
            // the L1+R1 chord (in place of L1's usual LSHIFT). Real keyboard
            // Ctrl also lands here. Don't swallow — the modifier propagates
            // to OpenRCT2's InputManager so the construction Z-lock paths
            // see KMOD_CTRL. The latch is consumed in
            // ProcessVirtualGamepadCursor to suppress fast cursor.
            case SDL_SCANCODE_LCTRL:
            case SDL_SCANCODE_RCTRL: _vKbCtrl = down; return false;
            // OPENRCT2MINI cut 59: face X / face Y / L2 / R2 onto F-keys
            // emitted by the cut 43 set_key patch. Action fires on press;
            // release is silent.
            // OPENRCT2MINI polish: rotate view is the default for L2/R2;
            // R1 (gamepad-modifier) flips them to zoom. The original
            // arrangement (zoom default, R1 to rotate) was awkward when
            // sightseeing — most "I want to look around" actions are
            // rotation, and zoom is occasional. The chord pattern stays
            // consistent: the modifier always swaps to the secondary
            // function.
            case SDL_SCANCODE_F14: // L2 — rotate view CCW, or zoom out with R1
                if (down)
                {
                    if (_vGamepadMod)
                        Windows::MainWindowZoom(false, false);
                    else
                        ViewportRotateAll(-1);
                }
                return true;
            case SDL_SCANCODE_F15: // R2 — rotate view CW, or zoom in with R1
                if (down)
                {
                    if (_vGamepadMod)
                        Windows::MainWindowZoom(true, false);
                    else
                        ViewportRotateAll(1);
                }
                return true;
            case SDL_SCANCODE_F16: // face X — cycle game speed (Normal -> Quick
                                   // -> Fast -> Turbo -> Pause -> Normal)
                if (down && gLegacyScene != LegacyScene::titleSequence
                    && Network::GetMode() == Network::Mode::none)
                {
                    if (GameIsPaused())
                    {
                        PauseToggle();
                        if (gGameSpeed != 1)
                            GameResetSpeed();
                    }
                    else if (gGameSpeed >= 4)
                    {
                        PauseToggle();
                    }
                    else
                    {
                        GameIncreaseGameSpeed();
                    }
                }
                return true;
            case SDL_SCANCODE_F17: // face Y — rotate construction object CW
                                   // (3x = anti-clockwise when R1 held)
                if (down)
                {
                    int turns = _vGamepadMod ? 3 : 1;
                    for (int i = 0; i < turns; ++i)
                        ::ShortcutRotateConstructionObject();
                }
                return true;
            default: return false;
        }
    }
#endif

    InGameConsole _inGameConsole;
    std::unique_ptr<ITitleSequencePlayer> _titleSequencePlayer;

public:
    InGameConsole& GetInGameConsole()
    {
        return _inGameConsole;
    }

    InputManager& GetInputManager()
    {
        return _inputManager;
    }

    ShortcutManager& GetShortcutManager()
    {
        return _shortcutManager;
    }

    explicit UiContext(IPlatformEnvironment& env)
        : _platformUiContext(CreatePlatformUiContext())
        , _windowManager(CreateWindowManager())
        , _shortcutManager(env)
    {
        LogSDLVersion();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0)
        {
            SDLException::Throw("SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)");
        }
        _cursorRepository.LoadCursors();
        _shortcutManager.loadUserBindings();
    }

    ~UiContext() override
    {
        UiContext::CloseWindow();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void InitialiseScriptExtensions() override
    {
#ifdef ENABLE_SCRIPTING
        auto& scriptEngine = GetContext()->GetScriptEngine();
        Scripting::UiScriptExtensions::Extend(scriptEngine);
#endif
    }

    void Tick() override
    {
        _inGameConsole.Update();

        _windowManager->UpdateMapTooltip();

        WindowDispatchUpdateAll();
    }

    void Draw(RenderTarget& rt) override
    {
        auto bgColour = ThemeGetColour(WindowClass::chat, 0);
        ChatDraw(rt, bgColour);
        _inGameConsole.Draw(rt);
    }

    // Window
    void* GetWindow() override
    {
        return _window;
    }

    int32_t GetWidth() override
    {
        return _width;
    }

    int32_t GetHeight() override
    {
        return _height;
    }

    ScaleQuality GetScaleQuality() override
    {
        return _scaleQuality;
    }

    void SetFullscreenMode(FullscreenMode mode) override
    {
#ifndef __EMSCRIPTEN__
        static constexpr int32_t kSDLFullscreenFlags[] = {
            0,
            SDL_WINDOW_FULLSCREEN,
            SDL_WINDOW_FULLSCREEN_DESKTOP,
        };
        uint32_t windowFlags = kSDLFullscreenFlags[EnumValue(mode)];

        // HACK Changing window size when in fullscreen usually has no effect
        if (mode == FullscreenMode::fullscreen)
        {
            SDL_SetWindowFullscreen(_window, 0);

            // Set window size
            UpdateFullscreenResolutions();
            Resolution resolution = GetClosestResolution(
                Config::Get().general.fullscreenWidth, Config::Get().general.fullscreenHeight);
            SDL_SetWindowSize(_window, resolution.Width, resolution.Height);
        }
        else if (mode == FullscreenMode::windowed)
        {
            SDL_SetWindowSize(_window, Config::Get().general.windowWidth, Config::Get().general.windowHeight);
        }

        if (SDL_SetWindowFullscreen(_window, windowFlags))
        {
            LOG_FATAL("SDL_SetWindowFullscreen %s", SDL_GetError());
            exit(1);

            // TODO try another display mode rather than just exiting the game
        }
#else
        if (mode == FullscreenMode::fullscreen)
        {
            emscripten_request_fullscreen("!canvas", false);
        }
        else if (mode == FullscreenMode::windowed)
        {
            emscripten_exit_fullscreen();
        }
#endif // __EMSCRIPTEN__
    }

    const std::vector<Resolution>& GetFullscreenResolutions() override
    {
        UpdateFullscreenResolutions();
        return _fsResolutions;
    }

    bool HasFocus() override
    {
        uint32_t windowFlags = GetWindowFlags();
        return (windowFlags & SDL_WINDOW_INPUT_FOCUS) != 0;
    }

    bool IsMinimised() override
    {
        uint32_t windowFlags = GetWindowFlags();
        return (windowFlags & SDL_WINDOW_MINIMIZED) || (windowFlags & SDL_WINDOW_HIDDEN);
    }

    bool IsSteamOverlayActive() override
    {
        return _steamOverlayActive;
    }

    // Input
    const CursorState* GetCursorState() override
    {
        return &_cursorState;
    }

    const uint8_t* GetKeysState() override
    {
        return _keysState;
    }

    const uint8_t* GetKeysPressed() override
    {
        return _keysPressed;
    }

    CursorID GetCursor() override
    {
        return _cursorRepository.GetCurrentCursor();
    }

    void SetCursor(CursorID cursor) override
    {
        _cursorRepository.SetCurrentCursor(cursor);
    }

    void SetCursorScale(uint8_t scale) override
    {
        _cursorRepository.SetCursorScale(scale);
    }

    void SetCursorVisible(bool value) override
    {
        SDL_ShowCursor(value ? SDL_ENABLE : SDL_DISABLE);
    }

    ScreenCoordsXY GetCursorPosition() override
    {
        // OPENRCT2MINI cut 48: return the virtual-cursor position (kept in
        // _cursorState.position by both SDL_MOUSEMOTION and our D-pad /
        // arrow-key handler) instead of the OS mouse. Callers like
        // InputViewportDragContinue() use this to compute drag deltas;
        // querying SDL_GetMouseState would return the unmoving real-mouse
        // position even though the virtual cursor is being driven by
        // keyboard / D-pad — so camera pan via right-mouse + arrows
        // would always see a zero delta and never pan.
        //
        // _cursorState.position is in scaled (game-canvas) pixels;
        // GetCursorPosition contract is window pixels, so multiply by
        // windowScale.
        const auto scale = Config::Get().general.windowScale;
        return ScreenCoordsXY{
            static_cast<int32_t>(_cursorState.position.x * scale),
            static_cast<int32_t>(_cursorState.position.y * scale),
        };
    }

    void SetCursorPosition(const ScreenCoordsXY& cursorPosition) override
    {
        SDL_WarpMouseInWindow(nullptr, cursorPosition.x, cursorPosition.y);
    }

    void SetCursorTrap(bool value) override
    {
        SDL_SetWindowGrab(_window, value ? SDL_TRUE : SDL_FALSE);
    }

    void SetKeysPressed(uint32_t keysym, uint8_t scancode) override
    {
        _lastKeyPressed = keysym;
        _keysPressed[scancode] = 1;
    }

    // Drawing
    std::shared_ptr<IDrawingEngineFactory> GetDrawingEngineFactory() override
    {
        return std::make_shared<DrawingEngineFactory>();
    }

    void DrawWeatherAnimation(IWeatherDrawer* weatherDrawer, RenderTarget& rt, DrawWeatherFunc drawFunc) override
    {
        int32_t left = rt.x;
        int32_t right = left + rt.width;
        int32_t top = rt.y;
        int32_t bottom = top + rt.height;

        for (auto& w : gWindowList)
        {
            DrawWeatherWindow(rt, weatherDrawer, w.get(), left, right, top, bottom, drawFunc);
        }
    }

    // Text input
    bool IsTextInputActive() override
    {
        return _textComposition.IsActive();
    }

    TextInputSession* StartTextInput(u8string& buffer, size_t maxLength) override
    {
        return _textComposition.Start(buffer, maxLength);
    }

    void StopTextInput() override
    {
        _textComposition.Stop();
    }

    void ProcessMessages() override
    {
        _lastKeyPressed = 0;
        _cursorState.left &= ~CURSOR_CHANGED;
        _cursorState.middle &= ~CURSOR_CHANGED;
        _cursorState.right &= ~CURSOR_CHANGED;
        _cursorState.old = 0;

        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
                case SDL_QUIT:
                    ContextQuit();
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_RESIZED)
                    {
                        LOG_VERBOSE("New Window size: %ux%u\n", e.window.data1, e.window.data2);
                        OnResize(e.window.data1, e.window.data2);
                    }

                    switch (e.window.event)
                    {
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_MOVED:
                        case SDL_WINDOWEVENT_MAXIMIZED:
                        case SDL_WINDOWEVENT_RESTORED:
                        {
                            // Update default display index
                            int32_t displayIndex = SDL_GetWindowDisplayIndex(_window);
                            if (displayIndex != Config::Get().general.defaultDisplay)
                            {
                                Config::Get().general.defaultDisplay = displayIndex;
                                Config::Save();
                            }
                            break;
                        }
                    }

                    if (Config::Get().sound.audioFocus)
                    {
                        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                        {
                            SetAudioVolume(1);
                        }
                        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        {
                            SetAudioVolume(0);
                        }
                    }
                    break;
                case SDL_MOUSEMOTION:
                    _cursorState.position = { static_cast<int32_t>(e.motion.x / Config::Get().general.windowScale),
                                              static_cast<int32_t>(e.motion.y / Config::Get().general.windowScale) };
                    break;
                case SDL_MOUSEWHEEL:
                    if (_inGameConsole.IsOpen())
                    {
                        _inGameConsole.Scroll(e.wheel.y * 3); // Scroll 3 lines at a time
                        break;
                    }
                    _cursorState.wheel -= e.wheel.y;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                {
                    if (e.button.which == SDL_TOUCH_MOUSEID)
                    {
                        break;
                    }
                    ScreenCoordsXY mousePos = { static_cast<int32_t>(e.button.x / Config::Get().general.windowScale),
                                                static_cast<int32_t>(e.button.y / Config::Get().general.windowScale) };
                    switch (e.button.button)
                    {
                        case SDL_BUTTON_LEFT:
                            StoreMouseInput(MouseState::leftPress, mousePos);
                            _cursorState.left = CURSOR_PRESSED;
                            _cursorState.old = 1;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            _cursorState.middle = CURSOR_PRESSED;
                            break;
                        case SDL_BUTTON_RIGHT:
                            StoreMouseInput(MouseState::rightPress, mousePos);
                            _cursorState.right = CURSOR_PRESSED;
                            _cursorState.old = 2;
                            break;
                    }
                    _cursorState.touch = false;

                    {
                        InputEvent ie;
                        ie.deviceKind = InputDeviceKind::mouse;
                        ie.modifiers = SDL_GetModState();
                        ie.button = e.button.button;
                        ie.state = InputEventState::down;
                        _inputManager.queueInputEvent(std::move(ie));
                    }
                    break;
                }
                case SDL_MOUSEBUTTONUP:
                {
                    if (e.button.which == SDL_TOUCH_MOUSEID)
                    {
                        break;
                    }
                    ScreenCoordsXY mousePos = { static_cast<int32_t>(e.button.x / Config::Get().general.windowScale),
                                                static_cast<int32_t>(e.button.y / Config::Get().general.windowScale) };
                    switch (e.button.button)
                    {
                        case SDL_BUTTON_LEFT:
                            StoreMouseInput(MouseState::leftRelease, mousePos);
                            _cursorState.left = CURSOR_RELEASED;
                            _cursorState.old = 3;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            _cursorState.middle = CURSOR_RELEASED;
                            break;
                        case SDL_BUTTON_RIGHT:
                            StoreMouseInput(MouseState::rightRelease, mousePos);
                            _cursorState.right = CURSOR_RELEASED;
                            _cursorState.old = 4;
                            break;
                    }
                    _cursorState.touch = false;

                    {
                        InputEvent ie;
                        ie.deviceKind = InputDeviceKind::mouse;
                        ie.modifiers = SDL_GetModState();
                        ie.button = e.button.button;
                        ie.state = InputEventState::release;
                        _inputManager.queueInputEvent(std::move(ie));
                    }
                    break;
                }
                // Apple sends touchscreen events for trackpads, so ignore these events on macOS
#ifndef __MACOSX__
                case SDL_FINGERMOTION:
                    _cursorState.position = { static_cast<int32_t>(e.tfinger.x * _width),
                                              static_cast<int32_t>(e.tfinger.y * _height) };
                    break;
                case SDL_FINGERDOWN:
                {
                    ScreenCoordsXY fingerPos = { static_cast<int32_t>(e.tfinger.x * _width),
                                                 static_cast<int32_t>(e.tfinger.y * _height) };

                    _cursorState.touchIsDouble
                        = (!_cursorState.touchIsDouble
                           && e.tfinger.timestamp - _cursorState.touchDownTimestamp < kTouchDoubleTimeout);

                    if (_cursorState.touchIsDouble)
                    {
                        StoreMouseInput(MouseState::rightPress, fingerPos);
                        _cursorState.right = CURSOR_PRESSED;
                        _cursorState.old = 2;
                    }
                    else
                    {
                        StoreMouseInput(MouseState::leftPress, fingerPos);
                        _cursorState.left = CURSOR_PRESSED;
                        _cursorState.old = 1;
                    }
                    _cursorState.touch = true;
                    _cursorState.touchDownTimestamp = e.tfinger.timestamp;
                    break;
                }
                case SDL_FINGERUP:
                {
                    ScreenCoordsXY fingerPos = { static_cast<int32_t>(e.tfinger.x * _width),
                                                 static_cast<int32_t>(e.tfinger.y * _height) };

                    if (_cursorState.touchIsDouble)
                    {
                        StoreMouseInput(MouseState::rightRelease, fingerPos);
                        _cursorState.right = CURSOR_RELEASED;
                        _cursorState.old = 4;
                    }
                    else
                    {
                        StoreMouseInput(MouseState::leftRelease, fingerPos);
                        _cursorState.left = CURSOR_RELEASED;
                        _cursorState.old = 3;
                    }
                    _cursorState.touch = true;
                    break;
                }
#endif
                case SDL_KEYDOWN:
                {
#ifndef __MACOSX__
                    // Ignore winkey keydowns. Handles edge case where tiling
                    // window managers don't eat the keypresses when changing
                    // workspaces.
                    if (SDL_GetModState() & KMOD_GUI)
                    {
                        break;
                    }
#endif
#ifdef ENABLE_SOFTWARE_CURSOR
                    // OPENRCT2MINI: cut 38b. Swallow our virtual-cursor keys
                    // before the keyboard pipeline sees them — otherwise
                    // arrows pan the camera and Z/X trigger menu shortcuts.
                    if (InterceptVirtualCursorKey(e.key.keysym.scancode, true))
                        break;
#endif
                    _textComposition.HandleMessage(&e);
                    auto ie = GetInputEventFromSDLEvent(e);
                    ie.state = InputEventState::down;
                    _inputManager.queueInputEvent(std::move(ie));
                    break;
                }
                case SDL_KEYUP:
                {
#ifdef ENABLE_SOFTWARE_CURSOR
                    if (InterceptVirtualCursorKey(e.key.keysym.scancode, false))
                        break;
#endif
                    auto ie = GetInputEventFromSDLEvent(e);
                    ie.state = InputEventState::release;
                    _inputManager.queueInputEvent(std::move(ie));
                    break;
                }
                case SDL_MULTIGESTURE:
                    if (e.mgesture.numFingers == 2)
                    {
                        if (e.mgesture.timestamp > _lastGestureTimestamp + 1000)
                        {
                            _gestureRadius = 0;
                        }
                        _lastGestureTimestamp = e.mgesture.timestamp;
                        _gestureRadius += e.mgesture.dDist;

                        // Zoom gesture
                        constexpr int32_t tolerance = 128;
                        int32_t gesturePixels = static_cast<int32_t>(_gestureRadius * _width);
                        if (abs(gesturePixels) > tolerance)
                        {
                            _gestureRadius = 0;
                            Windows::MainWindowZoom(gesturePixels > 0, true);
                        }
                    }
                    break;
                case SDL_TEXTEDITING:
                    _textComposition.HandleMessage(&e);
                    break;
                case SDL_TEXTINPUT:
                    _textComposition.HandleMessage(&e);
                    break;
                default:
                {
                    _inputManager.queueInputEvent(e);
                    break;
                }
            }
        }

#ifdef ENABLE_SOFTWARE_CURSOR
        // OPENRCT2MINI: cut 38b. Update virtual cursor from latched key/pad
        // state AFTER the event loop, so we see this frame's KEYDOWNs.
        ProcessVirtualGamepadCursor();
#endif

        _cursorState.any = _cursorState.left | _cursorState.middle | _cursorState.right;

        // Updates the state of the keys
        int32_t numKeys = 256;
        _keysState = SDL_GetKeyboardState(&numKeys);
    }

    /**
     * Helper function to set various render target features.
     * Does not get triggered on resize, but rather manually on config changes.
     */
    void TriggerResize() override
    {
        char scaleQualityBuffer[4];
        _scaleQuality = ScaleQuality::SmoothNearestNeighbour;
        if (Config::Get().general.windowScale == std::floor(Config::Get().general.windowScale))
        {
            _scaleQuality = ScaleQuality::NearestNeighbour;
        }

        ScaleQuality scaleQuality = _scaleQuality;
        if (_scaleQuality == ScaleQuality::SmoothNearestNeighbour)
        {
            scaleQuality = ScaleQuality::Linear;
        }
        snprintf(scaleQualityBuffer, sizeof(scaleQualityBuffer), "%d", static_cast<int32_t>(scaleQuality));
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaleQualityBuffer);

        int32_t width, height;
        SDL_GetWindowSize(_window, &width, &height);
        OnResize(width, height);
    }

    void CreateWindow() override
    {
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Config::Get().general.minimizeFullscreenFocusLoss ? "1" : "0");

        // Set window position to default display
        int32_t defaultDisplay = std::clamp(Config::Get().general.defaultDisplay, 0, 0xFFFF);
        auto windowPos = ScreenCoordsXY{ static_cast<int32_t>(SDL_WINDOWPOS_UNDEFINED_DISPLAY(defaultDisplay)),
                                         static_cast<int32_t>(SDL_WINDOWPOS_UNDEFINED_DISPLAY(defaultDisplay)) };

        CreateWindow(windowPos);

        // Check if steam overlay renderer is loaded into the process
        _steamOverlayActive = _platformUiContext->IsSteamOverlayAttached();
    }

    void CloseWindow() override
    {
        DrawingEngineDispose();
        if (_window != nullptr)
        {
            SDL_DestroyWindow(_window);
            _window = nullptr;
        }
    }

    void RecreateWindow() override
    {
        // Use the position of the current window for the new window
        ScreenCoordsXY windowPos;
        SDL_SetWindowFullscreen(_window, 0);
        SDL_GetWindowPosition(_window, &windowPos.x, &windowPos.y);

        CloseWindow();
        CreateWindow(windowPos);
    }

    void ShowMessageBox(const std::string& message) override
    {
        _platformUiContext->ShowMessageBox(_window, message);
    }

    int32_t ShowMessageBox(
        const std::string& title, const std::string& message, const std::vector<std::string>& options) override
    {
        auto message_box_button_data = std::make_unique<SDL_MessageBoxButtonData[]>(options.size());
        for (size_t i = 0; i < options.size(); i++)
        {
            message_box_button_data[i].buttonid = static_cast<int>(i);
            message_box_button_data[i].text = options[i].c_str();
        }

        SDL_MessageBoxData message_box_data{};
        message_box_data.window = _window;
        message_box_data.title = title.c_str();
        message_box_data.message = message.c_str();
        message_box_data.numbuttons = static_cast<int>(options.size());
        message_box_data.buttons = message_box_button_data.get();

        int buttonid{};

        SDL_ShowMessageBox(&message_box_data, &buttonid);

        return buttonid;
    }

    bool HasMenuSupport() override
    {
        return _platformUiContext->HasMenuSupport();
    }

    int32_t ShowMenuDialog(const std::vector<std::string>& options, const std::string& title, const std::string& text) override
    {
        return _platformUiContext->ShowMenuDialog(options, title, text);
    }

    void OpenFolder(const std::string& path) override
    {
        _platformUiContext->OpenFolder(path);
    }

    void OpenURL(const std::string& url) override
    {
        _platformUiContext->OpenURL(url);
    }

    std::string ShowFileDialog(const FileDialogDesc& desc) override
    {
        return _platformUiContext->ShowFileDialog(_window, desc);
    }

    std::string ShowDirectoryDialog(const std::string& title) override
    {
        return _platformUiContext->ShowDirectoryDialog(_window, title);
    }

    bool HasFilePicker() const override
    {
        return _platformUiContext->HasFilePicker();
    }

    IWindowManager* GetWindowManager() override
    {
        return _windowManager.get();
    }

    bool SetClipboardText(const utf8* target) override
    {
#ifndef __EMSCRIPTEN__
        return (SDL_SetClipboardText(target) == 0);
#else
        return (
            MAIN_THREAD_EM_ASM_INT(
                {
                    try
                    {
                        navigator.clipboard.writeText(UTF8ToString($0));
                        return 0;
                    }
                    catch (e)
                    {
                        return -1;
                    };
                },
                target)
            == 0);
#endif
    }

    ITitleSequencePlayer* GetTitleSequencePlayer() override
    {
        if (_titleSequencePlayer == nullptr)
        {
            _titleSequencePlayer = Title::CreateTitleSequencePlayer();
        }
        return _titleSequencePlayer.get();
    }

private:
    void LogSDLVersion()
    {
        SDL_version version{};
        SDL_GetVersion(&version);
        LOG_VERBOSE("SDL2 version: %d.%d.%d", version.major, version.minor, version.patch);
    }

    void InferDisplayDPI()
    {
        auto& config = Config::Get().general;
        if (!config.inferDisplayDPI)
            return;

        int wWidth, wHeight;
        SDL_GetWindowSize(_window, &wWidth, &wHeight);

        auto renderer = SDL_GetRenderer(_window);
        int rWidth, rHeight;
        if (SDL_GetRendererOutputSize(renderer, &rWidth, &rHeight) == 0)
            config.windowScale = rWidth / wWidth;

        config.inferDisplayDPI = false;
        Config::Save();
    }

    void CreateWindow(const ScreenCoordsXY& windowPos)
    {
#ifdef __EMSCRIPTEN__
        MAIN_THREAD_EM_ASM({
            Module.canvas.width = window.innerWidth;
            Module.canvas.height = window.innerHeight;
        });
        int32_t width = 0;
        int32_t height = 0;
        emscripten_get_canvas_element_size("!canvas", &width, &height);
#else
        // Get saved window size
        int32_t width = Config::Get().general.windowWidth;
        int32_t height = Config::Get().general.windowHeight;
#endif

        // OPENRCT2MINI cut 40c: default window is the Miyoo Mini panel size
        // (640×480). Anything bigger is wasted on this device; the mmiyoo
        // render driver also caps texture dimensions at 800×600, so 1280×720
        // would crash SDL_CreateTexture before the title screen rendered.
        // Set defaults if size is invalid
        if (width <= 0)
            width = 640;
        if (height <= 0)
            height = 480;

        // OPENRCT2MINI cut 40d: hard-clamp the window to the panel resolution
        // even if a stale config.ini specifies something larger. A previous
        // crashing run (cut 40b) could have written window_width=1280 /
        // window_height=720; without the clamp the renderer's screen-texture
        // creation would fail again because mmiyoo limits textures to 800×600.
        if (width > 640)
            width = 640;
        if (height > 480)
            height = 480;

        // Create window in window first rather than fullscreen so we have the display the window is on first
        uint32_t flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (Config::Get().general.drawingEngine == DrawingEngine::OpenGL)
        {
            flags |= SDL_WINDOW_OPENGL;
        }

        _window = SDL_CreateWindow(OPENRCT2_NAME, windowPos.x, windowPos.y, width, height, flags);
        if (_window == nullptr)
        {
            const char* error = SDL_GetError();
            std::string errorMessage = String::stdFormat(
                "SDL_CreateWindow(" OPENRCT2_NAME ", %d, %d, %d, %d, %d) failed: %s", windowPos.x, windowPos.y, width, height,
                flags, error);
            SDLException::Throw(errorMessage.c_str());
        }

        ApplyScreenSaverLockSetting();

        // OPENRCT2MINI: cut 29. Miyoo Mini screen is 640×480; upstream's 720×480
        // minimum would force scaling/cropping. Drop to the device's native size.
        // The dev host can still resize larger; the change only affects the floor.
        SDL_SetWindowMinimumSize(_window, 640, 480);
        SetCursorTrap(Config::Get().general.trapCursor);
        _platformUiContext->SetWindowIcon(_window);

        // Initialise the surface, palette and draw buffer
        DrawingEngineInit();
        InferDisplayDPI();
        OnResize(width, height);

        UpdateFullscreenResolutions();

        SetFullscreenMode(static_cast<FullscreenMode>(Config::Get().general.fullscreenMode));
        TriggerResize();
    }

#ifdef ENABLE_SOFTWARE_CURSOR
    // OPENRCT2MINI: cut 38. Virtual-cursor poll. Reads the D-pad and face
    // buttons from any connected SDL game controllers, advances a virtual
    // cursor position with simple hold-acceleration, and pushes synthetic
    // mouse events into the SDL queue. The rest of the input pipeline is
    // unchanged — those events are picked up by the SDL_MOUSEMOTION /
    // SDL_MOUSEBUTTON cases in ProcessMessages just below this call.
    // OPENRCT2MINI: cut 38b. Per-frame virtual cursor poll. Runs AFTER the
    // SDL_PollEvent loop, so latched key state from this frame's KEYDOWN /
    // KEYUP intercepts is current. Writes directly to _cursorState and
    // StoreMouseInput rather than synthesising SDL events — the synthetic
    // events would only be processed on the next frame, and there's no
    // benefit to going through the SDL queue when we own all the targets.
    void ProcessVirtualGamepadCursor()
    {
        // OPENRCT2MINI OSK: cursor is suspended while the on-screen
        // keyboard is up — the D-pad drives OSK selection instead.
        // Clearing the latches here belt-and-braces against arrow events
        // arriving on the same frame the OSK opens (with stale `down`
        // state) and the cursor drifting after the OSK closes.
        if (Windows::OskIsActive())
        {
            _vKbUp = _vKbDown = _vKbLeft = _vKbRight = false;
            _vKbZ = _vKbX = false;
            return;
        }
        const auto& controllers = _inputManager.getGameControllers();

        // Read controller state.
        bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
        bool btnA = false, btnB = false, fastModifier = false;
        for (auto* gc : controllers)
        {
            if (gc == nullptr)
                continue;
            dpadUp |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
            dpadDown |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
            dpadLeft |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
            dpadRight |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
            btnA |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
            btnB |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0;
            // Right-shoulder = "fast cursor" multiplier so users can cross
            // the 640x480 frame quickly without long D-pad holds.
            fastModifier |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
        }
        // OPENRCT2MINI cut 53b: always OR in the keyboard latched state. The
        // earlier gate skipped this when text composition was active so
        // arrow keys could navigate within text fields, but on a device
        // with no keyboard there's no way to type anyway, and on host the
        // user has plenty of other keys for text edit. Cursor mobility
        // beats within-field caret movement.
        dpadUp |= _vKbUp;
        dpadDown |= _vKbDown;
        dpadLeft |= _vKbLeft;
        dpadRight |= _vKbRight;
        btnA |= _vKbZ;
        btnB |= _vKbX;
        fastModifier |= _vKbShift;
        // OPENRCT2MINI cut 60: when Ctrl is held (L1+R1 chord on the device,
        // or real keyboard Ctrl) suppress fast cursor. Ctrl is the
        // construction Z-lock modifier; the user is being precise and fast
        // cursor defeats the purpose.
        if (_vKbCtrl)
            fastModifier = false;

        if (!_vcursorInitialised)
        {
            _vcursorX = static_cast<float>(std::max(_width, 1) / 2);
            _vcursorY = static_cast<float>(std::max(_height, 1) / 2);
            _vcursorInitialised = true;
        }

        // Sync from real mouse motion that happened during this frame's
        // SDL_PollEvent loop — without this, moving the mouse would shift
        // _cursorState.position but our virtual cursor would keep firing
        // motion from its old position the next frame. _cursorState.position
        // is in scaled (game-canvas) pixels.
        //
        // OPENRCT2MINI cut 47: only resync when the int32 position differs
        // from what WE wrote last frame. Otherwise we round-trip the float
        // through a truncated int32 every frame and lose any sub-pixel
        // fractional movement — with cut 44c's framerate-scaled speed
        // (0.75 px/frame at 60 Hz) the float never crosses the next
        // integer in the positive direction, so right/down get stuck.
        const float scale = static_cast<float>(Config::Get().general.windowScale);
        if (_cursorState.position.x != _vcursorLastIntX
            || _cursorState.position.y != _vcursorLastIntY)
        {
            _vcursorX = static_cast<float>(_cursorState.position.x) * scale;
            _vcursorY = static_cast<float>(_cursorState.position.y) * scale;
        }

        const bool anyDir = dpadUp || dpadDown || dpadLeft || dpadRight;
        const uint32_t now = SDL_GetTicks();
        if (anyDir && _vdpadHeldSinceMs == 0)
            _vdpadHeldSinceMs = now;
        else if (!anyDir)
            _vdpadHeldSinceMs = 0;

        // OPENRCT2MINI cut 44c: framerate-independent cursor speed. The
        // earlier cut measured speed in pixels-per-frame, which made the
        // cursor lurch with FPS hitches and tied movement rate to a fixed
        // 60 Hz assumption. Convert to pixels-per-second and multiply by
        // a clamped dt. Clamp to 100 ms so that a 1-second hitch doesn't
        // teleport the cursor across the screen on the recovering frame.
        uint32_t dtMs = (_vcursorLastTickMs == 0) ? 16 : (now - _vcursorLastTickMs);
        if (dtMs > 100)
            dtMs = 100;
        _vcursorLastTickMs = now;
        const float dtSec = static_cast<float>(dtMs) / 1000.0f;

        if (anyDir)
        {
            // OPENRCT2MINI cut 58: cursor speed and acceleration are now
            // exposed through the Options > Controls > Cursor section. The
            // two pre-existing gamepad scrollbars (Deadzone, Sensitivity)
            // were repurposed in-place — we kept the underlying config
            // keys (gamepadDeadzone, gamepadSensitivity) so existing
            // configs keep working without a migration; only the labels
            // (en-GB.txt 6785-6791) and this consumer were rewired.
            //
            //   gamepadDeadzone     (0..32767, displayed 0..100%) drives
            //                       acceleration: 0% = slow ramp (800 ms
            //                       hold to reach max speed), 100% = the
            //                       max speed kicks in immediately.
            //   gamepadSensitivity  (0.5..3.0, displayed 50..300%) is a
            //                       linear multiplier on the cursor speed.
            //                       1.0 = original cut-44c rate (45 → 150
            //                       px/sec).
            //
            // Fast modifier (R1) still applies its 2.5× boost on top.
            const uint32_t held = now - _vdpadHeldSinceMs;
            const float accel = std::clamp(
                static_cast<float>(Config::Get().general.gamepadDeadzone) / 32767.0f, 0.0f, 1.0f);
            const float rampMs = 800.0f * (1.0f - accel) + 50.0f * accel;
            const float ramp = std::min(1.0f, static_cast<float>(held) / rampMs);
            const float speedMult = std::clamp(Config::Get().general.gamepadSensitivity, 0.1f, 5.0f);
            float speedPxPerSec = (45.0f + 105.0f * ramp) * speedMult;
            if (fastModifier)
                speedPxPerSec *= 2.5f;
            const float speed = speedPxPerSec * dtSec;

            float dx = 0.0f, dy = 0.0f;
            if (dpadLeft)
                dx -= speed;
            if (dpadRight)
                dx += speed;
            if (dpadUp)
                dy -= speed;
            if (dpadDown)
                dy += speed;
            if (dx != 0.0f && dy != 0.0f)
            {
                dx *= 0.7071f;
                dy *= 0.7071f;
            }

            const float maxX = static_cast<float>(std::max(_width, 1) * scale - 1);
            const float maxY = static_cast<float>(std::max(_height, 1) * scale - 1);
            _vcursorX = std::clamp(_vcursorX + dx, 0.0f, maxX);
            _vcursorY = std::clamp(_vcursorY + dy, 0.0f, maxY);

            // Update _cursorState directly (mirrors SDL_MOUSEMOTION handler).
            // OPENRCT2MINI cut 47: round instead of truncate-toward-zero, and
            // remember the int we wrote so the resync-from-real-mouse logic
            // above can tell our own write apart from an SDL_MOUSEMOTION
            // override.
            const int32_t newX = static_cast<int32_t>(std::round(_vcursorX / scale));
            const int32_t newY = static_cast<int32_t>(std::round(_vcursorY / scale));
            _cursorState.position = { newX, newY };
            _vcursorLastIntX = newX;
            _vcursorLastIntY = newY;
        }

        // A/B (and Z/X) → left/right mouse buttons. Only emit on transition.
        auto handleButton = [&](bool now_pressed, bool& prev_pressed,
                                MouseState pressEvent, MouseState releaseEvent,
                                uint8_t& cursorBtn, uint8_t cursorOldDown,
                                uint8_t cursorOldUp) {
            if (now_pressed == prev_pressed)
                return;
            ScreenCoordsXY pos = {
                static_cast<int32_t>(_vcursorX / scale),
                static_cast<int32_t>(_vcursorY / scale),
            };
            if (now_pressed)
            {
                StoreMouseInput(pressEvent, pos);
                cursorBtn = CURSOR_PRESSED;
                _cursorState.old = cursorOldDown;
            }
            else
            {
                StoreMouseInput(releaseEvent, pos);
                cursorBtn = CURSOR_RELEASED;
                _cursorState.old = cursorOldUp;
            }
            _cursorState.touch = false;
            prev_pressed = now_pressed;
        };
        handleButton(
            btnA, _vprevA, MouseState::leftPress, MouseState::leftRelease,
            _cursorState.left, /*down=*/1, /*up=*/3);
        handleButton(
            btnB, _vprevB, MouseState::rightPress, MouseState::rightRelease,
            _cursorState.right, /*down=*/2, /*up=*/4);
    }
#endif

    void OnResize(int32_t width, int32_t height)
    {
        // Scale the native window size to the game's canvas size
        _width = static_cast<int32_t>(width / Config::Get().general.windowScale);
        _height = static_cast<int32_t>(height / Config::Get().general.windowScale);

        DrawingEngineResize();

        uint32_t flags = SDL_GetWindowFlags(_window);
        if ((flags & SDL_WINDOW_MINIMIZED) == 0)
        {
            WindowResizeGui(_width, _height);
            Windows::WindowRelocateWindows(_width, _height);
        }

        GfxInvalidateScreen();

        // Check if the window has been resized in windowed mode and update the config file accordingly
        int32_t nonWindowFlags =
#ifndef __MACOSX__
            SDL_WINDOW_MAXIMIZED |
#endif
            SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP;

        if (!(flags & nonWindowFlags))
        {
            if (width != Config::Get().general.windowWidth || height != Config::Get().general.windowHeight)
            {
                Config::Get().general.windowWidth = width;
                Config::Get().general.windowHeight = height;
                Config::Save();
            }
        }
    }

    void UpdateFullscreenResolutions()
    {
        // Query number of display modes
        int32_t displayIndex = SDL_GetWindowDisplayIndex(_window);
        int32_t numDisplayModes = SDL_GetNumDisplayModes(displayIndex);

        // Get desktop aspect ratio
        SDL_DisplayMode mode;
        SDL_GetDesktopDisplayMode(displayIndex, &mode);

        // Get resolutions
        auto resolutions = std::vector<Resolution>();
        float desktopAspectRatio = static_cast<float>(mode.w) / mode.h;
        for (int32_t i = 0; i < numDisplayModes; i++)
        {
            SDL_GetDisplayMode(displayIndex, i, &mode);
            if (mode.w > 0 && mode.h > 0)
            {
                float aspectRatio = static_cast<float>(mode.w) / mode.h;
                if (std::fabs(desktopAspectRatio - aspectRatio) < 0.1f)
                {
                    resolutions.push_back({ mode.w, mode.h });
                }
            }
        }

        // Sort by area
        std::sort(resolutions.begin(), resolutions.end(), [](const Resolution& a, const Resolution& b) -> bool {
            int32_t areaA = a.Width * a.Height;
            int32_t areaB = b.Width * b.Height;
            return areaA < areaB;
        });

        // Remove duplicates
        auto last = std::unique(resolutions.begin(), resolutions.end(), [](const Resolution& a, const Resolution& b) -> bool {
            return (a.Width == b.Width && a.Height == b.Height);
        });
        resolutions.erase(last, resolutions.end());

        // Update config fullscreen resolution if not set
        if (!resolutions.empty()
            && (Config::Get().general.fullscreenWidth == -1 || Config::Get().general.fullscreenHeight == -1))
        {
            Config::Get().general.fullscreenWidth = resolutions.back().Width;
            Config::Get().general.fullscreenHeight = resolutions.back().Height;
        }

        _fsResolutions = resolutions;
    }

    Resolution GetClosestResolution(int32_t inWidth, int32_t inHeight)
    {
        Resolution result = { 640, 480 };
        int32_t closestAreaDiff = -1;
        int32_t destinationArea = inWidth * inHeight;
        for (const Resolution& resolution : _fsResolutions)
        {
            // Check if exact match
            if (resolution.Width == inWidth && resolution.Height == inHeight)
            {
                result = resolution;
                break;
            }

            // Check if area is closer to best match
            int32_t areaDiff = std::abs((resolution.Width * resolution.Height) - destinationArea);
            if (closestAreaDiff == -1 || areaDiff < closestAreaDiff)
            {
                closestAreaDiff = areaDiff;
                result = resolution;
            }
        }
        return result;
    }

    uint32_t GetWindowFlags()
    {
        return SDL_GetWindowFlags(_window);
    }

    static void DrawWeatherWindow(
        RenderTarget& rt, IWeatherDrawer* weatherDrawer, WindowBase* original_w, int16_t left, int16_t right, int16_t top,
        int16_t bottom, DrawWeatherFunc drawFunc)
    {
        WindowBase* w{};
        auto itStart = WindowGetIterator(original_w);
        for (auto it = std::next(itStart);; it++)
        {
            if (it == gWindowList.end())
            {
                // Loop ended, draw weather for original_w
                auto vp = original_w->viewport;
                if (vp != nullptr)
                {
                    left = std::max<int16_t>(left, vp->pos.x);
                    right = std::min<int16_t>(right, vp->pos.x + vp->width);
                    top = std::max<int16_t>(top, vp->pos.y);
                    bottom = std::min<int16_t>(bottom, vp->pos.y + vp->height);
                    if (left < right && top < bottom)
                    {
                        auto width = right - left;
                        auto height = bottom - top;
                        drawFunc(rt, weatherDrawer, left, top, width, height);
                    }
                }
                return;
            }

            w = it->get();

            if (w->flags.has(WindowFlag::dead))
            {
                continue;
            }

            if (right <= w->windowPos.x || bottom <= w->windowPos.y)
            {
                continue;
            }

            if (w->right() <= left || w->bottom() <= top)
            {
                continue;
            }

            if (left >= w->windowPos.x)
            {
                break;
            }

            DrawWeatherWindow(rt, weatherDrawer, original_w, left, w->windowPos.x, top, bottom, drawFunc);

            left = w->windowPos.x;
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, bottom, drawFunc);
            return;
        }

        auto wRight = w->right();
        if (right > wRight)
        {
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, wRight, top, bottom, drawFunc);

            left = wRight;
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, bottom, drawFunc);
            return;
        }

        if (top < w->windowPos.y)
        {
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, w->windowPos.y, drawFunc);

            top = w->windowPos.y;
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, bottom, drawFunc);
            return;
        }

        auto wBottom = w->bottom();
        if (bottom > wBottom)
        {
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, wBottom, drawFunc);

            top = wBottom;
            DrawWeatherWindow(rt, weatherDrawer, original_w, left, right, top, bottom, drawFunc);
            return;
        }
    }

    InputEvent GetInputEventFromSDLEvent(const SDL_Event& e)
    {
        InputEvent ie;
        ie.deviceKind = InputDeviceKind::keyboard;
        ie.modifiers = e.key.keysym.mod;
        ie.button = e.key.keysym.sym;

        // Handle dead keys
        if (ie.button == (SDLK_SCANCODE_MASK | 0))
        {
            switch (e.key.keysym.scancode)
            {
                case SDL_SCANCODE_APOSTROPHE:
                    ie.button = '\'';
                    break;
                case SDL_SCANCODE_GRAVE:
                    ie.button = '`';
                    break;
                default:
                    break;
            }
        }

        return ie;
    }

    void SetAudioVolume(float value)
    {
        auto& audioContext = GetContext()->GetAudioContext();
        auto* mixer = audioContext.GetMixer();
        if (mixer != nullptr)
        {
            mixer->SetVolume(value);
        }
    }
};

std::unique_ptr<IUiContext> Ui::CreateUiContext(IPlatformEnvironment& env)
{
    return std::make_unique<UiContext>(env);
}

InGameConsole& Ui::GetInGameConsole()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    return uiContext.GetInGameConsole();
}

InputManager& Ui::GetInputManager()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    return uiContext.GetInputManager();
}

ShortcutManager& Ui::GetShortcutManager()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    return uiContext.GetShortcutManager();
}
