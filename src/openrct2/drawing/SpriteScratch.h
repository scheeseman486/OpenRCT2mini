/*****************************************************************************
 * OpenRCT2mini Sprite Scratch — process-lifetime scratch file holding decoded
 * per-object sprite data, mmap'd so the kernel can page out cold images.
 *
 * The G1Element offsets that are set up by ImageTable::Read keep being raw
 * pointers, but they point into a file-backed mmap region. Pages that aren't
 * actively rendered can be reclaimed by the kernel under pressure and brought
 * back from the scratch file on demand — turning a permanent ~100 MB RSS cost
 * into a working set bounded by what's actually being rendered.
 *
 * Lifetime: the scratch file is created in /tmp (or $TMPDIR) on first call to
 * Append(), unlinked immediately so it cleans up when the process exits.
 *****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace OpenRCT2::Drawing
{
    // Append `bytes` (length `size`) to the scratch file and return a stable
    // pointer to the mmap'd location. Pointer is valid for the process lifetime.
    // Returns nullptr on failure (caller should fall back to heap).
    uint8_t* SpriteScratchAppend(const void* bytes, size_t size);

    // Diagnostics: total bytes ever appended to the scratch file.
    size_t SpriteScratchTotalSize();

    // Hint the kernel that every page of every scratch mapping is currently cold.
    // Use this after a load phase that touched many sprites (e.g. Object loading at
    // launch / scenario load) to drop the loader's working-set cost back to nothing.
    // Pages fault back in transparently when the renderer next touches them.
    void SpriteScratchEvict();

    // Same as SpriteScratchEvict, but skips the work unless at least `minAppendsToTrigger`
    // SpriteScratchAppend calls have happened since the last eviction. Cheap to call on
    // every tick: lock-free fast path when nothing changed. Returns true if it evicted.
    bool SpriteScratchEvictIfIdle(uint32_t minAppendsToTrigger = 16);
}
