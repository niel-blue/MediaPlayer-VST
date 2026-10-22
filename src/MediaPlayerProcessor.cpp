#include "MediaPlayerProcessor.h"

#include "MediaPlayerIds.h"
#include "MediaPlayerMessages.h"
#include "MediaPlayerDebugLog.h"

#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace MediaPlayer {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr uint32 kStateMagic = 0x3153504D; // "MPS1" in little-endian form.
constexpr uint32 kStateVersion = 2;
constexpr uint32 kLegacyStateVersion = 1;
constexpr uint32 kMaxPlaylistEntries =
    static_cast<uint32>(PlaylistModel::kMaximumEntries);
constexpr int32 kMaxPathBytes = 1024 * 1024;
constexpr int32 kDefaultPlaylistHeight = 60;
constexpr int32 kMinPlaylistHeight = 60;
// Keep the host-facing volume control at its existing 0.0..1.0 range while
// leaving a small amount of headroom for normal media sources.
constexpr float kOutputGainScale = 0.5f;

bool writePath(Steinberg::IBStreamer& stream,
               const std::filesystem::path& path)
{
    const auto utf8Path = path.u8string();
    if (utf8Path.empty() ||
        utf8Path.size() > static_cast<std::size_t>(kMaxPathBytes))
        return false;

    const auto size = static_cast<int32>(utf8Path.size());
    return stream.writeInt32(size) &&
           stream.writeRaw(utf8Path.data(), size) == size;
}

bool readPath(Steinberg::IBStreamer& stream, std::filesystem::path& path)
{
    int32 size = 0;
    if (!stream.readInt32(size) || size <= 0 || size > kMaxPathBytes)
        return false;

    std::string utf8Path(static_cast<std::size_t>(size), '\0');
    if (stream.readRaw(utf8Path.data(), size) != size)
        return false;

    try {
        path = std::filesystem::u8path(utf8Path.data(),
                                       utf8Path.data() + utf8Path.size());
    } catch (...) {
        return false;
    }
    return !path.empty();
}

} // namespace

tresult PLUGIN_API Processor::initialize(FUnknown* context)
{
    const auto result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    // The plug-in is a media source. It intentionally has no audio input and
    // exposes one fixed stereo output for Cantabile routing.
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    setControllerClass(kControllerUID);

    return kResultOk;
}

