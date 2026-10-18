#include "PcmRingBuffer.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace MediaPlayer {

PcmRingBuffer::PcmRingBuffer(std::size_t capacityFrames)
    : capacityFrames_(std::max<std::size_t>(capacityFrames, 2)),
      left_(capacityFrames_),
      right_(capacityFrames_)
{
    assert(capacityFrames_ <
           (std::numeric_limits<std::uint64_t>::max() / 2));
}

std::size_t PcmRingBuffer::indexFor(std::uint64_t frame) const noexcept
{
    return static_cast<std::size_t>(frame % capacityFrames_);
}

std::size_t PcmRingBuffer::availableFrames() const noexcept
{
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto read = readFrame_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

std::size_t PcmRingBuffer::freeFrames() const noexcept
{
    return capacityFrames_ - std::min(availableFrames(), capacityFrames_);
}

std::size_t PcmRingBuffer::writeStereo(const float* left,
                                       const float* right,
                                       std::size_t frames) noexcept
{
    if (left == nullptr || right == nullptr || frames == 0)
        return 0;

    const auto write = writeFrame_.load(std::memory_order_relaxed);
    const auto read = readFrame_.load(std::memory_order_acquire);
    const auto free = capacityFrames_ -
                      std::min<std::size_t>(static_cast<std::size_t>(write - read),
                                            capacityFrames_);
    const auto count = std::min(frames, free);

    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index = indexFor(write + offset);
        left_[index] = left[offset];
        right_[index] = right[offset];
    }

    writeFrame_.store(write + count, std::memory_order_release);
    return count;
}

std::size_t PcmRingBuffer::readStereo(float* left,
                                      float* right,
                                      std::size_t frames) noexcept
{
    if (left == nullptr || right == nullptr || frames == 0)
        return 0;

    const auto read = readFrame_.load(std::memory_order_relaxed);
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto available = static_cast<std::size_t>(write - read);
    const auto count = std::min(frames, std::min(available, capacityFrames_));

    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index = indexFor(read + offset);
        left[offset] = left_[index];
        right[offset] = right_[index];
    }

    readFrame_.store(read + count, std::memory_order_release);
    return count;
}

void PcmRingBuffer::clear() noexcept
{
    const auto write = writeFrame_.load(std::memory_order_relaxed);
    readFrame_.store(write, std::memory_order_release);
}

} // namespace MediaPlayer
