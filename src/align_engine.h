#pragma once
#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace sap {

enum class Mode { Static, Dynamic };

struct Settings {
    Mode mode = Mode::Static;
    double sampleRate = 48000.0;
    double maxDelayMs = 12.0;
    double analysisWindowMs = 200.0;
    double hopMs = 50.0;
    double minConfidence = 0.80;
    double smoothingMs = 200.0;
    double maxSlewMsPerSecond = 8.0;
    // DYNAMIC multi-resolution refinement.
    // DYNAMIC is landmark-driven: the delay curve should follow acoustic
    // events closely enough to build a genuinely time-varying warp.
    double dynamicFineWindowMs = 24.0;
    double dynamicEventFrameMs = 3.0;
    double dynamicEventMinSeparationMs = 20.0;
    double dynamicEventThreshold = 0.20;
    double dynamicEventMatchWindowMs = 15.0;
    double dynamicMicroWindowMs = 12.0;
    double dynamicMicroSearchMs = 3.0;
    bool hasInitialDelaySamples = false;
    double initialDelaySamples = 0.0;
};

struct Point {
    double timeSec = 0.0;
    double delaySamples = 0.0;
    double confidence = 0.0;
    bool keyPoint = false;
};

struct Result {
    double staticDelaySamples = 0.0;
    double staticConfidence = 0.0;
    double staticAnalysisTimeSec = 0.0;
    double staticCorrelation = 0.0;
    int staticSupportWindows = 0;
    int staticTotalWindows = 0;
    std::vector<Point> curve;
};

class AlignEngine {
public:
    static Result analyze(const std::vector<float>& master,
                          const std::vector<float>& source,
                          const Settings& settings);

    static double estimateDelay(const float* master,
                                const float* source,
                                size_t n,
                                int maxLag,
                                double sampleRate,
                                double& confidence);

private:
    static double normalizedCorrelation(const float* a, const float* b,
                                        size_t n, int lag);
};

}
