#include "align_engine.h"

namespace sap {

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
    double best=-2.0, second=-2.0;
    int bestLag=0;

    for (int lag=-maxLag; lag<=maxLag; ++lag) {
        const double c = normalizedCorrelation(master, source, n, lag);
        if (c > best) { second=best; best=c; bestLag=lag; }
        else if (c > second) second=c;
    }

    confidence = std::clamp((best + 1.0) * 0.5, 0.0, 1.0);
    confidence *= std::clamp(0.5 + (best-second)*2.0, 0.0, 1.0);

    return static_cast<double>(bestLag);
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
        const int maxLag = std::max(1, std::min<int>(requestedMaxLag, static_cast<int>(n / 2)));
        r.staticDelaySamples = estimateDelay(master.data(), source.data(),
                                             n, maxLag, c);
        r.staticConfidence=c;
        r.staticAnalysisTimeSec = 0.0;
        r.staticCorrelation = normalizedCorrelation(
            master.data(), source.data(), n, static_cast<int>(r.staticDelaySamples));
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
        const double d = estimateDelay(master.data() + pos,
                                       source.data() + pos,
                                       win, maxLag, c);
        const int lag = static_cast<int>(d);
        const double corr = normalizedCorrelation(master.data() + pos,
                                                  source.data() + pos,
                                                  win, lag);
        const int index = lag + maxLag;
        if (index >= 0 && index < lagSpan) {
            const double score = c * std::max(0.0, corr);
            lagScore[static_cast<size_t>(index)] += score;
            lagConfidence[static_cast<size_t>(index)] += c;
            lagCorrelationSum[static_cast<size_t>(index)] += std::max(0.0, corr);
            lagCount[static_cast<size_t>(index)] += 1;
            if (corr > lagBestCorrelation[static_cast<size_t>(index)]) {
                lagBestCorrelation[static_cast<size_t>(index)] = corr;
                lagBestTime[static_cast<size_t>(index)] =
                    static_cast<double>(pos) / settings.sampleRate;
            }
        }

        if (c > strongestConfidence) {
            strongestConfidence = c;
            strongestDelay = d;
            strongestPos = pos;
            strongestCorrelation = corr;
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

    // For consensus mode, repeated agreement across windows is the primary
    // reliability signal. A perfect correlation repeated throughout the clip
    // should be trusted even when the local second-peak separation is small.
    const double supportStrength = std::clamp(supportRatio * 2.0, 0.0, 1.0);
    const double correlationStrength = std::clamp(averageSupportCorrelation, 0.0, 1.0);
    const double localPeakStrength = std::clamp(averageSupportConfidence, 0.0, 1.0);
    r.staticConfidence = std::clamp(
        correlationStrength * supportStrength * (0.75 + 0.25 * localPeakStrength),
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
        double d = estimateDelay(master.data()+pos, source.data()+pos,
                                 win, maxLag, c);

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
