/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "RideAudio.h"

#include "../Context.h"
#include "../OpenRCT2.h"
#include "../audio/Audio.h"
#include "../audio/AudioChannel.h"
#include "../audio/AudioContext.h"
#include "../audio/AudioMixer.h"
#include "../config/Config.h"
#include "../interface/Viewport.h"
#include "../object/AudioObject.h"
#include "../object/MusicObject.h"
#include "../object/ObjectManager.h"
#include "Ride.h"
#include "RideData.h"

#include <algorithm>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::Audio;

namespace OpenRCT2::RideAudio
{
    // OPENRCT2MINI: cut from 32 → 3. With software mixing + on-demand
    // disk streaming on Miyoo Mini hardware, ~4 concurrent music tracks
    // produced choppy audio in user testing on dense parks. The upstream
    // FCFS rule (first 32 rides to enumerate win) is also replaced with
    // priority eviction in RideUpdateMusicPosition — at cap, the quietest
    // current instance is replaced if the new ride would be louder. The
    // existing per-ride volume formula (CalculateVolume / newVolume in
    // UpdateMusicInstance) already encodes "distance from screen-center",
    // so it's a free priority key.
    constexpr size_t MAX_RIDE_MUSIC_CHANNELS = 3;

    // OPENRCT2MINI: eviction hysteresis. Volume is a int16 in the rough
    // range [-4000 (very quiet), -700 (loudest)] — about 3300 units of
    // dynamic range. To prevent two rides with near-equal volumes from
    // ping-ponging in and out of the cap-3 set every frame (which causes
    // audible mid-track cuts as channels are torn down and recreated),
    // a challenger must beat an incumbent's volume by at least this many
    // units to displace it. ~6 % of the dynamic range — small enough to
    // let a genuinely louder ride through, large enough to absorb the
    // per-frame volume jitter from camera moves and ride travel.
    //
    // We only apply hysteresis to incumbents (rides that already have a
    // playing channel from the previous frame); fresh challengers can
    // still freely contest cap slots against each other within a single
    // frame's enumeration, so the loudest 3 still end up in the set
    // regardless of enumeration order.
    constexpr int16_t kMusicEvictionHysteresis = 200;

    /**
     * Represents an audio channel to play a particular ride's music track.
     */
    struct RideMusicChannel
    {
        ::RideId RideId{};
        uint8_t TrackIndex{};

        size_t Offset{};
        int16_t Volume{};
        int16_t Pan{};
        uint16_t Frequency{};

        std::shared_ptr<IAudioChannel> Channel{};
        IAudioSource* Source{};

        RideMusicChannel(
            const ViewportRideMusicInstance& instance, std::shared_ptr<IAudioChannel> channel, IAudioSource* source)
        {
            RideId = instance.RideId;
            TrackIndex = instance.TrackIndex;

            Offset = std::max<size_t>(0, instance.Offset - 10000);
            Volume = instance.Volume;
            Pan = instance.Pan;
            Frequency = instance.Frequency;

            channel->SetOffset(Offset);
            channel->SetVolume(DStoMixerVolume(Volume));
            channel->SetPan(DStoMixerPan(Pan));
            channel->SetRate(DStoMixerRate(Frequency));
            Channel = std::move(channel);

            Source = source;
        }

        RideMusicChannel(const RideMusicChannel&) = delete;

        RideMusicChannel(RideMusicChannel&& src) noexcept
        {
            *this = std::move(src);
        }

        RideMusicChannel& operator=(RideMusicChannel&& src) noexcept
        {
            using std::swap;

            RideId = src.RideId;
            TrackIndex = src.TrackIndex;

            Offset = src.Offset;
            Volume = src.Volume;
            Pan = src.Pan;
            Frequency = src.Frequency;

            swap(Channel, src.Channel);
            swap(Source, src.Source);

            return *this;
        }

        ~RideMusicChannel()
        {
            if (Channel != nullptr)
            {
                Channel->Stop();
            }
            if (Source != nullptr)
            {
                Source->Release();
            }
        }

        bool IsPlaying() const
        {
            if (Channel != nullptr)
            {
                return Channel->IsPlaying();
            }
            return false;
        }

        size_t GetOffset() const
        {
            if (Channel != nullptr)
            {
                return Channel->GetOffset();
            }
            return 0;
        }

