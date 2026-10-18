# MediaPlayer VST

[日本語](README.md)

MediaPlayer VST is a Windows VST3 plug-in for playing audio and video files inside a DAW or VST host. Audio is sent to the host through the plug-in's Stereo Output. Video is shown in a separate resizable window.

<img width="300" height="321" alt="MPV" src="https://github.com/user-attachments/assets/2a6764cc-5bf8-4f23-a330-944e63df93d4" />

## Features

- Play, pause, stop, and seek audio and video files
- One playlist for audio and video, with drag-and-drop reordering
- Previous/next navigation and automatic advance at the end of a track
- Drag and drop files onto the player, playlist, or video window
- Volume adjustment with the knob or mouse wheel
- Resizable video window with aspect ratio preserved
- Video window move, fullscreen, and seek controls
- Playlist, selected media, playback position, volume, and window layout saved in VST3 state

The plug-in outputs audio only. Video appears in a separate window; the plug-in does not send video to the DAW, streaming software, virtual cameras, NDI, or Spout. To use the video in a stream, capture the video window in your streaming software.

Playback is routed through the host. Connect the MediaPlayer VST Stereo Output to the mixer or output channel you want to monitor.

MediaPlayer VST does not save settings in the Windows registry or a separate settings file. State is saved in the DAW/VST host project or preset.

It supports common audio and video formats that FFmpeg can decode, including WAV, MP3, FLAC, AAC, M4A, OGG, MP4, MKV, MOV, AVI, WebM, WMV, MPEG, and M2TS. Actual support also depends on the codecs contained in each file.

## Requirements

- Windows x64
- A DAW or VST host that supports VST3

## Install

Download the ZIP from GitHub Releases, extract it, and copy `MediaPlayer VST.vst3` into a VST3 plug-in folder. Before updating, close the DAW/VST host, replace the existing bundle, and rescan plug-ins if needed.

## Use

Choose a media file with `Open`, or drag files onto the player or playlist. Video files can also be opened by dropping them directly onto the video window.

Use the play button to toggle playback and pause. Stop returns to the beginning while keeping the media loaded. The seek bar works during playback. Adjust volume with the knob or mouse wheel.

`Playlist` shows or hides the playlist. Double-click an item to play it. Drag an item to reorder the list. Delete a selected item with the Delete key or the right-click menu; the original media file is not deleted. `Clear` empties the playlist. Drag the bottom edge of the playlist to change its visible height.

In the video window, click to toggle playback and double-click to toggle fullscreen. Drag the window to move it. The mouse wheel adjusts volume, and the seek bar along the bottom follows the player.

## Build

Visual Studio 2022 C++ tools and CMake 3.25 or later are required.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_ffmpeg.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The first CMake configure downloads the pinned VST3 SDK. The FFmpeg setup script downloads the pinned Windows x64 LGPL shared build and checks its SHA-256 hash.

## License

MediaPlayer VST is released under the [MIT License](LICENSE). Third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
