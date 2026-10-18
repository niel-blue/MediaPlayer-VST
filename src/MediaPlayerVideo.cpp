#include "MediaPlayerVideo.h"
#include "MediaPlayerDebugLog.h"
#include "MediaPlayerVideoDecoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace MediaPlayer {
namespace {

constexpr wchar_t kVideoWindowClassName[] = L"DAWMediaPlayer.VideoWindow";
constexpr UINT_PTR kVideoTimer = 1;
constexpr UINT_PTR kSingleClickTimer = 2;
constexpr UINT kScale25 = 2001;
constexpr UINT kScale50 = 2002;
constexpr UINT kScale100 = 2003;
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr int kVideoSeekBarHeight = 6;
constexpr COLORREF kVideoSeekBackground = RGB(12, 22, 35);
constexpr COLORREF kVideoSeekTrack = RGB(28, 76, 108);
constexpr COLORREF kVideoSeekAccent = RGB(0, 178, 235);
constexpr COLORREF kVideoSeekHandle = RGB(231, 244, 251);
constexpr COLORREF kPauseOverlayBackground = RGB(12, 22, 35);
constexpr COLORREF kPauseOverlayBorder = RGB(6, 201, 255);
constexpr COLORREF kPauseOverlayIcon = RGB(231, 244, 251);

HWND asWindow(void* handle) noexcept
{
    return static_cast<HWND>(handle);
}

void drawPauseOverlay(HDC device, int clientWidth, int videoHeight)
{
    if (device == nullptr || clientWidth <= 0 || videoHeight <= 0)
        return;

    const auto diameter = std::clamp(
        std::min(clientWidth, videoHeight) / 4,
        64,
        128);
    const auto left = (clientWidth - diameter) / 2;
    const auto top = (videoHeight - diameter) / 2;
    const auto right = left + diameter;
    const auto bottom = top + diameter;

    const auto background = CreateSolidBrush(kPauseOverlayBackground);
    const auto border = CreatePen(PS_SOLID, 2, kPauseOverlayBorder);
    const auto oldBackground = SelectObject(device, background);
    const auto oldBorder = SelectObject(device, border);
    Ellipse(device, left, top, right, bottom);
    SelectObject(device, oldBorder);
    SelectObject(device, oldBackground);
    DeleteObject(border);
    DeleteObject(background);

    const auto barWidth = std::max(8, diameter / 8);
    const auto barHeight = std::max(28, diameter / 2);
    const auto barGap = std::max(8, diameter / 10);
    const auto firstBarLeft = clientWidth / 2 - barGap / 2 - barWidth;
    const auto secondBarLeft = clientWidth / 2 + barGap / 2;
    const auto barTop = (videoHeight - barHeight) / 2;
    const auto barBrush = CreateSolidBrush(kPauseOverlayIcon);
    const auto oldBrush = SelectObject(device, barBrush);
    RoundRect(device,
              firstBarLeft,
              barTop,
              firstBarLeft + barWidth,
              barTop + barHeight,
              4,
              4);
    RoundRect(device,
              secondBarLeft,
              barTop,
              secondBarLeft + barWidth,
              barTop + barHeight,
              4,
              4);
    SelectObject(device, oldBrush);
    DeleteObject(barBrush);
}

} // namespace

VideoEngine::VideoEngine()
    : decoder_(std::make_unique<VideoDecoder>(&stopRequested_)),
      worker_(&VideoEngine::workerLoop, this)
{
}

