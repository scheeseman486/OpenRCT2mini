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
#include "input/InputContextStrategy.h"
#include "input/ShortcutIds.h"
#include "input/ShortcutManager.h"
#include "interface/Dropdown.h"
#include "interface/InGameConsole.h"
#include "interface/Theme.h"
#include "interface/Viewport.h"
#include "interface/ViewportInteraction.h"
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
#include <openrct2/profiling/Bench.h>
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
#include <openrct2/world/MapSelection.h>
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

// OPENRCT2MINI text-editing-de-hardcode: KB_PRIMARY_MODIFIER macro
// removed. It was unused dead code left over from an earlier refactor
// — the live consumer was always TextComposition.cpp, and that file's
// SDL_KEYDOWN switch is now driven by bindable shortcuts rather than
// the macro.

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
    // OPENRCT2MINI gamepad-plan 1.6: legacy _vKbUp/Down/Left/Right/Z/X
    // arrow + click latches deleted. Their consumers in Process-
    // VirtualGamepadCursor migrated to held-state polls against the
    // cursor.* shortcut bindings; their setters in InterceptVirtual-
    // CursorKey were removed. Keyboard arrows / Z / X still drive
    // cursor motion / click / cancel via registerKeyboardDefault
    // entries on the new shortcuts (Shortcuts.cpp).
    //
    // OPENRCT2MINI input-rework: _vGamepadMod removed. Was a latch
    // for the device-specific R1+C close-window chord, which itself
    // existed only because the LALT/RALT intercept used to scrub
    // KMOD_LALT/RALT from SDL's mod state — breaking chord-shortcut
    // matching for ALT+anything, including the registered ALT+C →
    // kInterfaceCloseWindowUnderCursor binding. The scrub and the
    // C-handler chord are both gone; ALT+C now fires through
    // ShortcutManager like any other chord. ALT+RETURN reverts to
    // SDL's default fullscreen toggle (no-op on the device's
    // libmi_gfx panel; standard behaviour on host).
    // OPENRCT2MINI gamepad-plan 1.6: _vKbCtrl deleted — every consumer
    // migrated to InputManager::handleModifiers()'s synthetic
    // ModifierKey::ctrl bit (via real KMOD_CTRL + kInterfaceConstruction-
    // ZLock binding's held-state). The SDL_SCANCODE_LCTRL/RCTRL handler
    // no longer sets it (still returns false to propagate to SDL mod
    // state).
    //
    // OPENRCT2MINI gamepad-plan 1.6: _vKbCPressed / _vKbCHoldFired /
    // _vKbCPressedAtMs / _vKbCSuppressShade deleted. The shade tap-
    // OPENRCT2MINI hold-binding refactor: tap-vs-hold detection for
    // shade-window / shade-all moved into ShortcutManager's
    // _holdPending mechanism. The legacy edge-tracking state
    // (_vShadePrev, _vShadePressedAtMs, _vShadeHoldFired,
    // _vShadeSuppress) is gone — the dispatcher handles deferring
    // the tap on press, firing the hold on threshold-elapsed, and
    // firing the tap on release-before-threshold. Same 500 ms hold
    // window (now expressed via the "HOLD " prefix on the
    // kInterfaceToggleShadeAllWindows binding).

    // OPENRCT2MINI cursor-cancel-drag split: edge-tracking for the
    // kInterfaceCameraDrag held-state poll in ProcessWorldCursor.
    // Rising edge → InputContextDragBeginAtCursor (dispatches
    // camera-pan / scroll-drag / window-drag by cursor location).
    // Falling edge → InputContextDragEndCurrent + camera-tap
    // right-click fallback. cursor.cancel no longer drives any drag
    // begin — this poll is the sole source for all three gestures.
    bool _vCameraDragPrev = false;
    // OPENRCT2MINI cursor-cancel-tile-action-plan §3.5 (Phase C
    // follow-up 2026-05-17): at the rising edge of kInterfaceCameraDrag,
    // capture whether kCursorCancel was also held by the same press
    // (i.e. the user's physical input is bound to BOTH shortcuts —
    // mouse RMB default, or a gamepad button rebound to overlap).
    // Used at the falling edge: synthesise a kCursorCancel release
    // ONLY when the shared-binding flag was set. Without this gate,
    // a press of a camera.drag-only input (e.g. default PAD X)
    // would short-press-synth a cursor.cancel release on every
    // short tap and fire deletion even though that input isn't
    // bound to cursor.cancel.
    bool _vCameraDragSharedWithCancel = false;

    // OPENRCT2MINI: per-frame edge-detection state for routing host
    // gamepad cursor.* shortcuts to the OSK while it's active. See the
    // OskIsActive() block in ProcessVirtualGamepadCursor for context —
    // host SDL_CONTROLLERBUTTONDOWN events bypass the keyboard
    // intercept that the OSK relies on, so we synthesise OskHandleKey
    // calls on rising / falling edges of the held-state poll.
    bool _vOskPrevUp = false;
    bool _vOskPrevDown = false;
    bool _vOskPrevLeft = false;
    bool _vOskPrevRight = false;
    bool _vOskPrevClick = false;
    bool _vOskPrevCancel = false;

    // OPENRCT2MINI OSK: when a KEYDOWN handed to OskHandleKey closes the
    // OSK (e.g. ESCAPE → Cancel, RETURN → Commit), we still need to
    // swallow the matching KEYUP. Otherwise the now-active topmost
    // window (LoadSave, etc.) sees the KEYUP and reacts (LoadSave's
    // ESCAPE handler closes the dialog). Set on KEYDOWN that just
    // closed the OSK; cleared when its KEYUP arrives or after a
    // single non-matching event passes.
    SDL_Scancode _oskClosingSwallowKey = SDL_SCANCODE_UNKNOWN;

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
    // OPENRCT2MINI W9: a window is shadable iff it has the standard
    // caption + closeBox prefix and isn't flagged noTitleBar — same rule
    // resizeFrame() uses to decide whether to append a shadeBox button.
    static bool isShadableWindow(const WindowBase& w)
    {
        if (w.flags.has(WindowFlag::noTitleBar))
            return false;
        if (w.widgets.size() < 3)
            return false;
        if (w.widgets[1].type != WidgetType::caption)
            return false;
        if (w.widgets[2].type != WidgetType::closeBox)
            return false;
        return true;
    }

    void ShadeWindowUnderCursor()
    {
        auto* wm = GetWindowManager();
        if (wm == nullptr)
            return;
        // _cursorState.position is in scaled (game-canvas) pixels — same
        // coordinate space FindFromPoint compares against w->windowPos.
        WindowBase* w = wm->FindFromPoint(_cursorState.position);
        if (w == nullptr)
            return;
        if (!isShadableWindow(*w))
            return;
        w->toggleShade();
    }

    // OPENRCT2MINI: R1+X close-window. Closes the window under the cursor
    // if it has a closeBox in the standard slot — that's the same bar the
    // user could click manually with face-A on the closeBox itself, so
    // there's no risk of closing a modal / toolbar / tooltip that doesn't
    // expose a close button. Toolbars (noBackground) and the main viewport
    // shell (stickToBack / noTitleBar) all fail isShadableWindow for the
    // same caption + closeBox reason and are excluded automatically.
    void CloseWindowUnderCursor()
    {
        auto* wm = GetWindowManager();
        if (wm == nullptr)
            return;
        WindowBase* w = wm->FindFromPoint(_cursorState.position);
        if (w == nullptr || w->flags.has(WindowFlag::dead))
            return;
        if (!isShadableWindow(*w))
            return;
        wm->Close(*w);
    }

    void ToggleShadeAll()
    {
        // Decide direction in one pass: if any shadable window is
        // currently expanded, shade everything; otherwise unshade all.
        bool anyExpanded = false;
        WindowVisitEach([&anyExpanded](WindowBase* w) {
            if (w == nullptr || w->flags.has(WindowFlag::dead))
                return;
            if (!isShadableWindow(*w))
                return;
            if (!w->isShaded)
                anyExpanded = true;
        });
        const bool target = anyExpanded; // true == should be shaded
        WindowVisitEach([target](WindowBase* w) {
            if (w == nullptr || w->flags.has(WindowFlag::dead))
                return;
            if (!isShadableWindow(*w))
                return;
            if (w->isShaded != target)
                w->toggleShade();
        });
    }

    bool InterceptVirtualCursorKey(SDL_Scancode sc, bool down)
    {
        // OPENRCT2MINI osk-overhaul §1: OSK no longer intercepts raw
        // scancodes. Navigation / activation / backspace now flow
        // through bindable shortcuts (kFocus*, kCursorClick, kCursor-
        // Cancel) into OskContextImpl in InputManager.cpp. Commit /
        // dismiss still flow through the OSK's modal-hooks. Net effect
        // for device users is identical — the default keyboard bindings
        // for those shortcuts cover the same scancodes the OSK was
        // listening to directly. Host users can rebind.
        //
        // The `_oskClosingSwallowKey` pending-KEYUP mechanism is also
        // gone — without the intercept consuming the KEYDOWN there's
        // no asymmetric state to clean up on KEYUP.
        (void)sc;
        (void)down;
        switch (sc)
        {
            // OPENRCT2MINI gamepad-plan 1.6: SDL_SCANCODE_UP/DOWN/LEFT/
            // RIGHT/Z/X intercepts removed. They previously latched
            // into _vKb* flags that fed cursor motion / click / cancel
            // in ProcessVirtualGamepadCursor. Now those scancodes fall
            // through to ShortcutManager which fires the bound
            // shortcut, and the new held-state poll picks up cursor
            // motion / click / cancel from the binding's `current`
            // set (default keyboard arrows / Z / X via register-
            // KeyboardDefault calls in Shortcuts.cpp). Net effect on
            // the device is identical; on host the user can rebind.
            // OPENRCT2MINI W0: device L1 dual-emits Q + LSHIFT and R1 dual-
            // emits A + LALT (the SDL2 set_key patch in build-deps.sh). On
            // host PC there's no patch, so pressing the user-facing letter
            // alone doesn't fire the modifier side. Shadow the modifier
            // bits here so a host dev pressing Q/A gets the same effective
            // behavior as a device user pressing L1/R1.
            //
            // OPENRCT2MINI gamepad-plan 1.6: SDL_SCANCODE_Q intercept
            // dropped. Was injecting KMOD_LSHIFT into SDL's mod state
            // so OpenRCT2's shift-modifier behaviours (raise placement
            // Z, vertical track stack, etc.) fired when device L1 = Q
            // was held. Now kInterfaceShiftModifier with default
            // keyboard "Q" (registered via registerKeyboardDefault in
            // Shortcuts.cpp) ORs into ModifierKey::shift through
            // handleModifiers, covering the same call sites without
            // polluting SDL's chord-shortcut matcher.
            // OPENRCT2MINI input-rework: SDL_SCANCODE_LALT / RALT
            // intercept removed. Was scrubbing KMOD_LALT/RALT from
            // SDL's mod state to suppress SDL2's built-in ALT+RETURN
            // fullscreen toggle, but the side effect was that no
            // ALT+anything chord-shortcut binding could ever match —
            // including the registered ALT+C → kInterfaceCloseWindow-
            // UnderCursor binding. That forced a hardcoded SDL_SCAN-
            // CODE_C chord handler below to detect the chord locally
            // via a private _vGamepadMod latch instead of going
            // through ShortcutManager, which in turn swallowed every
            // C event and made C uncapturable in the rebind UI.
            //
            // Letting ALT/Alt fall through to SDL means ALT+RETURN
            // now toggles fullscreen via SDL's default handling. On
            // the device that's a no-op (libmi_gfx panel has no real
            // windowed mode). On host it's the standard SDL keystroke
            // most users expect. Trading that handful of pixels of
            // ergonomics for a working rebind UI and a working ALT+C
            // close-window chord (which goes through the standard
            // chord-shortcut path now).
            // OPENRCT2MINI cut 59: L1 (LSHIFT) is the Shift modifier reach,
            // but it no longer activates fast cursor — only R1 (F13) does.
            // We don't swallow LSHIFT/RSHIFT so the modifier still
            // propagates to OpenRCT2's input pipeline (scenery
            // vertical-stack, track/footpath Z-raise, etc.).
            case SDL_SCANCODE_LSHIFT:
            case SDL_SCANCODE_RSHIFT: return false;
            // OPENRCT2MINI cut 60 / gamepad-plan 1.6: LCTRL / RCTRL.
            // The SDL driver emits LCTRL for the L1+R1 chord (in place
            // of L1's usual LSHIFT). Real keyboard Ctrl also lands
            // here. Don't swallow — the modifier propagates to SDL's
            // mod state so chord-shortcut matching still works (CTRL+L
            // load, etc.) and so InputManager::handleModifiers() picks
            // it up as ModifierKey::ctrl for construction Z-lock /
            // fast-cursor suppression. 1.6 dropped the _vKbCtrl latch
            // — handleModifiers() reads SDL's mod state directly and
            // also ORs the kInterfaceConstructionZLock binding's held
            // state, covering both the keyboard CTRL and any rebind.
            case SDL_SCANCODE_LCTRL:
            case SDL_SCANCODE_RCTRL: return false;
            // OPENRCT2MINI W0 (was cut 59): face X / face Y / L2 / R2 onto
            // WASD-cluster letters emitted by the SDL2 set_key patch. F-keys
            // (F14-F17) were testable only on full-size PC keyboards; letters
            // are universal. Each case is gated on !hasTextInputFocus so a
            // dev typing in a peep-rename / chat / console field still gets
            // the literal letter; the OSK is the device-side text-entry
            // path and routes its own keys via OskHandleKey above.
            // OPENRCT2MINI gamepad-plan 1.5h: SDL_SCANCODE_W / _S
            // intercepts removed. The device's vendor SDL2 emits W for
            // L2 and S for R2; falling through to ShortcutManager lets
            // the registered shortcut bindings (kViewGeneralRotate-
            // Anticlockwise / kViewGeneralRotateClockwise by default
            // per registerKeyboardDefault calls in Shortcuts.cpp) fire
            // them. The user can rebind L2 / R2 to anything — including
            // zoom, the gamepad-mod swap behaviour the legacy code did
            // automatically — through the normal rebind UI.
            //
            // Loses the cut-31-era _vGamepadMod-conditional rotate-vs-
            // zoom toggle. If anyone wants R1+L2 = zoom-out, they bind
            // a chord shortcut to PAD R1+L2 against kViewGeneralZoomOut
            // explicitly. Cleaner generalisation is worth one less
            // baked-in chord behaviour.
            // OPENRCT2MINI input-rework: SDL_SCANCODE_C intercept
            // removed. Was hardcoding the device's R1+C close-window
            // chord by reading the now-deleted _vGamepadMod latch
            // (only because the LALT scrub above broke real chord-
            // shortcut matching for ALT+anything). With both gone,
            // C falls through to the regular SDL → InputManager →
            // ShortcutManager pipeline:
            //   * Default shade-window (poll-driven) still works.
            //   * ALT+C → kInterfaceCloseWindowUnderCursor fires
            //     through the registered chord binding now that
            //     KMOD_LALT survives.
            //   * PAD R1+Y → kInterfaceCloseWindowUnderCursor fires
            //     through the registered pad chord, same path.
            //   * The C scancode is visible to the rebind UI's
            //     keypress capture, so users can bind C to anything
            //     they want.
            // OPENRCT2MINI gamepad-plan 1.5i: SDL_SCANCODE_V intercept
            // removed. Falls through to ShortcutManager which fires the
            // bound shortcut — by default kInterfaceRotateConstruction
            // (registered with keyboard "V" via registerKeyboardDefault
            // in Shortcuts.cpp). Loses the _vGamepadMod-conditional
            // 3×=anti-clockwise behaviour; user binds a separate chord
            // shortcut if they miss it.
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

    // OPENRCT2MINI gamepad-plan 1.5c: action bridges for shade / close-
    // window shortcuts. Called by the Ui::Fire* free functions defined
    // at the bottom of this file, which in turn run from action lambdas
    // in Shortcuts.cpp.
    //
    // ToggleShadeAll fires on press of kInterfaceToggleShadeAllWindows
    // (default keyboard SHIFT+C, no PAD default — shade-all is also
    // available via a 500 ms hold of the shade-window button).
    //
    // CloseWindowAndSuppressShade fires on press of kInterfaceClose-
    // WindowUnderCursor (default keyboard ALT+C, default PAD R1+Y).
    // The suppress flag prevents the shade-shortcut poll's tap path
    // from also firing on the same chord — without it, R1+Y would
    // close the window AND shade an arbitrary other window when Y is
    // released.
    void ActionToggleShadeAll() { ToggleShadeAll(); }
    void ActionShadeWindowUnderCursor() { ShadeWindowUnderCursor(); }
    void ActionCloseWindowAndSuppressShade()
    {
        CloseWindowUnderCursor();
        // OPENRCT2MINI hold-binding refactor: drop any pending hold
        // tracking for the C / Y key so the shade-window tap doesn't
        // also fire when the user releases the chord. The C / Y press
        // entered _holdPending if the user is holding R1/ALT and pressed
        // C/Y, then the chord matched and fired this action; without
        // this cancel, releasing C/Y would still fire the deferred tap
        // (shade an arbitrary window). Two cancels — one keyboard, one
        // joybutton — cover both physical input devices.
        auto& sm = _shortcutManager;
        sm.cancelPendingHoldForInput(InputDeviceKind::keyboard, SDLK_c);
        sm.cancelPendingHoldForInput(
            InputDeviceKind::joyButton, static_cast<uint32_t>(SDL_CONTROLLER_BUTTON_Y));
    }

    ShortcutManager& GetShortcutManager()
    {
        return _shortcutManager;
    }

    // OPENRCT2MINI text-editing-de-hardcode: expose the live
    // TextComposition for the bindable caret + clipboard action lambdas
    // in Shortcuts.cpp.
    TextComposition& GetTextComposition()
    {
        return _textComposition;
    }

    explicit UiContext(IPlatformEnvironment& env)
        : _platformUiContext(CreatePlatformUiContext())
        , _windowManager(CreateWindowManager())
        , _shortcutManager(env)
    {
        LogSDLVersion();
        // OPENRCT2MINI: SDL_INIT_GAMECONTROLLER is required for SDL to
        // emit SDL_CONTROLLERBUTTONDOWN/UP and SDL_CONTROLLERAXISMOTION
        // events with translated button enum values (SDL_CONTROLLER_-
        // BUTTON_DPAD_UP etc.). Without it, SDL only emits raw
        // SDL_JOYBUTTONDOWN events with hardware-specific button
        // indices, and a DS4's D-pad fires as SDL_JOYHATMOTION rather
        // than buttons at all. queueInputEvent's SDL_CONTROLLERBUTTON-
        // DOWN branch is what populates _heldGamepadButtons with the
        // canonical enum keys that getState's joyButton path checks
        // against, so without GAMECONTROLLER init the held-state poll
        // can't see any gamepad input. SDL_GameControllerOpen on its
        // own auto-inits the subsystem but doesn't register the event
        // watch — so explicitly include SDL_INIT_GAMECONTROLLER here.
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
        {
            SDLException::Throw("SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)");
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
#ifdef ENABLE_SOFTWARE_CURSOR
        // OPENRCT2MINI revision 78: when the software cursor is in charge
        // (always on the device, and on host dev/test builds with
        // ENABLE_SOFTWARE_CURSOR baked in via cut 45), keep the SDL/system
        // cursor permanently hidden. Otherwise game callers like
        // ContextShowCursor / SCENE_INTRO_END / etc. would re-enable the
        // OS cursor on top of the composited software one and the user
        // sees two cursors slightly offset — exactly what looked like a
        // trail under fast movement before. Software-cursor visibility
        // is a separate concern handled inside the engine (loading-window
        // suppression, cut 38b's intro skip, etc.).
        (void)value;
        SDL_ShowCursor(SDL_DISABLE);
#else
        SDL_ShowCursor(value ? SDL_ENABLE : SDL_DISABLE);
#endif
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
#ifdef ENABLE_SOFTWARE_CURSOR
        // OPENRCT2MINI: in cursor-lock drags (camera drag, scroll-drag) the
        // input pipeline calls SetCursorPosition each frame to pin the
        // cursor at gInputDragLast. SDL_WarpMouseInWindow's effect arrives
        // as a SDL_MOUSEMOTION event on the NEXT frame — between now and
        // then, the software-cursor draw reads _cursorState.position which
        // the virtual-cursor poll has already shifted by D-pad/arrow-key
        // motion this frame, so the cursor visibly jitters one pixel away
        // from the lock point and snaps back. Sync _cursorState.position
        // and the virtual-cursor latches up immediately so the very next
        // draw shows the cursor at the lock point.
        const float scale = static_cast<float>(Config::Get().general.windowScale);
        const int32_t scaledX = static_cast<int32_t>(std::round(static_cast<float>(cursorPosition.x) / scale));
        const int32_t scaledY = static_cast<int32_t>(std::round(static_cast<float>(cursorPosition.y) / scale));
        _cursorState.position = { scaledX, scaledY };
        _vcursorX = static_cast<float>(cursorPosition.x);
        _vcursorY = static_cast<float>(cursorPosition.y);
        _vcursorLastIntX = scaledX;
        _vcursorLastIntY = scaledY;
#endif
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

    // OPENRCT2MINI gamepad-plan 1.11: walk every connected SDL game
    // controller and submit a rumble pulse. Inputs are float 0.0–1.0
    // motor intensities (low / high) and a duration in milliseconds.
    // Globally gated by Config::Get().general.gamepadRumbleEnabled and
    // scaled by gamepadRumbleIntensity before the SDL call so users
    // can mute / soften haptics without rebuilding game logic. Pads
    // SDL reports as having no rumble are silently skipped (e.g. Mini
    // before 2.6 wires its motor through SDL).
    //
    // Phase 1.11 ships with one-shot semantics — each call overwrites
    // any in-progress rumble on a given pad. Phase 1.11b adds a
    // per-controller decay queue layered on top so dual-motor pulses
    // with mismatched durations and continuous (loop-driven) rumble
    // can co-exist; for now a hot crash punch on top of an active
    // rumble simply replaces it, which is fine for the events 1.11
    // hooks (crash, construction refusal — both punctual).
    void RumbleControllers(float low, float high, uint32_t durationMs) override
    {
        const auto& cfg = Config::Get().general;
        if (!cfg.gamepadRumbleEnabled)
            return;
        if (durationMs == 0)
            return;

        const float globalScale = std::clamp(cfg.gamepadRumbleIntensity, 0.0f, 1.0f);
        const float scaledLow = std::clamp(low, 0.0f, 1.0f) * globalScale;
        const float scaledHigh = std::clamp(high, 0.0f, 1.0f) * globalScale;
        if (scaledLow <= 0.0f && scaledHigh <= 0.0f)
            return;

        // Convert to SDL's 0..0xFFFF magnitude. round() avoids the
        // off-by-one floor that would map intensity 1.0 to 0xFFFE.
        const auto toSdl = [](float v) -> uint16_t {
            const auto u = static_cast<uint32_t>(std::lround(v * 65535.0f));
            return static_cast<uint16_t>(std::min<uint32_t>(u, 0xFFFF));
        };
        const uint16_t lowMag = toSdl(scaledLow);
        const uint16_t highMag = toSdl(scaledHigh);

        for (auto* gc : _inputManager.getGameControllers())
        {
            if (gc == nullptr)
                continue;
            if (!SDL_GameControllerHasRumble(gc))
                continue;
            // SDL silently ignores the motor a single-motor pad
            // doesn't have, so passing both magnitudes is safe.
            // Phase 1.11b's mix() helper handles the case where SDL
            // reports a single combined motor and we want low + high
            // blended — for 1.11's plumbing we just pass through.
            SDL_GameControllerRumble(gc, lowMag, highMag, durationMs);
        }
    }

    // OPENRCT2MINI gamepad-plan 1.13: DualShock-style LED control.
    // Walks every connected SDL game controller and submits an
    // SDL_GameControllerSetLED colour. The (r, g, b) input is the
    // pre-brightness severity colour; this layer applies the global
    // brightness scaler internally so callers don't need to know the
    // config value. Globally gated by gamepadLedEnabled — when off
    // the call is a complete no-op, and the LED keeps whatever colour
    // it had (Led::tickEngine will issue a 0,0,0 sweep when it next
    // observes the enabled flag false → clear).
    //
    // Pads SDL reports as having no LED (Xbox, most generic USB pads,
    // the Mini's panel button-board pre-Phase-2) are silently skipped
    // — SetLED would error and the engine wastes no CPU on them.
    void SetControllerLED(uint8_t r, uint8_t g, uint8_t b) override
    {
        const auto& cfg = Config::Get().general;
        if (!cfg.gamepadLedEnabled)
            return;

        const float brightness = std::clamp(cfg.gamepadLedBrightness, 0.0f, 1.0f);
        const auto scale = [brightness](uint8_t v) -> uint8_t {
            const auto scaled = static_cast<int32_t>(std::lround(static_cast<float>(v) * brightness));
            return static_cast<uint8_t>(std::clamp(scaled, 0, 255));
        };
        const uint8_t sR = scale(r);
        const uint8_t sG = scale(g);
        const uint8_t sB = scale(b);

        for (auto* gc : _inputManager.getGameControllers())
        {
            if (gc == nullptr)
                continue;
            if (!SDL_GameControllerHasLED(gc))
                continue;
            SDL_GameControllerSetLED(gc, sR, sG, sB);
        }
    }

    // OPENRCT2MINI gamepad-plan 1.13: capability probe. Returns true
    // if any currently-connected pad reports an LED. Led::tickEngine
    // uses this to short-circuit the per-frame fade evaluation on
    // setups where no LED is present (host with Xbox pad, the Mini
    // pre-Phase-2, headless test runs) — saves a multiply + branch
    // per frame at no cost to correctness.
    bool ControllerHasLED() override
    {
        for (auto* gc : _inputManager.getGameControllers())
        {
            if (gc == nullptr)
                continue;
            if (SDL_GameControllerHasLED(gc))
                return true;
        }
        return false;
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

        // OPENRCT2MINI rev 95d: while the bench is running, drain SDL
        // events without dispatching them so input can't perturb the
        // deterministic title-sequence run. SDL_QUIT is still honored
        // so the user can ctrl-C / close the window. Any presses that
        // would normally land on a window, advance the cursor, scroll
        // the console, or fire a keyboard shortcut are silently
        // dropped — the bench is meant to run uninterrupted.
        if (::OpenRCT2::Profiling::Bench::isActive())
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
            {
                if (ev.type == SDL_QUIT)
                    ContextQuit();
            }
            return;
        }

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
                    if (e.motion.xrel != 0 || e.motion.yrel != 0)
                    {
                        OpenRCT2::Ui::Windows::gDropdown.navigationSource
                            = OpenRCT2::Dropdown::NavigationSource::cursor;
                        _inputManager.onTransitionEvent(
                            InputManager::SelectorTransitionSource::realMouseMotion);
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (_inGameConsole.IsOpen())
                    {
                        _inGameConsole.Scroll(e.wheel.y * 3); // Scroll 3 lines at a time
                        break;
                    }
                    _cursorState.wheel -= e.wheel.y;
                    // OPENRCT2MINI mouse-input refactor: scroll wheel
                    // is now a bindable input. Emit a shortcut input
                    // event for each wheel click so any shortcut bound
                    // to MOUSE WHEEL UP / DOWN fires. Default bindings
                    // ship in Shortcuts.cpp (zoom in / out). The
                    // _cursorState.wheel feed above stays so in-widget
                    // scroll handling (WindowAllWheelInput) still
                    // works for scroll widgets.
                    if (e.wheel.y != 0)
                    {
                        const int32_t ticks = std::abs(e.wheel.y);
                        const uint32_t button
                            = (e.wheel.y > 0) ? 8u /*kMouseWheelUpButton*/ : 9u /*kMouseWheelDownButton*/;
                        for (int32_t i = 0; i < ticks; ++i)
                        {
                            InputEvent ie;
                            ie.deviceKind = InputDeviceKind::mouse;
                            ie.modifiers = SDL_GetModState();
                            ie.button = button;
                            ie.state = InputEventState::down;
                            _inputManager.queueInputEvent(std::move(ie));
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                {
                    if (e.button.which == SDL_TOUCH_MOUSEID)
                    {
                        break;
                    }
                    // OPENRCT2MINI mouse-input refactor: no hardcoded
                    // mouse-button bindings. Real LMB / RMB no longer
                    // synthesise StoreMouseInput from this handler —
                    // they're bindable shortcuts now (default
                    // cursor.click ← MOUSE 0, cursor.cancel ← MOUSE 2).
                    // The per-frame poll in ProcessWorldCursor::
                    // handleButton drives the click synthesis off
                    // whatever input is bound to those shortcuts.
                    //
                    // Sync _cursorState.position from the click event
                    // so the poll synthesises StoreMouseInput at the
                    // exact click position (not the most recent
                    // MOUSEMOTION sample). _cursorState.left / right /
                    // middle / touch / old still update so the
                    // cursor renderer keeps the correct pressed-look.
                    _cursorState.position = {
                        static_cast<int32_t>(e.button.x / Config::Get().general.windowScale),
                        static_cast<int32_t>(e.button.y / Config::Get().general.windowScale),
                    };
                    switch (e.button.button)
                    {
                        case SDL_BUTTON_LEFT:
                            _cursorState.left = CURSOR_PRESSED;
                            _cursorState.old = 1;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            _cursorState.middle = CURSOR_PRESSED;
                            break;
                        case SDL_BUTTON_RIGHT:
                            _cursorState.right = CURSOR_PRESSED;
                            _cursorState.old = 2;
                            break;
                    }
                    _cursorState.touch = false;

                    {
                        InputEvent ie;
                        ie.deviceKind = InputDeviceKind::mouse;
                        ie.modifiers = SDL_GetModState();
                        // OPENRCT2MINI mouse-input refactor: convert
                        // SDL's 1-based button index to 0-based to
                        // match ShortcutInput's binding storage
                        // (LMB=0, MMB=1, RMB=2). Without this,
                        // matches() never fired for real mouse
                        // events because binding's button=0 (LMB)
                        // disagreed with event's button=1
                        // (SDL_BUTTON_LEFT).
                        ie.button = e.button.button > 0 ? e.button.button - 1 : 0;
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
                    // OPENRCT2MINI mouse-input refactor: same as
                    // SDL_MOUSEBUTTONDOWN above — release synthesis
                    // is now driven by the per-frame poll, not this
                    // handler. Position-sync so the poll uses the
                    // exact release-event coordinates.
                    _cursorState.position = {
                        static_cast<int32_t>(e.button.x / Config::Get().general.windowScale),
                        static_cast<int32_t>(e.button.y / Config::Get().general.windowScale),
                    };
                    switch (e.button.button)
                    {
                        case SDL_BUTTON_LEFT:
                            _cursorState.left = CURSOR_RELEASED;
                            _cursorState.old = 3;
                            break;
                        case SDL_BUTTON_MIDDLE:
                            _cursorState.middle = CURSOR_RELEASED;
                            break;
                        case SDL_BUTTON_RIGHT:
                            _cursorState.right = CURSOR_RELEASED;
                            _cursorState.old = 4;
                            break;
                    }
                    _cursorState.touch = false;

                    {
                        InputEvent ie;
                        ie.deviceKind = InputDeviceKind::mouse;
                        ie.modifiers = SDL_GetModState();
                        ie.button = e.button.button > 0 ? e.button.button - 1 : 0;
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
                    // OPENRCT2MINI per-binding Modifier mode: filter OS
                    // key auto-repeat (SDL sets e.key.repeat > 0 for
                    // OS-generated repeat-while-held events). Chord
                    // / modifier-mode bindings need rising-edge-only
                    // dispatch so that holding CTRL+C doesn't fire the
                    // chord again at the OS repeat rate. TextComposition
                    // above already handled the repeat for backspace /
                    // caret-move purposes via its own SDL_KEYDOWN path.
                    if (e.key.repeat != 0)
                        break;
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

        // OPENRCT2MINI defaults-export follow-up: only fall back to
        // 640×480 when the config value is the "unset" sentinel
        // (width or height <= 0). Once that's done, we trust whatever
        // the config specifies — including on the Mini, where the
        // canonical seed config.ini already carries -1 / -1 and so
        // hits this fallback path anyway. The previous cut 40c/40d
        // hard-clamp (max(640, width)/max(480, height)) was removed:
        // it stopped host builds from honouring a perfectly valid
        // user-chosen window size, and on the Mini the Options >
        // Display widgets are greyed out anyway so the only way to
        // get an out-of-range value is hand-editing config.ini —
        // which is the user's call to make.
        //
        // If a Mini user hand-edits to a size the mmiyoo render
        // driver can't allocate (texture cap ~800×600), SDL_Create-
        // Texture will fail at first draw and the user gets to deal
        // with that consequence.
        if (width <= 0)
            width = 640;
        if (height <= 0)
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

        // OPENRCT2MINI: cut 29. Minimum window size, NOT a hard
        // override on config.ini's window_width / window_height —
        // those are honoured verbatim above. This call only stops
        // the user from dragging the window smaller than 640×480
        // at runtime via the standard SDL resize handles. 640×480
        // matches the Mini's native panel and is a sane floor for
        // host builds too (upstream's 720×480 would force scaling
        // on the Mini, which is the whole reason for the drop).
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
    // OPENRCT2MINI gamepad-plan 1.6b step 2: OSK-mode cursor handler.
    // Carved out of ProcessVirtualGamepadCursor. While OSK is up the
    // virtual cursor is suspended; the D-pad / face buttons drive OSK
    // selection instead. Routes the cursor.* shortcut held-state to
    // OskHandleKey via per-shortcut edge detection, filtering out
    // keyboard-kind bindings (those are owned by the SDL_KEYDOWN
    // intercept at the top of InterceptVirtualCursorKey — polling
    // keyboard sources here too would advance OSK selection by 2 per
    // press). Mapping is by *purpose*, not physical key — cursor.up
    // → SDL_SCANCODE_UP regardless of which device fired it.
    void ProcessOskCursor()
    {
        // OPENRCT2MINI gamepad-plan 1.6 + hold-binding refactor:
        // legacy _vKb* arrow / Z / X latches deleted; the shade-
        // shortcut tap-vs-hold state used to live in _vShade* here
        // and got reset on every OSK-cursor frame. Both are gone —
        // shade-window/all are now tracked inside ShortcutManager's
        // _holdPending, which is unaffected by OSK transitions and
        // releases naturally on key release.

        struct OskRoute
        {
            std::string_view shortcutId;
            int32_t scancode;
            bool* prev;
        };
        const OskRoute routes[] = {
            { ShortcutId::kCursorUp,     SDL_SCANCODE_UP,    &_vOskPrevUp },
            { ShortcutId::kCursorDown,   SDL_SCANCODE_DOWN,  &_vOskPrevDown },
            { ShortcutId::kCursorLeft,   SDL_SCANCODE_LEFT,  &_vOskPrevLeft },
            { ShortcutId::kCursorRight,  SDL_SCANCODE_RIGHT, &_vOskPrevRight },
            { ShortcutId::kCursorClick,  SDL_SCANCODE_Z,     &_vOskPrevClick },
            { ShortcutId::kCursorCancel, SDL_SCANCODE_X,     &_vOskPrevCancel },
        };
        for (const auto& r : routes)
        {
            const auto* sc = _shortcutManager.getShortcut(r.shortcutId);
            bool now = false;
            if (sc != nullptr)
            {
                // OPENRCT2MINI gamepad-plan 1.10.3: skip mouse-source
                // bindings for kCursorClick. Without this, real LMB
                // (default mouse-source kCursorClick binding from #369)
                // routes through the SDL_SCANCODE_Z synthesis below
                // and selects the *focused* OSK key (D-pad position)
                // instead of the key under the cursor — the same
                // physical click would also reach the OSK widget's
                // normal click handler via StoreMouseInput, but the
                // Z synthesis runs first and consumes the action.
                // Filtering mouse sources out of the click route lets
                // mouse-clicks fall through to StoreMouseInput so the
                // OSK widget picks them up at the cursor position;
                // gamepad / keyboard sources still synthesise Z to
                // commit the focused key, which is the documented
                // gamepad nav behaviour.
                //
                // Keyboard sources are skipped for ALL routes (not just
                // click) because the SDL_KEYDOWN intercept already
                // routes UP/DOWN/LEFT/RIGHT/Z/X to OskHandleKey
                // directly — running them through here too would
                // double-fire.
                const bool skipMouseForClick = (r.shortcutId == ShortcutId::kCursorClick);
                for (const auto& input : sc->current)
                {
                    if (input.kind == InputDeviceKind::keyboard)
                        continue;
                    if (skipMouseForClick && input.kind == InputDeviceKind::mouse)
                        continue;
                    if (_inputManager.getState(input))
                    {
                        now = true;
                        break;
                    }
                }
            }
            if (now != *r.prev)
            {
                Windows::OskHandleKey(r.scancode, now);
                *r.prev = now;
            }
        }
    }

    // OPENRCT2MINI gamepad-plan 1.6b step 3: world-mode cursor handler.
    // Carved out of ProcessVirtualGamepadCursor's tail. Owns the shade
    // tap-vs-hold detector, the cursor.* held-state polls (motion /
    // click / cancel / fast-modifier), and the per-frame velocity
    // integrator that drives _vcursorX/Y and emits synthetic mouse
    // press/release events. Runs whenever getActiveContext() ==
    // world (the default).
    //
    // Ordering note: this method reads _vOskPrev* indirectly via
    // ProcessOskCursor's edge tracking (none of these reads happen in
    // world mode), so the dispatcher's _vOskPrev* clear above is
    // belt-and-braces — keeps stale prev state from a prior OSK
    // session from synthesising a release the next time the OSK
    // opens. The shade-shortcut state (_vShade*) is reset each frame
    // by ProcessOskCursor while OSK is up, so on first world-mode
    // frame after OSK closes the rising/falling edge logic here
    // starts clean.
    void ProcessWorldCursor()
    {
        // OPENRCT2MINI hold-binding refactor: the per-frame shade-window
        // poll that used to live here (rising-edge press tracking +
        // hold timer + tap-on-release with a suppress flag) is gone.
        // ShortcutManager now owns the tap-vs-hold dispatch via the
        // HoldPendingState mechanism: kInterfaceShadeWindowUnderCursor
        // (default "C" / "PAD Y") is a tap binding (holdMs == 0) and
        // kInterfaceToggleShadeAllWindows has additional "HOLD C" /
        // "HOLD PAD Y" bindings (holdMs == 500). The same physical
        // input drives both; ShortcutManager defers the tap on press
        // and fires whichever side wins (release-before-500ms ⇒ tap,
        // 500ms-elapsed ⇒ hold). The R1+C / R1+Y close-window chord
        // calls ShortcutManager::cancelPendingHoldForInput from its
        // action so the deferred tap doesn't fire on chord release.

        // OPENRCT2MINI cursor-cancel-drag split: kInterfaceCameraDrag
        // held-state poll. This is the SOLE source of begin/end for
        // every right-click-style drag gesture — camera-pan over a
        // viewport, scroll-drag over an overflowing list, and window-
        // position drag over a draggable dialog. cursor.cancel used to
        // ride along on the same RMB and synthesise rightPress through
        // the MouseInput state machine (which ran the dispatch), but
        // that bundling meant any binding of cursor.cancel pulled the
        // drag gestures along with it. The drag-init dispatch is now
        // local to InputContextDragBeginAtCursor in MouseInput.cpp,
        // driven exclusively by this poll, and cursor.cancel keeps
        // only its cancel-action role.
        //
        // Begin gates by cursor location inside InputContextDragBegin-
        // AtCursor (viewport → scroll → window, with isUndraggableShell
        // filters), so the poll is safe to run unconditionally — it's
        // a no-op when the cursor isn't over a draggable target.
        //
        // The polling-driven End is split from the state-machine end
        // (which used to handle MouseState::rightRelease in the three
        // drag states) so the gesture works on inputs that don't
        // synthesise a rightRelease event — e.g. PAD B bound to
        // kInterfaceCameraDrag. Motion-continue stays in the state
        // machine since it's already driven by per-frame released-
        // fallback mouse events.
        {
            const auto* dragShortcut = _shortcutManager.getShortcut(
                ShortcutId::kInterfaceCameraDrag);
            // Rebind-capture gate (matches the cursor.* held-state poll
            // below): when the Input Bindings window is capturing an
            // input, suppress the drag begin/end edge detection so a
            // user pressing the input currently bound to camera-drag
            // (RMB / PAD B by default) to capture it doesn't start a
            // camera-pan / scroll-drag / window-drag mid-capture. The
            // falling edge logic re-uses _vCameraDragPrev — by clamping
            // dragNow to false on every frame the modal is open, we
            // also ensure that if a drag had somehow started just before
            // the modal popped, it gets cleanly ended on the next frame.
            bool dragNow = (dragShortcut != nullptr) && _inputManager.getState(*dragShortcut);
            if (_shortcutManager.isPendingShortcutChange())
                dragNow = false;
            if (dragNow && !_vCameraDragPrev)
            {
                // OPENRCT2MINI cursor-cancel-drag split: this poll now
                // dispatches ALL three drag gestures (camera-pan,
                // scroll-drag, window-drag) based on what the cursor is
                // over at press time. cursor.cancel no longer triggers
                // drag init — see MouseInput.cpp rightPress comment.
                InputContextDragBeginAtCursor();
                // §3.5 (Phase C follow-up 2026-05-17): record whether
                // the same physical input is bound to both camera.drag
                // and cursor.cancel. The user's bindings can route an
                // input to either or both — mouse RMB default is both;
                // default gamepad PAD X is camera.drag only; default
                // gamepad PAD B is cursor.cancel only. We only want to
                // synth kCursorCancel release on the falling edge when
                // the press WAS shared, otherwise pressing PAD X would
                // fire deletion in addition to its drag — and PAD B
                // wouldn't fire deletion at all because the synthesis
                // path is the only one wired for it.
                const auto* cancelShortcut = _shortcutManager.getShortcut(
                    ShortcutId::kCursorCancel);
                _vCameraDragSharedWithCancel
                    = (cancelShortcut != nullptr)
                    && _inputManager.getState(*cancelShortcut);
            }
            else if (!dragNow && _vCameraDragPrev)
            {
                // Capture the camera-tap state BEFORE ending — the end
                // function clears _inputState and so wipes the short-
                // press tick. Only camera-pan has the tap fallback; for
                // a scroll-drag or window-drag, the begin already moved
                // state and a release just commits.
                const bool wasCameraDrag = CameraDragInProgress();
                const bool wasShortPress = wasCameraDrag && CameraDragWasShortPress();
                InputContextDragEndCurrent();
                // §3.5 shared-binding gate: only synthesise if the
                // input that drove the drag is also bound to
                // cursor.cancel. Otherwise the user pressed a
                // camera.drag-only input (e.g. default PAD X) and
                // expects only the drag — no destructive action.
                if (wasShortPress && _vCameraDragSharedWithCancel)
                {
                    // OPENRCT2MINI cursor-cancel-tile-action-plan
                    // §3.3 (Phase C): synthesise kCursorCancel into
                    // the active strategy instead of calling
                    // ViewportInteractionRightClick directly. The
                    // active strategy decides what to do —
                    // ToolContext::onCancel dispatches the right-
                    // click action at the grid cursor's tile (so a
                    // short-press in a tool with the grid cursor
                    // armed deletes the gamepad-pointed tile, not
                    // the mouse-pointed one); when no grid cursor
                    // is armed, the strategy falls back to the
                    // screen-coord right-click — same call the
                    // legacy mouse-short-press path made directly.
                    //
                    // §3.4 (Phase C follow-up 2026-05-17): the
                    // short-press timer is only used when the
                    // action would be a delete (tool context).
                    // World context cursor.cancel used to bring up
                    // info windows on RMB tap; with kInterface-
                    // CameraDrag now owning the camera-pan
                    // gesture on RMB, a short tap in world is
                    // just a tap on the camera — no info window.
                    // The user can still get peep / banner detail
                    // dialogs by clicking the element with the
                    // primary cursor button (cursor.click on
                    // LMB) which dispatches the same info via the
                    // viewport interaction left-click path.
                    //
                    // The poll is device-agnostic — it reacts to
                    // any input bound to kInterfaceCameraDrag,
                    // whether mouse, keyboard, or gamepad. So a
                    // user who rebinds a gamepad button to BOTH
                    // cursor.cancel and camera.drag (mirroring
                    // the mouse RMB default) gets the same
                    // tap-to-delete behaviour as the default
                    // mouse setup.
                    //
                    // §7.6 gate: only synthesise when the active
                    // context is a tool context. Modal text
                    // contexts (OSK, loadSave, textInput, etc.)
                    // interpret kCursorCancel as backspace /
                    // dismiss / etc., and a mouse right-click
                    // over a tool window's modal text-entry
                    // should NOT eat that as a backspace.
                    auto& im = GetInputManager();
                    const auto ctx = im.getActiveContext();
                    const bool isToolCtx
                        = ctx == InputContext::toolFootpath
                        || ctx == InputContext::toolTerrain
                        || ctx == InputContext::toolWater
                        || ctx == InputContext::toolScenery
                        || ctx == InputContext::toolLandRights
                        || ctx == InputContext::toolTileInspector
                        || ctx == InputContext::toolRideConstruction;
                    // Active context is `widgetFocus` whenever the
                    // user has not explicitly cycled INTO the tool
                    // viewport entry — including the common case of
                    // the tool window itself being focused. A tool
                    // can still be armed in that state
                    // (gInputFlags.toolActive). RMB on a destroyable
                    // element in land tool mode is exactly that
                    // path, so include it here too.
                    const bool armedToolInFocus
                        = ctx == InputContext::widgetFocus
                        && gInputFlags.has(InputFlag::toolActive);
                    if (isToolCtx || armedToolInFocus)
                    {
                        InputEvent ev{};
                        ev.deviceKind = InputDeviceKind::mouse;
                        ev.state = InputEventState::release;
                        im.shouldSuppressAction(ShortcutId::kCursorCancel, ev);
                    }
                }
                _vCameraDragSharedWithCancel = false;
            }
            _vCameraDragPrev = dragNow;
        }

        // OPENRCT2MINI gamepad-plan 1.6b step 0: cursor motion / click /
        // cancel / fast-modifier are driven solely by held-state polls
        // against the cursor.* shortcut bindings. The legacy
        // SDL_GameControllerGetButton(...) direct poll (DPAD_UP/DOWN/
        // LEFT/RIGHT, BUTTON_A/B, RIGHTSHOULDER) was deleted here; that
        // path predated rev 329's getState joybutton fix and was OR'd
        // into the same flags the held-state poll already populates —
        // pure duplication.
        //
        // The held-state poll uses ShortcutManager rather than raw SDL,
        // so:
        //   * The user can rebind cursor.* to any keyboard key, mouse
        //     button, or PAD button via the rebind UI.
        //   * Default PAD bindings (PAD DPAD_*, PAD A, PAD B, PAD R1
        //     for fast-modifier) are registered in Shortcuts.cpp via
        //     registerPadDefault.
        //   * Default keyboard bindings (UP/DOWN/LEFT/RIGHT, Z, X,
        //     LALT) are registered via registerKeyboardDefault — covers
        //     the device's vendor-SDL2 fake-scancode emissions (arrow
        //     keys / Z / X / R1→LALT) without any device-specific
        //     code path here.
        //
        // ShortcutManager dispatches *actions* on press only, but cursor
        // click / right-click / motion need PRESS AND RELEASE transi-
        // tions for drag, context-menu, and continuous cursor velocity.
        // Hence the held-state poll instead of action lambdas — the
        // existing edge-trigger logic in handleButton synthesises mouse
        // press / release transitions from these flags.
        //
        // Same shape as W2's window-drag held-state poll (MouseInput.cpp
        // §549). The action lambdas registered in 1.5a stay as stubs —
        // they could fire on press transitions, but doing so would
        // double-fire alongside this poll's edge detection.
        bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
        bool btnA = false, btnB = false, fastModifier = false;
        // OPENRCT2MINI cursor-selector-modal-plan §3.4 / CS.6:
        // while the selector owns the screen, the polled cursor.*
        // velocity is suppressed — the cursor doesn't move, so its
        // ProcessMouseOver loop doesn't clobber dropdown highlights
        // (F.14 navigationSource flip stays correct) and the
        // physical D-pad is free to drive focus.* without the
        // virtual cursor following along. Real mouse motion flips
        // the state back to `mixed`, at which point the polled
        // velocity is re-enabled.
        const bool selectorActive
            = _inputManager.getSelectorMode() == InputManager::SelectorMode::active;
        // OPENRCT2MINI rebind-capture gate: when the Input Bindings
        // window is in capture mode (user pressed a row to rebind a
        // shortcut and the modal is waiting for an input), the
        // ShortcutManager's processEvent path correctly bypasses
        // action dispatch — but the held-state polls below read
        // _inputManager.getState() which still reports the literal
        // physical state of the bound input. Without this gate, a
        // user pressing the input currently bound to cursor.click
        // (e.g. Z, LMB, or PAD A) to capture it as a new binding
        // synthesises a leftPress in handleButton, which the mouse
        // pump dispatches to whatever widget is under the cursor
        // (often the rebind modal itself — dismissing it before the
        // capture commits). Same class of failure for cursor.cancel
        // (synthetic rightPress), cursor.up/down/left/right (virtual
        // cursor wandering mid-rebind) and kInterfaceCameraDrag
        // (drag begins mid-rebind — gated separately below at the
        // camera-drag poll).
        const bool capturePending = _shortcutManager.isPendingShortcutChange();
        {
            auto& sm = _shortcutManager;
            if (auto* up = sm.getShortcut(ShortcutId::kCursorUp))
            {
                dpadUp = _inputManager.getState(*up);
            }
            if (auto* down = sm.getShortcut(ShortcutId::kCursorDown))
            {
                dpadDown = _inputManager.getState(*down);
            }
            if (auto* left = sm.getShortcut(ShortcutId::kCursorLeft))
            {
                dpadLeft = _inputManager.getState(*left);
            }
            if (auto* right = sm.getShortcut(ShortcutId::kCursorRight))
            {
                dpadRight = _inputManager.getState(*right);
            }
            if (selectorActive || capturePending)
            {
                dpadUp = dpadDown = dpadLeft = dpadRight = false;
            }
            if (auto* clickShortcut = sm.getShortcut(ShortcutId::kCursorClick))
            {
                btnA = _inputManager.getState(*clickShortcut);
            }
            if (auto* cancelShortcut = sm.getShortcut(ShortcutId::kCursorCancel))
            {
                btnB = _inputManager.getState(*cancelShortcut);
            }
            // OPENRCT2MINI cursor-selector-modal-plan v2 follow-up:
            // while the selector owns the screen the world cursor is
            // entirely disabled — both visually (HardwareDisplayDraw-
            // ingEngine suppresses the software cursor sprite) and
            // for input. Without this, a real LMB press at the
            // invisible cursor's last position still passes through
            // cursor.click's MOUSE 0 default binding into the held-
            // state poll, and handleButton below synthesises
            // StoreMouseInput(leftPress) at _cursorState.position —
            // i.e. the click registers on whatever widget sits at the
            // (hidden) cursor's coords.
            //
            // The earlier revision tried to "suppress only the press
            // edge so any in-flight release still fires" by gating on
            // !_vprevA / !_vprevB. That's exactly wrong for the
            // auto-wake case. Sequence on host with no other windows
            // when PAD A is mapped to cursor.click:
            //
            //   1. Press: selector still hidden, _vprevA=false.
            //      handleButton sees press edge, synthesises
            //      leftPress at the virtual cursor's pos. MouseInput
            //      lights up the Options toolbar widget and opens the
            //      dropdown.
            //   2. Auto-wake: the new dropdown is a non-chrome
            //      topmost window so the selector flips hidden→active
            //      this frame. _vprevA is now true (handleButton
            //      promoted it on the press edge).
            //   3. Release: selectorActive=true, _vprevA=true. The
            //      "if (!_vprevA) btnA=false" gate does NOT clear
            //      btnA. handleButton sees a fresh release edge and
            //      synthesises StoreMouseInput(leftRelease) at the
            //      virtual cursor's current pos — which still sits
            //      right under the dropdown's first menu item. Mouse-
            //      Input's DropdownActive::leftRelease handler then
            //      runs DropdownIndexFromPoint and commits item 0.
            //
            // So when the selector is active, also force-clear the
            // held-state tracking. Forcing _vprevA=false means
            // handleButton sees prev=now=false on the release frame
            // and skips both edge synthesis paths. We don't leave any
            // widget stuck pressed-down because the leftPress edge
            // already opened the dropdown — MouseInput's normal
            // DropdownActive state will tear that down via its own
            // bottom-toolbar / escape paths, and the selector-driven
            // focus mode owns dispatch from here on.
            if (selectorActive)
            {
                btnA = false;
                _vprevA = false;
                btnB = false;
                _vprevB = false;
            }
            // Rebind capture: suppress click/cancel synthesis entirely,
            // including releases of presses that started before capture.
            // The user starts a capture by pressing the Change button —
            // _vprevA was already false at that point, so there's no
            // release to honour. Any press-and-hold spanning the modal
            // popup is the user's input-being-captured, not a real
            // click intent.
            if (capturePending)
            {
                btnA = false;
                btnB = false;
            }
            if (auto* fastShortcut = sm.getShortcut(ShortcutId::kCursorFastModifier))
            {
                fastModifier = _inputManager.getState(*fastShortcut);
            }
        }
        // OPENRCT2MINI cut 60 / gamepad-plan 1.5d: when the Z-lock
        // modifier is held, suppress fast cursor. Z-lock means the user
        // is being precise on the construction Z axis; fast cursor
        // defeats the purpose. We consult InputManager's canonical
        // ModifierKey::ctrl bit instead of polling sources separately —
        // 1.5d-ext fattened handleModifiers() to OR the
        // kInterfaceConstructionZLock binding's held-state into that
        // bit, so it already covers both the legacy SDL_SCANCODE_LCTRL/
        // RCTRL latch (real keyboard Ctrl + the device's L1+R1 chord
        // mapped to LCTRL by the vendor SDL2) and any user rebind.
        if (_inputManager.isModifierKeyPressed(ModifierKey::ctrl))
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

        // OPENRCT2MINI cursor-sync (2026-05-17): when the selector
        // owns input (focus mode / grid cursor), the software pixel
        // cursor sprite is hidden (see HardwareDisplayDrawing-
        // Engine.cpp:407). Park the hidden cursor's underlying
        // position at whatever the user is currently working on —
        // the grid / edge cursor's tile in a tool context, or the
        // focused widget's centre in widget-focus context — so a
        // switch back to mouse input (real mouse motion or an
        // explicit show-cursor binding) wakes the pointer where the
        // user already had attention, not at a stale OS-pointer
        // position from before the gamepad took over.
        //
        // This runs AFTER the resync-from-real-mouse block above
        // intentionally: when the user moves the real mouse,
        // SelectorMode flips to `hidden` first (the v2 lastInput-
        // WasRealMouse state machine in InputManager); the sync
        // block is gated on `selectorActive` and so no-ops on the
        // first frame after a real mouse wake. That preserves the
        // "cursor wakes wherever the OS pointer is when the user
        // grabs the mouse" UX.
        if (selectorActive)
        {
            std::optional<ScreenCoordsXY> syncTo;
            // OPENRCT2MINI cursor-sync revised 2026-05-17 #2: prefer
            // the global map-selection tile when a tool is armed —
            // gMapSelectPositionA tracks the grid cursor's tile both
            // when the user is engaged in grid-cursor mode (the
            // GridCursorModel writes the globals via WriteGridCursor-
            // Selection on each step) and when the active context is
            // still widgetFocus (the tool's mouse-hover code writes
            // the globals, which the gamepad also follows once the
            // user engages). The earlier shape only consulted the
            // active strategy's cursor model — that returned the
            // stale PixelCursorModel for widgetFocus and the cursor
            // parked at the focused widget instead of the selected
            // tile.
            if (gMapSelectFlags.has(MapSelectFlag::enable)
                || gMapSelectFlags.has(MapSelectFlag::enableConstruct))
            {
                syncTo = ViewportInteractionMapToScreen(gMapSelectPositionA);
            }
            // Strategy cursor model fallback: kept as a backstop for
            // edge cases where MapSelectFlag::enable isn't set (rare;
            // some tools dispatch placement without setting the
            // ghost). dynamic_cast on the active context's cursor
            // model picks up GridCursorModel / EdgeCursorModel
            // directly.
            if (!syncTo.has_value())
            {
                auto& strategy = _inputManager.getActiveContextStrategy();
                if (auto* model = strategy.getCursorModel(); model != nullptr)
                {
                    CoordsXY worldXY{};
                    bool haveWorld = false;
                    if (auto* grid = dynamic_cast<GridCursorModel*>(model); grid != nullptr)
                    {
                        worldXY = grid->getPosition().ToCoordsXY();
                        haveWorld = true;
                    }
                    else if (auto* edge = dynamic_cast<EdgeCursorModel*>(model); edge != nullptr)
                    {
                        worldXY = edge->getPosition().ToCoordsXY();
                        haveWorld = true;
                    }
                    if (haveWorld)
                        syncTo = ViewportInteractionMapToScreen(worldXY);
                }
            }
            // Widget-focus fallback: only when there's no tool
            // ghost AND no tile cursor model. Park the pixel
            // cursor at the focused widget's centre — WidgetFocus
            // .cpp already uses these bounds for drawing the focus
            // ring; we reuse the same geometry so the cursor lands
            // behind the ring.
            //
            // Gate on toolActive so the widget-focus fallback never
            // overrides a tool-armed state. The previous shape ran
            // this whenever the gMapSelectFlags / strategy paths
            // didn't produce a syncTo — including the transient
            // first frames after a tool is armed but before
            // gMapSelectFlags::enable is written. In that window
            // the sync would park at the focused tool-window
            // button, the user would move the mouse to wake the
            // cursor, and (because the OS pointer was warped to
            // the focus widget — see SDL_WarpMouseInWindow below)
            // the wake position would be the focus button, not
            // the grid cursor. With this gate, while a tool is
            // armed, we either park at the grid cursor's tile OR
            // leave the cursor alone (no fallback) — never at the
            // tool window's focused button.
            if (!syncTo.has_value()
                && !gInputFlags.has(InputFlag::toolActive))
            {
                if (auto* w = _inputManager.getFocusedWindow(); w != nullptr)
                {
                    const auto widgetIdx = _inputManager.getFocusedWidget();
                    if (widgetIdx != kWidgetIndexNull && widgetIdx < w->widgets.size())
                    {
                        const auto& widget = w->widgets[widgetIdx];
                        const auto cx = w->windowPos.x + (widget.left + widget.right) / 2;
                        const auto cy = w->windowPos.y + (widget.top + widget.bottom) / 2;
                        syncTo = ScreenCoordsXY{ cx, cy };
                    }
                }
            }
            if (syncTo.has_value())
            {
                // Clamp so the parked position can never escape the
                // canvas (off-screen tile in a panned viewport, for
                // example). Out-of-range coords would still wake at
                // the clamped edge — usable, no crash.
                const int32_t maxX = std::max(_width, 1) - 1;
                const int32_t maxY = std::max(_height, 1) - 1;
                const int32_t sx = std::clamp(syncTo->x, 0, maxX);
                const int32_t sy = std::clamp(syncTo->y, 0, maxY);
                _cursorState.position = { sx, sy };
                _vcursorX = static_cast<float>(sx) * scale;
                _vcursorY = static_cast<float>(sy) * scale;
                _vcursorLastIntX = sx;
                _vcursorLastIntY = sy;
                // Also warp the OS pointer. Writing _cursorState
                // .position alone affects only the internal mirror;
                // the OS pointer stays wherever the user last left
                // it. On the next real mouse-motion event SDL
                // reports the OS pointer's absolute coords, the
                // resync-from-real-mouse block clobbers our
                // parked state, and the cursor wakes at the OS
                // pointer position — defeating the consistency
                // guarantee. SDL_WarpMouseInWindow synchronises
                // the OS pointer so the mouse-motion wake position
                // is near our parked tile rather than wherever
                // the OS pointer happened to be when the gamepad
                // took over.
                //
                // Mini doesn't have a real mouse, so the warp is
                // a no-op there; on host (windowed / fullscreen)
                // it works as expected.
                if (_window != nullptr)
                    SDL_WarpMouseInWindow(_window, sx, sy);
            }
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

        // OPENRCT2MINI gamepad-plan 1.9 follow-on: analog cursor input
        // through the bindable cursor.up/down/left/right shortcuts.
        // Reads each direction's analog magnitude (0.0..1.0) via
        // InputManager::getAnalogState, which only inspects joyAxis
        // bindings — digital cursor.* bindings (keyboard arrows, PAD
        // DPAD_*) go through the digital block above. Net X / Y
        // velocity = right-left and down-up magnitudes.
        //
        // The user can rebind cursor.up/down/left/right to any
        // joyAxis (e.g. STICK_R UP, or trigger axes for accelerator-
        // pedal cursor speed) and the analog velocity follows the
        // binding. Default PAD STICK_L bindings ship in
        // Shortcuts.cpp.
        //
        // Speed model: 200 px/sec at full deflection × gamepad-
        // Sensitivity (the cursor speed knob from cut 58). Fast-
        // modifier still applies for the 2.5× boost.
        {
            // ShortcutManager::getShortcut is non-const (it returns a
            // non-const RegisteredShortcut*); take a non-const ref so
            // the calls compile. We don't mutate anything via these
            // pointers here, just read held-state.
            auto& sm = _shortcutManager;
            const auto* cursorUp = sm.getShortcut(ShortcutId::kCursorUp);
            const auto* cursorDown = sm.getShortcut(ShortcutId::kCursorDown);
            const auto* cursorLeft = sm.getShortcut(ShortcutId::kCursorLeft);
            const auto* cursorRight = sm.getShortcut(ShortcutId::kCursorRight);

            const float upMag = (cursorUp != nullptr) ? _inputManager.getAnalogState(*cursorUp) : 0.0f;
            const float downMag = (cursorDown != nullptr) ? _inputManager.getAnalogState(*cursorDown) : 0.0f;
            const float leftMag = (cursorLeft != nullptr) ? _inputManager.getAnalogState(*cursorLeft) : 0.0f;
            const float rightMag = (cursorRight != nullptr) ? _inputManager.getAnalogState(*cursorRight) : 0.0f;

            const float rawX = rightMag - leftMag;
            const float rawY = downMag - upMag;

            if (rawX != 0.0f || rawY != 0.0f)
            {
                const float speedMult = std::clamp(Config::Get().general.gamepadSensitivity, 0.1f, 5.0f);
                float speedPxPerSec = 200.0f * speedMult;
                if (fastModifier && !_inputManager.isModifierKeyPressed(ModifierKey::ctrl))
                    speedPxPerSec *= 2.5f;
                const float speedThisFrame = speedPxPerSec * dtSec;
                const float adx = rawX * speedThisFrame;
                const float ady = rawY * speedThisFrame;
                const float maxX = static_cast<float>(std::max(_width, 1) * scale - 1);
                const float maxY = static_cast<float>(std::max(_height, 1) * scale - 1);
                _vcursorX = std::clamp(_vcursorX + adx, 0.0f, maxX);
                _vcursorY = std::clamp(_vcursorY + ady, 0.0f, maxY);
                const int32_t newX = static_cast<int32_t>(std::round(_vcursorX / scale));
                const int32_t newY = static_cast<int32_t>(std::round(_vcursorY / scale));
                _cursorState.position = { newX, newY };
                _vcursorLastIntX = newX;
                _vcursorLastIntY = newY;
            }
        }

        // A/B (and Z/X) → left/right mouse buttons. Only emit on transition.
        auto handleButton = [&](bool now_pressed, bool& prev_pressed,
                                MouseState pressEvent, MouseState releaseEvent,
                                uint8_t& cursorBtn, uint8_t cursorOldDown,
                                uint8_t cursorOldUp) {
            if (now_pressed == prev_pressed)
                return;
            // OPENRCT2MINI W9: use _cursorState.position (the authoritative
            // current cursor pos) instead of recomputing from _vcursorX/Y
            // via truncate. The D-pad write block above stores
            // _cursorState.position by ROUNDING _vcursorX/Y, but truncate
            // here disagreed with that round whenever _vcursorX had a
            // fractional part >= 0.5 — which happens any time the user
            // navigated via D-pad / arrow keys to a non-integer float
            // position. The 1-pixel mismatch made gInputDragLast (set
            // from the press event's truncated pos) lag _cursorState.position
            // during a drag; per-frame newWindowCoords ended up at
            // windowPos+1 even though the cursor was completely still,
            // and on release the truncate-based release event snapped
            // the window back to windowPos. Reading _cursorState.position
            // directly removes the disagreement.
            ScreenCoordsXY pos = _cursorState.position;
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

    // OPENRCT2MINI gamepad-plan 1.6b step 3: per-frame dispatcher.
    // Asks InputManager what context we're in and routes to the
    // appropriate handler. Adding a new context is a one-line switch
    // arm here plus a new ProcessXxxCursor method — no existing
    // handler touched. The OSK-just-closed _vOskPrev* clear is the
    // dispatcher's belt-and-braces against a stale "still held"
    // record from a previous OSK session synthesising a phantom
    // release the next time OSK opens.
    void ProcessVirtualGamepadCursor()
    {
        switch (_inputManager.getActiveContext())
        {
            // OPENRCT2MINI osk-overhaul bug-fix §D: route the OSK
            // context through the same ProcessWorldCursor as every
            // other modal. ProcessOskCursor (now obsolete — it only
            // ever synthesised OskHandleKey scancodes, which became a
            // no-op once OSK navigation moved to focus-mode dispatch)
            // skipped the cursor.click → StoreMouseInput(leftPress)
            // synthesis that ProcessWorldCursor owns. Without that
            // synthesis no widget gets clicked: not OSK keys, not the
            // OK / Cancel / close box on the parent TextInputWindow,
            // not the textbox below — neither for the gamepad path
            // (cursor.click ← PAD A) nor for the real-mouse path
            // (cursor.click ← MOUSE LEFT). Falling through to the
            // shared handler restores normal click behaviour for
            // every window underneath / alongside the OSK.
            case InputContext::osk:
            case InputContext::world:
            // OPENRCT2MINI focus-mode-plan / Phase F.7 follow-up:
            // widgetFocus reuses the world cursor handler. The real
            // mouse-click synthesis lives there — LMB-held → fires
            // StoreMouseInput leftPress → MouseInput.cpp dispatches
            // onMouseDown/Up on whatever widget the cursor is over.
            // Without this fallthrough the widgetFocus context took
            // the default arm and did NOTHING per frame, which left
            // mouse clicks completely unprocessed (user reported
            // clicks not working at all on the title screen after
            // focus mode landed).
            //
            // D-pad-into-virtual-cursor movement also runs from
            // ProcessWorldCursor, which means D-pad in focus mode
            // currently moves BOTH the focus ring (via the strategy
            // dispatch) AND the virtual mouse cursor (via this
            // poll). The double-effect is mildly confusing but
            // strictly additive — users can still navigate via D-
            // pad and click whatever they end up over. Splitting
            // the mouse-click synthesis out of ProcessWorldCursor
            // so widgetFocus only gets the click half is the
            // proper follow-up.
            case InputContext::widgetFocus:
            // OPENRCT2MINI input-plan fix: modal list/typing contexts
            // also need the world cursor poll. The poll's job here is
            // to synthesise widget-level mouse events from the
            // cursor.click / cursor.cancel held state and to dispatch
            // camera/scroll/window drags from cursor.cancel (or its
            // user-bound replacement). Without this, the LoadSave /
            // overwrite-prompt / TextInput / console / textbox modals
            // are visually responsive (hover still updates the
            // _cursorState position via SDL_MOUSEMOTION) but every
            // click is dead — there's no other code path that turns
            // an LMB-down into StoreMouseInput(leftPress) → widget
            // dispatch. Earlier rationale claimed "RMB would start a
            // camera drag underneath the modal" but InputContextDrag-
            // BeginAtCursor already routes by cursor target — RMB
            // over the modal's title bar starts a window-drag on the
            // modal, never a camera-pan underneath. The earlier skip
            // was an over-correction that killed clicks.
            //
            // The shortcut-allow-list in InputManager::isShortcut-
            // AllowedInActiveContext stays the narrow allow-list for
            // these modals — cursor.click / cursor.cancel are
            // included so the held-state poll's getState() reads
            // true on the press, and the action lambdas are empty so
            // no other side effect can leak through.
            case InputContext::textInput:
            case InputContext::loadSaveOverwritePrompt:
            case InputContext::loadSave:
            case InputContext::console:
            case InputContext::widgetTextBox:
                _vOskPrevUp = _vOskPrevDown = _vOskPrevLeft = _vOskPrevRight = false;
                _vOskPrevClick = _vOskPrevCancel = false;
                ProcessWorldCursor();
                return;
            default:
                _vOskPrevUp = _vOskPrevDown = _vOskPrevLeft = _vOskPrevRight = false;
                _vOskPrevClick = _vOskPrevCancel = false;
                return;
        }
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

// OPENRCT2MINI text-editing-de-hardcode: free-function accessor for
// the live TextComposition owned by UiContext. Called from the action
// lambdas registered in Shortcuts.cpp for the new caret + clipboard
// shortcuts.
TextComposition& Ui::GetTextComposition()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    return uiContext.GetTextComposition();
}

// OPENRCT2MINI gamepad-plan 1.5c + hold-binding refactor: action
// bridges for the shade / close-window shortcuts. Invoked from action
// lambdas registered in Shortcuts.cpp::registerDefaultShortcuts().
// Shade-all fires on hold-elapsed of the bound input, shade-window
// fires on tap-release before the hold threshold — both driven by
// ShortcutManager's _holdPending mechanism rather than the per-frame
// poll that used to live in ProcessWorldCursor.
void Ui::FireToggleShadeAll()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    uiContext.ActionToggleShadeAll();
}

void Ui::FireCloseWindowUnderCursor()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    uiContext.ActionCloseWindowAndSuppressShade();
}

void Ui::FireShadeWindowUnderCursor()
{
    auto& uiContext = static_cast<UiContext&>(GetContext()->GetUiContext());
    uiContext.ActionShadeWindowUnderCursor();
}
