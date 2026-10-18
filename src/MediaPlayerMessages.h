#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace MediaPlayer {

inline constexpr char kOpenFileMessage[] = "MediaPlayer.OpenFile";
inline constexpr char kPlayMessage[] = "MediaPlayer.Play";
inline constexpr char kPauseMessage[] = "MediaPlayer.Pause";
inline constexpr char kStopMessage[] = "MediaPlayer.Stop";
inline constexpr char kSeekMessage[] = "MediaPlayer.Seek";
inline constexpr char kSetVolumeMessage[] = "MediaPlayer.SetVolume";
inline constexpr char kStatusRequestMessage[] = "MediaPlayer.StatusRequest";
inline constexpr char kStatusMessage[] = "MediaPlayer.Status";
inline constexpr char kStatusSequenceAttribute[] = "StatusSequence";
inline constexpr char kFilePathAttribute[] = "FilePathUtf8";
inline constexpr char kPositionAttribute[] = "Position";
inline constexpr char kVolumeAttribute[] = "Volume";
inline constexpr char kSampleRateAttribute[] = "SampleRate";
inline constexpr char kPositionFramesAttribute[] = "PositionFrames";
inline constexpr char kDurationFramesAttribute[] = "DurationFrames";
inline constexpr char kPlayingAttribute[] = "Playing";
inline constexpr char kLoadedAttribute[] = "Loaded";
inline constexpr char kCurrentFilePathAttribute[] = "CurrentFilePathUtf8";
inline constexpr char kSelectPlaylistMessage[] = "MediaPlayer.SelectPlaylist";
inline constexpr char kClearPlaylistMessage[] = "MediaPlayer.ClearPlaylist";
inline constexpr char kRemovePlaylistMessage[] = "MediaPlayer.RemovePlaylist";
inline constexpr char kMovePlaylistMessage[] = "MediaPlayer.MovePlaylist";
inline constexpr char kSetVideoPlacementMessage[] = "MediaPlayer.SetVideoPlacement";
inline constexpr char kSetEditorLayoutMessage[] = "MediaPlayer.SetEditorLayout";
inline constexpr char kPlaylistCountAttribute[] = "PlaylistCount";
inline constexpr char kCurrentPlaylistIndexAttribute[] = "CurrentPlaylistIndex";
inline constexpr char kPlaylistPathAttribute[] = "PlaylistPathUtf8";
inline constexpr char kPlaylistPlayAttribute[] = "PlaylistPlay";
inline constexpr char kPlaylistRemoveIndexAttribute[] = "PlaylistRemoveIndex";
inline constexpr char kPlaylistSourceIndexAttribute[] = "PlaylistSourceIndex";
inline constexpr char kPlaylistTargetIndexAttribute[] = "PlaylistTargetIndex";
inline constexpr char kPlaylistItemAttributePrefix[] = "PlaylistItemUtf8.";
inline constexpr char kVideoPlacementValidAttribute[] = "VideoPlacementValid";
inline constexpr char kVideoPlacementXAttribute[] = "VideoPlacementX";
inline constexpr char kVideoPlacementYAttribute[] = "VideoPlacementY";
inline constexpr char kVideoPlacementWidthAttribute[] = "VideoPlacementWidth";
inline constexpr char kVideoPlacementHeightAttribute[] = "VideoPlacementHeight";
inline constexpr char kVideoPlacementUserSizedAttribute[] = "VideoPlacementUserSized";
inline constexpr char kVideoPlacementUserPositionAttribute[] = "VideoPlacementUserPosition";
inline constexpr char kPlaylistVisibleAttribute[] = "PlaylistVisible";
inline constexpr char kPlaylistHeightAttribute[] = "PlaylistHeight";

struct VideoWindowPlacement final {
    std::int32_t x {0};
    std::int32_t y {0};
    std::int32_t width {0};
    std::int32_t height {0};
    bool valid {false};
    bool userSized {false};
    bool userPosition {false};
};

struct EditorLayoutState final {
    bool playlistVisible {false};
    std::int32_t playlistHeight {60};
};

inline std::string playlistItemAttribute(std::size_t index)
{
    return std::string(kPlaylistItemAttributePrefix) + std::to_string(index);
}

} // namespace MediaPlayer
