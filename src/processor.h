#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "align_engine.h"
#include "plugin_ids.h"
#include <vector>
#include <cstddef>

namespace SmartAlignPost {

class Processor final : public Steinberg::Vst::AudioEffect {
public:
    Processor();

    static Steinberg::FUnknown* createInstance(void*);

    Steinberg::tresult PLUGIN_API initialize(
        Steinberg::FUnknown* context) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs,
        Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs,
        Steinberg::int32 numOuts) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setProcessing(
        Steinberg::TBool state) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API canProcessSampleSize(
        Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;

    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API process(
        Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState(
        Steinberg::IBStream* state) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API getState(
        Steinberg::IBStream* state) SMTG_OVERRIDE;

private:
    static constexpr std::size_t kLatencySamples = 1024;
    static constexpr double kMaxResidualSamples = 256.0;

    double sampleRate = 48000.0;
    double residualSamples = 0.0;
    bool applying = false;

    std::vector<float> delayLine;
    std::size_t writeIndex = 0;

    void resetDelayLine();
    void ensureDelayLine();

    float readInterpolated(double index) const;

    void handleParameterChanges(
        Steinberg::Vst::ProcessData& data);

    static double normalizedToResidual(double value);
    static double residualToNormalized(double value);
};

}
