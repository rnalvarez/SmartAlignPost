#include "public.sdk/source/main/pluginfactory.h"
#include "processor.h"
#include "controller.h"
#include "version.h"

#define stringPluginName "Smart Align Post Residual"

BEGIN_FACTORY_DEF("Ramiro N. Alvarez",
                  "https://github.com/rnalvarez/SmartAlignPost",
                  "")

DEF_VST3_CLASS(
    stringPluginName,
    "Fx",
    Vst::kDistributable,
    FULL_VERSION_STR,
    INLINE_UID_FROM_FUID(SmartAlignPost::ProcessorUID),
    SmartAlignPost::Processor::createInstance,
    INLINE_UID_FROM_FUID(SmartAlignPost::ControllerUID),
    SmartAlignPost::Controller::createInstance)

END_FACTORY
