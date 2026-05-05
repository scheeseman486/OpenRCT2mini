/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "AudioSampleTable.h"

#include "../Context.h"
#include "../PlatformEnvironment.h"
#include "../audio/AudioContext.h"
#include "../core/File.h"
#include "../core/Json.hpp"
#include "../core/Path.hpp"
#include "../localisation/Formatting.h"
#include "../localisation/StringIds.h"
#include "../ui/UiContext.h"
#include "Object.h"

namespace OpenRCT2
{
    std::vector<AudioSampleTable::Entry>& AudioSampleTable::GetEntries()
    {
        return _entries;
    }

    void AudioSampleTable::ReadFromJson(IReadObjectContext* context, const json_t& root)
    {
        json_t jSamples = root["samples"];
        if (jSamples.is_array())
        {
            for (auto& jSample : jSamples)
            {
                SourceInfo sourceInfo;
                int32_t modifier{};
                if (jSample.is_string())
                {
                    sourceInfo = ParseSource(jSample.get<std::string>());
                }
                else if (jSample.is_object())
                {
                    auto& jSource = jSample.at("source");
                    if (jSource.is_string())
                    {
                        sourceInfo = ParseSource(jSource.get<std::string>());
                        if (jSample.contains("modifier"))
                        {
                            auto& jModifier = jSample.at("modifier");
                            if (jModifier.is_number())
                            {
                                modifier = jModifier.get<int32_t>();
                            }
                        }
                    }
                }

                auto asset = context->GetAsset(sourceInfo.Path);
                if (!sourceInfo.SourceRange)
                {
                    auto& entry = _entries.emplace_back();
                    entry.Asset = asset;
                    entry.Modifier = modifier;
                }
                else
                {
                    Range<int32_t> r(1, 5);
                    for (auto index : *sourceInfo.SourceRange)
                    {
                        auto& entry = _entries.emplace_back();
                        entry.Asset = asset;
                        entry.PathIndex = index;
                        entry.Modifier = modifier;
                    }
                }
            }
        }
    }

    void AudioSampleTable::LoadFrom(const AudioSampleTable& table, size_t sourceStartIndex, size_t length)
    {
        // Ensure we stay in bounds of source table
        if (sourceStartIndex >= table._entries.size())
            return;
        length = std::min(length, table._entries.size() - sourceStartIndex);

        // Asset packs may allocate more images for an object that original, or original object may
        // not allocate any images at all.
        if (_entries.size() < length)
        {
            _entries.resize(length);
        }

        for (size_t i = 0; i < length; i++)
        {
            const auto& sourceEntry = table._entries[sourceStartIndex + i];
            if (sourceEntry.Asset)
            {
                auto stream = sourceEntry.Asset->GetStream();
                if (stream != nullptr)
                {
                    auto& entry = _entries[i];
                    entry.Asset = sourceEntry.Asset;
                    entry.PathIndex = sourceEntry.PathIndex;
                    entry.Modifier = sourceEntry.Modifier;
                }
            }
        }
    }

    void AudioSampleTable::Load()
    {
        // OPENRCT2MINI: cut 28. Eager pre-decode of every CSS sample into PCM RAM was
        // measured at ~10 MB live (CSS1.DAT 63 entries + CSS2.DAT, all converted to the
        // mixer's 22050 Hz S16-stereo target). Most sound effects (screams, brake squeals,
        // splashes, balloon pops, ...) only fire on rare events and many never play during
        // a session. Defer decoding to first play via GetSample(idx); the disk read +
        // PCM-extract is a one-shot cheap operation, and after it's loaded GetSample
        // returns the cached pointer with no additional cost.
    }

    void AudioSampleTable::Unload()
    {
        for (auto& entry : _entries)
        {
            if (entry.Source != nullptr)
            {
                entry.Source->Release();
                entry.Source = nullptr;
            }
        }
    }

    size_t AudioSampleTable::GetCount() const
    {
        return _entries.size();
    }

    Audio::IAudioSource* AudioSampleTable::GetSample(uint32_t index) const
    {
        if (index >= _entries.size())
            return nullptr;
        auto& entry = _entries[index];
        // OPENRCT2MINI: cut 28. Lazy decode-on-first-play. The cached pointer is owned by
        // the audio context (it AddSource's the unique_ptr into a context-side list);
        // we just remember it here so subsequent plays don't re-decode the same sample.
        if (entry.Source == nullptr)
        {
            entry.Source = LoadSample(index);
        }
        return entry.Source;
    }

    Audio::IAudioSource* AudioSampleTable::LoadSample(uint32_t index) const
    {
        Audio::IAudioSource* result{};
        if (index < _entries.size())
        {
            auto& entry = _entries[index];
            if (entry.Asset)
            {
                auto stream = entry.Asset->GetStream();
                if (stream != nullptr)
                {
                    auto& audioContext = GetContext()->GetAudioContext();
                    if (entry.PathIndex)
                    {
                        auto originalPosition = stream->GetPosition();
                        auto numSounds = stream->ReadValue<uint32_t>();
                        stream->SetPosition(originalPosition);

                        if (*entry.PathIndex >= numSounds)
                        {
                            auto& ui = GetContext()->GetUiContext();
                            ui.ShowMessageBox(FormatStringID(
                                STR_AUDIO_FILE_TRUNCATED, entry.Asset->GetPath().c_str(), *entry.PathIndex, numSounds));
                        }

                        result = audioContext.CreateStreamFromCSS(std::move(stream), *entry.PathIndex);
                    }
                    else
                    {
                        result = audioContext.CreateStreamFromWAV(std::move(stream));
                    }
                }
            }
        }
        return result;
    }

    int32_t AudioSampleTable::GetSampleModifier(uint32_t index) const
    {
        if (index < _entries.size())
        {
            return _entries[index].Modifier;
        }
        return 0;
    }
} // namespace OpenRCT2
