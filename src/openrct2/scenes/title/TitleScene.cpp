/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TitleScene.h"

#include <cstdio>

#include "../../Context.h"
#include "../../Diagnostic.h"
#include "../../MiniDebug.h"  // OPENRCT2MINI revision 64 — gated debug logging
#include "../../Game.h"
#include "../../GameState.h"
#include "../../Input.h"
#include "../../OpenRCT2.h"
#include "../../audio/Audio.h"
#include "../../config/Config.h"
#include "../../core/Console.hpp"
#include "../../drawing/Drawing.h"
#include "../../drawing/SpriteScratch.h"
#include "../../drawing/Text.h"
#include "../../interface/Screenshot.h"
#include "../../interface/Viewport.h"          // OPENRCT2MINI revision 68: viewport pan
#include "../../interface/Window.h"            // OPENRCT2MINI revision 68: WindowGetMain / Viewport
#include "../../interface/WindowBase.h"        // OPENRCT2MINI revision 68: full WindowBase
#include "../../localisation/LocalisationService.h"  // OPENRCT2MINI revision 68: object repo build
#include "../../network/Network.h"
#include "../../network/NetworkBase.h"
#include "../../object/DefaultObjects.h"       // OPENRCT2MINI revision 68: kMinimumRequiredObjects
#include "../../object/ObjectManager.h"        // OPENRCT2MINI revision 68: empty-park objects
#include "../../object/ObjectRepository.h"     // OPENRCT2MINI revision 68: object repo build
#include "../../scenario/ScenarioRepository.h"
#include "../../ui/UiContext.h"
#include "../../ui/WindowManager.h"
#include "../../util/Util.h"
#include "../../windows/Intent.h"
#include "../../world/Map.h"
#include "TitleSequence.h"
#include "TitleSequenceManager.h"
#include "TitleSequencePlayer.h"

using namespace OpenRCT2;

// TODO Remove when no longer required.
bool gPreviewingTitleSequenceInGame;

ITitleSequencePlayer* TitleScene::GetSequencePlayer()
{
    return _sequencePlayer;
}

size_t TitleScene::GetCurrentSequence()
{
    return _currentSequence;
}

bool TitleScene::PreviewSequence(size_t value)
{
    _currentSequence = value;
    _previewingSequence = TryLoadSequence(true);
    if (_previewingSequence)
    {
        if (gLegacyScene != LegacyScene::titleSequence)
        {
            gPreviewingTitleSequenceInGame = true;
        }
    }
    else
    {
        _currentSequence = TitleGetConfigSequence();
        if (gLegacyScene == LegacyScene::titleSequence)
        {
            TryLoadSequence();
        }
    }
    return _previewingSequence;
}

void TitleScene::StopPreviewingSequence()
{
    if (_previewingSequence)
    {
        WindowBase* mainWindow = WindowGetMain();
        if (mainWindow != nullptr)
        {
            WindowUnfollowSprite(*mainWindow);
        }
        _previewingSequence = false;
        _currentSequence = TitleGetConfigSequence();
        gPreviewingTitleSequenceInGame = false;
    }
}

bool TitleScene::IsPreviewingSequence()
{
    return _previewingSequence;
}

