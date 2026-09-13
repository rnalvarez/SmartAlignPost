#include "align_engine.h"
#include <cmath>
#include <iostream>
#include <cstdint>
#include <vector>

static std::vector<float> makeTestSignal(size_t n)
{
    // Deterministic pseudo-random sequence: a much better alignment test than a
    // periodic sine wave because it has a distinctive correlation peak.
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
    if (samples >= 0) {
        for (size_t i = static_cast<size_t>(samples); i < x.size(); ++i)
            y[i] = x[i - static_cast<size_t>(samples)];
    } else {
        const size_t d = static_cast<size_t>(-samples);
        for (size_t i = 0; i + d < x.size(); ++i)
            y[i] = x[i + d];
    }
    return y;
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr int knownDelay = 173;
    auto master = makeTestSignal(48000 * 2);
    auto source = delayed(master, knownDelay);

    sap::Settings s;
    s.sampleRate = sr;
    s.maxDelayMs = 10.0;
    s.mode = sap::Mode::Static;

    const auto r = sap::AlignEngine::analyze(master, source, s);
    if (std::abs(r.staticDelaySamples - knownDelay) > 1.0) {
        std::cerr << "Static delay test failed: got "
                  << r.staticDelaySamples << " expected " << knownDelay << "\n";
        return 1;
    }
    if (r.staticConfidence < 0.8) {
        std::cerr << "Confidence test failed: " << r.staticConfidence << "\n";
        return 1;
    }

    s.mode = sap::Mode::Dynamic;
    s.analysisWindowMs = 100.0;
    s.hopMs = 25.0;
    const auto d = sap::AlignEngine::analyze(master, source, s);
    if (d.curve.empty()) {
        std::cerr << "Dynamic curve is empty\n";
        return 1;
    }

    std::cout << "Smart Align Post DSP tests passed.\n";
    return 0;
}
