#include "controller.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace SmartAlignPost {

FUnknown* Controller::createInstance(void*) {
    return (IEditController*)new Controller();
}

tresult PLUGIN_API Controller::initialize(FUnknown* context) {
    const auto r = EditController::initialize(context);
    if (r != kResultOk)
        return r;

    // MODE: selector STATIC / DYNAMIC.
    // 0.0 = STATIC, 1.0 = DYNAMIC.
    parameters.addParameter(
        STR16("Mode"),
        nullptr,
        1,
        0.0,
        ParameterInfo::kCanAutomate,
        0);

    // ANALYZE / APPLY: botones momentáneos.
    // Se representan como parámetros de 0..1 para mantener compatibilidad
    // con el shell VST3 actual; la futura integración con REAPER consumirá
    // el cambio de valor como una acción y no como un estado persistente.
    parameters.addParameter(
        STR16("Analyze"),
        nullptr,
        1,
        0.0,
        ParameterInfo::kCanAutomate,
        1);

    parameters.addParameter(
        STR16("Apply"),
        nullptr,
        1,
        0.0,
        ParameterInfo::kCanAutomate,
        2);

    return kResultOk;
}

}