VideoEngine::~VideoEngine()
{
    {
        std::lock_guard lock(commandMutex_);
        stopRequested_.store(true, std::memory_order_release);
    }
    commandChanged_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void VideoEngine::enqueue(Command command)
{
    {
        std::lock_guard lock(commandMutex_);
        commands_.push_back(std::move(command));
    }
    commandChanged_.notify_all();
}

void VideoEngine::requestOpenFile(std::filesystem::path file)
{
    DebugLog::write("VIDEO_OPEN_REQUEST file=" + file.u8string());
    clearFrames();
    setClockSeconds(0.0, false);
    publishInfo(VideoInfo {});
    enqueue(Command {CommandType::open, std::move(file), 0.0f});
}

void VideoEngine::requestClear()
{
    clearFrames();
    setClockSeconds(0.0, false);
    publishInfo(VideoInfo {});
    enqueue(Command {CommandType::clear, {}, 0.0f});
}

void VideoEngine::requestPlay()
{
    setPlaying(true);
    enqueue(Command {CommandType::play, {}, 0.0f});
}

void VideoEngine::requestPause()
{
    setPlaying(false);
    enqueue(Command {CommandType::pause, {}, 0.0f});
}

void VideoEngine::requestStop()
{
    setClockSeconds(0.0, false);
    enqueue(Command {CommandType::stop, {}, 0.0f});
}

void VideoEngine::requestSeek(float normalizedPosition)
{
    normalizedPosition = std::clamp(normalizedPosition, 0.0f, 1.0f);
    bool playing = false;
    double duration = 0.0;
    {
        std::lock_guard lock(clockMutex_);
        playing = clockPlaying_;
        duration = durationSeconds_;
    }
    setClockSeconds(duration * normalizedPosition, playing);
    enqueue(Command {CommandType::seek, {}, normalizedPosition});
}

void VideoEngine::updateClock(std::uint64_t positionFrames,
                              int sampleRate,
                              bool playing)
{
    if (sampleRate <= 0)
        sampleRate = 48'000;
    setClockSeconds(static_cast<double>(positionFrames) /
                        static_cast<double>(sampleRate),
                    playing);
}

double VideoEngine::currentPositionSeconds() const
{
    return currentClockSeconds();
}

double VideoEngine::durationSeconds() const
{
    std::lock_guard lock(clockMutex_);
    return durationSeconds_;
}

bool VideoEngine::isPlaying() const
{
    std::lock_guard lock(clockMutex_);
    return clockPlaying_;
}

bool VideoEngine::currentVideoInfo(VideoInfo& info) const
{
    std::lock_guard lock(infoMutex_);
    if (!currentInfo_.has_value())
        return false;
    info = *currentInfo_;
    return true;
}

bool VideoEngine::consumeVideoInfo(VideoInfo& info)
{
    std::lock_guard lock(infoMutex_);
    if (!pendingInfo_.has_value())
        return false;
    info = *pendingInfo_;
    pendingInfo_.reset();
    return true;
}

std::shared_ptr<const VideoFrame> VideoEngine::acquireFrame()
{
    const auto target = currentClockSeconds();
    bool consumed = false;
    std::shared_ptr<const VideoFrame> result;
    {
        std::lock_guard lock(frameMutex_);
        while (!frameQueue_.empty()) {
            const auto& candidate = frameQueue_.front();
            const auto candidateSeconds = static_cast<double>(
                candidate->presentationMicroseconds) /
                kMicrosecondsPerSecond;
            // Show the first decoded frame even when its timestamp is a
            // little after zero. This is important while a newly opened
            // video is paused; otherwise the window stays black until play.
            if (currentFrame_ != nullptr && candidateSeconds > target + 0.005)
                break;
            currentFrame_ = candidate;
            frameQueue_.pop_front();
            consumed = true;
        }
        result = currentFrame_;
    }
    if (consumed)
        commandChanged_.notify_all();
    return result;
}

void VideoEngine::clearFrames()
{
    std::lock_guard lock(frameMutex_);
    frameQueue_.clear();
    currentFrame_.reset();
}

void VideoEngine::publishInfo(VideoInfo info)
{
    {
        std::lock_guard lock(infoMutex_);
        currentInfo_ = info;
        pendingInfo_ = info;
    }
    if (!info.hasVideo) {
        std::lock_guard lock(clockMutex_);
        durationSeconds_ = 0.0;
    } else {
        std::lock_guard lock(clockMutex_);
        durationSeconds_ = static_cast<double>(info.durationMicroseconds) /
                           kMicrosecondsPerSecond;
    }
}

double VideoEngine::currentClockSeconds() const
{
    std::lock_guard lock(clockMutex_);
    auto seconds = clockSeconds_;
    if (clockPlaying_) {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - clockUpdated_).count();
        seconds += std::max(0.0, elapsed);
    }
    if (durationSeconds_ > 0.0)
        seconds = std::min(seconds, durationSeconds_);
    return std::max(0.0, seconds);
}

void VideoEngine::setPlaying(bool playing)
{
    std::lock_guard lock(clockMutex_);
    const auto now = std::chrono::steady_clock::now();
    if (clockPlaying_)
        clockSeconds_ += std::max(
            0.0,
            std::chrono::duration<double>(now - clockUpdated_).count());
    clockPlaying_ = playing;
    clockUpdated_ = now;
}

void VideoEngine::setClockSeconds(double seconds, bool playing)
{
    std::lock_guard lock(clockMutex_);
    clockSeconds_ = std::max(0.0, seconds);
    clockPlaying_ = playing;
    clockUpdated_ = std::chrono::steady_clock::now();
}

void VideoEngine::workerLoop()
{
    for (;;) {
        Command command {};
        bool hasCommand = false;
        {
            std::unique_lock lock(commandMutex_);
            if (stopRequested_)
                break;
            const auto queueFull = [&] {
                std::lock_guard frameLock(frameMutex_);
                return frameQueue_.size() >= kFrameQueueCapacity;
            };
            const auto needsPausedFrame = [&] {
                std::lock_guard frameLock(frameMutex_);
                return frameQueue_.empty() && currentFrame_ == nullptr;
            };
            if (commands_.empty() &&
                (decoder_ == nullptr || decoderFinished_ || queueFull() ||
                 (!isPlaying() && !needsPausedFrame()))) {
                commandChanged_.wait_for(lock, std::chrono::milliseconds(8));
                continue;
            }
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                hasCommand = true;
            }
        }

        if (hasCommand) {
            switch (command.type) {
            case CommandType::open: {
                decoder_->close();
                decoderFinished_ = false;
                clearFrames();
                VideoInfo info {};
                if (decoder_->open(command.file, info)) {
                    publishInfo(info);
                    decoderFinished_ = false;
                    std::ostringstream message;
                    message << "VIDEO_OPEN file=" << command.file.u8string()
                            << " width=" << info.width
                            << " height=" << info.height
                            << " frameRate=" << info.frameRate
                            << " durationUs=" << info.durationMicroseconds;
                    DebugLog::write(message.str());
                } else {
                    publishInfo(VideoInfo {});
                    decoderFinished_ = true;
                    DebugLog::write("VIDEO_OPEN_ERROR file=" +
                                    command.file.u8string());
                }
                break;
            }
            case CommandType::clear:
                decoder_->close();
                decoderFinished_ = true;
                clearFrames();
                publishInfo(VideoInfo {});
                break;
            case CommandType::play:
                if (decoder_->isOpen()) {
                    // The audio transport is the timing authority. When a
                    // video window was hidden and is shown again, the
                    // decoder may still have a stale frame queue even though
                    // the shared clock is at the paused audio position.
                    // Re-seek on every resume so the first newly decoded
                    // frame is taken from that position, not from the stale
                    // queue or the beginning of the stream.
                    const auto resumeAt = currentClockSeconds();
                    if (decoder_->seek(resumeAt)) {
                        clearFrames();
                        decoderFinished_ = false;
                    }
                }
                break;
            case CommandType::pause:
                break;
            case CommandType::stop:
                if (decoder_->isOpen()) {
                    (void)decoder_->seek(0.0);
                    decoderFinished_ = false;
                    clearFrames();
                }
                break;
            case CommandType::seek: {
                if (decoder_->isOpen()) {
                    double duration = 0.0;
                    {
                        std::lock_guard lock(clockMutex_);
                        duration = durationSeconds_;
                    }
                    (void)decoder_->seek(
                        duration * std::clamp(command.normalizedPosition,
                                              0.0f,
                                              1.0f));
                    decoderFinished_ = false;
                    clearFrames();
                }
                break;
            }
            }
            continue;
        }

        if (decoder_ == nullptr || decoderFinished_)
            continue;
        const auto frame = decoder_->decodeNext();
        if (frame == nullptr) {
            decoderFinished_ = true;
            DebugLog::write("VIDEO_DECODE_FINISHED");
            continue;
        }
        {
            std::lock_guard lock(frameMutex_);
            if (frameQueue_.size() < kFrameQueueCapacity)
                frameQueue_.push_back(frame);
        }
    }
}

