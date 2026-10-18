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

    return failures == 0 ? 0 : 1;
}
