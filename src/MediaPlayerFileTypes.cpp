#include "MediaPlayerFileTypes.h"

#include <algorithm>
#include <cwctype>

namespace MediaPlayer {
namespace {

std::wstring lowerExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return extension;
}

} // namespace

MediaFileType classifyMediaFile(const std::filesystem::path& path)
{
    const auto extension = lowerExtension(path);
    if (extension == L".mp4" || extension == L".mkv" ||
        extension == L".mov" || extension == L".avi" ||
        extension == L".webm" || extension == L".wmv" ||
        extension == L".m4v" || extension == L".mpg" ||
        extension == L".mpeg" || extension == L".ts" ||
        extension == L".m2ts" || extension == L".flv" ||
        extension == L".3gp")
        return MediaFileType::video;

    if (extension == L".wav" || extension == L".mp3" ||
        extension == L".flac" || extension == L".aac" ||
        extension == L".m4a" || extension == L".ogg")
        return MediaFileType::audio;

    return MediaFileType::other;
}

std::wstring_view mediaFileTypeLabel(MediaFileType type) noexcept
{
    switch (type) {
    case MediaFileType::audio:
        return L"[AUDIO] ";
    case MediaFileType::video:
        return L"[VIDEO] ";
    case MediaFileType::other:
        return L"[MEDIA] ";
    }
    return L"[MEDIA] ";
}

} // namespace MediaPlayer
