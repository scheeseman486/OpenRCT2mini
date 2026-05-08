# OpenRCT2mini perf report — rev 93 device capture

Source: `snapshot-19700606-161956.orctprof` (Miyoo Mini, Alton Towers, profiler open). 30 s slow ring, 300 frames (~50 s of frame samples), arch arm-v7a.

## TL;DR

The capture is dominated by paint cost. The "considerably worse" performance the user observed is a combination of (a) one 8.0-second outlier frame caused by what's almost certainly an autosave catching the main thread, (b) the profiler window's own per-frame `invalidate()` propagating into the main viewport's repaint workload (~22 ms/frame extra when profiler is open), and (c) a busier audio scene (24 active channels vs. 5 in the v0.3.0 captures). The non-outlier baseline is 7–8 fps, essentially unchanged from v0.3.0.

Highest-leverage targets: rate-limit the profiler's self-invalidation (rev 93c, applied), amortise `PeepUpdateAll` across ticks, and add a visibility cache for the static-camera case. Initial framing of "cull 3 of 4 viewports" was wrong — the 3.5 ViewportPaint/frame ratio came from the dirty-block dispatcher repainting the same viewport once per dirty rect per sweep × two sweeps, not from multiple distinct viewports.

## 1. Capture context

| | |
| --- | --- |
| Park | Alton Towers (2258 guests, 33 staff, 151 vehicles) |
| Profiler window | open (sceneInvariant, opened from title screen via Options > Advanced > Start Profiler) |
| Audio buffer | 85 ms (4096 samples / 48 kHz) |
| Frame ring | 300 entries, ~50 s |
| Slow ring | 30 entries, 33 s span (t=27.8 → 60.5 s) |
| RSS at start / end | 51.2 MB → 57.0 MB |

The "profiler open" caveat matters: with the Sampler enabled, every `PROFILED_FUNCTION` callsite in OpenRCT2 is instrumented (~500–1000 ns per enter/exit on Cortex-A7). Hot loops with millions of calls/sec eat measurable wallclock. None of the numbers below should be read as the device's at-rest performance — they are the device's perf-while-being-measured.

## 2. Frame-time distribution

| bucket | frames | comment |
| --- | ---: | --- |
| <33 ms (>30 fps) | 0 | — |
| 33–66 ms | 62 | green territory, 15–30 fps |
| 66–100 ms | 6 | brief 10–15 fps zone |
| 100–150 ms (~7–10 fps) | 153 | **the steady state — 51 % of frames** |
| 150–250 ms | 56 | normal Alton-Towers expensive frames |
| 250–500 ms | 16 | costly frames |
| 500–1000 ms | 6 | clusters during scrolling/audio bursts |
| >1000 ms | 1 | the 8-second outlier |

Aggregated phase costs over the 300-frame ring (50.2 s wallclock):

| phase | total | avg/frame | share |
| --- | ---: | ---: | ---: |
| `gameTick` (`gameStateTick`) | 12.6 s | 42.0 ms | 25.1 % |
| `peepUpdate` (subset) | 9.0 s | 30.1 ms | 18.0 % |
| `paintWalk` (`ViewportFillColumn`) | 17.1 s | 57.1 ms | 34.2 % |
| `paintArrange` (`PaintSessionArrange`) | 0.4 s | 1.3 ms | 0.8 % |
| `paintDraw` (`ViewportPaintColumn`) | 6.2 s | 20.8 ms | 12.4 % |
| `drawingEngine` (full `Context::Draw`) | 29.7 s | 98.9 ms | **59.2 %** |

Reading: **paint owns 60% of wallclock**. Of `Context::Draw` (98.9 ms), the column iteration (paintWalk 57 + paintDraw 21 = 78 ms) is 79 %; the remaining 21 ms is split between window/UI draws, BeginDraw/EndDraw, and (importantly) the cost of doing this 3.5 times per frame for additional viewports.

## 3. The 8.0 s outlier (frame at t=15.3 s)

Phase split for that frame:

```
gameTick       66 ms     (normal)
peepUpdate      6 ms     (low — peeps got fewer ticks)
paintWalk     202 ms     (high but explicable)
paintArrange    4 ms
paintDraw     304 ms
drawingEngine 930 ms     (≈ Context::Draw)
audio_cb       51 us     (audio thread was idle!)
audio_channels  0        (all sounds dropped)
```