tresult PLUGIN_API Processor::setActive(TBool state)
{
    if (!state) {
        if (active_) {
            const auto snapshot = audioEngine_.snapshot();
            const auto playlist = playlist_.snapshot();
            std::lock_guard lock(restoreStateMutex_);
            restoreLoaded_ = snapshot.loaded &&
                             playlist.currentIndex >= 0 &&
                             playlist.currentIndex <
                                 static_cast<std::int32_t>(playlist.entries.size());
            restorePlaying_ = restoreLoaded_ && snapshot.playing;
            restorePositionFrames_ = snapshot.positionFrames;
            restoreDurationFrames_ = snapshot.durationFrames;
            restoreVolume_ = snapshot.volume;
            stateRestorePending_ = true;
        }
        audioEngine_.stop();
    }

    const auto result = AudioEffect::setActive(state);
    if (result != kResultOk)
        return result;

    if (state) {
        audioEngine_.start();
        active_ = true;
        bool restorePending = false;
        {
            std::lock_guard lock(restoreStateMutex_);
            restorePending = stateRestorePending_;
        }
        if (restorePending)
            applyRestoredState();
    } else {
        active_ = false;
    }
    return result;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup)
{
    audioEngine_.setOutputSampleRate(
        static_cast<int>(std::lround(setup.sampleRate)));
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setBusArrangements(
    SpeakerArrangement* inputs,
    int32 numIns,
    SpeakerArrangement* outputs,
    int32 numOuts)
{
    // This is a source plug-in: no input buses, exactly one stereo output.
    if (numIns != 0 || numOuts != 1 || outputs == nullptr ||
        outputs[0] != SpeakerArr::kStereo)
        return kResultFalse;

    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API Processor::process(ProcessData& data)
{
    if (data.numSamples <= 0 || data.numOutputs <= 0 || data.outputs == nullptr)
        return kResultOk;

    auto& output = data.outputs[0];
    if (output.numChannels <= 0)
        return kResultOk;

    if (audioEngine_.consumeClearRequest())
        audioEngine_.audioQueue().clear();

    const auto snapshot = audioEngine_.snapshot();
    if (!snapshot.playing) {
        for (int32 channel = 0; channel < output.numChannels; ++channel) {
            if (data.symbolicSampleSize == kSample32) {
                std::memset(output.channelBuffers32[channel], 0,
                            static_cast<size_t>(data.numSamples) * sizeof(float));
            } else if (data.symbolicSampleSize == kSample64) {
                std::memset(output.channelBuffers64[channel], 0,
                            static_cast<size_t>(data.numSamples) * sizeof(double));
            } else {
                return kResultFalse;
            }
        }
        output.silenceFlags = getChannelMask(output.numChannels);
        return kResultOk;
    }

    auto& audioQueue = audioEngine_.audioQueue();
    const auto logUnderrun = [&](std::size_t requested,
                                 std::size_t framesRead,
                                 std::size_t availableBefore) {
        if (!snapshot.playing || framesRead >= requested)
            return;

        std::ostringstream message;
        message << "AUDIO_UNDERRUN requested=" << requested
                << " framesRead=" << framesRead
                << " availableBefore=" << availableBefore
                << " availableAfter=" << audioQueue.availableFrames()
                << " sampleRate=" << snapshot.sampleRate
                << " volume=" << snapshot.volume;
        DebugLog::write(message.str());
    };
    if (data.symbolicSampleSize == kSample32) {
        const auto availableBefore = audioQueue.availableFrames();
        const auto framesRead = output.numChannels >= 2
            ? audioQueue.readStereo(output.channelBuffers32[0],
                                    output.channelBuffers32[1],
                                    static_cast<size_t>(data.numSamples))
            : 0;
        audioEngine_.reportConsumedFrames(framesRead);
        logUnderrun(static_cast<std::size_t>(data.numSamples),
                    framesRead,
                    availableBefore);
        const auto gain = snapshot.volume * kOutputGainScale;
        for (std::size_t frame = 0; frame < framesRead; ++frame) {
            output.channelBuffers32[0][frame] *= gain;
            output.channelBuffers32[1][frame] *= gain;
        }

        for (int32 channel = 0; channel < output.numChannels; ++channel) {
            if (framesRead < static_cast<size_t>(data.numSamples)) {
                std::memset(output.channelBuffers32[channel] + framesRead, 0,
                            (static_cast<size_t>(data.numSamples) - framesRead) *
                                sizeof(float));
            }
        }

        output.silenceFlags = framesRead == 0
            ? getChannelMask(output.numChannels)
            : 0;
    } else if (data.symbolicSampleSize == kSample64) {
        const auto availableBefore = audioQueue.availableFrames();
        bool producedAudio = false;
        std::size_t consumedFrames = 0;
        for (int32 sample = 0; sample < data.numSamples; ++sample) {
            float left = 0.0f;
            float right = 0.0f;
            const auto framesRead = output.numChannels >= 2
                ? audioQueue.readStereo(&left, &right, 1)
                : 0;

            output.channelBuffers64[0][sample] = framesRead == 1 ? left : 0.0;
            if (framesRead == 1) {
                producedAudio = true;
                ++consumedFrames;
            }
            if (output.numChannels >= 2)
                output.channelBuffers64[1][sample] = framesRead == 1 ? right : 0.0;
        }

        for (int32 channel = 2; channel < output.numChannels; ++channel)
            std::memset(output.channelBuffers64[channel], 0,
                        static_cast<size_t>(data.numSamples) * sizeof(double));

        const auto gain = static_cast<double>(snapshot.volume) *
                          static_cast<double>(kOutputGainScale);
        for (int32 sample = 0; sample < data.numSamples; ++sample) {
            output.channelBuffers64[0][sample] *= gain;
            if (output.numChannels >= 2)
                output.channelBuffers64[1][sample] *= gain;
        }
        audioEngine_.reportConsumedFrames(consumedFrames);
        logUnderrun(static_cast<std::size_t>(data.numSamples),
                    consumedFrames,
                    availableBefore);

        output.silenceFlags = producedAudio ? 0 : getChannelMask(output.numChannels);
    } else {
        return kResultFalse;
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64
        ? kResultTrue
        : kResultFalse;
}

tresult PLUGIN_API Processor::setState(IBStream* state)
{
    if (state == nullptr)
        return kInvalidArgument;

    IBStreamer stream(state, kLittleEndian);
    uint32 magic = 0;
    if (!stream.readInt32u(magic)) {
        // Older builds wrote an empty state. Treat that as the default state.
        return kResultOk;
    }
    uint32 version = 0;
    if (magic != kStateMagic || !stream.readInt32u(version) ||
        (version != kLegacyStateVersion && version != kStateVersion))
        return kResultFalse;

    uint32 playlistCount = 0;
    int32 currentIndex = -1;
    int32 loaded = 0;
    int32 playing = 0;
    float volume = 1.0f;
    uint64 positionFrames = 0;
    uint64 durationFrames = 0;
    if (!stream.readInt32u(playlistCount) ||
        playlistCount > kMaxPlaylistEntries ||
        !stream.readInt32(currentIndex) ||
        !stream.readInt32(loaded) ||
        !stream.readInt32(playing) ||
        !stream.readFloat(volume) ||
        !stream.readInt64u(positionFrames) ||
        !stream.readInt64u(durationFrames))
        return kResultFalse;

    VideoWindowPlacement videoPlacement {};
    EditorLayoutState editorLayout {};
    editorLayout.playlistHeight = kDefaultPlaylistHeight;
    if (version >= kStateVersion) {
        int32 videoPlacementValid = 0;
        int32 videoPlacementUserSized = 0;
        int32 videoPlacementUserPosition = 0;
        int32 playlistVisible = 0;
        if (!stream.readInt32(videoPlacementValid) ||
            !stream.readInt32(videoPlacement.x) ||
            !stream.readInt32(videoPlacement.y) ||
            !stream.readInt32(videoPlacement.width) ||
            !stream.readInt32(videoPlacement.height) ||
            !stream.readInt32(videoPlacementUserSized) ||
            !stream.readInt32(videoPlacementUserPosition) ||
            !stream.readInt32(playlistVisible) ||
            !stream.readInt32(editorLayout.playlistHeight))
            return kResultFalse;

        videoPlacement.valid = videoPlacementValid != 0;
        videoPlacement.userSized = videoPlacementUserSized != 0;
        videoPlacement.userPosition = videoPlacementUserPosition != 0;
        editorLayout.playlistVisible = playlistVisible != 0;
    }

    if (!videoPlacement.valid || videoPlacement.width <= 0 ||
        videoPlacement.height <= 0) {
        videoPlacement = {};
    }
    editorLayout.playlistHeight = std::max(
        editorLayout.playlistHeight, kMinPlaylistHeight);

    std::vector<std::filesystem::path> playlist;
    playlist.reserve(playlistCount);
    for (uint32 index = 0; index < playlistCount; ++index) {
        std::filesystem::path path;
        if (!readPath(stream, path))
            return kResultFalse;
        playlist.push_back(std::move(path));
    }

    if (currentIndex < -1 ||
        currentIndex >= static_cast<int32>(playlist.size()))
        currentIndex = -1;
    if (loaded == 0 || currentIndex < 0) {
        loaded = 0;
        playing = 0;
        positionFrames = 0;
        durationFrames = 0;
    }
    if (!std::isfinite(volume))
        volume = 1.0f;
    volume = std::clamp(volume, 0.0f, 1.0f);

    playlist_.restore(std::move(playlist), currentIndex);
    {
        std::lock_guard lock(restoreStateMutex_);
        restoreLoaded_ = loaded != 0;
        restorePlaying_ = restoreLoaded_ && playing != 0;
        restoreVolume_ = volume;
        restorePositionFrames_ = positionFrames;
        restoreDurationFrames_ = durationFrames;
        stateRestorePending_ = true;
    }
    {
        std::lock_guard lock(uiStateMutex_);
        videoPlacement_ = videoPlacement;
        editorLayout_ = editorLayout;
    }
    audioEngine_.setVolume(volume);
    if (active_)
        applyRestoredState();

    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state)
{
    if (state == nullptr)
        return kInvalidArgument;

    const auto playlist = playlist_.snapshot();

    const auto snapshot = audioEngine_.snapshot();
    const auto loadedPath = audioEngine_.currentFilePath();
    const auto loaded = snapshot.loaded && playlist.currentIndex >= 0 &&
                        playlist.currentIndex <
                            static_cast<std::int32_t>(playlist.entries.size()) &&
                        loadedPath == playlist.entries[
                            static_cast<std::size_t>(playlist.currentIndex)];
    const auto playing = loaded && snapshot.playing;
    VideoWindowPlacement videoPlacement {};
    EditorLayoutState editorLayout {};
    {
        std::lock_guard lock(uiStateMutex_);
        videoPlacement = videoPlacement_;
        editorLayout = editorLayout_;
    }

    IBStreamer stream(state, kLittleEndian);
    if (!stream.writeInt32u(kStateMagic) ||
        !stream.writeInt32u(kStateVersion) ||
        !stream.writeInt32u(static_cast<uint32>(playlist.entries.size())) ||
        !stream.writeInt32(playlist.currentIndex) ||
        !stream.writeInt32(loaded ? 1 : 0) ||
        !stream.writeInt32(playing ? 1 : 0) ||
        !stream.writeFloat(snapshot.volume) ||
        !stream.writeInt64u(loaded ? snapshot.positionFrames : 0) ||
        !stream.writeInt64u(loaded ? snapshot.durationFrames : 0) ||
        !stream.writeInt32(videoPlacement.valid ? 1 : 0) ||
        !stream.writeInt32(videoPlacement.x) ||
        !stream.writeInt32(videoPlacement.y) ||
        !stream.writeInt32(videoPlacement.width) ||
        !stream.writeInt32(videoPlacement.height) ||
        !stream.writeInt32(videoPlacement.userSized ? 1 : 0) ||
        !stream.writeInt32(videoPlacement.userPosition ? 1 : 0) ||
        !stream.writeInt32(editorLayout.playlistVisible ? 1 : 0) ||
        !stream.writeInt32(editorLayout.playlistHeight))
        return kResultFalse;

    for (const auto& path : playlist.entries) {
        if (!writePath(stream, path))
            return kResultFalse;
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::notify(IMessage* message)
{
    if (message == nullptr)
        return kInvalidArgument;

    if (FIDStringsEqual(message->getMessageID(), kOpenFileMessage)) {
        const void* data = nullptr;
        uint32 size = 0;
        const auto result = message->getAttributes()->getBinary(
            kFilePathAttribute, data, size);
        if (result != kResultOk || data == nullptr || size == 0)
            return kInvalidArgument;

        try {
            const auto* bytes = static_cast<const char*>(data);
            requestOpenFile(std::filesystem::u8path(bytes, bytes + size));
        } catch (...) {
            return kResultFalse;
        }
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kSelectPlaylistMessage)) {
        const void* data = nullptr;
        uint32 size = 0;
        int64 play = 0;
        if (message->getAttributes()->getBinary(
                kPlaylistPathAttribute, data, size) != kResultOk ||
            data == nullptr || size == 0 ||
            message->getAttributes()->getInt(kPlaylistPlayAttribute, play) !=
                kResultOk)
            return kInvalidArgument;

        try {
            const auto* bytes = static_cast<const char*>(data);
            selectPlaylist(std::filesystem::u8path(bytes, bytes + size),
                           play != 0);
        } catch (...) {
            return kResultFalse;
        }
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kClearPlaylistMessage)) {
        clearPlaylist();
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kEditPlaylistSelectionMessage)) {
        const void* data = nullptr;
        uint32 size = 0;
        int64 insertionIndex = -1;
        auto* attributes = message->getAttributes();
        if (attributes->getBinary(kPlaylistSelectedIndicesAttribute, data, size) != kResultOk ||
            data == nullptr || size == 0 || size % sizeof(std::int32_t) != 0 ||
            size / sizeof(std::int32_t) > PlaylistModel::kMaximumEntries ||
            attributes->getInt(kPlaylistInsertionIndexAttribute, insertionIndex) != kResultOk ||
            insertionIndex < -1 || insertionIndex > static_cast<int64>(PlaylistModel::kMaximumEntries))
            return kInvalidArgument;
        std::vector<std::int32_t> indices(size / sizeof(std::int32_t));
        std::memcpy(indices.data(), data, size);
        if (insertionIndex < 0) {
            const auto result = playlist_.remove(indices);
            if (result.removedCurrent)
                audioEngine_.requestUnload();
            return result.removed ? kResultOk : kInvalidArgument;
        }
        return playlist_.move(indices, static_cast<std::int32_t>(insertionIndex))
            ? kResultOk : kInvalidArgument;
    }

    if (FIDStringsEqual(message->getMessageID(), kRemovePlaylistMessage)) {
        int64 index = -1;
        if (message->getAttributes()->getInt(
                kPlaylistRemoveIndexAttribute, index) != kResultOk ||
            index < 0 || index > std::numeric_limits<std::int32_t>::max())
            return kInvalidArgument;
        removePlaylist(static_cast<std::int32_t>(index));
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kMovePlaylistMessage)) {
        int64 sourceIndex = -1;
        int64 targetIndex = -1;
        if (message->getAttributes()->getInt(
                kPlaylistSourceIndexAttribute, sourceIndex) != kResultOk ||
            message->getAttributes()->getInt(
                kPlaylistTargetIndexAttribute, targetIndex) != kResultOk ||
            sourceIndex < 0 || targetIndex < 0 ||
            sourceIndex > std::numeric_limits<std::int32_t>::max() ||
            targetIndex > std::numeric_limits<std::int32_t>::max())
            return kInvalidArgument;
        movePlaylist(static_cast<std::int32_t>(sourceIndex),
                     static_cast<std::int32_t>(targetIndex));
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kSetVideoPlacementMessage)) {
        auto* attributes = message->getAttributes();
        int64 valid = 0;
        int64 x = 0;
        int64 y = 0;
        int64 width = 0;
        int64 height = 0;
        int64 userSized = 0;
        int64 userPosition = 0;
        if (attributes->getInt(kVideoPlacementValidAttribute, valid) != kResultOk ||
            attributes->getInt(kVideoPlacementXAttribute, x) != kResultOk ||
            attributes->getInt(kVideoPlacementYAttribute, y) != kResultOk ||
            attributes->getInt(kVideoPlacementWidthAttribute, width) != kResultOk ||
            attributes->getInt(kVideoPlacementHeightAttribute, height) != kResultOk ||
            attributes->getInt(kVideoPlacementUserSizedAttribute, userSized) != kResultOk ||
            attributes->getInt(kVideoPlacementUserPositionAttribute, userPosition) != kResultOk)
            return kInvalidArgument;

        VideoWindowPlacement placement {};
        placement.valid = valid != 0 && width > 0 && height > 0;
        placement.x = static_cast<int32>(x);
        placement.y = static_cast<int32>(y);
        placement.width = static_cast<int32>(width);
        placement.height = static_cast<int32>(height);
        placement.userSized = userSized != 0;
        placement.userPosition = userPosition != 0;
        {
            std::lock_guard lock(uiStateMutex_);
            videoPlacement_ = placement;
        }
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kSetEditorLayoutMessage)) {
        int64 visible = 0;
        int64 height = kDefaultPlaylistHeight;
        auto* attributes = message->getAttributes();
        if (attributes->getInt(kPlaylistVisibleAttribute, visible) != kResultOk ||
            attributes->getInt(kPlaylistHeightAttribute, height) != kResultOk)
            return kInvalidArgument;

        EditorLayoutState layout {};
        layout.playlistVisible = visible != 0;
        layout.playlistHeight = std::max(
            static_cast<int32>(height), kMinPlaylistHeight);
        {
            std::lock_guard lock(uiStateMutex_);
            editorLayout_ = layout;
        }
        return kResultOk;
    }

    if (FIDStringsEqual(message->getMessageID(), kPlayMessage)) {
        requestPlay();
        return kResultOk;
    }
    if (FIDStringsEqual(message->getMessageID(), kPauseMessage)) {
        requestPause();
        return kResultOk;
    }
    if (FIDStringsEqual(message->getMessageID(), kStopMessage)) {
        requestStop();
        return kResultOk;
    }
    if (FIDStringsEqual(message->getMessageID(), kSetVolumeMessage)) {
        double volume = 1.0;
        if (message->getAttributes()->getFloat(kVolumeAttribute, volume) != kResultOk)
            return kInvalidArgument;
        setVolume(static_cast<float>(volume));
        return kResultOk;
    }
    if (FIDStringsEqual(message->getMessageID(), kSeekMessage)) {
        double position = 0.0;
        if (message->getAttributes()->getFloat(kPositionAttribute, position) != kResultOk)
            return kInvalidArgument;
        requestSeek(static_cast<float>(position));
        return kResultOk;
    }
    if (FIDStringsEqual(message->getMessageID(), kStatusRequestMessage)) {
        const auto snapshot = audioEngine_.snapshot();
        auto playlist = playlist_.snapshot();

        // A natural end is reported separately from pause/stop. Advance only
        // when there is an actual following entry, and use the same open/play
        // path as a manual playlist selection.
        bool advancedToNext = false;
        if (snapshot.finished) {
            const auto nextFile = playlist_.advanceFrom(playlist.currentIndex);
            if (nextFile.has_value()) {
                playlist = playlist_.snapshot();
                audioEngine_.requestOpenFile(*nextFile);
                audioEngine_.requestPlay();
                advancedToNext = true;
            }
        }
        const auto currentFilePath = audioEngine_.currentFilePath();
        const auto reportedLoaded = !advancedToNext && snapshot.loaded &&
                                     playlist.currentIndex >= 0 &&
                                     playlist.currentIndex <
                                         static_cast<std::int32_t>(
                                             playlist.entries.size()) &&
                                     currentFilePath == playlist.entries[
                                         static_cast<std::size_t>(
                                             playlist.currentIndex)];
        auto* response = allocateMessage();
        if (response == nullptr)
            return kResultFalse;
        response->setMessageID(kStatusMessage);
        auto* attributes = response->getAttributes();
        VideoWindowPlacement videoPlacement {};
        EditorLayoutState editorLayout {};
        {
            std::lock_guard lock(uiStateMutex_);
            videoPlacement = videoPlacement_;
            editorLayout = editorLayout_;
        }
        const auto statusSequence = statusSequence_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (attributes->setInt(kStatusSequenceAttribute,
                               static_cast<int64>(statusSequence)) !=
            kResultOk) {
            response->release();
            return kResultFalse;
        }
        attributes->setInt(kPositionFramesAttribute,
                           static_cast<int64>(reportedLoaded
                                                  ? snapshot.positionFrames
                                                  : 0));
        attributes->setInt(kDurationFramesAttribute,
                           static_cast<int64>(reportedLoaded
                                                  ? snapshot.durationFrames
                                                  : 0));
        attributes->setInt(kPlayingAttribute,
                           reportedLoaded && snapshot.playing ? 1 : 0);
        attributes->setFloat(kVolumeAttribute, snapshot.volume);
        attributes->setInt(kSampleRateAttribute, snapshot.sampleRate);
        attributes->setInt(kLoadedAttribute, reportedLoaded ? 1 : 0);
        attributes->setInt(kPlaylistCountAttribute,
                           static_cast<int64>(playlist.entries.size()));
        attributes->setInt(kCurrentPlaylistIndexAttribute,
                           playlist.currentIndex);
        attributes->setInt(kVideoPlacementValidAttribute,
                           videoPlacement.valid ? 1 : 0);
        attributes->setInt(kVideoPlacementXAttribute, videoPlacement.x);
        attributes->setInt(kVideoPlacementYAttribute, videoPlacement.y);
        attributes->setInt(kVideoPlacementWidthAttribute, videoPlacement.width);
        attributes->setInt(kVideoPlacementHeightAttribute, videoPlacement.height);
        attributes->setInt(kVideoPlacementUserSizedAttribute,
                           videoPlacement.userSized ? 1 : 0);
        attributes->setInt(kVideoPlacementUserPositionAttribute,
                           videoPlacement.userPosition ? 1 : 0);
        attributes->setInt(kPlaylistVisibleAttribute,
                           editorLayout.playlistVisible ? 1 : 0);
        attributes->setInt(kPlaylistHeightAttribute,
                           editorLayout.playlistHeight);
        for (std::size_t index = 0; index < playlist.entries.size(); ++index) {
            const auto utf8Path = playlist.entries[index].u8string();
            const auto attributeName = playlistItemAttribute(index);
            const auto pathResult = attributes->setBinary(
                attributeName.c_str(),
                utf8Path.data(),
                static_cast<uint32>(utf8Path.size()));
            if (pathResult != kResultOk) {
                response->release();
                return pathResult;
            }
        }
        if (reportedLoaded) {
            const auto utf8Path = currentFilePath.u8string();
            const auto pathResult = attributes->setBinary(
                kCurrentFilePathAttribute,
                utf8Path.data(),
                static_cast<uint32>(utf8Path.size()));
            if (pathResult != kResultOk) {
                response->release();
                return pathResult;
            }
        }
        const auto result = sendMessage(response);
        response->release();
        return result;
    }

    return AudioEffect::notify(message);
}

