#include "FfmpegAudioDecoder.h"
#include "MediaPlayerAudioEngine.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

void writeU16(std::ofstream& stream, std::uint16_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

void writeU32(std::ofstream& stream, std::uint32_t value)
{
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
    stream.put(static_cast<char>((value >> 16) & 0xff));
    stream.put(static_cast<char>((value >> 24) & 0xff));
}

void writeTag(std::ofstream& stream, const char (&tag)[5])
{
    stream.write(tag, 4);
}

std::filesystem::path writeTestWave()
{
    constexpr std::uint32_t sampleRate = 44'100;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t frames = 44'100;
    constexpr std::uint32_t blockAlign = channels * bitsPerSample / 8;
    constexpr std::uint32_t dataSize = frames * blockAlign;

    const auto file = std::filesystem::temp_directory_path() /
                      "media_player_vst3_ffmpeg_test.wav";
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    assert(stream.good());

    writeTag(stream, "RIFF");
    writeU32(stream, 36 + dataSize);
    writeTag(stream, "WAVE");
    writeTag(stream, "fmt ");
    writeU32(stream, 16);
    writeU16(stream, 1);
    writeU16(stream, channels);
    writeU32(stream, sampleRate);
    writeU32(stream, sampleRate * blockAlign);
    writeU16(stream, blockAlign);
    writeU16(stream, bitsPerSample);
    writeTag(stream, "data");
    writeU32(stream, dataSize);

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto phase = static_cast<double>(frame) / sampleRate;
        const auto sample = static_cast<std::int16_t>(
            std::sin(phase * 2.0 * 3.141592653589793 * 440.0) * 12'000.0);
        writeU16(stream, static_cast<std::uint16_t>(sample));
        writeU16(stream, static_cast<std::uint16_t>(sample));
    }

    stream.close();
    return file;
}

} // namespace

