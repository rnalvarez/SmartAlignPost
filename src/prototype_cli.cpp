#include "align_engine.h"

#include <algorithm>
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
    out.mono.resize(maxFrames);

    f.clear();
    f.seekg(dataPos + std::streamoff(requestedStart * frameBytes));
    std::vector<uint8_t> frame(frameBytes);
    for (size_t i = 0; i < maxFrames; ++i) {
        f.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        if (f.gcount() != static_cast<std::streamsize>(frame.size())) {
            out.mono.resize(i);
            break;
        }
        double sum = 0.0;
        for (int c = 0; c < out.channels; ++c)
            sum += pcmSample(frame.data() + c * bytesPerSample, out.bitsPerSample, out.format);
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 5 && argc != 6) {
        emitError("Uso: SmartAlignPostPrototype.exe <MASTER.wav> <SOURCE.wav> [MASTER_START_SEC SOURCE_START_SEC [DURATION_SEC]]");
        return 2;
    }

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
    if (argc == 6) {
        if (!parseNonNegative(argv[5], "DURATION_SEC", duration, error)) {
            emitError(error); return 6;
        }
        if (duration < 0.5 || duration > 180.0) {
            emitError("DURATION_SEC debe estar entre 0.5 y 180 segundos.");
            return 6;
        }
    }

    WavData master, source;
    if (!loadWav(argv[1], master, error, masterStart, duration)) {
        emitError(error); return 3;
    }
    if (!loadWav(argv[2], source, error, sourceStart, duration)) {
        emitError(error); return 3;
    }

    if (master.sampleRate != source.sampleRate) {
        emitError("sample rates distintos (MASTER=" + std::to_string(master.sampleRate) +
                  ", SOURCE=" + std::to_string(source.sampleRate) + ")");
        return 4;
    }

    const size_t n = std::min(master.mono.size(), source.mono.size());
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

    const bool dynamic = (argc == 6);
    settings.mode = dynamic ? sap::Mode::Dynamic : sap::Mode::Static;

    // DYNAMIC prioritizes throughput: the delay can change on the scale of
    // hundreds of milliseconds in production, so a 120 ms window and 200 ms
    // hop provide adequate temporal resolution while reducing FFT work by
    // several times versus the original 200/100 ms configuration.
    if (dynamic) {
        settings.analysisWindowMs = 120.0;
        settings.hopMs = 200.0;
        settings.smoothingMs = 180.0;
        settings.maxSlewMsPerSecond = 20.0;
    }

    const auto result = sap::AlignEngine::analyze(master.mono, source.mono, settings);
    const double staticDelayMs = result.staticDelaySamples * 1000.0 / settings.sampleRate;

    std::cout << "MODE=" << (dynamic ? "DYNAMIC" : "STATIC") << "\n";
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
            std::cout << "POINT=" << p.timeSec << "," << delayMs << ","
                       << p.delaySamples << "," << p.confidence << "\n";
        }
    }

    return 0;
}
