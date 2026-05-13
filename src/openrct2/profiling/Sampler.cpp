/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI P1: Performance profiler sampler. See Sampler.h and
// profiler-plan.md.
//
// P1 ships only the framework: ring buffers, enable/disable flag,
// RunFrame hooks that record frame start/end timestamps. No metric
// collection beyond that — P2 will fill in CPU phases by reading the
// Profiling registry, P4 adds memory polls, etc. The ring buffers and
// public API are sized correctly from day one so later phases just
// populate fields.

#include "Sampler.h"

#ifdef ENABLE_PERFORMANCE_PROFILER

#include "../GameState.h"
#include "../entity/EntityBase.h"
#include "../entity/EntityRegistry.h"
#include "Profiling.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef __linux__
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

#ifdef __GLIBC__
    // mallinfo() is deprecated upstream in favour of mallinfo2() (glibc
    // 2.33+), but the OnionUI toolchain ships glibc 2.28. We're stuck
    // with the legacy API. Fields are int-typed and overflow past 2 GB —
    // irrelevant on a 128 MB device.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    #include <malloc.h>
    #pragma GCC diagnostic pop
#endif

#ifdef __APPLE__
    #include <mach/mach.h>
    #include <mach/task.h>
#endif

#ifndef PROFILER_HISTORY_SECONDS
    #define PROFILER_HISTORY_SECONDS 30
#endif

namespace OpenRCT2::Profiling::Sampler
{
    namespace
    {
        // Frame ring is sized to PROFILER_HISTORY_SECONDS at 30 FPS.
        constexpr uint32_t kFrameRingCapacity = PROFILER_HISTORY_SECONDS * 30;
        // Slow poll runs at 1 Hz; keep 3× the frame-history window so the
        // user can scroll back further on memory/IO than on CPU.
        constexpr uint32_t kSlowPollRingCapacity = PROFILER_HISTORY_SECONDS * 3;

        // Hot-path gate. Relaxed everywhere — no synchronisation, just
        // the cheapest possible "is anyone watching" check.
        std::atomic<bool> _enabled{ false };

        // Ring buffers. Empty until first enable(); .reserve() then
        // resize-to-capacity so contents are stable for the lifetime
        // of the process.
        std::vector<FrameSnapshot> _frameRing;
        std::vector<SlowPoll> _slowRing;
        uint32_t _frameHead = 0;
        uint32_t _slowHead = 0;
        uint32_t _frameCount = 0;
        uint32_t _slowCount = 0;

        // Time origin for the frameStartMs field. Captured at enable().
        std::chrono::steady_clock::time_point _epoch;

        // Last frame's start timestamp; onFrameEnd subtracts to get
        // duration. We record into the most-recently-written slot.
        std::chrono::steady_clock::time_point _frameStartTp;
        bool _frameInFlight = false;

        // Last second-boundary we polled the slow ring at.
        uint32_t _lastSlowPollMs = 0;

        // Audio callback peak — written from the audio thread (relaxed),
        // read from the main thread once per frame. uint32_t µs so a
        // single store is atomic on ARMv7 without ldrexd/strexd.
        std::atomic<uint32_t> _audioCallbackUs{ 0 };

        // Active channel count, written from the audio thread on each
        // callback. Same single-store-relaxed pattern.
        std::atomic<uint16_t> _audioChannels{ 0 };

        // SDL audio buffer's deadline budget in ms. Set once at
        // AudioMixer::Init, read by the Audio tab for the deadline line.
        std::atomic<uint16_t> _audioBufferMs{ 0 };

        // Paint counters — main thread only (paint multithreading is
        // force-disabled per revision 291). Updated via recordPaint*()
        // called from ViewportPaint, read-and-reset in onFrameEnd.
        uint16_t _paintColumnPeak = 0;
        uint16_t _paintEntriesPeak = 0;
        uint16_t _windowsDrawnLast = 0;

        // Sprite-cache hit/miss counters. Atomic because SpriteCacheLookup
        // can be called from background asset-loading threads (FileIndex
        // workers). Read-and-reset by the slow poll once per second.
        std::atomic<uint32_t> _spriteCacheHits{ 0 };
        std::atomic<uint32_t> _spriteCacheMisses{ 0 };

