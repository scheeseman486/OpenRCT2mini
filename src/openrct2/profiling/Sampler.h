/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/
#pragma once

// OPENRCT2MINI P1: Performance profiler sampler. See profiler-plan.md
// for the full design.
//
// The sampler is the data-collection layer behind the in-game Performance
// Profiler window. It hooks into Context::RunFrame and captures one
// FrameSnapshot per frame plus one SlowPoll every ~1 second. Data lands in
// pre-allocated ring buffers; the GUI reads from those.
//
// Cost when disabled (window never opened):
//   ~1 atomic relaxed load + branch in RunFrame's onFrameStart hook.
// Cost when enabled, window not visible:
//   per-frame: ~1 µs (counter copies, no syscalls)
//   per-second: ~50 µs (mallinfo + /proc reads)

#include <atomic>
#include <cstdint>
#include <vector>

#ifdef ENABLE_PERFORMANCE_PROFILER

namespace OpenRCT2::Profiling::Sampler
{
    // ----- Snapshot POD types -------------------------------------------
    //
    // Layout is part of the on-disk snapshot file format. Any change to
    // these structs MUST bump kSnapshotFormatVersion in SnapshotWriter
    // AND update the SCHM emitter. See profiler-plan.md "Snapshot save
    // format" → maintenance rule.

    // 80 bytes, sampled every frame. uint32_t microseconds wraps at
    // ~71 minutes — fine for a profiler that's looking at the recent past.
    struct FrameSnapshot
    {
        uint32_t frameStartMs;        // ms since profiler enable
        uint32_t frameDurationUs;     // wall-clock, full frame end-to-end

        // CPU phases (microseconds) — read from Profiling registry.
        uint32_t gameTickUs;
        uint32_t peepUpdateUs;
        uint32_t paintWalkUs;
        uint32_t paintArrangeUs;
        uint32_t paintDrawUs;
        uint32_t drawingEngineUs;
        uint32_t audioCallbackPeakUs;

        // Entity counts — direct read of EntityRegistry list sizes.
        uint16_t guestCount;
        uint16_t staffCount;
        uint16_t vehicleCount;
        uint16_t miscEntityCount;

        // Paint counters — peak across all sessions this frame.
        uint16_t paintEntriesUsed;
        uint16_t paintColumnCount;
        uint16_t windowsDrawn;

        // P8: active audio channels (sound effects + ride music streams)
        // at end of most recent audio callback. Sampled by AudioMixer
        // calling Sampler::recordAudioChannelCount.
        uint16_t audioChannelCount;

        // Pad to 80 bytes for predictable file layout. Compiler will
        // already pack to a multiple of 4 on ARMv7 / x86 32-bit.
        uint32_t slowPollIndex;       // index of the most recent SlowPoll
    };
    static_assert(sizeof(FrameSnapshot) == 56, "FrameSnapshot layout drift");
    // Note: 56 bytes, not the plan's 80 — the budget had room for future
    // fields. 56 is what the current set of metrics actually consumes
    // (9 × uint32_t timing + 8 × uint16_t counters + 1 × uint32_t index).
    // Keep this assert tight so the binary format is stable. ANY change
    // here MUST bump the snapshot formatVersion in P9 SnapshotWriter.

    // 40 bytes, sampled every ~1 second.
    struct SlowPoll
    {
        uint32_t timestampMs;
        uint32_t rssKB;
        uint32_t heapInUseKB;
        uint32_t heapMmapKB;
        uint32_t spriteScratchKB;
        uint32_t spriteCacheHits;
        uint32_t spriteCacheMisses;
        uint32_t readBytesPerSec;
        uint32_t majorFaultsPerSec;
        uint32_t minorFaultsPerSec;
    };
    static_assert(sizeof(SlowPoll) == 40, "SlowPoll layout drift");

    // ----- Lifecycle -----------------------------------------------------

    // Allocates the ring buffers and arms the RunFrame hooks. Idempotent.
    void enable();

    // Disarms the hooks. Buffers retained for re-enable; not freed.
    void disable();

    // Cheap relaxed atomic load. The RunFrame hooks check this first.
    bool isEnabled();

