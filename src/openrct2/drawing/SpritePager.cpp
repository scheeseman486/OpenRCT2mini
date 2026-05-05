/*****************************************************************************
 * OpenRCT2mini Sprite Paging — implementation of mmap-backed sprite data.
 *****************************************************************************/

#include "SpritePager.h"

#include "../Diagnostic.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace OpenRCT2::Drawing
{
    // Per-pointer record so the deleter can find the size.
    // mmap regions are page-aligned and small in count; a tiny intrusive
    // map is fine. We use a static lookup keyed by pointer.
    namespace
    {
        struct MmapRecord
        {
            uint8_t* base = nullptr;
            size_t size = 0;
            uint8_t* userPtr = nullptr;
        };

        // Linear scan over a small fixed array — typical population is 1-5 entries
        // (g1, g2, palettes, fonts, tracks, optional csg). Keeps it simple.
        static constexpr size_t kMaxMmapRecords = 16;
        MmapRecord g_records[kMaxMmapRecords] = {};

        bool RegisterMmap(uint8_t* userPtr, uint8_t* base, size_t size)
        {
            for (auto& r : g_records)
            {
                if (r.userPtr == nullptr)
                {
                    r.userPtr = userPtr;
                    r.base = base;
                    r.size = size;
                    return true;
                }
            }
            return false;
        }

        void UnregisterAndUnmap(uint8_t* userPtr) noexcept
        {
            for (auto& r : g_records)
            {
                if (r.userPtr == userPtr)
                {
                    if (r.base && r.size)
                        ::munmap(r.base, r.size);
                    r = {};
                    return;
                }
            }
        }

        void MmapDeleter(uint8_t* p) noexcept
        {
            if (p) UnregisterAndUnmap(p);
        }
    }

    SpriteDataPtr MmapSpriteData(const std::string& path, size_t offset, size_t size, size_t* outSize)
    {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            LOG_ERROR("MmapSpriteData: open failed for %s: %s", path.c_str(), std::strerror(errno));
            return MakeEmptySpriteData();
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0)
        {
            LOG_ERROR("MmapSpriteData: fstat failed for %s: %s", path.c_str(), std::strerror(errno));
            ::close(fd);
            return MakeEmptySpriteData();
        }

        if (size == 0) size = static_cast<size_t>(st.st_size) - offset;

        // mmap requires page-aligned offset. Round down.
        long pageSize = ::sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) pageSize = 4096;
        size_t alignedOffset = offset & ~static_cast<size_t>(pageSize - 1);
        size_t userSkew = offset - alignedOffset;
        size_t mapSize = size + userSkew;

        void* base = ::mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, static_cast<off_t>(alignedOffset));
        ::close(fd);
        if (base == MAP_FAILED)
        {
            LOG_ERROR("MmapSpriteData: mmap failed for %s: %s", path.c_str(), std::strerror(errno));
            return MakeEmptySpriteData();
        }

        uint8_t* basePtr = static_cast<uint8_t*>(base);
        uint8_t* userPtr = basePtr + userSkew;

        if (!RegisterMmap(userPtr, basePtr, mapSize))
        {
            LOG_ERROR("MmapSpriteData: out of mmap record slots; falling back to no-mmap");
            ::munmap(base, mapSize);
            return MakeEmptySpriteData();
        }

        if (outSize) *outSize = size;
        return SpriteDataPtr{ userPtr, &MmapDeleter };
    }
}