void TitleScene::Load()
{
    LOG_VERBOSE("TitleScene::Load()");
    MINI_DBG_PUTS("  title: Load() entered");

    if (GameIsPaused())
    {
        PauseToggle();
    }

    gLegacyScene = LegacyScene::titleSequence;
    gScreenAge = 0;
    gCurrentLoadedPath.clear();

#ifndef DISABLE_NETWORK
    GetContext().GetNetwork().Close();
#endif
    gameStateInitAll(getGameState(), kDefaultMapSize);
    MINI_DBG_PUTS("  title: gameStateInitAll done");
    ContextResetSubsystems();
    MINI_DBG_PUTS("  title: ContextResetSubsystems done");
    ContextOpenWindow(WindowClass::mainWindow);
    MINI_DBG_PUTS("  title: mainWindow open");

    TitleInitialise();
    MINI_DBG_PUTS("  title: TitleInitialise done");

    if (_sequencePlayer != nullptr)
    {
        MINI_DBG_PUTS("  title: TryLoadSequence start");
        // Force the title sequence to load / update so we
        // don't see a blank screen for a split second.
        _loadedTitleSequenceId = SIZE_MAX;
        TryLoadSequence();
        MINI_DBG_PUTS("  title: TryLoadSequence done, calling Update");
        _sequencePlayer->Update();
        MINI_DBG_PUTS("  title: sequencePlayer->Update done");
    }

    Audio::PlayTitleMusic();

    CreateWindows();
    MINI_DBG_PUTS("  title: CreateWindows done");

    if (gOpenRCT2ShowChangelog)
    {
        gOpenRCT2ShowChangelog = false;
        ContextOpenWindow(WindowClass::changelog);
    }

    // OPENRCT2MINI revision 70d: deferred park-load error drain moved to
    // TitleScene::Tick. The cmdline-load failure path causes TWO TitleScene
    // ::Load runs in close succession — once from the LoadParkFromStream
    // catch arm's SetActiveScene (rev 70d, needed to reset corrupted state),
    // then again from SwitchToStartUpScene's natural fallback at
    // Context.cpp:1324. The second Load's CloseAllExceptFlags clobbers any
    // dialog opened by the first. Tick runs on a stable frame after both
    // transitions have settled, so the dialog stays put.

    LOG_VERBOSE("TitleScene::Load() finished");
    MINI_DBG_PUTS("  title: Load() finished");
}

void TitleScene::Tick()
{
    // OPENRCT2MINI revision 41 / 64. Per-tick subsystem tag for SIGSEGV
    // post-mortem — gated behind OPENRCT2MINI_DEBUG.
    static uint32_t s_titleTick = 0;
    auto ttick = []([[maybe_unused]] const char* tag) {
        MINI_DBG_LOG("  title.tick: %s\n", tag);
    };
    bool logThisTick = (s_titleTick % 60) == 0;
    s_titleTick++;

    // OPENRCT2MINI revision 70d: drain any pending park-load error here,
    // before any other Tick work. By the time we reach the first stable
    // Tick after a failed load, all scene transitions have run and any
    // CloseAllExceptFlags cleanup has finished. Opening the error dialog
    // here means it survives. Mirrors the gOpenRCT2ShowChangelog pattern
    // (which is checked in Load — but the error has the extra constraint
    // of needing to outlast a possible double Load when SwitchToStartUpScene
    // and the catch arm both fire SetActiveScene).
    static StringId gOpenRCT2PendingParkLoadError = kStringIdNone;
    if (gOpenRCT2PendingParkLoadError != kStringIdNone)
    {
        auto pendingTitle = gOpenRCT2PendingParkLoadError;
        gOpenRCT2PendingParkLoadError = kStringIdNone;
        ContextShowError(pendingTitle, kStringIdNone, {});
    }

    gInUpdateCode = true;

    // OPENRCT2MINI: opportunistically reclaim sprite-scratch pages once any load phase
    // (Object index build, asset packs, audio object load, scenario load via title
    // sequence) has produced new appends. Lock-free no-op when nothing was appended.
    if (logThisTick) ttick("SpriteScratchEvictIfIdle");
    OpenRCT2::Drawing::SpriteScratchEvictIfIdle();

    if (logThisTick) ttick("ScreenshotCheck/HandleKeyboardInput");
    ScreenshotCheck();
    TitleHandleKeyboardInput();

    if (!gOpenRCT2Headless && GameIsNotPaused())
    {
        if (logThisTick) ttick("TryLoadSequence");
        TryLoadSequence();
        if (logThisTick) ttick("sequencePlayer->Update");
        _sequencePlayer->Update();

        int32_t numUpdates = 1;
        if (gGameSpeed > 1)
        {
            numUpdates = 1 << (gGameSpeed - 1);
        }
        if (logThisTick) ttick("gameStateUpdateLogic");
        for (int32_t i = 0; i < numUpdates; i++)
        {
            gameStateUpdateLogic();
        }
        if (logThisTick) ttick("UpdatePaletteEffects");
        UpdatePaletteEffects();
        // update_weather_animation();

        // OPENRCT2MINI revision 68 (revised): in (none) mode the empty park
        // is set up once by TryLoadSequence (objects loaded, gameStateInitAll'd,
        // savedViewPos captured at map centre). Each tick we apply a small
        // screen-X offset relative to that captured anchor and wrap modulo a
        // loop length that fits inside the visually-uniform interior. Wraps
        // are visually invisible because every tile in the loop region is
        // identical grass — the user perceives an infinite plane drifting
        // past. Critically: we do *not* re-anchor each tick. Re-anchoring
        // would race against ViewportMove's own savedViewPos sync (jitter)
        // and any drift past the playable bounds would invoke the
        // edge-clamp at Viewport.cpp:535-559 which is what makes the camera
        // "follow the boundary".
        if (Config::Get().interface.noTitleSequence && _noSequencePanAnchored)
        {
            if (logThisTick) ttick("noTitleSequence pan");
            auto* mainWindow = WindowGetMain();
            if (mainWindow != nullptr && mainWindow->viewport != nullptr)
            {
                // Loop length 256 px ≈ 8 isometric-x tiles; with a
                // map-centre anchor on a 150x150 map we have ~70 tiles of
                // grass in every direction so a ±128 px excursion stays
                // safely inside the playable area and the
                // ViewportUpdatePosition edge-clamp never fires.
                constexpr int32_t kPanPxPerTick = 1;     // ~40 px/sec at 40 ticks/sec
                constexpr int32_t kLoopWidthPx = 256;    // ~6.4 s per loop
                _noSequencePanTick++;
                // Centre the loop on the anchor: oscillate over
                // [anchor - kLoopWidthPx/2, anchor + kLoopWidthPx/2 - 1].
                int32_t offsetPx = (_noSequencePanTick * kPanPxPerTick) % kLoopWidthPx;
                offsetPx -= kLoopWidthPx / 2;
                mainWindow->savedViewPos.x = _noSequencePanAnchor.x + offsetPx;
                mainWindow->savedViewPos.y = _noSequencePanAnchor.y;
                mainWindow->viewport->viewPos = mainWindow->savedViewPos;
                mainWindow->invalidate();
            }
        }
    }

    gInputFlags.unset(InputFlag::viewportScrolling);

    if (logThisTick) ttick("ContextHandleInput");
    ContextHandleInput();

    gInUpdateCode = false;
    if (logThisTick) ttick("done");
}

