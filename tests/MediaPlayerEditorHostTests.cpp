#include "MediaPlayerController.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "MediaPlayerEditor.h"
#include "MediaPlayerEditorLayout.h"
#include "MediaPlayerProcessor.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
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
int previewEditor(bool snapshot, bool light = false)
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
    editor.setLightTheme(light);
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
        std::ofstream output(light ? "tmp/mediaplayer-vst-light.bmp" : "tmp/mediaplayer-vst-ui.bmp", std::ios::binary);
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

int testPlaylistMultiSelection()
{
    Steinberg::Vst::HostApplication host;
    MediaPlayer::Controller controller;
    MediaPlayer::Processor processor;
    controller.initialize(&host);
    processor.initialize(&host);
    controller.connect(&processor);
    processor.connect(&controller);
    ViewRect initial(0, 0, MediaPlayer::EditorLayout::kEditorWidth, 400);
    MediaPlayer::NativeEditor editor(&controller, &initial);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Playlist edit test", WS_POPUP,
        0, 0, 400, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    StrictFrame frame;
    frame.host = parent;
    editor.attached(parent, kPlatformTypeHWND);
    editor.setFrame(&frame);
    MediaPlayer::EditorLayoutState layout;
    layout.playlistVisible = true;
    layout.playlistHeight = 200;
    controller.sendEditorLayout(layout);
    const std::vector<std::filesystem::path> entries {
        L"C:/MediaPlayerTests/first.mp3", L"C:/MediaPlayerTests/second.mp3",
        L"C:/MediaPlayerTests/third.mp3", L"C:/MediaPlayerTests/fourth.mp3",
        L"C:/MediaPlayerTests/fifth.mp3", L"C:/MediaPlayerTests/sixth.mp3"};
    for (const auto& path : entries)
        processor.requestOpenFile(path);
    processor.selectPlaylist(entries.front(), false);
    controller.requestPlaybackStatus();
    const auto list = FindWindowExW(GetWindow(parent, GW_CHILD), nullptr, L"LISTBOX", nullptr);
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    const auto pointAt = [&](int index, bool bottom = false) {
        RECT row {};
        SendMessageW(list, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row));
        return MAKELPARAM(8, bottom ? row.bottom - 2 : (row.top + row.bottom) / 2);
    };
    const auto click = [&](int index, WPARAM modifiers = 0) {
        const auto point = pointAt(index);
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON | modifiers, point);
        SendMessageW(list, WM_LBUTTONUP, modifiers, point);
    };
    const auto selected = [&] {
        const auto count = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
        std::vector<int> result(count > 0 ? count : 0);
        if (!result.empty())
            SendMessageW(list, LB_GETSELITEMS, result.size(),
                         reinterpret_cast<LPARAM>(result.data()));
        return result;
    };
    const auto rowContains = [&](int index, const wchar_t* name) {
        wchar_t text[512] {};
        SendMessageW(list, LB_GETTEXT, index, reinterpret_cast<LPARAM>(text));
        return std::wstring(text).find(name) != std::wstring::npos;
    };
    click(1);
    click(4, MK_SHIFT);
    expect(selected() == std::vector<int>{1, 2, 3, 4},
           "Shift-click selects a contiguous range");
    controller.requestPlaybackStatus();
    controller.requestPlaybackStatus();
    expect(selected() == std::vector<int>{1, 2, 3, 4},
           "status polling preserves an editing selection");
    click(3, MK_SHIFT);
    expect(selected() == std::vector<int>{1, 2, 3},
           "repeated Shift-click keeps the original anchor");
    click(4, MK_SHIFT);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(2));
    SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, pointAt(3));
    SendMessageW(list, WM_LBUTTONUP, 0, pointAt(3));
    controller.requestPlaybackStatus();
    expect(selected() == std::vector<int>{1, 2, 3, 4} && rowContains(1, L"second"),
           "dropping a selected group onto itself preserves its order and selection");
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(2));
    SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, pointAt(5, true));
    controller.requestPlaybackStatus();
    SendMessageW(list, WM_LBUTTONUP, 0, pointAt(5, true));
    controller.requestPlaybackStatus();
    expect(rowContains(0, L"first") && rowContains(1, L"sixth") &&
           rowContains(2, L"second") && rowContains(5, L"fifth") &&
           selected() == std::vector<int>{2, 3, 4, 5},
           "dragging selected rows down moves the group and keeps it selected");
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, pointAt(3));
    SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(8, 0));
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(8, 0));
    controller.requestPlaybackStatus();
    expect(rowContains(0, L"second") && rowContains(3, L"fifth") &&
           rowContains(4, L"first") && selected() == std::vector<int>{0, 1, 2, 3},
           "dragging selected rows up preserves the group order");
    SendMessageW(list, WM_KEYDOWN, VK_DELETE, 0);
    controller.requestPlaybackStatus();
    expect(SendMessageW(list, LB_GETCOUNT, 0, 0) == 2 &&
           rowContains(0, L"first") && rowContains(1, L"sixth"),
           "Delete removes every selected row through the real controller and processor");
    click(1);
    click(0, MK_SHIFT);
    expect(selected() == std::vector<int>{0, 1}, "Shift-click supports a backward range");
    SendMessageW(list, WM_KEYDOWN, VK_DELETE, 0);
    controller.requestPlaybackStatus();
    expect(SendMessageW(list, LB_GETCOUNT, 0, 0) == 0,
           "Delete can remove the entire selected playlist");
    editor.setFrame(nullptr);
    editor.removed();
    DestroyWindow(parent);
    controller.disconnect(&processor);
    processor.disconnect(&controller);
    controller.terminate();
    processor.terminate();
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--snapshot-light")
        return previewEditor(true, true);
    if (argc > 1 && std::string_view(argv[1]) == "--preview-light")
        return previewEditor(false, true);
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
    // Exercise the actual theme button, per-instance colors, and host state.
    const auto panel = FindWindowExW(child, nullptr, MediaPlayer::NativeEditor::kPlayerPanelClassName, nullptr);
    const auto theme = FindWindowExW(panel, nullptr, L"BUTTON", L"Dark");
    expect(theme != nullptr, "theme button is visible in the player header");
    RECT themeRect {};
    GetWindowRect(theme, &themeRect);
    expect(themeRect.right - themeRect.left == 40 && themeRect.bottom - themeRect.top == 16,
           "theme button uses compact 40x16 bounds");
    SendMessageW(theme, BM_CLICK, 0, 0);
    expect(editor.isLightTheme() && controller.isLightTheme(), "button selects Light");
    const auto dc = GetDC(child);
    const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(child, WM_CTLCOLORLISTBOX,
        reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(list)));
    LOGBRUSH description {};
    GetObjectW(brush, sizeof(description), &description);
    expect(description.lbColor == RGB(255,255,255) && GetTextColor(dc) == RGB(23,50,71),
        "Light updates native list background and text together");
    ReleaseDC(child, dc);
    ResizableMemoryIBStream state;
    expect(controller.getState(&state) == kResultOk, "theme saves to host controller state");
    state.rewind();
    MediaPlayer::Controller restored;
    expect(restored.setState(&state) == kResultOk && restored.isLightTheme(),
        "Light restores from host state");
    {
        ViewRect otherSize(0,0,300,160);
        MediaPlayer::Controller separate;
        MediaPlayer::NativeEditor separateEditor(&separate, &otherSize);
        expect(!separateEditor.isLightTheme(), "other plugin instance stays Dark");
        MediaPlayer::NativeEditor restoredEditor(&restored, &otherSize);
        expect(restoredEditor.isLightTheme(), "new view uses restored theme");
    }
    SendMessageW(theme, BM_CLICK, 0, 0);
    expect(!editor.isLightTheme(), "button switches back to Dark");
    expect(SendMessageW(list, LB_GETCOUNT, 0, 0) == 0, "theme switching preserves the empty playlist");
    expect((GetWindowLongPtrW(list, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0,
           "playlist does not use the standard white inset border");
    expect((GetWindowLongPtrW(list, GWL_STYLE) & LBS_OWNERDRAWFIXED) != 0,
           "playlist uses the custom music-player row renderer");
    const std::vector<std::filesystem::path> playlistItems {
        L"C:/MediaPlayerTests/first.mp3",
        L"C:/MediaPlayerTests/second.mp3"};
    editor.updatePlaylist(playlistItems, 0);
    expect(SendMessageW(list, LB_GETSEL, 0, 0) > 0,
           "playlist highlights its current item");
    editor.updatePlaylist(playlistItems, 1);
    expect(SendMessageW(list, LB_GETSEL, 1, 0) > 0 &&
           SendMessageW(list, LB_GETSELCOUNT, 0, 0) == 1,
           "playlist highlight follows a track advance without rebuilding");
    SendMessageW(list, LB_SETSEL, TRUE, 0);
    editor.updatePlaylist(playlistItems, 1);
    expect(SendMessageW(list, LB_GETSELCOUNT, 0, 0) == 2,
           "unchanged playback status does not collapse a multiple selection");
    editor.updatePlaylist(playlistItems, 0);
    expect(SendMessageW(list, LB_GETSELCOUNT, 0, 0) == 1 &&
           SendMessageW(list, LB_GETSEL, 0, 0) > 0,
           "a real track change still moves the selection to the current song");
    RECT firstItem {};
    SendMessageW(list, LB_GETITEMRECT, 0,
                 reinterpret_cast<LPARAM>(&firstItem));
    const auto clickX = firstItem.left + 5;
    const auto clickY = firstItem.top +
                        (firstItem.bottom - firstItem.top) / 2;
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(clickX, clickY));
    expect(GetFocus() == list,
           "clicking a playlist row gives the list keyboard focus");
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(clickX, clickY));
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
    const auto multiSelectionFailed = testPlaylistMultiSelection();
    return failures || multiSelectionFailed ? 1 : 0;
}