void VideoWindow::registerClass()
{
    static std::once_flag registration;
    std::call_once(registration, [] {
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        // The window is explicitly double-buffered in paint(). Avoid the
        // class-level erase/redraw behavior that can expose an intermediate
        // black frame while the video is being updated.
        // Request WM_LBUTTONDBLCLK so a double-click can be distinguished from
        // the existing single-click transport action.
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = reinterpret_cast<WNDPROC>(&VideoWindow::windowProc);
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(
            nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        windowClass.hbrBackground = static_cast<HBRUSH>(
            GetStockObject(BLACK_BRUSH));
        windowClass.lpszClassName = kVideoWindowClassName;
        RegisterClassExW(&windowClass);
    });
}

VideoWindow::~VideoWindow()
{
    close();
}

bool VideoWindow::create(void* owner, VideoEngine* engine)
{
    if (created_)
        return true;
    registerClass();
    owner_ = owner;
    engine_ = engine;
    const auto ownerWindow = GetAncestor(asWindow(owner), GA_ROOT);
    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kVideoWindowClassName,
        L"MediaPlayer VST - Video",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        360,
        ownerWindow,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (window_ == nullptr) {
        owner_ = nullptr;
        engine_ = nullptr;
        return false;
    }
    created_ = true;
    placementInitialized_ = false;
    closedByUser_ = false;
    fullscreen_ = false;
    fullscreenRestoreValid_ = false;
    singleClickPending_ = false;
    SetTimer(asWindow(window_), kVideoTimer, 16, nullptr);
    DragAcceptFiles(asWindow(window_), TRUE);
    ShowWindow(asWindow(window_), SW_HIDE);
    return true;
}

void VideoWindow::close()
{
    if (engine_ != nullptr)
        engine_->requestPause();
    if (window_ != nullptr) {
        if (fullscreen_)
            exitFullscreen();
        KillTimer(asWindow(window_), kSingleClickTimer);
        rememberCurrentSize();
        KillTimer(asWindow(window_), kVideoTimer);
        DragAcceptFiles(asWindow(window_), FALSE);
        DestroyWindow(asWindow(window_));
    }
    window_ = nullptr;
    owner_ = nullptr;
    engine_ = nullptr;
    created_ = false;
    frame_.reset();
    placementInitialized_ = false;
    draggingSeek_ = false;
    singleClickPending_ = false;
    windowDragPending_ = false;
    windowDragConsumed_ = false;
    volumeIndicatorVisible_ = false;
    fullscreen_ = false;
    fullscreenRestoreValid_ = false;
}

void VideoWindow::hide()
{
    if (window_ != nullptr)
        ShowWindow(asWindow(window_), SW_HIDE);
}

void VideoWindow::showForPlayback()
{
    if (window_ == nullptr)
        return;

    if (!info_.hasVideo && engine_ != nullptr) {
        VideoInfo currentInfo {};
        if (engine_->currentVideoInfo(currentInfo) && currentInfo.hasVideo)
            applyVideoInfo(currentInfo);
    }
    if (!info_.hasVideo)
        return;

    closedByUser_ = false;
    if (!placementInitialized_) {
        showAtInitialSize();
        return;
    }
    ShowWindow(asWindow(window_), SW_SHOW);
    UpdateWindow(asWindow(window_));
}

void VideoWindow::setUserCloseHandler(std::function<void()> handler)
{
    userCloseHandler_ = std::move(handler);
}

void VideoWindow::setUserClickHandler(std::function<void()> handler)
{
    userClickHandler_ = std::move(handler);
}

void VideoWindow::setUserFileDropHandler(
    std::function<void(const std::filesystem::path&)> handler)
{
    userFileDropHandler_ = std::move(handler);
}

void VideoWindow::setUserSeekHandler(std::function<void(float)> handler)
{
    userSeekHandler_ = std::move(handler);
}

void VideoWindow::setUserWheelHandler(std::function<void(int)> handler)
{
    userWheelHandler_ = std::move(handler);
}

void VideoWindow::setUserPlacementHandler(
    std::function<void(const VideoWindowPlacement&)> handler)
{
    userPlacementHandler_ = std::move(handler);
}

void VideoWindow::applyRememberedPlacement(
    const VideoWindowPlacement& placement)
{
    rememberedX_ = placement.x;
    rememberedY_ = placement.y;
    rememberedWidth_ = std::max<std::int32_t>(1, placement.width);
    rememberedHeight_ = std::max<std::int32_t>(1, placement.height);
    hasUserSizedWindow_ = placement.valid && placement.userSized;
    hasUserPosition_ = placement.valid && placement.userPosition;

    if (!placement.valid) {
        rememberedX_ = 0;
        rememberedY_ = 0;
        rememberedWidth_ = 0;
        rememberedHeight_ = 0;
        placementInitialized_ = false;
        return;
    }

    if (window_ != nullptr && placementInitialized_ &&
        rememberedWidth_ > 0 && rememberedHeight_ > 0) {
        applyingPlacement_ = true;
        SetWindowPos(asWindow(window_),
                     nullptr,
                     rememberedX_,
                     rememberedY_,
                     rememberedWidth_,
                     rememberedHeight_,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyingPlacement_ = false;
    }
}

VideoWindowPlacement VideoWindow::currentPlacement() const noexcept
{
    VideoWindowPlacement placement {};
    placement.x = rememberedX_;
    placement.y = rememberedY_;
    placement.width = rememberedWidth_;
    placement.height = rememberedHeight_;
    placement.valid = placement.width > 0 && placement.height > 0;
    placement.userSized = hasUserSizedWindow_;
    placement.userPosition = hasUserPosition_;
    return placement;
}

void VideoWindow::notifyPlacementChanged() const
{
    if (userPlacementHandler_ != nullptr)
        userPlacementHandler_(currentPlacement());
}

void VideoWindow::showVolumeIndicator(float normalizedVolume)
{
    volumeIndicatorLevel_ = std::clamp(normalizedVolume, 0.0f, 1.0f);
    volumeIndicatorVisible_ = true;
    volumeIndicatorUntil_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(1200);
    if (window_ != nullptr)
        InvalidateRect(asWindow(window_), nullptr, FALSE);
}

void VideoWindow::handleDroppedFiles(void* rawDrop)
{
    const auto drop = reinterpret_cast<HDROP>(rawDrop);
    if (drop == nullptr)
        return;

    const auto fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < fileCount; ++index) {
        const auto characterCount = DragQueryFileW(drop, index, nullptr, 0);
        if (characterCount == 0)
            continue;

        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(characterCount) + 1);
        if (DragQueryFileW(drop,
                           index,
                           buffer.data(),
                           static_cast<UINT>(buffer.size())) == 0)
            continue;

        if (userFileDropHandler_ != nullptr)
            userFileDropHandler_(std::filesystem::path(buffer.data()));
    }
    DragFinish(drop);
}

