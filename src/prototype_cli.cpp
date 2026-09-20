#include "align_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi =
    3.1415926535897932384626433832795;

void writeU16(
    std::ofstream& f,
    uint16_t v)
{
    f.put(static_cast<char>(v & 0xff));
    f.put(static_cast<char>((v >> 8) & 0xff));
}

void writeU32(
    std::ofstream& f,
    uint32_t v)
{
    f.put(static_cast<char>(v & 0xff));
    f.put(static_cast<char>((v >> 8) & 0xff));
    f.put(static_cast<char>((v >> 16) & 0xff));
    f.put(static_cast<char>((v >> 24) & 0xff));
}

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    uint16_t format = 0;
    std::vector<float> mono;
    // Interleaved samples are retained for the DYNAMIC_RENDER path so the
    // correction can preserve the source channel count.
    std::vector<float> interleaved;
};

uint32_t readU32(std::ifstream& f)
{
    uint8_t b[4]{};
    f.read(reinterpret_cast<char*>(b), 4);
    return uint32_t(b[0]) |
           (uint32_t(b[1]) << 8) |
           (uint32_t(b[2]) << 16) |
           (uint32_t(b[3]) << 24);
}

uint16_t readU16(std::ifstream& f)
{
    uint8_t b[2]{};
    f.read(reinterpret_cast<char*>(b), 2);
    return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}

bool readFourCC(std::ifstream& f, const char* expected)
{
    char id[4]{};
    f.read(id, 4);
    return std::memcmp(id, expected, 4) == 0;
}

float pcmSample(const uint8_t* p, int bits, uint16_t format)
{
    if (format == 3 && bits == 32) {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }

    if (bits == 16) {
        const int16_t s = int16_t(
            uint16_t(p[0]) | (uint16_t(p[1]) << 8));
        return float(s) / 32768.0f;
    }

    if (bits == 24) {
        int32_t s = int32_t(p[0]) |
                    (int32_t(p[1]) << 8) |
                    (int32_t(p[2]) << 16);
        if (s & 0x00800000)
            s |= ~0x00FFFFFF;
        return float(s) / 8388608.0f;
    }

    if (bits == 32) {
        const int32_t s = int32_t(p[0]) |
                          (int32_t(p[1]) << 8) |
                          (int32_t(p[2]) << 16) |
                          (int32_t(p[3]) << 24);
        return float(s) / 2147483648.0f;
    }

    return 0.0f;
}

