#include "MediaPlayerPlaylist.h"

#include <filesystem>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

} // namespace

int main()
{
    using MediaPlayer::PlaylistModel;
    const auto first = std::filesystem::path(L"C:\\media\\first.wav");
    const auto second = std::filesystem::path(L"C:\\media\\second.mp4");
    const auto third = std::filesystem::path(L"C:\\media\\third.flac");

    PlaylistModel playlist;
    expect(playlist.addOrSelect(first), "first item can be added");
    expect(playlist.addOrSelect(second), "second item can be added");
    expect(playlist.snapshot().currentIndex == 1,
           "newly added item becomes current");

    expect(playlist.select(first).has_value(), "existing first item can be selected");
    auto state = playlist.snapshot();
    expect(state.entries.size() == 2, "selecting does not duplicate an item");
    expect(state.currentIndex == 0, "first item remains selected after polling");
    expect(state.pendingOpenPath == first, "selection records the pending file");

    const auto advanced = playlist.advanceFrom(0);
    expect(advanced == second, "natural end advances to the following item");
    state = playlist.snapshot();
    expect(state.currentIndex == 1, "advance updates the current index");

    expect(playlist.addOrSelect(third), "third item can be added");
    const auto removedBeforeCurrent = playlist.remove(0);
    expect(removedBeforeCurrent.removed && !removedBeforeCurrent.removedCurrent,
           "removing an earlier item keeps the active media loaded");
    state = playlist.snapshot();
    expect(state.currentIndex == 1 && state.entries[1] == third,
           "current index follows the same item after an earlier removal");

    const auto removedCurrent = playlist.remove(1);
    expect(removedCurrent.removedCurrent,
           "removing the active item is reported to the processor");
    state = playlist.snapshot();
    expect(state.currentIndex == -1 && state.pendingOpenPath.empty(),
           "removing the active item clears selection and pending open state");

    playlist.restore({first, second}, 0);
    state = playlist.snapshot();
    expect(state.currentIndex == 0 && state.entries.size() == 2,
           "host state restores entries and selection together");
    expect(state.pendingOpenPath.empty(),
           "restoring host state does not invent a pending open request");

    const auto fourth = std::filesystem::path(L"C:/media/fourth.wav");
    const auto fifth = std::filesystem::path(L"C:/media/fifth.wav");
    playlist.restore({first, second, third}, 0);
    expect(playlist.move(0, 2) && playlist.snapshot().currentIndex == 2 &&
           playlist.snapshot().entries == std::vector<std::filesystem::path>{second, third, first},
           "legacy single-item moves retain their target-index semantics");
    expect(!playlist.move(0, 3), "single-item moves reject an invalid target index");
    playlist.restore({first, second, third, fourth, fifth}, 2);
    expect(playlist.move(std::vector<std::int32_t>{1, 2}, 5),
           "a selected group can move to the end");
    state = playlist.snapshot();
    expect(state.entries == std::vector<std::filesystem::path>{first, fourth, fifth, second, third} &&
           state.currentIndex == 4, "group order and playing track survive a downward move");
    expect(playlist.move(std::vector<std::int32_t>{3, 4}, 0),
           "a selected group can move to the beginning");
    state = playlist.snapshot();
    expect(state.entries == std::vector<std::filesystem::path>{second, third, first, fourth, fifth} &&
           state.currentIndex == 1, "group order and playing track survive an upward move");
    playlist.restore({first, second, third, fourth, fifth}, 3);
    expect(playlist.move(std::vector<std::int32_t>{0, 2}, 4),
           "noncontiguous selected rows can move together");
    state = playlist.snapshot();
    expect(state.entries == std::vector<std::filesystem::path>{second, fourth, first, third, fifth} &&
           state.currentIndex == 1, "moving other rows preserves the current item");
    const auto unchanged = state.entries;
    expect(!playlist.move(std::vector<std::int32_t>{0, 99}, 0) &&
           playlist.snapshot().entries == unchanged, "invalid batch move is atomic");
    playlist.restore({first, second, third, fourth, fifth}, 4);
    const auto batchRemoved = playlist.remove(std::vector<std::int32_t>{1, 3, 1});
    state = playlist.snapshot();
    expect(batchRemoved.removed && !batchRemoved.removedCurrent &&
           state.entries == std::vector<std::filesystem::path>{first, third, fifth} &&
           state.currentIndex == 2, "batch deletion handles duplicates without deleting extra rows");
    const auto activeRemoved = playlist.remove(std::vector<std::int32_t>{0, 2});
    expect(activeRemoved.removedCurrent && playlist.snapshot().currentIndex == -1 &&
           playlist.snapshot().entries == std::vector<std::filesystem::path>{third},
           "deleting a group containing the playing track clears that track");
    expect(!playlist.remove(std::vector<std::int32_t>{0, 5}).removed &&
           playlist.snapshot().entries.size() == 1, "invalid batch delete is atomic");
    expect(playlist.remove(std::vector<std::int32_t>{0}).removed &&
           playlist.snapshot().entries.empty(), "all selected entries can be removed");
    return failures == 0 ? 0 : 1;
}
