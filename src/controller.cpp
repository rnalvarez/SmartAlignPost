#include "controller.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "pluginterfaces/base/ustring.h"

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
    // kIsList tells generic editors that this is a discrete list.
    parameters.addParameter(
        STR16("Mode"), nullptr, 1, 0.0,
        ParameterInfo::kCanAutomate | ParameterInfo::kIsList, 0);

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

tresult PLUGIN_API Controller::getParamStringByValue(
    ParamID tag, ParamValue valueNormalized, String128 string) {
    if (tag == 0) {
        Steinberg::UString(string, 128).fromAscii(
            valueNormalized < 0.5 ? "STATIC" : "DYNAMIC");
        return kResultOk;
    }
    return EditController::getParamStringByValue(tag, valueNormalized, string);
}

}