bool loadWav(
    const std::string& path,
    WavData& out,
    std::string& error,
    double startSeconds,
    double durationSeconds)
{
    if (startSeconds < 0.0)
        startSeconds = 0.0;

    if (durationSeconds <= 0.0)
        durationSeconds = 1.0;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "No se pudo abrir: " + path;
        return false;
    }

    if (!readFourCC(f, "RIFF")) {
        error = "No es WAV RIFF: " + path;
        return false;
    }

    (void)readU32(f);

    if (!readFourCC(f, "WAVE")) {
        error = "No es WAVE: " + path;
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint32_t dataSize = 0;
    std::streampos dataPos{};

    while (f && !haveData) {
        char id[4]{};
        f.read(id, 4);
        if (f.gcount() != 4)
            break;

        const uint32_t size = readU32(f);
        const std::streampos next =
            f.tellg() + std::streamoff(size + (size & 1));

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

    if (!haveFmt || !haveData) {
        error = "WAV sin fmt/data: " + path;
        return false;
    }

    if (out.channels < 1) {
        error = "WAV sin canales: " + path;
        return false;
    }

    if (!((out.format == 1 &&
           (out.bitsPerSample == 16 ||
            out.bitsPerSample == 24 ||
            out.bitsPerSample == 32)) ||
          (out.format == 3 && out.bitsPerSample == 32))) {
        error =
            "Formato WAV no soportado (PCM 16/24/32 o float32): " + path;
        return false;
    }

    const std::size_t bytesPerSample =
        static_cast<std::size_t>(out.bitsPerSample / 8);
    const std::size_t frameBytes =
        bytesPerSample * static_cast<std::size_t>(out.channels);

    if (frameBytes == 0) {
        error = "Frame WAV inválido: " + path;
        return false;
    }

    const std::size_t frames = dataSize / frameBytes;
    const std::size_t requestedStart =
        static_cast<std::size_t>(
            startSeconds * static_cast<double>(out.sampleRate));

    if (requestedStart >= frames) {
        error = "Inicio fuera del archivo: " + path;
        return false;
    }

    const std::size_t requestedFrames =
        static_cast<std::size_t>(
            durationSeconds * static_cast<double>(out.sampleRate));

    const std::size_t maxFrames =
        std::min<std::size_t>(
            frames - requestedStart,
            std::max<std::size_t>(1, requestedFrames));

    const std::size_t bytesToRead = maxFrames * frameBytes;
    std::vector<uint8_t> raw(bytesToRead);

    f.clear();
    f.seekg(dataPos +
            std::streamoff(requestedStart * frameBytes));

    f.read(
        reinterpret_cast<char*>(raw.data()),
        static_cast<std::streamsize>(bytesToRead));

    const std::size_t bytesRead =
        static_cast<std::size_t>(
            std::max<std::streamsize>(0, f.gcount()));

    const std::size_t actualFrames =
        bytesRead / frameBytes;

    if (actualFrames == 0) {
        error = "No se pudieron leer frames: " + path;
        return false;
    }

    out.mono.resize(actualFrames);
    out.interleaved.resize(
        actualFrames * static_cast<std::size_t>(out.channels));

    for (std::size_t i = 0; i < actualFrames; ++i) {
        const uint8_t* frame =
            raw.data() + i * frameBytes;

        double sum = 0.0;
        for (int channel = 0; channel < out.channels; ++channel) {
            const float sample = pcmSample(
                frame + channel * bytesPerSample,
                out.bitsPerSample,
                out.format);

            out.interleaved[
                i * static_cast<std::size_t>(out.channels) +
                static_cast<std::size_t>(channel)] = sample;

            sum += sample;
        }

        out.mono[i] =
            static_cast<float>(sum / out.channels);
    }

    return true;
}

bool parseDouble(
    const char* text,
    const char* name,
    double& value,
    std::string& error)
{
    try {
        value = std::stod(text);
        if (!std::isfinite(value)) {
            error = std::string("Valor no finito para ") + name;
            return false;
        }
        return true;
    } catch (...) {
        error =
            std::string("Valor inválido para ") +
            name + ": " + text;
        return false;
    }
}

std::vector<float> resampleToProjectTime(
    const std::vector<float>& native,
    double playbackRate,
    std::size_t outputFrames)
{
    std::vector<float> out(outputFrames, 0.0f);
    if (native.empty() || playbackRate <= 0.0)
        return out;

    for (std::size_t i = 0; i < outputFrames; ++i) {
        const double srcIndex =
            static_cast<double>(i) * playbackRate;

        if (srcIndex < 0.0 ||
            srcIndex >= static_cast<double>(native.size()))
            break;

        const std::size_t i0 =
            static_cast<std::size_t>(std::floor(srcIndex));

        const std::size_t i1 =
            std::min(i0 + 1, native.size() - 1);

        const double frac =
            srcIndex - static_cast<double>(i0);

        out[i] = static_cast<float>(
            (1.0 - frac) * native[i0] +
            frac * native[i1]);
    }

    return out;
}


double delayAtCurveCubic(
    const std::vector<sap::Point>& curve,
    double timeSec)
{
    if (curve.empty())
        return 0.0;

    if (curve.size() == 1)
        return curve.front().delaySamples;

    if (timeSec <= curve.front().timeSec)
        return curve.front().delaySamples;

    if (timeSec >= curve.back().timeSec)
        return curve.back().delaySamples;

    std::size_t segment = 0;
    for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
        if (timeSec >= curve[i].timeSec &&
            timeSec <= curve[i + 1].timeSec) {
            segment = i;
            break;
        }
    }

    const auto& a = curve[segment];
    const auto& b = curve[segment + 1];
    const double dt =
        std::max(1.0e-9, b.timeSec - a.timeSec);
    const double u =
        std::clamp(
            (timeSec - a.timeSec) / dt,
            0.0,
            1.0);

    double mA = 0.0;
    if (segment == 0) {
        mA = (b.delaySamples - a.delaySamples) / dt;
    } else {
        const auto& p = curve[segment - 1];
        mA = (b.delaySamples - p.delaySamples) /
             std::max(
                 1.0e-9,
                 b.timeSec - p.timeSec);
    }

    double mB = 0.0;
    if (segment + 1 == curve.size() - 1) {
        mB = (b.delaySamples - a.delaySamples) / dt;
    } else {
        const auto& n = curve[segment + 2];
        mB = (n.delaySamples - a.delaySamples) /
             std::max(
                 1.0e-9,
                 n.timeSec - a.timeSec);
    }

    const double u2 = u * u;
    const double u3 = u2 * u;

    const double h00 =  2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 =       u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 =       u3 -       u2;

    const double value =
        h00 * a.delaySamples +
        h10 * dt * mA +
        h01 * b.delaySamples +
        h11 * dt * mB;

    const double lo =
        std::min(a.delaySamples, b.delaySamples);
    const double hi =
        std::max(a.delaySamples, b.delaySamples);

    return std::clamp(value, lo, hi);
}

