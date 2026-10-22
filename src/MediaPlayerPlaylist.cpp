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
    return remove(std::vector<std::int32_t>{index});
}

PlaylistModel::RemoveResult PlaylistModel::remove(
    const std::vector<std::int32_t>& indices)
{
    std::lock_guard lock(mutex_);
    auto selected = indices;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected.empty() || selected.front() < 0 ||
        selected.back() >= static_cast<std::int32_t>(entries_.size()))
        return {};

    const auto removingCurrent = std::binary_search(
        selected.begin(), selected.end(), currentIndex_);
    const auto removedBeforeCurrent = std::lower_bound(
        selected.begin(), selected.end(), currentIndex_) - selected.begin();
    for (auto it = selected.rbegin(); it != selected.rend(); ++it)
        entries_.erase(entries_.begin() + *it);

    if (entries_.empty() || removingCurrent) {
        // Removing the active item deliberately leaves no selection. It must
        // not silently switch playback to a neighboring item.
        currentIndex_ = -1;
        pendingOpenPath_.clear();
    } else if (currentIndex_ >= 0) {
        currentIndex_ -= static_cast<std::int32_t>(removedBeforeCurrent);
    }

    return {true, removingCurrent};
}

bool PlaylistModel::move(std::int32_t sourceIndex, std::int32_t targetIndex)
{
    std::lock_guard lock(mutex_);
    if (targetIndex < 0 || targetIndex >= static_cast<std::int32_t>(entries_.size()))
        return false;
    return moveLocked({sourceIndex},
                      targetIndex > sourceIndex ? targetIndex + 1 : targetIndex);
}

bool PlaylistModel::move(const std::vector<std::int32_t>& indices,
                         std::int32_t insertionIndex)
{
    std::lock_guard lock(mutex_);
    return moveLocked(indices, insertionIndex);
}

bool PlaylistModel::moveLocked(const std::vector<std::int32_t>& indices,
                               std::int32_t insertionIndex)
{
    const auto entryCount = static_cast<std::int32_t>(entries_.size());
    auto selected = indices;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected.empty() || selected.front() < 0 || selected.back() >= entryCount ||
        insertionIndex < 0 || insertionIndex > entryCount)
        return false;
    std::vector<std::int32_t> order;
    order.reserve(entries_.size());
    for (std::int32_t index = 0; index < entryCount; ++index) {
        if (!std::binary_search(selected.begin(), selected.end(), index))
            order.push_back(index);
    }
    const auto beforeInsertion = std::lower_bound(
        selected.begin(), selected.end(), insertionIndex) - selected.begin();
    order.insert(order.begin() + insertionIndex - beforeInsertion,
                 selected.begin(), selected.end());
    std::vector<std::filesystem::path> reordered;
    reordered.reserve(entries_.size());
    auto newCurrent = -1;
    for (const auto index : order) {
        if (index == currentIndex_)
            newCurrent = static_cast<std::int32_t>(reordered.size());
        reordered.push_back(std::move(entries_[static_cast<std::size_t>(index)]));
    }
    entries_ = std::move(reordered);
    currentIndex_ = newCurrent;
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
