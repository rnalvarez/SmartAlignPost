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
};

struct Anchor {
    std::size_t center = 0;
    double energy = 0.0;
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

Measurement measurePhaseBand(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t center,
    std::size_t window,
    double predictedDelaySamples,
    double searchMinSamples,
    double searchMaxSamples,
    double sampleRate,
    double phaseMinHz,
    double phaseMaxHz);

struct TransientEstimate {
    bool valid = false;
    double delaySamples = 0.0;
    double confidence = 0.0;
    double masterOnsetSamples = 0.0;
    double sourceOnsetSamples = 0.0;
    double refinedCorrelation = 0.0;
};

double transientDerivativeRms(
    const std::vector<float>& x,
    std::size_t center,
    std::size_t radius)
{
    if (x.size() < 2)
        return 0.0;

    const std::size_t first = center > radius ? center - radius : 1;
    const std::size_t last = std::min(x.size(), center + radius + 1);

    if (first >= last)
        return 0.0;

    long double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double d =
            static_cast<double>(x[i]) -
            static_cast<double>(x[i - 1]);
        sum += d * d;
    }

    return std::sqrt(
        static_cast<double>(
            sum / static_cast<long double>(last - first)));
}

bool findDirectOnset(
    const std::vector<float>& x,
    double sampleRate,
    double& onsetSamples,
    double& confidence)
{
    confidence = 0.0;

    if (x.size() < 2048 || sampleRate <= 0.0)
        return false;

    const std::size_t frame =
        std::max<std::size_t>(
            32,
            static_cast<std::size_t>(
                std::llround(0.002 * sampleRate)));
    const std::size_t hop =
        std::max<std::size_t>(
            8,
            static_cast<std::size_t>(
                std::llround(0.0005 * sampleRate)));
    const std::size_t half = frame / 2;

    std::vector<double> scores;
    std::vector<double> levels;

    for (std::size_t center = half;
         center + half < x.size();
         center += hop) {
        scores.push_back(
            transientDerivativeRms(x, center, half));
        levels.push_back(
            rmsAround(x, center, half));
    }

    if (scores.size() < 8)
        return false;

    const double baseline = median(scores);

    std::vector<double> deviations;
    deviations.reserve(scores.size());
    for (double v : scores)
        deviations.push_back(std::abs(v - baseline));

    const double mad = median(deviations);
    const double peak = *std::max_element(scores.begin(), scores.end());
    const double peakLevel = *std::max_element(levels.begin(), levels.end());

    if (peak <= 0.0 || peakLevel <= 0.0)
        return false;

    // A transient must stand clearly above the derivative/noise floor.
    const double threshold = std::max(
        baseline + 5.0 * std::max(mad, 1.0e-12),
        0.22 * peak);

    if (peak < baseline * 1.8 || threshold >= peak)
        return false;

    const std::size_t minCenter =
        static_cast<std::size_t>(
            std::llround(0.040 * sampleRate));

    std::size_t found = 0;
    bool haveFound = false;

    for (std::size_t i = 0; i < scores.size(); ++i) {
        const std::size_t center =
            half + i * hop;

        if (center < minCenter)
            continue;

        if (scores[i] < threshold)
            continue;

        if (i + 1 < scores.size() &&
            scores[i + 1] < threshold * 0.55)
            continue;

        if (levels[i] < peakLevel * 0.05)
            continue;

        found = center;
        haveFound = true;
        break;
    }

    if (!haveFound)
        return false;

    const std::size_t foundIndex =
        (found - half) / hop;

    const double clarity =
        std::clamp(
            (scores[foundIndex] - baseline) /
            std::max(
                peak - baseline,
                1.0e-12),
            0.0,
            1.0);
    const double levelFraction =
        std::clamp(
            levels[foundIndex] /
            std::max(peakLevel, 1.0e-12),
            0.0,
            1.0);

    confidence =
        0.70 * clarity +
        0.30 * std::clamp(levelFraction / 0.20, 0.0, 1.0);

    onsetSamples = static_cast<double>(found);
    return confidence >= 0.30;
}

