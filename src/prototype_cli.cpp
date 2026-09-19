#include "align_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    uint16_t format = 0; // 1=PCM, 3=IEEE float
    std::vector<float> mono;
};

uint32_t readU32(std::ifstream& f) {
    uint8_t b[4]{};
    f.read(reinterpret_cast<char*>(b), 4);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) |
           (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

uint16_t readU16(std::ifstream& f) {
    uint8_t b[2]{};
    f.read(reinterpret_cast<char*>(b), 2);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

bool readFourCC(std::ifstream& f, const char* expected) {
    char id[4]{};
    f.read(id, 4);
    return std::memcmp(id, expected, 4) == 0;
}

float pcmSample(const uint8_t* p, int bits, uint16_t format) {
    if (format == 3 && bits == 32) {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    if (bits == 16) {
        int16_t s = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
        return float(s) / 32768.0f;
    }
    if (bits == 24) {
        int32_t s = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
        if (s & 0x00800000) s |= ~0x00FFFFFF;
        return float(s) / 8388608.0f;
    }
    if (bits == 32) {
        int32_t s = int32_t(p[0]) | (int32_t(p[1]) << 8) |
                    (int32_t(p[2]) << 16) | (int32_t(p[3]) << 24);
        return float(s) / 2147483648.0f;
    }
    return 0.0f;
}

bool loadWav(const std::string& path,
             WavData& out,
             std::string& error,
             double startSeconds = 0.0,
             double durationSeconds = 60.0) {
    if (startSeconds < 0.0) startSeconds = 0.0;
    if (durationSeconds <= 0.0) durationSeconds = 60.0;

    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "No se pudo abrir: " + path; return false; }
    if (!readFourCC(f, "RIFF")) { error = "No es WAV RIFF: " + path; return false; }
    (void)readU32(f);
    if (!readFourCC(f, "WAVE")) { error = "No es WAVE: " + path; return false; }

    bool haveFmt = false, haveData = false;
    uint32_t dataSize = 0;
    std::streampos dataPos{};

    while (f && !haveData) {
        char id[4]{};
        f.read(id, 4);
        if (f.gcount() != 4) break;
        const uint32_t size = readU32(f);
        const std::streampos next = f.tellg() + std::streamoff(size + (size & 1));

        if (std::memcmp(id, "fmt ", 4) == 0) {
            out.format = readU16(f);
            out.channels = int(readU16(f));
            out.sampleRate = int(readU32(f));
            (void)readU32(f);
            (void)readU16(f);
            out.bitsPerSample = int(readU16(f));
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataPos = f.tellg();
            dataSize = size;
            haveData = true;
        }
        f.seekg(next);
    }

    if (!haveFmt || !haveData) { error = "WAV sin fmt/data: " + path; return false; }
    if (out.channels < 1) { error = "WAV sin canales: " + path; return false; }
    if (!((out.format == 1 && (out.bitsPerSample == 16 || out.bitsPerSample == 24 || out.bitsPerSample == 32)) ||
          (out.format == 3 && out.bitsPerSample == 32))) {
        error = "Formato WAV no soportado (usar PCM 16/24/32 o float32): " + path;
        return false;
    }

    const size_t bytesPerSample = static_cast<size_t>(out.bitsPerSample / 8);
    const size_t frameBytes = bytesPerSample * static_cast<size_t>(out.channels);
    if (frameBytes == 0) { error = "Frame WAV inválido: " + path; return false; }
    const size_t frames = dataSize / frameBytes;

    const size_t requestedStart = static_cast<size_t>(startSeconds * static_cast<double>(out.sampleRate));
    if (requestedStart >= frames) {
        error = "Inicio de análisis fuera del archivo: " + path +
                " (start=" + std::to_string(startSeconds) + "s, duration=" +
                std::to_string(static_cast<double>(frames) / out.sampleRate) + "s)";
        return false;
    }

    const size_t requestedFrames = static_cast<size_t>(durationSeconds * static_cast<double>(out.sampleRate));
    const size_t maxFrames = std::min<size_t>(frames - requestedStart, requestedFrames);
    if (maxFrames == 0) {
        error = "No hay frames disponibles para el tramo solicitado: " + path;
        return false;
    }

    // Read the requested audio block in one I/O operation instead of issuing
    // one f.read() call per audio frame. The previous implementation could
    // perform millions of tiny reads on a long production WAV and dominate
    // the DYNAMIC analysis time before the DSP even started.
    const size_t bytesToRead = maxFrames * frameBytes;
    std::vector<uint8_t> raw(bytesToRead);

    f.clear();
    f.seekg(dataPos + std::streamoff(requestedStart * frameBytes));
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytesToRead));
    const size_t bytesRead = static_cast<size_t>(std::max<std::streamsize>(0, f.gcount()));
    const size_t actualFrames = bytesRead / frameBytes;
    if (actualFrames == 0) {
        error = "No se pudieron leer frames del tramo solicitado: " + path;
        return false;
    }

    out.mono.resize(actualFrames);
    for (size_t i = 0; i < actualFrames; ++i) {
        const uint8_t* frame = raw.data() + i * frameBytes;
        double sum = 0.0;
        for (int c = 0; c < out.channels; ++c)
            sum += pcmSample(frame + c * bytesPerSample, out.bitsPerSample, out.format);
        out.mono[i] = static_cast<float>(sum / out.channels);
    }
    return true;
}