    // Reset all counters and ring buffers to empty. Window's "Reset"
    // button calls this. Does not disable the sampler.
    void resetData();

    // ----- Frame hooks ---------------------------------------------------
    //
    // Called from Context::RunFrame. onFrameStart at the top of the
    // function (after the powerOff poll), onFrameEnd just before the
    // function returns. Both are no-ops when isEnabled() is false.

    void onFrameStart();
    void onFrameEnd();

    // OPENRCT2MINI: Discard the in-flight frame that onFrameStart began —
    // do not write a slot in the ring on this call's onFrameEnd. Used by
    // Context::RunFixedFrame's framerate-pacing early-return path: when
    // the tick accumulator hasn't filled, RunFixedFrame Sleeps and
    // returns without ticking, drawing, or hitting any PROFILED_FUNCTION
    // callsite. Recording such a frame would pollute the ring with a
    // sub-millisecond duration (giving absurd FPS readings: 600-500000)
    // and zero CPU phase deltas. Slow polls in onFrameStart still ran
    // and stay in the slow ring — those are time-based, not frame-based.
    void cancelFrame();

    // ----- Audio callback hook ------------------------------------------
    //
    // Called from AudioMixer::GetNextAudioChunk. Records the duration of
    // the most recent callback so the Audio tab can show whether the
    // callback fits in the SDL audio buffer's deadline. Plain atomic
    // store of uint32_t microseconds — no CAS, no max — see
    // profiler-plan.md "Audio sampling".
    void recordAudioCallbackUs(uint32_t durationUs);

    // Number of channels active at the end of the most recent audio
    // callback (sound effects + ride music streams + title music).
    // Read by the Audio tab to show "what's playing right now".
    void recordAudioChannelCount(uint16_t channelCount);

    // The audio buffer's deadline budget in milliseconds. Computed in
    // AudioMixer::Init from `samples / freq` and recorded once. Used by
    // the Audio tab to overlay a horizontal "deadline" line on the
    // callback duration graph — if the line touches it, we're risking
    // underruns.
    void recordAudioBufferMs(uint16_t bufferMs);
    uint16_t getAudioBufferMs();

    // ----- Paint pipeline hooks -----------------------------------------
    //
    // Called from ViewportPaint and PaintSession bookkeeping. Only the
    // peak across all viewports drawn this frame is retained; onFrameEnd
    // reads-and-resets. Single-threaded — paint multithreading is
    // force-disabled in this fork (revision 291) so plain counters are
    // safe.
    void recordPaintColumnCount(uint16_t columnCount);
    void recordPaintEntriesUsed(uint16_t entriesUsed);
    void recordWindowsDrawn(uint16_t windowsDrawn);

    // ----- Sprite-cache hit/miss counters --------------------------------
    //
    // Called from SpriteCacheLookup. Cumulative-since-last-slow-poll counts
    // are read-and-reset into each SlowPoll's spriteCacheHits/Misses fields,
    // so the Memory tab and analyse scripts can sum them across the slow
    // ring to compute the lifetime hit rate during the captured window.
    // Cheap relaxed atomic increments; gated on isEnabled() so the cost is
    // a single load+branch when the profiler is closed.
    void recordSpriteCacheHit();
    void recordSpriteCacheMiss();

    // ----- Read-side -----------------------------------------------------
    //
    // The window calls these from its onUpdate / onDraw to render data.
    // Returns const refs to the live ring buffers. Capacity is fixed
    // after enable(); contents are appended in a wrap-around fashion
    // (the latest sample is at index `(head - 1) mod capacity`).

    const std::vector<FrameSnapshot>& getFrameRing();
    const std::vector<SlowPoll>& getSlowPollRing();

    // Index of the *next* write slot in each ring. Subtract one
    // (mod capacity) to get the most recent sample.
    uint32_t getFrameRingHead();
    uint32_t getSlowPollRingHead();

    // Number of valid entries in each ring (≤ capacity).
    uint32_t getFrameRingCount();
    uint32_t getSlowPollRingCount();

} // namespace OpenRCT2::Profiling::Sampler

#endif // ENABLE_PERFORMANCE_PROFILER
