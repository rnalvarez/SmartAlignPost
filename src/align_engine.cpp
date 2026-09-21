#include "align_engine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

namespace sap {
namespace {

using Complex = std::complex<double>;
constexpr double kPi = 3.1415926535897932384626433832795;

struct Measurement {
    double phatDelay = 0.0;
    double waveformDelay = 0.0;
    double finalDelay = 0.0;
    double confidence = 0.0;
    double correlation = 0.0;
    double phaseAgreement = 0.0;
    double directDelay = 0.0;
    double directCorrelation = 0.0;
};

struct Anchor {
    std::size_t center = 0;
    double energy = 0.0;
    double onsetScore = 0.0;
    std::size_t onsetSample = 0;
    bool onsetDriven = false;
};

std::size_t nextPow2(std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

void fft(std::vector<Complex>& a, bool inverse)
{
    const std::size_t n = a.size();

    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi /
                             static_cast<double>(len);
        const Complex wlen(std::cos(angle), std::sin(angle));

        for (std::size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            const std::size_t half = len >> 1;
            for (std::size_t j = 0; j < half; ++j) {
                const Complex u = a[i + j];
                const Complex v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double invN = 1.0 / static_cast<double>(n);
        for (auto& v : a)
            v *= invN;
    }
}

double hann(std::size_t i, std::size_t n)
{
    if (n <= 1)
        return 1.0;
    return 0.5 - 0.5 * std::cos(
        2.0 * kPi * static_cast<double>(i) /
        static_cast<double>(n - 1));
}

double sampleLinear(const std::vector<float>& x, double pos)
{
    if (x.empty() || pos < 0.0 || pos >= static_cast<double>(x.size()))
        return 0.0;

    const std::size_t i0 = static_cast<std::size_t>(std::floor(pos));
    const std::size_t i1 = std::min(i0 + 1, x.size() - 1);
    const double frac = pos - static_cast<double>(i0);
    return (1.0 - frac) * x[i0] + frac * x[i1];
}

double sampleLagrange4(const std::vector<float>& x, double pos)
{
    if (x.empty() || pos < 1.0 ||
        pos >= static_cast<double>(x.size() - 2))
        return sampleLinear(x, pos);

    const std::size_t i = static_cast<std::size_t>(std::floor(pos));
    const double f = pos - static_cast<double>(i);

    const double c0 = -f * (f - 1.0) * (f - 2.0) / 6.0;
    const double c1 =  (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
    const double c2 = -(f + 1.0) * f * (f - 2.0) / 2.0;
    const double c3 =  (f + 1.0) * f * (f - 1.0) / 6.0;

    return c0 * x[i - 1] +
           c1 * x[i] +
           c2 * x[i + 1] +
           c3 * x[i + 2];
}

double rmsAround(const std::vector<float>& x, std::size_t center, std::size_t radius)
{
    if (x.empty())
        return 0.0;

    const std::size_t first = center > radius ? center - radius : 0;
    const std::size_t last = std::min(x.size(), center + radius + 1);

    if (first >= last)
        return 0.0;

    long double sum = 0.0;
    for (std::size_t i = first; i < last; ++i)
        sum += static_cast<long double>(x[i]) * x[i];

    return std::sqrt(
        static_cast<double>(sum / static_cast<long double>(last - first)));
}

double normalizedWaveformCorrelation(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t center,
    std::size_t halfWindow,
    double delaySamples)
{
    if (master.empty() || source.empty())
        return 0.0;

    const std::size_t first = center > halfWindow ? center - halfWindow : 0;
    const std::size_t last = std::min(master.size(), center + halfWindow + 1);

    long double sumA = 0.0;
    long double sumB = 0.0;
    long double sumAA = 0.0;
    long double sumBB = 0.0;
    long double sumAB = 0.0;
    std::size_t count = 0;

    for (std::size_t i = first; i < last; ++i) {
        const double sourcePos = static_cast<double>(i) + delaySamples;
        if (sourcePos < 1.0 ||
            sourcePos >= static_cast<double>(source.size() - 2))
            continue;

        const double a = master[i];
        const double b = sampleLagrange4(source, sourcePos);

        sumA += a;
        sumB += b;
        sumAA += a * a;
        sumBB += b * b;
        sumAB += a * b;
        ++count;
    }

    if (count < 32)
        return 0.0;

    const long double inv = 1.0L / static_cast<long double>(count);
    const long double cov = sumAB - sumA * sumB * inv;
    const long double varA = sumAA - sumA * sumA * inv;
    const long double varB = sumBB - sumB * sumB * inv;

    if (varA <= 0.0 || varB <= 0.0)
        return 0.0;

    return static_cast<double>(
        cov / std::sqrt(varA * varB));
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;

    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    double result = values[mid];

    if (values.size() % 2 == 0) {
        const auto maxLeft = *std::max_element(values.begin(), values.begin() + mid);
        result = 0.5 * (result + maxLeft);
    }

    return result;
}

std::vector<double> buildEnergyCurve(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t window,
    std::size_t hop)
{
    const std::size_t half = window / 2;
    const std::size_t usable = std::min(master.size(), source.size());

    std::vector<double> energy;
    if (usable < window)
        return energy;

    for (std::size_t center = half; center + half < usable; center += std::max<std::size_t>(1, hop))
        energy.push_back(rmsAround(master, center, half));

    return energy;
}

bool isImpulsiveMaterial(
    const std::vector<float>& master,
    double sampleRate)
{
    if (master.size() < 256 || sampleRate <= 0.0)
        return false;

    std::size_t window = std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::llround(
                settings.analysisWindowMs *
                settings.sampleRate / 1000.0)),
        1024,
        8192);

    std::size_t hop =
        std::max<std::size_t>(
            1,
            static_cast<std::size_t>(
                std::llround(
                    settings.hopMs *
                    settings.sampleRate / 1000.0)));

    const int maxLag =
        std::max(
            1,
            static_cast<int>(
                std::llround(
                    settings.maxDelayMs *
                    settings.sampleRate / 1000.0)));

    constexpr std::size_t kMinimumAnchorCount = 4;

    auto staticAnchors =
        selectAnchors(
            master,
            source,
            window,
            hop,
            settings.energyGateRatio,
            settings.anchorSeparationMs / 1000.0,
            settings.sampleRate,
            std::max<std::size_t>(
                1,
                settings.staticAnchorCount));

    // Adaptive fallback: reduce analysis window and hop only when the normal
    // selection produced too little redundancy. A single isolated transient
    // may legitimately remain one anchor at the end, but we do not pretend it
    // is redundant evidence.
    if (staticAnchors.size() < kMinimumAnchorCount) {
        const std::size_t floorWindow =
            std::max<std::size_t>(
                1024,
                static_cast<std::size_t>(
                    std::llround(
                        0.020 *
                        settings.sampleRate)));

        const std::size_t floorHop =
            std::max<std::size_t>(
                1,
                static_cast<std::size_t>(
                    std::llround(
                        0.010 *
                        settings.sampleRate)));

        const double scales[] = {
            0.75,
            0.50,
            0.333333333333
        };

        for (double scale : scales) {
            const std::size_t candidateWindow =
                std::max(
                    floorWindow,
                    static_cast<std::size_t>(
                        std::llround(
                            static_cast<double>(window) *
                            scale)));

            const std::size_t candidateHop =
                std::max(
                    floorHop,
                    static_cast<std::size_t>(
                        std::llround(
                            static_cast<double>(hop) *
                            scale)));

            if (candidateWindow == window &&
                candidateHop == hop)
                continue;

            const auto retry =
                selectAnchors(
                    master,
                    source,
                    candidateWindow,
                    candidateHop,
                    settings.energyGateRatio,
                    settings.anchorSeparationMs / 1000.0,
                    settings.sampleRate,
                    std::max<std::size_t>(
                        1,
                        settings.staticAnchorCount));

            if (retry.size() > staticAnchors.size()) {
                staticAnchors = retry;
                window = candidateWindow;
                hop = candidateHop;
            }

            if (staticAnchors.size() >=
                kMinimumAnchorCount)
                break;
        }
    }

    result.staticTotalWindows =
        static_cast<int>(
            staticAnchors.size());

    result.staticAnalysisTimeSec =
        staticAnchors.empty()
            ? 0.0
            : static_cast<double>(
                staticAnchors[
                    staticAnchors.size() / 2].center) /
              settings.sampleRate;

    std::vector<double> staticDelays;
    std::vector<double> staticCorrelations;
    std::vector<double> staticConfidences;
    staticDelays.reserve(staticAnchors.size());

    const double seededDelay = settings.hasInitialDelaySamples
        ? settings.initialDelaySamples
        : 0.0;

    for (const auto& anchor : staticAnchors) {
        const Measurement m = measurePhase(
            master, source,
            anchor.center,
            window,
            seededDelay,
            -static_cast<double>(maxLag),
            static_cast<double>(maxLag),
            settings.sampleRate,
            anchor.onsetDriven,
            anchor.onsetSample);

        if (m.confidence < settings.minConfidence * 0.75)
            continue;

        staticDelays.push_back(m.finalDelay);
        staticCorrelations.push_back(m.correlation);
        staticConfidences.push_back(m.confidence);
    }

    result.staticSupportWindows = static_cast<int>(staticDelays.size());

    if (!staticDelays.empty()) {
        result.staticDelaySamples =
            median(staticDelays);

        result.staticCorrelation =
            median(staticCorrelations);

        const double rawConfidence =
            median(staticConfidences);

        std::vector<double> deviation;
        deviation.reserve(staticDelays.size());
        for (double d : staticDelays)
            deviation.push_back(
                std::abs(
                    d - result.staticDelaySamples));

        result.staticDelayMADSamples =
            median(deviation);

        const double redundancy =
            std::min(
                1.0,
                std::sqrt(
                    static_cast<double>(
                        result.staticSupportWindows) /
                    4.0));

        result.staticConfidence =
            rawConfidence * redundancy;

        if (result.staticSupportWindows == 1) {
            // One acoustic measurement is allowed to produce a useful delay,
            // but it must never present itself as high-confidence alignment.
            result.staticConfidence =
                std::min(
                    result.staticConfidence,
                    0.35);
        }
    }

    if (settings.mode == Mode::Static) {
        result.modeUsed = Mode::Static;
        return result;
    }

    const bool knownRateDrift =
        std::isfinite(settings.playbackRateRatio) &&
        std::abs(settings.playbackRateRatio - 1.0) > 1.0e-6;

    if (knownRateDrift && settings.mode != Mode::Static) {
        // First undo the relative SOURCE/MASTER playback-rate ratio in the
        // project-time SOURCE buffer. This restores the acoustic offset in the
        // same timebase as MASTER, so the existing STATIC estimator can measure
        // it without having to chase a moving delay peak.
        const double ratio =
            std::max(
                1.0e-12,
                settings.playbackRateRatio);

        const std::size_t rateAlignedUsable =
            std::min<std::size_t>(
                master.size(),
                source.empty()
                    ? 0
                    : std::min<std::size_t>(
                        source.size(),
                        static_cast<std::size_t>(
                            std::floor(
                                static_cast<double>(
                                    source.size() - 1) *
                                ratio)) +
                            1));

        if (rateAlignedUsable >= 2048) {
            std::vector<float> rateAlignedSource(
                rateAlignedUsable);

            for (std::size_t i = 0;
                 i < rateAlignedUsable;
                 ++i) {
                const double sourcePos =
                    static_cast<double>(i) / ratio;

                rateAlignedSource[i] =
                    static_cast<float>(
                        sampleLagrange4(
                            source,
                            sourcePos));
            }

            Settings staticRateSettings = settings;
            staticRateSettings.mode = Mode::Static;
            staticRateSettings.playbackRateRatio = 1.0;
            staticRateSettings.hasInitialDelaySamples = false;

            const Result rateStatic =
                AlignEngine::analyze(
                    std::vector<float>(
                        master.begin(),
                        master.begin() + rateAlignedUsable),
                    rateAlignedSource,
                    staticRateSettings);

            if (std::isfinite(rateStatic.staticDelaySamples) &&
                rateStatic.staticConfidence >=
                    std::max(
                        0.45,
                        settings.minConfidence * 0.60)) {

                const double slopePerSec =
                    (1.0 / ratio - 1.0) *
                    settings.sampleRate;

                const double anchorAtZero =
                    rateStatic.staticDelaySamples /
                    ratio;

                result.staticDelaySamples =
                    anchorAtZero;
                result.staticCorrelation =
                    rateStatic.staticCorrelation;
                result.staticConfidence =
                    rateStatic.staticConfidence;
                result.staticSupportWindows =
                    rateStatic.staticSupportWindows;
                result.staticTotalWindows =
                    rateStatic.staticTotalWindows;

                const auto timeline =
                    selectTimelineAnchors(
                        master,
                        source,
                        window,
                        std::max<std::size_t>(
                            1,
                            static_cast<std::size_t>(
                                std::llround(
                                    std::max(
                                        250.0,
                                        settings.hopMs) *
                                    settings.sampleRate /
                                    1000.0))),
                        std::max<std::size_t>(
                            2,
                            std::min<std::size_t>(
                                settings.maxDynamicAnchors,
                                32)),
                        settings.sampleRate));

                if (timeline.size() >= 2) {
                    result.curve.clear();

                    for (const auto& anchor : timeline) {
                        const double t =
                            static_cast<double>(
                                anchor.center) /
                            settings.sampleRate;

                        Point p;
                        p.timeSec = t;
                        p.delaySamples =
                            anchorAtZero +
                            t * slopePerSec;
                        p.confidence =
                            std::max(
                                result.staticConfidence,
                                0.50);
                        p.keyPoint = true;
                        p.phatDelaySamples =
                            p.delaySamples;
                        p.waveformDelaySamples =
                            p.delaySamples;
                        p.phaseAgreement = 1.0;
                        result.curve.push_back(p);
                    }

                    result.modeUsed = Mode::Dynamic;
                    return result;
                }
            }
        }
    }

    if (staticDelays.empty() && knownRateDrift &&
        settings.mode != Mode::Static) {
        // Recover a coarse absolute alignment even when the normal static
        // confidence gate rejects every high-energy anchor. The known
        // playback-rate drift is enough to establish the trajectory; we only
        // need one trustworthy absolute phase offset to anchor it.
        const auto coarseAnchors = selectTimelineAnchors(
            master,
            source,
            window,
            std::max<std::size_t>(
                1,
                static_cast<std::size_t>(
                    std::llround(
                        std::max(250.0, settings.hopMs) *
                        settings.sampleRate / 1000.0))),
            7,
            settings.sampleRate);

        Measurement bestMeasurement;
        bool haveBest = false;

        for (const auto& anchor : coarseAnchors) {
            const Measurement m = measurePhase(
                master,
                source,
                anchor.center,
                window,
                0.0,
                -static_cast<double>(maxLag),
                static_cast<double>(maxLag),
                settings.sampleRate,
                anchor.onsetDriven,
                anchor.onsetSample);

            if (!haveBest || m.confidence > bestMeasurement.confidence) {
                bestMeasurement = m;
                haveBest = true;
            }
        }

        if (haveBest && bestMeasurement.confidence >= 0.40) {
            result.staticDelaySamples = bestMeasurement.finalDelay;
            result.staticCorrelation = bestMeasurement.correlation;
            result.staticConfidence = bestMeasurement.confidence;
            staticDelays.push_back(bestMeasurement.finalDelay);
            staticCorrelations.push_back(bestMeasurement.correlation);
            staticConfidences.push_back(bestMeasurement.confidence);
            result.staticSupportWindows = 1;
        }
    }

    if (staticDelays.empty()) {
        result.modeUsed = Mode::Static;
        return result;
    }

    // AUTO must not decide "STATIC" only from the handful of highest-energy
    // anchors. A gentle time-stretch can produce a perfectly coherent drift
    // while those high-energy anchors happen to cluster in one part of the
    // take. First run a very cheap temporal scout across the whole overlap.
    const double staticCenter = result.staticDelaySamples;
    double staticSpread = 0.0;
    for (double d : staticDelays)
        staticSpread = std::max(staticSpread, std::abs(d - staticCenter));

    const double dynamicThreshold =
        std::max(0.75, 0.45 * settings.sampleRate / 1000.0);

    bool coherentTemporalDrift = false;
    if (settings.mode != Mode::Static) {
        const auto scoutAnchors = selectTimelineAnchors(
            master,
            source,
            window,
            std::max<std::size_t>(
                1,
                static_cast<std::size_t>(
                    std::llround(
                        std::max(250.0, settings.hopMs) *
                        settings.sampleRate / 1000.0))),
            7,
            settings.sampleRate);

        std::vector<std::pair<double, double>> scout;
        scout.reserve(scoutAnchors.size());

        double predictedDelay = staticCenter;
        bool havePrediction = false;
        std::size_t previousCenter = 0;

        for (const auto& anchor : scoutAnchors) {
            const std::size_t center = anchor.center;

            double searchMin =
                -static_cast<double>(maxLag);
            double searchMax =
                static_cast<double>(maxLag);

            if (havePrediction) {
                const double hopSec =
                    static_cast<double>(
                        center - previousCenter) /
                    settings.sampleRate;

                const double maxStep =
                    std::max(
                        2.0,
                        settings.maxSlewMsPerSecond /
                        1000.0 *
                        hopSec *
                        settings.sampleRate);

                const double localSearch =
                    std::min(
                        static_cast<double>(maxLag),
                        std::max(
                            4.0 *
                                settings.sampleRate /
                                1000.0,
                            2.0 * maxStep));

                searchMin =
                    std::max(
                        -static_cast<double>(maxLag),
                        predictedDelay - localSearch);
                searchMax =
                    std::min(
                        static_cast<double>(maxLag),
                        predictedDelay + localSearch);
            }

            const Measurement m =
                measurePhase(
                    master,
                    source,
                    center,
                    window,
                    havePrediction
                        ? predictedDelay
                        : staticCenter,
                    searchMin,
                    searchMax,
                    settings.sampleRate,
                    anchor.onsetDriven,
                    anchor.onsetSample);

            double acceptedDelay = m.finalDelay;
            double effectiveConfidence = m.confidence;

            // Consolidated REAPER time-stretch can weaken the broadband
            // GCC-PHAT confidence even when the local waveform relationship
            // remains very strong. Recover that evidence with a short
            // predictive waveform search around the current estimate.
            if (effectiveConfidence <
                    settings.minConfidence) {
                const DynamicLocalResult local =
                    refineDynamicLocalDelay(
                        master,
                        source,
                        center,
                        acceptedDelay,
                        settings.sampleRate,
                        settings.dynamicScoutWindowMs,
                        settings.dynamicScoutSearchMs);

                if (local.score >=
                    std::max(
                        0.60,
                        settings.minConfidence * 0.85)) {
                    acceptedDelay =
                        local.delaySamples;
                    effectiveConfidence =
                        std::max(
                            effectiveConfidence,
                            local.score);
                }
            }

            if (effectiveConfidence >=
                settings.minConfidence * 0.75) {
                if (havePrediction) {
                    const double hopSec =
                        static_cast<double>(
                            center - previousCenter) /
                        settings.sampleRate;

                    const double maxStep =
                        std::max(
                            2.0,
                            settings.maxSlewMsPerSecond /
                            1000.0 *
                            hopSec *
                            settings.sampleRate);

                    // Do not reshape the observation into the predicted
                    // trajectory. If a point jumps beyond the physical slew
                    // budget, it is evidence against the point, not something
                    // to be silently clamped into a plausible-looking drift.
                    if (std::abs(
                            acceptedDelay -
                            predictedDelay) >
                        maxStep * 1.25) {
                        continue;
                    }
                }

                scout.emplace_back(
                    static_cast<double>(center) /
                        settings.sampleRate,
                    acceptedDelay);

                predictedDelay = acceptedDelay;
                havePrediction = true;
                previousCenter = center;
            }
        }

        result.scoutPoints = static_cast<int>(scout.size());

        if (scout.size() >= 1) {
            result.scoutFirstDelaySamples = scout.front().second;
            result.scoutLastDelaySamples = scout.back().second;
        }

        if (scout.size() >= 3) {
            const double firstDelay = scout.front().second;
            const double lastDelay = scout.back().second;
            const double endToEnd = std::abs(lastDelay - firstDelay);

            double meanT = 0.0;
            double meanD = 0.0;
            for (const auto& p : scout) {
                meanT += p.first;
                meanD += p.second;
            }
            meanT /= static_cast<double>(scout.size());
            meanD /= static_cast<double>(scout.size());

            double cov = 0.0;
            double varT = 0.0;
            double varD = 0.0;
            for (const auto& p : scout) {
                const double dt = p.first - meanT;
                const double dd = p.second - meanD;
                cov += dt * dd;
                varT += dt * dt;
                varD += dd * dd;
            }

            double rSquared = 0.0;
            if (varT > 1.0e-12 && varD > 1.0e-12) {
                const double corr =
                    cov / std::sqrt(varT * varD);
                rSquared = std::clamp(corr * corr, 0.0, 1.0);
            }

            result.scoutR2 = rSquared;

            // A real microphone-distance drift does not have to be linear.
            // R² alone can reject a genuine monotonic walk when movement
            // accelerates, decelerates, or contains small local reversals.
            const double directionDeadband =
                std::max(1.0, dynamicThreshold * 0.25);

            int positiveSteps = 0;
            int negativeSteps = 0;
            int meaningfulSteps = 0;

            for (std::size_t i = 1; i < scout.size(); ++i) {
                const double delta =
                    scout[i].second - scout[i - 1].second;

                if (std::abs(delta) < directionDeadband)
                    continue;

                ++meaningfulSteps;
                if (delta > 0.0)
                    ++positiveSteps;
                else
                    ++negativeSteps;
            }

            double directionConsistency = 0.0;
            if (meaningfulSteps > 0) {
                directionConsistency =
                    static_cast<double>(
                        std::max(positiveSteps, negativeSteps)) /
                    static_cast<double>(meaningfulSteps);
            }

            result.scoutDirectionConsistency =
                directionConsistency;

            std::vector<double> stepValues;
            stepValues.reserve(
                scout.size() - 1);

            for (std::size_t i = 1;
                 i < scout.size();
                 ++i) {
                stepValues.push_back(
                    scout[i].second -
                    scout[i - 1].second);
            }

            const double medianStep =
                median(stepValues);

            std::vector<double> stepDeviation;
            stepDeviation.reserve(stepValues.size());

            for (double step : stepValues)
                stepDeviation.push_back(
                    std::abs(
                        step -
                        medianStep));

            result.scoutStepMADSamples =
                median(stepDeviation);

            const bool linearEvidence =
                rSquared >= 0.45;
            const bool monotonicEvidence =
                meaningfulSteps >= 3 &&
                directionConsistency >= 0.67;

            // Robust early-vs-late evidence handles real movement that is
            // neither linear nor strictly monotonic. Compare the median delay
            // of the first half with the median delay of the last half.
            const std::size_t split =
                std::max<std::size_t>(1, scout.size() / 2);

            std::vector<double> earlyDelays;
            std::vector<double> lateDelays;
            earlyDelays.reserve(split);
            lateDelays.reserve(scout.size() - split);

            for (std::size_t i = 0; i < scout.size(); ++i) {
                if (i < split)
                    earlyDelays.push_back(scout[i].second);
                else
                    lateDelays.push_back(scout[i].second);
            }

            const double earlyMedian = median(earlyDelays);
            const double lateMedian = median(lateDelays);

            const double robustShift =
                std::abs(lateMedian - earlyMedian);

            result.scoutRobustShiftSamples = robustShift;

            const bool robustTemporalEvidence =
                scout.size() >= 4 &&
                robustShift > std::max(
                    2.0 * dynamicThreshold,
                    1.0 * settings.sampleRate / 1000.0);

            const bool enoughScoutAnchors =
                scout.size() >=
                kMinimumAnchorCount;

            const double stepDispersionLimit =
                std::max(
                    48.0,
                    0.20 * std::max(
                        endToEnd,
                        dynamicThreshold));

            const bool lowMeasurementDispersion =
                scout.size() < 3 ||
                result.scoutStepMADSamples <=
                    stepDispersionLimit;

            coherentTemporalDrift =
                enoughScoutAnchors &&
                lowMeasurementDispersion &&
                endToEnd > dynamicThreshold &&
                (linearEvidence ||
                 monotonicEvidence ||
                 robustTemporalEvidence);

            result.scoutCoherent =
                coherentTemporalDrift;
        }
    }

    const bool explicitDynamic =
        settings.mode == Mode::Dynamic;

    result.evidenceInsufficient =
        settings.mode == Mode::Auto &&
        (result.staticSupportWindows <
            static_cast<int>(
                kMinimumAnchorCount) ||
         result.scoutPoints <
            static_cast<int>(
                kMinimumAnchorCount));

    const bool needsDynamic =
        explicitDynamic ||
        knownRateDrift ||
        coherentTemporalDrift ||
        (settings.mode != Mode::Static &&
         !coherentTemporalDrift &&
         staticSpread > dynamicThreshold &&
         !result.evidenceInsufficient);

    if (!needsDynamic) {
        result.modeUsed = Mode::Static;
        return result;
    }

    const std::size_t dynamicHop = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::llround(
                std::max(120.0, settings.hopMs) *
                settings.sampleRate / 1000.0)));