        void Update(const ViewportRideMusicInstance& instance)
        {
            if (Volume != instance.Volume)
            {
                Volume = instance.Volume;
                if (Channel != nullptr)
                {
                    Channel->SetVolume(DStoMixerVolume(Volume));
                }
            }
            if (Pan != instance.Pan)
            {
                Pan = instance.Pan;
                if (Channel != nullptr)
                {
                    Channel->SetPan(DStoMixerPan(Pan));
                }
            }
            if (Frequency != instance.Frequency)
            {
                Frequency = instance.Frequency;
                if (Channel != nullptr)
                {
                    Channel->SetRate(DStoMixerRate(Frequency));
                }
            }
        }
    };

    static std::vector<ViewportRideMusicInstance> _musicInstances;
    static std::vector<RideMusicChannel> _musicChannels;

    void StopAllChannels()
    {
        _musicChannels.clear();
    }

    void ClearAllViewportInstances()
    {
        _musicInstances.clear();
    }

    void DefaultStartRideMusicChannel(const ViewportRideMusicInstance& instance)
    {
        auto& objManager = GetContext()->GetObjectManager();
        auto ride = GetRide(instance.RideId);
        auto musicObj = objManager.GetLoadedObject<MusicObject>(ride->music);
        if (musicObj != nullptr)
        {
            auto shouldLoop = musicObj->GetTrackCount() == 1;
            auto source = musicObj->GetTrackSample(instance.TrackIndex);
            if (source != nullptr)
            {
                auto channel = CreateAudioChannel(source, MixerGroup::RideMusic, shouldLoop, 0);
                if (channel != nullptr)
                {
                    _musicChannels.emplace_back(instance, channel, source);
                }
            }
        }
    }
    void CircusStartRideMusicChannel(const ViewportRideMusicInstance& instance)
    {
        auto& objManager = GetContext()->GetObjectManager();
        ObjectEntryDescriptor desc(ObjectType::audio, AudioObjectIdentifiers::kRCT2Circus);
        auto audioObj = static_cast<AudioObject*>(objManager.GetLoadedObject(desc));
        if (audioObj != nullptr)
        {
            auto source = audioObj->GetSample(0);
            if (source != nullptr)
            {
                auto channel = CreateAudioChannel(source, MixerGroup::Sound, false, 0);
                if (channel != nullptr)
                {
                    _musicChannels.emplace_back(instance, channel, nullptr);
                }
            }
        }
    }

    static void StartRideMusicChannel(const ViewportRideMusicInstance& instance)
    {
        // Create new music channel
        auto ride = GetRide(instance.RideId);
        const auto& rtd = ride->getRideTypeDescriptor();
        rtd.StartRideMusic(instance);
    }

    static void StopInactiveRideMusicChannels()
    {
        _musicChannels.erase(
            std::remove_if(
                _musicChannels.begin(), _musicChannels.end(),
                [](const auto& channel) {
                    auto found = std::any_of(_musicInstances.begin(), _musicInstances.end(), [&channel](const auto& instance) {
                        return instance.RideId == channel.RideId && instance.TrackIndex == channel.TrackIndex;
                    });
                    if (!found || !channel.IsPlaying())
                    {
                        return true;
                    }

                    return false;
                }),
            _musicChannels.end());
    }

    static void UpdateRideMusicChannelForMusicParams(const ViewportRideMusicInstance& instance)
    {
        // Find existing music channel
        auto foundChannel = std::find_if(
            _musicChannels.begin(), _musicChannels.end(), [&instance](const RideMusicChannel& channel) {
                return channel.RideId == instance.RideId && channel.TrackIndex == instance.TrackIndex;
            });

        if (foundChannel != _musicChannels.end())
        {
            foundChannel->Update(instance);
        }
        else if (_musicChannels.size() < MAX_RIDE_MUSIC_CHANNELS)
        {
            StartRideMusicChannel(instance);
        }
    }

    /**
     * Start, update and stop audio channels for each ride music instance that can be heard across all viewports.
     */
    void UpdateMusicChannels()
    {
        if (gLegacyScene == LegacyScene::scenarioEditor || gLegacyScene == LegacyScene::titleSequence)
            return;

        // TODO Allow circus music (CSS24) to play if ride music is disabled (that should be sound)
        if (gGameSoundsOff || !Config::Get().sound.rideMusicEnabled)
            return;

        StopInactiveRideMusicChannels();
        for (const auto& instance : _musicInstances)
        {
            UpdateRideMusicChannelForMusicParams(instance);
        }
    }

