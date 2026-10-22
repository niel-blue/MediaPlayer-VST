#pragma once

#include "vst_base_palette.h"

#include <array>
#include <cstdint>

// MediaPlayer-specific appearance. Base colors and knob geometry remain in
// vst_base_palette.h / vst_base_controls.h; do not duplicate their RGB values.
namespace MediaPlayer::EditorStyle {

namespace Colors {
constexpr std::uint32_t native(vst_base::Color color) noexcept
{
    // Win32 COLORREF uses 0x00BBGGRR.
    return color.r | (std::uint32_t{color.g} << 8) | (std::uint32_t{color.b} << 16);
}
inline constexpr auto& kPalette = vst_base::kDark;
inline constexpr auto kBackground = native(kPalette.background);
inline constexpr auto kPlaylistBackground = native(kPalette.surface);
inline constexpr auto kPanel = native(kPalette.selection);
inline constexpr auto kPanelBorder = native(kPalette.border);
inline constexpr auto kPlaylistSeparator = native(kPalette.separator);
inline constexpr auto kSeekTrack = native(kPalette.track);
inline constexpr auto kText = native(kPalette.text);
inline constexpr auto kMutedText = native(kPalette.mutedText);
inline constexpr auto kAccent = native(kPalette.action);
inline constexpr auto kAccentBright = native(kPalette.accent);
inline constexpr auto kPlaylistScrollThumb = native(kPalette.buttonBorder);
} // namespace Colors

// Per-editor colors: changing one instance must not recolor another instance.
struct Theme final {
    bool light {false};
    const vst_base::Palette& palette() const noexcept { return light ? vst_base::kLight : vst_base::kDark; }
    std::uint32_t kBackground, kPlaylistBackground, kPanel, kPanelBorder;
    std::uint32_t kPlaylistSeparator, kSeekTrack, kText, kMutedText;
    std::uint32_t kAccent, kAccentBright, kPlaylistScrollThumb, kAccentText;
    explicit Theme(bool isLight = false) { setLight(isLight); }
    void setLight(bool isLight) noexcept {
        light = isLight;
        const auto& p = palette();
        kBackground = Colors::native(p.background); kPlaylistBackground = Colors::native(p.surface);
        kPanel = Colors::native(p.selection); kPanelBorder = Colors::native(p.border);
        kPlaylistSeparator = Colors::native(p.separator); kSeekTrack = Colors::native(p.track);
        kText = Colors::native(p.text); kMutedText = Colors::native(p.mutedText);
        kAccent = Colors::native(p.action); kAccentBright = Colors::native(p.accent);
        kPlaylistScrollThumb = Colors::native(p.buttonBorder); kAccentText = Colors::native(p.accentText);
    }
};

struct Font final {
    int pixels;
    int weight;
    const wchar_t* family;
};
namespace Fonts {
inline constexpr wchar_t kTextFamily[] = L"Segoe UI";
inline constexpr wchar_t kSymbolFamily[] = L"Segoe UI Symbol";
inline constexpr int kRegular = 400;
inline constexpr int kSemibold = 600;
inline constexpr Font kBrand {18, kSemibold, kTextFamily};
inline constexpr Font kTitle {12, kRegular, kTextFamily};
inline constexpr Font kStatus {11, kRegular, kTextFamily};
inline constexpr Font kCaption {10, kRegular, kTextFamily};
inline constexpr Font kPlaylistRow {12, kRegular, kTextFamily};
inline constexpr Font kPlaylistNote {12, kRegular, kSymbolFamily};
inline constexpr Font kPlaylistBadge {9, kRegular, kTextFamily};
inline constexpr auto kOpenCaption = kStatus;
inline constexpr auto kButton = kCaption;
inline constexpr auto kTrackCount = kCaption;
inline constexpr auto kVolume = kCaption;
} // namespace Fonts

inline constexpr int kBorderWidth = 1;
inline constexpr int kButtonCornerDiameter = 6;

namespace Seek {
inline constexpr int kEdgeInset = 3;
inline constexpr float kLineWidth = 2.0f;
inline constexpr float kHandleDiameter = 5.0f;
inline constexpr float kHandleRadius = kHandleDiameter / 2;
} // namespace Seek

enum class TransportIcon { play, stop, previous, next };
namespace Transport {
inline constexpr float kPlayFaceInset = 3.0f;
inline constexpr float kPressedFaceDiameter = 24.0f;
inline constexpr float kPauseLeft = -5.0f;
inline constexpr float kPauseRight = 2.0f;
inline constexpr float kPauseHalfHeight = 6.0f;
inline constexpr float kPauseWidth = 3.0f;
inline constexpr float kPlayLeft = -3.5f;
inline constexpr float kPlayTip = 6.0f;
inline constexpr float kPlayHalfHeight = 6.0f;
inline constexpr float kStopSize = 9.0f;
inline constexpr float kSkipBase = 4.0f;
inline constexpr float kSkipTip = 3.0f;
inline constexpr float kSkipHalfHeight = 5.0f;
inline constexpr float kPreviousBar = -7.0f;
inline constexpr float kNextBar = 5.0f;
inline constexpr float kSkipBarWidth = 2.0f;
} // namespace Transport

namespace Folder {
struct Point final { float x; float y; };
inline constexpr float kTop = 9.0f;
inline constexpr float kStrokeWidth = 1.3f;
inline constexpr std::array<Point, 7> kOutline {{
    {-8, 0}, {-3, 0}, {-1, 2}, {8, 2}, {8, 12}, {-8, 12}, {-8, 0}}};
} // namespace Folder

namespace Playlist {
inline constexpr int kSelectionMarkWidth = 3;
inline constexpr int kDropLineWidth = 2;
inline constexpr int kDropLineInset = 3;
} // namespace Playlist

namespace Scrollbar {
inline constexpr int kHorizontalInset = 4;
inline constexpr int kVerticalInset = 3;
inline constexpr int kMinimumThumbHeight = 16;
inline constexpr int kThumbCornerDiameter = 8;
} // namespace Scrollbar

// Interaction settings are separate from drawing dimensions.
namespace Behavior {
inline constexpr unsigned int kStatusIntervalMs = 100;
inline constexpr unsigned int kDragScrollIntervalMs = 120;
inline constexpr int kDragScrollEdge = 16;
inline constexpr int kSeekSteps = 1000;
inline constexpr float kVolumePerDragPixel = 0.005f;
inline constexpr float kVolumePerWheelStep = 0.05f;
} // namespace Behavior

} // namespace MediaPlayer::EditorStyle