double sinc(double x)
{
    if (std::abs(x) < 1.0e-12)
        return 1.0;

    const double px = kPi * x;
    return std::sin(px) / px;
}

float sampleSincInterleaved(
    const std::vector<float>& data,
    std::size_t channels,
    double framePos,
    std::size_t channel)
{
    if (data.empty() ||
        channels == 0 ||
        channel >= channels ||
        framePos < 0.0)
        return 0.0f;

    const double cutoff = 0.96;
    constexpr int radius = 16;

    const long long center =
        static_cast<long long>(std::floor(framePos));

    double sum = 0.0;
    double weightSum = 0.0;

    for (int k = -radius + 1; k <= radius; ++k) {
        const long long index =
            center + static_cast<long long>(k);

        const std::size_t frameCount =
            data.size() / channels;

        if (index < 0 ||
            static_cast<std::size_t>(index) >= frameCount)
            continue;

        const double distance =
            static_cast<double>(index) - framePos;

        const double windowArg =
            std::abs(distance) /
            static_cast<double>(radius);

        if (windowArg >= 1.0)
            continue;

        // Hann-windowed low-pass kernel. The source and destination sample
        // rates are normally identical; the small 0.96 cutoff provides a
        // little headroom around Nyquist while keeping transients intact.
        const double window =
            0.5 + 0.5 *
            std::cos(kPi * windowArg);

        const double w =
            cutoff *
            sinc(cutoff * distance) *
            window;

        sum +=
            static_cast<double>(
                data[
                    static_cast<std::size_t>(index) *
                    channels +
                    channel]) * w;

        weightSum += w;
    }

    if (std::abs(weightSum) < 1.0e-12)
        return 0.0f;

    const double value = sum / weightSum;
    return static_cast<float>(
        std::clamp(value, -1.0, 1.0));
}

