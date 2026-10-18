#pragma once

namespace MediaPlayer::EditorLayout {

inline constexpr int kEditorWidth = 300;
inline constexpr int kBaseEditorHeight = 160;
inline constexpr int kPlaylistPanelTop = kBaseEditorHeight;
inline constexpr int kPlaylistHeaderTop = 138;
inline constexpr int kPlaylistBottomMargin = 10;
inline constexpr int kMinPlaylistHeight = 60;
inline constexpr int kPlaylistItemHeight = 24;
inline constexpr int kPlaylistResizeGripHeight = 6;

struct ControlBounds final {
    int x {0};
    int y {0};
    int width {0};
    int height {0};
};

struct ControlLayout final {
    int contentWidth {0};
    int playlistHeight {0};
    ControlBounds playerPanel;
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
};

[[nodiscard]] ControlLayout calculate(int rootWidth, int playlistHeight);

} // namespace MediaPlayer::EditorLayout
