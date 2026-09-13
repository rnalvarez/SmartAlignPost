#pragma once
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "align_engine.h"
#include "plugin_ids.h"
#include <vector>
#include <atomic>

namespace SmartAlignPost {

class Processor final : public Steinberg::Vst::AudioEffect {
public:
    Processor();
    static Steinberg::FUnknown* createInstance(void*);
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                      Steinberg::int32 numIns,
                                                      Steinberg::Vst::SpeakerArrangement* outputs,
                                                      Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;

private:
    double sampleRate = 48000.0;
    sap::Settings settings;
    std::vector<std::vector<float>> masterBuffer;
    std::vector<std::vector<float>> sourceBuffer;
    std::vector<double> currentDelay;
    bool applying = false;
    size_t bufferedSamples = 0;

    void ensureBuffers(size_t channels, size_t samples);
};

}