bool writeFloatWav(
    const std::string& path,
    const std::vector<float>& interleaved,
    int sampleRate,
    int channels,
    std::string& error)
{
    if (sampleRate <= 0 ||
        channels <= 0 ||
        interleaved.empty() ||
        interleaved.size() %
            static_cast<std::size_t>(channels) != 0) {
        error = "Datos inválidos para WAV de salida.";
        return false;
    }

    const uint64_t dataBytes64 =
        static_cast<uint64_t>(
            interleaved.size()) *
        sizeof(float);

    if (dataBytes64 > 0xffffffffULL - 36ULL) {
        error = "WAV de salida demasiado grande.";
        return false;
    }

    const uint32_t dataBytes =
        static_cast<uint32_t>(dataBytes64);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "No se pudo crear WAV de salida: " + path;
        return false;
    }

    f.write("RIFF", 4);
    writeU32(f, 36u + dataBytes);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    writeU32(f, 16u);
    writeU16(f, 3u); // IEEE float
    writeU16(
        f,
        static_cast<uint16_t>(channels));

    writeU32(
        f,
        static_cast<uint32_t>(sampleRate));

    writeU32(
        f,
        static_cast<uint32_t>(
            sampleRate *
            channels *
            static_cast<int>(sizeof(float))));

    writeU16(
        f,
        static_cast<uint16_t>(
            channels * static_cast<int>(sizeof(float))));

    writeU16(f, 32u);

    f.write("data", 4);
    writeU32(f, dataBytes);
    f.write(
        reinterpret_cast<const char*>(
            interleaved.data()),
        static_cast<std::streamsize>(dataBytes));

    if (!f) {
        error = "Error escribiendo WAV de salida: " + path;
        return false;
    }

    return true;
}

bool renderDynamicCorrection(
    const WavData& source,
    double sourceStart,
    double sourceRate,
    double duration,
    double sampleRate,
    const std::vector<sap::Point>& curve,
    std::vector<float>& correctedInterleaved,
    std::vector<float>& correctedMono,
    std::string& error)
{
    if (curve.size() < 2) {
        error = "Curva DYNAMIC insuficiente para renderizar.";
        return false;
    }

    if (source.sampleRate !=
        static_cast<int>(std::llround(sampleRate))) {
        error = "Sample rate SOURCE/PROJECT incompatible.";
        return false;
    }

    const std::size_t channels =
        static_cast<std::size_t>(
            std::max(1, source.channels));

    const std::size_t outputFrames =
        static_cast<std::size_t>(
            std::floor(duration * sampleRate));

    if (outputFrames < 2048) {
        error = "Tramo demasiado corto para DYNAMIC_RENDER.";
        return false;
    }

    correctedInterleaved.resize(
        outputFrames * channels);

    correctedMono.resize(outputFrames);

    for (std::size_t i = 0; i < outputFrames; ++i) {
        const double t =
            static_cast<double>(i) / sampleRate;

        const double delaySamples =
            delayAtCurveCubic(curve, t);

        // SOURCE is sampled directly at project time + measured delay.
        // This is the inverse of the delay that was measured by GCC-PHAT:
        // if SOURCE(t + D) = MASTER(t), corrected(t) = SOURCE(t + D).
        const double sourceTime =
            t +
            delaySamples / sampleRate;

        const double nativeFramePos =
            (sourceStart +
             sourceTime * sourceRate) *
            static_cast<double>(source.sampleRate);

        double mono = 0.0;

        for (std::size_t channel = 0;
             channel < channels;
             ++channel) {
            const float value =
                sampleSincInterleaved(
                    source.interleaved,
                    channels,
                    nativeFramePos,
                    channel);

            correctedInterleaved[
                i * channels + channel] =
                value;

            mono += value;
        }

        correctedMono[i] =
            static_cast<float>(
                mono /
                static_cast<double>(channels));
    }

    return true;
}

