/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

// OPENRCT2MINI revision 64: gate all "[OPENRCT2MINI] checkpoint: ..." style
// diagnostic logging behind a compile-time flag. Only enabled when
// CMAKE_BUILD_TYPE=Debug (or -DOPENRCT2MINI_DEBUG=1 is passed explicitly).
//
// Every call site uses one of these two macros instead of raw fputs / fprintf
// to stderr, so a release build elides the whole thing — no compiled-in
// strings, no fd writes, no stderr flushes, no perceptible overhead.
//
// Note: the SIGSEGV backtrace handler and the OOM diagnostics in
// src/openrct2-ui/Ui.cpp DO NOT use these macros. Those are unconditional —
// when a release build crashes we still want enough on stderr for the user
// to file a useful bug report. They're a few lines and only fire on actual
// failure paths, so they don't pollute normal operation.
//
// Use:
//   MINI_DBG_PUTS("preloader: jobs done");
//   MINI_DBG_LOG("present #%u — _bits %zu non-zero\n", n, count);

#include <cstdio>

#ifdef OPENRCT2MINI_DEBUG
    #define MINI_DBG_PUTS(msg)                                                      \
        do                                                                          \
        {                                                                           \
            std::fputs("[OPENRCT2MINI] " msg "\n", stderr);                         \
            std::fflush(stderr);                                                    \
        } while (0)

    #define MINI_DBG_LOG(fmt, ...)                                                  \
        do                                                                          \
        {                                                                           \
            std::fprintf(stderr, "[OPENRCT2MINI] " fmt, ##__VA_ARGS__);             \
            std::fflush(stderr);                                                    \
        } while (0)
#else
    #define MINI_DBG_PUTS(msg) ((void)0)
    #define MINI_DBG_LOG(fmt, ...) ((void)0)
#endif
