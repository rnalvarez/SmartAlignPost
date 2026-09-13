#include "public.sdk/source/main/pluginfactory.h"
#include "processor.h"
#include "controller.h"
#include "version.h"

#define stringPluginName "Smart Align Post"

BEGIN_FACTORY_DEF("Ramiro N. Alvarez",
                  "https://github.com/YOUR_USER/SmartAlignPost",
                  "mailto:replace@example.com")

DEF_CLASS2(INLINE_UID_FROM_FUID(SmartAlignPost::ProcessorUID),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           stringPluginName,
           Vst::kDistributable,
           "Fx",
           "Smart microphone alignment",
           FULL_VERSION_STR,
           kVstVersionString,
           SmartAlignPost::Processor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(SmartAlignPost::ControllerUID),
           PClassInfo::kManyInstances,
           kVstComponentControllerClass,
           stringPluginName,
           0,
           "",
           "",
           FULL_VERSION_STR,
           kVstVersionString,
           SmartAlignPost::Controller::createInstance)

END_FACTORY
