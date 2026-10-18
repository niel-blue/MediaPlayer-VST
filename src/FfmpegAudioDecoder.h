#pragma once

#include "PcmRingBuffer.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace MediaPlayer {

struct DecodedAudioInfo final {
    int sourceSampleRate {0};
    int sourceChannels {0};
    int outputSampleRate {0};
    std::uint64_t outputFrames {0};
    std::uint64_t durationFrames {0};
};

enum class AudioDecodeResult {
    producedAudio,
    needOutputSpace,
    finished,
    error,
};

// Incremental FFmpeg decoder. All methods are intended for the decoder/worker
// thread; they never run from the VST process callback.
class FfmpegAudioStream final {
public:
    FfmpegAudioStream();
    ~FfmpegAudioStream();

    FfmpegAudioStream(const FfmpegAudioStream&) = delete;
    FfmpegAudioStream& operator=(const FfmpegAudioStream&) = delete;

    bool open(const std::filesystem::path& file,
              int outputSampleRate,
              DecodedAudioInfo& info,
              std::string& error);
    AudioDecodeResult decode(PcmRingBuffer& output, std::string& error);
    DecodedAudioInfo decodedInfo() const noexcept;
    bool seekToNormalized(double normalizedPosition, std::string& error);
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Blocking FFmpeg decoder core. Call this only from the decoder/worker thread;
// never from the VST process callback.
class FfmpegAudioDecoder final {
public:
    bool decodeToQueue(const std::filesystem::path& file,
                      int outputSampleRate,
                      PcmRingBuffer& output,
                      DecodedAudioInfo& info,
                      std::string& error) const;
};

} // namespace MediaPlayer