TransientEstimate estimateDirectTransient(
    const std::vector<float>& master,
    const std::vector<float>& source,
    const Settings& settings,
    int maxLag)
{
    TransientEstimate out;

    double masterOnset = 0.0;
    double sourceOnset = 0.0;
    double masterConfidence = 0.0;
    double sourceConfidence = 0.0;

    if (!findDirectOnset(
            master,
            settings.sampleRate,
            masterOnset,
            masterConfidence) ||
        !findDirectOnset(
            source,
            settings.sampleRate,
            sourceOnset,
            sourceConfidence)) {
        return out;
    }

    const double onsetDelay =
        sourceOnset - masterOnset;

    if (std::abs(onsetDelay) >
        static_cast<double>(maxLag)) {
        return out;
    }

    const std::size_t center =
        static_cast<std::size_t>(
            std::llround(masterOnset));

    if (center < 384 ||
        center + 384 >= master.size()) {
        return out;
    }

    // Refine the first-arrival estimate only around the detected attack.
    // Keeping the search local prevents the room tail from becoming the
    // selected correlation peak again.
    const std::size_t refineWindow = 768; // 16 ms @ 48 kHz
    const double localHalfRange =
        4.0 * settings.sampleRate / 1000.0;

    const double searchMin =
        std::max(
            -static_cast<double>(maxLag),
            onsetDelay - localHalfRange);

    const double searchMax =
        std::min(
            static_cast<double>(maxLag),
            onsetDelay + localHalfRange);

    const Measurement refined =
        measurePhaseBand(
            master,
            source,
            center,
            refineWindow,
            onsetDelay,
            searchMin,
            searchMax,
            settings.sampleRate,
            settings.phaseMinHz,
            settings.phaseMaxHz);

    double finalDelay = onsetDelay;
    double refineWeight = 0.0;

    // The onset detector gives the physical first-arrival estimate. The
    // waveform/PHAT step is allowed to refine it only when it remains close
    // to that first-arrival estimate and has a useful local correlation.
    if (refined.correlation >= 0.35 &&
        refined.confidence >= 0.40 &&
        std::abs(refined.finalDelay - onsetDelay) <=
            2.0 * settings.sampleRate / 1000.0) {
        finalDelay = refined.finalDelay;
        refineWeight = 1.0;
    }

    out.valid = true;
    out.delaySamples = finalDelay;
    out.masterOnsetSamples = masterOnset;
    out.sourceOnsetSamples = sourceOnset;
    out.refinedCorrelation = refined.correlation;

    const double onsetConfidence =
        std::sqrt(
            std::clamp(
                masterConfidence * sourceConfidence,
                0.0,
                1.0));

    out.confidence =
        std::clamp(
            0.65 * onsetConfidence +
            0.35 * (refineWeight > 0.0
                ? std::max(0.0, refined.confidence)
                : onsetConfidence),
            0.0,
            1.0);

    return out;
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

std::vector<Anchor> selectAnchors(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t window,
    std::size_t hop,
    double gateRatio,
    double separationSeconds,
    double sampleRate,
    std::size_t maxAnchors)
{
    std::vector<Anchor> candidates;
    const std::size_t usable = std::min(master.size(), source.size());
    const std::size_t half = window / 2;

    if (usable < window)
        return candidates;

    double maxEnergy = 0.0;
    std::vector<std::pair<std::size_t, double>> all;

    for (std::size_t center = half;
         center + half < usable;
         center += std::max<std::size_t>(1, hop)) {
        const double e = rmsAround(master, center, half);
        maxEnergy = std::max(maxEnergy, e);
        all.emplace_back(center, e);
    }

    if (all.empty() || maxEnergy <= 0.0)
        return candidates;

    double meanEnergy = 0.0;
    for (const auto& p : all)
        meanEnergy += p.second;
    meanEnergy /= static_cast<double>(all.size());

    const double gate = meanEnergy +
        std::clamp(gateRatio, 0.0, 1.0) * (maxEnergy - meanEnergy);

    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) {
                  return a.second > b.second;
              });

    const std::size_t minSeparation = static_cast<std::size_t>(
        std::max(1.0, separationSeconds * sampleRate));

    for (const auto& candidate : all) {
        if (candidate.second < gate)
            break;

        bool separated = true;
        for (const auto& selected : candidates) {
            const std::size_t d = candidate.first > selected.center
                ? candidate.first - selected.center
                : selected.center - candidate.first;
            if (d < minSeparation) {
                separated = false;
                break;
            }
        }

        if (!separated)
            continue;

        candidates.push_back({candidate.first, candidate.second});
        if (candidates.size() >= maxAnchors)
            break;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Anchor& a, const Anchor& b) {
                  return a.center < b.center;
              });
    return candidates;
}