int main(int argc, char** argv)
{
    const bool ownsTestFile = argc < 2;
    const auto testFile = ownsTestFile
        ? writeTestWave()
        : std::filesystem::path(argv[1]);

    MediaPlayer::PcmRingBuffer queue(64'000);
    MediaPlayer::FfmpegAudioDecoder decoder;
    MediaPlayer::DecodedAudioInfo info;
    std::string error;

    const auto decoded = decoder.decodeToQueue(testFile, 48'000, queue, info, error);
    if (!decoded) {
        std::cerr << "FFmpeg decode failed: " << error << '\n';
        if (ownsTestFile)
            std::filesystem::remove(testFile);
        return 1;
    }

    assert(info.sourceSampleRate == 44'100);
    assert(info.sourceChannels == 2);
    assert(info.outputSampleRate == 48'000);
    assert(info.outputFrames > 4'000);
    assert(queue.availableFrames() == info.outputFrames);

    std::vector<float> left(256);
    std::vector<float> right(256);
    const auto read = queue.readStereo(left.data(), right.data(), left.size());
    assert(read == left.size());
    for (std::size_t index = 0; index < read; ++index) {
        assert(std::isfinite(left[index]));
        assert(std::isfinite(right[index]));
    }

    // The worker path must also work when the queue is smaller than the
    // complete file. The consumer drains it while the decoder advances.
    MediaPlayer::PcmRingBuffer streamingQueue(8'192);
    MediaPlayer::FfmpegAudioStream stream;
    MediaPlayer::DecodedAudioInfo streamingInfo;
    if (!stream.open(testFile, 48'000, streamingInfo, error)) {
        std::cerr << "FFmpeg streaming open failed: " << error << '\n';
        if (ownsTestFile)
            std::filesystem::remove(testFile);
        return 1;
    }

    std::vector<float> streamingLeft(256);
    std::vector<float> streamingRight(256);
    std::uint64_t streamedFrames = 0;
    for (int iteration = 0; iteration < 100'000; ++iteration) {
        const auto result = stream.decode(streamingQueue, error);
        if (result == MediaPlayer::AudioDecodeResult::error) {
            std::cerr << "FFmpeg streaming decode failed: " << error << '\n';
            if (ownsTestFile)
                std::filesystem::remove(testFile);
            return 1;
        }

        const auto drained = streamingQueue.readStereo(
            streamingLeft.data(), streamingRight.data(), streamingLeft.size());
        streamedFrames += drained;
        for (std::size_t index = 0; index < drained; ++index) {
            assert(std::isfinite(streamingLeft[index]));
            assert(std::isfinite(streamingRight[index]));
        }

        if (result == MediaPlayer::AudioDecodeResult::finished) {
            while (streamingQueue.availableFrames() > 0) {
                const auto remaining = streamingQueue.readStereo(
                    streamingLeft.data(), streamingRight.data(), streamingLeft.size());
                streamedFrames += remaining;
            }
            break;
        }
    }
    assert(streamedFrames > 4'000);

    if (!stream.seekToNormalized(0.5, error)) {
        std::cerr << "FFmpeg seek failed: " << error << '\n';
        if (ownsTestFile)
            std::filesystem::remove(testFile);
        return 1;
    }
    std::size_t seekedFrames = 0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto result = stream.decode(streamingQueue, error);
        if (result == MediaPlayer::AudioDecodeResult::error) {
            std::cerr << "FFmpeg post-seek decode failed: " << error << '\n';
            if (ownsTestFile)
                std::filesystem::remove(testFile);
            return 1;
        }
        seekedFrames += streamingQueue.availableFrames();
        if (seekedFrames > 0 || result == MediaPlayer::AudioDecodeResult::finished)
            break;
    }
    assert(seekedFrames > 0);
    stream.close();

    // Finally exercise the same command/worker path used by the processor.
    MediaPlayer::AudioEngine engine;
    engine.setOutputSampleRate(48'000);
    engine.setVolume(0.5f);
    assert(std::abs(engine.snapshot().volume - 0.5f) < 0.001f);
    engine.start();
    engine.requestOpenFile(testFile);
    engine.requestPlay();

    bool workerProducedAudio = false;
    bool fileLoaded = false;
    std::vector<float> engineLeft(256);
    std::vector<float> engineRight(256);
    for (int iteration = 0; iteration < 1'000; ++iteration) {
        if (engine.consumeClearRequest())
            engine.audioQueue().clear();

        const auto drained = engine.audioQueue().readStereo(
            engineLeft.data(), engineRight.data(), engineLeft.size());
        if (drained > 0) {
            workerProducedAudio = true;
            fileLoaded = engine.snapshot().loaded;
            engine.reportConsumedFrames(drained);
            for (std::size_t index = 0; index < drained; ++index) {
                assert(std::isfinite(engineLeft[index]));
                assert(std::isfinite(engineRight[index]));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    engine.requestPause();

    bool paused = false;
    for (int iteration = 0; iteration < 500; ++iteration) {
        if (!engine.snapshot().playing) {
            paused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(fileLoaded);
    assert(paused);
    assert(engine.currentFilePath() == testFile);
    const auto queuedFramesAtPause = engine.audioQueue().availableFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(engine.audioQueue().availableFrames() == queuedFramesAtPause);

    engine.requestStop();
    bool stoppedWithMediaLoaded = false;
    for (int iteration = 0; iteration < 500; ++iteration) {
        if (engine.consumeClearRequest())
            engine.audioQueue().clear();

        const auto snapshot = engine.snapshot();
        if (!snapshot.playing && snapshot.loaded &&
            snapshot.positionFrames == 0 &&
            engine.currentFilePath() == testFile) {
            stoppedWithMediaLoaded = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(stoppedWithMediaLoaded);

    engine.stop();

    if (!workerProducedAudio) {
        std::cerr << "AudioEngine worker did not produce audio";
        const auto engineError = engine.lastError();
        if (!engineError.empty())
            std::cerr << ": " << engineError;
        std::cerr << '\n';
        if (ownsTestFile)
            std::filesystem::remove(testFile);
        return 1;
    }

    if (ownsTestFile)
        std::filesystem::remove(testFile);

    std::cout << "FfmpegAudioDecoderTests: decoded " << info.outputFrames
              << " frames at " << info.outputSampleRate << " Hz, streamed "
              << streamedFrames << " frames\n";
    return 0;
}