void VideoWindow::applyVideoInfo(const VideoInfo& info)
{
    if (window_ == nullptr)
        return;
    if (!info.hasVideo || info.width <= 0 || info.height <= 0) {
        info_ = {};
        frame_.reset();
        // An empty info notification means the media is being cleared or
        // replaced. Do not carry the user-close state into the next media.
        closedByUser_ = false;
        hide();
        return;
    }

    const auto hadNoVideo = !info_.hasVideo;
    const auto dimensionsChanged = info_.width != info.width ||
                                   info_.height != info.height;
    info_ = info;
    if (hadNoVideo)
        closedByUser_ = false;
    if (closedByUser_)
        return;
    if (dimensionsChanged || !placementInitialized_)
        showAtInitialSize();
    else {
        ShowWindow(asWindow(window_), SW_SHOW);
        UpdateWindow(asWindow(window_));
    }
}

void VideoWindow::paint(void* rawDevice)
{
    const auto device = reinterpret_cast<HDC>(rawDevice);
    RECT client {};
    GetClientRect(asWindow(window_), &client);
    const auto clientWidth = std::max(1, static_cast<int>(client.right));
    const auto clientHeight = std::max(1, static_cast<int>(client.bottom));
    const auto seekBarHeight = std::min(kVideoSeekBarHeight, clientHeight);
    const auto videoHeight = std::max(1, clientHeight - seekBarHeight);

    // Render the whole frame off-screen, then copy it to the window in one
    // operation. Directly clearing the window before StretchDIBits made the
    // black background briefly visible on every refresh.
    const auto backBuffer = CreateCompatibleDC(device);
    const auto backBitmap = CreateCompatibleBitmap(
        device, clientWidth, clientHeight);
    if (backBuffer == nullptr || backBitmap == nullptr) {
        if (backBitmap != nullptr)
            DeleteObject(backBitmap);
        if (backBuffer != nullptr)
            DeleteDC(backBuffer);
        const auto background = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(device, &client, background);
        return;
    }

    const auto previousBitmap = SelectObject(backBuffer, backBitmap);
    const auto background = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(backBuffer, &client, background);

    if (frame_ != nullptr && !frame_->bgra.empty()) {
        // The default GDI stretch mode is not reliable for 32-bit video DIBs;
        // it can introduce colored speckles and broken edges when a 1080p
        // frame is reduced to the initial 50% window size.
        SetStretchBltMode(backBuffer, HALFTONE);
        SetBrushOrgEx(backBuffer, 0, 0, nullptr);
        const auto scale = std::min(
        static_cast<double>(clientWidth) / frame_->width,
        static_cast<double>(videoHeight) / frame_->height);
    const auto destinationWidth = std::max(
        1, static_cast<int>(std::lround(frame_->width * scale)));
    const auto destinationHeight = std::max(
        1, static_cast<int>(std::lround(frame_->height * scale)));
    const auto left = (clientWidth - destinationWidth) / 2;
    const auto top = (videoHeight - destinationHeight) / 2;

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = frame_->width;
    bitmapInfo.bmiHeader.biHeight = -frame_->height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(backBuffer,
                  left,
                  top,
                  destinationWidth,
                  destinationHeight,
                  0,
                  0,
                  frame_->width,
                  frame_->height,
                  frame_->bgra.data(),
                  &bitmapInfo,
                   DIB_RGB_COLORS,
                   SRCCOPY);
    }

    if (frame_ != nullptr && info_.hasVideo && engine_ != nullptr &&
        !engine_->isPlaying())
        drawPauseOverlay(backBuffer, clientWidth, videoHeight);

    RECT seekBar {0, videoHeight, clientWidth, clientHeight};
    const auto seekBackground = CreateSolidBrush(kVideoSeekBackground);
    FillRect(backBuffer, &seekBar, seekBackground);
    DeleteObject(seekBackground);

    const auto duration = engine_ != nullptr ? engine_->durationSeconds() : 0.0;
    const auto position = engine_ != nullptr
        ? engine_->currentPositionSeconds()
        : 0.0;
    const auto normalized = duration > 0.0
        ? std::clamp(position / duration, 0.0, 1.0)
        : 0.0;
    const auto progressWidth = static_cast<int>(std::lround(
        clientWidth * normalized));
    if (progressWidth > 0) {
        RECT progress {0, videoHeight, progressWidth, clientHeight};
        const auto progressBrush = CreateSolidBrush(kVideoSeekAccent);
        FillRect(backBuffer, &progress, progressBrush);
        DeleteObject(progressBrush);
    }
    const auto handleX = std::clamp(progressWidth, 1, clientWidth - 1);
    RECT handle {handleX - 1, videoHeight, handleX + 2, clientHeight};
    const auto handleBrush = CreateSolidBrush(kVideoSeekHandle);
    FillRect(backBuffer, &handle, handleBrush);
    DeleteObject(handleBrush);

    if (volumeIndicatorVisible_ &&
        std::chrono::steady_clock::now() < volumeIndicatorUntil_) {
        constexpr int kIndicatorWidth = 132;
        constexpr int kIndicatorHeight = 20;
        const auto indicatorWidth = std::min(
            kIndicatorWidth, std::max(96, clientWidth - 16));
        const auto indicatorLeft = std::max(
            8, clientWidth - indicatorWidth - 10);
        const auto indicatorTop = std::max(
            8, videoHeight - kIndicatorHeight - 10);
        RECT indicator {
            indicatorLeft,
            indicatorTop,
            std::min(clientWidth - 8, indicatorLeft + indicatorWidth),
            indicatorTop + kIndicatorHeight,
        };
        const auto indicatorBackground =
            CreateSolidBrush(kVideoSeekBackground);
        FillRect(backBuffer, &indicator, indicatorBackground);
        DeleteObject(indicatorBackground);
        const auto indicatorBorder = CreateSolidBrush(kVideoSeekAccent);
        FrameRect(backBuffer, &indicator, indicatorBorder);
        DeleteObject(indicatorBorder);

        const auto trackLeft = indicator.left + 10;
        const auto trackRight = std::max(trackLeft + 1, indicator.right - 10);
        const auto trackY = indicator.top + kIndicatorHeight / 2;
        RECT track {
            trackLeft,
            trackY - 2,
            trackRight,
            trackY + 3,
        };
        const auto trackBrush = CreateSolidBrush(kVideoSeekTrack);
        FillRect(backBuffer, &track, trackBrush);
        DeleteObject(trackBrush);

        const auto fillRight = trackLeft + static_cast<int>(std::lround(
            static_cast<double>(trackRight - trackLeft) *
            volumeIndicatorLevel_));
        if (fillRight > trackLeft) {
            RECT fill {trackLeft, track.top, fillRight, track.bottom};
            const auto fillBrush = CreateSolidBrush(kVideoSeekAccent);
            FillRect(backBuffer, &fill, fillBrush);
            DeleteObject(fillBrush);
        }

        const auto knobX = std::clamp(fillRight, trackLeft, trackRight);
        RECT knob {knobX - 2, trackY - 5, knobX + 3, trackY + 6};
        const auto knobBrush = CreateSolidBrush(kVideoSeekHandle);
        FillRect(backBuffer, &knob, knobBrush);
        DeleteObject(knobBrush);
    }

    BitBlt(device,
           0,
           0,
           clientWidth,
           clientHeight,
           backBuffer,
           0,
           0,
           SRCCOPY);
    SelectObject(backBuffer, previousBitmap);
    DeleteObject(backBitmap);
    DeleteDC(backBuffer);
}

