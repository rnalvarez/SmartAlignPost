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

    // MODE: use the SDK StringListParameter so VSTGUI can populate the
    // COptionMenu with real entries instead of relying on manual conversion.
    auto* modeParam = new StringListParameter(STR16("Mode"), 0);
    modeParam->appendString(STR16("STATIC"));
    modeParam->appendString(STR16("DYNAMIC"));
    parameters.addParameter(modeParam);

    // ANALYZE / APPLY are action parameters. The custom editor presents them
    // as push buttons; the processor integration will consume their events.
    parameters.addParameter(
        STR16("Analyze"), nullptr, 1, 0.0,
        ParameterInfo::kCanAutomate, 1);

    parameters.addParameter(
        STR16("Apply"), nullptr, 1, 0.0,
        ParameterInfo::kCanAutomate, 2);

    return kResultOk;
}

IPlugView* PLUGIN_API Controller::createView(const char* name) {
    if (name && FIDStringsEqual(name, ViewType::kEditor)) {
        return new VSTGUI::VST3Editor(this, "view", "smartalignpost.uidesc");
    }
    return nullptr;
}

}
