#include "MediaPlayerEditor.h"

#include "MediaPlayerController.h"
#include "MediaPlayerEditorLayout.h"
#include "MediaPlayerFileTypes.h"
#include "MediaPlayerMessages.h"
#include "MediaPlayerVideo.h"
#include "ui/MediaPlayerEditorStyle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include "ui/vst_base_knob_win32.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>

namespace MediaPlayer {
namespace {

constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kPlaylistDragScrollTimer = 2;
constexpr UINT kRemovePlaylistCommand = 3001;
using namespace EditorLayout;
using namespace EditorStyle::Colors;
using namespace EditorStyle::Behavior;
constexpr wchar_t kEditorProperty[] = L"DAWMediaPlayer.NativeEditor";

HWND asWindow(void* handle) noexcept
{
    return static_cast<HWND>(handle);
}

RECT nativeRect(const ControlBounds& bounds, int left = 0, int top = 0)
{
    return {left + bounds.x, top + bounds.y,
            left + bounds.x + bounds.width, top + bounds.y + bounds.height};
}

HFONT createEditorFont(const EditorStyle::Font& font)
{
    return CreateFontW(-font.pixels, 0, 0, 0, font.weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, font.family);
}

int seekPositionFor(std::uint64_t position, std::uint64_t duration) noexcept
{
    return duration == 0 ? 0 : static_cast<int>(std::min<std::uint64_t>(
        kSeekSteps, position * kSeekSteps / duration));
}

HBRUSH backgroundBrush(bool light = false)
{
    static HBRUSH dark = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kDark.background));
    static HBRUSH bright = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kLight.background));
    return light ? bright : dark;
}

HBRUSH panelBrush(bool light = false)
{
    static HBRUSH dark = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kDark.selection));
    static HBRUSH bright = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kLight.selection));
    return light ? bright : dark;
}

HBRUSH playlistBackgroundBrush(bool light = false)
{
    static HBRUSH dark = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kDark.surface));
    static HBRUSH bright = CreateSolidBrush(EditorStyle::Colors::native(vst_base::kLight.surface));
    return light ? bright : dark;
}

