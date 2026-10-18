#include "MediaPlayerController.h"
#include "MediaPlayerEditor.h"
#include "MediaPlayerEditorLayout.h"
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string_view>

using namespace Steinberg;

// Model a host that checks the resizing contract before accepting resizeView.
class StrictFrame final : public IPlugFrame {
public:
    tresult PLUGIN_API queryInterface(const TUID, void** object) override
    { *object = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
    {
        if (view->canResize() != kResultTrue ||
            view->checkSizeConstraint(size) != kResultTrue)
            return kResultFalse;
        ++accepted;
        if (host) {
            RECT bounds {0, 0, size->getWidth(), size->getHeight()};
            AdjustWindowRectEx(&bounds,
                static_cast<DWORD>(GetWindowLongPtrW(host, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(host, GWL_EXSTYLE)));
            SetWindowPos(host, nullptr, 0, 0,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return view->onSize(size);
    }
    int accepted = 0;
    HWND host = nullptr;
};

// Display the production editor without an audio chain for visual inspection.
// No alternate renderer: the controls below are the same NativeEditor as VST3.
int previewEditor(bool snapshot)
{
    SetProcessDPIAware();
    constexpr int width = MediaPlayer::EditorLayout::kEditorWidth;
    constexpr int height = 362;
    WNDCLASSW windowClass {};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DAWMediaPlayer.VisualCheck";
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
        if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    };
    RegisterClassW(&windowClass);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT bounds {0, 0, width, height};
    AdjustWindowRect(&bounds, style, FALSE);
    const auto parent = CreateWindowExW(0, windowClass.lpszClassName,
        L"MediaPlayer VST - UI preview", style, 200, 200,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!parent) return 1;
    MediaPlayer::Controller controller;
    StrictFrame frame;
    frame.host = parent;
    ViewRect size(0, 0, width, height);
    MediaPlayer::NativeEditor editor(&controller, &size);
    editor.attached(parent, kPlatformTypeHWND);
    MediaPlayer::EditorLayoutState layout;
    layout.playlistVisible = true;
    layout.playlistHeight = 192;
    editor.applyEditorLayout(layout);
    editor.setFrame(&frame);
    const std::vector<std::filesystem::path> tracks {
        L"E:/Music/Karaoke/Face the change (Vocals).mp3",
        L"E:/Music/Karaoke/Time goes by.mp3",
        L"E:/Music/Karaoke/安月名莉子 Acoustic cover series.flac",
        L"E:/Music/Karaoke/I believe.mp3",
        L"E:/Music/Karaoke/fragile.mp3",
        L"E:/Music/Karaoke/For the moment.mp3",
        L"E:/Music/Karaoke/Dear My Friend.mp3"};
    editor.updatePlaylist(tracks, 0);
    editor.updateMediaPath(tracks[0]);
    editor.updatePlaybackStatus(84 * 48000, 272 * 48000, 48000, false, true, 0.72f);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    if (snapshot) {
        RedrawWindow(parent, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        const auto screen = GetDC(parent);
        const auto buffer = CreateCompatibleDC(screen);
        BITMAPINFO bitmapInfo {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        const auto bitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS,
            &pixels, nullptr, 0);
        const auto oldBitmap = SelectObject(buffer, bitmap);
        const auto captured = PrintWindow(parent, buffer, PW_CLIENTONLY | 2);
        GdiFlush();
        BITMAPFILEHEADER header {};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + width * height * 4;
        std::ofstream output("tmp/mediaplayer-vst-ui.bmp", std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&bitmapInfo.bmiHeader),
                     sizeof(BITMAPINFOHEADER));
        output.write(static_cast<const char*>(pixels), width * height * 4);
        const auto saved = output.good();
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        ReleaseDC(parent, screen);
        editor.setFrame(nullptr);
        editor.removed();
        DestroyWindow(parent);
        return captured && saved ? 0 : 1;
    }
    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    editor.setFrame(nullptr);
    editor.removed();
    DestroyWindow(parent);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--preview")
        return previewEditor(false);
    if (argc > 1 && std::string_view(argv[1]) == "--snapshot")
        return previewEditor(true);
    MediaPlayer::Controller controller;
    ViewRect initial(0, 0, MediaPlayer::EditorLayout::kEditorWidth,
                     MediaPlayer::EditorLayout::kBaseEditorHeight);
    MediaPlayer::NativeEditor editor(&controller, &initial);
    StrictFrame frame;
    const auto parent = CreateWindowExW(0, L"STATIC", L"Test host", WS_POPUP,
        0, 0, 400, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent)
        return 1;
    frame.host = parent;
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    expect(editor.attached(parent, kPlatformTypeHWND) == kResultOk,
           "editor attaches without a frame");
    MediaPlayer::EditorLayoutState layout;
    layout.playlistVisible = true;
    layout.playlistHeight = 120;
    editor.applyEditorLayout(layout);
    editor.setFrame(&frame);
    expect(frame.accepted > 0, "late frame accepts pending playlist resize");
    ViewRect actual;
    editor.getSize(&actual);
    expect(actual.getWidth() == MediaPlayer::EditorLayout::kEditorWidth,
           "initial editor width follows the configured default");
    expect(actual.getHeight() == 290, "expanded playlist geometry reaches host");
    const auto child = GetWindow(parent, GW_CHILD);
    RECT client {};
    GetClientRect(child, &client);
    expect(client.bottom == 290, "native editor expands with host callback");
    const auto list = FindWindowExW(child, nullptr, L"LISTBOX", nullptr);
    expect((GetWindowLongPtrW(list, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0,
           "playlist does not use the standard white inset border");
    expect((GetWindowLongPtrW(list, GWL_STYLE) & LBS_OWNERDRAWFIXED) != 0,
           "playlist uses the custom music-player row renderer");
    RECT bounds {};
    GetWindowRect(list, &bounds);
    MapWindowPoints(nullptr, child, reinterpret_cast<POINT*>(&bounds), 2);
    expect(list && (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) &&
           bounds.bottom <= client.bottom,
           "playlist is visible inside editor bounds");
    // Mouse messages carry the position at the event time. The OS cursor may
    // already be somewhere else when a queued mouse-down is processed.
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 285));
    SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 365));
    SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(200, 365));
    editor.getSize(&actual);
    GetClientRect(parent, &client);
    expect(actual.getHeight() == 370 && client.bottom == 370,
           "bottom-strip drag expands both editor and host by 80px");
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 365));
    SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 225));
    SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(200, 225));
    editor.getSize(&actual);
    expect(actual.getHeight() == 230,
           "bottom-strip drag shrinks back to the minimum playlist height");
    const auto resizeRequests = frame.accepted;
    for (int height = 291; height <= 390; ++height) {
        ViewRect dragged(0, 0, 500, height);
        editor.checkSizeConstraint(&dragged);
        expect(dragged.getHeight() == height,
               "host drag height is preserved instead of snapping back");
        editor.onSize(&dragged);
    }
    ViewRect beyondPreviousLimit(0, 0, 500, 700);
    editor.checkSizeConstraint(&beyondPreviousLimit);
    expect(beyondPreviousLimit.getHeight() == 700,
           "host drag height is not capped at the former maximum");
    editor.onSize(&beyondPreviousLimit);
    expect(frame.accepted == resizeRequests,
           "host drag does not trigger competing plugin resize requests");
    GetClientRect(child, &client);
    GetWindowRect(list, &bounds);
    MapWindowPoints(nullptr, child, reinterpret_cast<POINT*>(&bounds), 2);
    expect(client.bottom == 700 && bounds.bottom == 690,
           "playlist follows the host drag height");
    layout.playlistVisible = false;
    editor.applyEditorLayout(layout);
    editor.getSize(&actual);
    expect(actual.getHeight() == MediaPlayer::EditorLayout::kBaseEditorHeight,
           "playlist hide shrinks the host view to the redesigned player panel");
    editor.setFrame(nullptr);
    editor.removed();
    DestroyWindow(parent);
    return failures ? 1 : 0;
}