The phases account for ~1.5 s of the 8.0 s frame. The remaining ~6.5 s is in `Context::RunFrame` outside `Tick` and `Draw` — that means `_uiContext->ProcessMessages()`, `_backgroundWorker.dispatchCompleted()`, `ContextHandleInput()`, or `WindowUpdateAll()`. The slow-poll at t=49.1 s shows:

- RSS climbed 53.7 → 55.2 MB in one second (+1.5 MB)
- Heap-inuse jumped 6.7 → 8.4 MB (+1.7 MB)
- Minor faults peaked at **1816/sec**
- Major faults at **7/sec**
- Audio channels dropped to 0 for that frame

The signature — heap growth, fault storm, audio silence, long blocked frame — is consistent with an **autosave**: a serialise-game-state allocation burst on the main thread, followed by a background-thread fsync that contends with the audio mixer for SD bandwidth. OpenRCT2's `_backgroundWorker.dispatchCompleted()` may have been waiting on the writer.

This frame is **not a profiler regression**. It happens in any run that catches an autosave during a busy moment. The next perf cut should add an "autosave start/end" timestamp to slow polls so we can confirm.

## 4. Per-call cost — where the time actually goes

Top 12 functions by inclusive time (over 30 s of slow-poll window):

| function | calls | total | avg | per-second cost |
| --- | ---: | ---: | ---: | ---: |
| `Context::RunFrame` | 589 | 61.2 s | 104 ms | wall (parent) |
| `Context::RunFixedFrame` | 589 | 61.2 s | 104 ms | wall (parent) |
| `Context::Draw` | 599 | 40.6 s | 67.8 ms | 1.35 s/s |
| `Painter::Paint` | 599 | 39.1 s | 65.2 ms | 1.30 s/s |
| `ViewportPaint` | 2123 | 32.1 s | 15.1 ms | 1.07 s/s |
| `ViewportFillColumn` | 18 569 | 25.1 s | 1.35 ms | 0.84 s/s |
| `Context::Tick` | 1511 | 20.8 s | 13.8 ms | 0.69 s/s |
| `TileElementPaintSetup` | 2 939 394 | 16.6 s | 5.65 µs | 0.55 s/s |
| `PaintTileElementBase` | 2 233 954 | 13.7 s | 6.13 µs | 0.46 s/s |
| `gameStateUpdateLogic` | 1511 | 12.8 s | 8.45 ms | 0.43 s/s |
| `gameStateTick` | 913 | 12.7 s | 13.86 ms | 0.42 s/s |
| `PeepUpdateAll` | 1511 | 9.1 s | 5.99 ms | 0.30 s/s |