void setDefaultFont(HWND control)
{
    const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HFONT titleFont()
{
    static const auto font = createEditorFont(EditorStyle::Fonts::kTitle);
    return font;
}

HFONT statusFont()
{
    static const auto font = createEditorFont(EditorStyle::Fonts::kStatus);
    return font;
}

// Draw at the actual control size with subpixel coverage. GDI's integer
// Ellipse/Polyline/Polygon primitives produce stair steps at these small sizes.
Gdiplus::Color vectorColor(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void drawVectorTransport(HDC device, const RECT& bounds, EditorStyle::TransportIcon kind,
                         bool playing, bool pressed, const EditorStyle::Theme& colors)
{
    using namespace EditorStyle::Transport;
    using Icon = EditorStyle::TransportIcon;
    Gdiplus::Graphics graphics(device);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(bounds.left + bounds.right) / 2;
    const float y = static_cast<float>(bounds.top + bounds.bottom) / 2;
    Gdiplus::SolidBrush icon(vectorColor(kind == Icon::play && !pressed && colors.light ? EditorStyle::Colors::native(colors.palette().onActionText) : colors.kText));
    if (kind == Icon::play) {
        const float diameter = static_cast<float>(bounds.right - bounds.left) - kPlayFaceInset;
        Gdiplus::SolidBrush face(vectorColor(pressed ? colors.kPanel : colors.kAccent));
        graphics.FillEllipse(&face, x - diameter / 2, y - diameter / 2,
                             diameter, diameter);
        if (playing) {
            graphics.FillRectangle(&icon, x + kPauseLeft, y - kPauseHalfHeight,
                                   kPauseWidth, kPauseHalfHeight * 2);
            graphics.FillRectangle(&icon, x + kPauseRight, y - kPauseHalfHeight,
                                   kPauseWidth, kPauseHalfHeight * 2);
        } else {
            const Gdiplus::PointF points[] {{x + kPlayLeft, y - kPlayHalfHeight},
                {x + kPlayTip, y}, {x + kPlayLeft, y + kPlayHalfHeight}};
            graphics.FillPolygon(&icon, points, 3);
        }
    } else {
        if (pressed) {
            Gdiplus::SolidBrush face(vectorColor(colors.kPanel));
            graphics.FillEllipse(&face,
                x - kPressedFaceDiameter / 2, y - kPressedFaceDiameter / 2,
                kPressedFaceDiameter, kPressedFaceDiameter);
        }
        if (kind == Icon::stop) {
            graphics.FillRectangle(&icon, x - kStopSize / 2, y - kStopSize / 2,
                                   kStopSize, kStopSize);
        } else {
            const float direction = kind == Icon::previous ? -1.0f : 1.0f;
            const Gdiplus::PointF points[] {{x - direction * kSkipBase, y - kSkipHalfHeight},
                {x + direction * kSkipTip, y}, {x - direction * kSkipBase, y + kSkipHalfHeight}};
            graphics.FillPolygon(&icon, points, 3);
            graphics.FillRectangle(&icon,
                x + (kind == Icon::previous ? kPreviousBar : kNextBar),
                y - kSkipHalfHeight, kSkipBarWidth, kSkipHalfHeight * 2);
        }
    }
}

void drawVectorDial(HDC device, int centerX, int centerY, float value, const EditorStyle::Theme& colors)
{
    Gdiplus::Graphics graphics(device);
    vst_base::DrawGainKnob(graphics, static_cast<float>(centerX),
        static_cast<float>(centerY), value, colors.palette());
}
std::wstring formatTime(std::uint64_t frames, int sampleRate)
{
    if (sampleRate <= 0)
        return L"--:--";

    const auto totalSeconds = frames / static_cast<std::uint64_t>(sampleRate);
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds / 60) % 60;
    const auto seconds = totalSeconds % 60;

    std::wostringstream text;
    if (hours > 0)
        text << std::setfill(L'0') << std::setw(2) << hours << L":";
    text << std::setfill(L'0') << std::setw(2) << minutes << L":"
         << std::setfill(L'0') << std::setw(2) << seconds;
    return text.str();
}

std::wstring compactSingleLineText(HWND control, const std::wstring& text)
{
    if (control == nullptr || text.empty())
        return text;

    RECT client {};
    GetClientRect(control, &client);
    const auto availableWidth = std::max(
        1,
        static_cast<int>(client.right - client.left));
    const auto device = GetDC(control);
    if (device == nullptr)
        return text;

    const auto font = reinterpret_cast<HFONT>(SendMessageW(
        control, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = SelectObject(device, font);
    const auto textWidth = [&](const std::wstring& value) {
        SIZE size {};
        GetTextExtentPoint32W(device, value.c_str(),
                              static_cast<int>(value.size()), &size);
        return static_cast<int>(size.cx);
    };
    const auto fits = [&](const std::wstring& value) {
        return textWidth(value) <= availableWidth;
    };

    auto result = text;
    if (!fits(text)) {
        const std::wstring ellipsis = L"...";
        std::size_t low = 0;
        std::size_t high = text.size();
        std::size_t best = 0;
        while (low <= high) {
            const auto middle = low + (high - low) / 2;
            if (fits(text.substr(0, middle) + ellipsis)) {
                best = middle;
                low = middle + 1;
            } else {
                if (middle == 0)
                    break;
                high = middle - 1;
            }
        }
        result = text.substr(0, best) + ellipsis;
    }

    if (oldFont != nullptr)
        SelectObject(device, oldFont);
    ReleaseDC(control, device);
    return result;
}

std::wstring playlistDisplayName(HWND list,
                                 const std::filesystem::path& path)
{
    auto name = path.stem().wstring();
    if (name.empty())
        name = path.wstring();
    return compactSingleLineText(list, name);
}

void registerEditorClass()
{
    static std::once_flag registration;
    std::call_once(registration, [] {
        INITCOMMONCONTROLSEX commonControls {};
        commonControls.dwSize = sizeof(commonControls);
        commonControls.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&commonControls);

        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        // The editor lays out its child controls explicitly in WM_SIZE. Let
        // that layout own invalidation during a playlist resize instead of
        // erasing the complete client area for every intermediate size.
        windowClass.style = 0;
        windowClass.lpfnWndProc = reinterpret_cast<WNDPROC>(&NativeEditor::windowProc);
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = backgroundBrush();
        windowClass.lpszClassName = NativeEditor::kWindowClassName;
        RegisterClassExW(&windowClass);

        windowClass.lpfnWndProc = reinterpret_cast<WNDPROC>(&NativeEditor::progressProc);
        windowClass.hbrBackground = panelBrush();
        windowClass.lpszClassName = NativeEditor::kProgressClassName;
        RegisterClassExW(&windowClass);

        windowClass.lpfnWndProc = reinterpret_cast<WNDPROC>(&NativeEditor::volumeProc);
        windowClass.lpszClassName = NativeEditor::kVolumeClassName;
        RegisterClassExW(&windowClass);

        windowClass.lpfnWndProc = reinterpret_cast<WNDPROC>(&NativeEditor::playerPanelProc);
        windowClass.hbrBackground = backgroundBrush();
        windowClass.lpszClassName = NativeEditor::kPlayerPanelClassName;
        RegisterClassExW(&windowClass);

    });
}

} // namespace

class EditorDropTarget final : public IDropTarget {
public:
    explicit EditorDropTarget(NativeEditor* editor)
        : editor_(editor)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                              void** object) override
    {
        if (object == nullptr)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++referenceCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --referenceCount_;
        if (count == 0)
            delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject,
                                         DWORD,
                                         POINTL,
                                         DWORD* effect) override
    {
        fileDrag_ = hasFileDrop(dataObject);
        if (effect != nullptr)
            *effect = fileDrag_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD,
                                       POINTL,
                                       DWORD* effect) override
    {
        if (effect != nullptr)
            *effect = fileDrag_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        fileDrag_ = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject,
                                   DWORD,
                                   POINTL,
                                   DWORD* effect) override
    {
        if (effect != nullptr)
            *effect = DROPEFFECT_NONE;

        FORMATETC format {
            CF_HDROP,
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL,
        };
        STGMEDIUM medium {};
        if (!fileDrag_ || dataObject == nullptr ||
            dataObject->GetData(&format, &medium) != S_OK) {
            fileDrag_ = false;
            return S_OK;
        }

        if (editor_ != nullptr)
            editor_->handleDroppedFiles(medium.hGlobal, false);
        ReleaseStgMedium(&medium);
        fileDrag_ = false;
        if (effect != nullptr)
            *effect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    static bool hasFileDrop(IDataObject* dataObject)
    {
        if (dataObject == nullptr)
            return false;
        FORMATETC format {
            CF_HDROP,
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL,
        };
        return dataObject->QueryGetData(&format) == S_OK;
    }

    std::atomic<ULONG> referenceCount_ {1};
    NativeEditor* editor_ {nullptr};
    bool fileDrag_ {false};
};

NativeEditor::NativeEditor(Controller* controller, Steinberg::ViewRect* size)
    : Steinberg::Vst::EditorView(controller, size),
      videoEngine_(std::make_unique<VideoEngine>()),
      videoWindow_(std::make_unique<VideoWindow>())
{
    controller->setEditor(this);
    colors_.setLight(controller->isLightTheme());
    videoWindow_->setLightTheme(colors_.light);
    videoWindow_->setUserCloseHandler([this] {
        if (auto* currentController =
                static_cast<Controller*>(getController()))
            currentController->sendTransportMessage(kPauseMessage);
    });
    videoWindow_->setUserClickHandler([this] {
        if (videoEngine_ == nullptr)
            return;
        sendTransport(videoEngine_->isPlaying() ? kPauseMessage
                                                : kPlayMessage);
    });
    videoWindow_->setUserFileDropHandler([this](
        const std::filesystem::path& file) {
        if (openMediaFile(file))
            sendTransport(kPlayMessage);
    });
    videoWindow_->setUserSeekHandler([this](float normalized) {
        sendSeekFromPosition(static_cast<int>(std::lround(
            std::clamp(normalized, 0.0f, 1.0f) * static_cast<float>(kSeekSteps))));
    });
    videoWindow_->setUserWheelHandler([this](int delta) {
        adjustVolumeFromWheel(delta);
    });
    videoWindow_->setUserPlacementHandler(
        [this](const VideoWindowPlacement& placement) {
            if (auto* currentController =
                    static_cast<Controller*>(getController()))
                currentController->sendVideoPlacement(placement);
        });
}

NativeEditor::~NativeEditor()
{
    // A host should call removed(), but release resources safely even when it
    // releases an attached view directly. Never leave GDI+ cleanup to DllMain.
    if (window_ != nullptr || vectorDrawingToken_ != 0)
        removedFromParent();
    if (auto* controller = static_cast<Controller*>(getController()))
        controller->clearEditor(this);
}

Steinberg::tresult PLUGIN_API NativeEditor::isPlatformTypeSupported(
    Steinberg::FIDString type)
{
    return type != nullptr && Steinberg::FIDStringsEqual(type, Steinberg::kPlatformTypeHWND)
        ? Steinberg::kResultTrue
        : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API NativeEditor::attached(
    void* parent, Steinberg::FIDString type)
{
    if (parent == nullptr || isPlatformTypeSupported(type) != Steinberg::kResultTrue)
        return Steinberg::kResultFalse;

    // GDI+ must start and stop outside the loader lock. A static destructor
    // runs during FreeLibrary/DllMain and can deadlock the host on shutdown.
    if (vectorDrawingToken_ == 0) {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token {};
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            return Steinberg::kResultFalse;
        vectorDrawingToken_ = token;
    }
    const auto result = Steinberg::Vst::EditorView::attached(parent, type);
    if (result != Steinberg::kResultOk) {
        const auto token = vectorDrawingToken_;
        vectorDrawingToken_ = 0;
        Gdiplus::GdiplusShutdown(token);
    }
    return result;
}

Steinberg::tresult PLUGIN_API NativeEditor::onSize(Steinberg::ViewRect* newSize)
{
    if (newSize == nullptr)
        return Steinberg::kInvalidArgument;
    const auto result = Steinberg::Vst::EditorView::onSize(newSize);
    if (result == Steinberg::kResultTrue && newSize != nullptr) {
        if (playlistVisible_) {
            playlistHeight_ = std::max(
                newSize->getHeight() - kBaseEditorHeight - kPlaylistBottomMargin,
                kMinPlaylistHeight);
        }
        // The host owns the VST view size. The native editor child is resized
        // only after this VST3 callback, which keeps the host frame and the
        // plugin's internal geometry in the same state.
        synchronizeNativeWindowSize(*newSize);
    }
    return result;
}

Steinberg::tresult PLUGIN_API NativeEditor::canResize()
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API NativeEditor::checkSizeConstraint(
    Steinberg::ViewRect* size)
{
    if (size == nullptr)
        return Steinberg::kInvalidArgument;

    const auto width = std::max(kEditorWidth, size->getWidth());
    const auto height = playlistVisible_
        ? std::max(size->getHeight(),
                   kBaseEditorHeight + kMinPlaylistHeight + kPlaylistBottomMargin)
        : kBaseEditorHeight;
    size->right = size->left + width;
    size->bottom = size->top + height;
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API NativeEditor::setFrame(
    Steinberg::IPlugFrame* frame)
{
    const auto result = Steinberg::Vst::EditorView::setFrame(frame);
    // Hosts may supply the frame after attached() and state restoration.
    // Retry the pending geometry once resizeView becomes available.
    if (frame != nullptr && window_ != nullptr)
        requestEditorHeight(playlistVisible_
            ? kBaseEditorHeight + playlistHeight_ + kPlaylistBottomMargin
            : kBaseEditorHeight);
    return result;
}

void NativeEditor::attachedToParent()
{
    registerEditorClass();
    if (auto* controller = static_cast<Controller*>(getController()))
        controller->beginEditorStateSync();

    const auto parent = asWindow(systemWindow);
    window_ = CreateWindowExW(
        WS_EX_COMPOSITED,
        kWindowClassName,
        L"MediaPlayer VST",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        kEditorWidth,
        kBaseEditorHeight,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (window_ != nullptr) {
        // onSize may already have arrived before the native child existed.
        synchronizeNativeWindowSize(getRect());
        createControls();
        if (videoWindow_ != nullptr) {
            videoWindow_->create(window_, videoEngine_.get());
            VideoInfo info {};
            if (videoEngine_ != nullptr &&
                videoEngine_->currentVideoInfo(info))
                videoWindow_->applyVideoInfo(info);
        }
        DragAcceptFiles(asWindow(window_), TRUE);
        DragAcceptFiles(asWindow(playerPanel_), TRUE);
        const auto oleResult = OleInitialize(nullptr);
        if (SUCCEEDED(oleResult)) {
            oleInitialized_ = true;
            auto registerDropTarget = [this](HWND target,
                                              bool& registered) {
                if (target == nullptr)
                    return;
                auto* dropTarget = new EditorDropTarget(this);
                if (RegisterDragDrop(target, dropTarget) == S_OK)
                    registered = true;
                dropTarget->Release();
            };
            registerDropTarget(asWindow(window_), editorDropTargetRegistered_);
            registerDropTarget(asWindow(playerPanel_),
                               playerPanelDropTargetRegistered_);
            registerDropTarget(asWindow(playlistList_),
                               playlistDropTargetRegistered_);
        }
        SetTimer(asWindow(window_), kStatusTimer, kStatusIntervalMs, nullptr);
    }

    Steinberg::Vst::EditorView::attachedToParent();
}

void NativeEditor::removedFromParent()
{
    if (window_ != nullptr) {
        if (videoWindow_ != nullptr)
            videoWindow_->close();
        KillTimer(asWindow(window_), kStatusTimer);
        DragAcceptFiles(asWindow(window_), FALSE);
        if (editorDropTargetRegistered_) {
            RevokeDragDrop(asWindow(window_));
            editorDropTargetRegistered_ = false;
        }
        if (playerPanel_ != nullptr) {
            const auto panel = asWindow(playerPanel_);
            DragAcceptFiles(panel, FALSE);
            if (playerPanelDropTargetRegistered_) {
                RevokeDragDrop(panel);
                playerPanelDropTargetRegistered_ = false;
            }
        }
        if (playlistList_ != nullptr) {
            const auto list = asWindow(playlistList_);
            DragAcceptFiles(list, FALSE);
            if (playlistDropTargetRegistered_) {
                RevokeDragDrop(list);
                playlistDropTargetRegistered_ = false;
            }
            RemovePropW(list, kEditorProperty);
            if (playlistOriginalProc_ != nullptr) {
                SetWindowLongPtrW(
                    list,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(playlistOriginalProc_));
                playlistOriginalProc_ = nullptr;
            }
        }
        if (oleInitialized_) {
            OleUninitialize();
            oleInitialized_ = false;
        }
        DestroyWindow(asWindow(window_));
        window_ = nullptr;
        titleLabel_ = nullptr;
        statusLabel_ = nullptr;
        currentTimeLabel_ = nullptr;
        durationLabel_ = nullptr;
        seekBar_ = nullptr;
        volumeBar_ = nullptr;
        openButton_ = nullptr;
        previousButton_ = nullptr;
        playButton_ = nullptr;
        stopButton_ = nullptr;
        nextButton_ = nullptr;
        playerPanel_ = nullptr;
        themeButton_ = nullptr;
        playlistList_ = nullptr;
        playlistClearButton_ = nullptr;
        playlistToggleButton_ = nullptr;
        playlistVisible_ = false;
        playlistVisibilityOverridePending_ = false;
        playlistResizing_ = false;
        requestedEditorHeight_ = 0;
    }

    if (vectorDrawingToken_ != 0) {
        const auto token = vectorDrawingToken_;
        vectorDrawingToken_ = 0;
        Gdiplus::GdiplusShutdown(token);
    }
    Steinberg::Vst::EditorView::removedFromParent();
}

long long __stdcall NativeEditor::windowProc(void* rawWindow,
                                             unsigned int message,
                                             unsigned long long wParam,
                                             long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* editor = reinterpret_cast<NativeEditor*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<NativeEditor*>(create->lpCreateParams);
        editor->window_ = rawWindow;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(editor));
    }

    if (editor != nullptr)
        return editor->handleMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long __stdcall NativeEditor::progressProc(void* rawWindow,
                                               unsigned int message,
                                               unsigned long long wParam,
                                               long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* editor = reinterpret_cast<NativeEditor*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<NativeEditor*>(create->lpCreateParams);
        editor->seekBar_ = rawWindow;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(editor));
    }

    if (editor != nullptr)
        return editor->handleProgressMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long __stdcall NativeEditor::playlistProc(void* rawWindow,
                                               unsigned int message,
                                               unsigned long long wParam,
                                               long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* editor = reinterpret_cast<NativeEditor*>(
        GetPropW(window, kEditorProperty));
    if (editor != nullptr && message == WM_DROPFILES) {
        editor->handleDroppedFiles(
            reinterpret_cast<void*>(static_cast<LONG_PTR>(wParam)), true);
        return 0;
    }

    if (editor != nullptr) {
        if (message == WM_LBUTTONDOWN) {
            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            const auto hit = static_cast<DWORD>(SendMessageW(
                window, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y)));
            const auto index = static_cast<int>(LOWORD(hit));
            if (HIWORD(hit) == 0 && index >= 0 &&
                index < static_cast<int>(editor->playlistPaths_.size())) {
                // This custom drag handler consumes WM_LBUTTONDOWN instead of
                // letting the list box's default procedure run. Give the list
                // keyboard focus explicitly so its Delete-key handler can run.
                SetFocus(window);
                const auto shift = (wParam & MK_SHIFT) != 0;
                const auto control = (wParam & MK_CONTROL) != 0;
                if (shift) {
                    auto anchor = static_cast<int>(SendMessageW(
                        window, LB_GETANCHORINDEX, 0, 0));
                    if (anchor < 0 || anchor >= static_cast<int>(editor->playlistPaths_.size()))
                        anchor = index;
                    if (!control)
                        SendMessageW(window, LB_SETSEL, FALSE, -1);
                    SendMessageW(window, LB_SELITEMRANGEEX,
                                 std::min(anchor, index), std::max(anchor, index));
                    SendMessageW(window, LB_SETCARETINDEX, index, FALSE);
                } else if (control) {
                    const auto selected = SendMessageW(window, LB_GETSEL, index, 0) > 0;
                    SendMessageW(window, LB_SETSEL, !selected, index);
                    SendMessageW(window, LB_SETCARETINDEX, index, FALSE);
                    SendMessageW(window, LB_SETANCHORINDEX, index, 0);
                } else if (SendMessageW(window, LB_GETSEL, index, 0) <= 0) {
                    editor->setPlaylistSelection({index}, index);
                }
                editor->playlistPointerModifiers_ = static_cast<unsigned int>(
                    wParam & (MK_SHIFT | MK_CONTROL));
                editor->playlistPointerDown_ = true;
                editor->playlistDragging_ = false;
                editor->playlistDragSourceIndex_ = index;
                editor->playlistDragStartX_ = x;
                editor->playlistDragStartY_ = y;
                editor->playlistDropInsertionIndex_ = -1;
                SetCapture(window);
                return 0;
            }
        }

        if (message == WM_LBUTTONDBLCLK) {
            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            const auto hit = static_cast<DWORD>(SendMessageW(
                window, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y)));
            const auto index = static_cast<int>(LOWORD(hit));
            if (HIWORD(hit) == 0 && index >= 0 &&
                index < static_cast<int>(editor->playlistPaths_.size())) {
                editor->playlistPointerDown_ = false;
                editor->playlistDragging_ = false;
                editor->playlistDragSourceIndex_ = -1;
                editor->playlistDropInsertionIndex_ = -1;
                KillTimer(window, kPlaylistDragScrollTimer);
                editor->setPlaylistSelection({index}, index);
                editor->selectPlaylistFromControl(true);
                editor->ignorePlaylistMouseUp_ = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }

        if (message == WM_MOUSEMOVE && editor->playlistPointerDown_) {
            if ((wParam & MK_LBUTTON) == 0) {
                editor->playlistPointerDown_ = false;
                editor->playlistDragging_ = false;
                editor->playlistDragSourceIndex_ = -1;
                editor->playlistDropInsertionIndex_ = -1;
                KillTimer(window, kPlaylistDragScrollTimer);
                if (GetCapture() == window)
                    ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            if (!editor->playlistDragging_) {
                const auto horizontalDistance = std::abs(
                    x - editor->playlistDragStartX_);
                const auto verticalDistance = std::abs(
                    y - editor->playlistDragStartY_);
                if (horizontalDistance < GetSystemMetrics(SM_CXDRAG) &&
                    verticalDistance < GetSystemMetrics(SM_CYDRAG))
                    return 0;

                editor->playlistDragging_ = true;
                SetTimer(window, kPlaylistDragScrollTimer,
                         kDragScrollIntervalMs, nullptr);
            }

            auto insertionIndex = editor->playlistInsertionIndexAt(
                rawWindow, x, y);
            if (editor->playlistDropInsertionIndex_ != insertionIndex) {
                editor->playlistDropInsertionIndex_ = insertionIndex;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        if (message == WM_TIMER &&
            wParam == kPlaylistDragScrollTimer &&
            editor->playlistDragging_) {
            POINT cursor {};
            RECT client {};
            if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor) &&
                GetClientRect(window, &client) && cursor.x >= client.left &&
                cursor.x < client.right) {
                const auto topIndex = static_cast<int>(SendMessageW(
                    window, LB_GETTOPINDEX, 0, 0));
                const auto count = static_cast<int>(SendMessageW(
                    window, LB_GETCOUNT, 0, 0));
                auto nextTop = topIndex;
                if (cursor.y < client.top + kDragScrollEdge)
                    nextTop = std::max(0, topIndex - 1);
                else if (cursor.y >= client.bottom - kDragScrollEdge)
                    nextTop = std::min(std::max(0, count - 1), topIndex + 1);
                if (nextTop != topIndex)
                    SendMessageW(window, LB_SETTOPINDEX,
                                 static_cast<WPARAM>(nextTop), 0);

                auto insertionIndex = editor->playlistInsertionIndexAt(
                    rawWindow, cursor.x, cursor.y);
                if (editor->playlistDropInsertionIndex_ != insertionIndex) {
                    editor->playlistDropInsertionIndex_ = insertionIndex;
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            return 0;
        }

        if (message == WM_LBUTTONUP && editor->ignorePlaylistMouseUp_) {
            editor->ignorePlaylistMouseUp_ = false;
            return 0;
        }

        if (message == WM_LBUTTONUP && editor->playlistPointerDown_) {
            const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            const auto sourceIndex = editor->playlistDragSourceIndex_;
            const auto wasDragging = editor->playlistDragging_;
            auto insertionIndex = wasDragging
                ? editor->playlistInsertionIndexAt(rawWindow, x, y)
                : -1;

            editor->playlistPointerDown_ = false;
            editor->playlistDragging_ = false;
            editor->playlistDragSourceIndex_ = -1;
            editor->playlistDropInsertionIndex_ = -1;
            KillTimer(window, kPlaylistDragScrollTimer);
            if (GetCapture() == window)
                ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);

            if (wasDragging) {
                if (insertionIndex >= 0)
                    editor->moveSelectedPlaylistEntries(insertionIndex);
            } else if (sourceIndex >= 0 && editor->playlistPointerModifiers_ == 0) {
                editor->setPlaylistSelection({sourceIndex}, sourceIndex);
                if (sourceIndex != editor->currentPlaylistIndex_)
                    editor->selectPlaylistFromControl(false);
            }
            return 0;
        }

        if ((message == WM_CANCELMODE || message == WM_CAPTURECHANGED) &&
            editor->playlistPointerDown_) {
            editor->playlistPointerDown_ = false;
            editor->playlistDragging_ = false;
            editor->playlistDragSourceIndex_ = -1;
            editor->playlistDropInsertionIndex_ = -1;
            KillTimer(window, kPlaylistDragScrollTimer);
            InvalidateRect(window, nullptr, FALSE);
        }

        if (message == WM_KEYDOWN && wParam == VK_DELETE) {
            editor->removeSelectedPlaylistEntries();
            return 0;
        }

        if (message == WM_CONTEXTMENU) {
            int index = -1;
            if (lParam == -1) {
                index = static_cast<int>(SendMessageW(
                    window, LB_GETCARETINDEX, 0, 0));
            } else {
                POINT cursor {
                    static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<short>(HIWORD(lParam))),
                };
                ScreenToClient(window, &cursor);
                const auto hit = static_cast<DWORD>(SendMessageW(
                    window,
                    LB_ITEMFROMPOINT,
                    0,
                    MAKELPARAM(cursor.x, cursor.y)));
                if (HIWORD(hit) == 0)
                    index = static_cast<int>(LOWORD(hit));
            }

            if (index >= 0 &&
                index < static_cast<int>(editor->playlistPaths_.size())) {
                if (SendMessageW(window, LB_GETSEL, index, 0) <= 0)
                    editor->setPlaylistSelection({index}, index);
                editor->showPlaylistContextMenu(rawWindow, index, lParam);
                return 0;
            }
        }

    }

    if (editor != nullptr && editor->playlistOriginalProc_ != nullptr) {
        const auto result = CallWindowProcW(
            reinterpret_cast<WNDPROC>(editor->playlistOriginalProc_),
            window,
            message,
            static_cast<WPARAM>(wParam),
            lParam);
        switch (message) {
        case WM_NCPAINT:
        case WM_NCACTIVATE:
        case WM_MOUSEWHEEL:
        case WM_VSCROLL:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP:
        case WM_NCMOUSEMOVE:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SIZE:
        case LB_ADDSTRING:
        case LB_DELETESTRING:
        case LB_RESETCONTENT:
        case LB_SETTOPINDEX:
            editor->drawPlaylistScrollbar();
            break;
        default:
            break;
        }
        return result;
    }
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long __stdcall NativeEditor::volumeProc(void* rawWindow,
                                             unsigned int message,
                                             unsigned long long wParam,
                                             long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* editor = reinterpret_cast<NativeEditor*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<NativeEditor*>(create->lpCreateParams);
        editor->volumeBar_ = rawWindow;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(editor));
    }

    if (editor != nullptr)
        return editor->handleVolumeMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long __stdcall NativeEditor::playerPanelProc(void* rawWindow,
                                                   unsigned int message,
                                                   unsigned long long wParam,
                                                   long long lParam)
{
    const auto window = asWindow(rawWindow);
    auto* editor = reinterpret_cast<NativeEditor*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        editor = static_cast<NativeEditor*>(create->lpCreateParams);
        editor->playerPanel_ = rawWindow;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(editor));
    }

    if (editor != nullptr) {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint {};
            const auto device = BeginPaint(window, &paint);
            editor->drawPlayerPanel(device);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DROPFILES:
        case WM_COMMAND:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_MOUSEWHEEL:
            return editor->handleMessage(message, wParam, lParam);
        case WM_ERASEBKGND: {
            RECT client {};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     backgroundBrush(editor->colors_.light));
            return 1;
        }
        default:
            break;
        }
    }
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long NativeEditor::handleProgressMessage(unsigned int message,
                                              unsigned long long wParam,
                                              long long lParam)
{
    const auto window = asWindow(seekBar_);
    switch (message) {
    case WM_MOUSEWHEEL:
        adjustVolumeFromWheel(static_cast<int>(
            GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wParam))));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        const auto device = BeginPaint(window, &paint);
        RECT client {};
        GetClientRect(window, &client);
        const auto background = CreateSolidBrush(colors_.kBackground);
        FillRect(device, &client, background);
        DeleteObject(background);

        const auto width = std::max(
            1,
            static_cast<int>(client.right - client.left));
        const auto progressWidth = std::clamp(
            width * seekPosition_ / kSeekSteps, 0, width);
        const auto handleX = std::clamp(client.left + progressWidth,
                                        client.left + EditorStyle::Seek::kEdgeInset,
                                        client.right - EditorStyle::Seek::kEdgeInset);
        {
            Gdiplus::Graphics graphics(device);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float y = static_cast<float>(client.bottom) / 2;
            using namespace EditorStyle::Seek;
            Gdiplus::Pen track(vectorColor(colors_.kSeekTrack), kLineWidth);
            Gdiplus::Pen progress(vectorColor(colors_.kAccent), kLineWidth);
            graphics.DrawLine(&track, static_cast<float>(kEdgeInset), y,
                              static_cast<float>(width - kEdgeInset), y);
            graphics.DrawLine(&progress, static_cast<float>(kEdgeInset), y,
                              static_cast<float>(handleX), y);
            Gdiplus::SolidBrush handle(vectorColor(colors_.kAccentBright));
            graphics.FillEllipse(&handle, static_cast<float>(handleX) - kHandleRadius,
                                  y - kHandleRadius, kHandleDiameter, kHandleDiameter);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
        const auto x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        RECT client {};
        GetClientRect(window, &client);
        const auto width = std::max(
            1,
            static_cast<int>(client.right - client.left));
        const auto position = std::clamp((x * kSeekSteps) / width, 0, kSeekSteps);

        if (message == WM_LBUTTONDOWN ||
            (message == WM_MOUSEMOVE && (wParam & MK_LBUTTON) != 0)) {
            draggingSeek_ = true;
            sendSeekFromPosition(position);
            SetCapture(window);
        } else if (message == WM_LBUTTONUP) {
            draggingSeek_ = false;
            sendSeekFromPosition(position);
            ReleaseCapture();
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long NativeEditor::handleVolumeMessage(unsigned int message,
                                            unsigned long long wParam,
                                            long long lParam)
{
    const auto window = asWindow(volumeBar_);
    switch (message) {
    case WM_MOUSEWHEEL:
        adjustVolumeFromWheel(static_cast<int>(
            GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wParam))));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        const auto device = BeginPaint(window, &paint);
        RECT client {};
        GetClientRect(window, &client);
        const auto width = std::max(1, static_cast<int>(client.right - client.left));
        const auto height = std::max(1, static_cast<int>(client.bottom - client.top));
        const auto background = CreateSolidBrush(colors_.kBackground);
        FillRect(device, &client, background);
        DeleteObject(background);

        const auto centerX = width / 2;
        const auto centerY = std::max(static_cast<int>(vst_base::kKnobStyle.gainDiameter / 2),
                                     (height - Player::kVolumeLabelAreaHeight) / 2);
        const auto normalized = std::clamp(volume_, 0.0f, 1.0f);
        drawVectorDial(device, centerX, centerY, normalized, colors_);

        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, colors_.kMutedText);
        const auto dialFont = createEditorFont(EditorStyle::Fonts::kVolume);
        const auto oldFont = SelectObject(device, dialFont);
        wchar_t value[16] {};
        swprintf_s(value, L"VOL %d%%", static_cast<int>(std::lround(
                                       normalized * 100.0f)));
        RECT valueRect {0, height - Player::kVolumeLabelAreaHeight,
                        width, height - Player::kVolumeLabelBottomInset};
        DrawTextW(device, value, -1, &valueRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(device, oldFont);
        DeleteObject(dialFont);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        lastVolumeDragY_ = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        draggingVolume_ = true;
        SetCapture(window);
        return 0;
    case WM_MOUSEMOVE:
        if (draggingVolume_ && GetCapture() == window &&
            (wParam & MK_LBUTTON) != 0) {
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            dragVolumeVertically(lastVolumeDragY_ - y);
            lastVolumeDragY_ = y;
        }
        return 0;
    case WM_LBUTTONUP:
        if (draggingVolume_) {
            const auto y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
            dragVolumeVertically(lastVolumeDragY_ - y);
            draggingVolume_ = false;
            if (GetCapture() == window)
                ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        draggingVolume_ = false;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window, message, static_cast<WPARAM>(wParam), lParam);
}

long long NativeEditor::handleMessage(unsigned int message,
                                      unsigned long long wParam,
                                      long long lParam)
{
    // The bottom strip belongs to the editor, not to a list item or its
    // non-client border. Screen coordinates remain stable while it resizes.
    if (message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ||
        message == WM_LBUTTONUP || message == WM_SETCURSOR) {
        POINT screen {};
        POINT client {};
        if (message == WM_SETCURSOR) {
            GetCursorPos(&screen);
            client = screen;
            ScreenToClient(asWindow(window_), &client);
        } else {
            // Use the event's location, not the possibly newer OS cursor.
            // A fast drag may already have left the 10px strip by the time
            // its queued mouse-down reaches this window.
            client = {static_cast<short>(LOWORD(lParam)),
                      static_cast<short>(HIWORD(lParam))};
            screen = client;
            ClientToScreen(asWindow(window_), &screen);
        }
        if (message == WM_LBUTTONDOWN && isPlaylistResizePoint(client.y)) {
            beginPlaylistResize(screen.y);
            return 0;
        }
        if (playlistResizing_ &&
            (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
            updatePlaylistResize(screen.y);
            if (message == WM_LBUTTONUP)
                endPlaylistResize();
            return 0;
        }
        if (message == WM_SETCURSOR &&
            (playlistResizing_ || isPlaylistResizePoint(client.y))) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
    }
    if (message == WM_CAPTURECHANGED && playlistResizing_)
        endPlaylistResize();
    switch (message) {
    case WM_MOUSEWHEEL:
        adjustVolumeFromWheel(static_cast<int>(
            GET_WHEEL_DELTA_WPARAM(static_cast<WPARAM>(wParam))));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint {};
        const auto window = BeginPaint(asWindow(window_), &paint);
        FillRect(window, &paint.rcPaint, backgroundBrush(colors_.light));
        if (playlistVisible_) {
            RECT client {};
            GetClientRect(asWindow(window_), &client);
            const auto layout = EditorLayout::calculate(client.right, playlistHeight_);
            const auto border = nativeRect(layout.playlistFrame);
            const auto brush = CreateSolidBrush(colors_.kPanelBorder);
            // The player panel owns the top edge at the shared boundary.
            RECT leftEdge {border.left, border.top, border.left + Playlist::kFrameWidth, border.bottom};
            RECT rightEdge {border.right - Playlist::kFrameWidth, border.top, border.right, border.bottom};
            RECT bottomEdge {border.left, border.bottom - Playlist::kFrameWidth, border.right, border.bottom};
            FillRect(window, &leftEdge, brush);
            FillRect(window, &rightEdge, brush);
            FillRect(window, &bottomEdge, brush);
            const auto grip = nativeRect(EditorLayout::calculateResizeGrip(
                client.right, client.bottom));
            FillRect(window, &grip, brush);
            DeleteObject(brush);
        }
        EndPaint(asWindow(window_), &paint);
        return 0;
    }
    case WM_SIZE:
        resizeControls();
        return 0;
    case WM_DROPFILES:
        handleDroppedFiles(
            reinterpret_cast<void*>(static_cast<LONG_PTR>(wParam)), true);
        return 0;
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            if (auto* controller = static_cast<Controller*>(getController()))
                controller->requestPlaybackStatus();
            if (videoEngine_ != nullptr && videoWindow_ != nullptr) {
                VideoInfo info {};
                while (videoEngine_->consumeVideoInfo(info)) {
                    videoAvailable_ = info.hasVideo;
                    videoWindow_->applyVideoInfo(info);
                }
                if (videoAvailable_ && videoEngine_->isPlaying())
                    videoWindow_->showForPlayback();
                if (videoRequestPending_ && !audioLoaded_ && videoAvailable_) {
                    const auto duration = videoEngine_->durationSeconds();
                    const auto position = videoEngine_->currentPositionSeconds();
                    durationFrames_ = static_cast<std::uint64_t>(std::max(
                        0.0,
                        duration * static_cast<double>(sampleRate_)));
                    positionFrames_ = static_cast<std::uint64_t>(std::clamp(
                        position * static_cast<double>(sampleRate_),
                        0.0,
                        static_cast<double>(durationFrames_)));
                    setPlaybackState(videoEngine_->isPlaying());
                    if (!draggingSeek_) {
                        seekPosition_ = seekPositionFor(positionFrames_, durationFrames_);
                        if (seekBar_ != nullptr)
                            InvalidateRect(asWindow(seekBar_), nullptr, FALSE);
                    }
                    updateTimeLabels();
                }
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        const auto controlId = LOWORD(wParam);
        const auto notification = HIWORD(wParam);
        const auto control = reinterpret_cast<HWND>(static_cast<LONG_PTR>(lParam));
        if (control == asWindow(playlistList_)) {
            if (notification == LBN_SELCHANGE) {
                if (SendMessageW(control, LB_GETSELCOUNT, 0, 0) == 1 &&
                    (GetKeyState(VK_SHIFT) & 0x8000) == 0 &&
                    (GetKeyState(VK_CONTROL) & 0x8000) == 0)
                    selectPlaylistFromControl(false);
                return 0;
            }
            if (notification == LBN_DBLCLK) {
                selectPlaylistFromControl(true);
                return 0;
            }
        }
        if (notification != BN_CLICKED)
            break;

        switch (controlId) {
        case kThemeButton:
            setLightTheme(!colors_.light);
            return 0;
        case kOpenButton:
            chooseFile();
            return 0;
        case kPreviousButton:
            selectAdjacentPlaylist(-1);
            return 0;
        case kPlayButton:
            sendTransport(playing_ ? kPauseMessage : kPlayMessage);
            return 0;
        case kStopButton:
            sendTransport(kStopMessage);
            return 0;
        case kNextButton:
            selectAdjacentPlaylist(1);
            return 0;
        case kPlaylistToggleButton:
            togglePlaylistVisibility();
            return 0;
        case kClearPlaylistButton:
            if (auto* controller = static_cast<Controller*>(getController()))
                controller->sendClearPlaylist();
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_DRAWITEM:
        if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            item != nullptr && item->CtlType == ODT_LISTBOX &&
            item->CtlID == kPlaylistList) {
            drawPlaylistItem(item);
        } else {
            drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        }
        return 1;
    case WM_MEASUREITEM:
        if (auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            item != nullptr && item->CtlType == ODT_LISTBOX &&
            item->CtlID == kPlaylistList) {
            item->itemHeight = kPlaylistItemHeight;
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto device = reinterpret_cast<HDC>(wParam);
        const auto control = reinterpret_cast<HWND>(static_cast<LONG_PTR>(lParam));
        const auto listBox = message == WM_CTLCOLORLISTBOX;
        SetTextColor(device,
                     listBox || control == asWindow(titleLabel_)
                         ? colors_.kText
                         : colors_.kMutedText);
        SetBkColor(device, listBox ? colors_.kPlaylistBackground : colors_.kBackground);
        SetBkMode(device, TRANSPARENT);
        return reinterpret_cast<long long>(listBox ? playlistBackgroundBrush(colors_.light)
                                                   : backgroundBrush(colors_.light));
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }

    return DefWindowProcW(asWindow(window_),
                          message,
                          static_cast<WPARAM>(wParam),
                          lParam);
}

void NativeEditor::createControls()
{
    const auto parent = asWindow(window_);
    const auto instance = GetModuleHandleW(nullptr);
    const auto layout = EditorLayout::calculate(
        kEditorWidth, playlistHeight_);

    playerPanel_ = CreateWindowExW(
        0, kPlayerPanelClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        layout.playerPanel.x,
        layout.playerPanel.y,
        layout.playerPanel.width,
        layout.playerPanel.height,
        parent, nullptr, instance, this);
    const auto playerParent = asWindow(playerPanel_);

    auto createStatic = [&](const wchar_t* text,
                            const ControlBounds& bounds,
                            DWORD style = WS_CHILD | WS_VISIBLE) {
        const auto control = CreateWindowExW(
            0, L"STATIC", text, style,
            bounds.x, bounds.y, bounds.width, bounds.height,
            playerParent, nullptr, instance, nullptr);
        setDefaultFont(control);
        return control;
    };

    const auto singleLineStyle = WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP;
    titleLabel_ = createStatic(L"", layout.title, singleLineStyle);
    statusLabel_ = createStatic(L"", layout.status, singleLineStyle);
    SendMessageW(asWindow(titleLabel_), WM_SETFONT,
                 reinterpret_cast<WPARAM>(titleFont()), TRUE);
    SendMessageW(asWindow(statusLabel_), WM_SETFONT,
                 reinterpret_cast<WPARAM>(statusFont()), TRUE);

    seekBar_ = CreateWindowExW(
        0, kProgressClassName, L"",
        WS_CHILD | WS_VISIBLE,
        layout.seekBar.x,
        layout.seekBar.y,
        layout.seekBar.width,
        layout.seekBar.height,
        playerParent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSeekBar)), instance, this);

    currentTimeLabel_ = createStatic(L"00:00", layout.currentTime);
    durationLabel_ = createStatic(L"--:--", layout.duration,
                                  WS_CHILD | WS_VISIBLE | SS_RIGHT);
    SendMessageW(asWindow(currentTimeLabel_), WM_SETFONT,
                 reinterpret_cast<WPARAM>(statusFont()), TRUE);
    SendMessageW(asWindow(durationLabel_), WM_SETFONT,
                 reinterpret_cast<WPARAM>(statusFont()), TRUE);

    auto createButton = [&](const wchar_t* text, int id,
                            const ControlBounds& bounds,
                            HWND buttonParent) {
        const auto control = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            bounds.x, bounds.y, bounds.width, bounds.height,
            buttonParent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        setDefaultFont(control);
        return control;
    };

    themeButton_ = createButton(L"Dark", kThemeButton,
        {kEditorWidth - kContentMargin - Player::kThemeWidth, Player::kThemeTop, Player::kThemeWidth, Player::kThemeHeight}, playerParent);
    SetWindowTextW(asWindow(themeButton_), colors_.light ? L"Light" : L"Dark");
    openButton_ = createButton(L"Open", kOpenButton,
                               layout.openButton, playerParent);
    previousButton_ = createButton(L"", kPreviousButton,
                                   layout.previousButton, playerParent);
    playButton_ = createButton(L"", kPlayButton,
                               layout.playButton, playerParent);
    stopButton_ = createButton(L"", kStopButton,
                               layout.stopButton, playerParent);
    nextButton_ = createButton(L"", kNextButton,
                               layout.nextButton, playerParent);

    volumeBar_ = CreateWindowExW(
        0, kVolumeClassName, L"",
        WS_CHILD | WS_VISIBLE,
        layout.volumeBar.x,
        layout.volumeBar.y,
        layout.volumeBar.width,
        layout.volumeBar.height,
        playerParent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVolumeBar)), instance, this);

    playlistToggleButton_ = createButton(
        L"Playlist", kPlaylistToggleButton, layout.playlistToggle,
        playerParent);

    playlistClearButton_ = createButton(L"Clear", kClearPlaylistButton,
                                        layout.playlistClear, playerParent);

    playlistList_ = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL |
            LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_EXTENDEDSEL,
        layout.playlistList.x,
        layout.playlistList.y,
        layout.playlistList.width,
        layout.playlistList.height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlaylistList)),
        instance, nullptr);
    setDefaultFont(asWindow(playlistList_));
    const auto list = asWindow(playlistList_);
    SetPropW(list, kEditorProperty, reinterpret_cast<HANDLE>(this));
    playlistOriginalProc_ = reinterpret_cast<void*>(SetWindowLongPtrW(
        list,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&NativeEditor::playlistProc)));
    DragAcceptFiles(list, TRUE);
    setPlaylistVisibility(playlistVisible_, false);
    refreshMediaLabels();
    updatePlaylist(playlistPaths_, currentPlaylistIndex_);
    updateTimeLabels();
}

