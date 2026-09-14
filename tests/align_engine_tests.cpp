#include "align_engine.h"
#include <cmath>
#include <iostream>
#include <cstdint>
#include <vector>

static std::vector<float> makeTestSignal(size_t n)
{
    // Deterministic broadband signal: a distinctive correlation peak without
    // relying on any external WAV files.
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

static bool approx(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr double speedOfSound = 343.0; // m/s, nominal test value
    constexpr size_t durationSamples = 48000 * 4;

    // Baseline exact delay test.
    constexpr int knownDelay = 173;
    auto master = makeTestSignal(durationSamples);
    auto source = delayed(master, knownDelay);

    sap::Settings s;
    s.sampleRate = sr;
    s.maxDelayMs = 10.0;
    s.mode = sap::Mode::Static;

    const auto r = sap::AlignEngine::analyze(master, source, s);
    if (!approx(r.staticDelaySamples, knownDelay, 1.0)) {
        std::cerr << "Static delay test failed: got "
                  << r.staticDelaySamples << " expected " << knownDelay << "\n";
        return 1;
    }
    if (r.staticConfidence < 0.8) {
        std::cerr << "Confidence test failed: " << r.staticConfidence << "\n";
        return 1;
    }

    // Acoustic distance sweep. Two clips have identical duration and identical
    // source content; SOURCE is delayed only by the propagation time created by
    // a microphone spacing distance d. This is the core physical test for the
    // static alignment prototype.
    const std::vector<double> distancesMeters = {0.02, 0.05, 0.10, 0.20, 0.50};
    for (double distance : distancesMeters) {
        const double expectedSamplesExact = distance / speedOfSound * sr;
        const int expectedSamples = static_cast<int>(std::lround(expectedSamplesExact));
        const auto distanceSource = delayed(master, expectedSamples);

        sap::Settings distanceSettings;
        distanceSettings.sampleRate = sr;
        distanceSettings.maxDelayMs = 12.0;
        distanceSettings.analysisWindowMs = 200.0;
        distanceSettings.hopMs = 50.0;
        distanceSettings.mode = sap::Mode::Static;

        const auto dr = sap::AlignEngine::analyze(master, distanceSource, distanceSettings);
        if (!approx(dr.staticDelaySamples, expectedSamples, 1.0)) {
            std::cerr << "Distance alignment test failed: distance=" << distance
                      << " m expected=" << expectedSamples
                      << " samples got=" << dr.staticDelaySamples << "\n";
            return 2;
        }
        if (dr.staticConfidence < 0.8) {
            std::cerr << "Distance confidence test failed: distance=" << distance
                      << " m confidence=" << dr.staticConfidence << "\n";
            return 3;
        }
        if (dr.staticTotalWindows <= 0 || dr.staticSupportWindows <= 0) {
            std::cerr << "Distance support test failed: distance=" << distance
                      << " support=" << dr.staticSupportWindows
                      << "/" << dr.staticTotalWindows << "\n";
            return 4;
        }

        std::cout << "DISTANCE=" << distance
                  << "m EXPECTED_SAMPLES=" << expectedSamples
                  << " EXPECTED_MS=" << (expectedSamplesExact * 1000.0 / sr)
                  << " GOT_SAMPLES=" << dr.staticDelaySamples
                  << " CONFIDENCE=" << dr.staticConfidence
                  << " SUPPORT=" << dr.staticSupportWindows
                  << "/" << dr.staticTotalWindows << "\n";
    }

    // Dynamic mode regression: it must still expose a non-empty delay curve.
    s.mode = sap::Mode::Dynamic;
    s.analysisWindowMs = 100.0;
    s.hopMs = 25.0;
    const auto d = sap::AlignEngine::analyze(master, source, s);
    if (d.curve.empty()) {
        std::cerr << "Dynamic curve is empty\n";
        return 5;
    }

    std::cout << "Smart Align Post DSP tests passed.\n";
    return 0;
}
