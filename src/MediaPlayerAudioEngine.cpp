#include "MediaPlayerAudioEngine.h"
#include "MediaPlayerDebugLog.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace MediaPlayer {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    stop();
}

void AudioEngine::setOutputSampleRate(int sampleRate) noexcept
{
    if (sampleRate > 0)
        outputSampleRate_.store(sampleRate, std::memory_order_release);
}

void AudioEngine::start()
{
    std::lock_guard lock(commandMutex_);
    if (worker_.joinable())
        return;

    stopRequested_ = false;
    worker_ = std::thread(&AudioEngine::workerLoop, this);
    DebugLog::write("AUDIO_ENGINE_START");
}

void AudioEngine::stop()
{
    {
        std::lock_guard lock(commandMutex_);
        stopRequested_ = true;
        commands_.clear();
    }
    commandChanged_.notify_one();

    if (worker_.joinable())
        worker_.join();

    decoder_.reset();
    playing_.store(false, std::memory_order_release);
    loaded_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    positionFrames_.store(0, std::memory_order_release);
    durationFrames_.store(0, std::memory_order_release);
    decoderFinished_ = false;
    {
        std::lock_guard lock(stateMutex_);
        currentFilePath_.clear();
    }
    requestQueueClear();
    DebugLog::write("AUDIO_ENGINE_STOP");
}

void AudioEngine::requestOpenFile(std::filesystem::path file)
{
    finished_.store(false, std::memory_order_release);
    enqueue({CommandType::openFile, std::move(file)});
}

void AudioEngine::requestPlay()
{
    enqueue({CommandType::play, {}});
}

void AudioEngine::requestPause()
{
    finished_.store(false, std::memory_order_release);
    enqueue({CommandType::pause, {}});
}

void AudioEngine::requestStop()
{
    finished_.store(false, std::memory_order_release);
    enqueue({CommandType::stop, {}});
}

void AudioEngine::requestUnload()
{
    finished_.store(false, std::memory_order_release);
    enqueue({CommandType::unload, {}});
}

void AudioEngine::requestSeek(float normalizedPosition)
{
    finished_.store(false, std::memory_order_release);
    enqueue({CommandType::seek, {}, std::clamp(normalizedPosition, 0.0f, 1.0f)});
}

void AudioEngine::setVolume(float normalizedVolume) noexcept
{
    volume_.store(std::clamp(normalizedVolume, 0.0f, 1.0f),
                  std::memory_order_release);
}

AudioEngine::PlaybackSnapshot AudioEngine::snapshot() const noexcept
{
    return {
        loaded_.load(std::memory_order_acquire),
        playing_.load(std::memory_order_acquire),
        finished_.load(std::memory_order_acquire),
        volume_.load(std::memory_order_acquire),
        outputSampleRate_.load(std::memory_order_acquire),
        positionFrames_.load(std::memory_order_acquire),
        durationFrames_.load(std::memory_order_acquire),
    };
}

void AudioEngine::reportConsumedFrames(std::size_t frames) noexcept
{
    if (frames > 0)
        positionFrames_.fetch_add(static_cast<std::uint64_t>(frames),
                                  std::memory_order_relaxed);
}

std::filesystem::path AudioEngine::currentFilePath() const
{
    std::lock_guard lock(stateMutex_);
    return currentFilePath_;
}

std::string AudioEngine::lastError() const
{
    std::lock_guard lock(stateMutex_);
    return lastError_;
}

void AudioEngine::enqueue(Command command)
{
    {
        std::lock_guard lock(commandMutex_);
        commands_.push_back(std::move(command));
    }
    commandChanged_.notify_one();
}

