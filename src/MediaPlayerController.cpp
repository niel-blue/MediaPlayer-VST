#include "MediaPlayerController.h"
#include "base/source/fstreamer.h"

#include "MediaPlayerEditor.h"
#include "MediaPlayerEditorLayout.h"
#include "MediaPlayerMessages.h"
#include "MediaPlayerPlaylist.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace MediaPlayer {

using namespace Steinberg;
using namespace Steinberg::Vst;

tresult PLUGIN_API Controller::getState(IBStream* state)
{
    if (!state) return kInvalidArgument;
    IBStreamer stream(state, kLittleEndian);
    return stream.writeInt32(0x54484D31) && stream.writeInt32(lightTheme_ ? 1 : 0)
        ? kResultOk : kResultFalse;
}
tresult PLUGIN_API Controller::setState(IBStream* state)
{
    if (!state) return kInvalidArgument;
    IBStreamer stream(state, kLittleEndian);
    int32 magic = 0, light = 0;
    if (!stream.readInt32(magic) || magic != 0x54484D31 ||
        !stream.readInt32(light) || (light != 0 && light != 1)) return kResultFalse;
    lightTheme_ = light != 0;
    if (editor_) editor_->setLightTheme(lightTheme_, false);
    return kResultOk;
}
tresult PLUGIN_API Controller::initialize(FUnknown* context)
{
    lastStatusSequence_ = -1;
    hasAppliedEditorState_ = false;
    // Parameters and the editor are introduced with the transport GUI. The
    // controller is present already so the processor/controller split is
    // stable from the first VST3 package.
    return EditController::initialize(context);
}

IPlugView* PLUGIN_API Controller::createView(const char* name)
{
    if (name != nullptr && FIDStringsEqual(name, ViewType::kEditor)) {
        ViewRect size(0, 0, EditorLayout::kEditorWidth,
                      EditorLayout::kBaseEditorHeight);
        return new NativeEditor(this, &size);
    }

    return nullptr;
}

tresult Controller::sendOpenFile(const std::filesystem::path& file)
{
    const auto utf8Path = file.u8string();
    if (utf8Path.empty() ||
        utf8Path.size() > std::numeric_limits<std::uint32_t>::max())
        return kInvalidArgument;

    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kOpenFileMessage);
    const auto result = message->getAttributes()->setBinary(
        kFilePathAttribute,
        utf8Path.data(),
        static_cast<uint32>(utf8Path.size()));

    const auto sendResult = result == kResultOk ? sendMessage(message) : result;
    message->release();
    return sendResult;
}

tresult Controller::sendSelectPlaylist(const std::filesystem::path& file,
                                       bool play)
{
    const auto utf8Path = file.u8string();
    if (utf8Path.empty() ||
        utf8Path.size() > std::numeric_limits<std::uint32_t>::max())
        return kInvalidArgument;

    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kSelectPlaylistMessage);
    auto* attributes = message->getAttributes();
    const auto pathResult = attributes->setBinary(
        kPlaylistPathAttribute,
        utf8Path.data(),
        static_cast<uint32>(utf8Path.size()));
    const auto playResult = attributes->setInt(kPlaylistPlayAttribute, play ? 1 : 0);
    const auto result = pathResult == kResultOk && playResult == kResultOk
        ? sendMessage(message) : kResultFalse;
    message->release();
    return result;
}

tresult Controller::sendClearPlaylist()
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kClearPlaylistMessage);
    const auto result = sendMessage(message);
    message->release();
    return result;
}

tresult Controller::sendRemovePlaylist(std::int32_t index)
{
    if (index < 0)
        return kInvalidArgument;

    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kRemovePlaylistMessage);
    const auto attributeResult = message->getAttributes()->setInt(
        kPlaylistRemoveIndexAttribute, index);
    const auto result = attributeResult == kResultOk
        ? sendMessage(message)
        : attributeResult;
    message->release();
    return result;
}