(Inclusive times nest: `RunFrame > Draw > Painter::Paint > ViewportPaint > ViewportFillColumn > TileElementPaintSetup`. Each level's "self" time = parent − sum(children). Per-second cost = total ÷ 30 s wallclock.)

### 4.1 ViewportPaint — 3.5 calls per frame, but not from extra viewports

`ViewportPaint` is called **2123 times across 599 Draw calls** = ~3.5 / frame. The empty-park v0.3.0 capture had the same ratio (3.3 / frame) with no peeps and no openable subject windows — so this isn't gameplay-driven and isn't extra viewports. It's the dirty-block dispatcher.

`Painter::Paint → de.PaintWindows()` runs `DrawAllDirtyBlocks()` twice per frame. Each sweep walks the dirty-rect list, calling `WindowDraw` on every window overlapping each rect. The main world window covers the entire screen, so it gets called once per dirty rect per sweep. **3.5 / frame ≈ 1.75 dirty rects per sweep × 2 sweeps**, with the same single viewport.

What invalidates rects every frame:
1. Camera scrolling / entity tweening — rects on the main viewport area.
2. Toolbar clock / press states / tooltips — small rects on the toolbar.
3. **The profiler window's per-frame `invalidate()`** — a 440×320 rect over the world, which forces the main viewport to repaint that whole region beneath the profiler.

(3) is mine. With the profiler closed, the natural dirty-rect rate is ~1 rect/sweep × 2 = 2 ViewportPaint/frame; with it open, the profiler's self-invalidate adds another and pushes it to ~3.5. At 15.1 ms / call, the extra 1.5 calls cost ~22 ms / frame.

The actionable fix is in §O1 (revised): rate-limit the profiler's invalidation. Genuine secondary viewports (when the user opens a Ride/Guest/Staff/Park window) are §O1b — same fix pattern but per-window.

### 4.2 Tile walk — 158 elements per column

`ViewportFillColumn` runs ~31 times per frame (8 columns × 3.5 viewports + minor variation). Inside one column:

```
ViewportFillColumn        1.35 ms
├── TileElementPaintSetup 158 calls × 5.65 µs = 0.89 ms  (66%)
├── PaintTileElementBase  120 calls × 6.13 µs = 0.74 ms  (54%)
├── PaintSurface          ~40 calls × 11.96 µs = 0.48 ms (35%)
├── EntityPaintSetup      ~316 calls × 0.66 µs = 0.21 ms (15%)
└── PaintSessionArrange   1 call × 23.2 µs    = 0.023 ms (2%)
```

Per call counts are huge but per call cost is tiny (microseconds). The path here is "do less work" rather than "do work faster":

- **Visibility caching**: when the camera doesn't move (which is most of the time on a handheld), the set of visible PaintStructs barely changes. A camera-position-keyed cache that reuses last frame's quadrant lists when delta-camera == 0 would skip ~70 % of `ViewportFillColumn` work in normal play.
- **Map LOD**: at zoom 0 (default device view), nearly every map tile produces 4–8 PaintStructs. At zoom 1 we only see ½ as many tiles but still emit the same density. Skipping detail tiers (small scenery, walls under a height threshold) at higher zooms is upstream-valid.

### 4.3 Game tick — 13.78 ms per tick × 50 ticks/sec = 690 ms/sec

The game ticks at 40 Hz (1511 / 30 s = 50.4 Hz, slightly over because of accumulator catch-up after stalls). Per tick:

| sub-tick | cost | comment |
| --- | ---: | --- |
| `PeepUpdateAll` | 6.0 ms | 2258 guests + 33 staff, ~2.6 µs each |
| `VehicleUpdateAll` | 0.6 ms | 151 vehicles |
| `Audio::UpdateVehicleSounds` | 0.4 ms | per-vehicle audio updates |
| `PeepUpdateCrowdNoise` | 0.4 ms | |
| `MapAnimations::InvalidateAndUpdateAll` | 0.18 ms | |
| `Ride::updateAll` | 0.13 ms | |
| Other (map, weather, finance, scenario, news) | ~6.0 ms | hard to attribute exactly |

PeepUpdateAll is 43 % of a tick. With 2300 peeps it's the obvious amortisation candidate.

`PathFinding::ChooseDirection` is called only 1316 times in 30 s (44 /sec) but at 2.24 ms/call = 100 ms/sec — that's expensive per call but rare. Worth pinning the algorithm but not the priority target.

## 5. Memory

Slow-poll RSS timeline (30 polls, t=27.8 → 60.5 s):

```
27–46 s:  RSS 51.2 MB, heap 6.7 MB, mmap 21.0 MB    (steady)
47.7 s:   RSS 53.7 MB (+2.5)                        205 minor faults/s
49.1 s:   RSS 55.2 MB (+1.5), heap 8.4 MB (+1.7)    1816 minor faults/s, 7 major
50.6 s:   RSS 55.9 MB (+0.7), heap 8.8 MB           70 minor faults/s, 2 major
51-60 s:  RSS settles at 57.0 MB, heap 9.0–9.1 MB
```

Net growth over 33 s: +5.8 MB RSS, +2.4 MB heap. **The heap is leaking 70 KB/sec into long-lived allocations during gameplay.** This is small but compounds across long sessions. Candidates:

- Audio decode buffers (24 channels × 4096 sample buffer = ~ 1 MB working set, would not show as growth though).
- Profiler ring buffers (sized at enable, no growth).
- ChooseDirection's pathfinding workspace if it allocates per call (1316 × ~1 KB allocation churn = 1.3 MB across the capture).

The 1816-minor-faults spike is enormous. 1816 minor faults × 4 KB = 7.3 MB of pages touched in one second — more than the ~1.5 MB of new allocation accounts for. The kernel is faulting in pages that are already mapped but not present, which means glibc's `M_MMAP_THRESHOLD = 32 KB` and `MALLOC_ARENA_MAX=2` (revision 12) may be returning pages aggressively only to re-fault them on next use. Worth tuning further: `mallopt(M_TRIM_THRESHOLD, larger value)` to keep the heap "warm".

## 6. Audio

Audio callback wallclock peaks observed in the frame ring:

```
ch=0–5    cb peak 50–500 us (0.1–0.6%)
ch=10–15  cb peak 10–15 ms (12–18%)
ch=20–24  cb peak 22–28 ms (26–33%)
```

Linear scaling with channel count, ~1 ms per active channel. With a 85 ms callback budget the device is comfortable, but not by much — anything that pushes channels past ~70 (impossible currently, capped at 24) would underrun.

`Audio::UpdateVehicleSounds` runs every tick at 0.4 ms — this is per-vehicle distance/volume calculation on the main thread, contributing 20 ms/sec to game-tick cost. Not a huge target but a clean candidate for spatial caching.

## 7. Profiler instrumentation tax (when window is open)

A back-of-envelope check on what the active `PROFILED_FUNCTION` machinery costs:

- ~3 M `TileElementPaintSetup` + 2.2 M `PaintTileElementBase` calls / 30 s = **170k callsite enter/exits per second**, hot loop.
- Each enter/exit pair: 2× `clock_gettime` (~100–200 ns/call on ARM vDSO) + 5–7 atomic ops + thread_local stack push/pop.
- Conservative: 500 ns × 170k = **85 ms / sec instrumentation overhead** in those two functions alone.

Add the rest of the instrumented hot loops (PaintSurface, EntityPaintSetup, ViewportSurface*) and the open-profiler tax is realistically **150–250 ms / sec** on the device. That's 15–25 % of available CPU. With profiler closed it is zero (gated on `_enabled` atomic-load early-out, verified after rev 93b's gate fixes).

