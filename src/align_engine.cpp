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

    // Confidence combines correlation strength and peak separation.
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
        return r;
    }

    const int maxLag = std::max(1, std::min<int>(requestedMaxLag, static_cast<int>(win / 2) - 1));

    // STATIC: search the recording in overlapping windows and keep the
    // strongest, most unambiguous correlation peak.
    double bestConfidence = -1.0;
    double bestDelay = 0.0;
    size_t bestPos = 0;

    for (size_t pos = 0; pos + win <= n; pos += hop) {
        double c = 0.0;
        const double d = estimateDelay(master.data() + pos,
                                       source.data() + pos,
                                       win, maxLag, c);
        if (c > bestConfidence) {
            bestConfidence = c;
            bestDelay = d;
            bestPos = pos;
        }
    }

    r.staticDelaySamples = bestDelay;
    r.staticConfidence = std::max(0.0, bestConfidence);
    r.staticAnalysisTimeSec = static_cast<double>(bestPos) / settings.sampleRate;
    r.staticCorrelation = normalizedCorrelation(
        master.data() + bestPos,
        source.data() + bestPos,
        win,
        static_cast<int>(bestDelay));

    if (settings.mode == Mode::Static) return r;

    double previous = r.staticDelaySamples;
    for (size_t pos=0; pos+win<=n; pos+=hop) {
        double c=0.0;
        double d = estimateDelay(master.data()+pos, source.data()+pos,
                                 win, maxLag, c);

        if (c < settings.minConfidence) {
            d = previous;
        } else {
            // Conservative temporal slew limiting.
            const double maxStep =
                settings.maxSlewMsPerSecond / 1000.0 *
                (static_cast<double>(hop) / settings.sampleRate) *
                settings.sampleRate;
            d = std::clamp(d, previous-maxStep, previous+maxStep);
        }

        // Exponential smoothing.
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