void TitleScene::Stop()
{
    Audio::StopAll();
}

void TitleScene::ChangePresetSequence(size_t preset)
{
    size_t count = TitleSequenceManager::GetCount();
    if (preset >= count)
    {
        return;
    }

    const utf8* configId = TitleSequenceManager::GetConfigID(preset);
    Config::Get().interface.currentTitleSequencePreset = configId;

    if (!_previewingSequence)
        _currentSequence = preset;

    auto* windowMgr = Ui::GetWindowManager();
    windowMgr->InvalidateAll();
}

/**
 * Creates the windows shown on the title screen; New game, load game,
 * tutorial, toolbox and exit.
 */
void TitleScene::CreateWindows()
{
    ContextOpenWindow(WindowClass::titleMenu);
    ContextOpenWindow(WindowClass::titleExit);
    ContextOpenWindow(WindowClass::titleOptions);
    ContextOpenWindow(WindowClass::titleLogo);
    ContextOpenWindow(WindowClass::titleVersion);
    WindowResizeGui(ContextGetWidth(), ContextGetHeight());
}

void TitleScene::TitleInitialise()
{
    if (gOpenRCT2Headless)
    {
        return;
    }
    if (_sequencePlayer == nullptr)
    {
        _sequencePlayer = GetContext().GetUiContext().GetTitleSequencePlayer();
    }
    if (Config::Get().interface.randomTitleSequence)
    {
        const size_t total = TitleSequenceManager::GetCount();
        if (total > 0)
        {
            bool RCT1Installed = false, RCT1AAInstalled = false, RCT1LLInstalled = false;
            uint32_t RCT1Count = 0;
            const size_t scenarioCount = ScenarioRepositoryGetCount();

            for (size_t s = 0; s < scenarioCount; s++)
            {
                const ScenarioSource sourceGame = ScenarioRepositoryGetByIndex(s)->SourceGame;
                switch (sourceGame)
                {
                    case ScenarioSource::RCT1:
                        RCT1Count++;
                        break;
                    case ScenarioSource::RCT1_AA:
                        RCT1AAInstalled = true;
                        break;
                    case ScenarioSource::RCT1_LL:
                        RCT1LLInstalled = true;
                        break;
                    default:
                        break;
                }
            }

            // Mega Park can show up in the scenario list even if RCT1 has been uninstalled, so it must be greater than 1
            RCT1Installed = RCT1Count > 1;

            int32_t random = 0;
            bool safeSequence = false;
            const std::string RCT1String = LanguageGetString(STR_TITLE_SEQUENCE_RCT1);
            const std::string RCT1AAString = LanguageGetString(STR_TITLE_SEQUENCE_RCT1_AA);
            const std::string RCT1LLString = LanguageGetString(STR_TITLE_SEQUENCE_RCT1_AA_LL);

            // Ensure the random sequence chosen isn't from RCT1 or expansion if the player doesn't have it installed
            while (!safeSequence)
            {
                random = UtilRand() % static_cast<int32_t>(total);
                const utf8* scName = TitleSequenceManager::GetName(random);
                if (scName == RCT1String)
                {
                    safeSequence = RCT1Installed;
                }
                else if (scName == RCT1AAString)
                {
                    safeSequence = RCT1AAInstalled;
                }
                else if (scName == RCT1LLString)
                {
                    safeSequence = RCT1LLInstalled;
                }
                else
                {
                    safeSequence = true;
                }
            }
            ChangePresetSequence(random);
        }
    }
    size_t seqId = TitleGetConfigSequence();
    if (seqId == SIZE_MAX)
    {
        seqId = TitleSequenceManager::GetIndexForConfigID("*OPENRCT2");
        if (seqId == SIZE_MAX)
        {
            seqId = 0;
        }
    }
    ChangePresetSequence(static_cast<int32_t>(seqId));
}

