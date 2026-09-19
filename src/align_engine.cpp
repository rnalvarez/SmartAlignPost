#include "align_engine.h"

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cmath>
#include <vector>
#ifdef SAP_GCC_DIAGNOSTIC
#include <iostream>
#endif

namespace sap {

namespace {

using Complex = std::complex<double>;
constexpr double kPi = 3.1415926535897932384626433832795;

void fft(std::vector<Complex>& x, bool inverse)
{
    const size_t n = x.size();
    if (n < 2) return;

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const Complex wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            const size_t half = len >> 1;
            for (size_t j = 0; j < half; ++j) {
                const Complex u = x[i + j];
                const Complex v = x[i + j + half] * w;
                x[i + j] = u + v;
                x[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double invN = 1.0 / static_cast<double>(n);
        for (auto& v : x) v *= invN;
    }
}

size_t nextPowerOfTwo(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}


struct DynamicEvent
{
    double timeSec = 0.0;
    double strength = 0.0;
};

std::vector<DynamicEvent> detectDynamicEvents(
    const std::vector<float>& signal,
    double sampleRate,
    const Settings& settings)
{
    std::vector<DynamicEvent> events;
    if (signal.empty() || sampleRate <= 0.0) return events;

    const size_t frame = std::max<size_t>(
        32,
        static_cast<size_t>(
            sampleRate * settings.dynamicEventFrameMs / 1000.0));
    const size_t hop = std::max<size_t>(16, frame);

    if (signal.size() < frame * 3) return events;

    const size_t frameCount = 1 + (signal.size() - frame) / hop;
    std::vector<double> energy(frameCount, 0.0);

    double averageEnergy = 0.0;
    for (size_t i = 0; i < frameCount; ++i) {
        const size_t start = i * hop;
        double sum = 0.0;
        for (size_t j = 0; j < frame; ++j) {
            const double s = signal[start + j];
            sum += s * s;
        }
        energy[i] = std::sqrt(sum / static_cast<double>(frame));
        averageEnergy += energy[i];
    }
    averageEnergy /= static_cast<double>(frameCount);

    // Light envelope smoothing. We deliberately do not look for individual
    // sample peaks: those depend too strongly on waveform phase and mic
    // polarity. We look for salient acoustic envelope transitions.
    std::vector<double> env(frameCount, 0.0);
    for (size_t i = 0; i < frameCount; ++i) {
        double sum = energy[i];
        int count = 1;
        if (i > 0) { sum += energy[i - 1]; ++count; }
        if (i + 1 < frameCount) { sum += energy[i + 1]; ++count; }
        env[i] = sum / static_cast<double>(count);
    }

    const double minActivity =
        std::max(1e-8, averageEnergy * 0.02);
    const size_t minSeparation = std::max<size_t>(
        1,
        static_cast<size_t>(
            settings.dynamicEventMinSeparationMs /
            settings.dynamicEventFrameMs));

    struct Candidate {
        size_t index = 0;
        double strength = 0.0;
    };
    std::vector<Candidate> candidates;

    for (size_t i = 1; i + 1 < frameCount; ++i) {
        const double left = env[i - 1];
        const double center = env[i];
        const double right = env[i + 1];

        if (left + center + right < minActivity) continue;

        // Normalized local slope change catches both onsets and offsets.
        const double transition =
            std::abs(right - left) /
            std::max(left + right, 1e-8);

        const bool localPeak = center >= left && center >= right;
        const bool localValley = center <= left && center <= right;
        const double neighbour = 0.5 * (left + right);
        const double prominence =
            std::abs(center - neighbour) /
            std::max(neighbour, 1e-8);

        double strength = transition;
        if (localPeak || localValley)
            strength = std::max(strength, 0.5 * prominence);

        if (strength < settings.dynamicEventThreshold)
            continue;

        candidates.push_back({i, strength});
    }

    // Non-maximum suppression in time: keep the strongest event within the
    // configured separation. This avoids generating a key marker for every
    // tiny fluctuation in speech.
    for (const auto& candidate : candidates) {
        if (events.empty()) {
            events.push_back({
                static_cast<double>(candidate.index * hop) / sampleRate,
                candidate.strength
            });
            continue;
        }

        const size_t previousIndex = static_cast<size_t>(
            std::llround(events.back().timeSec * sampleRate / hop));

        if (candidate.index >= previousIndex &&
            candidate.index - previousIndex < minSeparation) {
            if (candidate.strength > events.back().strength) {
                events.back().timeSec =
                    static_cast<double>(candidate.index * hop) / sampleRate;
                events.back().strength = candidate.strength;
            }
        } else {
            events.push_back({
                static_cast<double>(candidate.index * hop) / sampleRate,
                candidate.strength
            });
        }
    }

    return events;
}

struct DynamicObservation
{
    double timeSec = 0.0;
    double delaySamples = 0.0;
    double confidence = 0.0;
    bool keyPoint = false;
};


double normalizedWindowCorrelation(
    const float* a,
    const float* b,
    size_t n,
    int lag,
    size_t center,
    size_t halfWindow,
    bool derivative)
{
    if (n < 32) return 0.0;

    long long aStart = static_cast<long long>(center) -
                       static_cast<long long>(halfWindow);
    long long bStart = aStart + static_cast<long long>(lag);
    const long long length = static_cast<long long>(halfWindow * 2);

    if (aStart < 0 || bStart < 0 ||
        aStart + length > static_cast<long long>(n) ||
        bStart + length > static_cast<long long>(n)) {
        return 0.0;
    }

    const size_t count = derivative
        ? static_cast<size_t>(length - 1)
        : static_cast<size_t>(length);

    if (count < 16) return 0.0;

    double ma = 0.0;
    double mb = 0.0;

    if (!derivative) {
        for (size_t i = 0; i < count; ++i) {
            ma += a[static_cast<size_t>(aStart) + i];
            mb += b[static_cast<size_t>(bStart) + i];
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            const double da =
                static_cast<double>(a[static_cast<size_t>(aStart) + i + 1]) -
                static_cast<double>(a[static_cast<size_t>(aStart) + i]);
            const double db =
                static_cast<double>(b[static_cast<size_t>(bStart) + i + 1]) -
                static_cast<double>(b[static_cast<size_t>(bStart) + i]);
            ma += da;
            mb += db;
        }
    }

    ma /= static_cast<double>(count);
    mb /= static_cast<double>(count);

    double num = 0.0;
    double daEnergy = 0.0;
    double dbEnergy = 0.0;

    if (!derivative) {
        for (size_t i = 0; i < count; ++i) {
            const double xa =
                static_cast<double>(a[static_cast<size_t>(aStart) + i]) - ma;
            const double xb =
                static_cast<double>(b[static_cast<size_t>(bStart) + i]) - mb;
            num += xa * xb;
            daEnergy += xa * xa;
            dbEnergy += xb * xb;
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            const double xa =
                (static_cast<double>(a[static_cast<size_t>(aStart) + i + 1]) -
                 static_cast<double>(a[static_cast<size_t>(aStart) + i])) - ma;
            const double xb =
                (static_cast<double>(b[static_cast<size_t>(bStart) + i + 1]) -
                 static_cast<double>(b[static_cast<size_t>(bStart) + i])) - mb;
            num += xa * xb;
            daEnergy += xa * xa;
            dbEnergy += xb * xb;
        }
    }

    if (daEnergy <= 1e-15 || dbEnergy <= 1e-15) return 0.0;
    return num / std::sqrt(daEnergy * dbEnergy);
}

double refineLocalWaveformDelay(
    const float* master,
    const float* source,
    size_t n,
    double predictedDelaySamples,
    double sampleRate,
    double microWindowMs,
    double searchMs)
{
    if (n < 128 || sampleRate <= 0.0) return predictedDelaySamples;

    const size_t windowSamples = std::max<size_t>(
        96,
        static_cast<size_t>(
            sampleRate * microWindowMs / 1000.0));
    const size_t halfWindow = windowSamples / 2;
    if (halfWindow < 48 || halfWindow * 2 + 2 >= n)
        return predictedDelaySamples;

    const size_t center = n / 2;
    const int radius = std::max(
        1,
        static_cast<int>(
            sampleRate * searchMs / 1000.0));
    const int centerLag = static_cast<int>(
        std::llround(predictedDelaySamples));

    std::vector<double> scores(
        static_cast<size_t>(2 * radius + 1),
        -1.0);

    for (int i = -radius; i <= radius; ++i) {
        const int lag = centerLag + i;
        const double raw = std::abs(
            normalizedWindowCorrelation(
                master, source, n, lag, center, halfWindow, false));
        const double slope = std::abs(
            normalizedWindowCorrelation(
                master, source, n, lag, center, halfWindow, true));

        // Direct waveform shape is authoritative, while the first-difference
        // correlation emphasizes local peaks, valleys and transients without
        // locking us to their exact sample polarity.
        scores[static_cast<size_t>(i + radius)] =
            0.60 * raw + 0.40 * slope;
    }

    int best = radius;
    for (int i = 1; i < static_cast<int>(scores.size()); ++i) {
        if (scores[static_cast<size_t>(i)] >
            scores[static_cast<size_t>(best)]) {
            best = i;
        }
    }

    const int bestLag = centerLag + (best - radius);
    double refined = static_cast<double>(bestLag);

    if (best > 0 && best + 1 < static_cast<int>(scores.size())) {
        const double ym = scores[static_cast<size_t>(best - 1)];
        const double y0 = scores[static_cast<size_t>(best)];
        const double yp = scores[static_cast<size_t>(best + 1)];
        const double denom = ym - 2.0 * y0 + yp;
        if (std::abs(denom) > 1e-12) {
            const double offset = 0.5 * (ym - yp) / denom;
            refined += std::clamp(offset, -0.5, 0.5);
        }
    }

    // Keep this a local refinement around the spectral estimate. The micro
    // stage must not jump to a completely different correlation basin.
    const double maxCorrection =
        sampleRate * searchMs / 1000.0;
    if (std::abs(refined - predictedDelaySamples) > maxCorrection)
        return predictedDelaySamples;

    return refined;
}

double gccPhatDelay(const float* master,
                     const float* source,
                     size_t n,
                     int maxLag,
                     double sampleRate,
                     double& confidence,
                     double& peakCorrelation)
{
    confidence = 0.0;
    peakCorrelation = 0.0;
    if (n < 32 || sampleRate <= 0.0) return 0.0;

#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 0: enter n=" << n << " maxLag=" << maxLag << std::endl;
#endif

    // The zero-padding (fftSize - n) must cover the full lag search range,
    // or the circular correlation wraps around and contaminates results at
    // lags near +-maxLag. This matters most for short buffers (analyze()'s
    // n < win branch), where maxLag can be a large fraction of n.
    const size_t lagMargin = static_cast<size_t>(std::max(0, maxLag));
    const size_t fftSize = nextPowerOfTwo(n + lagMargin);
    std::vector<Complex> A(fftSize, Complex(0.0, 0.0));
    std::vector<Complex> B(fftSize, Complex(0.0, 0.0));
#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 1: allocated fftSize=" << fftSize << std::endl;
#endif

    double meanA = 0.0;
    double meanB = 0.0;
    for (size_t i = 0; i < n; ++i) {
        meanA += master[i];
        meanB += source[i];
    }
    meanA /= static_cast<double>(n);
    meanB /= static_cast<double>(n);

    for (size_t i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(n - 1);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * u);
        A[i] = Complex((static_cast<double>(master[i]) - meanA) * window, 0.0);
        B[i] = Complex((static_cast<double>(source[i]) - meanB) * window, 0.0);
    }

    fft(A, false);
#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 2: after fft A" << std::endl;
#endif
    fft(B, false);
#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 3: after fft B" << std::endl;
#endif

    // Keep the original cross-spectrum phase for robust fractional-delay
    // refinement after the coarse GCC-PHAT peak is found.
    std::vector<Complex> crossSpectrum(fftSize, Complex(0.0, 0.0));
    double maxCrossMagnitude = 0.0;
    for (size_t k = 0; k < fftSize; ++k) {
        const Complex cross = A[k] * std::conj(B[k]);
        crossSpectrum[k] = cross;
        maxCrossMagnitude = std::max(maxCrossMagnitude, std::abs(cross));
    }
#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 4: after cross max=" << maxCrossMagnitude << std::endl;
#endif

    for (size_t k = 0; k < fftSize; ++k) {
        const Complex cross = crossSpectrum[k];
        const double mag = std::abs(cross);
        A[k] = mag > 1e-12 ? cross / mag : Complex(0.0, 0.0);
    }

    fft(A, true);
#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 5: after inverse fft" << std::endl;
#endif

    double best = -1.0;
    double second = -1.0;
    int bestLag = 0;
    const int span = std::min(maxLag, static_cast<int>(fftSize / 2) - 1);

#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 6: before lag scan span=" << span << std::endl;
#endif
    for (int lag = -span; lag <= span; ++lag) {
        const size_t index = lag >= 0
            ? static_cast<size_t>(lag)
            : fftSize - static_cast<size_t>(-lag);
        const double value = std::abs(A[index].real());
        if (value > best) {
            second = best;
            best = value;
            bestLag = lag;
        } else if (value > second) {
            second = value;
        }
    }

#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC 7: bestLag=" << bestLag << " best=" << best << std::endl;
#endif
    double refinedLag = static_cast<double>(bestLag);
    double phaseDelay = -refinedLag;
    if (bestLag > -span && bestLag < span) {
        const auto valueAt = [&](int lag) -> double {
            const size_t index = lag >= 0
                ? static_cast<size_t>(lag)
                : fftSize - static_cast<size_t>(-lag);
            return std::abs(A[index].real());
        };
        const double ym = valueAt(bestLag - 1);
        const double y0 = valueAt(bestLag);
        const double yp = valueAt(bestLag + 1);
        const double denom = ym - 2.0 * y0 + yp;
        if (std::abs(denom) > 1e-12) {
            const double offset = 0.5 * (ym - yp) / denom;
            refinedLag += std::clamp(offset, -0.5, 0.5);
        }
    }

#ifndef SAP_DISABLE_PHASE_REFINE
    // Robust fractional-delay refinement:
    // 1) start from the GCC-PHAT coarse/parabolic estimate;
    // 2) remove that coarse delay from the cross-spectrum;
    // 3) fit only the small residual phase slope.
    //
    // This avoids globally unwrapping the raw cross-spectrum phase. Global
    // unwrap is fragile with sparse/low-energy bins and was the main reason
    // the previous refinement could under-correct or jump between phase
    // branches on real production material.
    double phaseWeightSum = 0.0;
    double phaseFreqSum = 0.0;
    double phaseSum = 0.0;
    double phaseFreq2Sum = 0.0;
    double phaseFreqPhaseSum = 0.0;
    int phaseBins = 0;

    const double nyquist = sampleRate * 0.5;
    const double fMin = std::max(250.0, sampleRate * 0.006);
    const double fMax = std::min(14000.0, nyquist * 0.88);
    const double magnitudeFloor = maxCrossMagnitude * 1e-3;

    auto fitResidualSlope = [&](bool rejectOutliers, double& slope, int& usedBins) -> bool {
        double wSum = 0.0;
        double fSum = 0.0;
        double pSum = 0.0;
        double f2Sum = 0.0;
        double fpSum = 0.0;
        usedBins = 0;

        for (size_t k = 1; k < fftSize / 2; ++k) {
            const double freq = static_cast<double>(k) * sampleRate /
                                static_cast<double>(fftSize);
            if (freq < fMin || freq > fMax) continue;

            const Complex cross = crossSpectrum[k];
            const double mag = std::abs(cross);
            if (mag <= magnitudeFloor) continue;

            const double coarseAngle =
                2.0 * kPi * freq * phaseDelay / sampleRate;
            const Complex derot(std::cos(coarseAngle), -std::sin(coarseAngle));
            const double residualPhase = std::atan2(
                (cross * derot).imag(), (cross * derot).real());

            if (rejectOutliers && std::abs(residualPhase) > 0.80)
                continue;

            const double w = std::sqrt(mag);
            wSum += w;
            fSum += w * freq;
            pSum += w * residualPhase;
            f2Sum += w * freq * freq;
            fpSum += w * freq * residualPhase;
            ++usedBins;
        }

        if (usedBins < 32 || wSum <= 0.0) return false;

        const double meanF = fSum / wSum;
        const double meanP = pSum / wSum;
        const double denom = f2Sum - wSum * meanF * meanF;
        if (std::abs(denom) <= 1e-12) return false;

        slope = (fpSum - wSum * meanF * meanP) / denom;
        return std::isfinite(slope);
    };

    double slope = 0.0;
    int phaseBinsFirst = 0;
    if (n >= 1024 && fitResidualSlope(false, slope, phaseBinsFirst)) {
        double candidateDelay =
            phaseDelay + slope * sampleRate / (2.0 * kPi);

        // Reject implausibly large fractional corrections. The coarse
        // GCC-PHAT/parabolic peak is authoritative for the integer basin;
        // phase refinement is only allowed to improve the local sub-sample
        // position.
        const double correction = candidateDelay - phaseDelay;
        if (std::abs(correction) <= 1.5 &&
            std::abs(candidateDelay) <= static_cast<double>(maxLag) &&
            std::isfinite(candidateDelay)) {
            phaseDelay = candidateDelay;

            // One robust second fit after the coarse correction. Outlying
            // bins from weak/reverberant spectral regions are removed.
            double slopeRefined = 0.0;
            int phaseBinsSecond = 0;
            if (fitResidualSlope(true, slopeRefined, phaseBinsSecond)) {
                const double secondCandidate =
                    phaseDelay + slopeRefined * sampleRate / (2.0 * kPi);
                const double secondCorrection =
                    secondCandidate - phaseDelay;

                if (std::abs(secondCorrection) <= 0.75 &&
                    std::abs(secondCandidate) <= static_cast<double>(maxLag) &&
                    std::isfinite(secondCandidate)) {
                    phaseDelay = secondCandidate;
                    phaseBins = phaseBinsSecond;
                } else {
                    phaseBins = phaseBinsFirst;
                }
            } else {
                phaseBins = phaseBinsFirst;
            }
        }
    }

#ifdef SAP_GCC_DIAGNOSTIC
    std::cerr << "GCC PHASE: bins=" << phaseBins
              << " delay=" << phaseDelay << std::endl;
#endif

#endif


    peakCorrelation = std::clamp(best, 0.0, 1.0);
    const double prominence = std::max(0.0, best - std::max(0.0, second));
    const double separation = best > 1e-12 ? prominence / best : 0.0;
    confidence = std::clamp(
        peakCorrelation * (0.65 + 0.35 * separation),
        0.0, 1.0);

    // Positive delay means SOURCE occurs later than MASTER. The
    // A*conj(B) cross-spectrum produces the opposite lag convention,
    // therefore invert the sign before returning the public delay.
    return phaseDelay;
}

} // namespace

double AlignEngine::normalizedCorrelation(const float* a, const float* b,
                                          size_t n, int lag)
{
    if (n == 0 || std::abs(lag) >= static_cast<int>(n)) return 0.0;
    size_t startA = 0, startB = 0, count = n;
    if (lag > 0) { startB = static_cast<size_t>(lag); count = n - startB; }
    else if (lag < 0) { startA = static_cast<size_t>(-lag); count = n - startA; }

    if (count < 16) return 0.0;

    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < count; ++i) {
        ma += a[startA+i];
        mb += b[startB+i];
    }
    ma /= count; mb /= count;