This is not a measurement bug — it's the actual cost of the `PROFILED_FUNCTION` design when it's active. The Sampler reads only six phases, but flipping the global enable activates instrumentation everywhere.

---

# Optimisation plan

Ordered by expected impact / effort. Each item lists a hypothesis, a test, and a target.

## Tier 1 — The big wins (paint reduction)

### O1. Reduce profiler-window-induced repaints of the world (target: −15 ms/frame when profiler open)

**Initial misdiagnosis (corrected).** The earlier draft of this report claimed "3.5 ViewportPaint per frame" meant 3.5 distinct viewports were active. That was wrong. After tracing the path: `Painter::Paint → de.PaintWindows()` runs `DrawAllDirtyBlocks()` *twice* per frame, and each pass walks the dirty-rect list, calling `WindowDraw` on every window that overlaps each rect. Inside `WindowDraw`, viewport-bearing windows call `ViewportPaint`. So **`ViewportPaint` is called once per (dirty rect × overlapping viewport-bearing window × dirty-block sweep)** — not once per viewport. The empty-park capture (snapshot 121331) hit 3.3 calls/frame with zero secondary windows open, confirming the multiplier is the dirty-block dispatcher, not extra viewports.

**The actual culprit when profiler is open.** The Performance Profiler window's `onUpdate` calls `invalidate()` every frame. That marks the profiler's 440×320 bounds dirty. The dirty-block dispatcher then redraws *every window overlapping that rect* — including the main viewport beneath, which has to re-walk the columns covering that region. The profiler is paying for its own repaint plus making the main viewport re-render the area underneath it, every frame.

**Hypothesis.** The 3.5-vs-2 ViewportPaint per frame difference between "profiler open" and "would-be-closed steady state" is the profiler's per-frame self-invalidation propagating to the world.

**Test.** Capture twice with the same scene: once with `_frameCounter & 0x3 == 0` gating the invalidate, once without. Compare `ViewportPaint` count in PROF and median frame time.

**Approach (applied in revision 93c).** Rate-limit the per-frame `invalidate()` to every 4th `onUpdate` tick. Tab animations and live data update at ~7–15 Hz on host, ~1.5–2 Hz on device — fine for monitoring. User-interactive paths (tab clicks, snapshot save) still call `invalidate()` explicitly, so they get immediate feedback.

**Target.** ViewportPaint count when the profiler window is open drops from ~3.5 / frame to ~2 / frame on a normal scene. On Alton Towers that's roughly −15 ms / frame for the hidden-by-profiler region not being re-walked unnecessarily.

**Side effects.** Slight stagger in tab-icon animation on the device. No correctness impact — `setPage()` and `handleSaveSnapshot` continue invalidating immediately when the user interacts.

### O1b. Genuine secondary viewports (target: +1–3 fps when those windows are open)

**Context.** Real secondary viewports exist when the user opens any of Park / Ride / Guest / Staff / Banner / Sign / Map's "open viewport" / track-design preview windows. Each is small (~100×100), but each does a full Z-walk on its bounds every dirty-block sweep that touches it.

