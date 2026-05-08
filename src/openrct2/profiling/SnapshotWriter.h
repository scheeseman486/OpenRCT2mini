/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/
#pragma once

// OPENRCT2MINI P9: Snapshot save format. See profiler-plan.md "Snapshot
// save format" for the full container layout.
//
// .orctprof file = 16-byte header (never compressed) + body. The body
// is one or more TLV chunks, optionally zstd-compressed as a single
// frame. Chunks: INFO, SCHM, FRMS, SLWS, PROF, NOTE, ENDF.
//
// The file format version is independent of OpenRCT2mini's release
// version. ANY change to FrameSnapshot or SlowPoll layout, OR any
// change to the chunk set, MUST bump kSnapshotFormatVersion.

#include <cstdint>
#include <string>

#ifdef ENABLE_PERFORMANCE_PROFILER

namespace OpenRCT2::Profiling::Sampler
{
    constexpr uint16_t kSnapshotFormatVersion = 1;

    // Save the current sampler state to disk. Path is the absolute
    // file path to write to (caller responsible for the directory
    // existing and any per-platform path quoting).
    //
    // Returns true on success. On false, no file is written (or any
    // partially-written file is removed by the OS via unlink). The
    // function is synchronous in P9; an async path is tracked as a
    // followup.
    //
    // `note` is an optional one-line user annotation embedded as a
    // NOTE chunk. Pass empty string to omit.
    bool saveSnapshot(const std::string& path, const std::string& note);
} // namespace OpenRCT2::Profiling::Sampler

#endif // ENABLE_PERFORMANCE_PROFILER
