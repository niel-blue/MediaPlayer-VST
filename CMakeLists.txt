cmake_minimum_required(VERSION 3.25)

project(MediaPlayerVST3
    VERSION 0.1.0
    DESCRIPTION "MediaPlayer VST"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # The SDK builds helper tools used to package and validate VST3 modules.
    # /FS prevents concurrent compiler processes from contending for a PDB.
    add_compile_options(/FS /utf-8)
endif()

set(VST3_SDK_ROOT "" CACHE PATH
    "Optional path to a local VST3 SDK checkout")
set(VST3_SDK_GIT_TAG
    "9fad9770f2ae8542ab1a548a68c1ad1ac690abe0" CACHE STRING
    "Pinned VST3 SDK commit used when the SDK is downloaded")
mark_as_advanced(VST3_SDK_GIT_TAG)

set(FFMPEG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg"
    CACHE PATH "Path to the FFmpeg shared development package"
)

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavformat/avformat.h")
    message(FATAL_ERROR
        "FFMPEG_ROOT does not point to an FFmpeg development package: ${FFMPEG_ROOT}\n"
        "Expected include/libavformat/avformat.h."
    )
endif()

if(NOT EXISTS "${FFMPEG_ROOT}/lib/avformat.lib" OR
   NOT EXISTS "${FFMPEG_ROOT}/lib/avcodec.lib" OR
   NOT EXISTS "${FFMPEG_ROOT}/lib/avutil.lib" OR
   NOT EXISTS "${FFMPEG_ROOT}/lib/swresample.lib" OR
   NOT EXISTS "${FFMPEG_ROOT}/lib/swscale.lib")
    message(FATAL_ERROR
        "FFMPEG_ROOT is missing one or more import libraries in ${FFMPEG_ROOT}/lib."
    )
endif()

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
# Installing a link into the user's global VST3 directory requires elevated
# Windows symlink privileges. The package under build/VST3 is the artifact for
# this project, so keep the automatic link disabled.
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

set(_media_player_sdk_added FALSE)
if(VST3_SDK_ROOT STREQUAL "")
    set(_media_player_local_sdk
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
    if(EXISTS "${_media_player_local_sdk}/CMakeLists.txt")
        set(VST3_SDK_ROOT "${_media_player_local_sdk}")
    else()
        include(FetchContent)
        FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG "${VST3_SDK_GIT_TAG}"
            GIT_SUBMODULES_RECURSE TRUE
            GIT_PROGRESS TRUE
        )
        message(STATUS
            "VST3_SDK_ROOT is not set; downloading the pinned VST3 SDK")
        FetchContent_MakeAvailable(vst3sdk)
        set(VST3_SDK_ROOT "${vst3sdk_SOURCE_DIR}")
        set(_media_player_sdk_added TRUE)
    endif()
endif()

if(NOT EXISTS "${VST3_SDK_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "VST3 SDK was not found at '${VST3_SDK_ROOT}'. "
        "Set VST3_SDK_ROOT to a valid checkout or leave it empty "
        "to download the pinned SDK."
    )
endif()

set(vst3sdk_SOURCE_DIR "${VST3_SDK_ROOT}")
if(NOT _media_player_sdk_added)
    add_subdirectory("${VST3_SDK_ROOT}" "${CMAKE_BINARY_DIR}/vst3sdk")
endif()
smtg_enable_vst3_sdk()

set(MEDIA_PLAYER_SOURCES
    src/MediaPlayerController.cpp
    src/MediaPlayerController.h
    src/MediaPlayerEditor.cpp
    src/MediaPlayerEditor.h
    src/MediaPlayerEditorLayout.cpp
    src/MediaPlayerEditorLayout.h
    src/MediaPlayerFileTypes.cpp
    src/MediaPlayerFileTypes.h
    src/MediaPlayerMessages.h
    src/MediaPlayerPlaylist.cpp
    src/MediaPlayerPlaylist.h
    src/MediaPlayerIds.h
    src/MediaPlayerProcessor.cpp
    src/MediaPlayerProcessor.h
    src/MediaPlayerAudioEngine.cpp
    src/MediaPlayerAudioEngine.h
    src/MediaPlayerDebugLog.cpp
    src/MediaPlayerDebugLog.h
    src/MediaPlayerVideo.cpp
    src/MediaPlayerVideo.h
    src/MediaPlayerVideoDecoder.cpp
    src/MediaPlayerVideoDecoder.h
    src/MediaPlayerFactory.cpp
    src/FfmpegAudioDecoder.cpp
    src/FfmpegAudioDecoder.h
    src/PcmRingBuffer.cpp
    src/PcmRingBuffer.h
)

smtg_add_vst3plugin(MediaPlayerVST3
    PACKAGE_NAME "MediaPlayer VST"
    SOURCES_LIST ${MEDIA_PLAYER_SOURCES}
)
smtg_target_configure_version_file(MediaPlayerVST3)

target_compile_features(MediaPlayerVST3 PRIVATE cxx_std_17)
target_link_libraries(MediaPlayerVST3 PRIVATE sdk)

add_library(ffmpeg_avutil SHARED IMPORTED GLOBAL)
set_target_properties(ffmpeg_avutil PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/avutil.lib"
    IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/avutil-61.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

add_library(ffmpeg_avcodec SHARED IMPORTED GLOBAL)
set_target_properties(ffmpeg_avcodec PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/avcodec.lib"
    IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/avcodec-63.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

add_library(ffmpeg_avformat SHARED IMPORTED GLOBAL)
set_target_properties(ffmpeg_avformat PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/avformat.lib"
    IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/avformat-63.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

add_library(ffmpeg_swresample SHARED IMPORTED GLOBAL)
set_target_properties(ffmpeg_swresample PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/swresample.lib"
    IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/swresample-7.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

add_library(ffmpeg_swscale SHARED IMPORTED GLOBAL)
set_target_properties(ffmpeg_swscale PROPERTIES
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/swscale.lib"
    IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/swscale-10.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
)

set(FFMPEG_LINK_LIBRARIES
    ffmpeg_avformat
    ffmpeg_avcodec
    ffmpeg_swresample
    ffmpeg_swscale
    ffmpeg_avutil
)

target_link_libraries(MediaPlayerVST3 PRIVATE ${FFMPEG_LINK_LIBRARIES})
target_include_directories(MediaPlayerVST3 PRIVATE "${FFMPEG_ROOT}/include")
if(WIN32)
    target_link_libraries(MediaPlayerVST3 PRIVATE
        user32 comdlg32 comctl32 shell32 uxtheme ole32 gdi32 gdiplus)
endif()

set(FFMPEG_RUNTIME_DLLS
    "${FFMPEG_ROOT}/bin/avformat-63.dll"
    "${FFMPEG_ROOT}/bin/avcodec-63.dll"
    "${FFMPEG_ROOT}/bin/avutil-61.dll"
    "${FFMPEG_ROOT}/bin/swresample-7.dll"
    "${FFMPEG_ROOT}/bin/swscale-10.dll"
)

# The VST3 SDK runs moduleinfotool and the validator as POST_BUILD steps.
# Place FFmpeg beside the module before linking so those tools can load the
# freshly built plug-in on a machine that has no system-wide FFmpeg install.
add_custom_command(TARGET MediaPlayerVST3 PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:MediaPlayerVST3>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:MediaPlayerVST3>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

add_custom_command(TARGET MediaPlayerVST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_ROOT}/LICENSE.txt"
        "$<TARGET_FILE_DIR:MediaPlayerVST3>/../Resources/FFmpeg-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFMPEG_ROOT}/BUILD-INFO.txt"
        "$<TARGET_FILE_DIR:MediaPlayerVST3>/../Resources/FFmpeg-BUILD-INFO.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VST3_SDK_ROOT}/LICENSE.txt"
        "$<TARGET_FILE_DIR:MediaPlayerVST3>/../Resources/VST3-SDK-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:MediaPlayerVST3>/../Resources/DAW-Media-Player-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:MediaPlayerVST3>/../Resources/THIRD_PARTY_NOTICES.md"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${MEDIA_PLAYER_SOURCES})

enable_testing()

add_executable(MediaPlayerCoreTests
    tests/PcmRingBufferTests.cpp
    src/PcmRingBuffer.cpp
    src/PcmRingBuffer.h
)
target_compile_features(MediaPlayerCoreTests PRIVATE cxx_std_17)
target_include_directories(MediaPlayerCoreTests PRIVATE src)
add_test(NAME MediaPlayerCoreTests COMMAND MediaPlayerCoreTests)

add_executable(MediaPlayerPlaylistTests
    tests/MediaPlayerPlaylistTests.cpp
    src/MediaPlayerPlaylist.cpp
    src/MediaPlayerPlaylist.h
)
target_compile_features(MediaPlayerPlaylistTests PRIVATE cxx_std_17)
target_include_directories(MediaPlayerPlaylistTests PRIVATE src)
add_test(NAME MediaPlayerPlaylistTests COMMAND MediaPlayerPlaylistTests)

add_executable(MediaPlayerFfmpegTests
    tests/FfmpegAudioDecoderTests.cpp
    src/FfmpegAudioDecoder.cpp
    src/FfmpegAudioDecoder.h
    src/MediaPlayerAudioEngine.cpp
    src/MediaPlayerAudioEngine.h
    src/MediaPlayerDebugLog.cpp
    src/MediaPlayerDebugLog.h
    src/PcmRingBuffer.cpp
    src/PcmRingBuffer.h
)
target_compile_features(MediaPlayerFfmpegTests PRIVATE cxx_std_17)
target_include_directories(MediaPlayerFfmpegTests PRIVATE
    src
    "${FFMPEG_ROOT}/include"
)
target_link_libraries(MediaPlayerFfmpegTests PRIVATE ${FFMPEG_LINK_LIBRARIES})
add_custom_command(TARGET MediaPlayerFfmpegTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:MediaPlayerFfmpegTests>"
)
add_test(NAME MediaPlayerFfmpegTests COMMAND MediaPlayerFfmpegTests)

add_executable(MediaPlayerStateTests
    tests/MediaPlayerStateTests.cpp
    src/MediaPlayerProcessor.cpp
    src/MediaPlayerProcessor.h
    src/MediaPlayerAudioEngine.cpp
    src/MediaPlayerAudioEngine.h
    src/MediaPlayerDebugLog.cpp
    src/MediaPlayerDebugLog.h
    src/FfmpegAudioDecoder.cpp
    src/FfmpegAudioDecoder.h
    src/PcmRingBuffer.cpp
    src/PcmRingBuffer.h
    src/MediaPlayerMessages.h
    src/MediaPlayerPlaylist.cpp
    src/MediaPlayerPlaylist.h
    src/MediaPlayerIds.h
)
target_compile_features(MediaPlayerStateTests PRIVATE cxx_std_17)
target_include_directories(MediaPlayerStateTests PRIVATE
    src
    "${FFMPEG_ROOT}/include"
)
target_link_libraries(MediaPlayerStateTests PRIVATE sdk ${FFMPEG_LINK_LIBRARIES})
add_custom_command(TARGET MediaPlayerStateTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:MediaPlayerStateTests>"
)
add_test(NAME MediaPlayerStateTests COMMAND MediaPlayerStateTests)

if(WIN32)
    set(MEDIA_PLAYER_EDITOR_TEST_SOURCES ${MEDIA_PLAYER_SOURCES})
    list(REMOVE_ITEM MEDIA_PLAYER_EDITOR_TEST_SOURCES src/MediaPlayerFactory.cpp)
    add_executable(MediaPlayerEditorHostTests
        tests/MediaPlayerEditorHostTests.cpp ${MEDIA_PLAYER_EDITOR_TEST_SOURCES})
    target_include_directories(MediaPlayerEditorHostTests PRIVATE src "${FFMPEG_ROOT}/include")
    target_link_libraries(MediaPlayerEditorHostTests PRIVATE sdk ${FFMPEG_LINK_LIBRARIES}
        user32 comdlg32 comctl32 shell32 uxtheme ole32 gdi32 gdiplus)
    add_custom_command(TARGET MediaPlayerEditorHostTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:MediaPlayerEditorHostTests>")
    add_test(NAME MediaPlayerEditorHostTests COMMAND MediaPlayerEditorHostTests)
    add_executable(MediaPlayerModuleLifecycleTests tests/MediaPlayerModuleLifecycleTests.cpp)
    target_include_directories(MediaPlayerModuleLifecycleTests PRIVATE src)
    target_link_libraries(MediaPlayerModuleLifecycleTests PRIVATE sdk user32)
    add_dependencies(MediaPlayerModuleLifecycleTests MediaPlayerVST3)
    add_test(NAME MediaPlayerModuleLifecycleTests COMMAND MediaPlayerModuleLifecycleTests
        "$<TARGET_FILE:MediaPlayerVST3>")
    set_tests_properties(MediaPlayerModuleLifecycleTests PROPERTIES TIMEOUT 20)
endif()