    // Once AUTO has established a coherent temporal drift, the dynamic
    // trajectory must also be sampled over the whole take. The old
    // energy-only selector could concentrate all dynamic points inside one
    // short high-energy region and miss a real phase walk outside it.
    auto anchors = selectTimelineAnchors(
        master,
        source,
        window,
        dynamicHop,
        std::max<std::size_t>(1, settings.maxDynamicAnchors),
        settings.sampleRate);

    if (anchors.empty()) {
        result.modeUsed = Mode::Static;
        return result;
    }

    double predictedDelay =
        settings.hasInitialDelaySamples
            ? settings.initialDelaySamples
            : result.staticDelaySamples;

    // DYNAMIC tracking can be materially less confident than the robust
    // static median, especially on short overlaps. Once a temporal drift is
    // explicitly justified (e.g. D_PLAYRATE != 1.0), do not force every
    // individual anchor to meet the full static confidence gate. Use the
    // quality of the static solution as the floor, with a conservative
    // absolute minimum.
    const bool scoutJustifiedDynamic =
        coherentTemporalDrift ||
        explicitDynamic ||
        knownRateDrift;

    const double dynamicPointMinConfidence =
        scoutJustifiedDynamic
            ? std::max(
                0.50,
                std::min(
                    settings.minConfidence,
                    result.staticConfidence * 0.95))
            : settings.minConfidence;

