#pragma once
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "plugin_ids.h"

namespace SmartAlignPost {
class Controller final : public Steinberg::Vst::EditController {
public:
    static Steinberg::FUnknown* createInstance(void*);
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
};
}