    std::pair<size_t, size_t> RideMusicGetTrackOffsetLength_Circus(const Ride& ride)
    {
        return { 1378, 12427456 };
    }

    std::pair<size_t, size_t> RideMusicGetTrackOffsetLength_Default(const Ride& ride)
    {
        auto& objManager = GetContext()->GetObjectManager();
        auto musicObj = objManager.GetLoadedObject<MusicObject>(ride.music);
        if (musicObj != nullptr)
        {
            auto numTracks = musicObj->GetTrackCount();
            if (ride.musicTuneId < numTracks)
            {
                auto track = musicObj->GetTrack(ride.musicTuneId);
                return { track->BytesPerTick, track->Size };
            }
        }
        return { 0, 0 };
    }

    static std::pair<size_t, size_t> RideMusicGetTrackOffsetLength(const Ride& ride)
    {
        const auto& rtd = ride.getRideTypeDescriptor();
        return rtd.MusicTrackOffsetLength(ride);
    }

    static void RideUpdateMusicPosition(Ride& ride)
    {
        auto [trackOffset, trackLength] = RideMusicGetTrackOffsetLength(ride);
        auto position = ride.musicPosition + trackOffset;
        if (position < trackLength)
        {
            ride.musicPosition = position;
        }
        else
        {
            ride.musicTuneId = kTuneIDNull;
            ride.musicPosition = 0;
        }
    }

    static void RideUpdateMusicPosition(
        Ride& ride, size_t offset, size_t length, int16_t volume, int16_t pan, uint16_t sampleRate)
    {
        if (offset < length)
        {
            // OPENRCT2MINI: replaced upstream's FCFS rule with
            // priority-based eviction. Volume here is the per-frame
            // loudness number from UpdateMusicInstance — a negative
            // int16 where higher (closer to zero) means closer to the
            // viewport centre. When the instance list is at cap, drop
            // the quietest current instance if this one would be
            // louder. Each frame _musicInstances is cleared by
            // ClearAllViewportInstances() before rides re-register, so
            // by the time UpdateMusicChannels runs, _musicInstances
            // holds exactly the loudest N rides — and the existing
            // StopInactiveRideMusicChannels / channel-allocation flow
            // does the rest (tears down channels for evicted rides,
            // creates channels for newly-promoted ones).
            if (_musicInstances.size() < MAX_RIDE_MUSIC_CHANNELS)
            {
                auto& instance = _musicInstances.emplace_back();
                instance.RideId = ride.id;
                instance.TrackIndex = ride.musicTuneId;
                instance.Offset = offset;
                instance.Volume = volume;
                instance.Pan = pan;
                instance.Frequency = sampleRate;
            }
            else
            {
                auto quietest = std::min_element(
                    _musicInstances.begin(), _musicInstances.end(),
                    [](const ViewportRideMusicInstance& a, const ViewportRideMusicInstance& b) {
                        return a.Volume < b.Volume;
                    });
                if (quietest != _musicInstances.end())
                {
                    // OPENRCT2MINI: incumbents (rides whose channel is
                    // still alive from the previous frame) get a
                    // hysteresis bonus to prevent thrash. _musicChannels
                    // is only torn down later, in
                    // StopInactiveRideMusicChannels, so it still
                    // reflects last frame's state during this pass.
                    const bool quietestIsIncumbent = std::any_of(
                        _musicChannels.begin(), _musicChannels.end(),
                        [&](const RideMusicChannel& ch) {
                            return ch.RideId == quietest->RideId && ch.TrackIndex == quietest->TrackIndex;
                        });
                    const int16_t threshold = quietest->Volume
                        + (quietestIsIncumbent ? kMusicEvictionHysteresis : int16_t{ 0 });
                    if (volume > threshold)
                    {
                        quietest->RideId = ride.id;
                        quietest->TrackIndex = ride.musicTuneId;
                        quietest->Offset = offset;
                        quietest->Volume = volume;
                        quietest->Pan = pan;
                        quietest->Frequency = sampleRate;
                    }
                }
            }
            ride.musicPosition = static_cast<uint32_t>(offset);
        }
        else
        {
            ride.musicTuneId = kTuneIDNull;
            ride.musicPosition = 0;
        }
    }

