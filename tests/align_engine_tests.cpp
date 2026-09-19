#include "align_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

static double lagrange4(
    const std::vector<float>& x,
    double pos)
{
    if (pos < 1.0 ||
        pos >= static_cast<double>(x.size() - 2))
        return 0.0;

    const std::size_t i =
        static_cast<std::size_t>(std::floor(pos));

    const double f =
        pos - static_cast<double>(i);

    const double c0 =
        -f * (f - 1.0) * (f - 2.0) / 6.0;
    const double c1 =
        (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
    const double c2 =
        -(f + 1.0) * f * (f - 2.0) / 2.0;
    const double c3 =
        (f + 1.0) * f * (f - 1.0) / 6.0;

    return c0 * x[i - 1] +
           c1 * x[i] +
           c2 * x[i + 1] +
           c3 * x[i + 2];
}

static std::vector<float> makeBroadbandSignal(
    std::size_t n)
{
    std::mt19937 rng(0x12345678u);
    std::normal_distribution<double> noise(0.0, 1.0);

    std::vector<float> x(n);
    double low = 0.0;

    for (std::size_t i = 0; i < n; ++i) {
        low = 0.995 * low + 0.005 * noise(rng);
        const double wide = noise(rng);
        x[i] = static_cast<float>(
            0.72 * wide + 0.28 * low);
    }

    return x;
}

static std::vector<float> delaySignal(
    const std::vector<float>& x,
    double delaySamples)
{
    std::vector<float> y(x.size(), 0.0f);

    for (std::size_t i = 3; i + 2 < x.size(); ++i) {
        const double sourcePos =
            static_cast<double>(i) - delaySamples;

        y[i] = static_cast<float>(
            lagrange4(x, sourcePos));
    }

    return y;
}

static std::vector<float> varyingDelaySignal(
    const std::vector<float>& x,
    double startDelay,
    double endDelay)
{
    std::vector<float> y(x.size(), 0.0f);

    const double denom =
        static_cast<double>(std::max<std::size_t>(
            1, x.size() - 1));

    for (std::size_t i = 3; i + 2 < x.size(); ++i) {
        const double u =
            static_cast<double>(i) / denom;

        const double delay =
            startDelay +
            (endDelay - startDelay) * u;

        const double sourcePos =
            static_cast<double>(i) - delay;

        y[i] = static_cast<float>(
            lagrange4(x, sourcePos));
    }

    return y;
}

static bool approx(
    double a,
    double b,
    double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

int main()
{
    constexpr double sr = 48000.0;

    std::cerr << "PHASE TEST: integer delay\n";

    const auto master =
        makeBroadbandSignal(
            static_cast<std::size_t>(sr * 6.0));

    {
        const double expected = 173.0;
        const auto source =
            delaySignal(master, expected);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Static;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (!approx(
                r.staticDelaySamples,
                expected,
                0.75)) {
            std::cerr
                << "integer delay failed: got "
                << r.staticDelaySamples
                << " expected "
                << expected << "\n";
            return 1;
        }

        if (r.staticConfidence < 0.75) {
            std::cerr
                << "integer confidence failed: "
                << r.staticConfidence << "\n";
            return 2;
        }
    }

    std::cerr << "PHASE TEST: fractional delay\n";

    {
        const double expected = 56.35;
        const auto source =
            delaySignal(master, expected);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Static;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 200.0;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (!approx(
                r.staticDelaySamples,
                expected,
                0.80)) {
            std::cerr
                << "fractional delay failed: got "
                << r.staticDelaySamples
                << " expected "
                << expected << "\n";
            return 3;
        }
    }

    std::cerr << "PHASE TEST: changing delay\n";

    {
        constexpr double startDelay = 42.0;
        constexpr double endDelay = 260.0;

        const auto source =
            varyingDelaySignal(
                master,
                startDelay,
                endDelay);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Dynamic;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.68;
        s.maxSlewMsPerSecond = 80.0;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.curve.size() < 4) {
            std::cerr
                << "dynamic curve too short: "
                << r.curve.size() << "\n";
            return 4;
        }

        double firstError =
            std::numeric_limits<double>::infinity();
        double lastError =
            std::numeric_limits<double>::infinity();

        firstError = std::abs(
            r.curve.front().delaySamples -
            startDelay);

        lastError = std::abs(
            r.curve.back().delaySamples -
            endDelay);

        if (firstError > 8.0 ||
            lastError > 8.0) {
            std::cerr
                << "dynamic endpoints failed: first="
                << r.curve.front().delaySamples
                << " last="
                << r.curve.back().delaySamples
                << " expected="
                << startDelay << " -> "
                << endDelay << "\n";
            return 5;
        }
    }

    std::cerr << "PHASE TEST: noisy source\n";

    {
        const double expected = 121.5;
        auto source =
            delaySignal(master, expected);

        std::mt19937 rng(77u);
        std::normal_distribution<double> noise(0.0, 0.08);

        for (auto& sample : source)
            sample = static_cast<float>(
                sample + noise(rng));

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Static;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 80.0;
        s.hopMs = 250.0;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (!approx(
                r.staticDelaySamples,
                expected,
                1.2)) {
            std::cerr
                << "noisy delay failed: got "
                << r.staticDelaySamples
                << " expected "
                << expected << "\n";
            return 6;
        }
    }

    std::cerr << "PHASE TEST: stable static should not become dynamic\n";

    {
        const double expected = 94.0;
        const auto source =
            delaySignal(master, expected);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Static ||
            !r.curve.empty()) {
            std::cerr
                << "AUTO incorrectly selected DYNAMIC\n";
            return 7;
        }
    }

    std::cerr << "ALL PHASE TESTS PASSED\n";
    return 0;
}