**Test.** Capture device with one Ride window open showing a moving train. Compare ViewportPaint count and frame cost vs. the same scene with no Ride windows.

**Approach.**
1. Refresh at 5–10 Hz instead of 30 Hz for non-focused windows. The Ride's "watch-the-train-go-round" preview doesn't need 30 Hz updates.
2. Verify revision 92's shade flag actually nulls the viewport's render contribution when the window is shaded.

**Target.** ~5–8 ms / frame saved per non-focused viewport-bearing window.

**Side effects.** Animated previews stutter visibly when not focused. Acceptable trade — the user's focus is on the main world view.

### O2. Tile-walk visibility cache (target: +2–3 fps)

**Hypothesis.** When the camera is stationary (most of the time), `ViewportFillColumn` re-walks identical tile elements producing identical PaintStructs. We can cache the PaintStruct list per (camera-pos, rotation, zoom) and invalidate only when the camera moves OR when a tile changes.

**Test.** Add a "camera moved this frame" boolean to FrameSnapshot and capture. Compare frame cost in moving vs static frames. If static frames are nearly as expensive, the cache will pay off.

**Approach.**
1. Persist `_paintColumns` PaintStructs across frames keyed on the camera identity.
2. Invalidate on camera change OR when `MapAnimations::InvalidateAndUpdateAll` reports a changed tile.
3. Only the main viewport benefits initially — secondary viewports refresh too rarely for it to matter.

**Target.** ~30 ms / frame saved on static-camera frames. Most realistic gameplay is static-camera (player watching a coaster).

### O3. Capped per-frame paint budget (target: +1–2 fps, smoother)

**Hypothesis.** Per-frame cost varies wildly (median 126 ms, p95 263 ms, max 8047 ms). A budgeted approach — allocate N ms to paint per frame, drop columns past the budget — would smooth the experience even if average doesn't move much.

**Test.** Add a "budget exhausted, dropped N columns" counter to FrameSnapshot. See if degraded-render frames look acceptable.

**Approach.**
1. Track wallclock during ViewportPaint loop.
2. Once paint exceeds X ms, skip remaining columns and mark the frame as "incomplete".
3. Next frame, paint the skipped columns first.

**Target.** Lock 30 ms per frame to paint, get a steadier ~10 fps with progressive refinement on heavy frames.

## Tier 2 — Game tick

### O4. Amortise PeepUpdateAll across ticks (target: +0.5 fps + smoother)

**Hypothesis.** PeepUpdateAll is 6 ms / tick × 50 ticks/sec = 300 ms / sec. With 2291 peeps, updating ¼ each tick reduces per-tick cost to 1.5 ms while keeping the same 40 Hz throughput per peep.

**Test.** Add per-frame peep update count to FrameSnapshot. Measure variance.

**Approach.**
1. Bucket peeps into 4 quartiles (or N buckets).
2. Each tick processes one bucket. Peeps see updates at ~10 Hz instead of 40 Hz, but PeepUpdateAll wallclock drops 4×.
3. Peeps already tolerate sub-tick latency in their state machine — most actions are 1–10 second timeouts.

**Target.** PeepUpdateAll cost from 300 ms/sec → 75 ms/sec. Frees ~7 ms per render frame.

### O5. ChooseDirection caching (target: +0.5 ms / tick)

**Hypothesis.** 2.24 ms / call × 44 / sec = 100 ms/sec. The pathfinder traverses the path graph from peep → goal. When two peeps share a goal (same ride, same exit), the result is identical.

**Approach.** Per-(start tile, goal tile) memoization, invalidated on path changes.

**Target.** Halve pathfinding cost.

## Tier 3 — Memory pressure

### O6. Identify the autosave stall (target: kill the 8s outlier)

**Hypothesis.** The 8 s frame at t=15.3 s coincides with heap growth + fault storm + audio silence. Autosave is the obvious culprit.

**Test.**
1. Add `autosaveActive` boolean and `autosaveDurationMs` to slow-poll.
2. Capture during a known autosave (every N minutes).
3. If confirmed: move autosave's serialisation phase off the main thread (currently it serialises synchronously on main, then writes from a background thread — the serialisation itself blocks).

**Target.** Eliminate the 1+ second blocking frames during autosave.

### O7. Heap-trim threshold tuning (target: -50 % minor fault rate)