tresult Controller::sendPlaylistSelectionEdit(
    const std::vector<std::int32_t>& indices, std::int32_t insertionIndex)
{
    if (indices.empty() || indices.size() > PlaylistModel::kMaximumEntries ||
        insertionIndex < -1)
        return kInvalidArgument;
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;
    message->setMessageID(kEditPlaylistSelectionMessage);
    auto* attributes = message->getAttributes();
    const auto indicesResult = attributes->setBinary(
        kPlaylistSelectedIndicesAttribute, indices.data(),
        static_cast<uint32>(indices.size() * sizeof(std::int32_t)));
    const auto insertionResult = attributes->setInt(
        kPlaylistInsertionIndexAttribute, insertionIndex);
    const auto result = indicesResult == kResultOk && insertionResult == kResultOk
        ? sendMessage(message) : kResultFalse;
    message->release();
    return result;
}

tresult Controller::sendMovePlaylist(std::int32_t sourceIndex,
                                     std::int32_t targetIndex)
{
    if (sourceIndex < 0 || targetIndex < 0)
        return kInvalidArgument;

    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kMovePlaylistMessage);
    auto* attributes = message->getAttributes();
    const auto sourceResult = attributes->setInt(
        kPlaylistSourceIndexAttribute, sourceIndex);
    const auto targetResult = attributes->setInt(
        kPlaylistTargetIndexAttribute, targetIndex);
    const auto result = sourceResult == kResultOk && targetResult == kResultOk
        ? sendMessage(message)
        : kResultFalse;
    message->release();
    return result;
}

tresult Controller::sendTransportMessage(const char* messageId)
{
    if (messageId == nullptr)
        return kInvalidArgument;

    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(messageId);
    const auto result = sendMessage(message);
    message->release();
    return result;
}

tresult Controller::sendSeek(float normalizedPosition)
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kSeekMessage);
    const auto attributeResult = message->getAttributes()->setFloat(
        kPositionAttribute, normalizedPosition);
    const auto result = attributeResult == kResultOk
        ? sendMessage(message)
        : attributeResult;
    message->release();
    return result;
}

tresult Controller::sendVolume(float normalizedVolume)
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kSetVolumeMessage);
    const auto attributeResult = message->getAttributes()->setFloat(
        kVolumeAttribute, normalizedVolume);
    const auto result = attributeResult == kResultOk
        ? sendMessage(message)
        : attributeResult;
    message->release();
    return result;
}

tresult Controller::sendVideoPlacement(const VideoWindowPlacement& placement)
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kSetVideoPlacementMessage);
    auto* attributes = message->getAttributes();
    const auto result =
        attributes->setInt(kVideoPlacementValidAttribute, placement.valid ? 1 : 0) == kResultOk &&
        attributes->setInt(kVideoPlacementXAttribute, placement.x) == kResultOk &&
        attributes->setInt(kVideoPlacementYAttribute, placement.y) == kResultOk &&
        attributes->setInt(kVideoPlacementWidthAttribute, placement.width) == kResultOk &&
        attributes->setInt(kVideoPlacementHeightAttribute, placement.height) == kResultOk &&
        attributes->setInt(kVideoPlacementUserSizedAttribute,
                           placement.userSized ? 1 : 0) == kResultOk &&
        attributes->setInt(kVideoPlacementUserPositionAttribute,
                           placement.userPosition ? 1 : 0) == kResultOk
        ? sendMessage(message)
        : kResultFalse;
    message->release();
    return result;
}

tresult Controller::sendEditorLayout(const EditorLayoutState& layout)
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kSetEditorLayoutMessage);
    auto* attributes = message->getAttributes();
    if (attributes->setInt(kPlaylistVisibleAttribute,
                           layout.playlistVisible ? 1 : 0) != kResultOk ||
        attributes->setInt(kPlaylistHeightAttribute, layout.playlistHeight) !=
            kResultOk) {
        message->release();
        return kResultFalse;
    }

    // The processor can still return one or more status snapshots created
    // before this layout message was handled. Keep the local user choice
    // pending so an older snapshot cannot immediately restore the playlist.
    pendingEditorLayout_ = layout;
    hasPendingEditorLayout_ = true;
    const auto result = sendMessage(message);
    if (result != kResultOk)
        hasPendingEditorLayout_ = false;
    message->release();
    return result;
}

