#pragma once

#include "FfmpegAudioDecoder.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace MediaPlayer {

// Owns the blocking FFmpeg work on a non-real-time thread and exposes only
// the lock-free PCM queue to the VST process callback.
class AudioEngine final {
public:
    // Keep roughly ten seconds of decoded PCM available at 48 kHz. This
    // absorbs short video-decoder spikes without changing the host callback.
    static constexpr std::size_t kQueueCapacityFrames = 480'000;
    static constexpr std::uint32_t kPrebufferMilliseconds = 500;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void setOutputSampleRate(int sampleRate) noexcept;
    void start();
    void stop();

    // These methods are control-side operations. They may allocate and take a
    // mutex; the VST process callback must never call them.
    void requestOpenFile(std::filesystem::path file);
    void requestPlay();
    void requestPause();
    // Stop at the beginning while keeping the decoder and media metadata
    // loaded so a later play request can restart the same file.
    void requestStop();
    // Remove the decoder and media metadata completely.
    void requestUnload();
    void requestSeek(float normalizedPosition);
    void setVolume(float normalizedVolume) noexcept;

    struct PlaybackSnapshot final {
        bool loaded {false};
        bool playing {false};
        bool finished {false};
        float volume {1.0f};
        int sampleRate {48'000};
        std::uint64_t positionFrames {0};
        std::uint64_t durationFrames {0};
    };

    PlaybackSnapshot snapshot() const noexcept;
    void reportConsumedFrames(std::size_t frames) noexcept;
    std::filesystem::path currentFilePath() const;

    PcmRingBuffer& audioQueue() noexcept { return audioQueue_; }

    // The consumer thread performs the actual queue clear at the beginning of
    // its next process block. This keeps the queue single-consumer safe.
    bool consumeClearRequest() noexcept
    {
        if (!clearQueueRequested_.exchange(false, std::memory_order_acq_rel))
            return false;

        audioQueue_.clear();
        clearQueueAcknowledged_.store(true, std::memory_order_release);
        return true;
    }

    bool isPlaying() const noexcept
    {
        return playing_.load(std::memory_order_acquire);
    }

    std::string lastError() const;

private:
    enum class CommandType {
        openFile,
        play,
        pause,
        stop,
        unload,
        seek,
    };

    struct Command final {
        CommandType type;
        std::filesystem::path file;
        float normalizedPosition {0.0f};
    };

    void enqueue(Command command);
    void workerLoop();
    void handle(Command command);
    void requestQueueClear() noexcept;
    std::uint64_t prebufferFrames() const noexcept;
    void setLastError(std::string error);

    PcmRingBuffer audioQueue_ {kQueueCapacityFrames};
    std::atomic<int> outputSampleRate_ {48'000};

    mutable std::mutex commandMutex_;
    std::condition_variable commandChanged_;
    std::deque<Command> commands_;
    bool stopRequested_ {false};
    std::thread worker_;

    std::unique_ptr<FfmpegAudioStream> decoder_;
    std::atomic<bool> playing_ {false};
    std::atomic<bool> loaded_ {false};
    std::atomic<bool> finished_ {false};
    std::atomic<float> volume_ {1.0f};
    std::atomic<std::uint64_t> positionFrames_ {0};
    std::atomic<std::uint64_t> durationFrames_ {0};
    std::atomic<bool> clearQueueRequested_ {false};
    std::atomic<bool> clearQueueAcknowledged_ {true};
    bool awaitingQueueClear_ {false};
    bool resumeAfterQueueClear_ {false};
    bool startAfterPrebuffer_ {false};
    bool decoderFinished_ {false};

    mutable std::mutex stateMutex_;
    std::string lastError_;
    std::filesystem::path currentFilePath_;
};

} // namespace MediaPlayer
