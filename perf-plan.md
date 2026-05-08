# OpenRCT2mini perf-plan

Optimisation plan derived from the rev 93d capture analysis (`perf-report-rev93.md`) and the source-code audit. Each item lists hypothesis, test, approach, target, side effects, and risk.

Items are stratified by **predictability** — how confident we are that the change will measurably help — rather than raw ms-saved. Predictable wins go first; structural rewrites go last.

## State of play (rev 93d, busy Alton Towers, profiler open)

- avg 213 ms / frame (4.7 fps), median 189 ms (5.3 fps)
- `drawingEngine` 150 ms (70 % of frame), `gameTick` 60 ms (28 %)
- 70 % of CPU time spent in `ViewportPaint`
- **Audio fixed** (rev 93d, `want.samples = 2048`)
- 100 % stutter (every frame > 50 ms)
- Heap growing 70-220 KB/sec during busy scenes
- 1079/s minor-fault peak — source not yet attributed (heap churn vs file-backed mmap)

C++17 backport (cut 33) audited — clean, no regressions in hot paths.

## Headline metrics

Use these consistently across captures:

- **Median frame time** — typical experience
- **p95 frame time** — bad-frame frequency
- **Minor-faults / sec peak** — memory pressure
- **ViewportPaint calls / Draw** — dispatcher multiplier

Avoid avg frame time as a headline (sensitive to outlier multi-second stalls).

---

## Tier 1 — Predictable wins (1-line / surgical)

### P1. `fixedPaintEntries` cap 256 → 512

**File**: `src/openrct2/paint/Paint.h:173`

**Hypothesis**. The cap was reduced from upstream's 1024 to 256 to keep `sizeof(PaintSession)` small. The capture shows max 391 entries per session — we routinely overflow into `dynamicPaintEntries`, which heap-allocates a `segmented_vector` and frees it on session reuse. With ~6 ViewportPaint calls per frame and overflow common on busy scenes, that's multiple malloc/free pairs per frame.

**Test**. Capture before / after on the same Alton Towers save. Compare:
- minor-fault rate (slow poll)
- RSS growth slope across the slow-poll window
- median + p95 frame time

**Approach**. Change `256` to `512` in two places (the static_vector cap and the segmented_vector chunk size — keep them consistent so a one-overflow case still avoids the chunk).

**Target**. Eliminate per-frame heap alloc when avg paintEntries ≤ 512 (covers our observed max of 391 with headroom). Expected: minor-fault rate halves, p95 frame time drops 5-10 %.

**Memory cost**. `sizeof(PaintEntry) = 68 bytes` on 32-bit ARM (verified — 6× int32_t Bounds + 5 ptrs + image_id + ScreenPos + MapPos + 4 small fields). `(512 - 256) × 68 = 17.4 KB` per active session. The `_freePaintSessions` pool grows to peak concurrent column count (~80 on busy parks) and stays there for process lifetime. **Persistent RSS impact: ~1.4 MB**. Fine vs. our 60 MB working set. L1d (32 KB) doesn't hold the full 34.8 KB session, but paint-entry access is streaming (sequential append, sequential read) so L1 capacity isn't the constraint — hardware prefetcher handles this pattern.

**Risk**. **Low.** One-line change, single-file blast radius.

**Effort**. < 1 hour.

### P2. PROFILED_FUNCTION lite mode for the 6 phases the Sampler reads

**File**: new — `src/openrct2/profiling/ProfilingMacrosLite.hpp` + ~6 callsite changes.

**Hypothesis**. When the profiler is open, every PROFILED_FUNCTION callsite costs ~400-900 ns on Cortex-A7 (2× clock_gettime via vDSO at ~150-300 ns each + 5+ relaxed atomics + thread_local stack push/pop). Hottest 40k callsites/frame × ~600 ns mean ≈ **24 ms / frame ≈ 10-11 % overhead** at 213 ms frames. The Sampler only reads 6 phases; all other PROFILED_FUNCTION callsites are dead instrumentation that still costs CPU when active.

**Test**. With profiler open, capture twice — once with the existing PROFILED_FUNCTION on inner hot loops, once swapped for the lite macro (or no-op). Compare frame time.

**Approach**. Define `PROFILED_FUNCTION_LITE()` that records only call count + total time (no min/max, no sample ring, no parent tracking). Two atomic adds, no clock_gettime if Profiling is disabled. Use lite for the 6 Sampler-read phases; remove instrumentation entirely from the millions-of-calls inner loops (`TileElementPaintSetup`, `EntityPaintSetup`, etc) — the Sampler gets aggregated time from the outer phases.

