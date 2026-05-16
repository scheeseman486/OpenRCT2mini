/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../core/StringTypes.h"
#include "ConfigTypes.h"

#include <atomic>

// windows.h defines an interface keyword
#ifdef interface
    #undef interface
#endif

enum class RideInspection : uint8_t;

namespace OpenRCT2::Config
{
    struct General
    {
        // Paths
        u8string rct1Path;
        u8string rct2Path;

        // Display
        int32_t defaultDisplay;
        int32_t windowWidth;
        int32_t windowHeight;
        int32_t fullscreenMode;
        int32_t fullscreenWidth;
        int32_t fullscreenHeight;
        float windowScale;
        bool inferDisplayDPI;
        ::DrawingEngine drawingEngine;
        bool uncapFPS;
        bool useVSync;
        bool showFPS;
        std::atomic_uint8_t multiThreading;
        bool minimizeFullscreenFocusLoss;
        bool disableScreensaver;

        // Map rendering
        bool landscapeSmoothing;
        bool alwaysShowGridlines;
        VirtualFloorStyles virtualFloorStyle;
        bool dayNightCycle;
        bool enableLightFx;
        bool enableLightFxForVehicles;
        bool upperCaseBanners;
        bool renderWeatherEffects;
        bool renderWeatherGloom;
        bool disableLightningEffect;
        bool showGuestPurchases;
        bool transparentScreenshot;
        bool transparentWater;

        bool invisibleRides;
        bool invisibleVehicles;
        bool invisibleTrees;
        bool invisibleScenery;
        bool invisiblePaths;
        bool invisibleSupports;

        // Localisation
        int32_t language;
        MeasurementFormat measurementFormat;
        TemperatureUnit temperatureFormat;
        bool showHeightAsUnits;
        int32_t dateFormat;
        CurrencyType currencyFormat;
        int32_t customCurrencyRate;
        CurrencyAffix customCurrencyAffix;
        u8string customCurrencySymbol;

        // Controls
        bool edgeScrolling;
        int32_t edgeScrollingSpeed;
        bool trapCursor;
        bool invertViewportDrag;
        bool zoomToCursor;
        // OPENRCT2MINI focus-mode-plan / Phase F.3: when true, the
        // widget-focus context is implicitly active in every window that
        // contains at least one focusable widget — the user can D-pad
        // through buttons / checkboxes / dropdowns without first
        // pressing the kInterfaceEnterFocusMode shortcut. When false,
        // focus mode is entered explicitly on shortcut press and exits
        // on the kInterfaceDismiss binding. Defaults to true on the
        // handheld build because gamepad is the primary input. See
        // focus-mode-plan.md §F.3 and the Options Phase F.7 UI hookup.
        bool widgetFocusAlwaysOn;

        // OPENRCT2MINI host-restoration-plan Phase 0: runtime knobs that
        // let host developers reproduce Mini-style behaviour for a
        // single field without recompiling the binary. The compile-time
        // OPENRCT2MINI flag still takes precedence; these only matter
        // on host (OPENRCT2MINI undefined). Phase 0 scaffolds them;
        // later phases wire them into the actual cut sites.
        //
        // mapSizeOverride: 0 means use the compiled-in
        // kMaximumMapSizeTechnical default. Non-zero values clamp the
        // effective max map size for testing (e.g. set to 257 on a
        // host build to reproduce the Mini 256-tile cap behaviour
        // without flipping the compile flag).
        uint16_t mapSizeOverride;

        // disableMultiThreadedRendering: on Mini this defaults to true
        // (cut U2 force-off). On host it defaults to false (the
        // Options > Display checkbox is honoured). Either build can
        // flip it at runtime via config.ini if a developer needs to
        // reproduce the opposite behaviour for testing.
        bool disableMultiThreadedRendering;

