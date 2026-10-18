#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace MediaPlayer {

// Owns the playlist and its selection as one synchronized state. Keeping the
// entries, current index, and pending-open path together prevents UI/status
// polling from observing a mixture of old and new playlist values.
class PlaylistModel final {
public:
    static constexpr std::size_t kMaximumEntries = 4096;

    struct Snapshot final {
        std::vector<std::filesystem::path> entries;
        std::int32_t currentIndex {-1};
        std::filesystem::path pendingOpenPath;
    };

    struct RemoveResult final {
        bool removed {false};
        bool removedCurrent {false};
    };

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] bool addOrSelect(const std::filesystem::path& file);
    [[nodiscard]] std::optional<std::filesystem::path> select(
        const std::filesystem::path& file);
    [[nodiscard]] std::optional<std::filesystem::path> advanceFrom(
        std::int32_t expectedCurrentIndex);
    [[nodiscard]] RemoveResult remove(std::int32_t index);
    [[nodiscard]] bool move(std::int32_t sourceIndex,
                           std::int32_t targetIndex);

    void restore(std::vector<std::filesystem::path> entries,
                 std::int32_t currentIndex);
    void clear();
    void clearPendingOpen();

private:
    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> entries_;
    std::int32_t currentIndex_ {-1};
    std::filesystem::path pendingOpenPath_;
};

} // namespace MediaPlayer