**Target**. Profiler-open overhead from ~10-11 % of CPU down to ~1-2 %. Saves ~20 ms / frame at current frame times. Profiler captures stop lying about the perf they observe.

**Risk**. **Low-medium.** Removing instrumentation from inner loops loses textual `profiler_export` data on those functions — but the textual workflow is rarely used in this fork and the in-game window is the canonical UI.

**Effort**. ~half a day.

### P3. Heap-trim threshold tuning *(conditional on P10 outcome)*

**File**: where we currently set `MALLOC_ARENA_MAX=2` (rev 12).

**Prerequisite**: P10's fault-source instrumentation must confirm the 1079/s minor-fault rate is from heap pages, not file-backed mmap. If they're SpriteScratch demand-faults, `mallopt` is the wrong fix and this item is skipped.

**Hypothesis**. With `MALLOC_ARENA_MAX=2` and default `M_TRIM_THRESHOLD=128 KB`, glibc returns pages to the kernel aggressively (sbrk(-N) on the heap top). The minor-fault spikes during busy scenes could be the kernel re-faulting heap pages we just released. Other potential fault sources to rule out via P10: SpriteScratch demand-paging (file-backed mmap; faults still count as minor if the page is in page cache). OGG audio decode is *not* in the default path — only used by user-installed mods.

Bumping `M_TRIM_THRESHOLD` to 1 MB would keep the heap top "warm" and reduce repeated sbrk churn.

**Test**. Same Alton Towers save before / after. Measure minor-fault rate via slow poll.

**Approach**. `mallopt(M_TRIM_THRESHOLD, 1 * 1024 * 1024)` early in startup, alongside the existing `MALLOC_ARENA_MAX` set.

**Target**. Steady-state minor faults ≤ 100/s.

**Memory cost**. Heap top can grow ~1 MB more before glibc trims. Negligible vs. our 60 MB working set.

**Risk**. **Low.** Single mallopt call.

**Effort**. < 1 hour after P10 confirms heap source.

---

## Tier 2 — Measurement-driven (instrument first)

### P10. Autosave + fault-source instrumentation

**File**: `src/openrct2/profiling/Sampler.cpp` (slow poll), `src/openrct2/scenario/Save.cpp` (or wherever autosave lives).

**Hypothesis**. The 8-second stalls observed in earlier captures are autosave write events caught mid-frame. The minor-fault spikes might be heap or file-backed pages — we don't know which.

**Approach**.
1. Add `bool autosaveActive` to slow-poll struct. Set true on autosave start, false on completion. Already-instrumented paths in Save.cpp.
2. Read `/proc/self/status` for `RssAnon` and `RssFile`, add to slow poll. Their growth rates indicate which kind of page is faulting in.
3. Re-capture device snapshot during a busy session that includes an autosave.

**Target**. Two new questions answered:
- Are the stalls autosave? (correlates `autosaveActive` with frame time spikes)
- Is the heap or the file cache faulting? (correlates `RssAnon` vs `RssFile` growth with `minorFaultsPerSec`)

**Risk**. **None** — pure instrumentation.

**Effort**. ~half a day.

### P5. UI invalidation reduction *(instrument-first, fix-after)*

**Files**: TBD after instrumentation pass.

**Hypothesis**. ~6 ViewportPaint calls / frame on busy parks come from ~3 disjoint dirty regions per dispatcher sweep. The regions probably include the profiler (when open), toolbar, cursor area, but I don't know the actual breakdown.

**Approach**.
1. Add a per-frame "invalidation source" log: each call to `InvalidationGrid::invalidate()` records caller (via address or string tag) and rect.
2. Capture a frame's worth of invalidations on the device.
3. Identify the top sources by rect-area × frequency.
4. Then fix the worst offenders (cursor / FPS / clock / dropdown highlights / etc.) by either rate-limiting or merging.

**Target**. Dirty rect count from ~3 per sweep → 1-2 per sweep. ViewportPaint calls per frame from ~6 → 2-4. ~30 % drawingEngine reduction.

**Risk**. **Medium.** Each fix is localised but easy to introduce visual stalls (cursor stuck, clock not updating, dropdown not highlighting).

**Effort**. Half a day to instrument, then 1-2 days to fix the identified offenders.

---

## Tier 3 — High-impact structural

### P4. Per-sweep ViewportPaint memoisation (the big one)

**File**: `src/openrct2/interface/Viewport.cpp`, `src/openrct2/drawing/X8DrawingEngine.cpp` (sweep boundaries).

