#pragma once

namespace MediaPlayer::EditorLayout {

inline constexpr int kEditorWidth = 300;
inline constexpr int kBaseEditorHeight = 160;
inline constexpr int kPlaylistPanelTop = kBaseEditorHeight;
inline constexpr int kPlaylistHeaderTop = 138;
inline constexpr int kPlaylistBottomMargin = 10;
inline constexpr int kMinPlaylistHeight = 60;
inline constexpr int kDefaultPlaylistHeight = kMinPlaylistHeight;
inline constexpr int kPlaylistItemHeight = 24;
inline constexpr int kPlaylistResizeGripHeight = 6;

struct ControlBounds final {
    int x {0};
    int y {0};
    int width {0};
    int height {0};
};

// Fixed geometry is kept here. calculate() only applies the host width and
// playlist height, so controls and owner-drawn text use the same dimensions.
inline constexpr int kContentMargin = 12;
namespace Player {
inline constexpr int kThemeWidth = 40;
inline constexpr int kThemeHeight = 16;
inline constexpr int kThemeTop = 8;
inline constexpr int kBrandBandHeight = 28;
inline constexpr int kBrandTop = 8;
inline constexpr ControlBounds kTitle {kContentMargin, 35, 0, 18};
inline constexpr ControlBounds kStatus {kContentMargin, 55, 0, 13};
inline constexpr int kTimeLabelWidth = 34;
inline constexpr int kTimeLabelTop = 68;
inline constexpr int kTimeLabelHeight = 16;
inline constexpr int kSeekGap = 4;
inline constexpr int kSeekTop = 72;
inline constexpr int kSeekHeight = 8;
inline constexpr int kMinimumSeekWidth = 40;
inline constexpr int kTransportReferenceWidth = 400;
inline constexpr ControlBounds kOpen {kContentMargin, 87, 42, 46};
inline constexpr ControlBounds kPrevious {124, 93, 26, 28};
inline constexpr ControlBounds kStop {155, 93, 26, 28};
inline constexpr ControlBounds kPlay {182, 89, 36, 36};
inline constexpr ControlBounds kNext {225, 93, 26, 28};
inline constexpr int kVolumeTop = 82;
inline constexpr int kVolumeWidth = 52;
inline constexpr int kVolumeHeight = 56;
inline constexpr int kVolumeLabelAreaHeight = 16;
inline constexpr int kVolumeLabelBottomInset = 2;
inline constexpr int kOpenLabelAreaHeight = 19;
inline constexpr int kOpenLabelBottomInset = 2;
} // namespace Player

namespace Playlist {
inline constexpr int kButtonWidth = 44;
inline constexpr int kButtonGap = 6;
inline constexpr int kHeaderInset = 2;
inline constexpr int kHeaderHeight = 16;
inline constexpr int kFrameWidth = 1;
inline constexpr int kIndexLeft = 10;
inline constexpr int kIndexWidth = 22;
inline constexpr int kNoteLeft = 33;
inline constexpr int kNoteWidth = 16;
inline constexpr int kTitleLeft = 54;
inline constexpr int kTitleRightInset = 45;
inline constexpr int kBadgeWidth = 35;
inline constexpr int kBadgeRightInset = 5;
inline constexpr int kBadgeVerticalInset = 4;
inline constexpr int kGripWidth = 36;
inline constexpr int kGripHeight = 2;
inline constexpr int kGripBottomInset = 4;
} // namespace Playlist

struct ControlLayout final {
    int contentWidth {0};
    int playlistHeight {0};
    ControlBounds playerPanel;
    ControlBounds brandBand;
    ControlBounds title;
    ControlBounds status;
    ControlBounds seekBar;
    ControlBounds currentTime;
    ControlBounds duration;
    ControlBounds openButton;
    ControlBounds previousButton;
    ControlBounds playButton;
    ControlBounds stopButton;
    ControlBounds nextButton;
    ControlBounds volumeBar;
    ControlBounds playlistToggle;
    ControlBounds playlistClear;
    ControlBounds playlistList;
    ControlBounds playlistFrame;
    ControlBounds playlistCount;
};

struct PlaylistRowLayout final {
    ControlBounds index;
    ControlBounds note;
    ControlBounds title;
    ControlBounds badge;
};

[[nodiscard]] ControlLayout calculate(int rootWidth, int playlistHeight);
[[nodiscard]] PlaylistRowLayout calculatePlaylistRow(int width, int height);
[[nodiscard]] ControlBounds calculateResizeGrip(int rootWidth, int rootHeight);

} // namespace MediaPlayer::EditorLayout