void AudioEngine::workerLoop()
{
    while (true) {
        Command command {};
        bool hasCommand = false;
        {
            std::lock_guard lock(commandMutex_);
            if (stopRequested_)
                break;
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                hasCommand = true;
            }
        }

        if (hasCommand) {
            handle(std::move(command));
            continue;
        }

        if (awaitingQueueClear_ &&
            clearQueueAcknowledged_.load(std::memory_order_acquire)) {
            awaitingQueueClear_ = false;
            if (resumeAfterQueueClear_) {
                if (!startAfterPrebuffer_)
                    playing_.store(true, std::memory_order_release);
                resumeAfterQueueClear_ = false;
            }
        }

        if (!awaitingQueueClear_ && decoder_) {
            if (startAfterPrebuffer_ &&
                audioQueue_.availableFrames() >= prebufferFrames()) {
                startAfterPrebuffer_ = false;
                playing_.store(true, std::memory_order_release);
                DebugLog::write("AUDIO_PREBUFFER_READY frames=" +
                                std::to_string(audioQueue_.availableFrames()));
            }

            const auto shouldDecode =
                playing_.load(std::memory_order_acquire) ||
                startAfterPrebuffer_;
            if (shouldDecode) {
                if (decoderFinished_) {
                    if (audioQueue_.availableFrames() == 0) {
                        playing_.store(false, std::memory_order_release);
                        finished_.store(true, std::memory_order_release);
                        positionFrames_.store(
                            durationFrames_.load(std::memory_order_acquire),
                            std::memory_order_release);
                        DebugLog::write("AUDIO_PLAYBACK_END");
                    }
                } else {
                    std::string error;
                    const auto result = decoder_->decode(audioQueue_, error);
                    if (result == AudioDecodeResult::error) {
                        setLastError(std::move(error));
                        decoder_.reset();
                        loaded_.store(false, std::memory_order_release);
                        playing_.store(false, std::memory_order_release);
                        finished_.store(false, std::memory_order_release);
                        DebugLog::write("AUDIO_DECODE_ERROR " + lastError());
                    } else if (result == AudioDecodeResult::finished) {
                        // Keep the transport in the playing state until the
                        // already-decoded tail has reached the process callback.
                        decoderFinished_ = true;
                        DebugLog::write("AUDIO_DECODE_FINISHED");
                    }

                    if (result == AudioDecodeResult::producedAudio)
                        continue;
                }
            }
        }

        std::unique_lock lock(commandMutex_);
        commandChanged_.wait_for(lock,
                                 std::chrono::milliseconds(2),
                                 [this] {
                                     return stopRequested_ || !commands_.empty();
                                 });
    }
}

