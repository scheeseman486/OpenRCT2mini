/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "PreloaderScene.h"

#include "../../Context.h"
#include "../../Diagnostic.h"
#include "../../Game.h"
#include "../../GameState.h"
#include "../../OpenRCT2.h"
#include "../../audio/Audio.h"
#include "../../interface/Viewport.h"
#include "../../localisation/StringIds.h"
#include "../../ui/WindowManager.h"
#include "../../windows/Intent.h"
#include "../../world/Map.h"

#include <cstdio>
#include <sstream>

using namespace OpenRCT2;

PreloaderScene::PreloaderScene(IContext& context)
    : Scene(context)
    , _jobs(1)
{
}

void PreloaderScene::Load()
{
    LOG_VERBOSE("PreloaderScene::Load()");

    gLegacyScene = LegacyScene::playing;
    gameStateInitAll(getGameState(), kDefaultMapSize);
    ContextResetSubsystems();
    ContextOpenWindow(WindowClass::mainWindow);
    WindowSetFlagForAllViewports(VIEWPORT_FLAG_RENDERING_INHIBITED, true);
    WindowResizeGui(ContextGetWidth(), ContextGetHeight());

    LOG_VERBOSE("PreloaderScene::Load() finished");
}

void PreloaderScene::Tick()
{
    gInUpdateCode = true;

    // Avoid race condition with background jobs modifying gWindowList.
    const bool jobsRunning = _jobs.IsBusy();

    // OPENRCT2MINI cut 40g: tick-progress log so we can tell whether the
    // preloader is making progress or stuck. Print roughly every second
    // (60 ticks). On a slow SD card the object-scan job can take 30-60s.
    static uint32_t s_tickCount = 0;
    if ((s_tickCount % 60) == 0)
    {
        std::fprintf(stderr, "[OPENRCT2MINI] preloader tick #%u jobsBusy=%d\n",
                     s_tickCount, jobsRunning ? 1 : 0);
        std::fflush(stderr);
    }
    s_tickCount++;

    if (!jobsRunning)
    {
        ContextHandleInput();
        auto* windowMgr = Ui::GetWindowManager();
        windowMgr->InvalidateAll();
    }

    gInUpdateCode = false;

    if (!jobsRunning)
    {
        // Make sure the job is fully completed.
        _jobs.Join();

        std::fprintf(stderr, "[OPENRCT2MINI] preloader: jobs done — calling FinishScene\n");
        std::fflush(stderr);
        FinishScene();
    }
}

void PreloaderScene::Stop()
{
    Audio::StopAll();
}
