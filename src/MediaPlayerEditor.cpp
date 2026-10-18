#include "MediaPlayerEditor.h"

#include "MediaPlayerController.h"
#include "MediaPlayerEditorLayout.h"
#include "MediaPlayerFileTypes.h"
#include "MediaPlayerMessages.h"
#include "MediaPlayerVideo.h"

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
constexpr UINT kPlaylistDragScrollIntervalMs = 120;
constexpr int kPlaylistDragScrollEdge = 16;
using namespace EditorLayout;

// Reference palette: D:/DL/MPV.png. See COLOR_PALETTE.md for the full mapping.
constexpr COLORREF kBackground = RGB(12, 22, 35);
constexpr COLORREF kPlaylistBackground = RGB(18, 33, 48);
constexpr COLORREF kPanel = RGB(20, 56, 86);
constexpr COLORREF kPanelBorder = RGB(39, 73, 97);
constexpr COLORREF kPlaylistSeparator = RGB(37, 74, 99);
constexpr COLORREF kSeekTrack = RGB(28, 76, 108);
constexpr COLORREF kDialTrack = RGB(36, 68, 93);
constexpr COLORREF kText = RGB(231, 244, 251);
constexpr COLORREF kMutedText = RGB(119, 148, 168);
constexpr COLORREF kAccent = RGB(0, 178, 235);
constexpr COLORREF kAccentBright = RGB(6, 201, 255);
constexpr COLORREF kProgressBackground = RGB(12, 22, 35);
constexpr COLORREF kPlaylistScrollThumb = RGB(69, 111, 137);
constexpr float kVolumePerDragPixel = 0.005f;
constexpr wchar_t kEditorProperty[] = L"DAWMediaPlayer.NativeEditor";

HWND asWindow(void* handle) noexcept
{
    return static_cast<HWND>(handle);
}

HBRUSH backgroundBrush()
{
    static HBRUSH brush = CreateSolidBrush(kBackground);
    return brush;
}

HBRUSH panelBrush()
{
    static HBRUSH brush = CreateSolidBrush(kPanel);
    return brush;
}

HBRUSH playlistBackgroundBrush()
{
    static HBRUSH brush = CreateSolidBrush(kPlaylistBackground);
    return brush;
}

