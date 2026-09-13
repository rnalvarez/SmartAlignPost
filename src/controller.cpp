#include "controller.h"
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace SmartAlignPost {
FUnknown* Controller::createInstance(void*) { return (IEditController*)new Controller(); }

tresult PLUGIN_API Controller::initialize(FUnknown* context) {
    const auto r = EditController::initialize(context);
    if (r != kResultOk) return r;

    parameters.addParameter(STR16("Mode"), nullptr, 1, 0.0,
                            ParameterInfo::kCanAutomate, 0);
    parameters.addParameter(STR16("Analyze"), nullptr, 1, 0.0,
                            ParameterInfo::kIsReadOnly, 1);
    parameters.addParameter(STR16("Apply"), nullptr, 1, 0.0,
                            ParameterInfo::kCanAutomate, 2);
    return kResultOk;
}
}
