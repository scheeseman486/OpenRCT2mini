/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI: timedemo-style benchmark.
//
// Implementation notes — see Bench.h for the public-API rationale.
//
// State model (rev 95b):
//   _active = true while collecting frames.
//   _reportPending = true once the run completes; cleared by
//     markReportConsumed() when InGameConsole::Update has displayed it.
//   _lastReport / _lastLogPath are stable reads after the first run.
//
// Determinism enforcement points (all live in Context::RunFixedFrame —
// see the BENCH-hooks block there):
//   1. Bypass the accumulator-too-low Sleep — uncap framerate.
//   2. Force exactly one Tick() per frame regardless of how long the
//      previous frame took. This drops the variable-deltaTime → variable-
//      ticks-per-frame coupling that would otherwise cause RNG-affecting
//      behaviour (more ticks → more peep behaviour decisions → different
//      camera scroll triggers in the title sequence).
//   3. RNG seed reset at start() — same scenario.cpp seed every run.
//
// On completion:
//   - Build the one-line report from per-frame timings.
//   - Write a detailed CSV log to <user>/bench/bench-NNNN-<sha>.txt.
//   - Print summary to stdout + LOG_INFO.
//   - Set _reportPending so InGameConsole::Update reopens itself and
//     displays the result on the next frame.

#include "Bench.h"

