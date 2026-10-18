#pragma once

#include "MediaPlayerMessages.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace MediaPlayer {

class VideoDecoder;

struct VideoInfo final {
    bool hasVideo {false};
    int width {0};
    int height {0};
    double frameRate {0.0};
    std::int64_t durationMicroseconds {0};
};

struct VideoFrame final {
    int width {0};
    int height {0};
    std::int64_t presentationMicroseconds {0};
    std::vector<std::uint8_t> bgra;
};

class VideoEngine final {
public:
    VideoEngine();
    ~VideoEngine();

    VideoEngine(const VideoEngine&) = delete;
    VideoEngine& operator=(const VideoEngine&) = delete;

    void requestOpenFile(std::filesystem::path file);
    void requestClear();
    void requestPlay();
    void requestPause();
    void requestStop();
    void requestSeek(float normalizedPosition);

    void updateClock(std::uint64_t positionFrames,
                     int sampleRate,
                     bool playing);
    double currentPositionSeconds() const;
    double durationSeconds() const;
    bool isPlaying() const;
    bool currentVideoInfo(VideoInfo& info) const;
    bool consumeVideoInfo(VideoInfo& info);
    std::shared_ptr<const VideoFrame> acquireFrame();

private:
    enum class CommandType {
        open,
        clear,
        play,
        pause,
        stop,
        seek,
    };

    struct Command final {
        CommandType type;
        std::filesystem::path file;
        float normalizedPosition {0.0f};
    };

    void enqueue(Command command);
    void workerLoop();
    void clearFrames();
    void publishInfo(VideoInfo info);
    double currentClockSeconds() const;
    void setPlaying(bool playing);
    void setClockSeconds(double seconds, bool playing);

    std::mutex commandMutex_;
    std::condition_variable commandChanged_;
    std::deque<Command> commands_;
    std::atomic<bool> stopRequested_ {false};
    std::unique_ptr<VideoDecoder> decoder_;
    std::thread worker_;
    bool decoderFinished_ {true};

    mutable std::mutex clockMutex_;
    double clockSeconds_ {0.0};
    bool clockPlaying_ {false};
    std::chrono::steady_clock::time_point clockUpdated_ {
        std::chrono::steady_clock::now()};
    double durationSeconds_ {0.0};

    static constexpr std::size_t kFrameQueueCapacity = 8;
    mutable std::mutex frameMutex_;
    std::deque<std::shared_ptr<const VideoFrame>> frameQueue_;
    std::shared_ptr<const VideoFrame> currentFrame_;

    mutable std::mutex infoMutex_;
    std::optional<VideoInfo> currentInfo_;
    std::optional<VideoInfo> pendingInfo_;
};

class VideoWindow final {
public:
    VideoWindow() = default;
    ~VideoWindow();

    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;

    bool create(void* owner, VideoEngine* engine);
    void close();
    void hide();
    void showForPlayback();
    void applyVideoInfo(const VideoInfo& info);
    void setUserCloseHandler(std::function<void()> handler);
    void setUserClickHandler(std::function<void()> handler);
    void setUserFileDropHandler(
        std::function<void(const std::filesystem::path&)> handler);
    void setUserSeekHandler(std::function<void(float)> handler);
    void setUserWheelHandler(std::function<void(int)> handler);
    void setUserPlacementHandler(
        std::function<void(const VideoWindowPlacement&)> handler);
    void applyRememberedPlacement(const VideoWindowPlacement& placement);
    void showVolumeIndicator(float normalizedVolume);

private:
    static long long __stdcall windowProc(void* window,
                                          unsigned int message,
                                          unsigned long long wParam,
                                          long long lParam);
    static void registerClass();

    void paint(void* device);
    void setScale(double scale);
    void resizeToClient(int width, int height);
    void constrainSizing(void* rawRect, unsigned long long edge) const;
    void rememberCurrentSize();
    void toggleFullscreen();
    void enterFullscreen();
    void exitFullscreen();
    VideoWindowPlacement currentPlacement() const noexcept;
    void notifyPlacementChanged() const;
    void handleDroppedFiles(void* rawDrop);
    void showAtInitialSize();

    void* window_ {nullptr};
    void* owner_ {nullptr};
    VideoEngine* engine_ {nullptr};
    VideoInfo info_ {};
    std::shared_ptr<const VideoFrame> frame_;
    bool created_ {false};
    bool applyingSize_ {false};
    bool applyingPlacement_ {false};
    bool hasUserSizedWindow_ {false};
    bool hasUserPosition_ {false};
    bool placementInitialized_ {false};
    bool closedByUser_ {false};
    bool draggingSeek_ {false};
    bool singleClickPending_ {false};
    bool windowDragPending_ {false};
    bool windowDragConsumed_ {false};
    int windowDragStartX_ {0};
    int windowDragStartY_ {0};
    int windowDragInitialX_ {0};
    int windowDragInitialY_ {0};
    int rememberedX_ {0};
    int rememberedY_ {0};
    int rememberedWidth_ {0};
    int rememberedHeight_ {0};
    bool fullscreen_ {false};
    bool fullscreenRestoreValid_ {false};
    int fullscreenRestoreX_ {0};
    int fullscreenRestoreY_ {0};
    int fullscreenRestoreWidth_ {0};
    int fullscreenRestoreHeight_ {0};
    std::uint64_t fullscreenRestoreStyle_ {0};
    std::uint64_t fullscreenRestoreExStyle_ {0};
    std::function<void()> userCloseHandler_;
    std::function<void()> userClickHandler_;
    std::function<void(const std::filesystem::path&)> userFileDropHandler_;
    std::function<void(float)> userSeekHandler_;
    std::function<void(int)> userWheelHandler_;
    std::function<void(const VideoWindowPlacement&)> userPlacementHandler_;
    bool volumeIndicatorVisible_ {false};
    float volumeIndicatorLevel_ {1.0f};
    std::chrono::steady_clock::time_point volumeIndicatorUntil_ {};
};

} // namespace MediaPlayer
