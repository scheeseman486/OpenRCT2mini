/*****************************************************************************
 * OpenRCT2mini revision 71 — persistent sprite-decode cache implementation.
 * See SpriteCache.h for design rationale.
 *****************************************************************************/

#include "SpriteCache.h"
#include "../Diagnostic.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace OpenRCT2::Drawing
{
    namespace
    {
        // 18-byte packed on-disk G1 record. Mirrors G1Element minus the
        // pointer (replaced by an offset within this entry's pixel data).
#pragma pack(push, 1)
        struct CachedG1
        {
            uint32_t pixelOffset;   // bytes from start of this entry's pixel data, or 0xFFFFFFFF for null
            int16_t width;
            int16_t height;
            int16_t xOffset;
            int16_t yOffset;
            uint16_t flagsHolder;
            int32_t zoomedOffset;
        };
        static_assert(sizeof(CachedG1) == 18, "CachedG1 must be 18 bytes packed");

        struct CacheFileHeader
        {
            char magic[8];          // "ORCT2MSC"
            uint32_t version;       // 1
            uint32_t reserved;      // 0
        };
        static_assert(sizeof(CacheFileHeader) == 16);

        struct IdxFileHeader
        {
            char magic[8];          // "ORCT2MIX"
            uint32_t version;       // 1
            uint32_t reserved;      // 0
        };
        static_assert(sizeof(IdxFileHeader) == 16);

        struct EntryHeader
        {
            char magic[8];          // "ENTRY\0\0\0"
            uint32_t numEntries;
            uint32_t pixelDataLen;
        };
        static_assert(sizeof(EntryHeader) == 16);

        struct IdxRecord
        {
            uint64_t hash;
            uint64_t cacheOffset;   // byte offset into objects.cache pointing at the EntryHeader
            uint64_t totalLen;      // bytes from EntryHeader through end of pixel data
            uint64_t flags;         // bit 0 = usesFallbackSprites
        };
        static_assert(sizeof(IdxRecord) == 32);
#pragma pack(pop)

        constexpr uint64_t kFlagUsesFallbackSprites = 1ull << 0;

        constexpr char kCacheMagic[8] = { 'O', 'R', 'C', 'T', '2', 'M', 'S', 'C' };
        constexpr char kIdxMagic[8]   = { 'O', 'R', 'C', 'T', '2', 'M', 'I', 'X' };
        constexpr char kEntryMagic[8] = { 'E', 'N', 'T', 'R', 'Y', '\0', '\0', '\0' };

        // FNV-1a (standard byte-wise variant) for the cache key. Different
        // from the OpenRCT2 64-bit-block FNV1a we use for park files; the
        // hash is purely internal so we use the simpler form.
        constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
        constexpr uint64_t kFnvPrime  = 0x00000100000001B3ull;

        uint64_t fnv1a(const void* data, size_t len, uint64_t seed = kFnvOffset)
        {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            uint64_t h = seed;
            for (size_t i = 0; i < len; i++)
            {
                h ^= p[i];
                h *= kFnvPrime;
            }
            return h;
        }

        struct CacheState
        {
            std::mutex lock;
            bool initialised = false;
            bool disabled = false;       // set true on irrecoverable error
            int cacheFd = -1;
            int idxFd = -1;
            uint8_t* cacheMmap = nullptr;
            size_t cacheMmapSize = 0;
            std::unordered_map<SpriteCacheKey, IdxRecord> index;
            std::string cachePath;
            std::string idxPath;
        };

        CacheState& State()
        {
            static CacheState s;
            return s;
        }

        // Resolves the directory to use. Same priority order as SpriteScratch
        // (ORCT_SCRATCH_DIR → /var/tmp → $XDG_CACHE_HOME → $HOME/.cache → $TMPDIR
        // → /tmp). Returns empty string if none usable.
        std::string ResolveCacheDir()
        {
            const char* candidates[6] = { nullptr };
            int n = 0;
            const char* env = std::getenv("ORCT_SCRATCH_DIR");
            if (env && *env) candidates[n++] = env;
            candidates[n++] = "/var/tmp";
            env = std::getenv("XDG_CACHE_HOME");
            if (env && *env) candidates[n++] = env;
            static char homeCache[256];
            env = std::getenv("HOME");
            if (env && *env)
            {
                std::snprintf(homeCache, sizeof(homeCache), "%s/.cache", env);
                candidates[n++] = homeCache;
            }
            env = std::getenv("TMPDIR");
            if (env && *env) candidates[n++] = env;
            candidates[n++] = "/tmp";

            for (int i = 0; i < n; i++)
            {
                (void)::mkdir(candidates[i], 0700);
                struct stat st {};
                if (::stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
                {
                    return std::string(candidates[i]);
                }
            }
            return {};
        }

        // Re-opens both files from scratch and validates magic / version.
        // Returns false on irrecoverable error (we then disable the cache).
        // On format mismatch, deletes the offending files so the next call
        // creates fresh ones.
        bool OpenOrCreate(CacheState& s)
        {
            const auto dir = ResolveCacheDir();
            if (dir.empty())
            {
                LOG_ERROR("SpriteCache: no usable cache directory");
                return false;
            }
            s.cachePath = dir + "/objects.cache";
            s.idxPath   = dir + "/objects.idx";

            // Open or create the cache file.
            s.cacheFd = ::open(s.cachePath.c_str(), O_RDWR | O_CREAT, 0600);
            if (s.cacheFd < 0)
            {
                LOG_ERROR("SpriteCache: open %s failed: %s", s.cachePath.c_str(), std::strerror(errno));
                return false;
            }

            // If empty, write the header.
            struct stat cst {};
            if (::fstat(s.cacheFd, &cst) != 0)
            {
                LOG_ERROR("SpriteCache: fstat cache failed: %s", std::strerror(errno));
                return false;
            }
            if (cst.st_size == 0)
            {
                CacheFileHeader h{};
                std::memcpy(h.magic, kCacheMagic, 8);
                h.version = 1;
                if (::write(s.cacheFd, &h, sizeof(h)) != static_cast<ssize_t>(sizeof(h)))
                {
                    LOG_ERROR("SpriteCache: write cache header failed");
                    return false;
                }
                ::fsync(s.cacheFd);
            }
            else
            {
                CacheFileHeader h{};
                if (::pread(s.cacheFd, &h, sizeof(h), 0) != static_cast<ssize_t>(sizeof(h))
                    || std::memcmp(h.magic, kCacheMagic, 8) != 0 || h.version != 1)
                {
                    // Format mismatch — wipe and start over. Truncate and
                    // re-call this function once.
                    LOG_WARNING("SpriteCache: cache file header invalid, rebuilding");
                    ::close(s.cacheFd);
                    s.cacheFd = -1;
                    ::unlink(s.cachePath.c_str());
                    ::unlink(s.idxPath.c_str());
                    return OpenOrCreate(s);
                }
            }

            // Same dance for the idx file.
            s.idxFd = ::open(s.idxPath.c_str(), O_RDWR | O_CREAT, 0600);
            if (s.idxFd < 0)
            {
                LOG_ERROR("SpriteCache: open %s failed: %s", s.idxPath.c_str(), std::strerror(errno));
                return false;
            }
            struct stat ist {};
            if (::fstat(s.idxFd, &ist) != 0)
            {
                LOG_ERROR("SpriteCache: fstat idx failed: %s", std::strerror(errno));
                return false;
            }
            if (ist.st_size == 0)
            {
                IdxFileHeader h{};
                std::memcpy(h.magic, kIdxMagic, 8);
                h.version = 1;
                if (::write(s.idxFd, &h, sizeof(h)) != static_cast<ssize_t>(sizeof(h)))
                {
                    LOG_ERROR("SpriteCache: write idx header failed");
                    return false;
                }
                ::fsync(s.idxFd);
                ist.st_size = sizeof(h);
            }
            else
            {
                IdxFileHeader h{};
                if (::pread(s.idxFd, &h, sizeof(h), 0) != static_cast<ssize_t>(sizeof(h))
                    || std::memcmp(h.magic, kIdxMagic, 8) != 0 || h.version != 1)
                {
                    LOG_WARNING("SpriteCache: idx file header invalid, rebuilding");
                    ::close(s.cacheFd); s.cacheFd = -1;
                    ::close(s.idxFd);   s.idxFd   = -1;
                    ::unlink(s.cachePath.c_str());
                    ::unlink(s.idxPath.c_str());
                    return OpenOrCreate(s);
                }
            }

            // Read all idx records into the in-memory map. Truncated tail
            // records are silently dropped (treated as if they were never
            // committed — a partial-write recovery).
            const size_t recordsBytes = static_cast<size_t>(ist.st_size) - sizeof(IdxFileHeader);
            const size_t numRecords = recordsBytes / sizeof(IdxRecord);
            s.index.reserve(numRecords);
            for (size_t i = 0; i < numRecords; i++)
            {
                IdxRecord rec{};
                off_t off = static_cast<off_t>(sizeof(IdxFileHeader) + i * sizeof(IdxRecord));
                if (::pread(s.idxFd, &rec, sizeof(rec), off) != static_cast<ssize_t>(sizeof(rec)))
                {
                    LOG_WARNING("SpriteCache: idx record %zu truncated, ignoring tail", i);
                    break;
                }
                // De-dup: a key may appear multiple times if an object was
                // updated in place. Last record wins.
                s.index[rec.hash] = rec;
            }

            // Mmap the cache file for reads. We map exactly the current size
            // ONCE at startup and never remap during the run. SpriteCacheStore
            // appends to the file but does not update the mapping or in-memory
            // index — entries written this run only become visible on the
            // NEXT run (when OpenOrCreate replays the idx file from disk and
            // mmaps the new cache size).
            //
            // Why: on 32-bit ARM (Miyoo Mini, 3 GB user VA), repeatedly
            // mmap-ing a growing file across hundreds of stores per park-load
            // accumulates the mappings as a leaked arithmetic series — easily
            // multiple GB of VA reserved on a cold-cache run, which exhausts
            // the 32-bit address space and crashes mid-load. Skipping the
            // remap costs us at most one extra decode per object that gets
            // looked up twice in the same run (which does not happen for
            // ImageTable::ReadJson — it's called once per identifier).
            if (cst.st_size > static_cast<off_t>(sizeof(CacheFileHeader)))
            {
                s.cacheMmapSize = static_cast<size_t>(cst.st_size);
                void* base = ::mmap(nullptr, s.cacheMmapSize, PROT_READ, MAP_SHARED, s.cacheFd, 0);
                if (base == MAP_FAILED)
                {
                    LOG_ERROR("SpriteCache: mmap cache failed: %s", std::strerror(errno));
                    s.cacheMmap = nullptr;
                    s.cacheMmapSize = 0;
                }
                else
                {
                    s.cacheMmap = static_cast<uint8_t*>(base);
                }
            }

            LOG_VERBOSE("SpriteCache: opened, %zu entries, %zu bytes", s.index.size(), s.cacheMmapSize);
            return true;
        }

        void EnsureInit(CacheState& s)
        {
            if (s.initialised) return;
            s.initialised = true;
            if (!OpenOrCreate(s))
            {
                s.disabled = true;
                if (s.cacheFd >= 0) { ::close(s.cacheFd); s.cacheFd = -1; }
                if (s.idxFd >= 0)   { ::close(s.idxFd);   s.idxFd   = -1; }
            }
        }

        // (RemapCache removed in revision 71b: leaking per-store mmaps blew
        // out 32-bit ARM virtual address space on cold-cache park loads. The
        // initial OpenOrCreate mapping is now the only mmap of the cache file
        // for the lifetime of the process; entries appended this run only
        // become visible on the next run.)
    } // anon namespace

    void SpriteCacheInit()
    {
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        EnsureInit(s);
    }

    SpriteCacheKey ComputeSpriteCacheKey(
        std::string_view objectIdentifier, std::string_view sourcePath, bool csgLoaded)
    {
        if (sourcePath.empty()) return 0;

        // Stat the source so we can fold mtime + size into the hash.
        struct stat st {};
        std::string path(sourcePath);
        if (::stat(path.c_str(), &st) != 0)
        {
            return 0;
        }

        uint64_t h = kFnvOffset;
        h = fnv1a(objectIdentifier.data(), objectIdentifier.size(), h);
        h = fnv1a(&st.st_size, sizeof(st.st_size), h);
        h = fnv1a(&st.st_mtime, sizeof(st.st_mtime), h);
        const uint8_t csgFlag = csgLoaded ? 1 : 0;
        h = fnv1a(&csgFlag, sizeof(csgFlag), h);
        // Avoid 0 as a key (we use it as a sentinel for "don't cache").
        if (h == 0) h = 1;
        return h;
    }

    std::optional<SpriteCacheHit> SpriteCacheLookup(SpriteCacheKey key)
    {
        if (key == 0) return std::nullopt;
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        EnsureInit(s);
        if (s.disabled || s.cacheMmap == nullptr) return std::nullopt;

        auto it = s.index.find(key);
        if (it == s.index.end()) return std::nullopt;

        const IdxRecord& rec = it->second;
        if (rec.cacheOffset + rec.totalLen > s.cacheMmapSize)
        {
            LOG_WARNING("SpriteCache: idx record references beyond cache file (corrupt); ignoring");
            return std::nullopt;
        }

        const uint8_t* base = s.cacheMmap + rec.cacheOffset;

        EntryHeader hdr{};
        std::memcpy(&hdr, base, sizeof(hdr));
        if (std::memcmp(hdr.magic, kEntryMagic, 8) != 0)
        {
            LOG_WARNING("SpriteCache: entry magic mismatch at offset %llu, ignoring", static_cast<unsigned long long>(rec.cacheOffset));
            return std::nullopt;
        }

        const uint8_t* g1Block = base + sizeof(hdr);
        const uint8_t* pixelBlock = g1Block + hdr.numEntries * sizeof(CachedG1);
        if (pixelBlock + hdr.pixelDataLen > s.cacheMmap + s.cacheMmapSize)
        {
            LOG_WARNING("SpriteCache: entry extends past cache mapping, ignoring");
            return std::nullopt;
        }

        SpriteCacheHit hit;
        hit.entries.reserve(hdr.numEntries);
        for (uint32_t i = 0; i < hdr.numEntries; i++)
        {
            CachedG1 c{};
            std::memcpy(&c, g1Block + i * sizeof(CachedG1), sizeof(c));
            G1Element e{};
            e.offset = (c.pixelOffset == 0xFFFFFFFFu)
                ? nullptr
                : const_cast<uint8_t*>(pixelBlock + c.pixelOffset);
            e.width = c.width;
            e.height = c.height;
            e.xOffset = c.xOffset;
            e.yOffset = c.yOffset;
            e.flags = G1Flags{ c.flagsHolder };
            e.zoomedOffset = c.zoomedOffset;
            hit.entries.push_back(e);
        }
        hit.usesFallbackSprites = (rec.flags & kFlagUsesFallbackSprites) != 0;
        return hit;
    }

    void SpriteCacheStore(
        SpriteCacheKey key, const std::vector<G1Element>& entries,
        const uint8_t* packedPixels, size_t packedSize, bool usesFallbackSprites)
    {
        if (key == 0) return;
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        EnsureInit(s);
        if (s.disabled || s.cacheFd < 0 || s.idxFd < 0) return;

        // Compute per-entry pixel offsets relative to packedPixels, in the
        // same order as ImageTable's cut-27 layout. The G1Element::offset
        // for cached entries already points into packedPixels, so a simple
        // pointer subtraction does it.
        const size_t numEntries = entries.size();
        std::vector<CachedG1> serialised;
        serialised.reserve(numEntries);
        for (const auto& e : entries)
        {
            CachedG1 c{};
            if (e.offset == nullptr)
            {
                c.pixelOffset = 0xFFFFFFFFu;
            }
            else
            {
                const ptrdiff_t delta = e.offset - packedPixels;
                if (delta < 0 || static_cast<size_t>(delta) >= packedSize)
                {
                    // Entry references something outside the packed buffer
                    // (e.g. a $G1[..] index into the runtime g1.dat atlas).
                    // Can't cache this — its referent isn't part of our
                    // storable bytes. Bail and skip storing this object.
                    return;
                }
                c.pixelOffset = static_cast<uint32_t>(delta);
            }
            c.width = e.width;
            c.height = e.height;
            c.xOffset = e.xOffset;
            c.yOffset = e.yOffset;
            c.flagsHolder = e.flags.holder;
            c.zoomedOffset = e.zoomedOffset;
            serialised.push_back(c);
        }

        // Append to objects.cache: [EntryHeader][G1 records][pixel data].
        EntryHeader hdr{};
        std::memcpy(hdr.magic, kEntryMagic, 8);
        hdr.numEntries = static_cast<uint32_t>(numEntries);
        hdr.pixelDataLen = static_cast<uint32_t>(packedSize);

        const off_t cacheOffset = ::lseek(s.cacheFd, 0, SEEK_END);
        if (cacheOffset < 0)
        {
            LOG_ERROR("SpriteCache: lseek cache failed: %s", std::strerror(errno));
            return;
        }

        // Write header + g1 records + pixels in three writes; not atomic but
        // a crash mid-way leaves stale data nothing references (idx isn't
        // updated yet).
        if (::write(s.cacheFd, &hdr, sizeof(hdr)) != static_cast<ssize_t>(sizeof(hdr))) return;
        const size_t g1BlockSize = serialised.size() * sizeof(CachedG1);
        if (g1BlockSize > 0
            && ::write(s.cacheFd, serialised.data(), g1BlockSize) != static_cast<ssize_t>(g1BlockSize))
            return;
        if (packedSize > 0 && packedPixels != nullptr
            && ::write(s.cacheFd, packedPixels, packedSize) != static_cast<ssize_t>(packedSize))
            return;
        ::fsync(s.cacheFd);

        const uint64_t totalLen = sizeof(hdr) + g1BlockSize + packedSize;

        // Append the idx record. If this fsync happens, the cache entry is
        // committed; if not, the cache tail is orphaned (next run sees no
        // matching idx record, treats as miss, re-decodes).
        IdxRecord rec{};
        rec.hash = key;
        rec.cacheOffset = static_cast<uint64_t>(cacheOffset);
        rec.totalLen = totalLen;
        rec.flags = usesFallbackSprites ? kFlagUsesFallbackSprites : 0;
        if (::lseek(s.idxFd, 0, SEEK_END) < 0
            || ::write(s.idxFd, &rec, sizeof(rec)) != static_cast<ssize_t>(sizeof(rec)))
        {
            LOG_ERROR("SpriteCache: write idx record failed: %s", std::strerror(errno));
            return;
        }
        ::fsync(s.idxFd);

        // Deliberately do NOT update s.index here. The store is durable on
        // disk (next run will see it during OpenOrCreate), but the cache
        // mmap covers only the prefix that existed at startup, so the bytes
        // for this entry aren't accessible via s.cacheMmap. If we inserted
        // into s.index, the next SpriteCacheLookup for the same key would
        // hit the bounds check inside Lookup and log a spurious "references
        // beyond cache file (corrupt)" warning. Keeping the in-memory index
        // unchanged for same-run stores means the lookup just returns miss
        // cleanly — same observable behaviour, no warning spam, no leaked
        // virtual address space.
    }

    SpriteCacheStats GetSpriteCacheStats()
    {
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        EnsureInit(s);
        SpriteCacheStats stats{};
        stats.entryCount = s.index.size();
        if (s.cacheFd >= 0)
        {
            struct stat st {};
            if (::fstat(s.cacheFd, &st) == 0)
                stats.totalBytes += static_cast<size_t>(st.st_size);
        }
        if (s.idxFd >= 0)
        {
            struct stat st {};
            if (::fstat(s.idxFd, &st) == 0)
                stats.totalBytes += static_cast<size_t>(st.st_size);
        }
        return stats;
    }
} // namespace OpenRCT2::Drawing
