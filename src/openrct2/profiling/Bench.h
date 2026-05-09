/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

// OPENRCT2MINI: timedemo-style benchmark.
//
// Runs the existing title-sequence camera path with a forced one-tick-
// per-frame schedule and uncapped framerate, counting frames over a
// fixed frame budget (Quake-style — frame-count target, not time-budget,
// so device slowdowns don't truncate the benchmark and produce
// misleading "good" numbers). At completion, prints frame stats to the
// console / stderr.
//
// Flow (rev 95c — runs from anywhere):
//   1. User types `bench` from anywhere in the game (title screen,
//      in-park, editor, etc.).
//   2. Console-command handler calls Bench::start() and Hide()s the
//      console.
//   3. Bench::start() force-switches the active scene to the title
//      scene. SetActiveScene calls Stop() then Load(); TitleScene::Load
//      fully reinitialises the world (gameStateInitAll) and re-Begin()s
//      the title sequence player, so its command stream restarts from
//      position 0. This is what makes "start from the same point every
//      time" possible.
//   4. The next 3000 frames (default) run the title-sequence demo with
//      the bench-active flag set, so RunFixedFrame skips the framerate-
//      cap Sleep and forces 1 Tick per frame.
//   5. After targetFrames frames, Bench::onFrameEnd() builds the
//      report, writes a detailed per-frame log to <user>/bench/, and
//      flips the report-pending flag. randomTitleSequence /
//      noTitleSequence config values are restored to their pre-bench
//      state.
//   6. InGameConsole::Update() polls hasPendingReport() once per frame.
//      When set: Open() the console, WriteLine() the summary, mark the
//      report consumed.
//
// Determinism strategy:
//   - Frame count target, not wall-clock time → identical workload
//     across runs regardless of device perf.
//   - SetActiveScene → TitleScene::Load reinitialises world state and
//     restarts the sequence command stream from position 0.
//   - One Tick per Frame, accumulator forced to zero → identical game-
//     state evolution regardless of how long each frame took to render.
//   - RNG seed reset at bench start → peeps in the loaded title park
//     behave identically across runs.
//   - randomTitleSequence forced off for the duration of the run →
//     same sequence loads each time even if the user enabled the
//     "Random sequence each load" option.
//   - noTitleSequence forced off → we always run the demo, not the
//     empty-park fallback pan.
//   - Title sequence is a deterministic command stream (LOAD / WAIT /
//     LOCATION / ZOOM / ROTATE) — same camera path each run.
//   - 60-frame warm-up before timing starts → eliminates the cold-
//     cache outlier on the first frame (filesystem page cache, sprite
//     decode cache, CPU L1i, allocator state). Quake/Doom-style
//     timedemo pattern: warm-up frames run under the same determinism
//     gates but are not recorded.
//   - gGameSpeed pinned to 1 → bench's early frames don't inherit a
//     leftover SetSpeed value (from a prior sequence run, or from the
//     cheats menu). gameStateInitAll doesn't reset gGameSpeed, so we
//     have to do it here.
//
// Console: `bench [frames=3000]`
//
// Summary line (stdout + log + reopened console):
//   bench: 3000 frames, 37.0 sec wall, 81.0 fps avg, median 12.0 ms,
//          p95 18.3 ms, max 45.2 ms
//
// Detailed log: <user>/bench/bench-NNNN-<sha>.txt — header line plus
//   one CSV row per frame: frame_index,duration_us.

#include <cstdint>
#include <string>

namespace OpenRCT2::Profiling::Bench
{
    // Kick off a benchmark run for `targetFrames` frames. Returns false
    // if a benchmark is already running. Always switches the active
    // scene to the title scene and restarts the title-sequence command
    // stream — works from any scene. Default 3000 frames at the Mini's
    // ~5-10 fps is a 5-10 minute capture — long enough that even
    // sub-1% trends across runs become visible, short enough not to
    // test user patience.
    //
    // WARNING: discards any unsaved park / editor state because the
    // scene switch goes through TitleScene::Load → gameStateInitAll.
    // Console help text warns the user.
    bool start(uint32_t targetFrames);

    // Called from Context::RunFrame at end-of-frame with the wallclock
    // duration of the frame in microseconds. Increments the frame
    // counter and stops the benchmark when the target is reached.
    void onFrameEnd(uint32_t frameDurationUs);

    // True while a benchmark run is in progress. Hot path checks this
    // to bypass the framerate-cap sleep and force one-tick-per-frame.
    bool isActive();

    // True from when onFrameEnd hits the target until the consumer
    // (InGameConsole::Update) calls markReportConsumed(). Used to drive
    // the auto-reopen-console-and-print flow without coupling the
    // libopenrct2 layer directly to the UI.
    bool hasPendingReport();
    void markReportConsumed();

    // Returns the most recent benchmark's report line (empty string if
    // none). Stable for the lifetime of the process after the first run.
    const std::string& getLastReport();

    // Path of the most recent detailed CSV log written by onFrameEnd.
    // Empty if no run has completed.
    const std::string& getLastLogPath();
} // namespace OpenRCT2::Profiling::Bench