        // ----- P3: CPU phase integration via Profiling registry --------
        //
        // We don't add new PROFILED_FUNCTION callsites; we read existing
        // ones. Each phase is identified by a substring match against
        // the function name (PROFILED_FUNCTION uses __PRETTY_FUNCTION__
        // so names vary slightly across compilers — substring is robust).
        //
        // First lookup is lazy: on the first frame after enable(), we
        // walk Profiling::getData() and cache pointers. Subsequent
        // frames just diff the cached counters. If a function hasn't
        // run yet (e.g., we opened the profiler from the title screen
        // before any in-game frame), its pointer stays null and we
        // retry the lookup each frame until found.
        struct PhaseSlot
        {
            const char* searchSubstring;
            ::OpenRCT2::Profiling::Function* func; // null until first match
            uint64_t lastTotalUs;
            // Pointer-to-member into FrameSnapshot.
            uint32_t FrameSnapshot::*field;
        };

        // Substrings include the trailing '(' so e.g. "ViewportPaint" doesn't
        // accidentally bind to ViewportPaintColumn / ViewportPaintWeatherGloom
        // (the registry is iterated in load order, so the first match wins).
        // The 0.3.0 captures showed paintWalk = 22 ms instead of the expected
        // 60 ms because "ViewportPaint" was matching ViewportPaintColumn first.
        PhaseSlot _phases[] = {
            // Top-level game tick: covers gameStateUpdateLogic + child work.
            { "gameStateTick(", nullptr, 0, &FrameSnapshot::gameTickUs },
            // PeepUpdateAll: the std::list iteration over guests/staff.
            // We expect this to be the dominant subsystem on busy parks.
            { "PeepUpdateAll(", nullptr, 0, &FrameSnapshot::peepUpdateUs },
            // ViewportFillColumn: per-column tile walk + emits PaintStructs.
            // Includes the embedded PaintSessionArrange call (tracked
            // separately below as the arrange sub-phase).
            { "ViewportFillColumn(", nullptr, 0, &FrameSnapshot::paintWalkUs },
            // PaintSessionArrange: the quadrant-sort pass that orders the
            // emitted PaintStructs into draw order. Subset of paintWalk —
            // showing it separately reveals how much of the walk is sort.
            { "PaintSessionArrange(", nullptr, 0, &FrameSnapshot::paintArrangeUs },
            // ViewportPaintColumn: per-column rasterise pass (PaintDrawStructs).
            // The pixel-pushing phase of the column iteration.
            { "ViewportPaintColumn(", nullptr, 0, &FrameSnapshot::paintDrawUs },
            // Context::Draw: BeginDraw + Painter::Paint + EndDraw wrapper.
            // The all-encompassing draw cost — includes everything above
            // plus window draws, UI overlay, and present overhead.
            { "Context::Draw(", nullptr, 0, &FrameSnapshot::drawingEngineUs },
        };

        // Track whether WE were the one to enable profiling, so disable()
        // doesn't yank profiling out from under e.g. an in-game console
        // user who'd already turned it on.
        bool _profilingTurnedOnByUs = false;

        // Last cumulative read_bytes value from /proc/self/io. The slow
        // poll diffs against this to compute a per-second rate.
        uint64_t _lastIoReadBytes = 0;

        // ----- P4: memory / I/O slow-poll helpers ----------------------
        //
        // Linux helpers behind __linux__; glibc-specific bits behind
        // __GLIBC__. macOS gets a partial fallback (RSS only) via Mach.
        // Returns 0 / "—" semantics for unavailable values.

        // RSS in kilobytes from /proc/self/statm (page count × page size).
        uint32_t readRssKB()
        {
#ifdef __linux__
            FILE* f = std::fopen("/proc/self/statm", "r");
            if (f == nullptr)
                return 0;
            // Format: size resident shared text lib data dt — pages.
            unsigned long size = 0, resident = 0;
            const int n = std::fscanf(f, "%lu %lu", &size, &resident);
            std::fclose(f);
            if (n < 2)
                return 0;
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize <= 0)
                return 0;
            return static_cast<uint32_t>((resident * static_cast<unsigned long>(pageSize)) / 1024);
#elif defined(__APPLE__)
            struct mach_task_basic_info info {};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            const kern_return_t kr = task_info(
                mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
            if (kr != KERN_SUCCESS)
                return 0;
            return static_cast<uint32_t>(info.resident_size / 1024);
#else
            return 0;
#endif
        }

        // /proc/self/io read_bytes counter (cumulative bytes read by the
        // process from any storage). 0 if /proc/self/io isn't accessible.
        uint64_t readIoReadBytes()
        {
#ifdef __linux__
            FILE* f = std::fopen("/proc/self/io", "r");
            if (f == nullptr)
                return 0;
            char line[128];
            uint64_t readBytes = 0;
            while (std::fgets(line, sizeof(line), f) != nullptr)
            {
                unsigned long long v = 0;
                if (std::sscanf(line, "read_bytes: %llu", &v) == 1)
                {
                    readBytes = static_cast<uint64_t>(v);
                    break;
                }
            }
            std::fclose(f);
            return readBytes;
#else
            return 0;
#endif
        }

