#include "MediaPlayerProcessor.h"

#include "public.sdk/source/vst/utility/memoryibstream.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace {

std::vector<Steinberg::uint8> saveState(MediaPlayer::Processor& processor)
{
    Steinberg::ResizableMemoryIBStream stream;
    assert(processor.getState(&stream) == Steinberg::kResultOk);
    auto bytes = stream.take();
    return std::move(bytes);
}

} // namespace

int main()
{
    MediaPlayer::Processor source;
    source.requestOpenFile(std::filesystem::u8path(
        "C:/Media/playlist/first.flac"));
    source.requestOpenFile(std::filesystem::u8path(
        "C:/Media/playlist/日本語の二曲目.mp3"));
    source.requestOpenFile(std::filesystem::u8path(
        "C:/Media/playlist/first.flac"));

    const auto selectedPath = std::filesystem::u8path(
        "C:/Media/playlist/second.mp3");
    source.requestOpenFile(selectedPath);
    source.selectPlaylist(selectedPath, false);

    const auto saved = saveState(source);
    assert(saved.size() > 32);

    Steinberg::ResizableMemoryIBStream input;
    assert(input.write(const_cast<Steinberg::uint8*>(saved.data()),
                       static_cast<Steinberg::int32>(saved.size()),
                       nullptr) == Steinberg::kResultOk);
    input.rewind();

    MediaPlayer::Processor restored;
    assert(restored.setState(&input) == Steinberg::kResultOk);
    const auto resaved = saveState(restored);
    assert(resaved == saved);

    return 0;
}
