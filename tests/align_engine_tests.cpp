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

static std::vector<float> fractionallyDelayed(const std::vector<float>& x, double samples)
{
    std::vector<float> y(x.size(), 0.0f);
    const int whole = static_cast<int>(std::floor(samples));
    const double frac = samples - static_cast<double>(whole);
    for (size_t i = static_cast<size_t>(std::max(whole + 1, 1)); i < x.size(); ++i) {
        const size_t j = i - static_cast<size_t>(whole);
        const float a = x[j];
        const float b = j > 0 ? x[j - 1] : x[j];
        y[i] = static_cast<float>((1.0 - frac) * a + frac * b);
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

    std::cerr << "STAGE: baseline\\n" << std::flush;

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

    std::cerr << "STAGE: distance sweep\\n" << std::flush;

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

    std::cerr << "STAGE: dynamic regression\\n" << std::flush;

    // Dynamic mode regression: it must still expose a non-empty delay curve.
    s.mode = sap::Mode::Dynamic;
    s.analysisWindowMs = 100.0;
    s.hopMs = 25.0;
    const auto d = sap::AlignEngine::analyze(master, source, s);
    if (d.curve.empty()) {
        std::cerr << "Dynamic curve is empty\n";
        return 5;
    }

    std::cerr << "STAGE: short buffer\\n" << std::flush;

    // Short-buffer regression (analyze()'s n < win branch): this path had
    // zero test coverage before. A clip shorter than one analysis window
    // must still resolve the correct delay when maxLag is a large fraction
    // of n. (The FFT padding change in gccPhatDelay is a defensive fix for
    // circular-wraparound risk in this regime, justified on first
    // principles; this test did not manage to construct a case where it
    // changes the outcome for broadband content, so treat it as closing a
    // coverage gap rather than as proof the old code returned a wrong
    // delay here.)
    {
        constexpr size_t shortN = 1024; // already a power of two: zero margin pre-fix
        auto shortMaster = makeTestSignal(shortN);
        constexpr int shortDelay = 300; // close to shortN/2, exercises the maxLag clamp
        auto shortSource = delayed(shortMaster, shortDelay);

        sap::Settings shortSettings;
        shortSettings.sampleRate = sr;
        shortSettings.maxDelayMs = 12.0; // requests maxLag=576, clamped to n/2=512
        shortSettings.mode = sap::Mode::Static;

        const auto shortResult = sap::AlignEngine::analyze(shortMaster, shortSource, shortSettings);
        if (!approx(shortResult.staticDelaySamples, shortDelay, 1.0)) {
            std::cerr << "Short-buffer delay test failed: got "
                      << shortResult.staticDelaySamples << " expected " << shortDelay << "\n";
            return 6;
        }
    }

    std::cerr << "STAGE: dynamic tracking\\n" << std::flush;

    // Dynamic tracking regression: the curve must follow a genuinely
    // time-varying delay, not just exist (the earlier empty-curve check
    // would pass even if every point were frozen at the wrong value). Uses
    // the same analysis settings as the production CLI.
    {
        auto dynMaster = makeTestSignal(durationSamples);
        const size_t blockSamples = static_cast<size_t>(0.48 * sr);
        constexpr int stepSamples = 4;
        constexpr int startDelay = 40;
        std::vector<float> dynSource(dynMaster.size(), 0.0f);
        for (size_t i = 0; i < dynMaster.size(); ++i) {
            const int block = static_cast<int>(i / blockSamples);
            const int delayHere = startDelay + block * stepSamples;
            if (static_cast<long>(i) - delayHere >= 0)
                dynSource[i] = dynMaster[static_cast<size_t>(static_cast<long>(i) - delayHere)];
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

        const auto dynResult = sap::AlignEngine::analyze(dynMaster, dynSource, dynSettings);
        if (dynResult.curve.size() < 5) {
            std::cerr << "Dynamic tracking test: curve too short (" << dynResult.curve.size() << " points)\n";
            return 7;
        }
        double maxErr = 0.0;
        for (const auto& p : dynResult.curve) {
            const size_t sampleAtT = static_cast<size_t>(p.timeSec * sr);
            const int block = static_cast<int>(sampleAtT / blockSamples);
            const double truth = startDelay + block * stepSamples;
            maxErr = std::max(maxErr, std::abs(p.delaySamples - truth));
        }
        // A brief settle time right at a step boundary is expected; a real
        // tracking failure misses by tens of samples, not a handful.
        if (maxErr > 8.0) {
            std::cerr << "Dynamic tracking test failed: max error " << maxErr << " samples\n";
            return 8;
        }
        std::cout << "DYNAMIC_TRACKING max_error=" << maxErr << " samples over "
                  << dynResult.curve.size() << " points\n";

        // Cross-chunk seed regression: the second analysis starts from the
        // last delay of the previous chunk instead of re-initializing from
        // an unrelated local warm-up window.
        sap::Settings seededSettings = dynSettings;
        seededSettings.hasInitialDelaySamples = true;
        seededSettings.initialDelaySamples = 52.0;
        const size_t chunkSamples = std::min(
            static_cast<size_t>(5.0 * sr), dynMaster.size());
        std::vector<float> seededMaster(dynMaster.begin(),
                                        dynMaster.begin() + static_cast<std::ptrdiff_t>(chunkSamples));
        std::vector<float> seededSource(dynSource.begin(),
                                        dynSource.begin() + static_cast<std::ptrdiff_t>(chunkSamples));
        const auto seeded = sap::AlignEngine::analyze(seededMaster, seededSource, seededSettings);
        if (seeded.curve.empty()) {
            std::cerr << "Seeded dynamic curve is empty\n";
            return 12;
        }
        const double firstSeeded = seeded.curve.front().delaySamples;
        if (std::abs(firstSeeded - seededSettings.initialDelaySamples) > 8.0) {
            std::cerr << "Seeded dynamic continuity failed: first="
                      << firstSeeded << " seed="
                      << seededSettings.initialDelaySamples << "\n";
            return 13;
        }
    }

    std::cerr << "STAGE: constant dynamic\\n" << std::flush;

    // Constant-delay DYNAMIC regression using the production settings.
    // The new dynamic path seeds from a short warm-up instead of running a
    // full STATIC pass, so a constant source must still stay locked to the
    // known MASTER delay.
    {
        constexpr int dynamicConstantDelay = 173;
        auto constantSource = delayed(master, dynamicConstantDelay);

        sap::Settings dynamicConstant;
        dynamicConstant.sampleRate = sr;
        dynamicConstant.mode = sap::Mode::Dynamic;
        dynamicConstant.maxDelayMs = 12.0;
        dynamicConstant.analysisWindowMs = 80.0;
        dynamicConstant.hopMs = 40.0;
        dynamicConstant.minConfidence = 0.80;
        dynamicConstant.smoothingMs = 60.0;
        dynamicConstant.maxSlewMsPerSecond = 120.0;

        const auto constantResult =
            sap::AlignEngine::analyze(master, constantSource, dynamicConstant);
        if (constantResult.curve.size() < 5) {
            std::cerr << "Dynamic constant-delay curve too short: "
                      << constantResult.curve.size() << " points\n";
            return 9;
        }
        double maxConstantError = 0.0;
        for (const auto& p : constantResult.curve)
            maxConstantError = std::max(
                maxConstantError, std::abs(p.delaySamples - dynamicConstantDelay));

        if (maxConstantError > 1.5) {
            std::cerr << "Dynamic constant-delay tracking failed: max error "
                      << maxConstantError << " samples\n";
            return 10;
        }
    }

    std::cerr << "STAGE: fractional phase\\n" << std::flush;

    // Phase-slope refinement regression: a fractional delay should no longer
    // collapse to the nearest sample. The test signal is broadband and the
    // expected residual is deliberately tighter than one whole sample.
    {
        constexpr double fractionalDelay = 56.30;
        auto fracSource = fractionallyDelayed(master, fractionalDelay);
        sap::Settings fractional;
        fractional.sampleRate = sr;
        fractional.maxDelayMs = 12.0;
        fractional.analysisWindowMs = 120.0;
        fractional.hopMs = 40.0;
        fractional.mode = sap::Mode::Dynamic;

        const auto fr = sap::AlignEngine::analyze(master, fracSource, fractional);
        if (fr.curve.empty()) {
            std::cerr << "Fractional phase regression: empty curve\n";
            return 14;
        }
        double maxFractionalError = 0.0;
        for (const auto& p : fr.curve)
            maxFractionalError = std::max(
                maxFractionalError, std::abs(p.delaySamples - fractionalDelay));

        if (maxFractionalError > 0.35) {
            std::cerr << "Fractional phase refinement failed: max error "
                      << maxFractionalError << " samples\n";
            return 15;
        }
        std::cout << "FRACTIONAL_PHASE max_error="
                  << maxFractionalError << " samples\n";
    }

    std::cerr << "STAGE: 44.1k\\n" << std::flush;

    // Non-48 kHz regression: estimateDelay() must use the caller-provided
    // sample rate for its DSP path rather than assuming 48 kHz.
    {
        constexpr double sr44 = 44100.0;
        constexpr int delay44 = 117;
        auto master44 = makeTestSignal(44100);
        auto source44 = delayed(master44, delay44);
        double confidence44 = 0.0;
        const double measured44 = sap::AlignEngine::estimateDelay(
            master44.data(), source44.data(), master44.size(),
            220, sr44, confidence44);

        if (!approx(measured44, delay44, 1.0) || confidence44 < 0.8) {
            std::cerr << "44.1 kHz estimateDelay failed: got "
                      << measured44 << " expected " << delay44
                      << " confidence=" << confidence44 << "\n";
            return 11;
        }
    }

    std::cout << "Smart Align Post DSP tests passed.\n";
    return 0;
}
