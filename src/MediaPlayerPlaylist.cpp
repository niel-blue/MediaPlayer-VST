#include "MediaPlayerPlaylist.h"

#include <algorithm>
#include <iterator>

namespace MediaPlayer {

PlaylistModel::Snapshot PlaylistModel::snapshot() const
{
    std::lock_guard lock(mutex_);
    return {entries_, currentIndex_, pendingOpenPath_};
}

bool PlaylistModel::addOrSelect(const std::filesystem::path& file)
{
    if (file.empty())
        return false;

    std::lock_guard lock(mutex_);
    const auto found = std::find(entries_.begin(), entries_.end(), file);
    if (found == entries_.end()) {
        if (entries_.size() >= kMaximumEntries)
            return false;
        entries_.push_back(file);
        currentIndex_ = static_cast<std::int32_t>(entries_.size() - 1);
    } else {
        currentIndex_ = static_cast<std::int32_t>(
            std::distance(entries_.begin(), found));
    }
    pendingOpenPath_ = file;
    return true;
}

std::optional<std::filesystem::path> PlaylistModel::select(
    const std::filesystem::path& file)
{
    std::lock_guard lock(mutex_);
    const auto found = std::find(entries_.begin(), entries_.end(), file);
    if (found == entries_.end())
        return std::nullopt;

    currentIndex_ = static_cast<std::int32_t>(
        std::distance(entries_.begin(), found));
    pendingOpenPath_ = *found;
    return *found;
}

std::optional<std::filesystem::path> PlaylistModel::advanceFrom(
    std::int32_t expectedCurrentIndex)
{
    std::lock_guard lock(mutex_);
    if (currentIndex_ != expectedCurrentIndex || currentIndex_ < 0 ||
        currentIndex_ + 1 >= static_cast<std::int32_t>(entries_.size()))
        return std::nullopt;

    ++currentIndex_;
    pendingOpenPath_ = entries_[static_cast<std::size_t>(currentIndex_)];
    return pendingOpenPath_;
}

PlaylistModel::RemoveResult PlaylistModel::remove(std::int32_t index)
{
    std::lock_guard lock(mutex_);
    if (index < 0 || index >= static_cast<std::int32_t>(entries_.size()))
        return {};

    const auto removingCurrent = index == currentIndex_;
    entries_.erase(entries_.begin() + index);

    if (entries_.empty() || removingCurrent) {
        // Removing the active item deliberately leaves no selection. It must
        // not silently switch playback to a neighboring item.
        currentIndex_ = -1;
        pendingOpenPath_.clear();
    } else if (currentIndex_ > index) {
        --currentIndex_;
    }

    return {true, removingCurrent};
}

bool PlaylistModel::move(std::int32_t sourceIndex, std::int32_t targetIndex)
{
    std::lock_guard lock(mutex_);
    const auto entryCount = static_cast<std::int32_t>(entries_.size());
    if (sourceIndex < 0 || sourceIndex >= entryCount || targetIndex < 0 ||
        targetIndex >= entryCount)
        return false;
    if (sourceIndex == targetIndex)
        return true;

    auto movedEntry = std::move(entries_[static_cast<std::size_t>(sourceIndex)]);
    entries_.erase(entries_.begin() + sourceIndex);
    entries_.insert(entries_.begin() + targetIndex, std::move(movedEntry));

    if (currentIndex_ == sourceIndex) {
        currentIndex_ = targetIndex;
    } else if (sourceIndex < targetIndex && currentIndex_ > sourceIndex &&
               currentIndex_ <= targetIndex) {
        --currentIndex_;
    } else if (sourceIndex > targetIndex && currentIndex_ >= targetIndex &&
               currentIndex_ < sourceIndex) {
        ++currentIndex_;
    }

    return true;
}

void PlaylistModel::restore(std::vector<std::filesystem::path> entries,
                            std::int32_t currentIndex)
{
    std::lock_guard lock(mutex_);
    entries_ = std::move(entries);
    currentIndex_ = currentIndex >= 0 &&
                            currentIndex < static_cast<std::int32_t>(entries_.size())
                        ? currentIndex
                        : -1;
    pendingOpenPath_.clear();
}

void PlaylistModel::clear()
{
    std::lock_guard lock(mutex_);
    entries_.clear();
    currentIndex_ = -1;
    pendingOpenPath_.clear();
}

void PlaylistModel::clearPendingOpen()
{
    std::lock_guard lock(mutex_);
    pendingOpenPath_.clear();
}

} // namespace MediaPlayer