        // mallinfo: in-use sbrk arena bytes (uordblks) + total mmap'd
        // bytes (hblkhd). int-typed; overflows past 2 GB but irrelevant
        // for our target. Returns {0, 0} on non-glibc.
        struct HeapInfo
        {
            uint32_t inUseKB;
            uint32_t mmapKB;
        };
        HeapInfo readHeapInfo()
        {
#ifdef __GLIBC__
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            const struct mallinfo mi = mallinfo();
            #pragma GCC diagnostic pop
            // mi.uordblks: bytes of in-use sbrk allocations.
            // mi.hblkhd: bytes of mmap'd allocations.
            // Both `int`. Cast through unsigned to avoid sign-extending
            // a small negative if overflow ever occurs.
            const uint32_t inUseKB = static_cast<uint32_t>(static_cast<unsigned int>(mi.uordblks) / 1024);
            const uint32_t mmapKB = static_cast<uint32_t>(static_cast<unsigned int>(mi.hblkhd) / 1024);
            return { inUseKB, mmapKB };
#else
            return { 0, 0 };
#endif
        }

        // Page-fault deltas. We track cumulative counts and return per-
        // second rates. State persists across calls.
        long _lastMajFaults = 0;
        long _lastMinFaults = 0;
        bool _faultBaselineSet = false;

        struct FaultRates
        {
            uint32_t majPerSec;
            uint32_t minPerSec;
        };
        FaultRates readFaultRates(uint32_t deltaMs)
        {
#ifdef __linux__
            struct rusage ru {};
            if (getrusage(RUSAGE_SELF, &ru) != 0)
                return { 0, 0 };
            const long maj = ru.ru_majflt;
            const long min_ = ru.ru_minflt;
            FaultRates result{ 0, 0 };
            if (_faultBaselineSet && deltaMs > 0)
            {
                const long dmaj = std::max<long>(0, maj - _lastMajFaults);
                const long dmin = std::max<long>(0, min_ - _lastMinFaults);
                // (delta * 1000ms) / deltaMs = per-second rate.
                result.majPerSec = static_cast<uint32_t>((dmaj * 1000) / static_cast<long>(deltaMs));
                result.minPerSec = static_cast<uint32_t>((dmin * 1000) / static_cast<long>(deltaMs));
            }
            _lastMajFaults = maj;
            _lastMinFaults = min_;
            _faultBaselineSet = true;
            return result;
#else
            (void)deltaMs;
            return { 0, 0 };
#endif
        }