    std::size_t previousCenter = anchors.front().center;

    for (const auto& anchor : anchors) {
        const double dt = static_cast<double>(
            anchor.center - previousCenter) / settings.sampleRate;

        const double slewAllowance =
            std::max(
                2.0 * settings.sampleRate / 1000.0,
                settings.maxSlewMsPerSecond *
                dt * settings.sampleRate / 1000.0);

        const double dynamicMin =
            std::max(
                -static_cast<double>(maxLag),
                predictedDelay - slewAllowance);

        const double dynamicMax =
            std::min(
                static_cast<double>(maxLag),
                predictedDelay + slewAllowance);

        const bool first = result.curve.empty();

        const Measurement m = measurePhase(
            master, source,
            anchor.center,
            window,
            predictedDelay,
            first ? -static_cast<double>(maxLag) : dynamicMin,
            first ? static_cast<double>(maxLag) : dynamicMax,
            settings.sampleRate,
            anchor.onsetDriven,
            anchor.onsetSample);

        if (m.confidence >= dynamicPointMinConfidence) {
            double acceptedDelay = m.finalDelay;

            if (!first) {
                const double delta =
                    std::abs(
                        acceptedDelay -
                        predictedDelay);

                if (delta > slewAllowance * 1.25) {
                    // Reject the observation rather than forcing it onto the
                    // prior trajectory.
                    previousCenter = anchor.center;
                    continue;
                }
            }

            Point p;
            p.timeSec = static_cast<double>(anchor.center) / settings.sampleRate;
            p.delaySamples = acceptedDelay;
            p.confidence = m.confidence;
            p.keyPoint = true;
            p.phatDelaySamples = m.phatDelay;
            p.waveformDelaySamples = m.waveformDelay;
            p.phaseAgreement = m.phaseAgreement;

            result.curve.push_back(p);
            predictedDelay = acceptedDelay;
        }

        previousCenter = anchor.center;
    }