    static void RideUpdateMusicPosition(Ride& ride, int16_t volume, int16_t pan, uint16_t sampleRate)
    {
        auto foundChannel = std::find_if(_musicChannels.begin(), _musicChannels.end(), [&ride](const auto& channel) {
            return channel.RideId == ride.id && channel.TrackIndex == ride.musicTuneId;
        });

        auto [trackOffset, trackLength] = RideMusicGetTrackOffsetLength(ride);
        if (foundChannel != _musicChannels.end())
        {
            if (foundChannel->IsPlaying())
            {
                // Since we have a real music channel, use the offset from that
                auto newOffset = foundChannel->GetOffset();
                RideUpdateMusicPosition(ride, newOffset, trackLength, volume, pan, sampleRate);
            }
            else
            {
                // We had a real music channel, but it isn't playing anymore, so stop the track
                ride.musicPosition = 0;
                ride.musicTuneId = kTuneIDNull;
            }
        }
        else
        {
            // We do not have a real music channel, so simulate the playing of the music track
            auto newOffset = ride.musicPosition + trackOffset;
            RideUpdateMusicPosition(ride, newOffset, trackLength, volume, pan, sampleRate);
        }
    }

    static uint8_t CalculateVolume(int32_t pan)
    {
        uint8_t result = 255;
        int32_t v = std::min(std::abs(pan), 6143) - 2048;
        if (v > 0)
        {
            v = -((v / 4) - 1024) / 4;
            result = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
        return result;
    }

    /**
     * Register an instance of audible ride music for this frame at the given coordinates.
     */
    void UpdateMusicInstance(Ride& ride, const CoordsXYZ& rideCoords, uint16_t sampleRate)
    {
        if (gLegacyScene != LegacyScene::scenarioEditor && !gGameSoundsOff && gMusicTrackingViewport != nullptr)
        {
            auto rotatedCoords = Translate3DTo2DWithZ(GetCurrentRotation(), rideCoords);
            auto viewport = gMusicTrackingViewport;
            auto viewWidth = viewport->ViewWidth();
            auto viewWidth2 = viewWidth * 2;
            auto viewX = viewport->viewPos.x - viewWidth2;
            auto viewY = viewport->viewPos.y - viewWidth;
            auto viewX2 = viewWidth2 + viewWidth2 + viewport->ViewWidth() + viewX;
            auto viewY2 = viewWidth + viewWidth + viewport->ViewHeight() + viewY;
            if (viewX >= rotatedCoords.x || viewY >= rotatedCoords.y || viewX2 < rotatedCoords.x || viewY2 < rotatedCoords.y)
            {
                RideUpdateMusicPosition(ride);
            }
            else
            {
                auto x2 = (viewport->pos.x + viewport->zoom.ApplyInversedTo(rotatedCoords.x - viewport->viewPos.x)) * 0x10000;
                auto screenWidth = std::max(ContextGetWidth(), 64);
                auto panX = ((x2 / screenWidth) - 0x8000) >> 4;

                auto y2 = (viewport->pos.y + viewport->zoom.ApplyInversedTo(rotatedCoords.y - viewport->viewPos.y)) * 0x10000;
                auto screenHeight = std::max(ContextGetHeight(), 64);
                auto panY = ((y2 / screenHeight) - 0x8000) >> 4;

                auto volX = CalculateVolume(panX);
                auto volY = CalculateVolume(panY);
                auto volXY = std::min(volX, volY);
                if (volXY < gVolumeAdjustZoom * 3)
                {
                    volXY = 0;
                }
                else
                {
                    volXY = volXY - (gVolumeAdjustZoom * 3);
                }

                int16_t newVolume = -((static_cast<uint8_t>(-volXY - 1) * static_cast<uint8_t>(-volXY - 1)) / 16) - 700;
                if (volXY != 0 && newVolume >= -4000)
                {
                    auto newPan = std::clamp(panX, -10000, 10000);
                    RideUpdateMusicPosition(ride, newVolume, newPan, sampleRate);
                }
                else
                {
                    RideUpdateMusicPosition(ride);
                }
            }
        }
    }
} // namespace OpenRCT2::RideAudio
