#pragma once

#include "MediaPlayerMessages.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace MediaPlayer {

class Controller;
class EditorDropTarget;
class VideoEngine;
class VideoWindow;

class NativeEditor final : public Steinberg::Vst::EditorView {
public:
    NativeEditor(Controller* controller, Steinberg::ViewRect* size);
    ~NativeEditor() override;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(
        Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API attached(
        void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onSize(
        Steinberg::ViewRect* newSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(
        Steinberg::ViewRect* size) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setFrame(
        Steinberg::IPlugFrame* frame) SMTG_OVERRIDE;

    void updatePlaybackStatus(std::uint64_t positionFrames,
                              std::uint64_t durationFrames,
                              int sampleRate,
                              bool playing,
                              bool audioLoaded,
                              float volume);
    void updateMediaPath(const std::filesystem::path& file);
    void clearMedia();
    void applyVideoPlacement(const VideoWindowPlacement& placement);
    void applyEditorLayout(const EditorLayoutState& layout);
    bool updatePlaylist(const std::vector<std::filesystem::path>& entries,
                        std::int32_t currentIndex);

    static constexpr wchar_t kWindowClassName[] = L"DAWMediaPlayer.Editor";
    static constexpr wchar_t kProgressClassName[] = L"DAWMediaPlayer.Progress";
    static constexpr wchar_t kVolumeClassName[] = L"DAWMediaPlayer.Volume";
    static constexpr wchar_t kPlayerPanelClassName[] = L"DAWMediaPlayer.PlayerPanel";
    static long long __stdcall windowProc(void* window,
                                          unsigned int message,
                                          unsigned long long wParam,
                                          long long lParam);
    static long long __stdcall progressProc(void* window,
                                            unsigned int message,
                                            unsigned long long wParam,
                                            long long lParam);
    static long long __stdcall playlistProc(void* window,
                                            unsigned int message,
                                            unsigned long long wParam,
                                            long long lParam);
    static long long __stdcall volumeProc(void* window,
                                          unsigned int message,
                                          unsigned long long wParam,
                                          long long lParam);
    static long long __stdcall playerPanelProc(void* window,
                                               unsigned int message,
                                               unsigned long long wParam,
                                               long long lParam);

protected:
    void attachedToParent() SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

private:
    static constexpr int kDefaultPlaylistHeight = 60;

    enum ControlId {
        kOpenButton = 1001,
        kPlayButton = 1002,
        kStopButton = 1004,
        kPreviousButton = 1005,
        kSeekBar = 1006,
        kVolumeBar = 1007,
        kClearPlaylistButton = 1008,
        kPlaylistList = 1009,
        kPlaylistToggleButton = 1010,
        kNextButton = 1011,
    };

    long long handleMessage(unsigned int message,
                            unsigned long long wParam,
                            long long lParam);
    long long handleProgressMessage(unsigned int message,
                                    unsigned long long wParam,
                                    long long lParam);
    long long handleVolumeMessage(unsigned int message,
                                  unsigned long long wParam,
                                  long long lParam);
    void createControls();
    void resizeControls();
    void chooseFile();
    bool openMediaFile(const std::filesystem::path& file);
    void requestVideoPath(const std::filesystem::path& file);
    void sendTransport(const char* messageId);
    void sendSeekFromControl();
    void applyVolumeState(float normalizedVolume,
                          bool showIndicator = false);
    void setPlaylistVisibility(bool visible, bool persist = true);
    void togglePlaylistVisibility();
    void handleDroppedFiles(void* rawDrop, bool finishDrop);
    void refreshMediaLabels();
    void updateTimeLabels();
    void setPlaybackState(bool playing);
    void drawPlayerPanel(void* device) const;
    void drawPlaylistItem(const void* item) const;
    void drawPlaylistScrollbar() const;
    void drawButton(const void* item) const;
    void sendSeekFromPosition(int position);
    void dragVolumeVertically(int deltaY);
    void adjustVolumeFromWheel(int delta);
    void removePlaylistEntry(int index);
    int playlistInsertionIndexAt(void* rawWindow, int x, int y) const;
    void movePlaylistEntry(int sourceIndex, int insertionIndex);
    void showPlaylistContextMenu(void* rawWindow,
                                 int index,
                                 long long rawPosition);
    void selectAdjacentPlaylist(int direction);
    void selectPlaylistFromControl(bool play);
    void requestEditorHeight(int height);
    void synchronizeNativeWindowSize(const Steinberg::ViewRect& size);
    bool isPlaylistResizePoint(int y) const;
    void beginPlaylistResize(int y);
    void updatePlaylistResize(int y);
    void endPlaylistResize();
    void sendEditorLayoutState();

    void* window_ {nullptr};
    std::uintptr_t vectorDrawingToken_ {0};
    void* titleLabel_ {nullptr};
    void* statusLabel_ {nullptr};
    void* currentTimeLabel_ {nullptr};
    void* durationLabel_ {nullptr};
    void* seekBar_ {nullptr};
    void* volumeBar_ {nullptr};
    void* openButton_ {nullptr};
    void* previousButton_ {nullptr};
    void* playButton_ {nullptr};
    void* stopButton_ {nullptr};
    void* nextButton_ {nullptr};
    void* playerPanel_ {nullptr};
    void* playlistList_ {nullptr};
    void* playlistClearButton_ {nullptr};
    void* playlistToggleButton_ {nullptr};
    void* playlistOriginalProc_ {nullptr};
    bool oleInitialized_ {false};
    bool editorDropTargetRegistered_ {false};
    bool playerPanelDropTargetRegistered_ {false};
    bool playlistDropTargetRegistered_ {false};
    std::uint64_t positionFrames_ {0};
    std::uint64_t durationFrames_ {0};
    float volume_ {1.0f};
    int lastVolumeDragY_ {0};
    int sampleRate_ {48'000};
    bool playing_ {false};
    bool draggingSeek_ {false};
    bool draggingVolume_ {false};
    int seekPosition_ {0};
    bool mediaLoaded_ {false};
    bool audioLoaded_ {false};
    std::filesystem::path mediaPath_;
    std::filesystem::path videoRequestedPath_;
    bool videoRequestPending_ {false};
    bool audioStatusMatchesMedia_ {false};
    bool videoAvailable_ {false};
    std::vector<std::filesystem::path> playlistPaths_;
    std::int32_t currentPlaylistIndex_ {-1};
    bool playlistSelectionPending_ {false};
    bool playlistPointerDown_ {false};
    bool playlistDragging_ {false};
    bool ignorePlaylistMouseUp_ {false};
    int playlistDragSourceIndex_ {-1};
    int playlistDragStartX_ {0};
    int playlistDragStartY_ {0};
    int playlistDropInsertionIndex_ {-1};
    bool playlistVisible_ {false};
    bool playlistVisibilityOverridePending_ {false};
    bool playlistVisibilityOverride_ {false};
    int playlistHeight_ {kDefaultPlaylistHeight};
    bool playlistResizing_ {false};
    int playlistResizeStartY_ {0};
    int playlistResizeStartHeight_ {kDefaultPlaylistHeight};
    int laidOutWidth_ {0};
    int requestedEditorHeight_ {0};

    std::unique_ptr<VideoEngine> videoEngine_;
    std::unique_ptr<VideoWindow> videoWindow_;

    friend class EditorDropTarget;
};

} // namespace MediaPlayer