void Processor::requestOpenFile(std::filesystem::path file)
{
    if (file.empty())
        return;

    if (!playlist_.addOrSelect(file))
        return;
    audioEngine_.requestOpenFile(std::move(file));
}

void Processor::requestPlay()
{
    const auto playlist = playlist_.snapshot();
    std::filesystem::path file;
    if (playlist.currentIndex >= 0 &&
        playlist.currentIndex <
            static_cast<std::int32_t>(playlist.entries.size()))
        file = playlist.entries[static_cast<std::size_t>(playlist.currentIndex)];

    const auto snapshot = audioEngine_.snapshot();
    if (!file.empty() &&
        ((!snapshot.loaded && playlist.pendingOpenPath != file) ||
         (snapshot.loaded && audioEngine_.currentFilePath() != file)))
        audioEngine_.requestOpenFile(file);
    audioEngine_.requestPlay();
}

void Processor::requestPause()
{
    audioEngine_.requestPause();
}

void Processor::requestStop()
{
    playlist_.clearPendingOpen();
    audioEngine_.requestStop();
}

void Processor::requestSeek(float normalizedPosition)
{
    audioEngine_.requestSeek(normalizedPosition);
}

void Processor::setVolume(float normalizedVolume) noexcept
{
    audioEngine_.setVolume(normalizedVolume);
}