bool TitleScene::TryLoadSequence(bool loadPreview)
{
    // OPENRCT2MINI revision 67: explicit "no title sequence" mode. Render a
    // static empty park as the title backdrop instead of running any .parkseq.
    // The previous behaviour — falling through to gameStateInitAll only when
    // EVERY sequence failed to load — meant users couldn't ask for the empty
    // backdrop without uninstalling all their sequences. This branch surfaces
    // it as a first-class option. Skipped when previewing so the preview path
    // stays operative regardless of the live setting.
    if (!loadPreview && Config::Get().interface.noTitleSequence)
    {
        // First entry / state change into no-sequence mode: eject any
        // currently-loaded sequence, reload the minimum required default
        // objects (so the renderer can resolve slot-0 surface/edge sprites),
        // and re-init the empty park. After that, re-entry is a near-no-op
        // (both ids are SIZE_MAX) and the per-tick pan in Tick() takes over.
        if (_loadedTitleSequenceId != SIZE_MAX || _currentSequence != SIZE_MAX)
        {
            if (_sequencePlayer != nullptr)
            {
                _sequencePlayer->Eject();
            }
            _currentSequence = SIZE_MAX;
            _loadedTitleSequenceId = SIZE_MAX;

            // OPENRCT2MINI revision 68: pattern lifted from Editor::Load /
            // Editor::ObjectListLoad. After UnloadAll the slot table is empty;
            // loading kMinimumRequiredObjects (grass surface, rock edge,
            // plain station) in order claims slot 0 of each ObjectType, which
            // is what GetDefaultSurfaceElement() references. Without this,
            // every tile would reference an unloaded slot-0 surface object
            // and the viewport would render garbage.
            auto& context = GetContext();
            auto& objectManager = context.GetObjectManager();
            objectManager.UnloadAll();
            // LoadOrConstruct is idempotent — the preloader scene built the
            // index already, so this is a near-no-op on the title-screen
            // path. Cheap to call and safer if a future revision lets the
            // user wipe the index.
            auto& objectRepository = context.GetObjectRepository();
            objectRepository.LoadOrConstruct(context.GetLocalisationService().GetCurrentLanguage());
            Audio::LoadAudioObjects();
            for (const auto& entry : kMinimumRequiredObjects)
            {
                objectManager.LoadObject(entry);
            }

            gameStateInitAll(getGameState(), kDefaultMapSize);
            GameNotifyMapChanged();

            // OPENRCT2MINI revision 68 (revised): anchor the camera at the
            // map *centre* so the pan stays well inside the visually-uniform
            // grass interior. Anchoring at an edge tile (the original 1, 75)
            // put the void boundary in frame from frame zero AND, once the
            // pan offset accumulated past the playable bounds, tripped
            // ViewportUpdatePosition's edge-clamp at Viewport.cpp:535-559 —
            // which is what makes the camera appear to "follow the boundary".
            // Capture the savedViewPos that setViewportLocation produces;
            // re-deriving it every tick would jitter against ViewportMove's
            // own savedViewPos sync.
            auto* mainWindow = WindowGetMain();
            if (mainWindow != nullptr)
            {
                auto loc = TileCoordsXY{ kDefaultMapSize.x / 2, kDefaultMapSize.y / 2 }
                               .ToCoordsXY().ToTileCentre();
                int32_t z = TileElementHeight(loc);
                auto oldLegacyScene = gLegacyScene;
                gLegacyScene = LegacyScene::titleSequence;
                mainWindow->setViewportLocation({ loc, z });
                gLegacyScene = oldLegacyScene;
                _noSequencePanAnchor = mainWindow->savedViewPos;
                _noSequencePanAnchored = true;
            }
            else
            {
                _noSequencePanAnchored = false;
            }
            _noSequencePanTick = 0;

            GfxInvalidateScreen();
        }
        return true;
    }
    else if (_noSequencePanAnchored && !Config::Get().interface.noTitleSequence)
    {
        // Toggled out of (none) mode: forget the anchor so re-entering the
        // mode re-captures it (window may have been resized, drawing engine
        // swapped, etc).
        _noSequencePanAnchored = false;
    }

    if (!gOpenRCT2Headless && (_loadedTitleSequenceId != _currentSequence || loadPreview))
    {
        if (_sequencePlayer == nullptr)
        {
            _sequencePlayer = GetContext().GetUiContext().GetTitleSequencePlayer();
        }

        size_t numSequences = TitleSequenceManager::GetCount();
        if (numSequences > 0)
        {
            size_t targetSequence = _currentSequence;
            do
            {
                if (_sequencePlayer->Begin(targetSequence) && _sequencePlayer->Update())
                {
                    _loadedTitleSequenceId = targetSequence;
                    if (targetSequence != _currentSequence && !loadPreview)
                    {
                        // Forcefully change the preset to a preset that works.
                        const utf8* configId = TitleSequenceManager::GetConfigID(targetSequence);
                        Config::Get().interface.currentTitleSequencePreset = configId;
                    }
                    _currentSequence = targetSequence;
                    GfxInvalidateScreen();
                    return true;
                }
                targetSequence = (targetSequence + 1) % numSequences;
            } while (targetSequence != _currentSequence && !loadPreview);
        }
        Console::Error::WriteLine("Unable to play any title sequences.");
        _sequencePlayer->Eject();
        _currentSequence = SIZE_MAX;
        _loadedTitleSequenceId = SIZE_MAX;
        if (!loadPreview)
        {
            gameStateInitAll(getGameState(), kDefaultMapSize);
            GameNotifyMapChanged();
        }
        return false;
    }
    return true;
}

void TitleCreateWindows()
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        titleScene->CreateWindows();
    }
}

void* TitleGetSequencePlayer()
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        return titleScene->GetSequencePlayer();
    }
    return nullptr;
}

void TitleSequenceChangePreset(size_t preset)
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        titleScene->ChangePresetSequence(preset);
    }
}

size_t TitleGetConfigSequence()
{
    return TitleSequenceManager::GetIndexForConfigID(Config::Get().interface.currentTitleSequencePreset.c_str());
}

size_t TitleGetCurrentSequence()
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        return titleScene->GetCurrentSequence();
    }
    return 0;
}

bool TitlePreviewSequence(size_t value)
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        return titleScene->PreviewSequence(value);
    }
    return false;
}

void TitleStopPreviewingSequence()
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        titleScene->StopPreviewingSequence();
    }
}

bool TitleIsPreviewingSequence()
{
    auto* context = GetContext();
    auto* titleScene = static_cast<TitleScene*>(context->GetTitleScene());
    if (titleScene != nullptr)
    {
        return titleScene->IsPreviewingSequence();
    }
    return false;
}