std::vector<Anchor> selectTimelineAnchors(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t window,
    std::size_t hop,
    std::size_t maxAnchors)
{
    std::vector<Anchor> anchors;
    const std::size_t usable = std::min(master.size(), source.size());
    const std::size_t half = window / 2;

    if (usable < window || maxAnchors == 0)
        return anchors;

    std::size_t effectiveHop = std::max<std::size_t>(1, hop);
    const std::size_t span = usable - window;

    if (span > 0 && maxAnchors > 1) {
        const std::size_t maxByCount =
            (span + maxAnchors - 2) / (maxAnchors - 1);
        effectiveHop = std::max(effectiveHop, maxByCount);
    }

    for (std::size_t center = half;
         center + half < usable;
         center += effectiveHop) {
        anchors.push_back({
            center,
            rmsAround(master, center, half)
        });

        if (anchors.size() >= maxAnchors)
            break;
    }

    const std::size_t lastCenter = usable - half - 1;
    if (anchors.size() < maxAnchors &&
        lastCenter > half &&
        (anchors.empty() || anchors.back().center < lastCenter)) {
        anchors.push_back({
            lastCenter,
            rmsAround(master, lastCenter, half)
        });
    }

    return anchors;
}

Measurement measurePhaseBand(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t center,
    std::size_t window,
    double predictedDelaySamples,
    double searchMinSamples,
    double searchMaxSamples,
    double sampleRate,
    double phaseMinHz,
    double phaseMaxHz)
{
    Measurement out;

    const std::size_t half = window / 2;
    if (master.size() < window || source.size() < window)
        return out;

    if (center < half || center + half >= master.size())
        return out;

    const std::size_t fftSize = nextPow2(window * 2);
    std::vector<Complex> a(fftSize, Complex(0.0, 0.0));
    std::vector<Complex> b(fftSize, Complex(0.0, 0.0));

    const std::size_t first = center - half;

    for (std::size_t i = 0; i < window; ++i) {
        const std::size_t idx = first + i;
        if (idx < master.size())
            a[i] = static_cast<double>(master[idx]) * hann(i, window);
        if (idx < source.size())
            b[i] = static_cast<double>(source[idx]) * hann(i, window);
    }

    fft(a, false);
    fft(b, false);

    std::vector<Complex> cross(fftSize);

    const double nyquist = sampleRate * 0.5;
    const double bandMin =
        std::clamp(
            std::min(phaseMinHz, phaseMaxHz),
            0.0,
            nyquist);
    const double bandMax =
        std::clamp(
            std::max(phaseMinHz, phaseMaxHz),
            0.0,
            nyquist);
    const bool useBand =
        bandMax > 0.0 &&
        bandMax < nyquist - 1.0 &&
        bandMax > bandMin;

    for (std::size_t k = 0; k < fftSize; ++k) {
        if (useBand) {
            const std::size_t wrappedBin =
                k <= fftSize / 2
                    ? k
                    : fftSize - k;

            const double hz =
                static_cast<double>(wrappedBin) *
                sampleRate /
                static_cast<double>(fftSize);

            if (hz < bandMin || hz > bandMax) {
                cross[k] = Complex(0.0, 0.0);
                continue;
            }
        }

        const Complex c = b[k] * std::conj(a[k]);
        const double mag = std::abs(c);
        cross[k] =
            mag > 1.0e-14
                ? c / mag
                : Complex(0.0, 0.0);
    }

    fft(cross, true);

    const int maxLag = static_cast<int>(
        std::min(
            searchMaxSamples,
            static_cast<double>(fftSize / 2 - 2)));
    const int minLag = static_cast<int>(
        std::max(
            searchMinSamples,
            -static_cast<double>(fftSize / 2 - 2)));

    if (maxLag < minLag)
        return out;

    int bestLag = 0;
    double bestValue = -1.0;
    std::vector<double> magnitudes;
    magnitudes.reserve(static_cast<std::size_t>(maxLag - minLag + 1));

    for (int lag = minLag; lag <= maxLag; ++lag) {
        const std::size_t index = lag >= 0
            ? static_cast<std::size_t>(lag)
            : fftSize - static_cast<std::size_t>(-lag);

        const double value = cross[index].real();
        magnitudes.push_back(value);

        if (value > bestValue) {
            bestValue = value;
            bestLag = lag;
        }
    }

    if (bestValue <= 0.0)
        return out;

    double fractionalOffset = 0.0;
    if (bestLag > minLag && bestLag < maxLag) {
        const auto valueAt = [&](int lag) {
            const std::size_t index = lag >= 0
                ? static_cast<std::size_t>(lag)
                : fftSize - static_cast<std::size_t>(-lag);
            return cross[index].real();
        };

        const double ym = valueAt(bestLag - 1);
        const double y0 = valueAt(bestLag);
        const double yp = valueAt(bestLag + 1);
        const double denom = ym - 2.0 * y0 + yp;

        if (std::abs(denom) > 1.0e-12) {
            fractionalOffset = 0.5 * (ym - yp) / denom;
            fractionalOffset = std::clamp(fractionalOffset, -0.5, 0.5);
        }
    }

    out.phatDelay = static_cast<double>(bestLag) + fractionalOffset;

    std::sort(magnitudes.begin(), magnitudes.end());
    const double background = magnitudes[magnitudes.size() / 2];
    const double phatSharpness = std::clamp(
        (bestValue - background) /
        std::max(bestValue, 1.0e-9),
        0.0, 1.0);

    const double localSearch = 0.75;

    double bestWaveDelay = out.phatDelay;
    double bestCorrelation = -1.0;

    for (int step = -6; step <= 6; ++step) {
        const double d = out.phatDelay +
            static_cast<double>(step) * 0.125;
        if (d < searchMinSamples || d > searchMaxSamples)
            continue;

        const double corr = normalizedWaveformCorrelation(
            master, source, center, half, d);

        if (corr > bestCorrelation) {
            bestCorrelation = corr;
            bestWaveDelay = d;
        }
    }

    const double c0 = normalizedWaveformCorrelation(
        master, source, center, half, bestWaveDelay - 0.125);
    const double c1 = bestCorrelation;
    const double c2 = normalizedWaveformCorrelation(
        master, source, center, half, bestWaveDelay + 0.125);

    const double curvature = c0 - 2.0 * c1 + c2;
    if (std::abs(curvature) > 1.0e-12) {
        const double delta = 0.5 * (c0 - c2) / curvature;
        bestWaveDelay += std::clamp(delta * 0.125, -0.125, 0.125);
        bestCorrelation = normalizedWaveformCorrelation(
            master, source, center, half, bestWaveDelay);
    }

    out.waveformDelay = bestWaveDelay;
    out.correlation = std::max(0.0, bestCorrelation);

    const double difference = std::abs(out.waveformDelay - out.phatDelay);
    out.phaseAgreement = std::exp(
        -difference / std::max(0.25, localSearch));

    // GCC-PHAT is the primary measurement because it is a phase-only
    // cross-correlation. Waveform correlation is the independent sanity
    // check that prevents a narrow PHAT peak from being accepted when the
    // actual broadband waveforms disagree.
    out.finalDelay =
        0.70 * out.phatDelay +
        0.30 * out.waveformDelay;

    out.confidence = std::clamp(
        0.55 * phatSharpness +
        0.25 * out.correlation +
        0.20 * out.phaseAgreement,
        0.0, 1.0);

    return out;
}