#include "../Context.h"
#include "../Diagnostic.h"
#include "../Game.h"
#include "../GameState.h"
#include "../OpenRCT2.h"
#include "../PlatformEnvironment.h"
#include "../Version.h"
#include "../config/Config.h"
#include "../core/Console.hpp"
#include "../core/Path.hpp"
#include "../drawing/NewDrawing.h"
#include "../scenes/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace OpenRCT2::Profiling::Bench
{
    namespace
    {
        // Hot-path gate. Read once per frame from RunFixedFrame; relaxed
        // is fine — start() and onFrameEnd() run on the same thread (the
        // main thread) so we don't actually need cross-thread visibility.
        // Atomic just keeps the read cheap and clearly-typed.
        std::atomic<bool> _active{ false };

        // Latched true when onFrameEnd hits the target. InGameConsole::
        // Update polls this and, when set, opens itself + writes the
        // summary + calls markReportConsumed.
        std::atomic<bool> _reportPending{ false };

        // Captured at start(); written at end-of-frame so report() can
        // compute total wall time.
        std::chrono::steady_clock::time_point _startTime;
        std::chrono::steady_clock::time_point _endTime;

        uint32_t _targetFrames = 0;
        uint32_t _completedFrames = 0;

        // Warm-up frames before timing begins. The first frame after a
        // scene reload pays page-cache, sprite-decode, and CPU-cache
        // cold-start costs — observed at ~5 ms vs ~1 ms steady-state on
        // host. Subsequent frames are at steady state. Run a fixed
        // number of "scaffolding" frames under bench's determinism
        // gates (1 tick/frame, no vsync, no input) but don't record
        // their durations and don't start the wall clock until they're
        // done. Counted-down from kBenchWarmupFrames at start(); when
        // it hits zero we capture _startTime and begin recording.
        //
        // 60 frames at the device's typical ~10 fps under bench load
        // costs ~6 sec of wall time once; on the host where we hit
        // ~1000 fps it costs ~60 ms. Either way it's a one-time cost
        // that fully eliminates the frame-0 outlier seen in 3-run
        // variance comparisons.
        constexpr uint32_t kBenchWarmupFrames = 60;
        uint32_t _warmupRemaining = 0;

        // Per-frame duration ring. Reserved up to _targetFrames at start
        // so onFrameEnd never re-allocates. Cleared at next start().
        std::vector<uint32_t> _frameDurations;

        // Cached output line + log path. Persisted across calls so
        // getLastReport() / getLastLogPath() can be polled later.
        std::string _lastReport;
        std::string _lastLogPath;

        // RNG seed used at bench start. Fixed value — any constant works,
        // we just need it to be the same across runs. 0xBE0CC0DE picked
        // for the lulz; nothing depends on it being a specific number.
        constexpr uint32_t kBenchRngSeed = 0xBE0CC0DEu;

        // Saved Config values that bench overrides for determinism.
        // Restored when the run completes (or on early termination via
        // a future stop()). Default-init so that if onFrameEnd somehow
        // fires without a paired start, restore is a no-op write.
        bool _savedRandomTitleSequence = false;
        bool _savedNoTitleSequence = false;
        bool _configWasOverridden = false;

        // Force the deterministic-scene-driver config and stash the
        // user's prior values for restore. Idempotent — if start() is
        // called twice without a paired restore (shouldn't happen but
        // defensive), the second call won't overwrite the first
        // stashed values.
        void overrideConfigForBench()
        {
            if (_configWasOverridden)
                return;
            auto& iface = Config::Get().interface;
            _savedRandomTitleSequence = iface.randomTitleSequence;
            _savedNoTitleSequence = iface.noTitleSequence;
            iface.randomTitleSequence = false;
            iface.noTitleSequence = false;
            _configWasOverridden = true;
        }

        void restoreConfigAfterBench()
        {
            if (!_configWasOverridden)
                return;
            auto& iface = Config::Get().interface;
            iface.randomTitleSequence = _savedRandomTitleSequence;
            iface.noTitleSequence = _savedNoTitleSequence;
            _configWasOverridden = false;
        }

        // Build the report line from the recorded frame-duration vector.
        std::string buildReport()
        {
            const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(_endTime - _startTime).count();
            const double wallSec = static_cast<double>(wallNs) / 1'000'000'000.0;

            const uint32_t n = static_cast<uint32_t>(_frameDurations.size());
            if (n == 0)
            {
                // Should not happen — start() requires targetFrames > 0
                // and onFrameEnd is the only caller of this — but be
                // defensive in case someone wires up bench differently.
                return "bench: no frames recorded";
            }

            const double fps = static_cast<double>(n) / wallSec;

            // Median, p95, max: sort a copy so the per-frame log retains
            // chronological ordering.
            std::vector<uint32_t> sorted = _frameDurations;
            std::sort(sorted.begin(), sorted.end());

            const uint32_t medianUs = sorted[n / 2];
            const uint32_t p95Us = sorted[std::min(n - 1, static_cast<uint32_t>(n * 95 / 100))];
            const uint32_t maxUs = sorted.back();

            char buf[256];
            std::snprintf(
                buf, sizeof(buf),
                "bench: %u frames, %.3f sec wall, %.1f fps avg, median %.1f ms, p95 %.1f ms, max %.1f ms",
                n, wallSec, fps, medianUs / 1000.0, p95Us / 1000.0, maxUs / 1000.0);
            return std::string(buf);
        }

        // Sequential index for the next bench log filename — scans the
        // bench/ directory for existing bench-NNNN-* and returns
        // (max + 1). 0001 if directory is empty / new. Mirrors the
        // approach used by the profiler snapshot writer.
        uint32_t findNextLogIndex(const std::string& dir)
        {
            uint32_t maxSeen = 0;
            std::error_code ec;
            std::filesystem::directory_iterator it(dir, ec);
            if (ec)
                return 1;
            for (const auto& entry : it)
            {
                const auto& path = entry.path();
                const std::string name = path.filename().string();
                // bench-NNNN-...
                if (name.size() < 11 || name.compare(0, 6, "bench-") != 0)
                    continue;
                uint32_t n = 0;
                for (size_t i = 6; i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); ++i)
                {
                    n = n * 10 + static_cast<uint32_t>(name[i] - '0');
                }
                if (n > maxSeen)
                    maxSeen = n;
            }
            return maxSeen + 1;
        }

        // Build the detailed CSV log path and write per-frame timings.
        // Path layout: <user>/bench/bench-NNNN-<sha>.txt. Header + one
        // CSV row per frame: frame_index,duration_us. Returns the path
        // (empty on failure).
        std::string writeDetailedLog()
        {
            auto* ctx = GetContext();
            if (ctx == nullptr)
                return {};

            auto& env = ctx->GetPlatformEnvironment();
            const std::string userDir = env.GetDirectoryPath(DirBase::user);
            const std::string benchDir = ::OpenRCT2::Path::Combine(userDir, "bench");
            ::OpenRCT2::Path::CreateDirectory(benchDir);

            const uint32_t seq = findNextLogIndex(benchDir);
            const char* sha =
#ifdef OPENRCT2_COMMIT_SHA1_SHORT
                OPENRCT2_COMMIT_SHA1_SHORT;
#else
                "nogit";
#endif

            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), "bench-%04u-%s.txt", seq, sha);
            const std::string filepath = ::OpenRCT2::Path::Combine(benchDir, std::string(nameBuf));

            std::FILE* f = std::fopen(filepath.c_str(), "w");
            if (f == nullptr)
            {
                LOG_WARNING("bench: failed to open %s for writing", filepath.c_str());
                return {};
            }

            // Header — summary line first so the file is self-describing
            // even without re-parsing the CSV.
            std::fprintf(f, "# %s\n", _lastReport.c_str());
            std::fprintf(f, "# build: %s\n", sha);
            std::fprintf(f, "# target_frames: %u\n", _targetFrames);
            std::fprintf(f, "# warmup_frames: %u\n", kBenchWarmupFrames);
            std::fprintf(
                f, "# wall_us: %lld\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(_endTime - _startTime).count()));
            std::fprintf(f, "frame_index,duration_us\n");
            for (size_t i = 0; i < _frameDurations.size(); ++i)
            {
                std::fprintf(f, "%zu,%u\n", i, _frameDurations[i]);
            }
            std::fclose(f);
            return filepath;
        }

        // Reset RNG so peep behaviour is identical across runs. Title
        // sequence is a deterministic command stream; the only source of
        // run-to-run variance left is scenarioRand → peep decisions →
        // peep movement.
        void resetRng()
        {
            getGameState().scenarioRand.seed(kBenchRngSeed);
        }
    } // namespace

    bool start(uint32_t targetFrames)
    {
        if (_active.load(std::memory_order_relaxed))
        {
            return false;
        }

        if (targetFrames == 0)
        {
            return false;
        }

        auto* ctx = GetContext();
        if (ctx == nullptr)
        {
            ::OpenRCT2::Console::Error::WriteLine("bench: no context — cannot start");
            return false;
        }

        // Force config so the title sequence loads deterministically:
        //   - randomTitleSequence off → same .parkseq each run
        //   - noTitleSequence off → run the actual demo, not the
        //     empty-park pan fallback
        // Restored in onFrameEnd when the run completes.
        overrideConfigForBench();

        // Switch to (or reload) the title scene. SetActiveScene calls
        // Stop() on the previous scene and Load() on the new one;
        // TitleScene::Load fully reinitialises the world via
        // gameStateInitAll, sets _loadedTitleSequenceId = SIZE_MAX so
        // TryLoadSequence forces a Begin(), and Begin() calls Reset()
        // which moves the title-sequence player back to position 0.
        // This is what makes "start from the same point every time"
        // possible regardless of what scene the user was in.
        //
        // Note: this discards any unsaved park / editor state. The
        // console help text + console-command handler warn the user.
        IScene* titleScene = ctx->GetTitleScene();
        if (titleScene == nullptr)
        {
            restoreConfigAfterBench();
            ::OpenRCT2::Console::Error::WriteLine("bench: title scene unavailable");
            return false;
        }
        ctx->SetActiveScene(titleScene);

        // Disable SDL renderer vsync. By default the hardware drawing
        // engine creates the renderer with SDL_RENDERER_PRESENTVSYNC,
        // which paces SDL_RenderPresent to the display refresh rate
        // (60 Hz on host, panel refresh on device). The bench wants
        // raw "draw as fast as the CPU + paint pipeline can produce
        // frames", so we toggle vsync off for the duration of the run
        // and restore the user's setting in onFrameEnd.
        DrawingEngineSetVSync(false);

        _targetFrames = targetFrames;
        _completedFrames = 0;
        _frameDurations.clear();
        _frameDurations.reserve(_targetFrames);
        _warmupRemaining = kBenchWarmupFrames;
        // Don't clear _lastReport / _lastLogPath — keep the previous
        // run's summary readable until the new one completes.
        _reportPending.store(false, std::memory_order_relaxed);

        // Reset the scenario RNG AFTER the scene switch (TitleScene::
        // Load → gameStateInitAll re-seeds scenarioRand from the system
        // clock, so we have to pin it back to the bench seed last).
        resetRng();

        // Force gGameSpeed = 1 so the early bench frames run at the
        // intended 1× speed regardless of any prior SetSpeed command
        // in a previous title sequence run, or a cheats-menu speed
        // change. gGameSpeed is a global that gameStateInitAll does
        // NOT reset, and TitleScene::Tick multiplies the per-tick
        // gameStateUpdateLogic call count by 1<<(gGameSpeed-1). If we
        // didn't pin this, runs starting after a fast-forward would
        // produce different peep behaviour for the first ~few seconds.
        // Title sequences with their own SetSpeed commands will still
        // override this once they hit that command.
        gGameSpeed = 1;

        // _startTime is captured later, at the moment warm-up ends —
        // see onFrameEnd's warm-up branch. Capturing here would include
        // the warm-up window in the wall-clock total, defeating the
        // point. Initialise to "now" anyway so a stop-without-warmup
        // path (currently unreachable) doesn't read uninitialised
        // memory.
        _startTime = std::chrono::steady_clock::now();
        _active.store(true, std::memory_order_relaxed);

        LOG_INFO(
            "bench: starting %u-frame run from title-sequence reload (after %u warm-up frames)",
            targetFrames, kBenchWarmupFrames);
        return true;
    }

    void onFrameEnd(uint32_t frameDurationUs)
    {
        if (!_active.load(std::memory_order_relaxed))
        {
            return;
        }

        // Warm-up: run the determinism gates but don't record. Cold-
        // cache costs (filesystem page cache for .park / .parkseq /
        // sprite atlases / object .DAT, sprite-decode cache, CPU L1i,
        // glibc arena init) are concentrated in frame 0 of a fresh
        // scene reload. Burning N frames before timing eliminates the
        // outlier without requiring root for drop_caches.
        if (_warmupRemaining > 0)
        {
            --_warmupRemaining;
            if (_warmupRemaining == 0)
            {
                // Last warm-up frame just finished — start the clock
                // here, not in start(). _startTime is what gets
                // subtracted from _endTime to compute wall_us, so
                // capturing it now means the warm-up window is
                // excluded from the report.
                _startTime = std::chrono::steady_clock::now();
            }
            return;
        }

        _frameDurations.push_back(frameDurationUs);
        ++_completedFrames;

        if (_completedFrames >= _targetFrames)
        {
            _endTime = std::chrono::steady_clock::now();
            _active.store(false, std::memory_order_relaxed);

            // Put the user's randomTitleSequence / noTitleSequence
            // config back where we found it. Done before report writing
            // so even if writeDetailedLog throws (it shouldn't, but),
            // the user's settings aren't left in our overridden state.
            restoreConfigAfterBench();

            // Restore the renderer's vsync setting from the user's
            // config. SetVSync is a no-op when the value already
            // matches, so back-to-back benches don't flicker the
            // renderer.
            DrawingEngineSetVSync(Config::Get().general.useVSync);

            _lastReport = buildReport();
            _lastLogPath = writeDetailedLog();

            // Mirror to both stdout (so command-line / qemu users get it
            // in the same stream as their other output) and the log
            // (which on device goes to the launch.sh-redirected file).
            std::printf("%s\n", _lastReport.c_str());
            std::fflush(stdout);
            LOG_INFO("%s", _lastReport.c_str());
            if (!_lastLogPath.empty())
            {
                LOG_INFO("bench: detailed log written to %s", _lastLogPath.c_str());
            }

            // Latch the report-pending flag last, after _lastReport /
            // _lastLogPath are fully written, so InGameConsole::Update
            // never sees torn data.
            _reportPending.store(true, std::memory_order_release);
        }
    }

    bool isActive()
    {
        return _active.load(std::memory_order_relaxed);
    }

    bool hasPendingReport()
    {
        return _reportPending.load(std::memory_order_acquire);
    }

    void markReportConsumed()
    {
        _reportPending.store(false, std::memory_order_relaxed);
    }

    const std::string& getLastReport()
    {
        return _lastReport;
    }

    const std::string& getLastLogPath()
    {
        return _lastLogPath;
    }
} // namespace OpenRCT2::Profiling::Bench
