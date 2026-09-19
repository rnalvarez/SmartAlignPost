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

static std::vector<float> delayed(const std::vector<float>& x, int samples)
{
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = static_cast<size_t>(samples); i < x.size(); ++i)
        y[i] = x[i - static_cast<size_t>(samples)];
    return y;
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr size_t durationSamples = 48000 * 4;
    constexpr int stepSamples = 4;
    constexpr int startDelay = 40;

    const auto master = makeSignal(durationSamples);
    std::vector<float> source(master.size(), 0.0f);
    const size_t blockSamples = static_cast<size_t>(0.48 * sr);

    for (size_t i = 0; i < master.size(); ++i) {
        const int block = static_cast<int>(i / blockSamples);
        const int delayHere = startDelay + block * stepSamples;
        if (static_cast<long>(i) - delayHere >= 0)
            source[i] = master[static_cast<size_t>(
                static_cast<long>(i) - delayHere)];
    }

    sap::Settings s;
    s.sampleRate = sr;
    s.mode = sap::Mode::Dynamic;
    s.maxDelayMs = 12.0;
    s.analysisWindowMs = 80.0;
    s.hopMs = 40.0;
    s.minConfidence = 0.80;
    s.smoothingMs = 60.0;
    s.maxSlewMsPerSecond = 120.0;

    std::cerr << "DIAG: before Dynamic analyze" << std::endl;
    const auto result = sap::AlignEngine::analyze(master, source, s);
    std::cerr << "DIAG: after Dynamic analyze curve="
              << result.curve.size() << std::endl;

    if (result.curve.size() < 5)
        return 2;

    double maxErr = 0.0;
    for (const auto& p : result.curve) {
        const size_t sampleAtT = static_cast<size_t>(p.timeSec * sr);
        const int block = static_cast<int>(sampleAtT / blockSamples);
        const double truth = startDelay + block * stepSamples;
        maxErr = std::max(maxErr, std::abs(p.delaySamples - truth));
    }

    std::cerr << "DIAG: maxErr=" << maxErr << std::endl;
    return maxErr <= 8.0 ? 0 : 3;
}