tresult Controller::requestPlaybackStatus()
{
    auto* message = allocateMessage();
    if (message == nullptr)
        return kResultFalse;

    message->setMessageID(kStatusRequestMessage);
    const auto result = sendMessage(message);
    message->release();
    return result;
}

tresult PLUGIN_API Controller::notify(IMessage* message)
{
    if (message != nullptr &&
        FIDStringsEqual(message->getMessageID(), kStatusMessage)) {
        int64 position = 0;
        int64 duration = 0;
        int64 playing = 0;
        int64 sampleRate = 48'000;
        int64 loaded = 0;
        int64 playlistCount = 0;
        int64 currentPlaylistIndex = -1;
        int64 videoPlacementValid = 0;
        int64 videoPlacementX = 0;
        int64 videoPlacementY = 0;
        int64 videoPlacementWidth = 0;
        int64 videoPlacementHeight = 0;
        int64 videoPlacementUserSized = 0;
        int64 videoPlacementUserPosition = 0;
        int64 playlistVisible = 0;
        int64 playlistHeight = EditorLayout::kDefaultPlaylistHeight;
        int64 statusSequence = -1;
        double volume = 1.0;
        auto* attributes = message->getAttributes();
        if (attributes->getInt(kPositionFramesAttribute, position) != kResultOk ||
            attributes->getInt(kDurationFramesAttribute, duration) != kResultOk ||
            attributes->getInt(kPlayingAttribute, playing) != kResultOk ||
            attributes->getInt(kSampleRateAttribute, sampleRate) != kResultOk ||
            attributes->getInt(kLoadedAttribute, loaded) != kResultOk ||
            attributes->getInt(kPlaylistCountAttribute, playlistCount) != kResultOk ||
            attributes->getInt(kCurrentPlaylistIndexAttribute,
                               currentPlaylistIndex) != kResultOk ||
            attributes->getInt(kVideoPlacementValidAttribute,
                               videoPlacementValid) != kResultOk ||
            attributes->getInt(kVideoPlacementXAttribute,
                               videoPlacementX) != kResultOk ||
            attributes->getInt(kVideoPlacementYAttribute,
                               videoPlacementY) != kResultOk ||
            attributes->getInt(kVideoPlacementWidthAttribute,
                               videoPlacementWidth) != kResultOk ||
            attributes->getInt(kVideoPlacementHeightAttribute,
                               videoPlacementHeight) != kResultOk ||
            attributes->getInt(kVideoPlacementUserSizedAttribute,
                               videoPlacementUserSized) != kResultOk ||
            attributes->getInt(kVideoPlacementUserPositionAttribute,
                               videoPlacementUserPosition) != kResultOk ||
            attributes->getInt(kPlaylistVisibleAttribute,
                               playlistVisible) != kResultOk ||
            attributes->getInt(kPlaylistHeightAttribute,
                               playlistHeight) != kResultOk ||
            attributes->getInt(kStatusSequenceAttribute, statusSequence) !=
                kResultOk ||
            attributes->getFloat(kVolumeAttribute, volume) != kResultOk)
            return kInvalidArgument;

        if (statusSequence < 0 || statusSequence <= lastStatusSequence_)
            return kResultOk;
        lastStatusSequence_ = statusSequence;

        if (playlistCount < 0 ||
            playlistCount > static_cast<int64>(PlaylistModel::kMaximumEntries))
            return kInvalidArgument;

        std::vector<std::filesystem::path> playlist;
        playlist.reserve(static_cast<std::size_t>(playlistCount));
        for (int64 index = 0; index < playlistCount; ++index) {
            const auto attributeName = playlistItemAttribute(
                static_cast<std::size_t>(index));
            const void* pathData = nullptr;
            uint32 pathSize = 0;
            if (attributes->getBinary(attributeName.c_str(), pathData, pathSize) !=
                    kResultOk ||
                pathData == nullptr || pathSize == 0)
                return kInvalidArgument;

            try {
                const auto* bytes = static_cast<const char*>(pathData);
                playlist.emplace_back(std::filesystem::u8path(bytes,
                                                              bytes + pathSize));
            } catch (...) {
                return kResultFalse;
            }
        }

        if (currentPlaylistIndex < -1 ||
            currentPlaylistIndex >= static_cast<int64>(playlist.size()))
            currentPlaylistIndex = -1;

        if (editor_ != nullptr) {
            VideoWindowPlacement placement {};
            placement.valid = videoPlacementValid != 0 &&
                              videoPlacementWidth > 0 &&
                              videoPlacementHeight > 0;
            placement.x = static_cast<std::int32_t>(videoPlacementX);
            placement.y = static_cast<std::int32_t>(videoPlacementY);
            placement.width = static_cast<std::int32_t>(videoPlacementWidth);
            placement.height = static_cast<std::int32_t>(videoPlacementHeight);
            placement.userSized = videoPlacementUserSized != 0;
            placement.userPosition = videoPlacementUserPosition != 0;

            EditorLayoutState layout {};
            layout.playlistVisible = playlistVisible != 0;
            layout.playlistHeight = static_cast<std::int32_t>(playlistHeight);
            const auto samePlacement = [](const VideoWindowPlacement& left,
                                           const VideoWindowPlacement& right) {
                return left.x == right.x && left.y == right.y &&
                       left.width == right.width && left.height == right.height &&
                       left.valid == right.valid &&
                       left.userSized == right.userSized &&
                       left.userPosition == right.userPosition;
            };
            const auto sameLayout = [](const EditorLayoutState& left,
                                       const EditorLayoutState& right) {
                return left.playlistVisible == right.playlistVisible &&
                       left.playlistHeight == right.playlistHeight;
            };

            auto applyLayout = true;
            if (hasPendingEditorLayout_) {
                if (sameLayout(layout, pendingEditorLayout_))
                    hasPendingEditorLayout_ = false;
                else
                    applyLayout = false;
            }

            // Status is polled periodically for transport updates. The
            // editor layout is restored from the first snapshot only; after
            // that, the editor owns its visibility and height because those
            // values are changed directly by the GUI. Reapplying a later
            // snapshot can undo a just-completed hide operation.
            if (!hasAppliedEditorState_ ||
                !samePlacement(placement, lastAppliedVideoPlacement_))
                editor_->applyVideoPlacement(placement);
            if (applyLayout && !hasAppliedEditorState_)
                editor_->applyEditorLayout(layout);
            lastAppliedVideoPlacement_ = placement;
            if (applyLayout)
                lastAppliedEditorLayout_ = layout;
            hasAppliedEditorState_ = true;

            if (!editor_->updatePlaylist(
                    playlist,
                    static_cast<std::int32_t>(currentPlaylistIndex)))
                return kResultOk;
            if (loaded != 0) {
                const void* pathData = nullptr;
                uint32 pathSize = 0;
                if (attributes->getBinary(kCurrentFilePathAttribute,
                                          pathData,
                                          pathSize) != kResultOk ||
                    pathData == nullptr || pathSize == 0)
                    return kInvalidArgument;

                try {
                    const auto* bytes = static_cast<const char*>(pathData);
                    editor_->updateMediaPath(
                        std::filesystem::u8path(bytes, bytes + pathSize));
                } catch (...) {
                    return kResultFalse;
                }
            } else {
                editor_->clearMedia();
            }
            editor_->updatePlaybackStatus(
                position < 0 ? 0 : static_cast<std::uint64_t>(position),
                duration < 0 ? 0 : static_cast<std::uint64_t>(duration),
                sampleRate < 1 ? 48'000 : static_cast<int>(sampleRate),
                playing != 0,
                loaded != 0,
                static_cast<float>(volume));
        }
        return kResultOk;
    }

    return EditController::notify(message);
}

} // namespace MediaPlayer