void VideoWindow::setScale(double scale)
{
    if (info_.width <= 0 || info_.height <= 0)
        return;
    scale = std::clamp(scale, 0.01, 1.0);
    resizeToClient(std::max(1, static_cast<int>(
        std::lround(info_.width * scale))),
                   std::max(1, static_cast<int>(
        std::lround(info_.height * scale)) + kVideoSeekBarHeight));
    rememberCurrentSize();
    hasUserSizedWindow_ = true;
    hasUserPosition_ = true;
    notifyPlacementChanged();
}

void VideoWindow::resizeToClient(int width, int height)
{
    if (window_ == nullptr)
        return;
    RECT rect {0, 0, width, height};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(
        asWindow(window_), GWL_STYLE));
    const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(
        asWindow(window_), GWL_EXSTYLE));
    AdjustWindowRectEx(&rect, style, FALSE, extendedStyle);
    applyingSize_ = true;
    SetWindowPos(asWindow(window_),
                 nullptr,
                 0,
                 0,
                 rect.right - rect.left,
                 rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    applyingSize_ = false;
}

void VideoWindow::constrainSizing(void* rawRect,
                                  unsigned long long rawEdge) const
{
    if (info_.width <= 0 || info_.height <= 0 || rawRect == nullptr)
        return;
    auto* rect = static_cast<RECT*>(rawRect);
    const auto edge = static_cast<UINT>(rawEdge);
    const auto ratio = static_cast<double>(info_.width) / info_.height;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(
        asWindow(window_), GWL_STYLE));
    const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(
        asWindow(window_), GWL_EXSTYLE));
    RECT nonClientFrame {0, 0, 0, 0};
    AdjustWindowRectEx(&nonClientFrame,
                       style,
                       FALSE,
                       extendedStyle);
    const auto frameWidth = nonClientFrame.right - nonClientFrame.left;
    const auto frameHeight = nonClientFrame.bottom - nonClientFrame.top;
    constexpr LONG minimumClientWidth = 160;
    constexpr LONG minimumVideoHeight = 90;
    auto clientWidth = std::max(
        minimumClientWidth,
        rect->right - rect->left - frameWidth);
    auto clientHeight = std::max(
        minimumVideoHeight + kVideoSeekBarHeight,
        rect->bottom - rect->top - frameHeight);
    auto videoHeight = clientHeight - kVideoSeekBarHeight;

    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
        clientWidth = std::max(
            minimumClientWidth,
            static_cast<LONG>(std::lround(videoHeight * ratio)));
    else
        videoHeight = std::max(
            minimumVideoHeight,
            static_cast<LONG>(std::lround(clientWidth / ratio)));

    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
        videoHeight = std::max(
            minimumVideoHeight,
            static_cast<LONG>(std::lround(clientWidth / ratio)));
    else if (videoHeight == minimumVideoHeight)
        clientWidth = std::max(
            minimumClientWidth,
            static_cast<LONG>(std::lround(videoHeight * ratio)));

    clientHeight = videoHeight + kVideoSeekBarHeight;

    const auto width = clientWidth + frameWidth;
    const auto height = clientHeight + frameHeight;

    const auto left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT ||
                      edge == WMSZ_BOTTOMLEFT;
    const auto top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT ||
                     edge == WMSZ_TOPRIGHT;
    if (left)
        rect->left = rect->right - width;
    else
        rect->right = rect->left + width;
    if (top)
        rect->top = rect->bottom - height;
    else
        rect->bottom = rect->top + height;
}