        uint32_t msSinceEpoch()
        {
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - _epoch).count());
        }
    } // namespace

    void enable()
    {
        if (_enabled.load(std::memory_order_relaxed))
            return;

        if (_frameRing.empty())
        {
            _frameRing.resize(kFrameRingCapacity);
            _slowRing.resize(kSlowPollRingCapacity);
        }
        _frameHead = 0;
        _slowHead = 0;
        _frameCount = 0;
        _slowCount = 0;
        _frameInFlight = false;
        _lastSlowPollMs = 0;
        _audioCallbackUs.store(0, std::memory_order_relaxed);
        _epoch = std::chrono::steady_clock::now();

        // Reset phase lookup cache — re-find pointers on next frame.
        for (auto& p : _phases)
        {
            p.func = nullptr;
            p.lastTotalUs = 0;
        }
        _lastIoReadBytes = 0;
        _faultBaselineSet = false;
        _lastMajFaults = 0;
        _lastMinFaults = 0;
        _spriteCacheHits.store(0, std::memory_order_relaxed);
        _spriteCacheMisses.store(0, std::memory_order_relaxed);

        // Turn on the upstream profiling registry if it isn't already.
        // Tracked so disable() doesn't kill someone else's session.
        if (!::OpenRCT2::Profiling::isEnabled())
        {
            ::OpenRCT2::Profiling::enable();
            _profilingTurnedOnByUs = true;
        }

        _enabled.store(true, std::memory_order_relaxed);
    }

    void disable()
    {
        _enabled.store(false, std::memory_order_relaxed);
        _frameInFlight = false;

        if (_profilingTurnedOnByUs)
        {
            ::OpenRCT2::Profiling::disable();
            _profilingTurnedOnByUs = false;
        }
        // Buffers retained — re-enable is cheaper if we keep them.
    }

    bool isEnabled()
    {
        return _enabled.load(std::memory_order_relaxed);
    }

    void resetData()
    {
        _frameHead = 0;
        _slowHead = 0;
        _frameCount = 0;
        _slowCount = 0;
        _frameInFlight = false;
        _lastSlowPollMs = 0;
        _epoch = std::chrono::steady_clock::now();
        _audioCallbackUs.store(0, std::memory_order_relaxed);
    }

    void onFrameStart()
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;

        _frameStartTp = std::chrono::steady_clock::now();
        _frameInFlight = true;

        // Slow-poll boundary check — every ~1 second. P4 populates RSS,
        // heap, page-fault rates, and disk-read rate.
        const uint32_t nowMs = msSinceEpoch();
        const uint32_t sinceLastMs = nowMs - _lastSlowPollMs;
        if (sinceLastMs >= 1000)
        {
            _lastSlowPollMs = nowMs;
            SlowPoll& sp = _slowRing[_slowHead];
            sp = {};
            sp.timestampMs = nowMs;
            sp.rssKB = readRssKB();
            const HeapInfo heap = readHeapInfo();
            sp.heapInUseKB = heap.inUseKB;
            sp.heapMmapKB = heap.mmapKB;

            // Disk read rate: byte-count delta over the elapsed window.
            const uint64_t curIoReadBytes = readIoReadBytes();
            if (_lastIoReadBytes != 0 && sinceLastMs > 0)
            {
                const uint64_t delta = (curIoReadBytes >= _lastIoReadBytes)
                                           ? (curIoReadBytes - _lastIoReadBytes)
                                           : 0;
                sp.readBytesPerSec = static_cast<uint32_t>(
                    std::min<uint64_t>(
                        (delta * 1000) / sinceLastMs,
                        std::numeric_limits<uint32_t>::max()));
            }
            _lastIoReadBytes = curIoReadBytes;

            const FaultRates faults = readFaultRates(sinceLastMs);
            sp.majorFaultsPerSec = faults.majPerSec;
            sp.minorFaultsPerSec = faults.minPerSec;

            // Sprite-cache hit/miss deltas since the previous slow poll.
            // SpriteCacheLookup increments these via recordSpriteCacheHit /
            // recordSpriteCacheMiss; we read-and-reset here so each poll
            // is a per-second rate. analyze scripts sum across the ring to
            // get the lifetime totals during a capture window.
            sp.spriteCacheHits = _spriteCacheHits.exchange(0, std::memory_order_relaxed);
            sp.spriteCacheMisses = _spriteCacheMisses.exchange(0, std::memory_order_relaxed);

            // spriteScratchKB still unwired — would require a hook into
            // SpriteScratch.cpp's mmap accounting, deferred.

            _slowHead = (_slowHead + 1) % kSlowPollRingCapacity;
            if (_slowCount < kSlowPollRingCapacity)
                _slowCount++;
        }
    }

    void cancelFrame()
    {
        // Drop the in-flight frame that onFrameStart began so a matching
        // onFrameEnd does NOT write a slot in the ring. Called from
        // Context::RunFixedFrame's "_ticksAccumulator < kGameUpdateTimeMS"
        // sleep+return pacing path. See Sampler.h declaration for why.
        _frameInFlight = false;
    }

    void onFrameEnd()
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        if (!_frameInFlight)
            return;
        _frameInFlight = false;

        const auto endTp = std::chrono::steady_clock::now();
        const auto durationUs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(endTp - _frameStartTp).count());

        FrameSnapshot& fs = _frameRing[_frameHead];
        fs = {};
        fs.frameStartMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(_frameStartTp - _epoch).count());
        fs.frameDurationUs = durationUs;

        // P3: lazily resolve any unmapped phase pointers, then read
        // each phase's totalUs and write the per-frame delta.
        const bool allFound = std::all_of(
            std::begin(_phases), std::end(_phases),
            [](const PhaseSlot& p) { return p.func != nullptr; });
        if (!allFound)
        {
            const auto& funcs = ::OpenRCT2::Profiling::getData();
            for (auto& phase : _phases)
            {
                if (phase.func != nullptr)
                    continue;
                for (auto* f : funcs)
                {
                    const char* name = f->getName();
                    if (name != nullptr && std::strstr(name, phase.searchSubstring) != nullptr)
                    {
                        phase.func = f;
                        phase.lastTotalUs = static_cast<uint64_t>(f->getTotalTime());
                        break;
                    }
                }
            }
        }

        for (auto& phase : _phases)
        {
            if (phase.func == nullptr)
                continue;
            // getTotalTime() returns cumulative microseconds (relaxed
            // atomic load inside, see Profiling.h:120). The delta is
            // this frame's wall-clock contribution.
            const uint64_t cur = static_cast<uint64_t>(phase.func->getTotalTime());
            const uint64_t delta = (cur >= phase.lastTotalUs) ? (cur - phase.lastTotalUs) : 0;
            phase.lastTotalUs = cur;
            const uint32_t deltaUs = static_cast<uint32_t>(
                std::min<uint64_t>(delta, std::numeric_limits<uint32_t>::max()));
            fs.*(phase.field) = deltaUs;
        }

        // P6: entity counts. EntityRegistry maintains O(1) std::list
        // sizes, so this is just four reads — no traversal cost.
        // Lump misc entity types (litter, particles, money effect,
        // duck, balloon, jumping fountain, etc.) into a single bucket;
        // they're all roughly bookkeeping-priced per tick and the
        // entities tab presents them as one row.
        try
        {
            auto& reg = getGameState().entities;
            fs.guestCount = reg.GetEntityListCount(EntityType::guest);
            fs.staffCount = reg.GetEntityListCount(EntityType::staff);
            fs.vehicleCount = reg.GetEntityListCount(EntityType::vehicle);
            fs.miscEntityCount = reg.GetMiscEntityCount();
        }
        catch (...)
        {
            // Game state not yet ready (profiler opened from title
            // screen before any park is loaded). Leave at zero.
        }

        // P7: paint counters — read-and-reset.
        fs.paintColumnCount = _paintColumnPeak;
        fs.paintEntriesUsed = _paintEntriesPeak;
        fs.windowsDrawn = _windowsDrawnLast;
        _paintColumnPeak = 0;
        _paintEntriesPeak = 0;
        _windowsDrawnLast = 0;

        // Read-and-reset the audio peak. See Sampler.h for the
        // intentionally-imprecise sampling model.
        fs.audioCallbackPeakUs = _audioCallbackUs.exchange(0, std::memory_order_relaxed);
        // Channel count is a "current value" not a peak — just read.
        fs.audioChannelCount = _audioChannels.load(std::memory_order_relaxed);

        fs.slowPollIndex = (_slowCount > 0) ? ((_slowHead + kSlowPollRingCapacity - 1) % kSlowPollRingCapacity) : 0;

        _frameHead = (_frameHead + 1) % kFrameRingCapacity;
        if (_frameCount < kFrameRingCapacity)
            _frameCount++;
    }

    void recordAudioCallbackUs(uint32_t durationUs)
    {
        // No max-CAS — see profiler-plan.md "Audio sampling". This is
        // called from the SDL audio thread and the cost on ARMv7 must
        // be just a relaxed store, nothing exclusive.
        _audioCallbackUs.store(durationUs, std::memory_order_relaxed);
    }

    void recordAudioChannelCount(uint16_t channelCount)
    {
        // Audio thread → main thread, single relaxed store. Always
        // recorded (not gated on _enabled) — the buffer-ms metric is
        // also always-on, both have negligible cost.
        _audioChannels.store(channelCount, std::memory_order_relaxed);
    }

    void recordAudioBufferMs(uint16_t bufferMs)
    {
        _audioBufferMs.store(bufferMs, std::memory_order_relaxed);
    }

    uint16_t getAudioBufferMs()
    {
        return _audioBufferMs.load(std::memory_order_relaxed);
    }

    void recordPaintColumnCount(uint16_t columnCount)
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        if (columnCount > _paintColumnPeak)
            _paintColumnPeak = columnCount;
    }

    void recordPaintEntriesUsed(uint16_t entriesUsed)
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        if (entriesUsed > _paintEntriesPeak)
            _paintEntriesPeak = entriesUsed;
    }

    void recordWindowsDrawn(uint16_t windowsDrawn)
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        _windowsDrawnLast = windowsDrawn;
    }

    void recordSpriteCacheHit()
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        _spriteCacheHits.fetch_add(1, std::memory_order_relaxed);
    }

    void recordSpriteCacheMiss()
    {
        if (!_enabled.load(std::memory_order_relaxed))
            return;
        _spriteCacheMisses.fetch_add(1, std::memory_order_relaxed);
    }

    const std::vector<FrameSnapshot>& getFrameRing()
    {
        return _frameRing;
    }
    const std::vector<SlowPoll>& getSlowPollRing()
    {
        return _slowRing;
    }
    uint32_t getFrameRingHead()
    {
        return _frameHead;
    }
    uint32_t getSlowPollRingHead()
    {
        return _slowHead;
    }
    uint32_t getFrameRingCount()
    {
        return _frameCount;
    }
    uint32_t getSlowPollRingCount()
    {
        return _slowCount;
    }
} // namespace OpenRCT2::Profiling::Sampler

#endif // ENABLE_PERFORMANCE_PROFILER
