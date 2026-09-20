#include "processor.h"
#include "public.sdk/source/vst/ivstparameterchanges.h"
#include "base/source/fstreamer.h"
#include <algorithm>
#include <cmath>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace SmartAlignPost {

namespace {

constexpr std::size_t kRingExtraSamples = 16;

std::size_t wrapIndex(std::int64_t value, std::size_t size) {
    const auto mod = static_cast<std::int64_t>(size);
    value %= mod;
    if (value < 0)
        value += mod;
    return static_cast<std::size_t>(value);
}

} // namespace

Processor::Processor() {
    setControllerClass(ControllerUID);
}

FUnknown* Processor::createInstance(void*) {
    return (IAudioProcessor*)new Processor();
}

tresult PLUGIN_API Processor::initialize(FUnknown* context) {
    const auto r = AudioEffect::initialize(context);
    if (r != kResultOk)
        return r;

    // The residual processor is intentionally mono. REAPER can place this
    // instance on a single Take FX item without any per-track multichannel
    // routing requirements.
    addAudioInput(
        STR16("Lav Input"),
        SpeakerArr::kMono);

    addAudioOutput(
        STR16("Corrected Output"),
        SpeakerArr::kMono);

    ensureDelayLine();

    return kResultOk;
}

tresult PLUGIN_API Processor::terminate() {
    delayLine.clear();
    writeIndex = 0;
    return AudioEffect::terminate();
}

tresult PLUGIN_API Processor::setBusArrangements(
    SpeakerArrangement* inputs,
    int32 numIns,
    SpeakerArrangement* outputs,
    int32 numOuts) {

    if (numIns == 1 &&
        numOuts == 1 &&
        inputs[0] == SpeakerArr::kMono &&
        outputs[0] == SpeakerArr::kMono) {
        return AudioEffect::setBusArrangements(
            inputs, numIns, outputs, numOuts);
    }

    return kResultFalse;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    settings.sampleRate = sampleRate;
    ensureDelayLine();
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setProcessing(TBool state) {
    if (state)
        resetDelayLine();

    return AudioEffect::setProcessing(state);
}

tresult PLUGIN_API Processor::canProcessSampleSize(
    int32 symbolicSampleSize) {

    return symbolicSampleSize == kSample32
        ? kResultTrue
        : kResultFalse;
}

uint32 PLUGIN_API Processor::getLatencySamples() {
    return static_cast<uint32>(kLatencySamples);
}

void Processor::ensureDelayLine() {
    const auto required =
        kLatencySamples +
        static_cast<std::size_t>(2.0 * kMaxResidualSamples) +
        kRingExtraSamples;

    if (delayLine.size() != required) {
        delayLine.assign(required, 0.0f);
        writeIndex = 0;
    }
}

void Processor::resetDelayLine() {
    std::fill(delayLine.begin(), delayLine.end(), 0.0f);
    writeIndex = 0;
}

double Processor::normalizedToResidual(double value) {
    const auto clamped = std::clamp(value, 0.0, 1.0);
    return -kMaxResidualSamples +
           clamped * (2.0 * kMaxResidualSamples);
}

double Processor::residualToNormalized(double value) {
    const auto clamped =
        std::clamp(value, -kMaxResidualSamples, kMaxResidualSamples);

    return (clamped + kMaxResidualSamples) /
           (2.0 * kMaxResidualSamples);
}

void Processor::handleParameterChanges(ProcessData& data) {
    if (!data.inputParameterChanges)
        return;

    const int32 count =
        data.inputParameterChanges->getParameterCount();

    for (int32 i = 0; i < count; ++i) {
        auto* queue =
            data.inputParameterChanges->getParameterData(i);

        if (!queue)
            continue;

        ParamValue value = 0.0;
        int32 sampleOffset = 0;

        for (int32 point = 0;
             point < queue->getPointCount();
             ++point) {

            if (queue->getPoint(
                    point,
                    sampleOffset,
                    value) != kResultTrue) {
                continue;
            }

            switch (queue->getParameterId()) {
                case kResidualSamplesParam:
                    residualSamples =
                        normalizedToResidual(value);
                    break;

                case kApplyResidualParam:
                    applying = value >= 0.5;
                    break;

                default:
                    break;
            }
        }
    }
}

float Processor::readInterpolated(double index) const {
    if (delayLine.empty())
        return 0.0f;

    const auto base =
        static_cast<std::int64_t>(std::floor(index));

    const double frac =
        index - static_cast<double>(base);

    // 4-point Lagrange interpolation around the desired fractional sample.
    const double c0 =
        -frac * (frac - 1.0) * (frac - 2.0) / 6.0;

    const double c1 =
        (frac + 1.0) * (frac - 1.0) * (frac - 2.0) / 2.0;

    const double c2 =
        -(frac + 1.0) * frac * (frac - 2.0) / 2.0;

    const double c3 =
        (frac + 1.0) * frac * (frac - 1.0) / 6.0;

    const auto i0 = wrapIndex(base - 1, delayLine.size());
    const auto i1 = wrapIndex(base,     delayLine.size());
    const auto i2 = wrapIndex(base + 1, delayLine.size());
    const auto i3 = wrapIndex(base + 2, delayLine.size());

    return static_cast<float>(
        c0 * delayLine[i0] +
        c1 * delayLine[i1] +
        c2 * delayLine[i2] +
        c3 * delayLine[i3]);
}

tresult PLUGIN_API Processor::process(ProcessData& data) {
    handleParameterChanges(data);

    if (!data.inputs ||
        !data.outputs ||
        data.numInputs < 1 ||
        data.numOutputs < 1 ||
        data.inputs[0].numChannels < 1 ||
        data.outputs[0].numChannels < 1) {
        return kResultOk;
    }

    auto& in = data.inputs[0];
    auto& out = data.outputs[0];

    if (!in.channelBuffers32 ||
        !out.channelBuffers32) {
        return kResultFalse;
    }

    const float* input = in.channelBuffers32[0];
    float* output = out.channelBuffers32[0];

    const int32 numSamples = data.numSamples;

    for (int32 n = 0; n < numSamples; ++n) {
        delayLine[writeIndex] = input[n];

        const double readPosition =
            static_cast<double>(writeIndex) -
            static_cast<double>(kLatencySamples) +
            (applying ? residualSamples : 0.0);

        output[n] = readInterpolated(readPosition);

        writeIndex =
            (writeIndex + 1) % delayLine.size();
    }

    return kResultOk;
}

tresult PLUGIN_API Processor::setState(IBStream* state) {
    if (!state)
        return kResultFalse;

    IBStreamer streamer(state);

    int32 version = 0;
    float savedResidual = 0.0f;
    bool savedApplying = false;

    if (!streamer.readInt32(version) ||
        !streamer.readFloat(savedResidual) ||
        !streamer.readBool(savedApplying)) {
        return kResultFalse;
    }

    if (version != 1)
        return kResultFalse;

    residualSamples =
        std::clamp(
            static_cast<double>(savedResidual),
            -kMaxResidualSamples,
            kMaxResidualSamples);

    applying = savedApplying;

    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state) {
    if (!state)
        return kResultFalse;

    IBStreamer streamer(state);

    if (!streamer.writeInt32(1) ||
        !streamer.writeFloat(
            static_cast<float>(residualSamples)) ||
        !streamer.writeBool(applying)) {
        return kResultFalse;
    }

    return kResultOk;
}

}
