#pragma once

#include <cstddef>
#include <vector>

namespace sap {

enum class Mode {
    Static,
    Dynamic,
    Auto
};

struct Settings {
    Mode mode = Mode::Auto;
    double sampleRate = 48000.0;
    double maxDelayMs = 20.0;
    double analysisWindowMs = 60.0;
    double hopMs = 250.0;
    double minConfidence = 0.72;
    double maxSlewMsPerSecond = 100.0;
    double energyGateRatio = 0.35;
    double anchorSeparationMs = 180.0;
    std::size_t staticAnchorCount = 8;
    std::size_t maxDynamicAnchors = 240;
    // Local waveform fallback used by the AUTO DYNAMIC scout when a
    // consolidated/timestretched SOURCE weakens GCC-PHAT confidence.
    double dynamicScoutWindowMs = 12.0;
    double dynamicScoutSearchMs = 3.0;
    double phaseMinHz = 250.0;
    double phaseMaxHz = 7000.0;
    // Known project-time playback-rate ratio SOURCE / MASTER. A value
    // different from 1.0 is deterministic evidence of a temporal drift.
    double playbackRateRatio = 1.0;
    bool hasInitialDelaySamples = false;
    double initialDelaySamples = 0.0;
};

struct Point {
    double timeSec = 0.0;
    double delaySamples = 0.0;
    double confidence = 0.0;
    bool keyPoint = false;
    double phatDelaySamples = 0.0;
    double waveformDelaySamples = 0.0;
    double phaseAgreement = 0.0;
};

struct Result {
    double staticDelaySamples = 0.0;
    double staticConfidence = 0.0;
    double staticAnalysisTimeSec = 0.0;
    double staticCorrelation = 0.0;
    int staticSupportWindows = 0;
    int staticTotalWindows = 0;
    int scoutPoints = 0;
    double scoutFirstDelaySamples = 0.0;
    double scoutLastDelaySamples = 0.0;
    double scoutR2 = 0.0;
    double scoutDirectionConsistency = 0.0;
    double scoutRobustShiftSamples = 0.0;
    bool scoutCoherent = false;
    std::vector<Point> curve;
    Mode modeUsed = Mode::Static;
};

class AlignEngine {
public:
    static Result analyze(const std::vector<float>& master,
                          const std::vector<float>& source,
                          const Settings& settings);

    static double estimateDelay(const float* master,
                                const float* source,
                                std::size_t n,
                                int maxLag,
                                double sampleRate,
                                double& confidence);
};

} // namespace sap