void AudioEngine::handle(Command command)
{
    switch (command.type) {
    case CommandType::openFile: {
        decoder_.reset();
        decoderFinished_ = false;
        playing_.store(false, std::memory_order_release);
        loaded_.store(false, std::memory_order_release);
        finished_.store(false, std::memory_order_release);
        positionFrames_.store(0, std::memory_order_release);
        durationFrames_.store(0, std::memory_order_release);
        {
            std::lock_guard lock(stateMutex_);
            currentFilePath_.clear();
        }
        requestQueueClear();
        awaitingQueueClear_ = true;
        resumeAfterQueueClear_ = false;
        startAfterPrebuffer_ = false;
        setLastError({});

        auto decoder = std::make_unique<FfmpegAudioStream>();
        DecodedAudioInfo info;
        std::string error;
        if (!decoder->open(command.file,
                           outputSampleRate_.load(std::memory_order_acquire),
                           info,
                           error)) {
            setLastError(std::move(error));
            DebugLog::write("AUDIO_OPEN_ERROR file=" + command.file.u8string() +
                            " error=" + lastError());
            return;
        }
        decoder_ = std::move(decoder);
        const auto decodedInfo = decoder_->decodedInfo();
        durationFrames_.store(decodedInfo.durationFrames, std::memory_order_release);
        positionFrames_.store(0, std::memory_order_release);
        {
            std::lock_guard lock(stateMutex_);
            currentFilePath_ = command.file;
        }
        loaded_.store(true, std::memory_order_release);
        {
            std::ostringstream message;
            message << "AUDIO_OPEN file=" << command.file.u8string()
                    << " sourceRate=" << info.sourceSampleRate
                    << " sourceChannels=" << info.sourceChannels
                    << " outputRate=" << info.outputSampleRate
                    << " durationFrames=" << info.durationFrames;
            DebugLog::write(message.str());
        }
        break;
    }
    case CommandType::play:
        if (decoder_) {
            if (finished_.load(std::memory_order_acquire)) {
                decoderFinished_ = false;
                requestQueueClear();
                awaitingQueueClear_ = true;
                resumeAfterQueueClear_ = true;
                std::string error;
                if (!decoder_->seekToNormalized(0.0, error)) {
                    resumeAfterQueueClear_ = false;
                    playing_.store(false, std::memory_order_release);
                    setLastError(std::move(error));
                    break;
                }
                positionFrames_.store(0, std::memory_order_release);
                finished_.store(false, std::memory_order_release);
            }
            if (!playing_.load(std::memory_order_acquire)) {
                startAfterPrebuffer_ = true;
                if (awaitingQueueClear_)
                    resumeAfterQueueClear_ = true;
            }
        }
        break;
    case CommandType::pause:
        finished_.store(false, std::memory_order_release);
        startAfterPrebuffer_ = false;
        playing_.store(false, std::memory_order_release);
        resumeAfterQueueClear_ = false;
        break;
    case CommandType::stop:
        playing_.store(false, std::memory_order_release);
        finished_.store(false, std::memory_order_release);
        positionFrames_.store(0, std::memory_order_release);
        decoderFinished_ = false;
        startAfterPrebuffer_ = false;
        requestQueueClear();
        awaitingQueueClear_ = true;
        resumeAfterQueueClear_ = false;
        if (decoder_) {
            std::string error;
            if (!decoder_->seekToNormalized(0.0, error))
                setLastError(std::move(error));
            else
                setLastError({});
        }
        break;
    case CommandType::unload:
        decoder_.reset();
        decoderFinished_ = false;
        playing_.store(false, std::memory_order_release);
        loaded_.store(false, std::memory_order_release);
        finished_.store(false, std::memory_order_release);
        positionFrames_.store(0, std::memory_order_release);
        durationFrames_.store(0, std::memory_order_release);
        {
            std::lock_guard lock(stateMutex_);
            currentFilePath_.clear();
        }
        requestQueueClear();
        awaitingQueueClear_ = true;
        resumeAfterQueueClear_ = false;
        startAfterPrebuffer_ = false;
        break;
    case CommandType::seek: {
        if (!decoder_)
            break;

        // Keep the public transport state playing while the decoder seeks.
        // The queue is replaced asynchronously, so changing this flag to
        // false here would make a seek look like a pause to the editor.
        const auto wasPlaying = playing_.load(std::memory_order_acquire);
        decoderFinished_ = false;
        finished_.store(false, std::memory_order_release);
        requestQueueClear();
        awaitingQueueClear_ = true;
        resumeAfterQueueClear_ = wasPlaying;

        std::string error;
        if (!decoder_->seekToNormalized(command.normalizedPosition, error)) {
            resumeAfterQueueClear_ = false;
            playing_.store(false, std::memory_order_release);
            setLastError(std::move(error));
            break;
        }

        const auto duration = durationFrames_.load(std::memory_order_acquire);
        positionFrames_.store(static_cast<std::uint64_t>(
                                  command.normalizedPosition * duration),
                              std::memory_order_release);
        break;
    }
    }
}

std::uint64_t AudioEngine::prebufferFrames() const noexcept
{
    const auto sampleRate = static_cast<std::uint64_t>(
        std::max(1, outputSampleRate_.load(std::memory_order_acquire)));
    return std::max<std::uint64_t>(
        1,
        sampleRate * kPrebufferMilliseconds / 1000);
}

void AudioEngine::requestQueueClear() noexcept
{
    clearQueueAcknowledged_.store(false, std::memory_order_release);
    clearQueueRequested_.store(true, std::memory_order_release);
}

void AudioEngine::setLastError(std::string error)
{
    std::lock_guard lock(stateMutex_);
    lastError_ = std::move(error);
}

} // namespace MediaPlayer
