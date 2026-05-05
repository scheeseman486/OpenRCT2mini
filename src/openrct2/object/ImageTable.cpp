/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ImageTable.h"

#include "../Context.h"
#include "../Diagnostic.h"
#include "../OpenRCT2.h"
#include "../PlatformEnvironment.h"
#include "../SpriteIds.h"
#include "../core/File.h"
#include "../core/FileScanner.h"
#include "../core/Guard.hpp"
#include "../core/IStream.hpp"
#include "../core/Json.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../drawing/Drawing.h"
#include "../drawing/ImageImporter.h"
#include "../drawing/SpriteScratch.h"
#include "Object.h"
#include "ObjectFactory.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace OpenRCT2
{
    static thread_local std::map<u8string, std::unique_ptr<Object>> _objDataCache = {};

    struct ImageTable::RequiredImage
    {
        G1Element g1{};
        std::unique_ptr<RequiredImage> next_zoom;

        bool HasData() const
        {
            return g1.offset != nullptr;
        }

        RequiredImage() = default;
        RequiredImage(const RequiredImage&) = delete;

        RequiredImage(const G1Element& orig)
        {
            auto length = G1CalculateDataSize(&orig);
            g1 = orig;
            g1.offset = new uint8_t[length];
            std::memcpy(g1.offset, orig.offset, length);
            g1.flags.unset(G1Flag::hasZoomSprite);
        }

        RequiredImage(uint32_t idx, std::function<const G1Element*(uint32_t)> getter)
        {
            auto orig = getter(idx);
            if (orig != nullptr)
            {
                auto length = G1CalculateDataSize(orig);
                g1 = *orig;
                g1.offset = new uint8_t[length];
                std::memcpy(g1.offset, orig->offset, length);
                if (g1.flags.has(G1Flag::hasZoomSprite) && g1.zoomedOffset != 0)
                {
                    // Fetch image for next zoom level
                    next_zoom = std::make_unique<RequiredImage>(static_cast<uint32_t>(idx - g1.zoomedOffset), getter);
                    if (!next_zoom->HasData())
                    {
                        next_zoom = nullptr;
                        g1.flags.unset(G1Flag::hasZoomSprite);
                    }
                }
            }
        }

        ~RequiredImage()
        {
            delete[] g1.offset;
        }
    };

    std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::ParseImages(IReadObjectContext* context, std::string s)
    {
        std::vector<std::unique_ptr<RequiredImage>> result;
        if (s.empty())
        {
            result.push_back(std::make_unique<RequiredImage>());
        }
        else if (String::startsWith(s, "$CSG"))
        {
            auto rangeStart = s.find('[');
            auto rangeEnd = s.find(']');
            if (rangeStart != std::string::npos && rangeEnd != std::string::npos)
            {
                auto rangeString = s.substr(rangeStart, rangeEnd - rangeStart + 1);
                auto range = ParseRange(rangeString);
                if (IsCsgLoaded())
                {
                    for (auto i : range)
                    {
                        result.push_back(
                            std::make_unique<RequiredImage>(
                                static_cast<uint32_t>(SPR_CSG_BEGIN + i),
                                [](uint32_t idx) -> const G1Element* { return GfxGetG1Element(idx); }));
                    }
                }
                else
                {
                    std::string id(context->GetObjectIdentifier());
                    LOG_WARNING("CSG not loaded inserting placeholder images for %s", id.c_str());
                    result.resize(range.size());
                    for (auto& res : result)
                    {
                        res = std::make_unique<RequiredImage>();
                    }
                }
            }
        }
        else if (String::startsWith(s, "$G1"))
        {
            auto rangeStart = s.find('[');
            auto rangeEnd = s.find(']');
            if (rangeStart != std::string::npos && rangeEnd != std::string::npos)
            {
                auto rangeString = s.substr(rangeStart, rangeEnd - rangeStart + 1);
                auto range = ParseRange(rangeString);
                for (auto i : range)
                {
                    result.push_back(
                        std::make_unique<RequiredImage>(
                            static_cast<uint32_t>(i), [](uint32_t idx) -> const G1Element* { return GfxGetG1Element(idx); }));
                }
            }
        }
        else if (String::startsWith(s, "$RCT2:OBJDATA/"))
        {
            auto name = s.substr(14);
            auto rangeStart = name.find('[');
            auto rangeEnd = name.find(']');
            if (rangeStart != std::string::npos && rangeEnd != std::string::npos)
            {
                auto rangeString = name.substr(rangeStart, rangeEnd - rangeStart + 1);
                auto range = ParseRange(rangeString);
                name = name.substr(0, rangeStart);
                result = LoadObjectImages(context, name, range);
            }
        }
        else if (String::startsWith(s, "$LGX:"))
        {
            auto name = s.substr(5);
            auto rangeStart = name.find('[');
            auto rangeEnd = name.find(']');
            if (rangeStart != std::string::npos && rangeEnd != std::string::npos)
            {
                auto rangeString = name.substr(rangeStart, rangeEnd - rangeStart + 1);
                auto range = ParseRange(rangeString);
                name = name.substr(0, rangeStart);
                result = LoadImageArchiveImages(context, name, range);
            }
            else
            {
                result = LoadImageArchiveImages(context, name);
            }
        }
        else
        {
            try
            {
                auto imageData = context->GetData(s);
                auto image = Imaging::ReadFromBuffer(imageData);
                auto meta = Drawing::ImageImportMeta{};

                Drawing::ImageImporter importer;
                auto importResult = importer.Import(image, meta);

                result.push_back(std::make_unique<RequiredImage>(importResult.Element));
            }
            catch (const std::exception& e)
            {
                auto msg = String::stdFormat("Unable to load image '%s': %s", s.c_str(), e.what());
                context->LogWarning(ObjectError::badImageTable, msg.c_str());
                result.push_back(std::make_unique<RequiredImage>());
            }
        }
        return result;
    }

    std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::ParseImages(
        IReadObjectContext* context, std::vector<std::pair<std::string, Image>>& imageSources, json_t& el)
    {
        Guard::Assert(el.is_object(), "ImageTable::ParseImages expects parameter el to be object");

        auto path = Json::GetString(el["path"]);
        auto meta = Drawing::createImageImportMetaFromJson(el);

        std::vector<std::unique_ptr<RequiredImage>> result;
        try
        {
            auto itSource = std::find_if(
                imageSources.begin(), imageSources.end(),
                [&path](const std::pair<std::string, Image>& item) { return item.first == path; });
            if (itSource == imageSources.end())
            {
                throw std::runtime_error("Unable to find image in image source list.");
            }
            auto& image = itSource->second;

            Drawing::ImageImporter importer;
            auto importResult = importer.Import(image, meta);
            auto g1element = importResult.Element;
            result.push_back(std::make_unique<RequiredImage>(g1element));
        }
        catch (const std::exception& e)
        {
            auto msg = String::stdFormat("Unable to load image '%s': %s", path.c_str(), e.what());
            context->LogWarning(ObjectError::badImageTable, msg.c_str());
            result.push_back(std::make_unique<RequiredImage>());
        }
        return result;
    }

    std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::LoadImageArchiveImages(
        IReadObjectContext* context, const std::string& path, const std::vector<int32_t>& range)
    {
        std::vector<std::unique_ptr<RequiredImage>> result;
        auto gxRaw = context->GetData(path);
        std::optional<Gx> gxData = GfxLoadGx(gxRaw);
        if (gxData.has_value())
        {
            // Fix entry data offsets
            for (uint32_t i = 0; i < gxData->header.numEntries; i++)
            {
                if (gxData->elements[i].offset == nullptr)
                {
                    gxData->elements[i].offset = gxData->data.get();
                }
                else
                {
                    gxData->elements[i].offset += reinterpret_cast<uintptr_t>(gxData->data.get());
                }
            }

            if (!range.empty())
            {
                size_t placeHoldersAdded = 0;
                for (auto i : range)
                {
                    if (i >= 0 && (i < static_cast<int32_t>(gxData->header.numEntries)))
                    {
                        result.push_back(std::make_unique<RequiredImage>(gxData->elements[i]));
                    }
                    else
                    {
                        result.push_back(std::make_unique<RequiredImage>());
                        placeHoldersAdded++;
                    }
                }

                // Log place holder information
                if (placeHoldersAdded > 0)
                {
                    std::string msg = "Adding " + std::to_string(placeHoldersAdded) + " placeholders";
                    context->LogWarning(ObjectError::invalidProperty, msg.c_str());
                }
            }
            else
            {
                for (int i = 0; i < static_cast<int32_t>(gxData->header.numEntries); i++)
                    result.push_back(std::make_unique<RequiredImage>(gxData->elements[i]));
            }
        }
        else
        {
            auto msg = String::stdFormat("Unable to load Gx '%s'", path.c_str());
            context->LogWarning(ObjectError::badImageTable, msg.c_str());
            for (size_t i = 0; i < range.size(); i++)
            {
                result.push_back(std::make_unique<RequiredImage>());
            }
        }
        return result;
    }

    std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::LoadObjectImages(
        IReadObjectContext* context, const std::string& name, const std::vector<int32_t>& range)
    {
        std::vector<std::unique_ptr<RequiredImage>> result;
        Object* obj;

        auto cached = _objDataCache.find(name);
        if (cached != _objDataCache.end())
        {
            obj = cached->second.get();
        }
        else
        {
            auto objectPath = FindLegacyObject(name);
            auto tmp = ObjectFactory::CreateObjectFromLegacyFile(objectPath.c_str(), context->ShouldLoadImages());
            auto inserted = _objDataCache.insert({ name, std::move(tmp) });
            obj = inserted.first->second.get();
        }

        if (obj != nullptr)
        {
            auto& imgTable = static_cast<const Object*>(obj)->GetImageTable();
            auto numImages = static_cast<int32_t>(imgTable.GetCount());
            auto images = imgTable.GetImages();
            size_t placeHoldersAdded = 0;
            for (auto i : range)
            {
                if (i >= 0 && i < numImages)
                {
                    result.push_back(
                        std::make_unique<RequiredImage>(
                            static_cast<uint32_t>(i), [images](uint32_t idx) -> const G1Element* { return &images[idx]; }));
                }
                else
                {
                    result.push_back(std::make_unique<RequiredImage>());
                    placeHoldersAdded++;
                }
            }

            // Log place holder information
            if (placeHoldersAdded > 0)
            {
                std::string msg = "Adding " + std::to_string(placeHoldersAdded) + " placeholders";
                context->LogWarning(ObjectError::invalidProperty, msg.c_str());
            }
        }
        else
        {
            std::string msg = "Unable to open '" + name + "'";
            context->LogWarning(ObjectError::invalidProperty, msg.c_str());
            for (size_t i = 0; i < range.size(); i++)
            {
                result.push_back(std::make_unique<RequiredImage>());
            }
        }
        return result;
    }

    std::vector<int32_t> ImageTable::ParseRange(std::string s)
    {
        // Currently only supports [###] or [###..###]
        std::vector<int32_t> result = {};
        if (s.length() >= 3 && s[0] == '[' && s[s.length() - 1] == ']')
        {
            s = s.substr(1, s.length() - 2);
            auto parts = String::split(s, "..");
            if (parts.size() == 1)
            {
                result.push_back(String::parse<int32_t>(parts[0]));
            }
            else
            {
                auto left = String::parse<int32_t>(parts[0]);
                auto right = String::parse<int32_t>(parts[1]);
                if (left <= right)
                {
                    for (auto i = left; i <= right; i++)
                    {
                        result.push_back(i);
                    }
                }
                else
                {
                    for (auto i = right; i >= left; i--)
                    {
                        result.push_back(i);
                    }
                }
            }
        }
        return result;
    }

    std::string ImageTable::FindLegacyObject(const std::string& name)
    {
        const auto& env = GetContext()->GetPlatformEnvironment();
        auto objectsPath = env.GetDirectoryPath(DirBase::rct2, DirId::objects);
        auto objectPath = Path::Combine(objectsPath, name);
        if (File::Exists(objectPath))
        {
            return objectPath;
        }

        std::string altName = name;
        auto rangeStart = name.find(".DAT");
        if (rangeStart != std::string::npos)
        {
            altName.replace(rangeStart, 4, ".POB");
        }
        objectPath = Path::Combine(objectsPath, altName);
        if (File::Exists(objectPath))
        {
            return objectPath;
        }

        if (!File::Exists(objectPath))
        {
            // Search recursively for any file with the target name (case insensitive)
            auto filter = Path::Combine(objectsPath, u8"*.dat;*.pob");
            auto scanner = Path::ScanDirectory(filter, true);
            while (scanner->Next())
            {
                auto currentName = Path::GetFileName(scanner->GetPathRelative());
                if (String::iequals(currentName, name) || String::iequals(currentName, altName))
                {
                    objectPath = scanner->GetPath();
                    break;
                }
            }
        }
        return objectPath;
    }

    ImageTable::~ImageTable()
    {
        // OPENRCT2MINI: free only entries that own their offset (added via AddImage). Entries
        // populated by Read() reference SpriteScratch mmap (process-lifetime) or _heapFallback,
        // and must not be delete[]-d here.
        const size_t n = _entries.size();
        for (size_t i = 0; i < n; i++)
        {
            if (i < _entryOwnsOffset.size() && _entryOwnsOffset[i])
            {
                delete[] _entries[i].offset;
            }
        }
    }

    void ImageTable::Read(IReadObjectContext* context, IStream* stream)
    {
        if (gOpenRCT2NoGraphics)
        {
            return;
        }

        // OPENRCT2MINI: when the caller (typically the object-index builder) only wants
        // metadata, skip the heavy lift entirely — no scratch append, no transient buffer,
        // no header parsing. Just advance the stream past the image table so subsequent
        // reads keep working. ImageTable::Read is always the LAST operation on the stream
        // for legacy .DAT objects (verified across all .DAT object readers), so we don't
        // need to leave the stream in a precise position.
        if (context != nullptr && !context->ShouldLoadImages())
        {
            return;
        }

        try
        {
            uint32_t numImages = stream->ReadValue<uint32_t>();
            uint32_t imageDataSize = stream->ReadValue<uint32_t>();

            uint64_t headerTableSize = numImages * 16;
            uint64_t remainingBytes = stream->GetLength() - stream->GetPosition() - headerTableSize;
            if (remainingBytes > imageDataSize)
            {
                context->LogVerbose(ObjectError::badImageTable, "Image table size longer than expected.");
                imageDataSize = static_cast<uint32_t>(remainingBytes);
            }

            auto dataSize = static_cast<size_t>(imageDataSize);
            // OPENRCT2MINI Sprite Scratch: load into transient heap, then move to mmap'd
            // scratch file so the kernel can page out cold image data.
            auto transientBuffer = std::make_unique<uint8_t[]>(dataSize);
            if (transientBuffer == nullptr)
            {
                context->LogError(ObjectError::badImageTable, "Image table too large.");
                throw std::runtime_error("Image table too large.");
            }

            // Read g1 element headers — initially store offsets relative to a base of 0;
            // we'll fix them up below once we have the final scratch pointer.
            std::vector<G1Element> newEntries;
            std::vector<uintptr_t> elementOffsets;
            elementOffsets.reserve(numImages);
            for (uint32_t i = 0; i < numImages; i++)
            {
                G1Element g1Element{};

                uintptr_t imageDataOffset = static_cast<uintptr_t>(stream->ReadValue<uint32_t>());
                elementOffsets.push_back(imageDataOffset);

                g1Element.width = stream->ReadValue<int16_t>();
                g1Element.height = stream->ReadValue<int16_t>();
                g1Element.xOffset = stream->ReadValue<int16_t>();
                g1Element.yOffset = stream->ReadValue<int16_t>();
                g1Element.flags = stream->ReadValue<G1Flags>();
                g1Element.zoomedOffset = stream->ReadValue<uint16_t>();

                newEntries.push_back(std::move(g1Element));
            }

            // Read g1 element data into the transient heap buffer.
            size_t readBytes = static_cast<size_t>(stream->TryRead(transientBuffer.get(), dataSize));

            // If data is shorter than expected (some custom objects are unfortunately like that)
            size_t unreadBytes = dataSize - readBytes;
            if (unreadBytes > 0)
            {
                std::fill_n(transientBuffer.get() + readBytes, unreadBytes, 0);
                context->LogWarning(ObjectError::badImageTable, "Image table size shorter than expected.");
            }

            // Move data into the mmap'd scratch file. On success, free the transient
            // buffer — the scratch mmap is faulted in on render and pages can be evicted.
            uint8_t* scratchPtr = OpenRCT2::Drawing::SpriteScratchAppend(transientBuffer.get(), dataSize);
            if (scratchPtr != nullptr)
            {
                transientBuffer.reset();
            }
            else
            {
                // Scratch failed (e.g. /tmp full). Fall back to heap retention.
                LOG_WARNING("ImageTable: SpriteScratchAppend failed; using heap fallback");
                scratchPtr = transientBuffer.get();
                _heapFallback = std::move(transientBuffer);
            }

            // Patch G1Element offsets to point at the scratch-mmap'd region.
            uintptr_t base = reinterpret_cast<uintptr_t>(scratchPtr);
            for (size_t i = 0; i < newEntries.size(); i++)
            {
                newEntries[i].offset = reinterpret_cast<uint8_t*>(base + elementOffsets[i]);
            }

            _entries.insert(_entries.end(), newEntries.begin(), newEntries.end());
            // OPENRCT2MINI: these entries' offsets reference scratch/_heapFallback storage,
            // not individual heap allocations. Mark them so the destructor doesn't free them.
            _entryOwnsOffset.resize(_entries.size(), false);
            // OPENRCT2MINI: shrink the entries vector to exact size. push_back doubles
            // capacity on growth, so a 3000-image ride ends up with capacity 4096 — the
            // 1096 unused slots × 12 bytes = 13 KB of pointless overhang per Object.
            // Across hundreds of Objects this adds up to several MB.
            _entries.shrink_to_fit();
            _entryOwnsOffset.shrink_to_fit();
        }
        catch (const std::exception&)
        {
            context->LogError(ObjectError::badImageTable, "Bad image table.");
            throw;
        }
    }

    std::vector<std::pair<std::string, Image>> ImageTable::GetImageSources(IReadObjectContext* context, json_t& jsonImages)
    {
        std::vector<std::pair<std::string, Image>> result;
        for (auto& jsonImage : jsonImages)
        {
            if (jsonImage.is_object() && jsonImage.contains("path"))
            {
                auto path = Json::GetString(jsonImage["path"]);
                auto keepPalette = Json::GetString(jsonImage["palette"]) == "keep";
                auto itSource = std::find_if(result.begin(), result.end(), [&path](const std::pair<std::string, Image>& item) {
                    return item.first == path;
                });
                if (itSource == result.end())
                {
                    auto imageData = context->GetData(path);
                    auto imageFormat = keepPalette ? ImageFormat::png : ImageFormat::png32;
                    auto image = Imaging::ReadFromBuffer(imageData, imageFormat);
                    auto pair = std::make_pair<std::string, Image>(std::move(path), std::move(image));
                    result.push_back(std::move(pair));
                }
            }
        }
        return result;
    }

    bool ImageTable::ReadJson(IReadObjectContext* context, json_t& root)
    {
        Guard::Assert(root.is_object(), "ImageTable::ReadJson expects parameter root to be object");

        bool usesFallbackSprites = false;

        if (context->ShouldLoadImages())
        {
            // First gather all the required images from inspecting the JSON
            std::vector<std::unique_ptr<RequiredImage>> allImages;
            auto jsonImages = root["images"];
            if (!IsCsgLoaded() && root.contains("noCsgImages"))
            {
                jsonImages = root["noCsgImages"];
                usesFallbackSprites = true;
            }

            auto imageSources = GetImageSources(context, jsonImages);

            for (auto& jsonImage : jsonImages)
            {
                if (jsonImage.is_string())
                {
                    auto strImage = jsonImage.get<std::string>();
                    auto images = ParseImages(context, strImage);
                    allImages.insert(
                        allImages.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
                }
                else if (jsonImage.is_object())
                {
                    if (jsonImage.contains("gx"))
                    {
                        auto xOverride = Json::GetNumber<int16_t>(jsonImage["x"], std::numeric_limits<int16_t>::max());
                        auto yOverride = Json::GetNumber<int16_t>(jsonImage["y"], std::numeric_limits<int16_t>::max());
                        const bool hasXOverride = xOverride != std::numeric_limits<int16_t>::max();
                        const bool hasYOverride = yOverride != std::numeric_limits<int16_t>::max();

                        auto strImage = jsonImage["gx"].get<std::string>();
                        auto images = ParseImages(context, strImage);

                        if (hasXOverride || hasYOverride)
                        {
                            for (auto& image : images)
                            {
                                if (hasXOverride)
                                    image->g1.xOffset = xOverride;
                                if (hasYOverride)
                                    image->g1.yOffset = yOverride;
                            }
                        }

                        allImages.insert(
                            allImages.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
                    }
                    else
                    {
                        auto images = ParseImages(context, imageSources, jsonImage);
                        allImages.insert(
                            allImages.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
                    }
                }
            }

            // OPENRCT2MINI: cut 27. Previously each image's pixel bytes were copied via
            // AddImage() (per-image new uint8_t[length]). For JSON-loaded objects this
            // pinned ~30 MB on the heap (live trace: 31 MB across ~70K allocs), which
            // never gets reclaimed because each tiny allocation lives in the main arena.
            // The legacy DAT path (Read()) already routes pixel data through the
            // disk-backed SpriteScratch mmap so the kernel can MADV_DONTNEED cold pages.
            // Mirror that here: layout the entries first (so zoomedOffset arithmetic is
            // unchanged), then pack ALL pixel bytes for this Object into one buffer and
            // append in a single SpriteScratchAppend. Patch G1Element offsets to point
            // into the resulting mmap region; entries get _entryOwnsOffset=false so the
            // destructor leaves the scratch alone. On scratch failure we fall back to
            // the original per-image AddImage path.

            const auto imagesStartIndex = GetCount();

            std::vector<G1Element> newEntries;
            std::vector<const uint8_t*> sourceData;
            std::vector<size_t> dataLengths;
            newEntries.reserve(allImages.size());
            sourceData.reserve(allImages.size());
            dataLengths.reserve(allImages.size());

            // First pass: main images (one per allImages entry).
            for (const auto& img : allImages)
            {
                newEntries.push_back(img->g1);
                sourceData.push_back(img->g1.offset);
                dataLengths.push_back(G1CalculateDataSize(&img->g1));
            }

            // Second pass: zoom chains, appended after all main images so the
            // zoom-relative offsets match the original behaviour.
            for (size_t j = 0; j < allImages.size(); j++)
            {
                const RequiredImage* img = allImages[j].get();
                if (img->next_zoom == nullptr)
                    continue;

                // Patch the main entry's zoomedOffset to point back from the upcoming
                // zoom entry's table index. With both expressed in absolute-table
                // coordinates: (imagesStartIndex + j) - (imagesStartIndex + zoomIdx).
                const size_t mainIdx = j;
                const size_t zoomIdx = newEntries.size();
                newEntries[mainIdx].zoomedOffset = static_cast<int32_t>(mainIdx) - static_cast<int32_t>(zoomIdx);

                img = img->next_zoom.get();
                while (img != nullptr)
                {
                    G1Element zoomg1 = img->g1;
                    if (img->next_zoom != nullptr)
                    {
                        zoomg1.zoomedOffset = -1;
                    }
                    newEntries.push_back(zoomg1);
                    sourceData.push_back(img->g1.offset);
                    dataLengths.push_back(G1CalculateDataSize(&img->g1));
                    img = img->next_zoom.get();
                }
            }

            // Sum total bytes and compute per-entry offsets within the packed buffer.
            size_t totalSize = 0;
            std::vector<size_t> entryOffsets;
            entryOffsets.reserve(newEntries.size());
            for (size_t i = 0; i < newEntries.size(); i++)
            {
                entryOffsets.push_back(totalSize);
                totalSize += dataLengths[i];
            }

            bool flushedToScratch = false;
            if (totalSize > 0)
            {
                auto packBuf = std::make_unique<uint8_t[]>(totalSize);
                for (size_t i = 0; i < newEntries.size(); i++)
                {
                    if (dataLengths[i] > 0 && sourceData[i] != nullptr)
                    {
                        std::memcpy(packBuf.get() + entryOffsets[i], sourceData[i], dataLengths[i]);
                    }
                }

                uint8_t* scratchPtr = OpenRCT2::Drawing::SpriteScratchAppend(packBuf.get(), totalSize);
                if (scratchPtr != nullptr)
                {
                    // packBuf is no longer needed — data lives in the scratch mmap.
                    packBuf.reset();
                    for (size_t i = 0; i < newEntries.size(); i++)
                    {
                        newEntries[i].offset = (dataLengths[i] > 0)
                            ? scratchPtr + entryOffsets[i]
                            : nullptr;
                    }
                    for (auto& e : newEntries)
                    {
                        _entries.push_back(std::move(e));
                        _entryOwnsOffset.push_back(false);
                    }
                    flushedToScratch = true;
                }
                else if (_heapFallback == nullptr)
                {
                    // Scratch unavailable (e.g. /tmp full) but no prior fallback exists.
                    // Keep the packed buffer alive via _heapFallback so the entries can
                    // safely point into it. Single allocation per Object — still much
                    // better than the old per-image new[].
                    LOG_WARNING("ImageTable: SpriteScratchAppend failed; using heap fallback (JSON path)");
                    for (size_t i = 0; i < newEntries.size(); i++)
                    {
                        newEntries[i].offset = (dataLengths[i] > 0)
                            ? packBuf.get() + entryOffsets[i]
                            : nullptr;
                    }
                    _heapFallback = std::move(packBuf);
                    for (auto& e : newEntries)
                    {
                        _entries.push_back(std::move(e));
                        _entryOwnsOffset.push_back(false);
                    }
                    flushedToScratch = true;
                }
            }
            else
            {
                // No pixel data at all (placeholder/CSG-not-loaded entries) — push with null offsets.
                for (auto& e : newEntries)
                {
                    e.offset = nullptr;
                    _entries.push_back(std::move(e));
                    _entryOwnsOffset.push_back(false);
                }
                flushedToScratch = true;
            }

            if (!flushedToScratch)
            {
                // Last-resort fallback: scratch failed AND _heapFallback is already in
                // use (shouldn't happen — Read() and ReadJson() are mutually exclusive
                // per Object — but be defensive). Use the original per-image AddImage
                // path so we don't leak G1Element offsets pointing at freed pack memory.
                LOG_WARNING("ImageTable: scratch+heap-fallback both unavailable; per-image heap copy");
                for (auto& e : newEntries)
                {
                    AddImage(&e);
                }
            }

            // shrink_to_fit so the entries vector matches its actual size — push_back
            // doubling otherwise leaves up to 2× overhang per Object × hundreds of
            // Objects = several MB of dead vector capacity.
            _entries.shrink_to_fit();
            _entryOwnsOffset.shrink_to_fit();
            (void)imagesStartIndex; // reserved for future diagnostic logging
        }

        _objDataCache.clear();

        return usesFallbackSprites;
    }

    void ImageTable::AddImage(const G1Element* g1)
    {
        G1Element newg1 = *g1;
        auto length = G1CalculateDataSize(g1);
        bool ownsOffset = false;
        if (length == 0)
        {
            newg1.offset = nullptr;
        }
        else
        {
            newg1.offset = new uint8_t[length];
            std::copy_n(g1->offset, length, newg1.offset);
            ownsOffset = true; // OPENRCT2MINI: this entry owns its offset; destructor must free it.
        }
        _entries.push_back(std::move(newg1));
        _entryOwnsOffset.push_back(ownsOffset);
    }

    void ImageTable::addPalette(const G1Palette& g1)
    {
        Guard::Assert(g1.flags.has(G1Flag::isPalette));
        const auto base = reinterpret_cast<const G1Element*>(&g1);
        AddImage(base);
    }
} // namespace OpenRCT2
