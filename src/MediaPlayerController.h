#pragma once

#include "MediaPlayerMessages.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace MediaPlayer {

class NativeEditor;

class Controller final : public Steinberg::Vst::EditController {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IEditController*>(new Controller());
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::IPlugView* PLUGIN_API createView(const char* name) SMTG_OVERRIDE;

    Steinberg::tresult sendOpenFile(const std::filesystem::path& file);
    Steinberg::tresult sendSelectPlaylist(const std::filesystem::path& file,
                                          bool play);
    Steinberg::tresult sendClearPlaylist();
    Steinberg::tresult sendRemovePlaylist(std::int32_t index);
    Steinberg::tresult sendMovePlaylist(std::int32_t sourceIndex,
                                        std::int32_t targetIndex);
    Steinberg::tresult sendTransportMessage(const char* messageId);
    Steinberg::tresult sendSeek(float normalizedPosition);
    Steinberg::tresult sendVolume(float normalizedVolume);
    Steinberg::tresult sendVideoPlacement(const VideoWindowPlacement& placement);
    Steinberg::tresult sendEditorLayout(const EditorLayoutState& layout);
    Steinberg::tresult requestPlaybackStatus();
    Steinberg::tresult PLUGIN_API notify(
        Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

    void setEditor(NativeEditor* editor) noexcept { editor_ = editor; }
    void beginEditorStateSync() noexcept
    {
        hasAppliedEditorState_ = false;
        hasPendingEditorLayout_ = false;
    }
    void clearEditor(NativeEditor* editor) noexcept
    {
        if (editor_ == editor)
            editor_ = nullptr;
    }

    OBJ_METHODS(Controller, EditController)
    REFCOUNT_METHODS(EditController)

private:
    NativeEditor* editor_ {nullptr};
    std::int64_t lastStatusSequence_ {-1};
    bool hasAppliedEditorState_ {false};
    bool hasPendingEditorLayout_ {false};
    EditorLayoutState pendingEditorLayout_ {};
    VideoWindowPlacement lastAppliedVideoPlacement_ {};
    EditorLayoutState lastAppliedEditorLayout_ {};
};

} // namespace MediaPlayer
