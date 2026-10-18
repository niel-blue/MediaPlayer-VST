#include "MediaPlayerEditorLayout.h"

#include <algorithm>

namespace MediaPlayer::EditorLayout {
namespace {

constexpr int kPlaylistButtonWidth = 44;
constexpr int kPlaylistButtonGap = 6;
constexpr int kPlaylistHeaderHeight = 16;
constexpr int kMargin = 12;
constexpr int kTimeLabelWidth = 34;
constexpr int kTimeLabelY = 68;
constexpr int kSeekBarY = 72;
constexpr int kSeekBarHeight = 8;

} // namespace

ControlLayout calculate(int rootWidth, int playlistHeight)
{
    ControlLayout layout {};
    const auto safeRootWidth = std::max(1, rootWidth);
    layout.contentWidth = std::max(1, safeRootWidth - kMargin * 2);
    layout.playlistHeight = std::max(playlistHeight, kMinPlaylistHeight);

    const auto width = layout.contentWidth;
    const auto transportOffset = (safeRootWidth - 400) / 2;
    layout.playerPanel = {0, 0, safeRootWidth, kBaseEditorHeight};
    layout.title = {kMargin, 35, width, 18};
    layout.status = {kMargin, 55, width, 13};
    layout.seekBar = {kMargin + kTimeLabelWidth + 4,
                      kSeekBarY,
                      std::max(40, width - kTimeLabelWidth * 2 - 8),
                      kSeekBarHeight};
    layout.currentTime = {kMargin, kTimeLabelY, kTimeLabelWidth, 16};
    layout.duration = {kMargin + width - kTimeLabelWidth,
                       kTimeLabelY,
                       kTimeLabelWidth,
                       16};

    layout.openButton = {kMargin, 87, 42, 46};
    layout.previousButton = {124 + transportOffset, 93, 26, 28};
    layout.stopButton = {155 + transportOffset, 93, 26, 28};
    layout.playButton = {182 + transportOffset, 89, 36, 36};
    layout.nextButton = {225 + transportOffset, 93, 26, 28};
    layout.volumeBar = {kMargin + width - 52, 84, 52, 52};

    layout.playlistToggle = {kMargin,
                             kPlaylistHeaderTop + 2,
                             kPlaylistButtonWidth,
                             kPlaylistHeaderHeight};
    layout.playlistClear = {kMargin + kPlaylistButtonWidth + kPlaylistButtonGap,
                            kPlaylistHeaderTop + 2,
                            kPlaylistButtonWidth,
                            kPlaylistHeaderHeight};
    layout.playlistList = {kMargin,
                           kPlaylistPanelTop,
                           width,
                           layout.playlistHeight};
    return layout;
}

} // namespace MediaPlayer::EditorLayout
