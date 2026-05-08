/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI P1: Performance Profiler window. See profiler-plan.md.
//
// Seven top-edge tabs: Overview, CPU, Memory, I/O, Entities, Paint, Audio.
// P1 ships only the skeleton — tabs are clickable, all pages display
// "(no data yet)". Subsequent phases populate each tab.

#ifdef ENABLE_PERFORMANCE_PROFILER

    #include <algorithm>
    #include <cstdio>
    #include <openrct2-ui/UiStringIds.h>
    #include <openrct2-ui/interface/Widget.h>
    #include <openrct2-ui/interface/Window.h>
    #include <openrct2-ui/windows/Windows.h>
    #include <openrct2/Context.h>
    #include <openrct2/SpriteIds.h>
    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/drawing/Rectangle.h>
    #include <openrct2/drawing/Text.h>
    #include <openrct2/interface/Widget.h>
    #include <openrct2/localisation/Formatter.h>
    #include <openrct2/PlatformEnvironment.h>
    #include <openrct2/core/Path.hpp>
    #include <openrct2/profiling/Sampler.h>
    #include <openrct2/profiling/SnapshotWriter.h>
    #include <openrct2/ui/WindowManager.h>

    #include <chrono>
    #include <ctime>

using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    namespace
    {
        // Window dimensions sized to fit the Mini's 640×480 panel with
        // room left over to coexist with other windows. Width is
        // constant across tabs (graphs need a consistent horizontal
        // span). Height is per-tab — see kPageHeights below.
        constexpr int32_t kProfilerWidth = 440;
        constexpr int32_t kProfilerInitialHeight = 320;
        constexpr ScreenSize kProfilerWindowSize = { kProfilerWidth, kProfilerInitialHeight };

        // Per-tab window heights, measured from each tab's drawing
        // code: last drawn pixel + 4 px padding + 18 px reserved for
        // the Save Snapshot button strip at the bottom. Numbers are
        // rounded up to multiples of 4 for clean alignment.
        //
        // Order matches ProfilerPage enum:
        //   Overview, CPU, Memory, I/O, Entities, Paint, Audio.
        constexpr int32_t kPageHeights[] = {
            284, // Overview — frame-time graph + bottleneck label
            284, // CPU — phase graph + 4-row legend
            272, // Memory — graph + 3-row legend
            332, // I/O — two stacked graphs + 3-row legend
            216, // Entities — bar chart + footer
            312, // Paint — two stacked graphs + 3-row stats
            320, // Audio — callback + channels + 3-row stats
        };

        // Tab strip layout: 7 tabs at 31px each = 217px of strip,
        // starting 3px from the window's left edge.
        constexpr int16_t kTabStart = 3;
        constexpr int16_t kTabWidth = 31;
        constexpr int16_t kTabTop = 17;

        enum ProfilerPage : int32_t
        {
            kPageOverview = 0,
            kPageCpu,
            kPageMemory,
            kPageIo,
            kPageEntities,
            kPagePaint,
            kPageAudio,
            kPageCount,
        };

        enum WidgetIdx : WidgetIndex
        {
            WIDX_BACKGROUND,
            WIDX_TITLE,
            WIDX_CLOSE,
            WIDX_PAGE_BACKGROUND,
            WIDX_TAB_OVERVIEW,
            WIDX_TAB_CPU,
            WIDX_TAB_MEMORY,
            WIDX_TAB_IO,
            WIDX_TAB_ENTITIES,
            WIDX_TAB_PAINT,
            WIDX_TAB_AUDIO,
            WIDX_SAVE_SNAPSHOT,
        };

        // clang-format off
        static const auto kProfilerWidgets = makeWidgets(
            makeWindowShim(STR_PERFORMANCE_PROFILER, kProfilerWindowSize),
            makeWidget({  0,  43}, { kProfilerWindowSize.width, kProfilerWindowSize.height - 43 },
                       WidgetType::resize, WindowColour::secondary),
            makeTab   ({ kTabStart + 0 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_OVERVIEW),
            makeTab   ({ kTabStart + 1 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_CPU     ),
            makeTab   ({ kTabStart + 2 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_MEMORY  ),
            makeTab   ({ kTabStart + 3 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_IO      ),
            makeTab   ({ kTabStart + 4 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_ENTITIES),
            makeTab   ({ kTabStart + 5 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_PAINT   ),
            makeTab   ({ kTabStart + 6 * kTabWidth, kTabTop }, STR_PERFORMANCE_PROFILER_TAB_AUDIO   ),
            // P9: Save Snapshot button — bottom-right of the page area.
            // Always visible regardless of tab; writes the entire ring
            // state to <user>/profiler/snapshot-*.orctprof.
            makeWidget({ kProfilerWindowSize.width - 86, kProfilerWindowSize.height - 18 },
                       { 80, 14 }, WidgetType::button, WindowColour::secondary,
                       STR_PERFORMANCE_PROFILER_SAVE_SNAPSHOT)
        );
        // clang-format on

        // Animated tab sprite mapping. Placeholder set per profiler-plan.md
        // "Tab icons" — polish pass replaces these with bespoke icons.
        struct TabSpriteDef
        {
            uint32_t baseSprite;
            uint8_t frameCount; // 0 → static (single frame)
            uint8_t frameRate;  // ticks per frame; 0 → static
        };
        constexpr TabSpriteDef kTabSprites[kPageCount] = {
            { SPR_TAB_GRAPH_A_0, 8, 4 },                  // Overview
            { SPR_TAB_GEARS_0, 4, 2 },                    // CPU
            { SPR_TAB_FINANCES_SUMMARY_0, 8, 8 },         // Memory
            { SPR_G2_TAB_NEWS, 0, 0 },                    // I/O (static)
            { SPR_TAB_GUESTS_0, 8, 3 },                   // Entities
            // Paint tab — paintbrush, matches Options > Interface tab.
            { SPR_TAB_PAINT_0, 8, 4 },                    // Paint
            // Audio tab — spinning record, matches Options > Audio tab.
            { SPR_TAB_MUSIC_0, 16, 2 },                   // Audio
        };

        class PerformanceProfilerWindow final : public Window
        {
        public:
            void onOpen() override
            {
                setWidgets(kProfilerWidgets);
                widgetSetPressedExclusive(
                    *this, { WIDX_TAB_OVERVIEW, WIDX_TAB_CPU, WIDX_TAB_MEMORY, WIDX_TAB_IO, WIDX_TAB_ENTITIES,
                             WIDX_TAB_PAINT, WIDX_TAB_AUDIO },
                    WIDX_TAB_OVERVIEW + _page);

                // Polish: size the window to match the initial tab's
                // content (Overview). Subsequent tab clicks resize via
                // setPage → applyPageHeight.
                applyPageHeight();

                // P1: enable the sampler when the window opens.
                // P9 will introduce the auto-capture / "stay enabled
                // after window close" lifecycle described in the plan.
                Profiling::Sampler::enable();
            }

            void onClose() override
            {
                // P1: disable the sampler. P9 will keep it running if
                // auto-capture-on-stutter is armed.
                Profiling::Sampler::disable();
            }

            void onUpdate() override
            {
                _frameCounter++;
                // OPENRCT2MINI: rate-limit the full-window invalidate.
                // Each invalidate() marks the profiler's bounds dirty,
                // and the dirty-block dispatcher then redraws every
                // window that overlaps that rect — including the main
                // viewport underneath, which has to re-walk ~440x320 of
                // world content. On the device this is the dominant cost
                // of having the profiler open. Updating live data and
                // tab animations at ~7-15 Hz on host / ~1.5-2 Hz on
                // device is a good trade for half-to-quarter of the
                // profiler-induced viewport overhead.
                //
                // Toast appearance/expiry and tab clicks invalidate
                // explicitly via setPage() / handleSaveSnapshot, so the
                // user-interactive paths still get immediate feedback.
                if ((_frameCounter & 0x3) == 0)
                    invalidate();
            }

            void onMouseUp(WidgetIndex widgetIndex) override
            {
                switch (widgetIndex)
                {
                    case WIDX_CLOSE:
                        close();
                        break;
                    case WIDX_TAB_OVERVIEW:
                    case WIDX_TAB_CPU:
                    case WIDX_TAB_MEMORY:
                    case WIDX_TAB_IO:
                    case WIDX_TAB_ENTITIES:
                    case WIDX_TAB_PAINT:
                    case WIDX_TAB_AUDIO:
                        setPage(widgetIndex - WIDX_TAB_OVERVIEW);
                        break;
                    case WIDX_SAVE_SNAPSHOT:
                        handleSaveSnapshot();
                        break;
                }
            }

            // ----- P9 snapshot button handler --------------------------
            //
            // Composes the path `<user>/profiler/snapshot-YYYYMMDD-HHMMSS.orctprof`,
            // ensures the directory exists, calls SnapshotWriter, stashes
            // the result message for the next draw cycle.
            std::string _snapshotMessage;
            uint32_t _snapshotMessageExpireFrame = 0;

            void handleSaveSnapshot()
            {
                auto& env = GetContext()->GetPlatformEnvironment();

                // Build target directory + path. The user-data root is
                // a u8string we treat as a regular filesystem path.
                std::string userDir = env.GetDirectoryPath(DirBase::user);
                std::string profilerDir = ::OpenRCT2::Path::Combine(userDir, "profiler");
                ::OpenRCT2::Path::CreateDirectory(profilerDir);

                // Filename with current local time.
                const auto now = std::chrono::system_clock::now();
                const std::time_t t = std::chrono::system_clock::to_time_t(now);
                std::tm tm{};
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                char nameBuf[64];
                std::snprintf(
                    nameBuf, sizeof(nameBuf), "snapshot-%04d%02d%02d-%02d%02d%02d.orctprof",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec);

                const std::string filepath = ::OpenRCT2::Path::Combine(profilerDir, std::string(nameBuf));

                const bool ok = Profiling::Sampler::saveSnapshot(filepath, std::string{});
                if (ok)
                {
                    _snapshotMessage = std::string("Saved: ") + nameBuf;
                }
                else
                {
                    _snapshotMessage = std::string("Save failed: ") + nameBuf;
                }
                // Keep the message visible for ~4 seconds at 30 fps.
                _snapshotMessageExpireFrame = _frameCounter + 120;
            }

            void onPrepareDraw() override
            {
                widgetSetPressedExclusive(
                    *this, { WIDX_TAB_OVERVIEW, WIDX_TAB_CPU, WIDX_TAB_MEMORY, WIDX_TAB_IO, WIDX_TAB_ENTITIES,
                             WIDX_TAB_PAINT, WIDX_TAB_AUDIO },
                    WIDX_TAB_OVERVIEW + _page);
            }

            void onDraw(Drawing::RenderTarget& rt) override
            {
                drawWidgets(rt);
                drawTabImages(rt);
                drawPageContent(rt);
                drawSnapshotToast(rt);
            }

            // P9: brief message ribbon shown for ~4 sec after a save
            // attempt. Drawn alongside the Save Snapshot button in the
            // bottom strip — left of the button, vertically aligned
            // with the button's text. Sharing the bottom-strip row
            // means it never overlaps page content (which terminates
            // 4 px above the strip per the kPageHeights budget).
            void drawSnapshotToast(Drawing::RenderTarget& rt)
            {
                if (_snapshotMessage.empty())
                    return;
                if (_frameCounter >= _snapshotMessageExpireFrame)
                {
                    // Clear the message on expiry. Required because
                    // setPage() resets _frameCounter to 0 to restart
                    // the tab icon animation — without this clear,
                    // switching tabs after the toast had faded would
                    // make it reappear (counter < expire frame again).
                    _snapshotMessage.clear();
                    return;
                }
                const int32_t x = windowPos.x + 8;
                const int32_t y = windowPos.y + height - 15;
                drawText(rt, { x, y }, _snapshotMessage, { FontStyle::small });
            }

        private:
            int32_t _page = kPageOverview;
            uint32_t _frameCounter = 0;

            void setPage(int32_t newPage)
            {
                if (newPage == _page)
                    return;
                _page = newPage;
                _frameCounter = 0;
                applyPageHeight();
                invalidate();
            }

            // Resize the window + reposition the bottom-anchored widgets
            // (page background panel, Save Snapshot button) for the
            // current tab's content height. Mirrors how Cheats handles
            // per-tab resizing — set `height`, call resizeFrame(),
            // invalidate.
            void applyPageHeight()
            {
                const int32_t newHeight = kPageHeights[_page];
                if (newHeight == height)
                    return;

                invalidate();
                height = newHeight;

                // Page background panel — extend to the new bottom.
                auto& bg = widgets[WIDX_PAGE_BACKGROUND];
                bg.bottom = newHeight - 1;

                // Save Snapshot button — pinned 4 px above the bottom.
                auto& btn = widgets[WIDX_SAVE_SNAPSHOT];
                btn.top = newHeight - 18;
                btn.bottom = newHeight - 4;

                resizeFrame();
                invalidate();
            }

            void drawTabImages(Drawing::RenderTarget& rt)
            {
                for (int32_t i = 0; i < kPageCount; i++)
                {
                    const auto& def = kTabSprites[i];
                    uint32_t spriteIdx = def.baseSprite;
                    if (def.frameCount > 0 && _page == i)
                    {
                        spriteIdx += (_frameCounter / std::max<uint8_t>(1, def.frameRate)) % def.frameCount;
                    }
                    const auto& tabWidget = widgets[WIDX_TAB_OVERVIEW + i];
                    GfxDrawSprite(
                        rt, ImageId(spriteIdx),
                        windowPos + ScreenCoordsXY{ tabWidget.left, tabWidget.top });
                }
            }

            // Page dispatcher. P2 implements Overview, P3 implements
            // CPU. Other pages still fall through to a placeholder
            // until their phase lands.
            void drawPageContent(Drawing::RenderTarget& rt)
            {
                if (_page == kPageOverview)
                {
                    drawOverviewTab(rt);
                    return;
                }
                if (_page == kPageCpu)
                {
                    drawCpuTab(rt);
                    return;
                }
                if (_page == kPageMemory)
                {
                    drawMemoryTab(rt);
                    return;
                }
                if (_page == kPageIo)
                {
                    drawIoTab(rt);
                    return;
                }
                if (_page == kPageEntities)
                {
                    drawEntitiesTab(rt);
                    return;
                }
                if (_page == kPagePaint)
                {
                    drawPaintTab(rt);
                    return;
                }
                if (_page == kPageAudio)
                {
                    drawAudioTab(rt);
                    return;
                }
                drawPagePlaceholder(rt);
            }

            // -----------------------------------------------------------
            // P2: Overview tab.
            //
            // Layout (target window 440 wide × 380 tall, page area starts
            // ~y=43 of windowPos):
            //
            //   FPS / frame-time readouts        (innerTop .. +30)
            //   Y axis labels   |  graph plot    (innerTop+40 .. +180)
            //                   `--- X axis labels (+182 .. +192)
            //   Bottleneck text                  (innerTop+200 .. +212)
            //
            // The Y axis is fixed at 0..yScaleMs where yScaleMs grows
            // with the recent peak but is FLOOR'd at 50 ms. Single huge
            // outliers (window-open warmup, save loads) are clamped
            // visually but don't compress the rest of the line.
            // -----------------------------------------------------------

            // Tunable. Initial Y axis ceiling. Grows when peak exceeds it,
            // never shrinks back inside one window-open session — that
            // would make the graph jitter every time a transient spike
            // arrives or leaves the ring.
            static constexpr uint32_t kGraphInitialMaxMs = 50;

            // Average frame duration over the last `windowFrames` entries.
            // Returns ms as a double for display; 0 if no samples.
            double averageRecentFrameMs(uint32_t windowFrames) const
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return 0.0;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t take = std::min(count, windowFrames);
                uint64_t sumUs = 0;
                for (uint32_t i = 0; i < take; i++)
                {
                    const uint32_t idx = (head + cap - 1 - i) % cap;
                    sumUs += ring[idx].frameDurationUs;
                }
                return (sumUs / 1000.0) / static_cast<double>(take);
            }

            // Cached "ratchet" Y axis ceiling (ms). Only ever grows in
            // 25 ms steps; never shrinks while the window is open. This
            // gives a stable axis and avoids the "single 200 ms outlier
            // squashes the line" failure mode.
            uint32_t _graphMaxMs = kGraphInitialMaxMs;

            void updateGraphScale()
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return;

                // Use the 95th-ish percentile rather than absolute max so
                // a single warmup spike doesn't blow out the axis. Cheap
                // approximation: skip the worst ~5% by counting outliers.
                uint32_t peakMs = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t durMs = ring[i].frameDurationUs / 1000;
                    if (durMs > peakMs)
                        peakMs = durMs;
                }
                // Reject any single value > 4× the running ceiling — that's
                // an outlier (window-open frame, save load, etc.) which
                // shouldn't drive the axis.
                if (peakMs > _graphMaxMs * 4)
                    peakMs = _graphMaxMs;
                const uint32_t stepped = ((peakMs / 25) + 1) * 25;
                if (stepped > _graphMaxMs)
                    _graphMaxMs = stepped;
            }

            // The actual plot rect, after Y labels carve off the left and
            // X labels carve off the bottom of the outer box.
            ScreenRect plotRectFromOuter(ScreenRect outer) const
            {
                constexpr int32_t kYLabelW = 28;
                constexpr int32_t kXLabelH = 12;
                return ScreenRect{
                    { outer.GetLeft() + kYLabelW, outer.GetTop() + 2 },
                    { outer.GetRight() - 2, outer.GetBottom() - kXLabelH },
                };
            }

            // Draw the plot area + axis labels. `outer` is the full
            // bounding box that includes label gutters.
            void drawFrameTimeGraph(Drawing::RenderTarget& rt, ScreenRect outer)
            {
                const ScreenRect plot = plotRectFromOuter(outer);

                // Plot area border (inset, lighter colour).
                Rectangle::fillInset(rt, plot, colours[1], Rectangle::BorderStyle::inset);

                const uint32_t maxMs = _graphMaxMs;
                const int32_t plotH = plot.GetHeight();
                const int32_t plotW = plot.GetWidth();

                // Y axis: tick + label every kYStepMs ms, gridline.
                constexpr uint32_t kYStepMs = 25;
                for (uint32_t mark = 0; mark <= maxMs; mark += kYStepMs)
                {
                    const int32_t y = plot.GetBottom() - static_cast<int32_t>((mark * plotH) / maxMs);
                    if (mark > 0 && mark < maxMs)
                    {
                        GfxDrawDashedLine(
                            rt, { { plot.GetLeft() + 1, y }, { plot.GetRight() - 1, y } }, 2, PaletteIndex::pi10);
                    }
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", mark);
                    drawText(
                        rt, { plot.GetLeft() - 4, y - 4 }, buf,
                        { FontStyle::small, TextAlignment::right });
                }

                // X axis labels — fixed time scale based on the ring's
                // full capacity, NOT current fill level. The right edge
                // is always "now"; the left edge is always
                // "-PROFILER_HISTORY_SECONDS s". As samples accumulate
                // the line grows leftward from the right edge; once the
                // ring is full, samples scroll off the left.
                {
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", PROFILER_HISTORY_SECONDS);
                    drawText(rt, { plot.GetLeft() + 1, plot.GetBottom() + 2 }, leftBuf, { FontStyle::small });
                    drawText(
                        rt, { plot.GetRight() - 1, plot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                // ----- The actual line ----------------------------------
                //
                // Pin the most recent sample to plot.right, walk backward
                // (newest → oldest) one pixel-step at a time. The X
                // coordinate of any given sample is fixed by its age,
                // not by buffer fill ratio — so the horizontal scale
                // stays consistent across the whole window-open lifetime.
                //
                // pixels-per-sample = plotW / (cap - 1). For 300 samples
                // and a ~410 px plot, ~1.37 px per sample.
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanFrames = cap - 1;

                ScreenCoordsXY prev{};
                bool prevValid = false;
                // k=0 is the newest sample, k=count-1 is the oldest valid.
                // Stops at count, so for a partially-filled buffer the
                // line ends partway across rather than wrapping into
                // garbage slots.
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t durMs = ring[idx].frameDurationUs / 1000;
                    const uint32_t clamped = std::min(durMs, maxMs);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanFrames);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>((static_cast<uint64_t>(clamped) * plotH) / maxMs);
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, PaletteIndex::pi21);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            void drawOverviewTab(Drawing::RenderTarget& rt)
            {
                updateGraphScale();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                // ------- Text readouts ----------------------------------
                const double avg32 = averageRecentFrameMs(32); // ~1 sec at 30 fps
                const double fps = (avg32 > 0.001) ? (1000.0 / avg32) : 0.0;

                char buf[80];
                std::snprintf(buf, sizeof(buf), "FPS: %.1f  (target 40)", fps);
                drawText(rt, { innerLeft, innerTop }, buf, {});

                std::snprintf(buf, sizeof(buf), "Frame time: %.1f ms  (target <= 25)", avg32);
                drawText(rt, { innerLeft, innerTop + 12 }, buf, {});

                // ------- Frame-time graph -------------------------------
                // outer rect has room for Y labels on the left and an X
                // label row at the bottom. Inner plot is computed from it.
                const ScreenRect graphOuter = {
                    { innerLeft, innerTop + 32 },
                    { innerRight, innerTop + 32 + 160 },
                };
                drawFrameTimeGraph(rt, graphOuter);

                // ------- Bottleneck (placeholder until P3) --------------
                drawText(
                    rt, { innerLeft, graphOuter.GetBottom() + 8 },
                    "Bottleneck: (CPU phase data lands in P3)", {});
            }

            // -----------------------------------------------------------
            // P3: CPU tab.
            //
            // Multi-series line graph showing the four registered
            // profiling phases (game tick, peep update, viewport paint,
            // frame draw) overlaid on the same axes. Below the graph,
            // a small legend / stats table:
            //   ● Game tick      8.2 ms (avg 7.5)
            //   ● Peep update    4.1 ms (avg 3.8)
            //   ...
            //
            // Phases come from Sampler's PhaseSlot[] which read the
            // existing PROFILED_FUNCTION counters via the Profiling
            // registry — no new instrumentation in the engine.
            // -----------------------------------------------------------

            struct CpuSeries
            {
                const char* label;
                PaletteIndex paletteIdx;
                uint32_t Profiling::Sampler::FrameSnapshot::*field;
            };

            // Series order matches the phase indices in Sampler.cpp.
            // Colors picked from PaletteIndex.h for visual distinction.
            // pi173 is what the Ride Graphs window uses for "intense"
            // lines — kept that energy here.
            static constexpr CpuSeries kCpuSeries[] = {
                { "Game tick",      PaletteIndex::pi172, &Profiling::Sampler::FrameSnapshot::gameTickUs    },
                { "Peep update",    PaletteIndex::pi102, &Profiling::Sampler::FrameSnapshot::peepUpdateUs  },
                { "Viewport paint", PaletteIndex::pi138, &Profiling::Sampler::FrameSnapshot::paintWalkUs   },
                { "Frame draw",     PaletteIndex::yellow7, &Profiling::Sampler::FrameSnapshot::paintDrawUs },
            };
            // Cached ratchet ceiling for the CPU tab's Y axis. Tracked
            // separately from _graphMaxMs so the two tabs can have
            // different vertical scales (the per-phase numbers are
            // typically smaller than the total frame time).
            uint32_t _cpuMaxMs = kGraphInitialMaxMs;

            void updateCpuGraphScale()
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return;

                uint32_t peakMs = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    for (const auto& s : kCpuSeries)
                    {
                        const uint32_t v = (ring[i].*(s.field)) / 1000;
                        if (v > peakMs)
                            peakMs = v;
                    }
                }
                if (peakMs > _cpuMaxMs * 4)
                    peakMs = _cpuMaxMs;
                const uint32_t stepped = ((peakMs / 25) + 1) * 25;
                if (stepped > _cpuMaxMs)
                    _cpuMaxMs = stepped;
            }

            // Draw a single phase's line over the plot rect using the
            // same right-anchored-fixed-time-scale model as the Overview
            // graph. Skipped if the field's pointer-to-member yields
            // identical zeros across the ring (phase never resolved).
            void drawCpuSeries(
                Drawing::RenderTarget& rt, ScreenRect plot, uint32_t maxMs, const CpuSeries& series)
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanFrames = cap - 1;
                const int32_t plotW = plot.GetWidth();
                const int32_t plotH = plot.GetHeight();

                ScreenCoordsXY prev{};
                bool prevValid = false;
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t durMs = (ring[idx].*(series.field)) / 1000;
                    const uint32_t clamped = std::min(durMs, maxMs);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanFrames);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>((static_cast<uint64_t>(clamped) * plotH) / maxMs);
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, series.paletteIdx);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            // Average a specific field over the last `windowFrames`
            // entries. Returns ms as a double.
            double averageRecentFieldMs(
                uint32_t Profiling::Sampler::FrameSnapshot::*field, uint32_t windowFrames) const
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return 0.0;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t take = std::min(count, windowFrames);
                uint64_t sumUs = 0;
                for (uint32_t i = 0; i < take; i++)
                {
                    const uint32_t idx = (head + cap - 1 - i) % cap;
                    sumUs += ring[idx].*field;
                }
                return (sumUs / 1000.0) / static_cast<double>(take);
            }

            void drawCpuTab(Drawing::RenderTarget& rt)
            {
                updateCpuGraphScale();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                // ------- Header label -----------------------------------
                drawText(rt, { innerLeft, innerTop }, "CPU phases (ms per frame)", {});

                // ------- Multi-series plot ------------------------------
                const ScreenRect graphOuter = {
                    { innerLeft, innerTop + 18 },
                    { innerRight, innerTop + 18 + 130 },
                };
                const ScreenRect plot = plotRectFromOuter(graphOuter);

                Rectangle::fillInset(rt, plot, colours[1], Rectangle::BorderStyle::inset);

                const uint32_t maxMs = _cpuMaxMs;

                // Y axis ticks + labels (every 25 ms, same convention as
                // the Overview graph).
                constexpr uint32_t kYStepMs = 25;
                for (uint32_t mark = 0; mark <= maxMs; mark += kYStepMs)
                {
                    const int32_t y = plot.GetBottom() - static_cast<int32_t>((mark * plot.GetHeight()) / maxMs);
                    if (mark > 0 && mark < maxMs)
                    {
                        GfxDrawDashedLine(
                            rt, { { plot.GetLeft() + 1, y }, { plot.GetRight() - 1, y } }, 2,
                            PaletteIndex::pi10);
                    }
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", mark);
                    drawText(
                        rt, { plot.GetLeft() - 4, y - 4 }, buf,
                        { FontStyle::small, TextAlignment::right });
                }

                // X axis labels (fixed time scale; same as Overview).
                {
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", PROFILER_HISTORY_SECONDS);
                    drawText(rt, { plot.GetLeft() + 1, plot.GetBottom() + 2 }, leftBuf, { FontStyle::small });
                    drawText(
                        rt, { plot.GetRight() - 1, plot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                // Lines.
                for (const auto& s : kCpuSeries)
                {
                    drawCpuSeries(rt, plot, maxMs, s);
                }

                // ------- Stats / legend table ---------------------------
                int32_t rowY = graphOuter.GetBottom() + 14;
                for (const auto& s : kCpuSeries)
                {
                    // Color swatch — small filled rect using the line's
                    // palette index, so the legend is unambiguous.
                    Rectangle::fill(
                        rt,
                        ScreenRect{
                            { innerLeft, rowY + 2 },
                            { innerLeft + 8, rowY + 8 },
                        },
                        s.paletteIdx);

                    const double last = averageRecentFieldMs(s.field, 1);
                    const double avg5s = averageRecentFieldMs(s.field, 150);

                    char buf[80];
                    std::snprintf(
                        buf, sizeof(buf), "%-14s  %5.1f ms   (avg %4.1f)", s.label, last, avg5s);
                    drawText(rt, { innerLeft + 14, rowY }, buf, {});

                    rowY += 12;
                }
            }

            // -----------------------------------------------------------
            // P4: Memory tab.
            //
            // Three series sourced from the SlowPoll ring (1 Hz):
            //   RSS, heap in-use (mallinfo.uordblks), heap mmap (.hblkhd).
            // Below the graph, a small table of the most recent values.
            //
            // Y axis is auto-ratcheted in MB (via _memoryMaxMB) since
            // values range from a few MB to ~100 MB on the device. X
            // axis uses the slow-poll capacity, not frame capacity, so
            // the time scale is e.g. 90s on host (3× the frame history
            // of 30s) and 30s on device.
            // -----------------------------------------------------------

            struct MemSeries
            {
                const char* label;
                PaletteIndex paletteIdx;
                uint32_t Profiling::Sampler::SlowPoll::*field;
            };

            static constexpr MemSeries kMemSeries[] = {
                { "RSS",       PaletteIndex::pi172,   &Profiling::Sampler::SlowPoll::rssKB       },
                { "Heap used", PaletteIndex::pi102,   &Profiling::Sampler::SlowPoll::heapInUseKB },
                { "Heap mmap", PaletteIndex::yellow7, &Profiling::Sampler::SlowPoll::heapMmapKB  },
            };

            uint32_t _memoryMaxMB = 32; // initial ceiling, ratchets up

            void updateMemoryGraphScale()
            {
                const auto& ring = Profiling::Sampler::getSlowPollRing();
                const uint32_t count = Profiling::Sampler::getSlowPollRingCount();
                if (count == 0)
                    return;

                uint32_t peakMB = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    for (const auto& s : kMemSeries)
                    {
                        const uint32_t mb = (ring[i].*(s.field)) / 1024;
                        if (mb > peakMB)
                            peakMB = mb;
                    }
                }
                // No outlier rejection here — memory generally only
                // ratchets up cleanly. Round up to the next 16 MB step.
                const uint32_t stepped = ((peakMB / 16) + 1) * 16;
                if (stepped > _memoryMaxMB)
                    _memoryMaxMB = stepped;
            }

            // Plot a memory series. SlowPoll ring's time axis is the same
            // shape as the frame ring's — most recent at right, oldest
            // sample at left, fixed scale based on capacity.
            void drawMemorySeries(
                Drawing::RenderTarget& rt, ScreenRect plot, uint32_t maxMB, const MemSeries& series)
            {
                const auto& ring = Profiling::Sampler::getSlowPollRing();
                const uint32_t head = Profiling::Sampler::getSlowPollRingHead();
                const uint32_t count = Profiling::Sampler::getSlowPollRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanSamples = cap - 1;
                const int32_t plotW = plot.GetWidth();
                const int32_t plotH = plot.GetHeight();

                ScreenCoordsXY prev{};
                bool prevValid = false;
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t valMB = (ring[idx].*(series.field)) / 1024;
                    const uint32_t clamped = std::min(valMB, maxMB);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanSamples);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>((static_cast<uint64_t>(clamped) * plotH) / maxMB);
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, series.paletteIdx);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            // Most recent value of a SlowPoll field, in KB. Returns 0
            // if the ring is empty.
            uint32_t latestSlowPollKB(uint32_t Profiling::Sampler::SlowPoll::*field) const
            {
                const auto& ring = Profiling::Sampler::getSlowPollRing();
                const uint32_t head = Profiling::Sampler::getSlowPollRingHead();
                const uint32_t count = Profiling::Sampler::getSlowPollRingCount();
                if (count == 0)
                    return 0;
                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t idx = (head + cap - 1) % cap;
                return ring[idx].*field;
            }

            void drawMemoryTab(Drawing::RenderTarget& rt)
            {
                updateMemoryGraphScale();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                drawText(rt, { innerLeft, innerTop }, "Memory (MB, sampled at 1 Hz)", {});

                // Graph.
                const ScreenRect graphOuter = {
                    { innerLeft, innerTop + 18 },
                    { innerRight, innerTop + 18 + 130 },
                };
                const ScreenRect plot = plotRectFromOuter(graphOuter);

                Rectangle::fillInset(rt, plot, colours[1], Rectangle::BorderStyle::inset);

                const uint32_t maxMB = _memoryMaxMB;

                // Y axis: tick + label every (maxMB/4) MB, gridlines.
                const uint32_t yStepMB = std::max<uint32_t>(1, maxMB / 4);
                for (uint32_t mark = 0; mark <= maxMB; mark += yStepMB)
                {
                    const int32_t y = plot.GetBottom() - static_cast<int32_t>((mark * plot.GetHeight()) / maxMB);
                    if (mark > 0 && mark < maxMB)
                    {
                        GfxDrawDashedLine(
                            rt, { { plot.GetLeft() + 1, y }, { plot.GetRight() - 1, y } }, 2,
                            PaletteIndex::pi10);
                    }
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", mark);
                    drawText(
                        rt, { plot.GetLeft() - 4, y - 4 }, buf,
                        { FontStyle::small, TextAlignment::right });
                }

                // X axis labels — slow-poll runs at 1 Hz, so the ring
                // span in seconds == ring capacity directly.
                {
                    const uint32_t cap = static_cast<uint32_t>(
                        Profiling::Sampler::getSlowPollRing().size());
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", cap);
                    drawText(rt, { plot.GetLeft() + 1, plot.GetBottom() + 2 }, leftBuf, { FontStyle::small });
                    drawText(
                        rt, { plot.GetRight() - 1, plot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                for (const auto& s : kMemSeries)
                {
                    drawMemorySeries(rt, plot, maxMB, s);
                }

                // ------- Stats / legend table ---------------------------
                int32_t rowY = graphOuter.GetBottom() + 14;
                for (const auto& s : kMemSeries)
                {
                    Rectangle::fill(
                        rt,
                        ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                        s.paletteIdx);

                    const uint32_t kb = latestSlowPollKB(s.field);
                    char buf[80];
                    if (kb == 0 && Profiling::Sampler::getSlowPollRingCount() > 0)
                    {
                        // Field is genuinely zero or unsupported on this
                        // platform (e.g. mallinfo on non-glibc).
                        std::snprintf(buf, sizeof(buf), "%-12s  --", s.label);
                    }
                    else if (kb >= 1024)
                    {
                        std::snprintf(buf, sizeof(buf), "%-12s  %.1f MB", s.label, kb / 1024.0);
                    }
                    else
                    {
                        std::snprintf(buf, sizeof(buf), "%-12s  %u KB", s.label, kb);
                    }
                    drawText(rt, { innerLeft + 14, rowY }, buf, {});

                    rowY += 12;
                }
            }

            // -----------------------------------------------------------
            // P5: I/O tab.
            //
            // Two stacked plots:
            //   1. Disk read rate (KB/s) — single line, single ratchet
            //      scale. This is the metric the device cares about
            //      most: scrolling causes SpriteScratch page faults
            //      that materialize as SD card reads.
            //   2. Page faults/s — major (red) and minor (green) lines
            //      on a shared Y scale.
            //
            // Data comes from the SlowPoll ring (1 Hz). Same time-axis
            // model as the Memory tab.
            // -----------------------------------------------------------

            uint32_t _ioReadMaxKbps = 64;     // initial: 64 KB/s
            uint32_t _ioFaultsMaxRate = 100;  // initial: 100 faults/s

            void updateIoGraphScale()
            {
                const auto& ring = Profiling::Sampler::getSlowPollRing();
                const uint32_t count = Profiling::Sampler::getSlowPollRingCount();
                if (count == 0)
                    return;

                uint32_t peakKbps = 0;
                uint32_t peakFaults = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t kbps = ring[i].readBytesPerSec / 1024;
                    if (kbps > peakKbps)
                        peakKbps = kbps;
                    const uint32_t faults = std::max(ring[i].majorFaultsPerSec, ring[i].minorFaultsPerSec);
                    if (faults > peakFaults)
                        peakFaults = faults;
                }

                const uint32_t kbpsStepped = ((peakKbps / 64) + 1) * 64;
                if (kbpsStepped > _ioReadMaxKbps)
                    _ioReadMaxKbps = kbpsStepped;

                const uint32_t faultsStepped = ((peakFaults / 100) + 1) * 100;
                if (faultsStepped > _ioFaultsMaxRate)
                    _ioFaultsMaxRate = faultsStepped;
            }

            // Draw a single-series I/O plot. Generic over the SlowPoll
            // field and a unit/scale conversion: caller passes the
            // pointer-to-member, the current axis ceiling, and a
            // divisor so the field's natural units (bytes for reads,
            // count/s for faults) get scaled to display units.
            void drawIoSeries(
                Drawing::RenderTarget& rt, ScreenRect plot, uint32_t maxValue,
                uint32_t Profiling::Sampler::SlowPoll::*field, uint32_t divisor, PaletteIndex colour)
            {
                const auto& ring = Profiling::Sampler::getSlowPollRing();
                const uint32_t head = Profiling::Sampler::getSlowPollRingHead();
                const uint32_t count = Profiling::Sampler::getSlowPollRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanSamples = cap - 1;
                const int32_t plotW = plot.GetWidth();
                const int32_t plotH = plot.GetHeight();

                ScreenCoordsXY prev{};
                bool prevValid = false;
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t raw = ring[idx].*field;
                    const uint32_t scaled = (divisor > 0) ? (raw / divisor) : raw;
                    const uint32_t clamped = std::min(scaled, maxValue);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanSamples);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>(
                                          (static_cast<uint64_t>(clamped) * plotH) / std::max<uint32_t>(1, maxValue));
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, colour);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            // Helper: draw an inset plot rect with Y-axis ticks/labels
            // at evenly-spaced intervals, plus a top-right "max value"
            // annotation. Returns the inner plot rect (after gutters).
            ScreenRect drawIoPlotChrome(
                Drawing::RenderTarget& rt, ScreenRect outer, const char* title, uint32_t maxValue,
                const char* unitSuffix)
            {
                drawText(rt, { outer.GetLeft(), outer.GetTop() - 12 }, title, { FontStyle::small });

                const ScreenRect plot = plotRectFromOuter(outer);
                Rectangle::fillInset(rt, plot, colours[1], Rectangle::BorderStyle::inset);

                const uint32_t step = std::max<uint32_t>(1, maxValue / 4);
                for (uint32_t mark = 0; mark <= maxValue; mark += step)
                {
                    const int32_t y = plot.GetBottom() - static_cast<int32_t>((mark * plot.GetHeight()) / maxValue);
                    if (mark > 0 && mark < maxValue)
                    {
                        GfxDrawDashedLine(
                            rt, { { plot.GetLeft() + 1, y }, { plot.GetRight() - 1, y } }, 2,
                            PaletteIndex::pi10);
                    }
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%u", mark);
                    drawText(
                        rt, { plot.GetLeft() - 4, y - 4 }, buf,
                        { FontStyle::small, TextAlignment::right });
                }

                // Unit suffix top-right of the plot (e.g. "KB/s").
                drawText(
                    rt, { plot.GetRight(), plot.GetTop() - 10 }, unitSuffix,
                    { FontStyle::small, TextAlignment::right });

                return plot;
            }

            void drawIoTab(Drawing::RenderTarget& rt)
            {
                updateIoGraphScale();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                drawText(rt, { innerLeft, innerTop }, "I/O activity (sampled at 1 Hz)", {});

                // ------- Graph 1: disk read rate (KB/s) -----------------
                const ScreenRect readsOuter = {
                    { innerLeft, innerTop + 26 },
                    { innerRight, innerTop + 26 + 80 },
                };
                const ScreenRect readsPlot = drawIoPlotChrome(
                    rt, readsOuter, "Disk reads", _ioReadMaxKbps, "KB/s");
                drawIoSeries(
                    rt, readsPlot, _ioReadMaxKbps,
                    &Profiling::Sampler::SlowPoll::readBytesPerSec, /*divisor=*/1024,
                    PaletteIndex::pi172);

                // ------- Graph 2: page faults/sec -----------------------
                const ScreenRect faultsOuter = {
                    { innerLeft, readsOuter.GetBottom() + 22 },
                    { innerRight, readsOuter.GetBottom() + 22 + 80 },
                };
                const ScreenRect faultsPlot = drawIoPlotChrome(
                    rt, faultsOuter, "Page faults", _ioFaultsMaxRate, "faults/s");
                drawIoSeries(
                    rt, faultsPlot, _ioFaultsMaxRate,
                    &Profiling::Sampler::SlowPoll::majorFaultsPerSec, /*divisor=*/1,
                    PaletteIndex::pi172);
                drawIoSeries(
                    rt, faultsPlot, _ioFaultsMaxRate,
                    &Profiling::Sampler::SlowPoll::minorFaultsPerSec, /*divisor=*/1,
                    PaletteIndex::pi102);

                // X axis labels — one set, anchored to the bottom plot.
                {
                    const uint32_t cap = static_cast<uint32_t>(
                        Profiling::Sampler::getSlowPollRing().size());
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", cap);
                    drawText(
                        rt, { faultsPlot.GetLeft() + 1, faultsPlot.GetBottom() + 2 }, leftBuf,
                        { FontStyle::small });
                    drawText(
                        rt, { faultsPlot.GetRight() - 1, faultsPlot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                // ------- Stats / legend ---------------------------------
                int32_t rowY = faultsOuter.GetBottom() + 14;
                const uint32_t lastReadKbps =
                    latestSlowPollKB(&Profiling::Sampler::SlowPoll::readBytesPerSec);
                const uint32_t lastMajor =
                    latestSlowPollKB(&Profiling::Sampler::SlowPoll::majorFaultsPerSec);
                const uint32_t lastMinor =
                    latestSlowPollKB(&Profiling::Sampler::SlowPoll::minorFaultsPerSec);

                char buf[80];
                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi172);
                std::snprintf(buf, sizeof(buf), "Disk reads     %.1f KB/s", lastReadKbps / 1024.0);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi172);
                std::snprintf(buf, sizeof(buf), "Major faults   %u /s", lastMajor);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi102);
                std::snprintf(buf, sizeof(buf), "Minor faults   %u /s", lastMinor);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
            }

            // -----------------------------------------------------------
            // P6: Entities tab.
            //
            // Per-type entity counts as horizontal bar chart + stats
            // table. Bars are scaled relative to the largest count
            // across all rows so e.g. "Guest 487" fills the full width
            // and other rows scale proportionally.
            //
            // "Update cost" column comes from the existing CPU phase
            // PROFILED_FUNCTION measurements where possible:
            //   Guest    → PeepUpdateAll (already tracked in phases[1])
            //   Staff    → not separately profiled — share with Peep
            //   Vehicle  → not separately profiled — leave blank
            //   Misc    → not separately profiled — leave blank
            // The bare counts are still useful even when update cost
            // is unavailable.
            // -----------------------------------------------------------

            struct EntityRow
            {
                const char* label;
                PaletteIndex barColour;
                uint16_t Profiling::Sampler::FrameSnapshot::*countField;
                // Optional: phase field whose update cost to attribute
                // to this row. nullptr if no separate profiled phase.
                uint32_t Profiling::Sampler::FrameSnapshot::*costField;
            };

            static constexpr EntityRow kEntityRows[] = {
                { "Guest",   PaletteIndex::pi172,   &Profiling::Sampler::FrameSnapshot::guestCount,
                  &Profiling::Sampler::FrameSnapshot::peepUpdateUs },
                { "Staff",   PaletteIndex::pi102,   &Profiling::Sampler::FrameSnapshot::staffCount,
                  nullptr },
                { "Vehicle", PaletteIndex::pi138,   &Profiling::Sampler::FrameSnapshot::vehicleCount,
                  nullptr },
                { "Misc",    PaletteIndex::yellow7, &Profiling::Sampler::FrameSnapshot::miscEntityCount,
                  nullptr },
            };

            // Most recent value of a uint16_t count field. Returns 0
            // if the ring is empty.
            uint16_t latestEntityCount(uint16_t Profiling::Sampler::FrameSnapshot::*field) const
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return 0;
                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t idx = (head + cap - 1) % cap;
                return ring[idx].*field;
            }

            void drawEntitiesTab(Drawing::RenderTarget& rt)
            {
                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                drawText(rt, { innerLeft, innerTop }, "Entity counts (live)", {});

                // Find the max count across all rows so bars share a
                // common scale and the largest fills the available
                // width. Floor at 1 to avoid div-by-zero on empty park.
                uint16_t maxCount = 1;
                for (const auto& row : kEntityRows)
                {
                    const uint16_t v = latestEntityCount(row.countField);
                    if (v > maxCount)
                        maxCount = v;
                }

                // Layout: per row we draw [label | bar | numeric count
                // | update cost]. Column widths picked to fit the 432 px
                // page area.
                constexpr int32_t kLabelW = 70;
                constexpr int32_t kBarMaxW = 200;
                constexpr int32_t kCountW = 60;
                constexpr int32_t kRowH = 18;
                const int32_t barsLeft = innerLeft + kLabelW;
                const int32_t countLeft = barsLeft + kBarMaxW + 8;
                const int32_t costLeft = countLeft + kCountW + 8;

                // Header.
                int32_t rowY = innerTop + 24;
                drawText(rt, { innerLeft, rowY }, "Type", { FontStyle::small });
                drawText(rt, { barsLeft, rowY }, "Live count", { FontStyle::small });
                drawText(rt, { costLeft, rowY }, "Update cost", { FontStyle::small });
                rowY += 14;

                for (const auto& row : kEntityRows)
                {
                    const uint16_t count = latestEntityCount(row.countField);
                    const int32_t barW = (count == 0)
                                             ? 0
                                             : static_cast<int32_t>(
                                                 (static_cast<uint64_t>(count) * kBarMaxW) / maxCount);

                    // Label.
                    drawText(rt, { innerLeft, rowY + 2 }, row.label, {});

                    // Bar background (inset frame for readability).
                    Rectangle::fillInset(
                        rt,
                        ScreenRect{ { barsLeft, rowY + 2 }, { barsLeft + kBarMaxW, rowY + kRowH - 4 } },
                        colours[1], Rectangle::BorderStyle::inset);

                    // Bar fill.
                    if (barW > 0)
                    {
                        Rectangle::fill(
                            rt,
                            ScreenRect{
                                { barsLeft + 1, rowY + 3 },
                                { barsLeft + barW, rowY + kRowH - 5 } },
                            row.barColour);
                    }

                    // Numeric count.
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%u", count);
                    drawText(rt, { countLeft, rowY + 2 }, buf, {});

                    // Update cost (if separately tracked).
                    if (row.costField != nullptr)
                    {
                        const double avgMs = averageRecentFieldMs(row.costField, 32);
                        std::snprintf(buf, sizeof(buf), "%.1f ms", avgMs);
                        drawText(rt, { costLeft, rowY + 2 }, buf, {});
                    }
                    else
                    {
                        drawText(rt, { costLeft, rowY + 2 }, "—", {});
                    }

                    rowY += kRowH;
                }

                // Footer: total + reminder.
                rowY += 6;
                uint32_t total = 0;
                for (const auto& row : kEntityRows)
                {
                    total += latestEntityCount(row.countField);
                }
                char totalBuf[64];
                std::snprintf(totalBuf, sizeof(totalBuf), "Total live entities: %u", total);
                drawText(rt, { innerLeft, rowY }, totalBuf, {});

                rowY += 14;
                drawText(
                    rt, { innerLeft, rowY },
                    "Note: only Guest update cost is profiled separately.",
                    { FontStyle::small });
            }

            // -----------------------------------------------------------
            // P7: Paint tab.
            //
            // Shows what the viewport renderer is doing each frame:
            //   - PaintStruct count graph (peak per frame across
            //     visible viewports — the metric most directly tied
            //     to "how much scenery is on screen")
            //   - Paint column count graph (how the viewport is
            //     subdivided for the paint walk; grows with viewport
            //     width and zoom-out)
            //   - Phase split: Walk vs Draw wall-clock, drawn as
            //     two-line graph mirroring the CPU tab style. We don't
            //     have a separate PaintSessionArrange profile, so the
            //     "arrange" phase is folded into ViewportPaint.
            //
            // The paint tab is the diagnostic for "scrolling is slow"
            // — it lets you see whether the cost is in the walk
            // (entity/tile traversal) or the draw (per-pixel blit).
            // -----------------------------------------------------------

            uint32_t _paintEntriesMaxScale = 256;
            uint32_t _paintColumnsMaxScale = 16;

            void updatePaintGraphScales()
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return;

                uint32_t peakEntries = 0;
                uint32_t peakColumns = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    if (ring[i].paintEntriesUsed > peakEntries)
                        peakEntries = ring[i].paintEntriesUsed;
                    if (ring[i].paintColumnCount > peakColumns)
                        peakColumns = ring[i].paintColumnCount;
                }
                const uint32_t entriesStepped = ((peakEntries / 256) + 1) * 256;
                if (entriesStepped > _paintEntriesMaxScale)
                    _paintEntriesMaxScale = entriesStepped;
                const uint32_t columnsStepped = std::max<uint32_t>(8, ((peakColumns / 8) + 1) * 8);
                if (columnsStepped > _paintColumnsMaxScale)
                    _paintColumnsMaxScale = columnsStepped;
            }

            // Generic line plot over a uint16_t FrameSnapshot field.
            // Used for paintEntriesUsed and paintColumnCount.
            void drawFrameU16Series(
                Drawing::RenderTarget& rt, ScreenRect plot, uint32_t maxValue,
                uint16_t Profiling::Sampler::FrameSnapshot::*field, PaletteIndex colour)
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanFrames = cap - 1;
                const int32_t plotW = plot.GetWidth();
                const int32_t plotH = plot.GetHeight();

                ScreenCoordsXY prev{};
                bool prevValid = false;
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t v = ring[idx].*field;
                    const uint32_t clamped = std::min(v, maxValue);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanFrames);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>(
                                          (static_cast<uint64_t>(clamped) * plotH) / std::max<uint32_t>(1, maxValue));
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, colour);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            uint16_t latestU16Field(uint16_t Profiling::Sampler::FrameSnapshot::*field) const
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return 0;
                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t idx = (head + cap - 1) % cap;
                return ring[idx].*field;
            }

            void drawPaintTab(Drawing::RenderTarget& rt)
            {
                updatePaintGraphScales();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                drawText(rt, { innerLeft, innerTop }, "Paint pipeline", {});

                // ------- Graph 1: PaintStruct peak per frame ------------
                const ScreenRect entriesOuter = {
                    { innerLeft, innerTop + 26 },
                    { innerRight, innerTop + 26 + 70 },
                };
                const ScreenRect entriesPlot = drawIoPlotChrome(
                    rt, entriesOuter, "PaintStructs (peak/frame)", _paintEntriesMaxScale, "");
                drawFrameU16Series(
                    rt, entriesPlot, _paintEntriesMaxScale,
                    &Profiling::Sampler::FrameSnapshot::paintEntriesUsed,
                    PaletteIndex::pi172);

                // ------- Graph 2: paint columns per frame ---------------
                const ScreenRect columnsOuter = {
                    { innerLeft, entriesOuter.GetBottom() + 22 },
                    { innerRight, entriesOuter.GetBottom() + 22 + 70 },
                };
                const ScreenRect columnsPlot = drawIoPlotChrome(
                    rt, columnsOuter, "Paint columns", _paintColumnsMaxScale, "");
                drawFrameU16Series(
                    rt, columnsPlot, _paintColumnsMaxScale,
                    &Profiling::Sampler::FrameSnapshot::paintColumnCount,
                    PaletteIndex::pi138);

                // X axis labels — one set anchored to the bottom plot.
                {
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", PROFILER_HISTORY_SECONDS);
                    drawText(
                        rt, { columnsPlot.GetLeft() + 1, columnsPlot.GetBottom() + 2 }, leftBuf,
                        { FontStyle::small });
                    drawText(
                        rt, { columnsPlot.GetRight() - 1, columnsPlot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                // ------- Stats / table ----------------------------------
                int32_t rowY = columnsOuter.GetBottom() + 14;

                const uint16_t curEntries = latestU16Field(
                    &Profiling::Sampler::FrameSnapshot::paintEntriesUsed);
                const uint16_t curColumns = latestU16Field(
                    &Profiling::Sampler::FrameSnapshot::paintColumnCount);

                char buf[80];
                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi172);
                std::snprintf(buf, sizeof(buf), "PaintStructs   %u", curEntries);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi138);
                std::snprintf(buf, sizeof(buf), "Columns        %u", curColumns);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                // Paint walk wall-clock — already populated by P3 phase
                // tracking via ViewportPaint's PROFILED_FUNCTION.
                const double paintWalkAvgMs = averageRecentFieldMs(
                    &Profiling::Sampler::FrameSnapshot::paintWalkUs, 32);
                std::snprintf(buf, sizeof(buf), "ViewportPaint  %.1f ms (avg)", paintWalkAvgMs);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
            }

            // -----------------------------------------------------------
            // P8: Audio tab.
            //
            // Two stacked plots:
            //   1. Audio callback duration (µs) with a horizontal
            //      "deadline" line marking the SDL buffer's budget.
            //      Originally why the profiler exists: showing how
            //      close we are to underruns when scrolling on the
            //      Mini. The deadline line comes from the audio
            //      buffer's samples/freq — recorded once at AudioMixer
            //      init via Sampler::recordAudioBufferMs.
            //   2. Active channel count (sound effects + ride music).
            //
            // Stats below: callback peak/avg, current channels, buffer
            // budget, and headroom (budget - peak).
            // -----------------------------------------------------------

            uint32_t _audioCallbackMaxUs = 50000;  // 50 ms initial
            uint32_t _audioChannelsMaxScale = 8;

            void updateAudioGraphScales()
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count == 0)
                    return;

                uint32_t peakUs = 0;
                uint32_t peakChannels = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    if (ring[i].audioCallbackPeakUs > peakUs)
                        peakUs = ring[i].audioCallbackPeakUs;
                    if (ring[i].audioChannelCount > peakChannels)
                        peakChannels = ring[i].audioChannelCount;
                }

                // Audio callback peak: round to next 10ms step. Floor
                // at twice the current deadline so the deadline line is
                // visible when we're not stressed.
                const uint16_t deadlineMs = Profiling::Sampler::getAudioBufferMs();
                const uint32_t floorUs = std::max<uint32_t>(50000, static_cast<uint32_t>(deadlineMs) * 2 * 1000);
                const uint32_t steppedUs = ((peakUs / 10000) + 1) * 10000;
                if (steppedUs > _audioCallbackMaxUs)
                    _audioCallbackMaxUs = steppedUs;
                if (floorUs > _audioCallbackMaxUs)
                    _audioCallbackMaxUs = floorUs;

                const uint32_t channelsStepped = std::max<uint32_t>(4, ((peakChannels / 4) + 1) * 4);
                if (channelsStepped > _audioChannelsMaxScale)
                    _audioChannelsMaxScale = channelsStepped;
            }

            // Single-series u32 plot for the callback-duration graph.
            // Same right-anchored fixed-time-scale model as the rest.
            void drawFrameU32Series(
                Drawing::RenderTarget& rt, ScreenRect plot, uint32_t maxValue,
                uint32_t Profiling::Sampler::FrameSnapshot::*field, uint32_t divisor, PaletteIndex colour)
            {
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                if (count < 2)
                    return;

                const uint32_t cap = static_cast<uint32_t>(ring.size());
                const uint32_t spanFrames = cap - 1;
                const int32_t plotW = plot.GetWidth();
                const int32_t plotH = plot.GetHeight();

                ScreenCoordsXY prev{};
                bool prevValid = false;
                for (uint32_t k = 0; k < count; k++)
                {
                    const uint32_t idx = (head + cap - 1 - k) % cap;
                    const uint32_t raw = ring[idx].*field;
                    const uint32_t scaled = (divisor > 0) ? (raw / divisor) : raw;
                    const uint32_t clamped = std::min(scaled, maxValue);
                    const int32_t x = plot.GetRight()
                                      - static_cast<int32_t>((static_cast<uint64_t>(k) * plotW) / spanFrames);
                    const int32_t y = plot.GetBottom()
                                      - static_cast<int32_t>(
                                          (static_cast<uint64_t>(clamped) * plotH) / std::max<uint32_t>(1, maxValue));
                    if (prevValid)
                    {
                        GfxDrawLine(rt, { prev, { x, y } }, colour);
                    }
                    prev = { x, y };
                    prevValid = true;
                }
            }

            void drawAudioTab(Drawing::RenderTarget& rt)
            {
                updateAudioGraphScales();

                const int32_t innerLeft = windowPos.x + 8;
                const int32_t innerRight = windowPos.x + width - 8;
                const int32_t innerTop = windowPos.y + 50 + getTitleBarDiffNormal();

                drawText(rt, { innerLeft, innerTop }, "Audio (callback wall-clock)", {});

                // ------- Graph 1: callback duration ms with deadline ----
                const ScreenRect callOuter = {
                    { innerLeft, innerTop + 26 },
                    { innerRight, innerTop + 26 + 80 },
                };
                const uint32_t maxMs = _audioCallbackMaxUs / 1000;
                const ScreenRect callPlot = drawIoPlotChrome(
                    rt, callOuter, "Callback duration", maxMs, "ms");

                // Overlay the deadline as a dashed horizontal line. Only
                // drawn if we know the deadline (recordAudioBufferMs has
                // been called by AudioMixer::Init).
                const uint16_t deadlineMs = Profiling::Sampler::getAudioBufferMs();
                if (deadlineMs > 0 && maxMs > 0)
                {
                    const int32_t y = callPlot.GetBottom()
                                      - static_cast<int32_t>(
                                          (static_cast<uint64_t>(deadlineMs) * callPlot.GetHeight()) / maxMs);
                    GfxDrawDashedLine(
                        rt, { { callPlot.GetLeft() + 1, y }, { callPlot.GetRight() - 1, y } }, 4,
                        PaletteIndex::pi172);
                    char dlBuf[24];
                    std::snprintf(dlBuf, sizeof(dlBuf), "deadline %ums", deadlineMs);
                    drawText(
                        rt, { callPlot.GetRight() - 1, y - 10 }, dlBuf,
                        { FontStyle::small, TextAlignment::right });
                }

                drawFrameU32Series(
                    rt, callPlot, maxMs,
                    &Profiling::Sampler::FrameSnapshot::audioCallbackPeakUs, /*divisor=*/1000,
                    PaletteIndex::yellow7);

                // ------- Graph 2: active channels -----------------------
                const ScreenRect chOuter = {
                    { innerLeft, callOuter.GetBottom() + 22 },
                    { innerRight, callOuter.GetBottom() + 22 + 70 },
                };
                const ScreenRect chPlot = drawIoPlotChrome(
                    rt, chOuter, "Active channels", _audioChannelsMaxScale, "");
                drawFrameU16Series(
                    rt, chPlot, _audioChannelsMaxScale,
                    &Profiling::Sampler::FrameSnapshot::audioChannelCount,
                    PaletteIndex::pi102);

                // X axis labels — anchored to the bottom plot.
                {
                    char leftBuf[16];
                    std::snprintf(leftBuf, sizeof(leftBuf), "-%us", PROFILER_HISTORY_SECONDS);
                    drawText(
                        rt, { chPlot.GetLeft() + 1, chPlot.GetBottom() + 2 }, leftBuf,
                        { FontStyle::small });
                    drawText(
                        rt, { chPlot.GetRight() - 1, chPlot.GetBottom() + 2 }, "now",
                        { FontStyle::small, TextAlignment::right });
                }

                // ------- Stats / table ----------------------------------
                int32_t rowY = chOuter.GetBottom() + 14;

                // Direct pull of latest values from the most recent frame.
                const auto& ring = Profiling::Sampler::getFrameRing();
                const uint32_t head = Profiling::Sampler::getFrameRingHead();
                const uint32_t count = Profiling::Sampler::getFrameRingCount();
                uint32_t lastCallUs = 0;
                uint16_t lastChannels = 0;
                if (count > 0)
                {
                    const uint32_t cap = static_cast<uint32_t>(ring.size());
                    const uint32_t idx = (head + cap - 1) % cap;
                    lastCallUs = ring[idx].audioCallbackPeakUs;
                    lastChannels = ring[idx].audioChannelCount;
                }
                const double avgCallMs = averageRecentFieldMs(
                    &Profiling::Sampler::FrameSnapshot::audioCallbackPeakUs, 32);

                char buf[80];
                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::yellow7);
                std::snprintf(
                    buf, sizeof(buf), "Callback peak  %.1f ms (avg %.1f)",
                    lastCallUs / 1000.0, avgCallMs);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                Rectangle::fill(
                    rt, ScreenRect{ { innerLeft, rowY + 2 }, { innerLeft + 8, rowY + 8 } },
                    PaletteIndex::pi102);
                std::snprintf(buf, sizeof(buf), "Channels       %u active", lastChannels);
                drawText(rt, { innerLeft + 14, rowY }, buf, {});
                rowY += 12;

                if (deadlineMs > 0)
                {
                    const double headroomMs = deadlineMs - (lastCallUs / 1000.0);
                    std::snprintf(
                        buf, sizeof(buf), "Buffer budget  %u ms  (headroom %.1f ms)",
                        deadlineMs, headroomMs);
                    drawText(rt, { innerLeft + 14, rowY }, buf, {});
                }
                else
                {
                    drawText(
                        rt, { innerLeft + 14, rowY }, "Buffer budget  — (audio not initialised)",
                        {});
                }
            }

            // Catch-all for tabs whose phase hasn't shipped yet.
            void drawPagePlaceholder(Drawing::RenderTarget& rt)
            {
                StringId pageNameId = STR_PERFORMANCE_PROFILER_TAB_OVERVIEW + _page;
                const auto pageX = windowPos.x + width / 2;
                // Enlarged UI grows the title bar by 11 px and resizeFrame
                // shifts every body widget down by that amount; our manually
                // drawn content needs the same offset to stay below the tabs.
                const auto pageY = windowPos.y + 80 + getTitleBarDiffNormal();

                drawText(rt, { pageX, pageY }, pageNameId, {}, { TextAlignment::centre });

                char buf[64];
                std::snprintf(
                    buf, sizeof(buf), "ring fill: %u / %u",
                    Profiling::Sampler::getFrameRingCount(),
                    static_cast<uint32_t>(Profiling::Sampler::getFrameRing().size()));
                drawText(rt, { pageX, pageY + 16 }, buf, { TextAlignment::centre });
            }
        };
    } // namespace

    WindowBase* PerformanceProfilerOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::performanceProfiler);
        if (window == nullptr)
        {
            // sceneInvariant: the profiler is exempt from the WindowInitAll
            // and scenario-start close-alls so it persists across title <->
            // park, park <-> park (savegame load), and editor transitions.
            // The draw paths defensively try/catch around getGameState() so
            // empty-game and null-park states are safe; the Sampler's frame
            // ring is process-lifetime and continues sampling throughout —
            // a transition shows up as a frame-time spike in the graph
            // rather than a window dismissal.
            window = windowMgr->Create<PerformanceProfilerWindow>(
                WindowClass::performanceProfiler, ScreenCoordsXY(32, 32), kProfilerWindowSize,
                WindowFlags{ WindowFlag::sceneInvariant });
        }
        return window;
    }

} // namespace OpenRCT2::Ui::Windows

#endif // ENABLE_PERFORMANCE_PROFILER
