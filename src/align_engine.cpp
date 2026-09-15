#include "align_engine.h"

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cmath>
#include <vector>

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

    const size_t fftSize = nextPowerOfTwo(n);
    std::vector<Complex> A(fftSize, Complex(0.0, 0.0));
    std::vector<Complex> B(fftSize, Complex(0.0, 0.0));

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
    fft(B, false);

    for (size_t k = 0; k < fftSize; ++k) {
        const Complex cross = A[k] * std::conj(B[k]);
        const double mag = std::abs(cross);
        A[k] = mag > 1e-12 ? cross / mag : Complex(0.0, 0.0);
    }

    fft(A, true);

    double best = -1.0;
    double second = -1.0;
    int bestLag = 0;
    const int span = std::min(maxLag, static_cast<int>(fftSize / 2) - 1);

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

    double refinedLag = static_cast<double>(bestLag);
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

    peakCorrelation = std::clamp(best, 0.0, 1.0);
    const double prominence = std::max(0.0, best - std::max(0.0, second));
    const double separation = best > 1e-12 ? prominence / best : 0.0;
    confidence = std::clamp(
        peakCorrelation * (0.65 + 0.35 * separation),
        0.0, 1.0);

    // Positive delay means SOURCE occurs later than MASTER. The
    // A*conj(B) cross-spectrum produces the opposite lag convention,
    // therefore invert the sign before returning the public delay.
    return -refinedLag;
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
                                   double& confidence)
{
    double peak = 0.0;
    return gccPhatDelay(master, source, n, maxLag, 48000.0, confidence, peak);
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

    double previous = r.staticDelaySamples;
    for (size_t pos=0; pos+win<=n; pos+=hop) {
        double c=0.0;
        double peak=0.0;
        double d = gccPhatDelay(master.data()+pos, source.data()+pos,
                                win, maxLag, settings.sampleRate, c, peak);

        if (c < settings.minConfidence) {
            d = previous;
        } else {
            const double maxStep =
                settings.maxSlewMsPerSecond / 1000.0 *
                (static_cast<double>(hop) / settings.sampleRate) *
                settings.sampleRate;
            d = std::clamp(d, previous-maxStep, previous+maxStep);
        }

        const double tau = std::max(0.001, settings.smoothingMs/1000.0);
        const double dt = static_cast<double>(hop)/settings.sampleRate;
        const double alpha = 1.0 - std::exp(-dt/tau);
        d = previous + alpha*(d-previous);

        r.curve.push_back({static_cast<double>(pos)/settings.sampleRate, d, c});
        previous=d;
    }
    return r;
}

}