void setDefaultFont(HWND control)
{
    const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HFONT titleFont()
{
    static const auto font = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return font;
}

HFONT statusFont()
{
    static const auto font = CreateFontW(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return font;
}

// Draw at the actual control size with subpixel coverage. GDI's integer
// Ellipse/Polyline/Polygon primitives produce stair steps at these small sizes.
Gdiplus::Color vectorColor(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void drawVectorTransport(HDC device, const RECT& bounds, int kind,
                         bool playing, bool pressed)
{
    Gdiplus::Graphics graphics(device);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(bounds.left + bounds.right) / 2;
    const float y = static_cast<float>(bounds.top + bounds.bottom) / 2;
    Gdiplus::SolidBrush icon(vectorColor(kText));
    if (kind == 0) {
        const float diameter = static_cast<float>(bounds.right - bounds.left) - 3;
        Gdiplus::SolidBrush face(vectorColor(pressed ? kPanel : kAccent));
        graphics.FillEllipse(&face, x - diameter / 2, y - diameter / 2,
                             diameter, diameter);
        if (playing) {
            graphics.FillRectangle(&icon, x - 5.0f, y - 6.0f, 3.0f, 12.0f);
            graphics.FillRectangle(&icon, x + 2.0f, y - 6.0f, 3.0f, 12.0f);
        } else {
            const Gdiplus::PointF points[] {{x - 3.5f, y - 6},
                {x + 6, y}, {x - 3.5f, y + 6}};
            graphics.FillPolygon(&icon, points, 3);
        }
    } else {
        if (pressed) {
            Gdiplus::SolidBrush face(vectorColor(kPanel));
            graphics.FillEllipse(&face, x - 12, y - 12, 24.0f, 24.0f);
        }
        if (kind == 1) {
            graphics.FillRectangle(&icon, x - 4.5f, y - 4.5f, 9.0f, 9.0f);
        } else {
            const float direction = kind == 2 ? -1.0f : 1.0f;
            const Gdiplus::PointF points[] {{x - direction * 4, y - 5},
                {x + direction * 3, y}, {x - direction * 4, y + 5}};
            graphics.FillPolygon(&icon, points, 3);
            graphics.FillRectangle(&icon, kind == 2 ? x - 7 : x + 5,
                                    y - 5, 2.0f, 10.0f);
        }
    }
}

void drawVectorDial(HDC device, int centerX, int centerY, float value)
{
    Gdiplus::Graphics graphics(device);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(centerX);
    const float y = static_cast<float>(centerY);
    value = std::clamp(value, 0.0f, 1.0f);

    Gdiplus::SolidBrush shadow(Gdiplus::Color(90, 0, 0, 0));
    graphics.FillEllipse(&shadow, x - 17.0f, y - 16.2f, 34.0f, 34.0f);

    const Gdiplus::RectF shellBounds(x - 18.0f, y - 18.0f, 36.0f, 36.0f);
    Gdiplus::LinearGradientBrush shell(
        shellBounds,
        Gdiplus::Color(255, 82, 108, 128),
        Gdiplus::Color(255, 12, 24, 37),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillEllipse(&shell, x - 18.0f, y - 18.0f, 36.0f, 36.0f);
    Gdiplus::Pen shellEdge(vectorColor(kPanelBorder), 0.8f);
    graphics.DrawEllipse(&shellEdge, x - 17.5f, y - 17.5f, 35.0f, 35.0f);

    Gdiplus::Pen track(vectorColor(kDialTrack), 2.8f);
    Gdiplus::Pen active(vectorColor(kAccentBright), 2.8f);
    active.SetStartCap(Gdiplus::LineCapRound);
    active.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawArc(&track, x - 16.4f, y - 16.4f, 32.8f, 32.8f,
                     135.0f, 270.0f);
    if (value > 0)
        graphics.DrawArc(&active, x - 16.4f, y - 16.4f, 32.8f, 32.8f,
                         135.0f, 270.0f * value);

    const Gdiplus::RectF faceBounds(x - 13.2f, y - 13.2f, 26.4f, 26.4f);
    Gdiplus::LinearGradientBrush face(
        faceBounds,
        Gdiplus::Color(255, 34, 61, 82),
        Gdiplus::Color(255, 9, 18, 29),
        Gdiplus::LinearGradientModeVertical);
    graphics.FillEllipse(&face, faceBounds);
    Gdiplus::Pen faceEdge(vectorColor(kPanelBorder), 0.9f);
    graphics.DrawEllipse(&faceEdge, faceBounds);

    Gdiplus::Pen faceHighlight(Gdiplus::Color(125, 194, 221, 236), 0.75f);
    graphics.DrawArc(&faceHighlight, x - 12.1f, y - 12.1f, 24.2f, 24.2f,
                     205.0f, 130.0f);

    const auto angle = (135.0 + 270.0 * value) * 3.14159265358979323846 / 180.0;
    Gdiplus::Pen pointerShadow(vectorColor(kAccent), 2.8f);
    pointerShadow.SetStartCap(Gdiplus::LineCapRound);
    pointerShadow.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&pointerShadow,
        x + 2.8f * static_cast<float>(std::cos(angle)),
        y + 2.8f * static_cast<float>(std::sin(angle)),
        x + 9.8f * static_cast<float>(std::cos(angle)),
        y + 9.8f * static_cast<float>(std::sin(angle)));

    Gdiplus::Pen pointer(vectorColor(kText), 1.5f);
    pointer.SetStartCap(Gdiplus::LineCapRound);
    pointer.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&pointer,
        x + 2.8f * static_cast<float>(std::cos(angle)),
        y + 2.8f * static_cast<float>(std::sin(angle)),
        x + 9.8f * static_cast<float>(std::cos(angle)),
        y + 9.8f * static_cast<float>(std::sin(angle)));

    Gdiplus::SolidBrush pivot(vectorColor(kAccentBright));
    Gdiplus::SolidBrush pivotCore(vectorColor(kText));
    graphics.FillEllipse(&pivot, x - 2.1f, y - 2.1f, 4.2f, 4.2f);
    graphics.FillEllipse(&pivotCore, x - 0.8f, y - 0.8f, 1.6f, 1.6f);
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
            std::clamp(normalized, 0.0f, 1.0f) * 1000.0f)));
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
        SetTimer(asWindow(window_), kStatusTimer, 100, nullptr);
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
                SendMessageW(window, LB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
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
                         kPlaylistDragScrollIntervalMs, nullptr);
            }

            auto insertionIndex = editor->playlistInsertionIndexAt(
                rawWindow, x, y);
            const auto sourceIndex = editor->playlistDragSourceIndex_;
            if (insertionIndex == sourceIndex ||
                insertionIndex == sourceIndex + 1)
                insertionIndex = -1;
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
                if (cursor.y < client.top + kPlaylistDragScrollEdge)
                    nextTop = std::max(0, topIndex - 1);
                else if (cursor.y >= client.bottom - kPlaylistDragScrollEdge)
                    nextTop = std::min(std::max(0, count - 1), topIndex + 1);
                if (nextTop != topIndex)
                    SendMessageW(window, LB_SETTOPINDEX,
                                 static_cast<WPARAM>(nextTop), 0);

                auto insertionIndex = editor->playlistInsertionIndexAt(
                    rawWindow, cursor.x, cursor.y);
                const auto sourceIndex = editor->playlistDragSourceIndex_;
                if (insertionIndex == sourceIndex ||
                    insertionIndex == sourceIndex + 1)
                    insertionIndex = -1;
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
            if (insertionIndex == sourceIndex ||
                insertionIndex == sourceIndex + 1)
                insertionIndex = -1;

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
                    editor->movePlaylistEntry(sourceIndex, insertionIndex);
            } else if (sourceIndex >= 0 &&
                       SendMessageW(window, LB_GETCURSEL, 0, 0) !=
                           sourceIndex) {
                SendMessageW(window, LB_SETCURSEL,
                             static_cast<WPARAM>(sourceIndex), 0);
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
            const auto index = static_cast<int>(SendMessageW(
                window, LB_GETCURSEL, 0, 0));
            if (index >= 0) {
                editor->removePlaylistEntry(index);
                return 0;
            }
        }

        if (message == WM_CONTEXTMENU) {
            int index = -1;
            if (lParam == -1) {
                index = static_cast<int>(SendMessageW(
                    window, LB_GETCURSEL, 0, 0));
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
                SendMessageW(window, LB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
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
                     backgroundBrush());
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
        const auto background = CreateSolidBrush(kBackground);
        FillRect(device, &client, background);
        DeleteObject(background);

        const auto width = std::max(
            1,
            static_cast<int>(client.right - client.left));
        const auto progressWidth = std::clamp(
            width * seekPosition_ / 1000, 0, width);
        const auto handleX = std::clamp(client.left + progressWidth,
                                        client.left + 3,
                                        client.right - 3);
        {
            Gdiplus::Graphics graphics(device);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float y = static_cast<float>(client.bottom) / 2;
            Gdiplus::Pen track(vectorColor(kSeekTrack), 2.0f);
            Gdiplus::Pen progress(vectorColor(kAccent), 2.0f);
            graphics.DrawLine(&track, 3.0f, y, static_cast<float>(width - 3), y);
            graphics.DrawLine(&progress, 3.0f, y, static_cast<float>(handleX), y);
            Gdiplus::SolidBrush handle(vectorColor(kAccentBright));
            graphics.FillEllipse(&handle, static_cast<float>(handleX) - 2.5f,
                                  y - 2.5f, 5.0f, 5.0f);
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
        const auto position = std::clamp((x * 1000) / width, 0, 1000);

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
        const auto background = CreateSolidBrush(kBackground);
        FillRect(device, &client, background);
        DeleteObject(background);

        const auto centerX = width / 2;
        const auto centerY = std::max(18, (height - 16) / 2);
        const auto normalized = std::clamp(volume_, 0.0f, 1.0f);
        drawVectorDial(device, centerX, centerY, normalized);

        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kMutedText);
        const auto dialFont = CreateFontW(
            -10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto oldFont = SelectObject(device, dialFont);
        wchar_t value[16] {};
        swprintf_s(value, L"VOL %d%%", static_cast<int>(std::lround(
                                       normalized * 100.0f)));
        RECT valueRect {0, height - 16, width, height - 2};
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
        FillRect(window, &paint.rcPaint, backgroundBrush());
        if (playlistVisible_) {
            RECT client {};
            GetClientRect(asWindow(window_), &client);
            const auto layout = EditorLayout::calculate(client.right, playlistHeight_);
            RECT border {layout.playlistList.x - 1, layout.playlistList.y - 1,
                         layout.playlistList.x + layout.playlistList.width + 1,
                         layout.playlistList.y + layout.playlistList.height + 1};
            const auto brush = CreateSolidBrush(kPanelBorder);
            // The player panel owns the top edge at the shared boundary.
            RECT leftEdge {border.left, border.top, border.left + 1, border.bottom};
            RECT rightEdge {border.right - 1, border.top, border.right, border.bottom};
            RECT bottomEdge {border.left, border.bottom - 1, border.right, border.bottom};
            FillRect(window, &leftEdge, brush);
            FillRect(window, &rightEdge, brush);
            FillRect(window, &bottomEdge, brush);
            RECT grip {client.right / 2 - 18, client.bottom - 6,
                       client.right / 2 + 18, client.bottom - 4};
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
                        seekPosition_ = durationFrames_ == 0
                            ? 0
                            : static_cast<int>(std::min<std::uint64_t>(
                                  1000,
                                  positionFrames_ * 1000 / durationFrames_));
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
                         ? kText
                         : kMutedText);
        SetBkColor(device, listBox ? kPlaylistBackground : kBackground);
        SetBkMode(device, TRANSPARENT);
        return reinterpret_cast<long long>(listBox ? playlistBackgroundBrush()
                                                   : backgroundBrush());
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
            LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
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
    seekPosition_ = std::clamp(position, 0, 1000);
    const auto normalized = static_cast<float>(seekPosition_) / 1000.0f;
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

    const auto normalizedDelta = 0.05f * static_cast<float>(delta) /
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
        ? durationFrames_ * static_cast<std::uint64_t>(seekPosition_) / 1000
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
        const auto position = durationFrames_ == 0
            ? 0
            : static_cast<int>(std::min<std::uint64_t>(
                  1000, positionFrames_ * 1000 / durationFrames_));
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
        if (currentPlaylistIndex_ >= 0)
            SendMessageW(list, LB_SETCURSEL,
                         static_cast<WPARAM>(currentPlaylistIndex_), 0);
        else
            SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    return true;
}

void NativeEditor::removePlaylistEntry(int index)
{
    if (playlistList_ == nullptr ||
        index < 0 || index >= static_cast<int>(playlistPaths_.size()))
        return;

    if (auto* controller = static_cast<Controller*>(getController())) {
        if (controller->sendRemovePlaylist(index) == Steinberg::kResultOk)
            playlistSelectionPending_ = false;
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

void NativeEditor::movePlaylistEntry(int sourceIndex, int insertionIndex)
{
    if (sourceIndex < 0 ||
        sourceIndex >= static_cast<int>(playlistPaths_.size()) ||
        insertionIndex < 0 ||
        insertionIndex > static_cast<int>(playlistPaths_.size()))
        return;

    const auto targetIndex = insertionIndex > sourceIndex
        ? insertionIndex - 1
        : insertionIndex;
    if (targetIndex == sourceIndex)
        return;

    if (auto* controller = static_cast<Controller*>(getController()))
        controller->sendMovePlaylist(sourceIndex, targetIndex);
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
        removePlaylistEntry(index);
}

void NativeEditor::selectPlaylistFromControl(bool play)
{
    if (playlistList_ == nullptr)
        return;
    const auto index = static_cast<int>(SendMessageW(
        asWindow(playlistList_), LB_GETCURSEL, 0, 0));
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

    SendMessageW(asWindow(playlistList_),
                 LB_SETCURSEL,
                 static_cast<WPARAM>(targetIndex),
                 0);
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
    FillRect(device, &client, backgroundBrush());

    RECT brandBand {client.left, client.top, client.right, 28};
    const auto bandBrush = CreateSolidBrush(kBackground);
    FillRect(device, &brandBand, bandBrush);
    DeleteObject(bandBrush);

    SetBkMode(device, TRANSPARENT);
    const auto brandFont = CreateFontW(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const auto oldFont = SelectObject(device, brandFont);
    SetTextColor(device, kText);
    const wchar_t brandA[] = L"MediaPlayer ";
    TextOutW(device, 12, 8, brandA, 12);
    SIZE brandSize {};
    GetTextExtentPoint32W(device, brandA, 12, &brandSize);
    SetTextColor(device, kAccentBright);
    const wchar_t brandB[] = L"VST";
    TextOutW(device, 12 + brandSize.cx, 8, brandB, 3);
    SelectObject(device, oldFont);
    DeleteObject(brandFont);

    if (playlistVisible_) {
        const auto layout = EditorLayout::calculate(client.right, playlistHeight_);
        RECT topEdge {layout.playlistList.x - 1, kPlaylistPanelTop - 1,
                      layout.playlistList.x + layout.playlistList.width + 1,
                      kPlaylistPanelTop};
        const auto borderBrush = CreateSolidBrush(kPanelBorder);
        FillRect(device, &topEdge, borderBrush);
        DeleteObject(borderBrush);
    }

    if (playlistVisible_) {
        const auto countText = std::to_wstring(playlistPaths_.size()) +
                               (playlistPaths_.size() == 1
                                    ? L" TRACK"
                                    : L" TRACKS");
        const auto countFont = CreateFontW(
            -10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto oldCountFont = SelectObject(device, countFont);
        SetTextColor(device, kMutedText);
        RECT countBounds {112, kPlaylistHeaderTop + 2,
                          client.right - 12, kPlaylistHeaderTop + 18};
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
    const auto background = CreateSolidBrush(selected ? kPanel : kPlaylistBackground);
    FillRect(item->hDC, &item->rcItem, background);
    DeleteObject(background);

    if (selected) {
        RECT accent {item->rcItem.left, item->rcItem.top,
                     item->rcItem.left + 3, item->rcItem.bottom};
        const auto accentBrush = CreateSolidBrush(kAccent);
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
        const auto dropPen = CreatePen(PS_SOLID, 2, kAccentBright);
        const auto oldDropPen = SelectObject(item->hDC, dropPen);
        MoveToEx(item->hDC, item->rcItem.left + 3, lineY, nullptr);
        LineTo(item->hDC, item->rcItem.right - 3, lineY);
        SelectObject(item->hDC, oldDropPen);
        DeleteObject(dropPen);
    }

    const auto separatorPen = CreatePen(PS_SOLID, 1, kPlaylistSeparator);
    const auto oldPen = SelectObject(item->hDC, separatorPen);
    MoveToEx(item->hDC, item->rcItem.left, item->rcItem.bottom - 1, nullptr);
    LineTo(item->hDC, item->rcItem.right, item->rcItem.bottom - 1);
    SelectObject(item->hDC, oldPen);
    DeleteObject(separatorPen);

    SetBkMode(item->hDC, TRANSPARENT);
    const auto rowFont = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const auto oldFont = SelectObject(item->hDC, rowFont);

    const auto indexText = std::to_wstring(item->itemID + 1);
    SetTextColor(item->hDC, kMutedText);
    RECT indexRect {item->rcItem.left + 10, item->rcItem.top,
                    item->rcItem.left + 32, item->rcItem.bottom};
    DrawTextW(item->hDC, indexText.c_str(), -1, &indexRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const auto noteFont = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    const auto oldNoteFont = SelectObject(item->hDC, noteFont);
    SetTextColor(item->hDC, kAccentBright);
    RECT noteRect {item->rcItem.left + 33, item->rcItem.top,
                   item->rcItem.left + 49, item->rcItem.bottom};
    DrawTextW(item->hDC, L"♪", -1, &noteRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, oldNoteFont);
    DeleteObject(noteFont);

    auto title = path.stem().wstring();
    if (title.empty())
        title = path.filename().wstring();
    SetTextColor(item->hDC, kText);
    RECT titleRect {item->rcItem.left + 54, item->rcItem.top,
                    item->rcItem.right - 45, item->rcItem.bottom};
    DrawTextW(item->hDC, title.c_str(), -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    const auto type = classifyMediaFile(path);
    const wchar_t* badgeText = type == MediaFileType::audio
        ? L"AUDIO"
        : type == MediaFileType::video ? L"VIDEO" : L"MEDIA";
    RECT badge {item->rcItem.right - 40, item->rcItem.top + 4,
                item->rcItem.right - 5, item->rcItem.bottom - 4};
    const auto badgeFont = CreateFontW(
        -9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const auto oldBadgeFont = SelectObject(item->hDC, badgeFont);
    SetTextColor(item->hDC, kMutedText);
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

    const auto trackBrush = CreateSolidBrush(kProgressBackground);
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
            scrollbar.left + 4,
            scrollbar.top + 3,
            scrollbar.right - 4,
            scrollbar.bottom - 3,
        };
        const auto trackHeight = std::max(
            1, static_cast<int>(track.bottom - track.top));
        const auto thumbHeight = std::min(
            trackHeight,
            std::max(16, trackHeight * visibleItems / count));
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
        const auto thumbBrush = CreateSolidBrush(kPlaylistScrollThumb);
        const auto thumbPen = CreatePen(PS_SOLID, 1, kPlaylistScrollThumb);
        const auto oldBrush = SelectObject(device, thumbBrush);
        const auto oldPen = SelectObject(device, thumbPen);
        RoundRect(device, thumb.left, thumb.top, thumb.right, thumb.bottom,
                  8, 8);
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
    const auto iconButton = item->CtlID == kPreviousButton ||
                            playButton || item->CtlID == kStopButton ||
                            item->CtlID == kNextButton;

    const auto baseBrush = CreateSolidBrush(kBackground);
    FillRect(item->hDC, &item->rcItem, baseBrush);
    DeleteObject(baseBrush);

    if (iconButton) {
        const auto kind = playButton ? 0 : item->CtlID == kStopButton ? 1
            : item->CtlID == kPreviousButton ? 2 : 3;
        drawVectorTransport(item->hDC, item->rcItem, kind, playing_, selected);
        return;
    } else if (!openButton) {
        const auto fill = selected ? kPanel : kBackground;
        const auto fillBrush = CreateSolidBrush(fill);
        const auto borderPen = CreatePen(PS_SOLID, 1, kAccentBright);
        const auto oldBrush = SelectObject(item->hDC, fillBrush);
        const auto oldPen = SelectObject(item->hDC, borderPen);
        RoundRect(item->hDC,
                  item->rcItem.left,
                  item->rcItem.top,
                  item->rcItem.right,
                  item->rcItem.bottom,
                  6,
                  6);
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(fillBrush);
    }

    if (openButton) {
        {
            Gdiplus::Graphics graphics(item->hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen outline(vectorColor(selected ? kAccentBright : kText), 1.3f);
            const float x = static_cast<float>(item->rcItem.left + item->rcItem.right) / 2;
            const float y = static_cast<float>(item->rcItem.top) + 9;
            const Gdiplus::PointF folder[] {{x - 8, y}, {x - 3, y},
                {x - 1, y + 2}, {x + 8, y + 2}, {x + 8, y + 12},
                {x - 8, y + 12}, {x - 8, y}};
            graphics.DrawLines(&outline, folder, static_cast<int>(std::size(folder)));
        }

        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, kMutedText);
        const auto font = CreateFontW(
            -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto oldFont = SelectObject(item->hDC, font);
        RECT label {item->rcItem.left, item->rcItem.bottom - 19,
                    item->rcItem.right, item->rcItem.bottom - 2};
        DrawTextW(item->hDC, L"Open", -1, &label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item->hDC, oldFont);
        DeleteObject(font);
    } else {
        wchar_t text[32] {};
        GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, kText);
        const auto font = CreateFontW(
            -10,
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        const auto oldFont = SelectObject(item->hDC, font);
        RECT rect = item->rcItem;
        DrawTextW(item->hDC, text, -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, oldFont);
        DeleteObject(font);
    }

}

} // namespace MediaPlayer
