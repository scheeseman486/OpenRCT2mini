/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

// OPENRCT2MINI P9: Snapshot save. See profiler-plan.md "Snapshot save
// format" + the SnapshotWriter.h header.
//
// Layout:
//   File header — 16 bytes, never compressed:
//     char     magic[4]      = 'O','R','C','P'
//     uint16_t formatVersion = kSnapshotFormatVersion
//     uint16_t flags         (bit 0 = body zstd-compressed)
//     uint32_t bodyLength    (compressed if flag set)
//     uint32_t bodyChecksum  (zlib crc32 of body bytes post-compression)
//
//   Body — sequence of TLV chunks, then ENDF terminator:
//     uint32_t fourCC
//     uint32_t size
//     uint8_t  data[size]
//
//   Chunks: INFO, SCHM, FRMS, SLWS, PROF, NOTE, ENDF.

#include "SnapshotWriter.h"

#ifdef ENABLE_PERFORMANCE_PROFILER

#include "../core/Compression.h"
#include "../core/MemoryStream.h"
#include "Profiling.h"
#include "Sampler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h> // crc32()

namespace OpenRCT2::Profiling::Sampler
{
    namespace
    {
        constexpr uint16_t kFlagZstdCompressed = 0x0001;

        // FourCC packed little-endian — first byte is the "leftmost"
        // when written to disk. So 'O','R','C','P' on disk → we want
        // the first byte of the uint32_t to be 'O', which on a
        // little-endian host is the LSB. Pack accordingly.
        constexpr uint32_t makeFourCC(char a, char b, char c, char d)
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(a))
                | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
                | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
                | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
        }

        constexpr uint32_t kFourCC_INFO = makeFourCC('I', 'N', 'F', 'O');
        constexpr uint32_t kFourCC_SCHM = makeFourCC('S', 'C', 'H', 'M');
        constexpr uint32_t kFourCC_FRMS = makeFourCC('F', 'R', 'M', 'S');
        constexpr uint32_t kFourCC_SLWS = makeFourCC('S', 'L', 'W', 'S');
        constexpr uint32_t kFourCC_PROF = makeFourCC('P', 'R', 'O', 'F');
        constexpr uint32_t kFourCC_NOTE = makeFourCC('N', 'O', 'T', 'E');
        constexpr uint32_t kFourCC_ENDF = makeFourCC('E', 'N', 'D', 'F');

        // Append a chunk to a body MemoryStream: TLV header + data.
        void writeChunk(MemoryStream& body, uint32_t fourCC, const void* data, uint32_t size)
        {
            body.Write(&fourCC, sizeof(fourCC));
            body.Write(&size, sizeof(size));
            if (size > 0 && data != nullptr)
                body.Write(data, size);
        }

        // INFO chunk: list of (uint16_t keyLen, key bytes, uint16_t
        // valLen, val bytes). Strings are not null-terminated.
        void writeInfoChunk(MemoryStream& body, const std::string& note)
        {
            (void)note; // NOTE goes in its own chunk; INFO carries metadata only.

            MemoryStream payload;

            auto addKv = [&](const char* key, const std::string& val) {
                const uint16_t kLen = static_cast<uint16_t>(std::strlen(key));
                const uint16_t vLen = static_cast<uint16_t>(std::min<size_t>(val.size(), UINT16_MAX));
                payload.Write(&kLen, sizeof(kLen));
                payload.Write(key, kLen);
                payload.Write(&vLen, sizeof(vLen));
                if (vLen > 0)
                    payload.Write(val.data(), vLen);
            };

            // Architecture string — match Version.h's OPENRCT2_ARCHITECTURE.
#if defined(__amd64__) || defined(_M_AMD64)
            addKv("arch", "x86-64");
#elif defined(__arm__)
            addKv("arch", "arm-v7a");
#elif defined(__aarch64__)
            addKv("arch", "AArch64");
#elif defined(__i386__)
            addKv("arch", "x86");
#else
            addKv("arch", "unknown");
#endif

#if defined(__linux__)
            addKv("platform", "Linux");
#elif defined(__APPLE__)
            addKv("platform", "macOS");
#elif defined(_WIN32)
            addKv("platform", "Windows");
#else
            addKv("platform", "unknown");
#endif

            // Profiler-specific: ring sizes so the parser can sanity
            // check the FRMS / SLWS chunk sizes.
            addKv(
                "frame_ring_size",
                std::to_string(static_cast<uint32_t>(getFrameRing().size())));
            addKv(
                "slow_ring_size",
                std::to_string(static_cast<uint32_t>(getSlowPollRing().size())));
            addKv(
                "audio_buffer_ms",
                std::to_string(static_cast<uint32_t>(getAudioBufferMs())));

            const auto* data = static_cast<const uint8_t*>(payload.GetData());
            writeChunk(body, kFourCC_INFO, data, static_cast<uint32_t>(payload.GetLength()));
        }

        // SCHM chunk: layout descriptor for FrameSnapshot and SlowPoll.
        // For each struct: uint16_t nameLen + name, uint16_t totalSize,
        // uint16_t fieldCount, then per-field (uint16_t nameLen, name,
        // uint16_t offset, uint8_t width, char typeCode).
        // Type codes: 'u'=unsigned, 's'=signed, 'f'=float, 'b'=bool.
        void writeSchemaChunk(MemoryStream& body)
        {
            MemoryStream payload;

            struct FieldDesc
            {
                const char* name;
                uint16_t offset;
                uint8_t width;
                char typeCode;
            };

            auto writeStruct = [&](const char* structName, uint16_t totalSize,
                                   const std::vector<FieldDesc>& fields) {
                const uint16_t structNameLen = static_cast<uint16_t>(std::strlen(structName));
                payload.Write(&structNameLen, sizeof(structNameLen));
                payload.Write(structName, structNameLen);
                payload.Write(&totalSize, sizeof(totalSize));
                const uint16_t fieldCount = static_cast<uint16_t>(fields.size());
                payload.Write(&fieldCount, sizeof(fieldCount));
                for (const auto& f : fields)
                {
                    const uint16_t fnLen = static_cast<uint16_t>(std::strlen(f.name));
                    payload.Write(&fnLen, sizeof(fnLen));
                    payload.Write(f.name, fnLen);
                    payload.Write(&f.offset, sizeof(f.offset));
                    payload.Write(&f.width, sizeof(f.width));
                    payload.Write(&f.typeCode, sizeof(f.typeCode));
                }
            };

#define FIELD(structT, fieldName, code) \
    FieldDesc { #fieldName, static_cast<uint16_t>(offsetof(structT, fieldName)), \
                static_cast<uint8_t>(sizeof(static_cast<structT*>(nullptr)->fieldName)), code }

            writeStruct(
                "FrameSnapshot", static_cast<uint16_t>(sizeof(FrameSnapshot)),
                {
                    FIELD(FrameSnapshot, frameStartMs,         'u'),
                    FIELD(FrameSnapshot, frameDurationUs,      'u'),
                    FIELD(FrameSnapshot, gameTickUs,           'u'),
                    FIELD(FrameSnapshot, peepUpdateUs,         'u'),
                    FIELD(FrameSnapshot, paintWalkUs,          'u'),
                    FIELD(FrameSnapshot, paintArrangeUs,       'u'),
                    FIELD(FrameSnapshot, paintDrawUs,          'u'),
                    FIELD(FrameSnapshot, drawingEngineUs,      'u'),
                    FIELD(FrameSnapshot, audioCallbackPeakUs,  'u'),
                    FIELD(FrameSnapshot, guestCount,           'u'),
                    FIELD(FrameSnapshot, staffCount,           'u'),
                    FIELD(FrameSnapshot, vehicleCount,         'u'),
                    FIELD(FrameSnapshot, miscEntityCount,      'u'),
                    FIELD(FrameSnapshot, paintEntriesUsed,     'u'),
                    FIELD(FrameSnapshot, paintColumnCount,     'u'),
                    FIELD(FrameSnapshot, windowsDrawn,         'u'),
                    FIELD(FrameSnapshot, audioChannelCount,    'u'),
                    FIELD(FrameSnapshot, slowPollIndex,        'u'),
                });

            writeStruct(
                "SlowPoll", static_cast<uint16_t>(sizeof(SlowPoll)),
                {
                    FIELD(SlowPoll, timestampMs,        'u'),
                    FIELD(SlowPoll, rssKB,              'u'),
                    FIELD(SlowPoll, heapInUseKB,        'u'),
                    FIELD(SlowPoll, heapMmapKB,         'u'),
                    FIELD(SlowPoll, spriteScratchKB,    'u'),
                    FIELD(SlowPoll, spriteCacheHits,    'u'),
                    FIELD(SlowPoll, spriteCacheMisses,  'u'),
                    FIELD(SlowPoll, readBytesPerSec,    'u'),
                    FIELD(SlowPoll, majorFaultsPerSec,  'u'),
                    FIELD(SlowPoll, minorFaultsPerSec,  'u'),
                });

#undef FIELD

            const auto* data = static_cast<const uint8_t*>(payload.GetData());
            writeChunk(body, kFourCC_SCHM, data, static_cast<uint32_t>(payload.GetLength()));
        }

        // FRMS / SLWS: header (snapshotSize, count, startIndex) + raw POD bytes.
        void writeFrameRingChunk(MemoryStream& body)
        {
            MemoryStream payload;

            const auto& ring = getFrameRing();
            const uint32_t cap = static_cast<uint32_t>(ring.size());
            const uint32_t head = getFrameRingHead();
            const uint32_t count = getFrameRingCount();
            const uint32_t startIndex = (count >= cap) ? head : 0;
            const uint32_t snapshotSize = static_cast<uint32_t>(sizeof(FrameSnapshot));

            payload.Write(&snapshotSize, sizeof(snapshotSize));
            payload.Write(&count, sizeof(count));
            payload.Write(&startIndex, sizeof(startIndex));

            // Write the live ring slot-by-slot starting at startIndex
            // (de-rotated for the parser's convenience: oldest sample
            // first, newest last).
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t idx = (startIndex + i) % cap;
                payload.Write(&ring[idx], sizeof(FrameSnapshot));
            }

            const auto* data = static_cast<const uint8_t*>(payload.GetData());
            writeChunk(body, kFourCC_FRMS, data, static_cast<uint32_t>(payload.GetLength()));
        }

        void writeSlowRingChunk(MemoryStream& body)
        {
            MemoryStream payload;

            const auto& ring = getSlowPollRing();
            const uint32_t cap = static_cast<uint32_t>(ring.size());
            const uint32_t head = getSlowPollRingHead();
            const uint32_t count = getSlowPollRingCount();
            const uint32_t startIndex = (count >= cap) ? head : 0;
            const uint32_t snapshotSize = static_cast<uint32_t>(sizeof(SlowPoll));

            payload.Write(&snapshotSize, sizeof(snapshotSize));
            payload.Write(&count, sizeof(count));
            payload.Write(&startIndex, sizeof(startIndex));

            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t idx = (startIndex + i) % cap;
                payload.Write(&ring[idx], sizeof(SlowPoll));
            }

            const auto* data = static_cast<const uint8_t*>(payload.GetData());
            writeChunk(body, kFourCC_SLWS, data, static_cast<uint32_t>(payload.GetLength()));
        }

        // PROF: per-function summary. For each Profiling::Function:
        // uint32_t id (incrementing), name string (uint16_t len + UTF-8),
        // uint64_t callCount, uint64_t totalUs, uint64_t minUs, uint64_t maxUs.
        // Call graph children skipped in this version — would require
        // resolving Function* to id which adds complexity; not blocker
        // for analyze-pass-1.
        void writeProfChunk(MemoryStream& body)
        {
            MemoryStream payload;

            std::vector<::OpenRCT2::Profiling::Function*> snapshot;
            {
                std::scoped_lock lock(::OpenRCT2::Profiling::Detail::getRegistryMutex());
                const auto& funcs = ::OpenRCT2::Profiling::getData();
                snapshot = { funcs.begin(), funcs.end() };
            }

            const uint32_t funcCount = static_cast<uint32_t>(snapshot.size());
            payload.Write(&funcCount, sizeof(funcCount));

            for (uint32_t i = 0; i < funcCount; i++)
            {
                auto* f = snapshot[i];
                const char* name = (f != nullptr) ? f->getName() : "";
                if (name == nullptr)
                    name = "";
                const uint16_t nameLen = static_cast<uint16_t>(
                    std::min<size_t>(std::strlen(name), UINT16_MAX));

                const uint32_t id = i;
                const uint64_t callCount = (f != nullptr) ? f->getCallCount() : 0;
                // getTotalTime returns microseconds as double — convert.
                const uint64_t totalUs = (f != nullptr) ? static_cast<uint64_t>(f->getTotalTime()) : 0;
                const uint64_t minUs = (f != nullptr) ? static_cast<uint64_t>(f->getMinTime()) : 0;
                const uint64_t maxUs = (f != nullptr) ? static_cast<uint64_t>(f->getMaxTime()) : 0;

                payload.Write(&id, sizeof(id));
                payload.Write(&nameLen, sizeof(nameLen));
                if (nameLen > 0)
                    payload.Write(name, nameLen);
                payload.Write(&callCount, sizeof(callCount));
                payload.Write(&totalUs, sizeof(totalUs));
                payload.Write(&minUs, sizeof(minUs));
                payload.Write(&maxUs, sizeof(maxUs));
            }

            const auto* data = static_cast<const uint8_t*>(payload.GetData());
            writeChunk(body, kFourCC_PROF, data, static_cast<uint32_t>(payload.GetLength()));
        }

        void writeNoteChunk(MemoryStream& body, const std::string& note)
        {
            if (note.empty())
                return;
            const auto* data = reinterpret_cast<const uint8_t*>(note.data());
            const uint32_t size = static_cast<uint32_t>(
                std::min<size_t>(note.size(), std::numeric_limits<uint32_t>::max()));
            writeChunk(body, kFourCC_NOTE, data, size);
        }

        void writeEndChunk(MemoryStream& body)
        {
            writeChunk(body, kFourCC_ENDF, nullptr, 0);
        }
    } // namespace

    bool saveSnapshot(const std::string& path, const std::string& note)
    {
        // 1. Compose the body in memory.
        MemoryStream body;
        writeInfoChunk(body, note);
        writeSchemaChunk(body);
        writeFrameRingChunk(body);
        writeSlowRingChunk(body);
        writeProfChunk(body);
        writeNoteChunk(body, note);
        writeEndChunk(body);

        // 2. zstd-compress as a single frame.
        MemoryStream compressed;
        const uint64_t bodyLen = body.GetLength();
        bool compressionOk = false;
        if (bodyLen > 0)
        {
            // Need a fresh read-cursor on body for the compressor to
            // consume from position 0.
            body.SetPosition(0);
            try
            {
                compressionOk = ::OpenRCT2::Compression::zstdCompress(
                    body, bodyLen, compressed,
                    ::OpenRCT2::Compression::ZstdMetadata::checksum,
                    ::OpenRCT2::Compression::kZstdDefaultCompressionLevel);
            }
            catch (...)
            {
                compressionOk = false;
            }
        }

        // Use the compressed bytes if it worked, otherwise fall back
        // to writing the body uncompressed (still a valid file with
        // the flag bit clear).
        const uint8_t* writeData = nullptr;
        uint32_t writeLen = 0;
        uint16_t flags = 0;
        if (compressionOk && compressed.GetLength() > 0)
        {
            writeData = static_cast<const uint8_t*>(compressed.GetData());
            writeLen = static_cast<uint32_t>(compressed.GetLength());
            flags |= kFlagZstdCompressed;
        }
        else
        {
            writeData = static_cast<const uint8_t*>(body.GetData());
            writeLen = static_cast<uint32_t>(bodyLen);
        }

        // 3. CRC32 of body (post-compression if applicable).
        const uint32_t bodyCrc = static_cast<uint32_t>(
            ::crc32(0L, writeData, writeLen));

        // 4. Write header + body to disk.
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (fp == nullptr)
            return false;

        constexpr char magic[4] = { 'O', 'R', 'C', 'P' };
        const uint16_t version = kSnapshotFormatVersion;

        bool ok = true;
        ok &= (std::fwrite(magic, 1, 4, fp) == 4);
        ok &= (std::fwrite(&version, sizeof(version), 1, fp) == 1);
        ok &= (std::fwrite(&flags, sizeof(flags), 1, fp) == 1);
        ok &= (std::fwrite(&writeLen, sizeof(writeLen), 1, fp) == 1);
        ok &= (std::fwrite(&bodyCrc, sizeof(bodyCrc), 1, fp) == 1);
        if (writeLen > 0)
            ok &= (std::fwrite(writeData, 1, writeLen, fp) == writeLen);

        std::fclose(fp);
        return ok;
    }
} // namespace OpenRCT2::Profiling::Sampler

#endif // ENABLE_PERFORMANCE_PROFILER
