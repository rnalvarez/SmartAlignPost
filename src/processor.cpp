#include "processor.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace SmartAlignPost {

Processor::Processor() {
    setControllerClass(ControllerUID);
}

FUnknown* Processor::createInstance(void*) { return (IAudioProcessor*)new Processor(); }

tresult PLUGIN_API Processor::initialize(FUnknown* context) {
    const auto r = AudioEffect::initialize(context);
    if (r != kResultOk) return r;

    // V1 prototype: 8 mono inputs represented as one 8-channel audio bus.
    addAudioInput(STR16("Smart Align Inputs"), SpeakerArr::k71Music);
    addAudioOutput(STR16("Aligned Output"), SpeakerArr::k71Music);
    return kResultOk;
}

tresult PLUGIN_API Processor::terminate() {
    masterBuffer.clear();
    sourceBuffer.clear();
    return AudioEffect::terminate();
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                  SpeakerArrangement* outputs, int32 numOuts) {
    if (numIns == 1 && numOuts == 1 &&
        inputs[0] == SpeakerArr::k71Music &&
        outputs[0] == SpeakerArr::k71Music)
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    settings.sampleRate = sampleRate;
    return AudioEffect::setupProcessing(setup);
}

void Processor::ensureBuffers(size_t channels, size_t samples) {
    if (masterBuffer.size()!=channels) masterBuffer.assign(channels, {});
    if (sourceBuffer.size()!=channels) sourceBuffer.assign(channels, {});
    for (auto& b : masterBuffer) b.reserve(samples);
    for (auto& b : sourceBuffer) b.reserve(samples);
}

tresult PLUGIN_API Processor::process(ProcessData& data) {
    if (!data.inputs || !data.outputs || data.numInputs<1 || data.numOutputs<1)
        return kResultOk;

    auto& in = data.inputs[0];
    auto& out = data.outputs[0];
    const int32 ch = std::min<int32>(in.numChannels, out.numChannels);
    const int32 n = data.numSamples;

    // V1 routing convention:
    // channels 0-1 = master stereo
    // channels 2-3 = source 1
    // channels 4-5 = source 2
    // channels 6-7 = source 3
    // For now output is a pass-through until APPLY is enabled in the host/editor.
    for (int32 c=0;c<ch;++c)
        if (in.channelBuffers32 && out.channelBuffers32)
            std::memcpy(out.channelBuffers32[c], in.channelBuffers32[c],
                        sizeof(float)*n);

    // The actual offline analysis engine is intentionally separate.
    // A host integration layer will feed complete item buffers to AlignEngine.
    return kResultOk;
}

tresult PLUGIN_API Processor::setState(IBStream*) { return kResultOk; }
tresult PLUGIN_API Processor::getState(IBStream*) { return kResultOk; }

}