    double num=0.0, da=0.0, db=0.0;
    for (size_t i=0; i<count; ++i) {
        const double xa = a[startA+i]-ma;
        const double xb = b[startB+i]-mb;
        num += xa*xb;
        da += xa*xa;
        db += xb*xb;
    }
    if (da <= 1e-15 || db <= 1e-15) return 0.0;
    return num/std::sqrt(da*db);
}

double AlignEngine::estimateDelay(const float* master,
                                   const float* source,
                                   size_t n,
                                   int maxLag,
                                   double sampleRate,
                                   double& confidence)
{
    double peak = 0.0;
    return gccPhatDelay(master, source, n, maxLag, sampleRate, confidence, peak);
}

Result AlignEngine::analyze(const std::vector<float>& master,
                            const std::vector<float>& source,
                            const Settings& settings)
{
    Result r;
    if (master.empty() || source.empty()) return r;

    const size_t n = std::min(master.size(), source.size());
    const int requestedMaxLag = std::max(1, static_cast<int>(
        settings.sampleRate * settings.maxDelayMs / 1000.0));

    const size_t win = std::max<size_t>(256,
        static_cast<size_t>(settings.sampleRate * settings.analysisWindowMs / 1000.0));
    const size_t hop = std::max<size_t>(64,
        static_cast<size_t>(settings.sampleRate * settings.hopMs / 1000.0));

    if (n < win) {
        double c=0.0;
        double peak=0.0;
        const int maxLag = std::max(1, std::min<int>(requestedMaxLag, static_cast<int>(n / 2)));
        r.staticDelaySamples = gccPhatDelay(master.data(), source.data(),
                                             n, maxLag, settings.sampleRate, c, peak);
        r.staticConfidence=c;
        r.staticAnalysisTimeSec = 0.0;
        r.staticCorrelation = peak;
        r.staticSupportWindows = c >= settings.minConfidence ? 1 : 0;
        r.staticTotalWindows = 1;
        return r;
    }

    const int maxLag = std::max(1, std::min<int>(requestedMaxLag, static_cast<int>(win / 2) - 1));

    // DYNAMIC must not execute the STATIC consensus pass below first.
    // That would perform essentially the same GCC-PHAT windows twice and can
    // make even a short 24 s source exceed REAPER's 120 s process timeout on
    // slower Windows machines. DYNAMIC is a single tracking pass seeded from
    // the strongest of the first few windows.
    if (settings.mode == Mode::Dynamic) {
        double previous = 0.0;

        if (settings.hasInitialDelaySamples) {
            previous = settings.initialDelaySamples;
        } else {
            // First chunk: seed from the strongest of the first few windows.
            constexpr int kWarmupWindows = 5;
            double seedScore = -1.0;
            double seedDelay = 0.0;
            double seedCorrelation = 0.0;
            double seedConfidence = 0.0;
            size_t warmupPos = 0;
            int warmupCount = 0;

            for (; warmupCount < kWarmupWindows && warmupPos + win <= n;
                 ++warmupCount, warmupPos += hop) {
                double c = 0.0;
                double peak = 0.0;
                const double d = gccPhatDelay(master.data() + warmupPos,
                                              source.data() + warmupPos,
                                              win, maxLag, settings.sampleRate, c, peak);
                const double score = c * peak;
                if (score > seedScore) {
                    seedScore = score;
                    seedDelay = d;
                    seedCorrelation = peak;
                    seedConfidence = c;
                }
            }

            if (warmupCount == 0) return r;

            previous = seedDelay;
            r.staticDelaySamples = seedDelay;
            r.staticCorrelation = seedCorrelation;
            r.staticConfidence = seedConfidence;
            r.staticAnalysisTimeSec = 0.0;
            r.staticSupportWindows = warmupCount;
            r.staticTotalWindows = warmupCount;
        }

        std::vector<DynamicObservation> observations;
        observations.reserve((n / hop) + 64);

        const auto microRefine = [&](const float* m,
                                     const float* s,
                                     size_t length,
                                     double predicted) {
            return refineLocalWaveformDelay(
                m, s, length, predicted, settings.sampleRate,
                settings.dynamicMicroWindowMs,
                settings.dynamicMicroSearchMs);
        };

        // Dense baseline tracking. The baseline is intentionally more
        // frequent than the old 40 ms hop because the final warp should not
        // be forced to infer a change from widely spaced measurements.
        for (size_t pos = 0; pos + win <= n; pos += hop) {
            double c = 0.0;
            double peak = 0.0;
            double d = gccPhatDelay(master.data() + pos,
                                    source.data() + pos,
                                    win, maxLag, settings.sampleRate, c, peak);

            if (c >= settings.minConfidence) {
                d = microRefine(
                    master.data() + pos,
                    source.data() + pos,
                    win,
                    d);
            }

            if (c < settings.minConfidence) {
                d = previous;
            } else {
                const double maxStep =
                    settings.maxSlewMsPerSecond / 1000.0 *
                    (static_cast<double>(hop) / settings.sampleRate) *
                    settings.sampleRate;
                d = std::clamp(d, previous - maxStep, previous + maxStep);
            }

            const double tau = std::max(0.001, settings.smoothingMs / 1000.0);
            const double dt = static_cast<double>(hop) / settings.sampleRate;
            const double alpha = 1.0 - std::exp(-dt / tau);
            d = previous + alpha * (d - previous);

            // GCC/micro correlation is measured over the whole window.
            // Its temporal reference is therefore the window CENTER, not its
            // left edge. Labeling it at 'pos' shifts every warp control point
            // backwards by half a window and makes a changing acoustic delay
            // look less variable than it really is.
            const double observationTime =
                (static_cast<double>(pos) +
                 0.5 * static_cast<double>(win)) / settings.sampleRate;

            observations.push_back({
                observationTime,
                d,
                c,
                false
            });
            previous = d;
        }

        // Event refinement: for salient acoustic envelope transitions, measure
        // the delay again with a shorter local window. Three overlapping local
        // windows (-10/0/+10 ms) make the estimate less sensitive to choosing
        // one exact frame boundary. These points become hard temporal anchors
        // for the REAPER warp instead of being averaged away by consolidation.
        const auto events = detectDynamicEvents(master, settings.sampleRate, settings);
        const size_t fineWin = std::max<size_t>(
            256,
            static_cast<size_t>(
                settings.sampleRate * settings.dynamicFineWindowMs / 1000.0));
        const int fineMaxLag = std::max(
            1,
            std::min<int>(requestedMaxLag,
                          static_cast<int>(fineWin / 2) - 1));
        const int localOffsetSamples = static_cast<int>(
            settings.sampleRate * 0.010);

        for (const auto& event : events) {
            const size_t center = static_cast<size_t>(
                std::llround(event.timeSec * settings.sampleRate));
            if (center >= n) continue;

            double weightedDelay = 0.0;
            double weightSum = 0.0;
            double confidenceSum = 0.0;
            int successful = 0;

            for (const int offset : {
                -localOffsetSamples, 0, localOffsetSamples
            }) {
                long long startSigned =
                    static_cast<long long>(center) +
                    static_cast<long long>(offset) -
                    static_cast<long long>(fineWin / 2);

                if (startSigned < 0)
                    startSigned = 0;

                if (startSigned + static_cast<long long>(fineWin) >
                    static_cast<long long>(n)) {
                    startSigned =
                        static_cast<long long>(n) -
                        static_cast<long long>(fineWin);
                }

                if (startSigned < 0) continue;

                const size_t start =
                    static_cast<size_t>(startSigned);

                double c = 0.0;
                double peak = 0.0;
                double d = gccPhatDelay(
                    master.data() + start,
                    source.data() + start,
                    fineWin,
                    fineMaxLag,
                    settings.sampleRate,
                    c,
                    peak);

                if (c < settings.minConfidence)
                    continue;

                d = microRefine(
                    master.data() + start,
                    source.data() + start,
                    fineWin,
                    d);

                const double w = std::max(0.01, c * peak);
                weightedDelay += d * w;
                weightSum += w;
                confidenceSum += c;
                ++successful;
            }

            if (successful == 0 || weightSum <= 0.0)
                continue;

            const double refinedDelay = weightedDelay / weightSum;
            const double refinedConfidence =
                confidenceSum / static_cast<double>(successful);

            observations.push_back({
                event.timeSec,
                refinedDelay,
                refinedConfidence,
                true
            });
        }

        if (observations.empty()) return r;

        std::sort(
            observations.begin(),
            observations.end(),
            [](const DynamicObservation& a, const DynamicObservation& b) {
                if (a.timeSec != b.timeSec)
                    return a.timeSec < b.timeSec;
                return a.keyPoint > b.keyPoint;
            });

        // Build a LANDMARK-LOCKED delay map.  The old implementation
        // smoothed every point and then partially pulled event anchors toward
        // that smoothed state.  That is appropriate for noise suppression but
        // it also suppresses the very delay changes we need when the SOURCE
        // microphone moves relative to a fixed MASTER.
        //
        // Here:
        //   * normal windows get only light continuity limiting;
        //   * keyPoint landmarks use their locally measured delay directly;
        //   * no exponential smoothing is applied after the acoustic
        //     measurement has been refined.
        //
        // The resulting points are actual temporal constraints for the
        // REAPER stretch map, rather than samples of a heavily low-passed
        // delay estimate.
        r.curve.clear();
        r.curve.reserve(observations.size());

        double tracked = observations.front().delaySamples;
        double trackedTime = observations.front().timeSec;
        bool haveTracked = false;

        for (const auto& obs : observations) {
            if (obs.confidence < settings.minConfidence && haveTracked)
                continue;

            if (!haveTracked) {
                tracked = obs.delaySamples;
                trackedTime = obs.timeSec;
                haveTracked = true;
            } else {
                const double dt = std::max(
                    1e-4,
                    obs.timeSec - trackedTime);
                const double maxStep =
                    settings.maxSlewMsPerSecond / 1000.0 *
                    dt * settings.sampleRate;

                const double target = std::clamp(
                    obs.delaySamples,
                    tracked - maxStep,
                    tracked + maxStep);

                if (obs.keyPoint) {
                    // A landmark is a measured MASTER<->SOURCE correspondence,
                    // so do not blur it with neighbouring measurements.
                    tracked = target;
                } else {
                    // Keep continuity protection, but retain almost all of
                    // the local measurement. The CLI default is intentionally
                    // short (10 ms), so a moving microphone is not forced into
                    // a static-delay trajectory.
                    const double tau =
                        std::max(0.001, settings.smoothingMs / 1000.0);
                    const double alpha =
                        1.0 - std::exp(-dt / tau);
                    tracked = tracked + alpha * (target - tracked);
                }
                trackedTime = obs.timeSec;
            }

            r.curve.push_back({
                obs.timeSec,
                tracked,
                obs.confidence,
                obs.keyPoint
            });
        }

        if (!r.curve.empty()) {
            r.staticDelaySamples = r.curve.back().delaySamples;
            r.staticConfidence = r.curve.back().confidence;
        }

        r.staticTotalWindows =
            static_cast<int>(observations.size());
        r.staticSupportWindows = 0;
        for (const auto& p : r.curve) {
            if (p.confidence >= settings.minConfidence)
                ++r.staticSupportWindows;
        }

        return r;
    }

    const int lagSpan = 2 * maxLag + 1;
    std::vector<double> lagScore(static_cast<size_t>(lagSpan), 0.0);
    std::vector<double> lagConfidence(static_cast<size_t>(lagSpan), 0.0);
    std::vector<double> lagCorrelationSum(static_cast<size_t>(lagSpan), 0.0);
    std::vector<int> lagCount(static_cast<size_t>(lagSpan), 0);
    std::vector<double> lagBestTime(static_cast<size_t>(lagSpan), 0.0);
    std::vector<double> lagBestCorrelation(static_cast<size_t>(lagSpan), -1.0);

    double strongestConfidence = -1.0;
    double strongestDelay = 0.0;
    size_t strongestPos = 0;
    double strongestCorrelation = 0.0;
    int totalWindows = 0;

    for (size_t pos = 0; pos + win <= n; pos += hop) {
        double c = 0.0;
        double peak = 0.0;
        const double d = gccPhatDelay(master.data() + pos,
                                       source.data() + pos,
                                       win, maxLag, settings.sampleRate, c, peak);
        const int lag = static_cast<int>(std::llround(d));
        const int index = lag + maxLag;
        if (index >= 0 && index < lagSpan) {
            const double score = c * peak;
            lagScore[static_cast<size_t>(index)] += score;
            lagConfidence[static_cast<size_t>(index)] += c;
            lagCorrelationSum[static_cast<size_t>(index)] += peak;
            lagCount[static_cast<size_t>(index)] += 1;
            if (peak > lagBestCorrelation[static_cast<size_t>(index)]) {
                lagBestCorrelation[static_cast<size_t>(index)] = peak;
                lagBestTime[static_cast<size_t>(index)] =
                    static_cast<double>(pos) / settings.sampleRate;
            }
        }

        if (c > strongestConfidence) {
            strongestConfidence = c;
            strongestDelay = d;
            strongestPos = pos;
            strongestCorrelation = peak;
        }
        ++totalWindows;
    }

    int bestIndex = maxLag;
    double bestScore = -1.0;
    for (int i = 0; i < lagSpan; ++i) {
        if (lagScore[static_cast<size_t>(i)] > bestScore) {
            bestScore = lagScore[static_cast<size_t>(i)];
            bestIndex = i;
        }
    }

    double weightedDelay = 0.0;
    double totalWeight = 0.0;
    int supportWindows = 0;
    double supportConfidence = 0.0;
    double supportCorrelation = 0.0;
    int bestSupportIndex = bestIndex;
    for (int i = std::max(0, bestIndex - 1); i <= std::min(lagSpan - 1, bestIndex + 1); ++i) {
        const double w = lagScore[static_cast<size_t>(i)];
        weightedDelay += static_cast<double>(i - maxLag) * w;
        totalWeight += w;
        supportWindows += lagCount[static_cast<size_t>(i)];
        supportConfidence += lagConfidence[static_cast<size_t>(i)];
        supportCorrelation += lagCorrelationSum[static_cast<size_t>(i)];
        if (lagCount[static_cast<size_t>(i)] > lagCount[static_cast<size_t>(bestSupportIndex)])
            bestSupportIndex = i;
    }

    const double consensusDelay = totalWeight > 0.0
        ? weightedDelay / totalWeight
        : strongestDelay;

    r.staticDelaySamples = consensusDelay;
    r.staticAnalysisTimeSec =
        totalWeight > 0.0 ? lagBestTime[static_cast<size_t>(bestSupportIndex)]
                          : static_cast<double>(strongestPos) / settings.sampleRate;
    r.staticCorrelation =
        totalWeight > 0.0 ? lagBestCorrelation[static_cast<size_t>(bestSupportIndex)]
                          : strongestCorrelation;
    r.staticSupportWindows = supportWindows;
    r.staticTotalWindows = totalWindows;

    const double supportRatio = totalWindows > 0
        ? static_cast<double>(supportWindows) / static_cast<double>(totalWindows)
        : 0.0;
    const double averageSupportConfidence = supportWindows > 0
        ? supportConfidence / static_cast<double>(supportWindows)
        : 0.0;
    const double averageSupportCorrelation = supportWindows > 0
        ? supportCorrelation / static_cast<double>(supportWindows)
        : 0.0;

    const double supportStrength = std::clamp(supportRatio * 2.0, 0.0, 1.0);
    const double peakStrength = std::clamp(averageSupportCorrelation, 0.0, 1.0);
    const double confidenceStrength = std::clamp(averageSupportConfidence, 0.0, 1.0);
    r.staticConfidence = std::clamp(
        peakStrength * supportStrength * (0.70 + 0.30 * confidenceStrength),
        0.0, 1.0);

    if (supportWindows == 0 || bestScore <= 0.0) {
        r.staticDelaySamples = strongestDelay;
        r.staticAnalysisTimeSec = static_cast<double>(strongestPos) / settings.sampleRate;
        r.staticCorrelation = strongestCorrelation;
        r.staticConfidence = std::max(0.0, strongestConfidence);
        r.staticSupportWindows = 1;
    }

    if (settings.mode == Mode::Static) return r;

    return r;
}

}