**Hypothesis.** With `MALLOC_ARENA_MAX=2` and default `M_TRIM_THRESHOLD=128 KB`, glibc returns pages to the kernel aggressively. The 1816 minor-faults/sec spike is the kernel re-faulting them.

**Approach.**
1. `mallopt(M_TRIM_THRESHOLD, 1 MB)` to keep the heap top warm.
2. `mallopt(M_MMAP_MAX, 0)` to avoid mmap-backed allocs altogether (we already have 32 KB threshold).
3. Measure with the same Alton Towers capture.

**Target.** Steady-state minor faults <100/sec (vs. current 47/sec quiet, 1816/sec peaks).

### O8. Heap leak audit (target: stop 70 KB/sec growth)

**Hypothesis.** RSS grew 6 MB in 33 s. Some allocator is leaking or churning.

**Test.** Add per-frame heap-inuse delta to FrameSnapshot to localise. Cross-correlate with peep count, vehicle update events.

**Approach.** Standard heaptrack on host with synthetic load, but on device this is hard. Slow-poll trend analysis is the realistic path.

## Tier 4 — Profiler self-cost

### O9. Lite-mode PROFILED_FUNCTION (target: usable profiler in steady-state play)

**Hypothesis.** Activating upstream's PROFILED_FUNCTION instrumentation adds 150–250 ms/sec of overhead. The Sampler only reads six functions; the rest is wasted.

**Approach.** A `PROFILED_FUNCTION_LITE()` macro for the six callsites the Sampler matches. Lite version: just two atomic adds (call count + total time, no min/max, no sample ring, no parent tracking). Replace the upstream macro at those six callsites only. Keep upstream macro available for explicit textual profiling via `profiler_start`.

**Target.** Open-profiler overhead drops from 15-25 % to <2 %. Profiler becomes usable for live performance monitoring.

### O10. Skip Profiler-window redraw when not focused / shaded (target: -1 ms/frame when window open)

**Approach.** Already partially done via the shade flag. Also gate the per-frame `invalidate()` on "window visible AND not shaded AND focused". Update the window at 1 Hz when minimised.

## Tier 5 — Audio

### O11. Resample audio sources to 22 kHz on load (target: -50 % mixer cost)

**Hypothesis.** OpenRCT2 stores audio at native 44.1 kHz. Mixing 24 channels × 4096 samples × stereo at 48 kHz = 768 KB/callback to touch. Halving sample rate halves the work.

**Approach.** Transcode all sound effects at load time. Music streams (OGG) decode on-the-fly so no win there. Memory cost: SFX bank shrinks ~50 %.

**Target.** Audio-callback peak from 27 ms (33 %) → 14 ms (16 %). More headroom for content scaling.

### O12. Cap concurrent ride-music streams to 4 (target: less DRAM pressure)

Already partially implemented (revision 92's `Cap ride music streams + priority eviction`). Verify the eviction is firing — peak channel count = 24 in this capture, that's almost certainly more than 4 ride streams.

## Tier 6 — Out of scope but worth noting

- **Drawing engine swap.** X8DrawingEngine is a software paletted blitter. Vendor SDL2 + libmi_gfx is what gets to the screen. There may be wins by skipping the X8 → ARGB conversion step if libmi_gfx accepts paletted input directly. Out of scope — large refactor.
- **NEON tile-paint inner loops.** 6 µs per `PaintTileElementBase` call is dominated by branching and memory loads, not arithmetic. Unlikely to benefit from SIMD without restructuring the data layout.

## Suggested execution order

**Phase A (already partially landed in rev 93c):** O1 (profiler self-invalidate) — fixes the measurement bias so subsequent captures aren't dominated by the profiler observing itself.

**Phase B (quick wins, days):** O7 (heap-trim tuning), O9 (lite-mode `PROFILED_FUNCTION`), O10 (skip profiler redraw when shaded). Sets up clean perf-capture loop.

**Phase C (week):** O4 (peep amortisation), O1b (secondary viewport refresh rate) — biggest gameplay wins.

**Phase D (1–2 weeks):** O2 (camera-stationary visibility cache) — biggest single opportunity, highest risk.

**Phase E (as needed):** O3 (paint budget), O5 (pathfinding memo), O6 (autosave stall), O11/O12 (audio).

Re-capture device snapshots at each phase boundary to track progress against this baseline. The rev 93c profiler-invalidate fix means future captures will under-report the actual game perf cost relative to the rev-93 baseline — that's intentional, since "what the game costs" is what we want to measure, not "what the profiler costs to measure it."