const char* modeName(sap::Mode mode)
{
    switch (mode) {
    case sap::Mode::Dynamic: return "DYNAMIC";
    case sap::Mode::Auto:    return "AUTO";
    default:                 return "STATIC";
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 9 && argc != 10) {
        std::cout
            << "ERROR=Uso: SmartAlignPostPrototype.exe MASTER.wav SOURCE.wav "
               "[MASTER_START SOURCE_START DURATION MASTER_RATE SOURCE_RATE MODE [OUTPUT_WAV]]\n";
        return 2;
    }

    std::string error;
    double masterStart = 0.0;
    double sourceStart = 0.0;
    double duration = 60.0;
    double masterRate = 1.0;
    double sourceRate = 1.0;
    sap::Mode requestedMode = sap::Mode::Auto;
    bool renderDynamic = false;
    std::string outputWav;

    if (argc == 9 || argc == 10) {
        if (!parseDouble(argv[3], "MASTER_START_SEC", masterStart, error) ||
            !parseDouble(argv[4], "SOURCE_START_SEC", sourceStart, error) ||
            !parseDouble(argv[5], "DURATION_SEC", duration, error) ||
            !parseDouble(argv[6], "MASTER_PLAYRATE", masterRate, error) ||
            !parseDouble(argv[7], "SOURCE_PLAYRATE", sourceRate, error)) {
            std::cout << "ERROR=" << error << "\n";
            return 6;
        }

        if (masterStart < 0.0 || sourceStart < 0.0 ||
            duration < 0.5 || duration > 600.0 ||
            masterRate <= 0.0 || sourceRate <= 0.0) {
            std::cout << "ERROR=Parámetros de tiempo/playrate fuera de rango.\n";
            return 6;
        }

        const std::string mode = argv[8];

        if (mode == "DYNAMIC_RENDER") {
            if (argc != 10) {
                std::cout
                    << "ERROR=DYNAMIC_RENDER requiere OUTPUT_WAV.\n";
                return 6;
            }

            requestedMode = sap::Mode::Dynamic;
            renderDynamic = true;
            outputWav = argv[9];
        }
        else {
            if (argc != 9) {
                std::cout
                    << "ERROR=OUTPUT_WAV solo es válido con DYNAMIC_RENDER.\n";
                return 6;
            }

            if (mode == "STATIC")
                requestedMode = sap::Mode::Static;
            else if (mode == "DYNAMIC")
                requestedMode = sap::Mode::Dynamic;
            else if (mode == "AUTO")
                requestedMode = sap::Mode::Auto;
            else {
                std::cout
                    << "ERROR=MODE debe ser STATIC, DYNAMIC, AUTO o DYNAMIC_RENDER.\n";
                return 6;
            }
        }
    }

    const auto totalStart = std::chrono::steady_clock::now();

    WavData master;
    WavData source;

    const auto loadStart =
        std::chrono::steady_clock::now();

    const double masterReadDuration =
        duration * masterRate;

    const double sourceReadDuration =
        duration * sourceRate;

    if (!loadWav(
            argv[1],
            master,
            error,
            masterStart,
            masterReadDuration)) {
        std::cout << "ERROR=" << error << "\n";
        return 3;
    }

    if (!loadWav(
            argv[2],
            source,
            error,
            sourceStart,
            sourceReadDuration)) {
        std::cout << "ERROR=" << error << "\n";
        return 3;
    }

    const auto loadEnd =
        std::chrono::steady_clock::now();

    if (master.sampleRate != source.sampleRate) {
        std::cout
            << "ERROR=sample rates distintos MASTER="
            << master.sampleRate
            << " SOURCE="
            << source.sampleRate << "\n";
        return 4;
    }

    const std::size_t projectFrames =
        static_cast<std::size_t>(
            std::floor(duration * master.sampleRate));

    const auto masterProject =
        resampleToProjectTime(
            master.mono,
            masterRate,
            projectFrames);

    const auto sourceProject =
        resampleToProjectTime(
            source.mono,
            sourceRate,
            projectFrames);

    const std::size_t n =
        std::min(
            masterProject.size(),
            sourceProject.size());

    if (n < 2048) {
        std::cout
            << "ERROR=Tramo demasiado corto para análisis de fase.\n";
        return 5;
    }

    sap::Settings settings;
    settings.sampleRate =
        static_cast<double>(master.sampleRate);

    settings.maxDelayMs = 40.0;
    settings.analysisWindowMs = 60.0;
    settings.hopMs = 250.0;
    settings.minConfidence = 0.72;
    settings.maxSlewMsPerSecond = 100.0;
    settings.energyGateRatio = 0.35;
    settings.anchorSeparationMs = 180.0;
    settings.staticAnchorCount = 8;
    settings.maxDynamicAnchors = 240;
    settings.playbackRateRatio =
        sourceRate / std::max(1.0e-12, masterRate);
    settings.hasInitialDelaySamples = false;
    settings.initialDelaySamples = 0.0;

    // When AUTO is used with different item playback rates, the resulting
    // project-time relationship already contains a deterministic temporal
    // drift. Do not discard that known evidence merely because the highest-
    // energy acoustic anchors happen to cluster in one part of the take.
    const double rateRatio =
        sourceRate / std::max(1.0e-12, masterRate);
    const bool knownRateDrift =
        std::abs(rateRatio - 1.0) > 1.0e-6;

    const sap::Mode effectiveMode =
        (requestedMode == sap::Mode::Auto && knownRateDrift)
            ? sap::Mode::Dynamic
            : requestedMode;

    settings.mode = effectiveMode;

    const auto analyzeStart =
        std::chrono::steady_clock::now();

    const auto result =
        sap::AlignEngine::analyze(
            masterProject,
            sourceProject,
            settings);

    const auto analyzeEnd =
        std::chrono::steady_clock::now();

    const double sr =
        settings.sampleRate;

    const double loadMs =
        std::chrono::duration<double, std::milli>(
            loadEnd - loadStart).count();

    const double analyzeMs =
        std::chrono::duration<double, std::milli>(
            analyzeEnd - analyzeStart).count();


    if (renderDynamic) {
        if (result.modeUsed != sap::Mode::Dynamic ||
            result.curve.size() < 2) {
            std::cout
                << "ERROR=DYNAMIC_RENDER no obtuvo una curva DYNAMIC válida.\\n";
            return 11;
        }

        constexpr double kRenderPadSeconds = 0.080;

        const double renderStart =
            std::max(
                0.0,
                sourceStart -
                kRenderPadSeconds * sourceRate);

        const double renderSourceStartNative =
            sourceStart - renderStart;

        const double renderPreProjectSeconds =
            renderSourceStartNative /
            std::max(1.0e-12, sourceRate);

        WavData sourceRender;

        const double renderDurationNative =
            renderPreProjectSeconds +
            duration +
            kRenderPadSeconds;

        if (!loadWav(
                argv[2],
                sourceRender,
                error,
                renderStart,
                renderDurationNative * sourceRate)) {
            std::cout << "ERROR=" << error << "\n";
            return 11;
        }

        // SOURCE is loaded from renderStart. Keep the item-start offset
        // in native SOURCE time (not project time), because the renderer
        // advances through native SOURCE frames using sourceRate.
        std::vector<sap::Point> shiftedCurve =
            result.curve;

        for (auto& point : shiftedCurve) {
            point.timeSec =
                point.timeSec;
        }

        std::vector<float> correctedInterleaved;
        std::vector<float> correctedMono;

        if (!renderDynamicCorrection(
                sourceRender,
                renderSourceStartNative,
                sourceRate,
                duration,
                settings.sampleRate,
                shiftedCurve,
                correctedInterleaved,
                correctedMono,
                error)) {
            std::cout << "ERROR=" << error << "\n";
            return 11;
        }

        WavData correctedForValidation;
        correctedForValidation.sampleRate =
            static_cast<int>(std::llround(settings.sampleRate));
        correctedForValidation.channels =
            sourceRender.channels;
        correctedForValidation.interleaved =
            correctedInterleaved;
        correctedForValidation.mono =
            correctedMono;

        sap::Settings postSettings = settings;
        postSettings.mode = sap::Mode::Auto;
        postSettings.playbackRateRatio = 1.0;
        postSettings.hasInitialDelaySamples = false;

        const auto post =
            sap::AlignEngine::analyze(
                masterProject,
                correctedMono,
                postSettings);

        const sap::Settings postStaticSettings =
            [&]() {
                sap::Settings s = postSettings;
                s.mode = sap::Mode::Static;
                return s;
            }();

        const auto postStatic =
            sap::AlignEngine::analyze(
                masterProject,
                correctedMono,
                postStaticSettings);

        const double residualSamples =
            postStatic.staticDelaySamples;

        const bool postValid =
            std::isfinite(residualSamples) &&
            std::abs(residualSamples) <= 2.0 &&
            post.modeUsed != sap::Mode::Dynamic;

        std::cout
            << "POST_MODE_USED="
            << modeName(post.modeUsed)
            << "\n";
        std::cout
            << "POST_CURVE_COUNT="
            << post.curve.size()
            << "\n";
        std::cout
            << "POST_DELAY_SAMPLES="
            << residualSamples
            << "\n";
        std::cout
            << "POST_DELAY_MS="
            << residualSamples * 1000.0 / settings.sampleRate
            << "\n";
        std::cout
            << "POST_CONFIDENCE="
            << postStatic.staticConfidence
            << "\n";

        if (!postValid) {
            std::cout
                << "POST_VALID=0\n";
            return 12;
        }

        if (!writeFloatWav(
                outputWav,
                correctedInterleaved,
                sourceRender.sampleRate,
                sourceRender.channels,
                error)) {
            std::cout << "ERROR=" << error << "\n";
            return 11;
        }

        std::cout
            << "OUTPUT_WAV="
            << outputWav
            << "\n";
        std::cout
            << "POST_VALID=1\n";
    }

    const double totalMs =
        std::chrono::duration<double, std::milli>(
            analyzeEnd - totalStart).count();

    constexpr const char* kEngineVersion = "20260920-dynamic-render-1";

    std::cout << "ENGINE_VERSION="
              << kEngineVersion << "\n";
    std::cout << "MODE_REQUESTED="
              << modeName(requestedMode) << "\n";
    std::cout << "MODE_EFFECTIVE="
              << modeName(effectiveMode) << "\n";
    std::cout << "RATE_RATIO="
              << rateRatio << "\n";
    std::cout << "MODE_USED="
              << modeName(result.modeUsed) << "\n";
    std::cout << "LOAD_MS="
              << loadMs << "\n";
    std::cout << "ANALYZE_MS="
              << analyzeMs << "\n";
    std::cout << "TOTAL_MS="
              << totalMs << "\n";
    std::cout << "DELAY_MS="
              << result.staticDelaySamples * 1000.0 / sr << "\n";
    std::cout << "DELAY_SAMPLES="
              << result.staticDelaySamples << "\n";
    std::cout << "CONFIDENCE="
              << result.staticConfidence << "\n";
    std::cout << "CORRELATION="
              << result.staticCorrelation << "\n";
    std::cout << "SUPPORT_WINDOWS="
              << result.staticSupportWindows << "\n";
    std::cout << "TOTAL_WINDOWS="
              << result.staticTotalWindows << "\n";
    std::cout << "CURVE_COUNT="
              << result.curve.size() << "\n";
    std::cout << "SCOUT_POINTS="
              << result.scoutPoints << "\n";
    std::cout << "SCOUT_FIRST_MS="
              << result.scoutFirstDelaySamples * 1000.0 / sr << "\n";
    std::cout << "SCOUT_LAST_MS="
              << result.scoutLastDelaySamples * 1000.0 / sr << "\n";
    std::cout << "SCOUT_R2="
              << result.scoutR2 << "\n";
    std::cout << "SCOUT_DIRECTION="
              << result.scoutDirectionConsistency << "\n";
    std::cout << "SCOUT_ROBUST_SHIFT_MS="
              << result.scoutRobustShiftSamples * 1000.0 / sr << "\n";
    std::cout << "SCOUT_COHERENT="
              << (result.scoutCoherent ? 1 : 0) << "\n";

    for (const auto& p : result.curve) {
        const double delayMs =
            p.delaySamples * 1000.0 / sr;

        const double phatMs =
            p.phatDelaySamples * 1000.0 / sr;

        const double waveformMs =
            p.waveformDelaySamples * 1000.0 / sr;

        std::cout
            << "POINT="
            << p.timeSec << ","
            << delayMs << ","
            << p.delaySamples << ","
            << p.confidence << ","
            << (p.keyPoint ? 1 : 0) << ","
            << phatMs << ","
            << waveformMs << ","
            << p.phaseAgreement
            << "\n";
    }

    return 0;
}