**Hypothesis**. ViewportPaint is called ~6 times per frame on busy scenes because `DrawAllDirtyBlocks()` runs twice per `Painter::Paint`, and each pass dispatches each dirty rect to every overlapping window. The main world window overlaps ~3 dirty rects per sweep × 2 sweeps = ~6 calls. Most rects within a single sweep don't actually need the world re-walked — the dirty rects are typically non-overlapping pieces of the same world view.

**Sanity-check correction**: an earlier draft of this plan proposed a "columns rendered this sweep" bitset. **That is incorrect** — two non-overlapping dirty rects in the same sweep can both want column N's contribution but at *different Y ranges*. A column-only bitset would skip the second rect's column-walk even though those Y-pixels were never written.

**Corrected approach** — single full-viewport paint per sweep, dispatcher skips world for subsequent rects:

1. At top of `X8DrawingEngine::DrawAllDirtyBlocks()` (each sweep): paint the world full-viewport once into `_mainRT.bits`. Walks every column visible in the viewport — ~80 columns × 1.6 ms ≈ 130 ms once per sweep, instead of ~6 dispatches × 22.5 ms ≈ 135 ms today. Roughly the same cost on busy scenes, but eliminates the overlap multiplier.
2. Add a `_worldPaintedThisSweep` flag on the engine.
3. In `WindowDrawAll`, when iterating windows for a dirty rect: if the window owns the main viewport AND `_worldPaintedThisSweep` is true, skip its `WindowDrawViewport` call entirely. Foreground windows still draw per-rect as today (they need to overlay the freshly-painted world).
4. `WindowUpdateAllViewports()` between sweeps clears the flag.

**Why this is correct**: the world is painted into `_mainRT.bits` exactly once per sweep, before any foreground window draws. Foreground windows then overlay per-rect, painting their own pixels on top — same flow as today. The world pixels under foreground windows get overwritten as today; that's not a problem because we don't re-read those pixels.

**Why this is faster on busy scenes**: today's ~6 dirty rects covering ~25 % screen each = ~150 % of screen-worth of column-walks. Single full-viewport paint = 100 % of screen. ~33 % column-walk reduction, plus eliminates the sort/arrange done per dispatch.

**On a static frame with one tiny dirty rect** (e.g. cursor-only frame), this is a slight regression — full viewport vs. one column. Worth measuring; may need a heuristic that only triggers when N dirty rects exceed a threshold.

