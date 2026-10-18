#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

namespace MediaPlayer {

struct VideoFrame;
struct VideoInfo;

// FFmpeg-backed video decoder. The public interface contains no FFmpeg types,
// so decoder/format changes remain isolated from the playback clock and the
// native video window.
class VideoDecoder final {
public:
    explicit VideoDecoder(std::atomic<bool>* stopRequested);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& file,
                            VideoInfo& info);
    void close() noexcept;
    [[nodiscard]] bool seek(double seconds);
    [[nodiscard]] std::shared_ptr<const VideoFrame> decodeNext();
    [[nodiscard]] bool isOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace MediaPlayer