        // Gamepad
        int32_t gamepadDeadzone;
        float gamepadSensitivity;
        // OPENRCT2MINI gamepad-plan 1.9: analog right-stick → camera
        // pan. CameraDeadzone is the stick deflection magnitude
        // (0..32767) below which input is ignored — defaults to 8000
        // (~24%) to filter out resting drift on worn sticks.
        // CameraSensitivity is a linear multiplier on the resulting
        // pan velocity. InvertCameraY swaps the vertical axis sign,
        // for users who prefer flight-sim-style "up = pitch back".
        int32_t gamepadCameraDeadzone;
        float gamepadCameraSensitivity;
        bool gamepadInvertCameraY;
        // OPENRCT2MINI gamepad-plan 1.11: haptic (rumble) global gates.
        // gamepadRumbleEnabled (default true) is the master kill-switch
        // — when off, every UiContext::RumbleControllers call is a
        // silent no-op even with a rumble-capable pad plugged in.
        // gamepadRumbleIntensity (default 1.0) is a 0.0–1.0 multiplier
        // applied to both motor magnitudes before SDL submission, so
        // users can soften the felt strength without re-authoring per-
        // event profiles. Phase 1.11b layers a per-SoundId mapping
        // and editor on top; these two knobs remain the global gate.
        bool gamepadRumbleEnabled;
        float gamepadRumbleIntensity;
        // OPENRCT2MINI gamepad-plan 1.13: DualShock-style LED indicator
        // global gates. gamepadLedEnabled (default true) is the master
        // kill-switch — when off, UiContext::SetControllerLED is a
        // silent no-op even with a lightbar-capable pad plugged in,
        // and the Led::tickEngine fade is short-circuited.
        // gamepadLedBrightness (default 0.5) is a 0.0–1.0 multiplier
        // applied to each colour channel before SDL submission, so
        // users can dim the bright DualShock 4/5 lightbar without
        // re-authoring per-severity colours. 0.5 chosen because at
        // full strength a DS4 lightbar is uncomfortably bright in
        // dim rooms; 0.5 is still visible at arm's length.
        bool gamepadLedEnabled;
        float gamepadLedBrightness;
        // OPENRCT2MINI input-plan Track 2 §4.3: per-event haptic and
        // LED gates. Sit alongside the master enable / intensity
        // knobs. Each defaults to true so the v0.2 behaviour is
        // preserved on first launch; users who find a particular
        // event annoying (e.g. the critical-news rumble pulse) can
        // disable it without nuking the entire haptic / LED system
        // and without diving into the per-SoundId Rumble Editor.
        // Crash and ConstructionRefusal route through SoundId-mapped
        // envelopes (SoundId::crash and SoundId::error respectively);
        // the gate lives in Haptic::onSoundPlayed. CriticalNews fires
        // directly via Haptic::pulse from NewsItem::TickCurrent; the
        // gate lives at the call site. LED toggles gate per-severity
        // at the same TickCurrent call site that fires Led::setActive,
        // so the Test LED button in the LED Options window is
        // unaffected.
        bool gamepadRumbleOnCrash;
        bool gamepadRumbleOnCriticalNews;
        bool gamepadRumbleOnConstructionRefusal;
        bool gamepadLedOnCritical;
        bool gamepadLedOnWarning;
        bool gamepadLedOnMoney;

        // Miscellaneous
        bool playIntro;
        int32_t windowSnapProximity;
        bool savePluginData;
        bool debuggingTools;
        int32_t autosaveFrequency;
        int32_t autosaveAmount;
        bool autoStaffPlacement;
        bool handymenMowByDefault;
        bool autoOpenShops;
        RideInspection defaultInspectionInterval;
        int32_t windowLimit;
        bool scenarioUnlockingEnabled;
        bool scenarioHideMegaPark;
        bool showRealNamesOfGuests;
        bool showRealNamesOfStaff;
        bool allowEarlyCompletion;
        u8string assetPackOrder;
        u8string enabledAssetPacks;

        // Loading and saving
        bool confirmationPrompt;
        FileBrowserSort loadSaveSort;
        u8string lastSaveGameDirectory;
        u8string lastSaveLandscapeDirectory;
        u8string lastSaveScenarioDirectory;
        u8string lastSaveTrackDirectory;
        u8string lastRunVersion;
        bool useNativeBrowseDialog;
        int64_t lastVersionCheckTime;
        int16_t fileBrowserWidth;
        int16_t fileBrowserHeight;
        bool fileBrowserShowSizeColumn;
        bool fileBrowserShowDateColumn;
        ParkPreviewPref fileBrowserPreviewType;
    };

    // OPENRCT2MINI revision 59 / 61: cursor "themes". Persisted as a name
    // string in [interface] for forward-compatibility — adding new presets
    // won't shuffle int IDs out from under existing configs.
    //
    //   Classic       — original RCT2 look. pi10 outline + pi17 fill on mono
    //                   bitmaps; paletted full-gradient pointer.
    //   Default       — high-readability mono. pi0 (true black) outline +
    //                   pi255 (true white) fill on every bitmap. Uses the
    //                   project's hand-drawn HC pointer instead of the
    //                   paletted one.
    //   HighContrast  — inverted Default. pi255 outline + pi0 fill, same
    //                   bitmaps. Most aggressive readability against light
    //                   backgrounds.
    enum class CursorStyle : uint8_t
    {
        Classic = 0,
        Default = 1,
        HighContrast = 2,
        // OPENRCT2MINI revision 77: Windows-style theme. Same palette and
        // bitmaps as Default, except the main pointer is replaced with a
        // classic Win9x-style arrow (orct2mini_cursor_pointer_windows.png).
        Windows = 3,
    };

