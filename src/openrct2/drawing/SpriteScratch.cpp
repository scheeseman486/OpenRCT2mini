/*****************************************************************************
 * OpenRCT2mini Sprite Scratch — implementation.
 *****************************************************************************/

#include "SpriteScratch.h"

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
#include <vector>

#if defined(__GLIBC__)
    #include <malloc.h>
#endif

namespace OpenRCT2::Drawing
{
    namespace
    {
        struct ScratchFile
        {
            int fd = -1;
            size_t totalSize = 0;
            // Per-mapping records so we can keep them alive until process exit.
            // We never unmap (lifetime == process), so this is a small list.
            struct Mapping
            {
                uint8_t* base = nullptr;
                size_t mapSize = 0;  // includes alignment slack
                size_t userSkew = 0;
            };
            std::vector<Mapping> mappings;
            std::mutex lock;
            // OPENRCT2MINI: counter incremented by every Append. SpriteScratchEvictIfIdle()
            // checks whether anything was appended since the previous eviction; if so it
            // evicts the new working set. Lets us drop pages without knowing the call sites.
            std::atomic<uint32_t> appendsSinceLastEvict{ 0 };

            bool ensureOpen()
            {
                if (fd >= 0) return true;

                // OPENRCT2MINI: prefer a *disk*-backed temp location. madvise(MADV_DONTNEED)
                // on a tmpfs-backed file mapping cannot release pages — tmpfs has no
                // backing store to refault from, so the kernel keeps the pages forever
                // and they count against our cgroup MemoryMax. With a disk-backed file,
                // MADV_DONTNEED genuinely releases pages and they refault from disk on
                // next access. Order: ORCT_SCRATCH_DIR override → /var/tmp → $XDG_CACHE_HOME
                // → $HOME/.cache → $TMPDIR → /tmp.
                //
                // Note for future readers: on the Miyoo Mini under OnionUI, BOTH /tmp
                // AND /var/tmp are tmpfs (small RAM-backed mounts, typical embedded
                // Linux default). Falling through to either on the device produces
                // ENOSPC after a handful of appends and defeats the point of
                // MADV_DONTNEED. launch.sh (Packaging/miyoo_mini/package.sh:361)
                // exports ORCT_SCRATCH_DIR=$APPDIR/cache/sprite-scratch, which IS on
                // the SD card, and we hit that branch first — never the /tmp fallback.
                // The /tmp / /var/tmp candidates exist so the scratch still works on
                // host x86 dev builds (where /var/tmp is genuinely disk-backed) and
                // as a last resort if launch.sh's env var is missing.
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

                char path[256];
                for (int i = 0; i < n; i++)
                {
                    // mkdir best-effort; ignore EEXIST and EROFS.
                    (void)::mkdir(candidates[i], 0700);
                    std::snprintf(path, sizeof(path), "%s/openrctmini-sprites-XXXXXX", candidates[i]);
                    fd = ::mkstemp(path);
                    if (fd >= 0)
                    {
                        ::unlink(path);
                        return true;
                    }
                }
                LOG_ERROR("SpriteScratch: mkstemp failed in all candidate dirs: %s", std::strerror(errno));
                return false;
            }
        };

        ScratchFile& GetScratch()
        {
            static ScratchFile s;
            return s;
        }
    }

