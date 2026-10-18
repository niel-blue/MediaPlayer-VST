#pragma once

#include "pluginterfaces/base/funknown.h"

namespace MediaPlayer {

// These IDs are stable and belong exclusively to this plug-in.
static const Steinberg::FUID kProcessorUID {
    0x6F5C7A21, 0x8B1A4C62, 0xB7E3D4F0, 0x913E2A5C
};

static const Steinberg::FUID kControllerUID {
    0xA82D1F49, 0xF3C0687B, 0x9E15B4D2, 0x6C4A208E
};

} // namespace MediaPlayer
