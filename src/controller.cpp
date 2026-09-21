#include "controller.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "public.sdk/source/vst/vstparameters.h"

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

    auto* residual = new RangeParameter(
        STR16("Residual Samples"),
        kResidualSamplesParam,
        STR16("samples"),
        -256.0,
        256.0,
        0.0,
        0);

    parameters.addParameter(residual);

    parameters.addParameter(
        STR16("Apply Residual"),
        nullptr,
        1,
        0.0,
        ParameterInfo::kCanAutomate,
        kApplyResidualParam);

    return kResultOk;
}

IPlugView* PLUGIN_API Controller::createView(const char* name) {
    if (name && FIDStringsEqual(name, ViewType::kEditor)) {
        return new VSTGUI::VST3Editor(
            this,
            "view",
            "smartalignpost.uidesc");
    }

    return nullptr;
}

}