    if (result.curve.size() < 2) {
        // When the project already tells us that SOURCE is being played at a
        // different rate, the temporal drift itself is known. Do not collapse
        // that case back to STATIC just because the local acoustic confidence
        // is weak on a short/quiet take. Anchor the deterministic slope to the
        // robust static delay and let the waveform tracker validate it whenever
        // it can.
        if (knownRateDrift &&
            settings.mode != Mode::Static &&
            !staticDelays.empty()) {
            result.curve.clear();

            const auto timeline =
                selectTimelineAnchors(
                    master,
                    source,
                    window,
                    std::max<std::size_t>(
                        1,
                        static_cast<std::size_t>(
                            std::llround(
                                std::max(250.0, settings.hopMs) *
                                settings.sampleRate / 1000.0))),
                    std::max<std::size_t>(
                        2,
                        std::min<std::size_t>(
                            settings.maxDynamicAnchors,
                            32)),
                    settings.sampleRate));

            if (timeline.size() >= 2) {
                const double durationSec =
                    static_cast<double>(usable - 1) /
                    settings.sampleRate;
                const double centerTime =
                    durationSec * 0.5;
                // SOURCE playback-rate is expressed relative to MASTER.
                // To compensate SOURCE in project time we need the inverse
                // rate relationship: corrected(t) = SOURCE(t + D(t)) with
                // D(t) = t * (1 / ratio - 1).
                const double slopePerSec =
                    (1.0 / std::max(
                        1.0e-12,
                        settings.playbackRateRatio) - 1.0) *
                    settings.sampleRate;

                for (const auto& anchor : timeline) {
                    const double t =
                        static_cast<double>(anchor.center) /
                        settings.sampleRate;

                    Point p;
                    p.timeSec = t;
                    p.delaySamples =
                        result.staticDelaySamples +
                        (t - centerTime) * slopePerSec;
                    p.confidence =
                        result.staticConfidence;
                    p.keyPoint = true;
                    p.phatDelaySamples =
                        p.delaySamples;
                    p.waveformDelaySamples =
                        p.delaySamples;
                    p.phaseAgreement = 1.0;

                    result.curve.push_back(p);
                }
            }
        }

        if (result.curve.size() < 2) {
            result.curve.clear();
            result.modeUsed = Mode::Static;
            return result;
        }
    }

    // Preserve the static solution when the dynamic points are nearly
    // constant. This avoids inserting unnecessary REAPER stretch markers.
    double dynamicSpread = 0.0;
    for (const auto& p : result.curve)
        dynamicSpread = std::max(
            dynamicSpread,
            std::abs(p.delaySamples - result.staticDelaySamples));

    if (!explicitDynamic &&
        dynamicSpread <= std::max(0.75, 0.45 * settings.sampleRate / 1000.0)) {
        result.curve.clear();
        result.modeUsed = Mode::Static;
        return result;
    }

    result.modeUsed = Mode::Dynamic;
    return result;
}

double AlignEngine::estimateDelay(
    const float* master,
    const float* source,
    std::size_t n,
    int maxLag,
    double sampleRate,
    double& confidence)
{
    if (!master || !source || n < 1024 || maxLag <= 0) {
        confidence = 0.0;
        return 0.0;
    }

    std::vector<float> m(master, master + n);
    std::vector<float> s(source, source + n);
    return fallbackEstimate(m, s, maxLag, sampleRate, confidence);
}

} // namespace sap