bool parseNonNegative(const char* text, const char* name, double& value, std::string& error) {
    try {
        value = std::stod(text);
        if (value < 0.0) {
            error = std::string("ERROR: ") + name + " no puede ser negativo.";
            return false;
        }
        return true;
    } catch (...) {
        error = std::string("ERROR: valor inválido para ") + name + ": " + text;
        return false;
    }
}

void emitError(const std::string& message) {
    std::cout << "ERROR=" << message << "\n";
}

std::vector<float> resampleToProjectTime(
    const std::vector<float>& native,
    double playbackRate,
    size_t outputFrames)
{
    std::vector<float> out(outputFrames, 0.0f);
    if (native.empty() || playbackRate <= 0.0) return out;

    for (size_t i = 0; i < outputFrames; ++i) {
        const double srcIndex = static_cast<double>(i) * playbackRate;
        if (srcIndex < 0.0 ||
            srcIndex >= static_cast<double>(native.size())) {
            break;
        }

        const size_t i0 = static_cast<size_t>(std::floor(srcIndex));
        const size_t i1 = std::min(i0 + 1, native.size() - 1);
        const double frac = srcIndex - static_cast<double>(i0);
        out[i] = static_cast<float>(
            (1.0 - frac) * native[i0] +
            frac * native[i1]);
    }

    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 5 && argc != 6 && argc != 7 && argc != 9) {
        emitError("Uso: SmartAlignPostPrototype.exe <MASTER.wav> <SOURCE.wav> [MASTER_START_SEC SOURCE_START_SEC [DURATION_SEC [INITIAL_DELAY_SAMPLES [MASTER_PLAYRATE SOURCE_PLAYRATE]]]]");
        return 2;
    }

    const auto totalStart = std::chrono::steady_clock::now();

    double masterStart = 0.0;
    double sourceStart = 0.0;
    double duration = 60.0;
    std::string error;

    if (argc == 5 || argc == 6) {
        if (!parseNonNegative(argv[3], "MASTER_START_SEC", masterStart, error)) {
            emitError(error); return 6;
        }
        if (!parseNonNegative(argv[4], "SOURCE_START_SEC", sourceStart, error)) {
            emitError(error); return 6;
        }
    }
    double initialDelaySamples = 0.0;
    bool hasInitialDelay = false;
    double masterPlayRate = 1.0;
    double sourcePlayRate = 1.0;
    if (argc == 6 || argc == 7) {
        if (!parseNonNegative(argv[5], "DURATION_SEC", duration, error)) {
            emitError(error); return 6;
        }
        if (duration < 0.5 || duration > 180.0) {
            emitError("DURATION_SEC debe estar entre 0.5 y 180 segundos.");
            return 6;
        }
    }
    if (argc == 7 || argc == 9) {
        try {
            initialDelaySamples = std::stod(argv[6]);
            hasInitialDelay = true;
        } catch (...) {
            emitError(std::string("ERROR: valor inválido para INITIAL_DELAY_SAMPLES: ") + argv[6]);
            return 6;
        }
    }

    if (argc == 9) {
        try {
            masterPlayRate = std::stod(argv[7]);
            sourcePlayRate = std::stod(argv[8]);
            if (masterPlayRate <= 0.0 || sourcePlayRate <= 0.0) {
                emitError("MASTER_PLAYRATE y SOURCE_PLAYRATE deben ser > 0.");
                return 6;
            }
        } catch (...) {
            emitError("ERROR: valor inválido para MASTER_PLAYRATE/SOURCE_PLAYRATE.");
            return 6;
        }
    }

    WavData master, source;
    const auto loadStart = std::chrono::steady_clock::now();
    const double masterReadDuration = duration * masterPlayRate;
    const double sourceReadDuration = duration * sourcePlayRate;

    if (!loadWav(argv[1], master, error, masterStart, masterReadDuration)) {
        emitError(error); return 3;
    }
    if (!loadWav(argv[2], source, error, sourceStart, sourceReadDuration)) {
        emitError(error); return 3;
    }
    const auto loadEnd = std::chrono::steady_clock::now();

    if (master.sampleRate != source.sampleRate) {
        emitError("sample rates distintos (MASTER=" + std::to_string(master.sampleRate) +
                  ", SOURCE=" + std::to_string(source.sampleRate) + ")");
        return 4;
    }

    // Convert both takes onto the same PROJECT-TIME sample grid. The source
    // WAV itself may be played at a different D_PLAYRATE in REAPER. Without
    // this normalization the analyzer compares two files at native speed and
    // completely misses a non-destructive time-stretch applied to the SOURCE.
    const size_t projectFrames = static_cast<size_t>(
        std::floor(duration * master.sampleRate));
    const auto masterProject = resampleToProjectTime(
        master.mono, masterPlayRate, projectFrames);
    const auto sourceProject = resampleToProjectTime(
        source.mono, sourcePlayRate, projectFrames);

    const size_t n = std::min(masterProject.size(), sourceProject.size());
    if (n < 1024) {
        emitError("señal demasiado corta para analizar después del offset seleccionado");
        return 5;
    }

    sap::Settings settings;
    settings.sampleRate = static_cast<double>(master.sampleRate);
    settings.maxDelayMs = 12.0;
    settings.analysisWindowMs = 200.0;
    settings.hopMs = 100.0;
    settings.minConfidence = 0.80;
    settings.smoothingMs = 120.0;
    settings.maxSlewMsPerSecond = 12.0;

    // Both 6-argument and 7-argument invocations are DYNAMIC.
    // The 7-argument form carries INITIAL_DELAY_SAMPLES for continuity
    // between REAPER chunks. Previously argc==7 was accidentally treated
    // as STATIC, so every chunk after the first lost the dynamic settings
    // and re-ran the expensive STATIC consensus analysis.
    const bool dynamic = (argc == 6 || argc == 7 || argc == 9);
    settings.mode = dynamic ? sap::Mode::Dynamic : sap::Mode::Static;

    // DYNAMIC prioritizes throughput: the delay can change on the scale of
    // hundreds of milliseconds in production, so a 120 ms window and 200 ms
    // hop provide adequate temporal resolution while reducing FFT work by
    // several times versus the original 200/100 ms configuration.
    if (dynamic) {
        // High temporal resolution for a moving SOURCE. The measurement time
        // is the window center; a 10 ms hop gives the warp frequent control
        // points while the landmark pass adds exact acoustic anchors.
        // Dynamic drift can exceed the nominal single-mic acoustic offset
        // during long takes or artificial rate-drift tests.
        settings.maxDelayMs = 40.0;
        settings.analysisWindowMs = 40.0;
        settings.hopMs = 10.0;
        settings.smoothingMs = 10.0;
        settings.maxSlewMsPerSecond = 600.0;
        settings.dynamicFineWindowMs = 24.0;
        settings.dynamicEventFrameMs = 3.0;
        settings.dynamicEventMinSeparationMs = 20.0;
        settings.dynamicEventMatchWindowMs = 15.0;
        settings.dynamicMicroWindowMs = 12.0;
        settings.dynamicMicroSearchMs = 3.0;
        settings.hasInitialDelaySamples = hasInitialDelay;
        settings.initialDelaySamples = initialDelaySamples;
    }

    const auto analyzeStart = std::chrono::steady_clock::now();
    const auto result = sap::AlignEngine::analyze(masterProject, sourceProject, settings);
    const auto analyzeEnd = std::chrono::steady_clock::now();
    const double staticDelayMs = result.staticDelaySamples * 1000.0 / settings.sampleRate;
    const double loadMs = std::chrono::duration<double, std::milli>(loadEnd - loadStart).count();
    const double analyzeMs = std::chrono::duration<double, std::milli>(analyzeEnd - analyzeStart).count();
    const double totalMs = std::chrono::duration<double, std::milli>(
        analyzeEnd - totalStart).count();

    std::cout << "MODE=" << (dynamic ? "DYNAMIC" : "STATIC") << "\n";
    std::cout << "LOAD_MS=" << loadMs << "\n";
    std::cout << "ANALYZE_MS=" << analyzeMs << "\n";
    std::cout << "TOTAL_MS=" << totalMs << "\n";
    std::cout << "DELAY_MS=" << staticDelayMs << "\n";
    std::cout << "DELAY_SAMPLES=" << result.staticDelaySamples << "\n";
    std::cout << "CONFIDENCE=" << result.staticConfidence << "\n";
    std::cout << "WINDOW_SEC=" << result.staticAnalysisTimeSec << "\n";
    std::cout << "CORRELATION=" << result.staticCorrelation << "\n";
    std::cout << "SUPPORT_WINDOWS=" << result.staticSupportWindows << "\n";
    std::cout << "TOTAL_WINDOWS=" << result.staticTotalWindows << "\n";

    if (dynamic) {
        std::cout << "CURVE_COUNT=" << result.curve.size() << "\n";
        for (const auto& p : result.curve) {
            const double delayMs = p.delaySamples * 1000.0 / settings.sampleRate;
            // Field order: time, delayMs, delaySamples, confidence. delayMs
            // is the one callers should use to convert into D_STARTOFFS
            // seconds (matching how the DELAY_MS summary line above is
            // used) -- delaySamples alone is not a time unit and dividing
            // it by a take's D_PLAYRATE (as the pre-fix DYNAMIC REAPER
            // scripts did) is a unit-conversion bug, not a valid seconds
            // conversion.
            const double sourceAbsoluteSec =
                sourceStart + p.sourceTimeSec * sourcePlayRate;
            // Field order:
            // master-local-time, delayMs, delaySamples, confidence, keyPoint,
            // source-absolute-time.  The last field is the actual paired
            // SOURCE position and is preferred by the REAPER layer.
            std::cout << "POINT=" << p.timeSec << "," << delayMs << ","
                       << p.delaySamples << "," << p.confidence << ","
                       << (p.keyPoint ? 1 : 0) << ","
                       << sourceAbsoluteSec << "\n";
        }
    }

    return 0;
}
