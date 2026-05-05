/*****************************************************************************
 * OpenRCT2mini Sprite Paging — backs sprite-atlas data with either heap (legacy)
 * or mmap (Sprite Paging system, see OpenRCT2mini-Plan.md §5).
 *
 * Provides a unique_ptr-compatible smart pointer (`SpriteDataPtr`) that owns
 * the data via a runtime-selected deleter, plus helpers to construct from
 * heap-allocated or mmap'd file data.
 *****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace OpenRCT2::Drawing
{
    // Function-pointer deleter so the type is stable regardless of allocation source.
    using SpriteDataPtr = std::unique_ptr<uint8_t[], void (*)(uint8_t*)>;

    inline SpriteDataPtr MakeEmptySpriteData()
    {
        return SpriteDataPtr{ nullptr, +[](uint8_t*) noexcept {} };
    }

    // Wrap an existing heap-allocated unique_ptr<uint8_t[]> (delete[] cleanup).
    inline SpriteDataPtr MakeHeapSpriteData(std::unique_ptr<uint8_t[]> raw)
    {
        return SpriteDataPtr{ raw.release(), +[](uint8_t* p) noexcept { delete[] p; } };
    }

    // mmap a region of `path` into memory. Returns empty on failure.
    // Caller may pass `offset == 0` and `size == 0` to mmap the whole file
    // — the actual file size is then stored in *outSize.
    SpriteDataPtr MmapSpriteData(const std::string& path, size_t offset, size_t size, size_t* outSize);
}
