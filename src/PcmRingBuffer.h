#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MediaPlayer {

// Single-producer/single-consumer interleaved-free stereo PCM queue.
//
// The decoder thread is the producer and the VST process thread is the
// consumer. The storage is allocated once in the constructor; read/write do
// not allocate, wait, or take a mutex.
class PcmRingBuffer final {
public:
    explicit PcmRingBuffer(std::size_t capacityFrames);

    PcmRingBuffer(const PcmRingBuffer&) = delete;
    PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;

    std::size_t capacityFrames() const noexcept { return capacityFrames_; }
    std::size_t availableFrames() const noexcept;
    std::size_t freeFrames() const noexcept;

    // Returns the number of complete stereo frames written.
    std::size_t writeStereo(const float* left,
                            const float* right,
                            std::size_t frames) noexcept;

    // Returns the number of complete stereo frames read.
    std::size_t readStereo(float* left,
                           float* right,
                           std::size_t frames) noexcept;

    // Call only after the producer and consumer have stopped using the queue.
    void clear() noexcept;

private:
    std::size_t indexFor(std::uint64_t frame) const noexcept;

    const std::size_t capacityFrames_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::atomic<std::uint64_t> readFrame_ {0};
    std::atomic<std::uint64_t> writeFrame_ {0};
};

} // namespace MediaPlayer