void VideoWindow::rememberCurrentSize()
{
    if (window_ == nullptr)
        return;
    RECT rect {};
    GetWindowRect(asWindow(window_), &rect);
    rememberedX_ = rect.left;
    rememberedY_ = rect.top;
    rememberedWidth_ = std::max(1L, rect.right - rect.left);
    rememberedHeight_ = std::max(1L, rect.bottom - rect.top);
}

void VideoWindow::toggleFullscreen()
{
    if (fullscreen_)
        exitFullscreen();
    else
        enterFullscreen();
}

void VideoWindow::enterFullscreen()
{
    if (window_ == nullptr || fullscreen_)
        return;

    RECT restoreRect {};
    if (!GetWindowRect(asWindow(window_), &restoreRect))
        return;

    const auto monitor = MonitorFromWindow(
        asWindow(window_), MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr)
        return;

    MONITORINFO monitorInfo {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    fullscreenRestoreX_ = restoreRect.left;
    fullscreenRestoreY_ = restoreRect.top;
    fullscreenRestoreWidth_ = std::max(1L, restoreRect.right - restoreRect.left);
    fullscreenRestoreHeight_ = std::max(1L, restoreRect.bottom - restoreRect.top);
    fullscreenRestoreStyle_ = static_cast<std::uint64_t>(GetWindowLongPtrW(
        asWindow(window_), GWL_STYLE));
    fullscreenRestoreExStyle_ = static_cast<std::uint64_t>(GetWindowLongPtrW(
        asWindow(window_), GWL_EXSTYLE));
    fullscreenRestoreValid_ = true;
    fullscreen_ = true;

    constexpr auto fullscreenStyle =
        static_cast<LONG_PTR>(WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    const auto currentExStyle = static_cast<LONG_PTR>(GetWindowLongPtrW(
        asWindow(window_), GWL_EXSTYLE));
    applyingPlacement_ = true;
    SetWindowLongPtrW(asWindow(window_), GWL_STYLE, fullscreenStyle);
    SetWindowLongPtrW(asWindow(window_), GWL_EXSTYLE, currentExStyle);
    SetWindowPos(asWindow(window_), HWND_TOP,
                 monitorInfo.rcMonitor.left,
                 monitorInfo.rcMonitor.top,
                 monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                 monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    applyingPlacement_ = false;
    ShowWindow(asWindow(window_), SW_SHOW);
    UpdateWindow(asWindow(window_));
}

void VideoWindow::exitFullscreen()
{
    if (window_ == nullptr || !fullscreen_)
        return;

    const auto restoreStyle = static_cast<LONG_PTR>(fullscreenRestoreStyle_);
    const auto restoreExStyle = static_cast<LONG_PTR>(fullscreenRestoreExStyle_);
    const auto restoreValid = fullscreenRestoreValid_ &&
                              fullscreenRestoreWidth_ > 0 &&
                              fullscreenRestoreHeight_ > 0;
    fullscreen_ = false;

    applyingPlacement_ = true;
    SetWindowLongPtrW(asWindow(window_), GWL_STYLE, restoreStyle);
    SetWindowLongPtrW(asWindow(window_), GWL_EXSTYLE, restoreExStyle);
    if (restoreValid) {
        SetWindowPos(asWindow(window_), HWND_TOP,
                     fullscreenRestoreX_,
                     fullscreenRestoreY_,
                     fullscreenRestoreWidth_,
                     fullscreenRestoreHeight_,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    applyingPlacement_ = false;

    if (restoreValid) {
        rememberedX_ = fullscreenRestoreX_;
        rememberedY_ = fullscreenRestoreY_;
        rememberedWidth_ = fullscreenRestoreWidth_;
        rememberedHeight_ = fullscreenRestoreHeight_;
        hasUserSizedWindow_ = true;
        hasUserPosition_ = true;
        placementInitialized_ = true;
        notifyPlacementChanged();
    }
    fullscreenRestoreValid_ = false;
    ShowWindow(asWindow(window_), SW_SHOW);
    UpdateWindow(asWindow(window_));
}

void VideoWindow::showAtInitialSize()
{
    if (window_ == nullptr)
        return;
    int width = rememberedWidth_;
    int height = rememberedHeight_;
    if (!hasUserSizedWindow_ || width <= 0 || height <= 0) {
        auto scale = 0.5;
        MONITORINFO monitorInfo {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const auto monitor = MonitorFromWindow(
            asWindow(owner_), MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
            const auto workWidth = std::max<LONG>(
                1,
                monitorInfo.rcWork.right - monitorInfo.rcWork.left);
            const auto workHeight = std::max<LONG>(
                1,
                monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
            scale = std::min({
                scale,
                0.9 * static_cast<double>(workWidth) / info_.width,
                0.9 * static_cast<double>(
                    std::max<LONG>(1, workHeight - kVideoSeekBarHeight)) /
                    info_.height});
        }
        RECT clientRect {
            0,
            0,
            std::max(160, static_cast<int>(std::lround(info_.width * scale))),
            std::max(90, static_cast<int>(std::lround(info_.height * scale))) +
                kVideoSeekBarHeight,
        };
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(
            asWindow(window_), GWL_STYLE));
        const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(
            asWindow(window_), GWL_EXSTYLE));
        AdjustWindowRectEx(&clientRect,
                           style,
                           FALSE,
                           extendedStyle);
        width = clientRect.right - clientRect.left;
        height = clientRect.bottom - clientRect.top;
    }

    int x = rememberedX_;
    int y = rememberedY_;
    if (!hasUserPosition_) {
        RECT ownerRect {};
        if (owner_ != nullptr && GetWindowRect(asWindow(owner_), &ownerRect)) {
            x = ownerRect.left +
                ((ownerRect.right - ownerRect.left) - width) / 2;
            y = ownerRect.top +
                ((ownerRect.bottom - ownerRect.top) - height) / 2;
        } else {
            MONITORINFO monitorInfo {};
            monitorInfo.cbSize = sizeof(monitorInfo);
            const auto monitor = MonitorFromWindow(
                asWindow(window_), MONITOR_DEFAULTTONEAREST);
            if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
                const auto workWidth = monitorInfo.rcWork.right -
                                       monitorInfo.rcWork.left;
                const auto workHeight = monitorInfo.rcWork.bottom -
                                        monitorInfo.rcWork.top;
                x = monitorInfo.rcWork.left + (workWidth - width) / 2;
                y = monitorInfo.rcWork.top + (workHeight - height) / 2;
            } else {
                x = 100;
                y = 100;
            }
        }
    }

    applyingPlacement_ = true;
    SetWindowPos(asWindow(window_),
                 nullptr,
                 x,
                 y,
                 width,
                 height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    applyingPlacement_ = false;
    placementInitialized_ = true;
    ShowWindow(asWindow(window_), SW_SHOW);
    UpdateWindow(asWindow(window_));
}

long long __stdcall VideoWindow::windowProc(void* rawWindow,
                                            unsigned int message,
                                            unsigned long long wParam,
                                            long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* videoWindow = reinterpret_cast<VideoWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        videoWindow = static_cast<VideoWindow*>(create->lpCreateParams);
        videoWindow->window_ = rawWindow;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(videoWindow));
    }
    if (videoWindow == nullptr)
        return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);

    switch (message) {
    case WM_TIMER:
        if (wParam == kSingleClickTimer) {
            KillTimer(window, kSingleClickTimer);
            if (videoWindow->singleClickPending_) {
                videoWindow->singleClickPending_ = false;
                if (videoWindow->userClickHandler_)
                    videoWindow->userClickHandler_();
            }
            return 0;
        }
        if (wParam == kVideoTimer && videoWindow->engine_ != nullptr) {
            const auto nextFrame = videoWindow->engine_->acquireFrame();
            if (nextFrame != videoWindow->frame_) {
                videoWindow->frame_ = nextFrame;
                InvalidateRect(window, nullptr, FALSE);
            } else if (videoWindow->volumeIndicatorVisible_) {
                if (std::chrono::steady_clock::now() >=
                    videoWindow->volumeIndicatorUntil_)
                    videoWindow->volumeIndicatorVisible_ = false;
                InvalidateRect(window, nullptr, FALSE);
            } else {
                RECT client {};
                GetClientRect(window, &client);
                RECT seekBar {
                    0,
                    std::max(0, static_cast<int>(client.bottom) -
                                     kVideoSeekBarHeight),
                    client.right,
                    client.bottom,
                };
                InvalidateRect(window, &seekBar, FALSE);
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        const auto device = BeginPaint(window, &paint);
        videoWindow->paint(device);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DROPFILES:
        videoWindow->handleDroppedFiles(
            reinterpret_cast<void*>(static_cast<LONG_PTR>(wParam)));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZING:
        videoWindow->hasUserSizedWindow_ = true;
        videoWindow->constrainSizing(
            reinterpret_cast<void*>(static_cast<LONG_PTR>(lParam)), wParam);
        return TRUE;
    case WM_SIZE:
        if (videoWindow->created_ && !videoWindow->applyingSize_ &&
            !videoWindow->applyingPlacement_ &&
            !videoWindow->fullscreen_ &&
            wParam != SIZE_MINIMIZED) {
            videoWindow->rememberCurrentSize();
            if (videoWindow->hasUserSizedWindow_)
                videoWindow->hasUserPosition_ = true;
        }
        return 0;
    case WM_MOVE:
        if (videoWindow->created_ && !videoWindow->applyingSize_ &&
            !videoWindow->applyingPlacement_ &&
            !videoWindow->fullscreen_) {
            videoWindow->rememberCurrentSize();
            videoWindow->hasUserPosition_ = true;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (!videoWindow->fullscreen_) {
            videoWindow->rememberCurrentSize();
            videoWindow->notifyPlacementChanged();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        RECT client {};
        GetClientRect(window, &client);
        const auto seekTop = std::max(
            0,
            static_cast<int>(client.bottom) - kVideoSeekBarHeight);
        if (y >= seekTop) {
            videoWindow->draggingSeek_ = true;
            SetCapture(window);
            if (videoWindow->userSeekHandler_) {
                const auto width = std::max(1, static_cast<int>(client.right));
                videoWindow->userSeekHandler_(std::clamp(
                    static_cast<float>(x) / width, 0.0f, 1.0f));
            }
            return 0;
        }
        POINT cursor {};
        GetCursorPos(&cursor);
        videoWindow->windowDragPending_ = true;
        videoWindow->windowDragConsumed_ = false;
        videoWindow->windowDragStartX_ = cursor.x;
        videoWindow->windowDragStartY_ = cursor.y;
        RECT windowRect {};
        GetWindowRect(window, &windowRect);
        videoWindow->windowDragInitialX_ = windowRect.left;
        videoWindow->windowDragInitialY_ = windowRect.top;
        SetCapture(window);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (videoWindow->draggingSeek_ &&
            (wParam & MK_LBUTTON) != 0) {
            RECT client {};
            GetClientRect(window, &client);
            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto width = std::max(1, static_cast<int>(client.right));
            if (videoWindow->userSeekHandler_)
                videoWindow->userSeekHandler_(std::clamp(
                    static_cast<float>(x) / width, 0.0f, 1.0f));
            return 0;
        }
        if (videoWindow->windowDragPending_ ||
            videoWindow->windowDragConsumed_) {
            if ((wParam & MK_LBUTTON) == 0) {
                videoWindow->windowDragPending_ = false;
                videoWindow->windowDragConsumed_ = false;
                if (GetCapture() == window)
                    ReleaseCapture();
                return 0;
            }

            POINT cursor {};
            GetCursorPos(&cursor);
            const auto deltaX = cursor.x - videoWindow->windowDragStartX_;
            const auto deltaY = cursor.y - videoWindow->windowDragStartY_;
            const auto dragThresholdX = GetSystemMetrics(SM_CXDRAG);
            const auto dragThresholdY = GetSystemMetrics(SM_CYDRAG);
            if (!videoWindow->windowDragConsumed_ &&
                (std::abs(deltaX) >= dragThresholdX ||
                 std::abs(deltaY) >= dragThresholdY)) {
                videoWindow->windowDragPending_ = false;
                videoWindow->windowDragConsumed_ = true;
            }
            if (videoWindow->windowDragConsumed_) {
                SetWindowPos(window,
                             nullptr,
                             videoWindow->windowDragInitialX_ + deltaX,
                             videoWindow->windowDragInitialY_ + deltaY,
                             0,
                             0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (videoWindow->draggingSeek_) {
            RECT client {};
            GetClientRect(window, &client);
            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto width = std::max(1, static_cast<int>(client.right));
            if (videoWindow->userSeekHandler_)
                videoWindow->userSeekHandler_(std::clamp(
                    static_cast<float>(x) / width, 0.0f, 1.0f));
            videoWindow->draggingSeek_ = false;
            if (GetCapture() == window)
                ReleaseCapture();
            return 0;
        }
        if (videoWindow->windowDragConsumed_) {
            videoWindow->windowDragPending_ = false;
            videoWindow->windowDragConsumed_ = false;
            videoWindow->rememberCurrentSize();
            videoWindow->hasUserPosition_ = true;
            videoWindow->notifyPlacementChanged();
            if (GetCapture() == window)
                ReleaseCapture();
            return 0;
        }
        if (videoWindow->windowDragPending_) {
            videoWindow->windowDragPending_ = false;
            if (GetCapture() == window)
                ReleaseCapture();
            videoWindow->singleClickPending_ = true;
            SetTimer(window,
                     kSingleClickTimer,
                     std::max<UINT>(1, GetDoubleClickTime()),
                     nullptr);
            return 0;
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        RECT client {};
        GetClientRect(window, &client);
        const auto seekTop = std::max(
            0,
            static_cast<int>(client.bottom) - kVideoSeekBarHeight);
        if (y >= seekTop)
            return 0;

        KillTimer(window, kSingleClickTimer);
        videoWindow->singleClickPending_ = false;
        videoWindow->windowDragPending_ = false;
        videoWindow->windowDragConsumed_ = false;
        if (GetCapture() == window)
            ReleaseCapture();
        videoWindow->toggleFullscreen();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (videoWindow->userWheelHandler_) {
            videoWindow->userWheelHandler_(static_cast<int>(
                GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wParam))));
            return 0;
        }
        break;
    case WM_CANCELMODE:
        KillTimer(window, kSingleClickTimer);
        videoWindow->singleClickPending_ = false;
        videoWindow->draggingSeek_ = false;
        videoWindow->windowDragPending_ = false;
        videoWindow->windowDragConsumed_ = false;
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kScale25, L"25% size");
        AppendMenuW(menu, MF_STRING, kScale50, L"50% size");
        AppendMenuW(menu, MF_STRING, kScale100, L"100% size");
        POINT point {};
        if (lParam == -1) {
            GetCursorPos(&point);
        } else {
            point.x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            point.y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        }
        const auto command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            point.x,
            point.y,
            0,
            window,
            nullptr);
        DestroyMenu(menu);
        if (command == kScale25)
            videoWindow->setScale(0.25);
        else if (command == kScale50)
            videoWindow->setScale(0.50);
        else if (command == kScale100)
            videoWindow->setScale(1.00);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(window, kSingleClickTimer);
        videoWindow->singleClickPending_ = false;
        if (videoWindow->fullscreen_)
            videoWindow->exitFullscreen();
        videoWindow->rememberCurrentSize();
        videoWindow->hasUserPosition_ = true;
        videoWindow->closedByUser_ = true;
        videoWindow->notifyPlacementChanged();
        if (videoWindow->engine_ != nullptr)
            videoWindow->engine_->requestPause();
        if (videoWindow->userCloseHandler_)
            videoWindow->userCloseHandler_();
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kVideoTimer);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

} // namespace MediaPlayer