    struct Interface
    {
        bool toolbarButtonsCentred;
        bool toolbarShowFinances;
        bool toolbarShowResearch;
        bool toolbarShowCheats;
        bool toolbarShowNews;
        bool toolbarShowMute;
        bool toolbarShowChat;
        bool toolbarShowZoom;
        bool toolbarShowRotateAnticlockwise;
        bool consoleSmallFont;
        bool randomTitleSequence;
        // OPENRCT2MINI revision 67: when true, the title scene shows a static
        // empty park instead of loading any .parkseq. Mutually exclusive with
        // randomTitleSequence (the dropdown enforces it).
        bool noTitleSequence;
        u8string currentThemePreset;
        u8string currentTitleSequencePreset;
        int32_t objectSelectionFilterFlags;
        int32_t scenarioSelectLastTab;
        bool scenarioPreviewScreenshots;
        bool listRideVehiclesSeparately;
        bool windowButtonsOnTheLeft;
        bool enlargedUi;
        bool touchEnhancements;
        // OPENRCT2MINI osk-overhaul §8: when false, OSK is never
        // spawned (textboxes, TextInput modal, in-game console).
        // SDL_TEXTINPUT flows directly to TextComposition via the
        // hardware keyboard. Default true so device builds (Miyoo Mini)
        // see no regression; desktop users with a hardware keyboard
        // will likely want to disable it from Options > Controls.
        bool onScreenKeyboard;
        CursorStyle cursorStyle;
        // OPENRCT2MINI: optional 50%-translucent drop shadow under the
        // software cursor sprite, offset (+2, +2) pixels. Default off.
        bool cursorDropShadow;
        // OPENRCT2MINI grid-cursor-plan §3.4: D-pad-to-tile-step mapping
        // mode for the gamepad-driven grid cursor that tool contexts
        // use. compass (default) keeps the WORLD direction constant
        // under camera rotation (D-pad up = world north regardless of
        // rotation). diagonalLeft / diagonalRight keep the SCREEN
        // direction constant (D-pad up always moves visually upward,
        // biased NW or NE respectively). Stored as a uint8_t-backed
        // enum; INI key `grid_cursor_mode` under [interface].
        uint8_t gridCursorMode;
    };

    struct Sound
    {
        u8string device;
        bool masterSoundEnabled;
        uint8_t masterVolume;
        TitleMusicKind titleMusic;
        bool soundEnabled;
        uint8_t soundVolume;
        bool rideMusicEnabled;
        uint8_t rideMusicVolume;
        bool audioFocus;
    };

    struct Network
    {
        u8string playerName;
        int32_t defaultPort;
        u8string listenAddress;
        u8string defaultPassword;
        bool stayConnected;
        bool advertise;
        u8string advertiseAddress;
        int32_t maxplayers;
        u8string serverName;
        u8string serverDescription;
        u8string serverGreeting;
        u8string masterServerUrl;
        u8string providerName;
        u8string providerEmail;
        u8string providerWebsite;
        bool knownKeysOnly;
        bool logChat;
        bool logServerActions;
        bool pauseServerIfNoClients;
        bool desyncDebugging;
    };

    struct Notification
    {
        bool parkAward;
        bool parkMarketingCampaignFinished;
        bool parkWarnings;
        bool parkRatingWarnings;
        bool rideBrokenDown;
        bool rideCrashed;
        bool rideCasualties;
        bool rideWarnings;
        bool rideResearched;
        bool rideStalledVehicles;
        bool guestWarnings;
        bool guestLeftPark;
        bool guestQueuingForRide;
        bool guestOnRide;
        bool guestLeftRide;
        bool guestBoughtItem;
        bool guestUsedFacility;
        bool guestDied;
    };

    struct Font
    {
        u8string fileName;
        u8string fontName;
        int32_t offsetX;
        int32_t offsetY;
        int32_t sizeTiny;
        int32_t sizeSmall;
        int32_t sizeMedium;
        int32_t sizeBig;
        int32_t heightTiny;
        int32_t heightSmall;
        int32_t heightMedium;
        int32_t heightBig;
        bool enableHinting;
        int32_t hintingThreshold;
    };

    struct Plugin
    {
        bool enableHotReloading;
        u8string allowedHosts;
    };

    struct Config
    {
        Config() = default;

        // Prevent accidental copies
        Config(const Config&) = delete;

        General general;
        Interface interface;
        Sound sound;
        Network network;
        Notification notifications;
        Font fonts;
        Plugin plugin;
    };

    Config& Get();
    bool OpenFromPath(u8string_view path);
    bool SaveToPath(u8string_view path);
    u8string GetDefaultPath();
    bool SetDefaults();
    bool Save();
    bool FindOrBrowseInstallDirectory();
} // namespace OpenRCT2::Config
