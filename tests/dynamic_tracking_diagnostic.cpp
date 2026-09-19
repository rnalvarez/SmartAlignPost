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
    constexpr size_t n = 4096;
    constexpr int delay = 173;

    const auto master = makeSignal(n);
    const auto source = delayed(master, delay);

    std::cerr << "DIAG 1: before estimateDelay" << std::endl;
    double confidence = 0.0;
    const double measured = sap::AlignEngine::estimateDelay(
        master.data(), source.data(), master.size(), 576, sr, confidence);
    std::cerr << "DIAG 1: after estimateDelay delay=" << measured
              << " confidence=" << confidence << std::endl;

    if (std::abs(measured - delay) > 1.0)
        return 1;

    sap::Settings s;
    s.sampleRate = sr;
    s.mode = sap::Mode::Dynamic;
    s.maxDelayMs = 12.0;
    s.analysisWindowMs = 80.0;
    s.hopMs = 40.0;
    s.minConfidence = 0.80;
    s.smoothingMs = 60.0;
    s.maxSlewMsPerSecond = 120.0;

    std::cerr << "DIAG 2: before tiny Dynamic analyze" << std::endl;
    const auto result = sap::AlignEngine::analyze(master, source, s);
    std::cerr << "DIAG 2: after tiny Dynamic analyze curve="
              << result.curve.size() << std::endl;

    if (result.curve.empty())
        return 2;

    return 0;
}