void NativeEditor::resizeControls()
{
    if (window_ == nullptr)
        return;

    RECT client {};
    GetClientRect(asWindow(window_), &client);
    const auto rootWidth = std::max(
        1,
        static_cast<int>(client.right - client.left));
    const auto layout = EditorLayout::calculate(
        rootWidth, playlistHeight_);
    const auto widthChanged = laidOutWidth_ != layout.contentWidth;
    laidOutWidth_ = layout.contentWidth;

    const auto parent = asWindow(window_);
    const auto playerParent = asWindow(playerPanel_);
    auto move = [&](void* control, const ControlBounds& bounds,
                    HWND coordinateParent) {
        if (control == nullptr)
            return;

        const auto child = asWindow(control);
        RECT current {};
        GetWindowRect(child, &current);
        POINT currentPosition {current.left, current.top};
        MapWindowPoints(nullptr, coordinateParent, &currentPosition, 1);
        const auto currentWidth = static_cast<int>(current.right - current.left);
        const auto currentHeight = static_cast<int>(current.bottom - current.top);
        const auto changed = currentPosition.x != bounds.x ||
                             currentPosition.y != bounds.y ||
                             currentWidth != bounds.width ||
                             currentHeight != bounds.height;
        if (!changed)
            return;

        // Do not let each SetWindowPos issue its own intermediate paint. The
        // changed control is invalidated once after its final bounds are set.
        SetWindowPos(child, nullptr,
                     bounds.x,
                     bounds.y,
                     bounds.width,
                     bounds.height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        InvalidateRect(child, nullptr, FALSE);
    };

    move(playerPanel_, layout.playerPanel, parent);
    move(themeButton_, {rootWidth - kContentMargin - Player::kThemeWidth, Player::kThemeTop, Player::kThemeWidth, Player::kThemeHeight}, playerParent);
    move(titleLabel_, layout.title, playerParent);
    move(statusLabel_, layout.status, playerParent);
    move(seekBar_, layout.seekBar, playerParent);
    move(currentTimeLabel_, layout.currentTime, playerParent);
    move(durationLabel_, layout.duration, playerParent);
    move(openButton_, layout.openButton, playerParent);
    move(previousButton_, layout.previousButton, playerParent);
    move(playButton_, layout.playButton, playerParent);
    move(stopButton_, layout.stopButton, playerParent);
    move(nextButton_, layout.nextButton, playerParent);
    move(volumeBar_, layout.volumeBar, playerParent);
    move(playlistToggleButton_, layout.playlistToggle, playerParent);
    move(playlistClearButton_, layout.playlistClear, playerParent);
    // The list is a direct child of the editor root. The player panel and the
    // playlist therefore have separate geometry and visibility ownership.
    move(playlistList_, layout.playlistList, parent);

    // WM_SIZE can arrive after a host-side resize request. Visibility must
    // remain an explicit state, otherwise an old child window can remain
    // painted even though the playlist has been turned off.
    if (!playlistVisible_) {
        if (playlistList_ != nullptr)
            ShowWindow(asWindow(playlistList_), SW_HIDE);
        if (playlistClearButton_ != nullptr)
            ShowWindow(asWindow(playlistClearButton_), SW_HIDE);
    }
    if (widthChanged)
        refreshMediaLabels();
}

void NativeEditor::setLightTheme(bool light, bool notifyHost)
{
    colors_.setLight(light);
    if (auto* controller = static_cast<Controller*>(getController())) {
        controller->setLightTheme(light);
        if (notifyHost) controller->setDirty(true);
    }
    if (videoWindow_) videoWindow_->setLightTheme(light);
    if (themeButton_) SetWindowTextW(asWindow(themeButton_), light ? L"Light" : L"Dark");
    if (window_) {
        RedrawWindow(asWindow(window_), nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (playlistList_) RedrawWindow(asWindow(playlistList_), nullptr, nullptr,
            RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    }
}
void NativeEditor::chooseFile()
{
    wchar_t fileBuffer[32'768] {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = asWindow(window_);
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileBuffer));
    dialog.lpstrFilter =
        L"Media files\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.wmv;*.m4v;*.mpg;*.mpeg;*.ts;*.m2ts;*.flv;*.3gp;*.wav;*.mp3;*.flac;*.aac;*.m4a;*.ogg\0"
        L"All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) == FALSE)
        return;

    openMediaFile(std::filesystem::path(fileBuffer));
}

bool NativeEditor::openMediaFile(const std::filesystem::path& file)
{
    if (file.empty())
        return false;

    if (auto* controller = static_cast<Controller*>(getController())) {
        if (controller->sendOpenFile(file) == Steinberg::kResultOk)
        {
            requestVideoPath(file);
            return true;
        }
    }
    return false;
}

void NativeEditor::handleDroppedFiles(void* rawDrop, bool finishDrop)
{
    const auto drop = reinterpret_cast<HDROP>(rawDrop);
    if (drop == nullptr)
        return;

    const auto fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT index = 0; index < fileCount; ++index) {
        const auto characterCount = DragQueryFileW(drop, index, nullptr, 0);
        if (characterCount == 0)
            continue;

        std::vector<wchar_t> buffer(static_cast<std::size_t>(characterCount) + 1);
        if (DragQueryFileW(drop,
                           index,
                           buffer.data(),
                           static_cast<UINT>(buffer.size())) == 0)
            continue;

        openMediaFile(std::filesystem::path(buffer.data()));
    }
    if (finishDrop)
        DragFinish(drop);
}

void NativeEditor::setPlaylistVisibility(bool visible, bool persist)
{
    playlistHeight_ = std::max(playlistHeight_, kMinPlaylistHeight);
    playlistVisible_ = visible;

    const auto requestedHeight = visible
        ? kBaseEditorHeight + playlistHeight_ + kPlaylistBottomMargin
        : kBaseEditorHeight;

    // The list and Clear button are independent children. Hide them before
    // asking the host to shrink the VST view, so no playlist child can paint
    // in the old area during the host resize handshake.
    if (!visible) {
        if (playlistList_ != nullptr)
            ShowWindow(asWindow(playlistList_), SW_HIDE);
        if (playlistClearButton_ != nullptr)
            ShowWindow(asWindow(playlistClearButton_), SW_HIDE);
    }

    requestEditorHeight(requestedHeight);
    resizeControls();

    if (visible) {
        if (playlistList_ != nullptr)
            ShowWindow(asWindow(playlistList_), SW_SHOW);
        if (playlistClearButton_ != nullptr)
            ShowWindow(asWindow(playlistClearButton_), SW_SHOW);
    }
    if (playlistToggleButton_ != nullptr)
        ShowWindow(asWindow(playlistToggleButton_), SW_SHOW);
    if (playerPanel_ != nullptr)
        InvalidateRect(asWindow(playerPanel_), nullptr, FALSE);
    if (persist)
        sendEditorLayoutState();
}

void NativeEditor::togglePlaylistVisibility()
{
    const auto requestedVisibility = !playlistVisible_;
    playlistVisibilityOverride_ = requestedVisibility;
    playlistVisibilityOverridePending_ = true;
    setPlaylistVisibility(requestedVisibility);
}

void NativeEditor::requestEditorHeight(int height)
{
    height = std::max(height, kBaseEditorHeight);
    const auto targetChanged = requestedEditorHeight_ != height;
    requestedEditorHeight_ = height;

    const auto& current = getRect();
    const auto currentWidth = std::max(
        1,
        static_cast<int>(current.right - current.left));
    Steinberg::ViewRect desired = current;
    if (currentWidth <= 1)
        desired.right = desired.left + kEditorWidth;
    desired.bottom = desired.top + height;

    const auto logicalHeight = static_cast<int>(current.bottom - current.top);
    const auto logicalSizeNeedsResize = logicalHeight != height;

    // getRect() is the VST view's logical size. Also check the host-owned
    // parent: some hosts update one representation before the other. This
    // makes a hide/show request reach the host even when getRect() already
    // contains the requested height.
    RECT hostClient {};
    const auto hasHostSize = systemWindow != nullptr &&
                             GetClientRect(asWindow(systemWindow), &hostClient);
    const auto hostHeight = hasHostSize
        ? static_cast<int>(hostClient.bottom - hostClient.top)
        : 0;
    const auto hostSizeNeedsResize = !hasHostSize || hostHeight != height;

    if ((targetChanged || logicalSizeNeedsResize || hostSizeNeedsResize) &&
        plugFrame != nullptr)
        plugFrame->resizeView(this, &desired);
}

void NativeEditor::synchronizeNativeWindowSize(
    const Steinberg::ViewRect& size)
{
    if (window_ == nullptr)
        return;

    const auto width = std::max(1, static_cast<int>(size.right - size.left));
    const auto height = std::max(1, static_cast<int>(size.bottom - size.top));
    RECT client {};
    GetClientRect(asWindow(window_), &client);
    const auto currentWidth = static_cast<int>(client.right - client.left);
    const auto currentHeight = static_cast<int>(client.bottom - client.top);
    if (currentWidth == width && currentHeight == height) {
        resizeControls();
        return;
    }

    SetWindowPos(asWindow(window_), nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    // WM_SIZE lays out children synchronously. Repaint the complete hierarchy
    // only after that layout, including areas exposed by moved controls.
    RedrawWindow(asWindow(window_), nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

bool NativeEditor::isPlaylistResizePoint(int y) const
{
    if (!playlistVisible_ || playlistList_ == nullptr)
        return false;

    RECT client {};
    GetClientRect(asWindow(window_), &client);
    return y >= client.bottom - kPlaylistBottomMargin && y < client.bottom;
}

void NativeEditor::beginPlaylistResize(int y)
{
    playlistResizing_ = true;
    playlistResizeStartY_ = y;
    playlistResizeStartHeight_ = playlistHeight_;
    SetCapture(asWindow(window_));
    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
}

void NativeEditor::updatePlaylistResize(int y)
{
    if (!playlistResizing_)
        return;

    playlistHeight_ = std::max(
        playlistResizeStartHeight_ + (y - playlistResizeStartY_),
        kMinPlaylistHeight);
    requestEditorHeight(kBaseEditorHeight + playlistHeight_ +
                        kPlaylistBottomMargin);
}

void NativeEditor::endPlaylistResize()
{
    if (!playlistResizing_)
        return;
    playlistResizing_ = false;
    if (GetCapture() == asWindow(window_))
        ReleaseCapture();
    sendEditorLayoutState();
}

void NativeEditor::sendEditorLayoutState()
{
    if (auto* controller = static_cast<Controller*>(getController())) {
        EditorLayoutState layout {};
        layout.playlistVisible = playlistVisible_;
        layout.playlistHeight = playlistHeight_;
        controller->sendEditorLayout(layout);
    }
}

void NativeEditor::refreshMediaLabels()
{
    if (titleLabel_ == nullptr || statusLabel_ == nullptr)
        return;

    const auto title = mediaLoaded_ && !mediaPath_.empty()
        ? mediaPath_.filename().wstring()
        : std::wstring(L"No media loaded");
    auto status = mediaLoaded_ && !mediaPath_.empty()
        ? mediaPath_.parent_path().wstring()
        : std::wstring(L"Open a media file, or drag and drop files here");
    if (mediaLoaded_ && !mediaPath_.empty() && status.empty())
        status = L".";
    SetWindowTextW(asWindow(titleLabel_),
                   compactSingleLineText(asWindow(titleLabel_), title).c_str());
    SetWindowTextW(asWindow(statusLabel_),
                   compactSingleLineText(asWindow(statusLabel_), status).c_str());
}

void NativeEditor::sendTransport(const char* messageId)
{
    if (auto* controller = static_cast<Controller*>(getController()))
        controller->sendTransportMessage(messageId);

    if (videoEngine_ == nullptr || messageId == nullptr)
        return;
    if (std::strcmp(messageId, kPlayMessage) == 0) {
        videoEngine_->requestPlay();
        if (videoWindow_ != nullptr)
            videoWindow_->showForPlayback();
    } else if (std::strcmp(messageId, kPauseMessage) == 0) {
        videoEngine_->requestPause();
    } else if (std::strcmp(messageId, kStopMessage) == 0) {
        videoEngine_->requestStop();
    }
}

void NativeEditor::sendSeekFromControl()
{
    sendSeekFromPosition(seekPosition_);
}

void NativeEditor::sendSeekFromPosition(int position)
{
    seekPosition_ = std::clamp(position, 0, kSeekSteps);
    const auto normalized = static_cast<float>(seekPosition_) / static_cast<float>(kSeekSteps);
    if (auto* controller = static_cast<Controller*>(getController()))
        controller->sendSeek(normalized);
    if (videoEngine_ != nullptr)
        videoEngine_->requestSeek(normalized);
    InvalidateRect(asWindow(seekBar_), nullptr, FALSE);
    updateTimeLabels();
}

void NativeEditor::dragVolumeVertically(int deltaY)
{
    if (deltaY == 0)
        return;
    applyVolumeState(
        volume_ + static_cast<float>(deltaY) * kVolumePerDragPixel, false);
}

void NativeEditor::adjustVolumeFromWheel(int delta)
{
    if (delta == 0)
        return;

    const auto normalizedDelta = kVolumePerWheelStep * static_cast<float>(delta) /
                                 static_cast<float>(WHEEL_DELTA);
    applyVolumeState(volume_ + normalizedDelta, true);
}

void NativeEditor::applyVolumeState(float normalizedVolume,
                                    bool showIndicator)
{
    volume_ = std::clamp(normalizedVolume, 0.0f, 1.0f);

    if (auto* controller = static_cast<Controller*>(getController()))
        controller->sendVolume(volume_);
    if (volumeBar_ != nullptr)
        InvalidateRect(asWindow(volumeBar_), nullptr, FALSE);
    if (showIndicator && videoWindow_ != nullptr)
        videoWindow_->showVolumeIndicator(volume_);
}

void NativeEditor::updateTimeLabels()
{
    if (currentTimeLabel_ == nullptr || durationLabel_ == nullptr)
        return;

    const auto previewPosition = draggingSeek_ && durationFrames_ > 0
        ? durationFrames_ * static_cast<std::uint64_t>(seekPosition_) / kSeekSteps
        : positionFrames_;
    const auto current = formatTime(previewPosition, sampleRate_);
    const auto duration = durationFrames_ == 0
        ? std::wstring(L"--:--")
        : formatTime(durationFrames_, sampleRate_);
    SetWindowTextW(asWindow(currentTimeLabel_), current.c_str());
    SetWindowTextW(asWindow(durationLabel_), duration.c_str());
}

void NativeEditor::updatePlaybackStatus(std::uint64_t positionFrames,
                                        std::uint64_t durationFrames,
                                        int sampleRate,
                                        bool playing,
                                        bool audioLoaded,
                                        float volume)
{
    if (audioLoaded && videoRequestPending_ &&
        !audioStatusMatchesMedia_)
        return;

    positionFrames_ = positionFrames;
    durationFrames_ = durationFrames;
    sampleRate_ = sampleRate > 0 ? sampleRate : 48'000;
    setPlaybackState(playing);
    audioLoaded_ = audioLoaded;
    if (audioLoaded_ && mediaPath_ == videoRequestedPath_)
        videoRequestPending_ = false;
    if (videoEngine_ != nullptr && audioLoaded_)
        videoEngine_->updateClock(positionFrames_, sampleRate_, playing_);
    volume_ = std::clamp(volume, 0.0f, 1.0f);

    if (!draggingSeek_ && seekBar_ != nullptr) {
        const auto position = seekPositionFor(positionFrames_, durationFrames_);
        seekPosition_ = position;
        InvalidateRect(asWindow(seekBar_), nullptr, FALSE);
    }
    if (volumeBar_ != nullptr)
        InvalidateRect(asWindow(volumeBar_), nullptr, FALSE);
    updateTimeLabels();
}

void NativeEditor::applyVideoPlacement(const VideoWindowPlacement& placement)
{
    if (videoWindow_ != nullptr)
        videoWindow_->applyRememberedPlacement(placement);
}

void NativeEditor::applyEditorLayout(const EditorLayoutState& layout)
{
    if (playlistVisibilityOverridePending_) {
        if (layout.playlistVisible != playlistVisibilityOverride_)
            return;
        playlistVisibilityOverridePending_ = false;
    }

    playlistHeight_ = std::max(
        static_cast<int>(layout.playlistHeight),
        kMinPlaylistHeight);
    setPlaylistVisibility(layout.playlistVisible, false);
}

void NativeEditor::requestVideoPath(const std::filesystem::path& file)
{
    if (file.empty())
        return;

    audioLoaded_ = false;
    videoRequestedPath_ = file;
    videoRequestPending_ = true;
    audioStatusMatchesMedia_ = false;
    videoAvailable_ = false;
    updateMediaPath(file);
    audioStatusMatchesMedia_ = false;
}

void NativeEditor::updateMediaPath(const std::filesystem::path& file)
{
    // Opening a file from the editor starts the video engine immediately.
    // Audio status messages can arrive later and may still describe the
    // previous file. Do not let such a stale message replace the path that
    // the user just selected, or the video window will be cleared again.
    if (videoRequestPending_ && !videoRequestedPath_.empty() &&
        !file.empty() && file != videoRequestedPath_) {
        audioStatusMatchesMedia_ = false;
        return;
    }

    audioStatusMatchesMedia_ = true;
    const auto changed = mediaPath_ != file || !mediaLoaded_;
    mediaPath_ = file;
    mediaLoaded_ = !mediaPath_.empty();
    if (mediaPath_ == videoRequestedPath_ && audioLoaded_)
        videoRequestPending_ = false;
    if (changed && videoEngine_ != nullptr) {
        videoAvailable_ = false;
        if (videoWindow_ != nullptr)
            videoWindow_->hide();
        videoEngine_->requestOpenFile(mediaPath_);
    }
    refreshMediaLabels();
}

bool NativeEditor::updatePlaylist(
    const std::vector<std::filesystem::path>& entries,
    std::int32_t currentIndex)
{
    const auto normalizedIndex = currentIndex >= 0 &&
                                         currentIndex <
                                             static_cast<std::int32_t>(entries.size())
        ? currentIndex
        : -1;
    const auto entriesChanged = entries != playlistPaths_;
    const auto currentChanged = normalizedIndex != currentPlaylistIndex_;
    if (playlistSelectionPending_ && normalizedIndex != currentPlaylistIndex_)
        return false;

    if (entriesChanged)
        playlistSelectionPending_ = false;
    else if (playlistSelectionPending_ &&
             normalizedIndex == currentPlaylistIndex_)
        playlistSelectionPending_ = false;

    playlistPaths_ = entries;
    currentPlaylistIndex_ = normalizedIndex;
    if (playerPanel_ != nullptr)
        InvalidateRect(asWindow(playerPanel_), nullptr, FALSE);
    if (playlistPaths_.empty()) {
        videoRequestPending_ = false;
        videoRequestedPath_.clear();
    }
    if (currentPlaylistIndex_ >= 0 &&
        currentPlaylistIndex_ < static_cast<std::int32_t>(playlistPaths_.size()) &&
        mediaPath_ != playlistPaths_[static_cast<std::size_t>(
            currentPlaylistIndex_)] && !playlistSelectionPending_ &&
        !videoRequestPending_) {
        requestVideoPath(playlistPaths_[static_cast<std::size_t>(
            currentPlaylistIndex_)]);
    }

    if (playlistList_ == nullptr)
        return true;

    const auto list = asWindow(playlistList_);
    const auto itemCount = static_cast<std::size_t>(SendMessageW(
        list, LB_GETCOUNT, 0, 0));
    const auto listNeedsRebuild = entriesChanged ||
                                  itemCount != playlistPaths_.size();
    if (listNeedsRebuild) {
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& path : playlistPaths_) {
            const auto name = playlistDisplayName(list, path);
            SendMessageW(list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    // Follow actual track changes, but leave an editing selection alone during
    // ordinary status polling. A reordered group stays selected at its new rows.
    if (listNeedsRebuild && !playlistSelectionAfterEdit_.empty()) {
        std::vector<std::int32_t> selected;
        for (std::size_t index = 0; index < playlistPaths_.size(); ++index) {
            if (std::find(playlistSelectionAfterEdit_.begin(),
                          playlistSelectionAfterEdit_.end(), playlistPaths_[index]) !=
                playlistSelectionAfterEdit_.end())
                selected.push_back(static_cast<std::int32_t>(index));
        }
        setPlaylistSelection(selected);
        playlistSelectionAfterEdit_.clear();
    } else if ((listNeedsRebuild || currentChanged) && !playlistPointerDown_) {
        setPlaylistSelection(currentPlaylistIndex_ >= 0
            ? std::vector<std::int32_t>{currentPlaylistIndex_}
            : std::vector<std::int32_t>{});
    }
    return true;
}

std::vector<std::int32_t> NativeEditor::selectedPlaylistIndices() const
{
    if (playlistList_ == nullptr)
        return {};
    const auto list = asWindow(playlistList_);
    const auto count = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
    if (count <= 0)
        return {};
    std::vector<std::int32_t> indices(static_cast<std::size_t>(count));
    const auto copied = SendMessageW(list, LB_GETSELITEMS, count,
                                    reinterpret_cast<LPARAM>(indices.data()));
    if (copied == LB_ERR)
        return {};
    indices.resize(static_cast<std::size_t>(copied));
    return indices;
}

void NativeEditor::setPlaylistSelection(
    const std::vector<std::int32_t>& indices, int caret)
{
    if (playlistList_ == nullptr)
        return;
    const auto list = asWindow(playlistList_);
    SendMessageW(list, LB_SETSEL, FALSE, -1);
    for (const auto index : indices)
        SendMessageW(list, LB_SETSEL, TRUE, index);
    if (caret < 0 && !indices.empty())
        caret = indices.front();
    if (caret >= 0) {
        SendMessageW(list, LB_SETCARETINDEX, caret, FALSE);
        SendMessageW(list, LB_SETANCHORINDEX, caret, 0);
    }
}

void NativeEditor::removeSelectedPlaylistEntries()
{
    const auto indices = selectedPlaylistIndices();
    if (indices.empty())
        return;
    if (auto* controller = static_cast<Controller*>(getController())) {
        if (controller->sendPlaylistSelectionEdit(indices) == Steinberg::kResultOk) {
            playlistSelectionPending_ = false;
            playlistSelectionAfterEdit_.clear();
        }
    }
}

int NativeEditor::playlistInsertionIndexAt(void* rawWindow,
                                           int x,
                                           int y) const
{
    const auto window = asWindow(rawWindow);
    if (window == nullptr)
        return -1;

    RECT client {};
    if (!GetClientRect(window, &client) || x < client.left ||
        x >= client.right || y < client.top || y >= client.bottom)
        return -1;

    const auto count = static_cast<int>(SendMessageW(
        window, LB_GETCOUNT, 0, 0));
    if (count <= 0)
        return 0;

    const auto hit = static_cast<DWORD>(SendMessageW(
        window, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y)));
    const auto index = static_cast<int>(LOWORD(hit));
    if (index < 0 || index >= count)
        return y < client.bottom / 2 ? 0 : count;

    RECT itemRect {};
    if (SendMessageW(window, LB_GETITEMRECT,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&itemRect)) == LB_ERR)
        return -1;
    if (y < itemRect.top)
        return index;
    if (y >= itemRect.bottom)
        return index + 1;
    return y < (itemRect.top + itemRect.bottom) / 2 ? index : index + 1;
}

void NativeEditor::moveSelectedPlaylistEntries(int insertionIndex)
{
    const auto indices = selectedPlaylistIndices();
    if (indices.empty() || insertionIndex < 0 ||
        insertionIndex > static_cast<int>(playlistPaths_.size()))
        return;
    // Dropping a contiguous group onto itself does not produce a new snapshot.
    // Do not leave a selection-restoration request waiting for an unrelated edit.
    if (indices.back() - indices.front() + 1 == static_cast<int>(indices.size()) &&
        insertionIndex >= indices.front() && insertionIndex <= indices.back() + 1)
        return;
    if (auto* controller = static_cast<Controller*>(getController())) {
        std::vector<std::filesystem::path> selectedPaths;
        for (const auto index : indices)
            selectedPaths.push_back(playlistPaths_[static_cast<std::size_t>(index)]);
        if (controller->sendPlaylistSelectionEdit(indices, insertionIndex) == Steinberg::kResultOk) {
            playlistSelectionAfterEdit_ = std::move(selectedPaths);
            playlistSelectionPending_ = false;
        }
    }
}

void NativeEditor::showPlaylistContextMenu(void* rawWindow,
                                           int index,
                                           long long rawPosition)
{
    const auto window = asWindow(rawWindow);
    if (window == nullptr || index < 0)
        return;

    POINT point {};
    if (rawPosition == -1) {
        RECT itemRect {};
        if (SendMessageW(window,
                         LB_GETITEMRECT,
                         static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
            point.x = (itemRect.left + itemRect.right) / 2;
            point.y = (itemRect.top + itemRect.bottom) / 2;
            ClientToScreen(window, &point);
        } else {
            GetCursorPos(&point);
        }
    } else {
        point.x = static_cast<LONG>(
            static_cast<short>(LOWORD(rawPosition)));
        point.y = static_cast<LONG>(
            static_cast<short>(HIWORD(rawPosition)));
    }

    const auto menu = CreatePopupMenu();
    if (menu == nullptr)
        return;
    AppendMenuW(menu,
                MF_STRING,
                kRemovePlaylistCommand,
                L"Remove from Playlist");
    const auto command = TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        point.x,
                                        point.y,
                                        0,
                                        window,
                                        nullptr);
    DestroyMenu(menu);

    if (command == kRemovePlaylistCommand)
        removeSelectedPlaylistEntries();
}

void NativeEditor::selectPlaylistFromControl(bool play)
{
    if (playlistList_ == nullptr)
        return;
    const auto index = static_cast<int>(SendMessageW(
        asWindow(playlistList_), LB_GETCARETINDEX, 0, 0));
    if (index < 0 || index >= static_cast<int>(playlistPaths_.size()))
        return;

    currentPlaylistIndex_ = index;
    playlistSelectionPending_ = true;
    const auto& selectedPath = playlistPaths_[static_cast<std::size_t>(index)];
    if (auto* controller = static_cast<Controller*>(getController())) {
        if (controller->sendSelectPlaylist(selectedPath, play) ==
            Steinberg::kResultOk) {
            requestVideoPath(selectedPath);
            if (videoEngine_ != nullptr) {
                if (play) {
                    videoEngine_->requestPlay();
                    if (videoWindow_ != nullptr)
                        videoWindow_->showForPlayback();
                } else {
                    videoEngine_->requestPause();
                }
            }
        }
    }
}

void NativeEditor::selectAdjacentPlaylist(int direction)
{
    if (playlistList_ == nullptr || playlistPaths_.empty() ||
        direction == 0)
        return;

    const auto currentIndex = currentPlaylistIndex_;
    const auto targetIndex = currentIndex < 0
        ? (direction < 0
               ? static_cast<int>(playlistPaths_.size()) - 1
               : 0)
        : currentIndex + (direction < 0 ? -1 : 1);
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(playlistPaths_.size()))
        return;

    setPlaylistSelection({targetIndex}, targetIndex);
    selectPlaylistFromControl(true);
}

void NativeEditor::clearMedia()
{
    const auto keepIndependentVideo =
        videoRequestPending_ && !videoRequestedPath_.empty() &&
        mediaPath_ == videoRequestedPath_;
    audioLoaded_ = false;
    audioStatusMatchesMedia_ = false;
    if (keepIndependentVideo) {
        mediaLoaded_ = true;
        positionFrames_ = 0;
        durationFrames_ = 0;
        setPlaybackState(false);
        seekPosition_ = 0;
        refreshMediaLabels();
        if (seekBar_ != nullptr)
            InvalidateRect(asWindow(seekBar_), nullptr, FALSE);
        updateTimeLabels();
        return;
    }
    videoRequestPending_ = false;
    videoRequestedPath_.clear();
    videoAvailable_ = false;
    if (videoEngine_ != nullptr)
        videoEngine_->requestClear();
    if (videoWindow_ != nullptr)
        videoWindow_->hide();
    mediaLoaded_ = false;
    mediaPath_.clear();
    positionFrames_ = 0;
    durationFrames_ = 0;
    setPlaybackState(false);
    seekPosition_ = 0;
    refreshMediaLabels();
    if (seekBar_ != nullptr)
        InvalidateRect(asWindow(seekBar_), nullptr, FALSE);
    updateTimeLabels();
}

void NativeEditor::setPlaybackState(bool playing)
{
    if (playing_ == playing)
        return;

    playing_ = playing;
    if (playButton_ != nullptr)
        InvalidateRect(asWindow(playButton_), nullptr, FALSE);
}

void NativeEditor::drawPlayerPanel(void* rawDevice) const
{
    const auto device = static_cast<HDC>(rawDevice);
    const auto panel = asWindow(playerPanel_);
    if (device == nullptr || panel == nullptr)
        return;

    RECT client {};
    GetClientRect(panel, &client);
    FillRect(device, &client, backgroundBrush(colors_.light));

    const auto layout = EditorLayout::calculate(client.right, playlistHeight_);
    const auto brandBand = nativeRect(layout.brandBand);
    const auto bandBrush = CreateSolidBrush(colors_.light ? EditorStyle::Colors::native(colors_.palette().header) : colors_.kBackground);
    FillRect(device, &brandBand, bandBrush);
    DeleteObject(bandBrush);

    SetBkMode(device, TRANSPARENT);
    const auto brandFont = createEditorFont(EditorStyle::Fonts::kBrand);
    const auto oldFont = SelectObject(device, brandFont);
    SetTextColor(device, colors_.kText);
    const wchar_t brandA[] = L"MediaPlayer ";
    const auto brandLength = static_cast<int>(std::size(brandA) - 1);
    TextOutW(device, kContentMargin, Player::kBrandTop, brandA, brandLength);
    SIZE brandSize {};
    GetTextExtentPoint32W(device, brandA, brandLength, &brandSize);
    SetTextColor(device, colors_.kAccentText);
    const wchar_t brandB[] = L"VST";
    TextOutW(device, kContentMargin + brandSize.cx, Player::kBrandTop,
             brandB, static_cast<int>(std::size(brandB) - 1));
    SelectObject(device, oldFont);
    DeleteObject(brandFont);

    if (playlistVisible_) {
        const auto topEdge = nativeRect({layout.playlistFrame.x,
            layout.playlistFrame.y, layout.playlistFrame.width, Playlist::kFrameWidth});
        const auto borderBrush = CreateSolidBrush(colors_.kPanelBorder);
        FillRect(device, &topEdge, borderBrush);
        DeleteObject(borderBrush);
    }

    if (playlistVisible_) {
        const auto countText = std::to_wstring(playlistPaths_.size()) +
                               (playlistPaths_.size() == 1
                                    ? L" TRACK"
                                    : L" TRACKS");
        const auto countFont = createEditorFont(EditorStyle::Fonts::kTrackCount);
        const auto oldCountFont = SelectObject(device, countFont);
        SetTextColor(device, colors_.kMutedText);
        auto countBounds = nativeRect(layout.playlistCount);
        DrawTextW(device, countText.c_str(), static_cast<int>(countText.size()),
                  &countBounds, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(device, oldCountFont);
        DeleteObject(countFont);
    }
}

void NativeEditor::drawPlaylistItem(const void* rawItem) const
{
    const auto* item = static_cast<const DRAWITEMSTRUCT*>(rawItem);
    if (item == nullptr || item->itemID == static_cast<UINT>(-1) ||
        item->itemID >= playlistPaths_.size())
        return;

    const auto selected = (item->itemState & ODS_SELECTED) != 0;
    const auto& path = playlistPaths_[item->itemID];
    const auto background = CreateSolidBrush(selected ? colors_.kPanel : colors_.kPlaylistBackground);
    FillRect(item->hDC, &item->rcItem, background);
    DeleteObject(background);

    if (selected) {
        RECT accent {item->rcItem.left, item->rcItem.top,
                     item->rcItem.left + EditorStyle::Playlist::kSelectionMarkWidth, item->rcItem.bottom};
        const auto accentBrush = CreateSolidBrush(colors_.kAccent);
        FillRect(item->hDC, &accent, accentBrush);
        DeleteObject(accentBrush);
    }

    const auto insertionIndex = playlistDropInsertionIndex_;
    if (insertionIndex == static_cast<int>(item->itemID) ||
        (insertionIndex == static_cast<int>(playlistPaths_.size()) &&
         item->itemID + 1 == playlistPaths_.size())) {
        const auto lineY = insertionIndex == static_cast<int>(item->itemID)
            ? item->rcItem.top
            : item->rcItem.bottom - 1;
        const auto dropPen = CreatePen(PS_SOLID, EditorStyle::Playlist::kDropLineWidth, colors_.kAccentBright);
        const auto oldDropPen = SelectObject(item->hDC, dropPen);
        MoveToEx(item->hDC, item->rcItem.left + EditorStyle::Playlist::kDropLineInset, lineY, nullptr);
        LineTo(item->hDC, item->rcItem.right - EditorStyle::Playlist::kDropLineInset, lineY);
        SelectObject(item->hDC, oldDropPen);
        DeleteObject(dropPen);
    }

    const auto separatorPen = CreatePen(PS_SOLID, EditorStyle::kBorderWidth, colors_.kPlaylistSeparator);
    const auto oldPen = SelectObject(item->hDC, separatorPen);
    MoveToEx(item->hDC, item->rcItem.left, item->rcItem.bottom - 1, nullptr);
    LineTo(item->hDC, item->rcItem.right, item->rcItem.bottom - 1);
    SelectObject(item->hDC, oldPen);
    DeleteObject(separatorPen);

    SetBkMode(item->hDC, TRANSPARENT);
    const auto rowFont = createEditorFont(EditorStyle::Fonts::kPlaylistRow);
    const auto oldFont = SelectObject(item->hDC, rowFont);
    const auto row = EditorLayout::calculatePlaylistRow(
        item->rcItem.right - item->rcItem.left, item->rcItem.bottom - item->rcItem.top);

    const auto indexText = std::to_wstring(item->itemID + 1);
    SetTextColor(item->hDC, selected && colors_.light ? colors_.kText : colors_.kMutedText);
    auto indexRect = nativeRect(row.index, item->rcItem.left, item->rcItem.top);
    DrawTextW(item->hDC, indexText.c_str(), -1, &indexRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const auto noteFont = createEditorFont(EditorStyle::Fonts::kPlaylistNote);
    const auto oldNoteFont = SelectObject(item->hDC, noteFont);
    SetTextColor(item->hDC, colors_.kAccentText);
    auto noteRect = nativeRect(row.note, item->rcItem.left, item->rcItem.top);
    DrawTextW(item->hDC, L"♪", -1, &noteRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, oldNoteFont);
    DeleteObject(noteFont);

    auto title = path.stem().wstring();
    if (title.empty())
        title = path.filename().wstring();
    SetTextColor(item->hDC, colors_.kText);
    auto titleRect = nativeRect(row.title, item->rcItem.left, item->rcItem.top);
    DrawTextW(item->hDC, title.c_str(), -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    const auto type = classifyMediaFile(path);
    const wchar_t* badgeText = type == MediaFileType::audio
        ? L"AUDIO"
        : type == MediaFileType::video ? L"VIDEO" : L"MEDIA";
    auto badge = nativeRect(row.badge, item->rcItem.left, item->rcItem.top);
    const auto badgeFont = createEditorFont(EditorStyle::Fonts::kPlaylistBadge);
    const auto oldBadgeFont = SelectObject(item->hDC, badgeFont);
    SetTextColor(item->hDC, selected && colors_.light ? colors_.kText : colors_.kMutedText);
    DrawTextW(item->hDC, badgeText, -1, &badge,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, oldBadgeFont);
    DeleteObject(badgeFont);

    SelectObject(item->hDC, oldFont);
    DeleteObject(rowFont);
}

void NativeEditor::drawPlaylistScrollbar() const
{
    const auto list = asWindow(playlistList_);
    if (list == nullptr)
        return;

    WINDOWINFO info {};
    info.cbSize = sizeof(info);
    if (!GetWindowInfo(list, &info))
        return;

    const RECT scrollbar {
        info.rcClient.right - info.rcWindow.left,
        info.rcClient.top - info.rcWindow.top,
        info.rcWindow.right - info.rcWindow.left,
        info.rcClient.bottom - info.rcWindow.top,
    };
    if (scrollbar.right <= scrollbar.left ||
        scrollbar.bottom <= scrollbar.top)
        return;

    const auto device = GetWindowDC(list);
    if (device == nullptr)
        return;

    const auto trackBrush = CreateSolidBrush(colors_.kBackground);
    FillRect(device, &scrollbar, trackBrush);
    DeleteObject(trackBrush);

    RECT client {};
    GetClientRect(list, &client);
    const auto count = static_cast<int>(SendMessageW(
        list, LB_GETCOUNT, 0, 0));
    const auto itemHeight = std::max(
        1, static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
    const auto visibleItems = std::max(
        1, static_cast<int>(client.bottom - client.top) / itemHeight);
    if (count > visibleItems) {
        RECT track {
            scrollbar.left + EditorStyle::Scrollbar::kHorizontalInset,
            scrollbar.top + EditorStyle::Scrollbar::kVerticalInset,
            scrollbar.right - EditorStyle::Scrollbar::kHorizontalInset,
            scrollbar.bottom - EditorStyle::Scrollbar::kVerticalInset,
        };
        const auto trackHeight = std::max(
            1, static_cast<int>(track.bottom - track.top));
        const auto thumbHeight = std::min(
            trackHeight,
            std::max(EditorStyle::Scrollbar::kMinimumThumbHeight, trackHeight * visibleItems / count));
        const auto maximumTopIndex = count - visibleItems;
        const auto topIndex = std::clamp(
            static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)),
            0,
            maximumTopIndex);
        const auto thumbTravel = trackHeight - thumbHeight;
        const auto thumbTop = track.top +
            (maximumTopIndex == 0
                 ? 0
                 : thumbTravel * topIndex / maximumTopIndex);
        RECT thumb {track.left, thumbTop, track.right,
                    thumbTop + thumbHeight};
        const auto thumbBrush = CreateSolidBrush(colors_.kPlaylistScrollThumb);
        const auto thumbPen = CreatePen(PS_SOLID, EditorStyle::kBorderWidth, colors_.kPlaylistScrollThumb);
        const auto oldBrush = SelectObject(device, thumbBrush);
        const auto oldPen = SelectObject(device, thumbPen);
        RoundRect(device, thumb.left, thumb.top, thumb.right, thumb.bottom,
                  EditorStyle::Scrollbar::kThumbCornerDiameter,
                  EditorStyle::Scrollbar::kThumbCornerDiameter);
        SelectObject(device, oldPen);
        SelectObject(device, oldBrush);
        DeleteObject(thumbPen);
        DeleteObject(thumbBrush);
    }

    ReleaseDC(list, device);
}

void NativeEditor::drawButton(const void* rawItem) const
{
    const auto* item = static_cast<const DRAWITEMSTRUCT*>(rawItem);
    if (item == nullptr)
        return;

    const auto selected = (item->itemState & ODS_SELECTED) != 0;
    const auto playButton = item->CtlID == kPlayButton;
    const auto openButton = item->CtlID == kOpenButton;
    const auto themeButton = item->CtlID == kThemeButton;
    const auto iconButton = item->CtlID == kPreviousButton ||
                            playButton || item->CtlID == kStopButton ||
                            item->CtlID == kNextButton;

    const auto baseBrush = CreateSolidBrush(themeButton && colors_.light
        ? EditorStyle::Colors::native(colors_.palette().header) : colors_.kBackground);
    FillRect(item->hDC, &item->rcItem, baseBrush);
    DeleteObject(baseBrush);

    if (iconButton) {
        using Icon = EditorStyle::TransportIcon;
        const auto kind = playButton ? Icon::play : item->CtlID == kStopButton ? Icon::stop
            : item->CtlID == kPreviousButton ? Icon::previous : Icon::next;
        drawVectorTransport(item->hDC, item->rcItem, kind, playing_, selected, colors_);
        return;
    } else if (themeButton) {
        // A separate, flat appearance for preferences; no operation-button border.
        const auto& palette = colors_.palette();
        const auto fill = selected ? colors_.kPanel : EditorStyle::Colors::native(
            colors_.light ? palette.background : palette.surface);
        const auto brush = CreateSolidBrush(fill);
        const auto oldBrush = SelectObject(item->hDC, brush);
        const auto oldPen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
        RoundRect(item->hDC, item->rcItem.left, item->rcItem.top,
            item->rcItem.right, item->rcItem.bottom, 4, 4);
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(brush);
    } else if (!openButton) {
        const auto fill = selected ? colors_.kPanel : colors_.light ? colors_.kPlaylistBackground : colors_.kBackground;
        const auto fillBrush = CreateSolidBrush(fill);
        const auto borderPen = CreatePen(PS_SOLID, EditorStyle::kBorderWidth, colors_.light && !selected ? colors_.kPlaylistScrollThumb : colors_.kAccentBright);
        const auto oldBrush = SelectObject(item->hDC, fillBrush);
        const auto oldPen = SelectObject(item->hDC, borderPen);
        RoundRect(item->hDC,
                  item->rcItem.left,
                  item->rcItem.top,
                  item->rcItem.right,
                  item->rcItem.bottom,
                  EditorStyle::kButtonCornerDiameter,
                  EditorStyle::kButtonCornerDiameter);
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(fillBrush);
    }

    if (openButton) {
        {
            Gdiplus::Graphics graphics(item->hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen outline(vectorColor(selected ? colors_.kAccentBright : colors_.kText),
                                 EditorStyle::Folder::kStrokeWidth);
            const float x = static_cast<float>(item->rcItem.left + item->rcItem.right) / 2;
            const float y = static_cast<float>(item->rcItem.top) + EditorStyle::Folder::kTop;
            Gdiplus::PointF folder[EditorStyle::Folder::kOutline.size()];
            for (std::size_t index = 0; index < std::size(folder); ++index) {
                const auto point = EditorStyle::Folder::kOutline[index];
                folder[index] = {x + point.x, y + point.y};
            }
            graphics.DrawLines(&outline, folder, static_cast<int>(std::size(folder)));
        }

        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, colors_.kMutedText);
        const auto font = createEditorFont(EditorStyle::Fonts::kOpenCaption);
        const auto oldFont = SelectObject(item->hDC, font);
        RECT label {item->rcItem.left, item->rcItem.bottom - Player::kOpenLabelAreaHeight,
                    item->rcItem.right, item->rcItem.bottom - Player::kOpenLabelBottomInset};
        DrawTextW(item->hDC, L"Open", -1, &label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item->hDC, oldFont);
        DeleteObject(font);
    } else {
        wchar_t text[32] {};
        GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, themeButton && !selected ? colors_.kMutedText : colors_.kText);
        const auto font = createEditorFont(EditorStyle::Fonts::kButton);
        const auto oldFont = SelectObject(item->hDC, font);
        RECT rect = item->rcItem;
        DrawTextW(item->hDC, text, -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, oldFont);
        DeleteObject(font);
    }

}

} // namespace MediaPlayer