**Target**. ViewportPaint cost / frame from 141 ms → ~80-100 ms (~30-40 % reduction, more conservative than the "50 %" I'd originally claimed). Frame time from 213 ms → ~150-170 ms.

**Risk**. **Medium-high.** Failure modes:
- The "world is painted, skip it" flag must be cleared correctly between sweeps and between frames. A stale flag means the world doesn't repaint when it should — the user sees a frozen world.
- The full-viewport paint is one continuous ~130 ms render burst rather than ~6 smaller bursts. The audio thread (currently fine with rev 93d at `samples=2048`, ~42 ms callback period) might miss a callback if it can't get scheduled during the burst. Worth measuring audio cb peak before/after — if it ticks up, may need to break the full-viewport paint into smaller chunks with explicit yield points.
- A foreground window that *expects* per-rect world repaints (relies on world pixels being fresh under specific conditions) would break. None observed in upstream code, but worth grep'ing for direct reads of `_mainRT.bits` outside the standard draw path.

**Mitigations**.
- Implement behind a runtime config flag for staged rollout.
- Compare host x86 binary output bit-for-bit with the optimisation on / off on a static-camera scene.
- Test on every camera mode: scrolling, rotating, zooming, opened windows over the world.
- Capture audio cb peak alongside frame time on each device test.

**Effort**. **2-4 weeks** including testing — every iteration is build-ARM + dist + SD-swap + device test, ~30 min per cycle.

### P11. Tile-walk visibility-culling research

**Status**: research-only — implementation specifics depend on what the column dispatch already culls.

**File**: `src/openrct2/paint/tile_element/Paint.TileElement.cpp` and `Paint.cpp`'s `PaintSessionGenerateRotate`.

**Hypothesis**. 11 000 `TileElementPaintSetup` calls per frame is the inner-loop count. The column dispatch already clips tiles outside the visible column. What it doesn't do well: tiles obscured by buildings, tiles below the visible vertical range, tiles whose tallest scenery is below the rt's worldY.

**Approach**.
1. Audit `PaintSessionGenerateRotate` — what culling does it do? `MapIsEdge` rejects out-of-map tiles. Anything else?
2. Profile per-tile-element time to see if obscured tiles are the costly ones.
3. If obscured tiles cost meaningful time, add a per-tile vertical-bound cache (max-z of all elements at this map coord) and reject tiles whose max-z < rt.worldY before walking elements.

**Target**. Halve the inner-loop call count → ~5 ms / frame saved.

**Risk**. **Medium.** Heuristics that skip too much produce missing geometry; too little produces no win.

**Effort**. ~1 week including measurement.

---

## Tier 4 — Investigative / conditional

### P6. Bucketed peep update *(only if needed after Tier 1-3)*

**Note**: game tick is currently running at 19 Hz instead of the target 40 Hz because the render thread is starving the accumulator. If P1+P4 free CPU and the tick rate recovers to 40 Hz, peep update at 11 ms × 40 Hz = 440 ms/sec — actually worse than now. **P6 might become unnecessary or even counterproductive after the render fixes.**

**Decision rule**: re-measure tick rate after P4 lands. If gameTick > 30 ms / frame and tick rate is at 40 Hz, then bucket peeps; otherwise skip.

**Approach (if needed)**. Bucket peeps by `(EntityId.value & 7)`. Each tick processes one bucket. Each peep individually advances its state machine 8 ticks worth in one go (option (b) — same total CPU, lumpier per-peep but accurate state). Tweener interpolation hides the visual chunkiness.

**Risk**. **Medium.** Peep behaviour synchronisation (crowd noise, vehicle queue ordering) might assume per-tick updates. Need to audit consumers.

**Effort**. ~3-5 days.

### P7. Heap-leak audit

**Hypothesis**. RSS growing 70-220 KB/sec during gameplay implies a steady leak somewhere. Likely candidates if P1 doesn't fix it: `PathFinding::ChooseDirection` (~44 calls/sec, may allocate per call), audio decode buffers, scrolling-text cache.

**Test**. After P1 ships, capture again and check RSS slope. If still growing, look at `RssAnon` vs `RssFile` deltas (P10 instrumentation provides this).

**Effort**. unknown.

### P8. SFX resample to 22 kHz

**Status**: deferred. Audio currently has 71 % buffer headroom (12 ms callback peak / 42 ms buffer). Not a bottleneck.

**Effort, if revisited**: ~3 days, packaging implications.

### P9. Verify rev-92 ride-music cap

**Test**. Audio channel breakdown by source type. Snapshot peaks at 22-24 channels — verify how many are ride music vs SFX.

**Effort**. ~1 hour to instrument.

---

## Suggested execution order

1. **P1** (`fixedPaintEntries` cap 256→512) — 1 hour, predictable, low-risk
2. **P10** (autosave + fault-source instrumentation) — half a day, no behavioural change. **Required before P3.**
3. Capture device snapshot — measure P1 effect, attribute fault sources
4. **P3** (heap-trim threshold) — only if P10 confirms heap-source faults; 1 hour
5. **P2** (PROFILED_FUNCTION lite) — half a day; makes future captures honest about their own cost
6. Capture device snapshot — new baseline with measurement bias removed
7. **P5** (UI invalidation: instrument first, then fix) — half a day instrument + 1-2 days fix
8. Capture device snapshot
9. **P4** (per-sweep ViewportPaint memoisation) — 2-4 weeks, structural, high-risk. **Biggest single opportunity.**
10. **P11** (tile-walk visibility culling research) — 1 week
11. **P6** (peep amortisation) — only if tick rate hasn't recovered after P4
12. **P7** (heap leak) — only if P1 didn't address it
13. **P9** (ride-music cap verify) — quick win
14. **P8** (SFX resample) — only if audio re-emerges as bottleneck after P4

Re-capture device snapshots between each step. Headline metrics: **median frame time + p95 + minor-faults / sec peak + ViewportPaint calls / Draw**. Avoid avg frame time (sensitive to outlier autosave stalls).

## Out of scope / not worth doing

- **Drawing engine swap to OpenGL**: impractical (no GPU on Mini, vendor SDL2 expects software path).
- **Multithreaded paint**: already disabled in rev 291 — only 2 cores, audio thread already filling one.
- **NEON SIMD on tile walk**: cost dominated by branch + memory loads, not arithmetic.
- **C++17 → C++20 reversion**: would require Buildroot toolchain rebuild; cut 33 verified clean.
- **Switching from `sfl::static_vector` to `std::vector` for paintEntries**: would *introduce* heap allocs we just removed; static_vector is correct here.