Measurement measurePhase(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t center,
    std::size_t window,
    double predictedDelaySamples,
    double searchMinSamples,
    double searchMaxSamples,
    double sampleRate)
{
    return measurePhaseBand(
        master,
        source,
        center,
        window,
        predictedDelaySamples,
        searchMinSamples,
        searchMaxSamples,
        sampleRate,
        0.0,
        0.0);
}

Measurement measurePhaseAdaptive(
    const std::vector<float>& master,
    const std::vector<float>& source,
    std::size_t center,
    std::size_t window,
    double predictedDelaySamples,
    double searchMinSamples,
    double searchMaxSamples,
    const Settings& settings)
{
    const Measurement full = measurePhase(
        master,
        source,
        center,
        window,
        predictedDelaySamples,
        searchMinSamples,
        searchMaxSamples,
        settings.sampleRate);

    if (full.confidence >= settings.minConfidence)
        return full;

    // Recovery path for weak BOOM/LAV measurements:
    // 1) preserve the original full-band result;
    // 2) re-measure with a much shorter temporal window, which reduces the
    //    amount of reverberant tail inside the correlation;
    // 3) repeat the short-window measurement in the direct-arrival band.
    //
    // We only replace the original result when BOTH short measurements agree.
    // This prevents a single alternative correlation peak from becoming the
    // new answer.
    const std::size_t shortWindow =
        std::clamp<std::size_t>(
            static_cast<std::size_t>(
                std::llround(
                    32.0 * settings.sampleRate / 1000.0)),
            1024,
            window);

    const Measurement shortFull = measurePhase(
        master,
        source,
        center,
        shortWindow,
        predictedDelaySamples,
        searchMinSamples,
        searchMaxSamples,
        settings.sampleRate);

    const Measurement shortBand = measurePhaseBand(
        master,
        source,
        center,
        shortWindow,
        predictedDelaySamples,
        searchMinSamples,
        searchMaxSamples,
        settings.sampleRate,
        settings.phaseMinHz,
        settings.phaseMaxHz);

    const double shortAgreement =
        std::abs(
            shortFull.finalDelay -
            shortBand.finalDelay);

    const double agreementLimit =
        std::max(
            48.0,
            0.001 * settings.sampleRate);

    const bool shortResultsAgree =
        shortFull.confidence >= 0.35 &&
        shortBand.confidence >= 0.35 &&
        shortAgreement <= agreementLimit;

    if (shortResultsAgree) {
        // Prefer the band-limited result only when it is at least competitive
        // with the full short-window measurement. Otherwise the short full-band
        // measurement remains the safer direct-arrival estimate.
        return shortBand.confidence >=
                   shortFull.confidence * 0.90
            ? shortBand
            : shortFull;
    }

    // If the short-window methods disagree, keep the original result rather
    // than inventing a new delay from ambiguous reverberant material.
    return full;
}

