#include "MediaPlayerEditorLayout.h"

#include <algorithm>

namespace MediaPlayer::EditorLayout {
ControlLayout calculate(int rootWidth, int playlistHeight)
{
    ControlLayout layout {};
    const auto safeRootWidth = std::max(1, rootWidth);
    layout.contentWidth = std::max(1, safeRootWidth - kContentMargin * 2);
    layout.playlistHeight = std::max(playlistHeight, kMinPlaylistHeight);

    const auto width = layout.contentWidth;
    const auto transportOffset = (safeRootWidth - Player::kTransportReferenceWidth) / 2;
    layout.playerPanel = {0, 0, safeRootWidth, kBaseEditorHeight};
    layout.brandBand = {0, 0, safeRootWidth, Player::kBrandBandHeight};
    layout.title = Player::kTitle;
    layout.title.width = width;
    layout.status = Player::kStatus;
    layout.status.width = width;
    layout.seekBar = {kContentMargin + Player::kTimeLabelWidth + Player::kSeekGap,
                      Player::kSeekTop,
                      std::max(Player::kMinimumSeekWidth,
                          width - Player::kTimeLabelWidth * 2 - Player::kSeekGap * 2),
                      Player::kSeekHeight};
    layout.currentTime = {kContentMargin, Player::kTimeLabelTop,
                          Player::kTimeLabelWidth, Player::kTimeLabelHeight};
    layout.duration = {kContentMargin + width - Player::kTimeLabelWidth,
                       Player::kTimeLabelTop, Player::kTimeLabelWidth, Player::kTimeLabelHeight};

    layout.openButton = Player::kOpen;
    const auto transport = [transportOffset](ControlBounds bounds) {
        bounds.x += transportOffset;
        return bounds;
    };
    layout.previousButton = transport(Player::kPrevious);
    layout.stopButton = transport(Player::kStop);
    layout.playButton = transport(Player::kPlay);
    layout.nextButton = transport(Player::kNext);
    layout.volumeBar = {kContentMargin + width - Player::kVolumeWidth,
                        Player::kVolumeTop, Player::kVolumeWidth, Player::kVolumeHeight};

    layout.playlistToggle = {kContentMargin,
                             kPlaylistHeaderTop + Playlist::kHeaderInset,
                             Playlist::kButtonWidth,
                             Playlist::kHeaderHeight};
    layout.playlistClear = {kContentMargin + Playlist::kButtonWidth + Playlist::kButtonGap,
                            kPlaylistHeaderTop + Playlist::kHeaderInset,
                            Playlist::kButtonWidth,
                            Playlist::kHeaderHeight};
    const auto countLeft = layout.playlistClear.x + layout.playlistClear.width + Playlist::kButtonGap;
    layout.playlistCount = {countLeft, layout.playlistToggle.y,
                            safeRootWidth - kContentMargin - countLeft, Playlist::kHeaderHeight};
    layout.playlistList = {kContentMargin,
                           kPlaylistPanelTop,
                           width,
                           layout.playlistHeight};
    layout.playlistFrame = {layout.playlistList.x - Playlist::kFrameWidth,
                            layout.playlistList.y - Playlist::kFrameWidth,
                            width + Playlist::kFrameWidth * 2,
                            layout.playlistHeight + Playlist::kFrameWidth * 2};
    return layout;
}

PlaylistRowLayout calculatePlaylistRow(int width, int height)
{
    return {{Playlist::kIndexLeft, 0, Playlist::kIndexWidth, height},
            {Playlist::kNoteLeft, 0, Playlist::kNoteWidth, height},
            {Playlist::kTitleLeft, 0, width - Playlist::kTitleRightInset - Playlist::kTitleLeft, height},
            {width - Playlist::kBadgeRightInset - Playlist::kBadgeWidth,
             Playlist::kBadgeVerticalInset, Playlist::kBadgeWidth,
             height - Playlist::kBadgeVerticalInset * 2}};
}

ControlBounds calculateResizeGrip(int rootWidth, int rootHeight)
{
    return {rootWidth / 2 - Playlist::kGripWidth / 2,
            rootHeight - Playlist::kGripBottomInset - Playlist::kGripHeight,
            Playlist::kGripWidth, Playlist::kGripHeight};
}

} // namespace MediaPlayer::EditorLayout
