#include "MediaPlayerController.h"
#include "MediaPlayerIds.h"
#include "MediaPlayerProcessor.h"

#include "public.sdk/source/main/pluginfactory.h"

#define kMediaPlayerName "MediaPlayer VST"
#define kMediaPlayerVersion "0.1.0"

BEGIN_FACTORY_DEF("MediaPlayer VST", "", "")

DEF_CLASS2(
    INLINE_UID_FROM_FUID(MediaPlayer::kProcessorUID),
    PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    kMediaPlayerName,
    Vst::kDistributable,
    "Fx|Generator",
    kMediaPlayerVersion,
    kVstVersionString,
    MediaPlayer::Processor::createInstance
)

DEF_CLASS2(
    INLINE_UID_FROM_FUID(MediaPlayer::kControllerUID),
    PClassInfo::kManyInstances,
    kVstComponentControllerClass,
    kMediaPlayerName " Controller",
    0,
    "",
    kMediaPlayerVersion,
    kVstVersionString,
    MediaPlayer::Controller::createInstance
)

END_FACTORY
