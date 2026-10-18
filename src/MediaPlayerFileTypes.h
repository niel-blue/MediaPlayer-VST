#pragma once

#include <filesystem>
#include <string_view>

namespace MediaPlayer {

enum class MediaFileType {
    audio,
    video,
    other,
};

[[nodiscard]] MediaFileType classifyMediaFile(
    const std::filesystem::path& path);
[[nodiscard]] std::wstring_view mediaFileTypeLabel(MediaFileType type) noexcept;

} // namespace MediaPlayer
