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

bool loadWav(const std::string& path, WavData& out, std::string& error) {
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
            (void)readU32(f); // byte rate
            (void)readU16(f); // block align
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

    const size_t maxFrames = std::min<size_t>(frames, static_cast<size_t>(out.sampleRate) * 10u);
    out.mono.resize(maxFrames);

    f.clear();
    f.seekg(dataPos);
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

}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Uso: SmartAlignPostPrototype.exe <MASTER.wav> <SOURCE.wav>\n";
        return 2;
    }

    WavData master, source;
    std::string error;
    if (!loadWav(argv[1], master, error)) { std::cerr << error << "\n"; return 3; }
    if (!loadWav(argv[2], source, error)) { std::cerr << error << "\n"; return 3; }
    if (master.sampleRate != source.sampleRate) {
        std::cerr << "ERROR: sample rates distintos (MASTER=" << master.sampleRate
                  << ", SOURCE=" << source.sampleRate << ").\n";
        return 4;
    }

    const size_t n = std::min(master.mono.size(), source.mono.size());
    if (n < 1024) {
        std::cerr << "ERROR: señal demasiado corta para analizar.\n";
        return 5;
    }

    sap::Settings settings;
    settings.mode = sap::Mode::Static;
    settings.sampleRate = static_cast<double>(master.sampleRate);
    settings.maxDelayMs = 12.0;
    settings.analysisWindowMs = 200.0;

    const auto result = sap::AlignEngine::analyze(master.mono, source.mono, settings);
    const double delayMs = result.staticDelaySamples * 1000.0 / settings.sampleRate;

    // Machine-readable one-line fields for the REAPER bridge and diagnostics.
    std::cout << "DELAY_MS=" << delayMs << "\n";
    std::cout << "DELAY_SAMPLES=" << result.staticDelaySamples << "\n";
    std::cout << "CONFIDENCE=" << result.staticConfidence << "\n";
    std::cout << "WINDOW_SEC=" << result.staticAnalysisTimeSec << "\n";
    std::cout << "CORRELATION=" << result.staticCorrelation << "\n";
    std::cout << "SUPPORT_WINDOWS=" << result.staticSupportWindows << "\n";
    std::cout << "TOTAL_WINDOWS=" << result.staticTotalWindows << "\n";
    return 0;
}