    uint8_t* SpriteScratchAppend(const void* bytes, size_t size)
    {
        if (size == 0) return nullptr;
        auto& s = GetScratch();
        std::lock_guard<std::mutex> g(s.lock);
        if (!s.ensureOpen()) return nullptr;
        const uint32_t pending = s.appendsSinceLastEvict.fetch_add(1, std::memory_order_relaxed) + 1;
        // OPENRCT2MINI: every 8 appends, hint the kernel that older scratch pages are cold
        // and trim the glibc heap. Don't advise on the most recent mapping — it's about to
        // be read by the renderer and we'd just thrash. madvise(MADV_DONTNEED) on the
        // disk-backed scratch (cut 16) really releases pages, so older mappings whose
        // sprites haven't been touched yet get reclaimed.
        if ((pending & 7) == 0 && s.mappings.size() > 1)
        {
            // Walk all mappings except the last (which may still be hot).
            for (size_t i = 0; i + 1 < s.mappings.size(); i++)
            {
                const auto& m = s.mappings[i];
                if (m.base != nullptr && m.mapSize > 0)
                {
                    ::madvise(m.base, m.mapSize, MADV_DONTNEED);
                }
            }
#if defined(__GLIBC__)
            malloc_trim(0);
#endif
        }

        // Append at current end.
        off_t writeOffset = ::lseek(s.fd, 0, SEEK_END);
        if (writeOffset == static_cast<off_t>(-1))
        {
            LOG_ERROR("SpriteScratch: lseek failed: %s", std::strerror(errno));
            return nullptr;
        }

        // Write the data sequentially.
        const uint8_t* p = static_cast<const uint8_t*>(bytes);
        size_t remaining = size;
        while (remaining > 0)
        {
            ssize_t n = ::write(s.fd, p, remaining);
            if (n < 0)
            {
                if (errno == EINTR) continue;
                LOG_ERROR("SpriteScratch: write failed: %s", std::strerror(errno));
                return nullptr;
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }

        // mmap the just-written region. Round offset down to page boundary.
        long pageSize = ::sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) pageSize = 4096;
        size_t alignedOffset = static_cast<size_t>(writeOffset) & ~static_cast<size_t>(pageSize - 1);
        size_t userSkew = static_cast<size_t>(writeOffset) - alignedOffset;
        size_t mapSize = size + userSkew;

        void* base = ::mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, s.fd, static_cast<off_t>(alignedOffset));
        if (base == MAP_FAILED)
        {
            LOG_ERROR("SpriteScratch: mmap failed: %s", std::strerror(errno));
            return nullptr;
        }

        // Tell the kernel this is random-access (no read-ahead). Don't DONTNEED here:
        // the caller is about to copy these pages into G1Element offsets and the renderer
        // will touch them shortly afterwards. Premature DONTNEED just trips extra faults.
        // SpriteScratchEvict() is the explicit knob callers use to drop pages after a
        // bulk-load phase ends.
        ::madvise(base, mapSize, MADV_RANDOM);

        s.totalSize = static_cast<size_t>(writeOffset) + size;
        s.mappings.push_back({ static_cast<uint8_t*>(base), mapSize, userSkew });
        return static_cast<uint8_t*>(base) + userSkew;
    }

    size_t SpriteScratchTotalSize()
    {
        return GetScratch().totalSize;
    }

    void SpriteScratchEvict()
    {
        auto& s = GetScratch();
        std::lock_guard<std::mutex> g(s.lock);
        size_t evictedBytes = 0;
        size_t mappingsHinted = 0;
        for (const auto& m : s.mappings)
        {
            if (m.base != nullptr && m.mapSize > 0)
            {
                if (::madvise(m.base, m.mapSize, MADV_DONTNEED) == 0)
                {
                    evictedBytes += m.mapSize;
                    mappingsHinted++;
                }
            }
        }
        s.appendsSinceLastEvict.store(0, std::memory_order_relaxed);
        // OPENRCT2MINI: piggy-back malloc_trim. The same load phases that grew SpriteScratch
        // also blew the glibc main arena up to its high-water mark via short-lived buffers
        // (SawyerChunkReader chunks, MemoryStream::EnsureWriteCapacity grow-and-realloc,
        // ImageTable transientBuffer, etc.). glibc keeps that capacity forever unless we
        // ask. Returning unused pages to the kernel here is roughly free — pages re-fault
        // from zero on next allocation — and saves ~30 MB of [heap] on the title screen.
#if defined(__GLIBC__)
        malloc_trim(0);
#endif
        LOG_INFO(
            "SpriteScratchEvict: hinted %zu/%zu mappings, %zu MiB", mappingsHinted, s.mappings.size(),
            evictedBytes / (1024 * 1024));
    }

    bool SpriteScratchEvictIfIdle(uint32_t minAppendsToTrigger)
    {
        auto& s = GetScratch();
        // Quick check before locking — if no appends happened, skip the work entirely.
        uint32_t pending = s.appendsSinceLastEvict.load(std::memory_order_relaxed);
        if (pending < minAppendsToTrigger)
            return false;
        SpriteScratchEvict();
        return true;
    }
}