void Processor::selectPlaylist(const std::filesystem::path& file, bool play)
{
    const auto selectedFile = playlist_.select(file);
    if (!selectedFile.has_value())
        return;

    audioEngine_.requestOpenFile(*selectedFile);
    if (play)
        audioEngine_.requestPlay();
}

void Processor::clearPlaylist()
{
    playlist_.clear();
    audioEngine_.requestUnload();
}

void Processor::removePlaylist(std::int32_t index)
{
    const auto result = playlist_.remove(index);
    if (result.removedCurrent)
        audioEngine_.requestUnload();
}

void Processor::movePlaylist(std::int32_t sourceIndex,
                             std::int32_t targetIndex)
{
    static_cast<void>(playlist_.move(sourceIndex, targetIndex));
}

void Processor::applyRestoredState()
{
    std::filesystem::path file;
    bool loaded = false;
    bool playing = false;
    float volume = 1.0f;
    std::uint64_t positionFrames = 0;
    std::uint64_t durationFrames = 0;
    const auto playlist = playlist_.snapshot();
    {
        std::lock_guard lock(restoreStateMutex_);
        if (!stateRestorePending_)
            return;
        loaded = restoreLoaded_ && playlist.currentIndex >= 0 &&
                 playlist.currentIndex <
                     static_cast<std::int32_t>(playlist.entries.size());
        if (loaded)
            file = playlist.entries[
                static_cast<std::size_t>(playlist.currentIndex)];
        playing = restorePlaying_ && loaded;
        volume = restoreVolume_;
        positionFrames = restorePositionFrames_;
        durationFrames = restoreDurationFrames_;
        stateRestorePending_ = false;
    }

    audioEngine_.setVolume(volume);
    audioEngine_.requestUnload();
    if (!loaded || file.empty())
        return;

    audioEngine_.requestOpenFile(file);
    if (durationFrames > 0 && positionFrames > 0) {
        const auto normalized = std::clamp(
            static_cast<double>(positionFrames) /
                static_cast<double>(durationFrames),
            0.0,
            1.0);
        audioEngine_.requestSeek(static_cast<float>(normalized));
    }
    if (playing)
        audioEngine_.requestPlay();
}

} // namespace MediaPlayer
