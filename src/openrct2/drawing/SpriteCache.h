/*****************************************************************************
 * OpenRCT2mini revision 71 — persistent sprite-decode cache.
 *
 * The SpriteScratch (cut 16 + cut 27) holds decoded JSON-object PNG data in
 * a disk-backed (SD on the device) mmap'd file. That scratch is rebuilt from
 * scratch every launch — a per-launch cost of ~1-2 sec PNG decode + ~1-3 sec
 * SD write of ~30 MB. The decoded bytes are perfectly deterministic for a
 * given set of object inputs, so persisting them across launches lets us
 * skip both costs.
 *
 * This module provides:
 *
 * - A 64-bit cache key derived from an object's identifier plus its source
 *   file's mtime+size — so updating an object pack invalidates only the
 *   affected entries.
 *
 * - SpriteCacheLookup(key): on a hit, returns G1Element headers ready to
 *   be pushed into ImageTable, with `offset` fields already pointing into a
 *   process-lifetime mmap of the on-disk cache. ImageTable can use them as-is
 *   and skip the decode loop entirely.
 *
 * - SpriteCacheStore(key, entries, packedPixels): persists a freshly-decoded
 *   ImageTable so the next launch hits.
 *
 * On-disk layout under $ORCT_SCRATCH_DIR (which is on the SD card on the
 * device — see launch.sh / package.sh:361):
 *
 *   objects.cache (append-only):
 *       [16-byte file header: magic "ORCT2MSC", version=1]
 *       Repeating, one per cached object:
 *         [16-byte entry header: magic "ENTRY\0\0\0", numEntries, pixelLen]
 *         [numEntries × 18 bytes: CachedG1 records (packed)]
 *         [pixelLen bytes: packed pixel data]
 *
 *   objects.idx (index — read fully into memory at startup):
 *       [16-byte file header: magic "ORCT2MIX", version=1]
 *       Repeating, one per cached object:
 *         [32 bytes: hash, cacheOffset, totalLen, flags]
 *
 * Atomicity: cache appends are fsync'd before idx appends, so a crash
 * leaves stale data at the end of objects.cache that nothing references.
 * idx corruption (bad magic / truncated record) → blow both files away,
 * rebuild from scratch. Worst case is one full rebuild — same cost as a
 * pre-rev-71 cold launch.
 *
 *****************************************************************************/

#pragma once

#include "G1Element.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenRCT2::Drawing
{
    using SpriteCacheKey = uint64_t;

    /**
     * Build a cache key from an object identifier and its source file path.
     * The path's mtime and size are folded in so that an updated pack
     * invalidates the cached entry. CSG-loaded state is folded in too:
     * objects whose ImageTable depends on noCsgImages branch must not share
     * a key between csg-loaded and csg-missing builds.
     *
     * Returns 0 if path can't be stat'd (caller should treat that as
     * "don't cache this object").
     */
    SpriteCacheKey ComputeSpriteCacheKey(
        std::string_view objectIdentifier, std::string_view sourcePath, bool csgLoaded);

    struct SpriteCacheHit
    {
        // Populated G1Element headers with offset already patched into the
        // process-lifetime mmap of objects.cache. Caller pushes these into
        // ImageTable::_entries with _entryOwnsOffset = false.
        std::vector<G1Element> entries;

        // Whether the original ImageTable::ReadJson would have set
        // usesFallbackSprites = true (i.e. csg-loaded was false at cache-build
        // time and the object had a noCsgImages branch).
        bool usesFallbackSprites;
    };

    // Hit → returns populated entries. Miss / disabled / error → nullopt.
    std::optional<SpriteCacheHit> SpriteCacheLookup(SpriteCacheKey key);

    // Persist a freshly-decoded image table. `entries` should be the
    // post-pack G1Element vector with `offset` fields pointing into the
    // packed pixel buffer. `packedPixels` / `packedSize` is that buffer.
    // Caller computes per-entry pixel-data offsets the same way it does for
    // SpriteScratchAppend; we serialise those offsets relative to packedPixels.
    void SpriteCacheStore(
        SpriteCacheKey key, const std::vector<G1Element>& entries,
        const uint8_t* packedPixels, size_t packedSize, bool usesFallbackSprites);

    // Optionally invoked at startup. Reads objects.idx into the in-memory
    // hash map. Idempotent. If called multiple times, only the first call does
    // any work. SpriteCacheLookup will lazily initialise on first call too,
    // so this is a pure optimisation for skewing the cost out of the hot path.
    void SpriteCacheInit();

    // Diagnostics: returns total bytes of objects.cache + objects.idx, and
    // count of cached entries. Intended for log output / debug overlays.
    struct SpriteCacheStats
    {
        size_t totalBytes = 0;
        size_t entryCount = 0;
    };
    SpriteCacheStats GetSpriteCacheStats();
} // namespace OpenRCT2::Drawing
