#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "MediaPlayerAudioEngine.h"
#include "MediaPlayerMessages.h"
#include "MediaPlayerPlaylist.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace MediaPlayer {

class Processor final : public Steinberg::Vst::AudioEffect {
public:
    Processor() = default;
    ~Processor() override = default;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new Processor());
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs,
        Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs,
        Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(
        Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(
        Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API notify(
        Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

    // Control-side entry points used by the editor/connection-point layer.
    // They are intentionally separate from process(), which only consumes
    // the lock-free PCM queue.
    void requestOpenFile(std::filesystem::path file);
    void requestPlay();
    void requestPause();
    void requestStop();
    void requestSeek(float normalizedPosition);
    void setVolume(float normalizedVolume) noexcept;
    void selectPlaylist(const std::filesystem::path& file, bool play);
    void clearPlaylist();
    void removePlaylist(std::int32_t index);
    void movePlaylist(std::int32_t sourceIndex, std::int32_t targetIndex);

private:
    void applyRestoredState();

    AudioEngine audioEngine_;
    PlaylistModel playlist_;
    bool active_ {false};
    mutable std::mutex restoreStateMutex_;
    bool stateRestorePending_ {false};
    bool restoreLoaded_ {false};
    bool restorePlaying_ {false};
    float restoreVolume_ {1.0f};
    std::uint64_t restorePositionFrames_ {0};
    std::uint64_t restoreDurationFrames_ {0};
    mutable std::mutex uiStateMutex_;
    VideoWindowPlacement videoPlacement_ {};
    EditorLayoutState editorLayout_ {};
    std::atomic<std::uint64_t> statusSequence_ {0};
};

} // namespace MediaPlayer