double fallbackEstimate(
    const std::vector<float>& master,
    const std::vector<float>& source,
    int maxLag,
    double sampleRate,
    double& confidence)
{
    const std::size_t n = std::min(master.size(), source.size());
    if (n < 1024) {
        confidence = 0.0;
        return 0.0;
    }

    Settings s;
    s.sampleRate = sampleRate;
    s.maxDelayMs = 1000.0 * static_cast<double>(maxLag) / sampleRate;
    s.analysisWindowMs = 80.0;
    s.hopMs = 250.0;
    s.mode = Mode::Static;

    const Result r = AlignEngine::analyze(master, source, s);
    confidence = r.staticConfidence;
    return r.staticDelaySamples;
}

} // namespace

Result AlignEngine::analyze(
    const std::vector<float>& master,
    const std::vector<float>& source,
    const Settings& settings)
{
    Result result;

    const std::size_t usable = std::min(master.size(), source.size());
    if (usable < 2048 || settings.sampleRate <= 0.0)
        return result;

    const std::size_t window = std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::llround(settings.analysisWindowMs * settings.sampleRate / 1000.0)),
        1024,
        8192);

    const std::size_t hop = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::llround(settings.hopMs * settings.sampleRate / 1000.0)));

    const int maxLag = std::max(
        1,
        static_cast<int>(
            std::llround(settings.maxDelayMs * settings.sampleRate / 1000.0)));

    const auto staticAnchors = selectAnchors(
        master, source, window, hop,
        settings.energyGateRatio,
        settings.anchorSeparationMs / 1000.0,
        settings.sampleRate,
        std::max<std::size_t>(1, settings.staticAnchorCount));

    result.staticTotalWindows = static_cast<int>(staticAnchors.size());
    result.staticAnalysisTimeSec =
        staticAnchors.empty()
            ? 0.0
            : static_cast<double>(staticAnchors[staticAnchors.size() / 2].center) /
              settings.sampleRate;

    std::vector<double> staticDelays;
    std::vector<double> staticCorrelations;
    std::vector<double> staticConfidences;
    staticDelays.reserve(staticAnchors.size());

    const double seededDelay = settings.hasInitialDelaySamples
        ? settings.initialDelaySamples
        : 0.0;

    for (const auto& anchor : staticAnchors) {
        const Measurement m = measurePhaseAdaptive(
            master, source,
            anchor.center,
            window,
            seededDelay,
            -static_cast<double>(maxLag),
            static_cast<double>(maxLag),
            settings);

        if (m.confidence < settings.minConfidence * 0.75)
            continue;

        staticDelays.push_back(m.finalDelay);
        staticCorrelations.push_back(m.correlation);
        staticConfidences.push_back(m.confidence);
    }

    result.staticSupportWindows = static_cast<int>(staticDelays.size());

    if (!staticDelays.empty()) {
        result.staticDelaySamples = median(staticDelays);
        result.staticCorrelation = median(staticCorrelations);
        result.staticConfidence = median(staticConfidences);
    }

    // When the conventional RMS/GCC path is weak, explicitly look for the
    // first direct arrival. This is critical for far-field BOOM/LAV material
    // where the loudest 60 ms window may be dominated by room decay rather
    // than by the physical arrival of the useful signal.
    const bool needsTransientRescue =
        staticDelays.empty() ||
        result.staticConfidence < settings.minConfidence ||
        result.staticSupportWindows < 2;

    if (needsTransientRescue) {
        const TransientEstimate transient =
            estimateDirectTransient(
                master,
                source,
                settings,
                maxLag);

        if (transient.valid &&
            transient.confidence >= 0.35) {
            result.staticDelaySamples =
                transient.delaySamples;
            result.staticAnalysisTimeSec =
                transient.masterOnsetSamples /
                settings.sampleRate;
            result.staticCorrelation =
                transient.refinedCorrelation;
            result.staticConfidence =
                std::max(
                    result.staticConfidence,
                    transient.confidence);
            result.staticSupportWindows = 1;
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
                                32)));

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
            7);

        Measurement bestMeasurement;
        bool haveBest = false;

        for (const auto& anchor : coarseAnchors) {
            const Measurement m = measurePhaseAdaptive(
                master,
                source,
                anchor.center,
                window,
                0.0,
                -static_cast<double>(maxLag),
                static_cast<double>(maxLag),
                settings);

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

    std::vector<double> staticAbsoluteDeviations;
    staticAbsoluteDeviations.reserve(staticDelays.size());
    for (double d : staticDelays)
        staticAbsoluteDeviations.push_back(
            std::abs(d - staticCenter));

    const double staticMad =
        median(staticAbsoluteDeviations);

    const double dynamicThreshold =
        std::max(0.75, 0.45 * settings.sampleRate / 1000.0);

    bool coherentTemporalDrift = false;
    if (settings.mode != Mode::Static) {
        // Use a denser temporal scout than the old 7-point pass.
        // Real BOOM/LAV movement can contain quiet or ambiguous windows;
        // with only 7 samples, two bad endpoints can hide an otherwise clear
        // monotonic acoustic delay change.
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
            17);

        std::vector<std::pair<double, double>> scout;
        std::vector<double> scoutConfidences;
        scout.reserve(scoutAnchors.size());
        scoutConfidences.reserve(scoutAnchors.size());

        for (const auto& anchor : scoutAnchors) {
            const Measurement m = measurePhaseAdaptive(
                master,
                source,
                anchor.center,
                window,
                staticCenter,
                -static_cast<double>(maxLag),
                static_cast<double>(maxLag),
                settings);

            // PHAT can produce a mathematically sharp peak in a
            // quiet/ambiguous window even when the actual broadband waveform
            // correlation is effectively zero. Such a point must not count as
            // temporal evidence for AUTO DYNAMIC.
            constexpr double kTemporalCorrelationFloor = 0.20;
            if (m.confidence >= settings.minConfidence * 0.75 &&
                m.correlation >= kTemporalCorrelationFloor) {
                scout.emplace_back(
                    static_cast<double>(anchor.center) /
                        settings.sampleRate,
                    m.finalDelay);
                scoutConfidences.push_back(m.confidence);
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

            const double scoutConfidenceMedian =
                scoutConfidences.empty()
                    ? 0.0
                    : median(scoutConfidences);

            const bool strongScoutSupport =
                scout.size() >= 4 &&
                scoutConfidenceMedian >=
                    settings.minConfidence &&
                result.staticConfidence >=
                    settings.minConfidence;

            const bool linearEvidence =
                strongScoutSupport &&
                rSquared >= 0.45;

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

            const bool monotonicEvidence =
                strongScoutSupport &&
                meaningfulSteps >= 3 &&
                directionConsistency >= 0.67 &&
                robustShift > std::max(
                    2.0 * dynamicThreshold,
                    1.0 * settings.sampleRate / 1000.0);

            // A large early-vs-late shift is not enough by itself to
            // declare a dynamic acoustic delay. Reverberation, source
            // mismatch, or changing spectral balance can move the local
            // correlation peak while the true direct arrival is static.
            // In AUTO, require the static phase solution itself to be at or
            // above the normal confidence floor before this robust-only
            // temporal cue can promote the take to DYNAMIC.
            const bool robustTemporalEvidence =
                strongScoutSupport &&
                robustShift > std::max(
                    2.0 * dynamicThreshold,
                    1.0 * settings.sampleRate / 1000.0);

            // Requiring a real set of high-confidence scout measurements
            // prevents changing spectral balance/reverberation from being
            // mistaken for a time-varying acoustic delay.
            const double dynamicEndToEndThreshold =
                std::max(
                    3.0 * dynamicThreshold,
                    1.0 * settings.sampleRate / 1000.0);

            coherentTemporalDrift =
                strongScoutSupport &&
                endToEnd > dynamicEndToEndThreshold &&
                (linearEvidence ||
                 monotonicEvidence ||
                 robustTemporalEvidence);

            result.scoutCoherent = coherentTemporalDrift;
        }
    }

    const bool explicitDynamic = settings.mode == Mode::Dynamic;

    // In AUTO, a low-confidence static solution must remain conservative:
    // isolated anchor disagreement is not enough to promote BOOM/LAV material
    // to DYNAMIC. The temporal drift cues are allowed to do so only when the
    // underlying static phase solution is already trusted.
    const bool staticSpreadDynamic =
        staticDelays.size() >= 3 &&
        result.staticConfidence >= settings.minConfidence &&
        staticMad > dynamicThreshold;

    const bool needsDynamic = explicitDynamic ||
        knownRateDrift ||
        staticSpreadDynamic ||
        coherentTemporalDrift;

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
        std::max<std::size_t>(1, settings.maxDynamicAnchors));

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

    constexpr double kDynamicCorrelationFloor = 0.20;

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

        const Measurement m = measurePhaseAdaptive(
            master, source,
            anchor.center,
            window,
            predictedDelay,
            first ? -static_cast<double>(maxLag) : dynamicMin,
            first ? static_cast<double>(maxLag) : dynamicMax,
            settings);

        const bool correlationSupportsDynamic =
            explicitDynamic ||
            knownRateDrift ||
            m.correlation >= kDynamicCorrelationFloor;

        if (m.confidence >= dynamicPointMinConfidence &&
            correlationSupportsDynamic) {
            double acceptedDelay = m.finalDelay;

            if (!first) {
                const double delta = std::abs(acceptedDelay - predictedDelay);
                if (delta > slewAllowance * 1.25) {
                    // A discontinuity larger than the physical tracking
                    // allowance is more likely a false acoustic match than a
                    // genuine microphone movement. Keep the previous solution
                    // and do not create a false warp point.
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
                            32)));

            if (timeline.size() >= 2) {
                const double durationSec =
                    static_cast<double>(usable - 1) /
                    settings.sampleRate;
                const double centerTime =
                    durationSec * 0.5;
                const double slopePerSec =
                    (1.0 - settings.playbackRateRatio) *
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
