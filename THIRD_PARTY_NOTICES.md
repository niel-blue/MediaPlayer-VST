# Third-party notices

DAW Media Player is distributed under the MIT License. The following
third-party components retain their own licenses and are not relicensed under
the DAW Media Player license.

## Steinberg VST 3 SDK

DAW Media Player is built with the VST 3 SDK, which is distributed under the
MIT License.

Copyright (c) 2025 Steinberg Media Technologies GmbH

Source and license:
https://github.com/steinbergmedia/vst3sdk

Pinned commit: `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`

The SDK source is not included in this repository. The complete SDK license is
copied into built plug-in packages as `VST3-SDK-LICENSE.txt`.

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe
and other countries.

## FFmpeg

DAW Media Player uses shared libraries from the FFmpeg project. FFmpeg and its
included third-party libraries retain their respective licenses. The current
development package is configured without `--enable-gpl` and without
`--enable-nonfree`, and with `--enable-version3`; its FFmpeg libraries are
therefore handled as LGPL version 3 or later.

Binary package used for Windows builds:

- Provider: https://github.com/BtbN/FFmpeg-Builds
- Release: `autobuild-2026-08-14-13-16`
- Asset: `ffmpeg-N-126134-gc48230eb86-win64-lgpl-shared.zip`
- SHA-256: `0b7b0b900441dc75e4528edb4bf7224220253a9525f371e7edf1aca3cdd2edbe`
- FFmpeg revision: `c48230eb86`
- Corresponding FFmpeg source: https://github.com/FFmpeg/FFmpeg/tree/c48230eb86

Project and license information:
https://ffmpeg.org/
https://ffmpeg.org/legal.html

The FFmpeg development package is not included in this source repository. A
binary release that includes FFmpeg DLLs includes the applicable LGPL license
text and build information inside the VST3 bundle. The matching GitHub Release
also includes the exact FFmpeg source revision, the output of
`ffmpeg -buildconf`, and the available BtbN build-recipe snapshot. The original
BtbN daily release record for this package is no longer available, so the
included build-recipe snapshot is documented as the closest available source
record rather than as a directly verified match. Its scripts pin the external
libraries used by the build; each library retains its own license and source.
