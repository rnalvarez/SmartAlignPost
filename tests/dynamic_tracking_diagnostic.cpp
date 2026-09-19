#include "align_engine.h"

#include <algorithm>
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
    constexpr size_t n = 48000; // 1 second: enough for several production windows
    constexpr size_t block = static_cast<size_t>(0.48 * sr);
    constexpr int startDelay = 40;
    constexpr int stepDelay = 4;

    const auto master = makeSignal(n);
    std::vector<float> source(n, 0.0f);

    for (size_t i = 0; i < n; ++i) {
        const int blockIndex = static_cast<int>(i / block);
        const int delay = startDelay + blockIndex * stepDelay;
        if (static_cast<long>(i) - delay >= 0)
            source[i] = master[static_cast<size_t>(static_cast<long>(i) - delay)];
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

    std::cerr << "DIAG: before analyze" << std::endl;
    const auto result = sap::AlignEngine::analyze(master, source, s);
    std::cerr << "DIAG: after analyze curve=" << result.curve.size() << std::endl;

    if (result.curve.empty()) {
        std::cerr << "DIAG: empty curve" << std::endl;
        return 1;
    }

    double maxError = 0.0;
    for (const auto& p : result.curve) {
        const size_t sampleAtT = static_cast<size_t>(p.timeSec * sr);
        const int blockIndex = static_cast<int>(sampleAtT / block);
        const double truth = startDelay + blockIndex * stepDelay;
        maxError = std::max(maxError, std::abs(p.delaySamples - truth));
    }

    std::cout << "DIAG_DYNAMIC max_error=" << maxError
              << " points=" << result.curve.size() << std::endl;

    return maxError <= 8.0 ? 0 : 2;
}
