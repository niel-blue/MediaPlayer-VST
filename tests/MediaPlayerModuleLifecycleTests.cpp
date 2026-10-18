#define NOMINMAX
#include <windows.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "MediaPlayerEditorLayout.h"
#include <cstring>
#include <iostream>

using namespace Steinberg;

// Unlike the editor unit tests, this loads the real DLL and exercises its
// CRT destructors under FreeLibrary's loader lock, as an actual host does.
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    const auto module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) { std::cerr << "LoadLibrary: " << GetLastError() << std::endl; return 2; }
    const auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
    const auto exit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
    const auto getFactory = reinterpret_cast<IPluginFactory* (PLUGIN_API*)()>(
        GetProcAddress(module, "GetPluginFactory"));
    if (!init || !exit || !getFactory || !init()) return 3;
    auto* factory = getFactory();
    Vst::IEditController* controller = nullptr;
    for (int32 index = 0; index < factory->countClasses(); ++index) {
        PClassInfo info {};
        if (factory->getClassInfo(index, &info) == kResultOk &&
            std::strcmp(info.category, kVstComponentControllerClass) == 0) {
            factory->createInstance(info.cid, Vst::IEditController::iid,
                reinterpret_cast<void**>(&controller));
            break;
        }
    }
    if (!controller || controller->initialize(nullptr) != kResultOk) return 4;
    auto* view = controller->createView(Vst::ViewType::kEditor);
    const auto parent = CreateWindowExW(0, L"STATIC", L"MediaPlayer unload check",
        WS_POPUP, 0, 0, MediaPlayer::EditorLayout::kEditorWidth,
        MediaPlayer::EditorLayout::kBaseEditorHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!view || !parent || view->attached(parent, kPlatformTypeHWND) != kResultOk)
        return 5;
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto until = GetTickCount64() + 3000;
    while (GetTickCount64() < until) {
        MSG message {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    std::cout << "Removing empty editor" << std::endl;
    view->removed();
    view->release();
    DestroyWindow(parent);
    controller->terminate();
    controller->release();
    factory->release();
    std::cout << "ExitDll" << std::endl;
    if (!exit()) return 6;
    std::cout << "FreeLibrary" << std::endl;
    if (!FreeLibrary(module)) return 7;
    std::cout << "Unload completed" << std::endl;
    return 0;
}
