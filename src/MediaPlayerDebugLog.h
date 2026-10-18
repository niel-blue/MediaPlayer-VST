#pragma once

#include <string_view>

namespace MediaPlayer {

// Temporary diagnostic log for tracking transport, decoder, and audio queue
// events across the editor, audio worker, and video worker threads.
class DebugLog final {
public:
    static void write(std::string_view message) noexcept;
};

} // namespace MediaPlayer
