#include "align_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
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

static std::vector<float> makeImpulseReverbSignal(
    std::size_t n,
    std::size_t onset,
    double sampleRate)
{
    std::vector<float> x(n, 0.0f);

    std::mt19937 rng(0x51A7C1Au);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    for (std::size_t i = onset;
         i < n;
         ++i) {
        const double t =
            static_cast<double>(i - onset) /
            sampleRate;

        // Broadband direct transient: strong attack concentrated in the
        // first few milliseconds.
        const double direct =
            t < 0.008
                ? 1.4 *
                    std::exp(-t / 0.0018) *
                    (0.70 + 0.30 * noise(rng))
                : 0.0;

        // Independent, decaying room tail. It deliberately occupies much
        // more of the recording than the direct sound but is not identical
        // between microphones.
        const double room =
            0.34 *
            std::exp(-t / 0.115) *
            noise(rng);

        const double earlyReflection =
            (t >= 0.018 && t < 0.026)
                ? 0.42 *
                    std::exp(
                        -(t - 0.018) / 0.003) *
                    (0.7 + 0.3 * noise(rng))
                : 0.0;

        x[i] = static_cast<float>(
            direct +
            room +
            earlyReflection);
    }

    return x;
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

    std::cerr
        << "PHASE TEST: impulsive transient with reverb\n";

    {
        constexpr std::size_t n =
            static_cast<std::size_t>(
                sr * 1.45);

        constexpr std::size_t onset =
            static_cast<std::size_t>(
                sr * 0.310);

        constexpr double expected =
            -339.0;

        const auto master =
            makeImpulseReverbSignal(
                n,
                onset,
                sr);

        auto source =
            delaySignal(
                master,
                expected);

        // Give SOURCE a different reverberant tail while keeping the first
        // direct arrival strongly correlated. This models the boom/lav
        // acoustic-path difference that defeats full-band PHAT matching.
        std::mt19937 rng(0xA17C0DEu);
        std::normal_distribution<double> roomNoise(
            0.0,
            0.10);

        for (std::size_t i = onset;
             i < n;
             ++i) {
            const double t =
                static_cast<double>(i - onset) /
                sr;

            if (t > 0.008) {
                source[i] = static_cast<float>(
                    0.55 * source[i] +
                    0.45 *
                        std::exp(-t / 0.11) *
                        roomNoise(rng));
            }
        }

        sap::Settings s;
        s.sampleRate = sr;
        s.maxDelayMs = 40.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.72;

        s.mode = sap::Mode::Static;
        const auto rStatic =
            sap::AlignEngine::analyze(
                master,
                source,
                s);

        if (!approx(
                rStatic.staticDelaySamples,
                expected,
                10.0)) {
            std::cerr
                << "impulsive static delay failed: got "
                << rStatic.staticDelaySamples
                << " expected "
                << expected
                << " support="
                << rStatic.staticSupportWindows
                << " total="
                << rStatic.staticTotalWindows
                << " MAD="
                << rStatic.staticDelayMADSamples
                << "\n";
            return 17;
        }

        if (rStatic.staticSupportWindows < 1 ||
            rStatic.staticConfidence > 0.35) {
            std::cerr
                << "impulsive confidence redundancy failed: support="
                << rStatic.staticSupportWindows
                << " confidence="
                << rStatic.staticConfidence
                << "\n";
            return 18;
        }

        s.mode = sap::Mode::Auto;
        const auto rAuto =
            sap::AlignEngine::analyze(
                master,
                source,
                s);

        if (rAuto.modeUsed != sap::Mode::Static ||
            !rAuto.evidenceInsufficient ||
            rAuto.scoutPoints >= 4) {
            std::cerr
                << "impulsive AUTO evidence gate failed: mode="
                << (rAuto.modeUsed == sap::Mode::Dynamic
                        ? "DYNAMIC"
                        : "STATIC")
                << " evidenceInsufficient="
                << (rAuto.evidenceInsufficient ? 1 : 0)
                << " scoutPoints="
                << rAuto.scoutPoints
                << "\n";
            return 19;
        }
    }

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

        const auto expectedAt = [&](double timeSec) {
            const double durationSec =
                static_cast<double>(master.size() - 1) / sr;
            const double u =
                std::clamp(timeSec / durationSec, 0.0, 1.0);
            return startDelay +
                   (endDelay - startDelay) * u;
        };

        double maxError = 0.0;
        for (const auto& point : r.curve) {
            const double expected =
                expectedAt(point.timeSec);
            maxError = std::max(
                maxError,
                std::abs(point.delaySamples - expected));
        }

        if (maxError > 8.0) {
            std::cerr
                << "dynamic trajectory failed: max error="
                << maxError << " samples\n";
            return 5;
        }
    }

    std::cerr << "PHASE TEST: AUTO detects gradual drift\n";

    {
        constexpr double startDelay = 48.0;
        constexpr double endDelay = 228.0;

        const auto source =
            varyingDelaySignal(
                master,
                startDelay,
                endDelay);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.68;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Dynamic ||
            r.curve.size() < 4) {
            std::cerr
                << "AUTO drift detection failed: mode="
                << (r.modeUsed == sap::Mode::Dynamic ? "DYNAMIC" : "STATIC")
                << " curve="
                << r.curve.size()
                << "\n";
            return 8;
        }

        if (r.scoutPoints < 4 ||
            !r.scoutCoherent) {
            std::cerr
                << "AUTO drift scout failed: points="
                << r.scoutPoints
                << " coherent="
                << (r.scoutCoherent ? 1 : 0)
                << " R2="
                << r.scoutR2
                << " direction="
                << r.scoutDirectionConsistency
                << "\n";
            return 16;
        }

        const double first = r.curve.front().delaySamples;
        const double last = r.curve.back().delaySamples;

        if (last - first < 120.0) {
            std::cerr
                << "AUTO drift trajectory too small: first="
                << first
                << " last="
                << last
                << "\n";
            return 9;
        }
    }

    std::cerr << "PHASE TEST: known playback-rate drift stays dynamic\n";

    {
        const double expected = 120.0;
        const auto source =
            delaySignal(master, expected);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.72;
        s.playbackRateRatio = 0.999;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Dynamic ||
            r.curve.size() < 2) {
            std::cerr
                << "known-rate drift not kept dynamic: curve="
                << r.curve.size()
                << "\n";
            return 10;
        }

        const double drift =
            r.curve.back().delaySamples -
            r.curve.front().delaySamples;

        // The test take is 6 s long, so a 0.999 playback-rate ratio
        // accumulates about 288 samples of project-time drift.
        if (drift < 220.0 || drift > 360.0) {
            std::cerr
                << "known-rate drift unexpected: "
                << drift << " samples\n";
            return 11;
        }
    }

    std::cerr << "PHASE TEST: playback-rate drift above 1.0 uses inverse slope\n";

    {
        const double expected = 120.0;
        const double ratio = 1.007463;

        const auto source =
            delaySignal(master, expected);

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.72;
        s.playbackRateRatio = ratio;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Dynamic ||
            r.curve.size() < 2) {
            std::cerr
                << "above-1 playback-rate drift not dynamic: curve="
                << r.curve.size()
                << "\n";
            return 14;
        }

        const double curveDurationSec =
            r.curve.back().timeSec -
            r.curve.front().timeSec;

        const double expectedDrift =
            (1.0 / ratio - 1.0) *
            curveDurationSec *
            sr;

        const double actualDrift =
            r.curve.back().delaySamples -
            r.curve.front().delaySamples;

        if (std::abs(actualDrift - expectedDrift) > 8.0) {
            std::cerr
                << "above-1 playback-rate slope failed: actual="
                << actualDrift
                << " expected="
                << expectedDrift
                << " curveDuration="
                << curveDurationSec
                << " s\n";
            return 15;
        }
    }

    std::cerr << "PHASE TEST: AUTO accepts monotonic nonlinear drift\n";

    {
        const double durationSamples =
            static_cast<double>(master.size() - 1);

        std::vector<float> source(master.size(), 0.0f);
        for (std::size_t i = 3; i + 2 < source.size(); ++i) {
            const double u =
                static_cast<double>(i) /
                std::max(1.0, durationSamples);

            // Strongly nonlinear but monotonic project-time drift.
            const double shaped =
                0.05 * u + 0.95 * std::pow(u, 6.0);
            const double delay =
                35.0 + 260.0 * shaped;

            const double sourcePos =
                static_cast<double>(i) - delay;

            source[i] = static_cast<float>(
                lagrange4(master, sourcePos));
        }

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.68;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Dynamic ||
            r.curve.size() < 2 ||
            r.scoutDirectionConsistency < 0.67) {
            std::cerr
                << "AUTO nonlinear drift failed: mode="
                << (r.modeUsed == sap::Mode::Dynamic ? "DYNAMIC" : "STATIC")
                << " curve=" << r.curve.size()
                << " direction=" << r.scoutDirectionConsistency
                << " R2=" << r.scoutR2
                << "\n";
            return 12;
        }
    }

    std::cerr << "PHASE TEST: AUTO detects robust early-late shift\n";

    {
        // Deliberately non-linear and not strictly monotonic: the first half
        // and last half are still separated enough to represent a real
        // microphone-distance change.
        std::vector<float> source(master.size(), 0.0f);

        for (std::size_t i = 3; i + 2 < source.size(); ++i) {
            const double u =
                static_cast<double>(i) /
                static_cast<double>(master.size() - 1);

            double delay = 35.0;
            if (u < 0.50) {
                delay += 20.0 * std::sin(u * 30.0);
            } else {
                delay += 420.0 +
                         20.0 * std::sin(u * 30.0);
            }

            const double sourcePos =
                static_cast<double>(i) - delay;

            source[i] = static_cast<float>(
                lagrange4(master, sourcePos));
        }

        sap::Settings s;
        s.sampleRate = sr;
        s.mode = sap::Mode::Auto;
        s.maxDelayMs = 12.0;
        s.analysisWindowMs = 60.0;
        s.hopMs = 250.0;
        s.minConfidence = 0.68;

        const auto r =
            sap::AlignEngine::analyze(master, source, s);

        if (r.modeUsed != sap::Mode::Dynamic ||
            r.scoutRobustShiftSamples < 300.0) {
            std::cerr
                << "AUTO robust shift failed: mode="
                << (r.modeUsed == sap::Mode::Dynamic ? "DYNAMIC" : "STATIC")
                << " robust="
                << r.scoutRobustShiftSamples
                << "\n";
            return 13;
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
