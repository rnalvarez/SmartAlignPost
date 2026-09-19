#include "align_engine.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

static std::vector<float> makeSignal(size_t n)
{
    std::vector<float> x(n);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < n; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        x[i] = static_cast<float>((state / 4294967295.0) * 2.0 - 1.0);
    }
    return x;
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr size_t durationSamples = 48000 * 4;
    constexpr int stepSamples = 4;
    constexpr int startDelay = 40;

    const auto dynMaster = makeSignal(durationSamples);
    const size_t blockSamples = static_cast<size_t>(0.48 * sr);
    std::vector<float> dynSource(dynMaster.size(), 0.0f);

    for (size_t i = 0; i < dynMaster.size(); ++i) {
        const int block = static_cast<int>(i / blockSamples);
        const int delayHere = startDelay + block * stepSamples;
        if (static_cast<long>(i) - delayHere >= 0)
            dynSource[i] = dynMaster[static_cast<size_t>(
                static_cast<long>(i) - delayHere)];
    }

    sap::Settings dynSettings;
    dynSettings.sampleRate = sr;
    dynSettings.mode = sap::Mode::Dynamic;
    dynSettings.maxDelayMs = 12.0;
    dynSettings.analysisWindowMs = 80.0;
    dynSettings.hopMs = 40.0;
    dynSettings.minConfidence = 0.80;
    dynSettings.smoothingMs = 60.0;
    dynSettings.maxSlewMsPerSecond = 120.0;

    std::cerr << "DIAG 1: before unseeded dynamic" << std::endl;
    const auto dynResult = sap::AlignEngine::analyze(
        dynMaster, dynSource, dynSettings);
    std::cerr << "DIAG 1: after unseeded dynamic curve="
              << dynResult.curve.size() << std::endl;

    if (dynResult.curve.size() < 5)
        return 2;

    sap::Settings seededSettings = dynSettings;
    seededSettings.hasInitialDelaySamples = true;
    seededSettings.initialDelaySamples = 52.0;

    const size_t chunkSamples = std::min(
        static_cast<size_t>(5.0 * sr), dynMaster.size());

    std::vector<float> seededMaster(
        dynMaster.begin(),
        dynMaster.begin() + static_cast<std::ptrdiff_t>(chunkSamples));
    std::vector<float> seededSource(
        dynSource.begin(),
        dynSource.begin() + static_cast<std::ptrdiff_t>(chunkSamples));

    std::cerr << "DIAG 2: before seeded dynamic" << std::endl;
    const auto seeded = sap::AlignEngine::analyze(
        seededMaster, seededSource, seededSettings);
    std::cerr << "DIAG 2: after seeded dynamic curve="
              << seeded.curve.size() << std::endl;

    if (seeded.curve.empty())
        return 4;

    return 0;
}
