#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <cstdio>
#else
  #include <cstdio>
  #include <sys/wait.h>
#endif

static std::vector<float> makeSignal(size_t n)
{
    std::vector<float> x(n);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < n; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        x[i] = static_cast<float>((state / 4294967295.0) * 2.0 - 1.0);
    }
    return x;
}

static std::vector<float> delayed(const std::vector<float>& x, int samples)
{
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = static_cast<size_t>(samples); i < x.size(); ++i)
        y[i] = x[i - static_cast<size_t>(samples)];
    return y;
}

static void writeWav16(const std::filesystem::path& path,
                       const std::vector<float>& samples,
                       uint32_t sampleRate)
{
    const uint16_t audioFormat = 1;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byteRate = sampleRate * channels * bits / 8;
    const uint16_t blockAlign = channels * bits / 8;
    const uint32_t dataBytes =
        static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataBytes;

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot create WAV: " + path.string());

    auto u16 = [&](uint16_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto u32 = [&](uint32_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
        f.put(static_cast<char>((v >> 16) & 0xFF));
        f.put(static_cast<char>((v >> 24) & 0xFF));
    };

    f.write("RIFF", 4); u32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(audioFormat); u16(channels);
    u32(sampleRate); u32(byteRate); u16(blockAlign); u16(bits);
    f.write("data", 4); u32(dataBytes);

    for (float v : samples) {
        const float clamped = std::max(-1.0f, std::min(1.0f, v));
        const int16_t s = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        u16(static_cast<uint16_t>(s));
    }
}

static std::string shellQuote(const std::string& s)
{
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    return "'" + s + "'";
#endif
}

static bool runPrototype(const std::string& exe,
                         const std::string& master,
                         const std::string& source,
                         std::string& output,
                         int& exitCode)
{
#ifdef _WIN32
    // Do not use _popen/cmd.exe on Windows here. It introduces shell parsing
    // and can reinterpret the generated command line. The smoke test needs to
    // exercise the prototype executable directly.
    const auto outPath =
        std::filesystem::temp_directory_path() / "smartalign_post_smoke_cli.txt";

    HANDLE outHandle = CreateFileA(
        outPath.string().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (outHandle == INVALID_HANDLE_VALUE)
        return false;

    SetHandleInformation(outHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::string cmd =
        shellQuote(exe) + " " +
        shellQuote(master) + " " +
        shellQuote(source) +
        " 0 0 24.0";

    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outHandle;
    si.hStdError = outHandle;

    const BOOL created = CreateProcessA(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(outHandle);

    if (!created) {
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD processCode = 1;
    GetExitCodeProcess(pi.hProcess, &processCode);
    exitCode = static_cast<int>(processCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::ifstream f(outPath, std::ios::binary);
    output.assign((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());

    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    return true;
#else
    const std::string command =
        shellQuote(exe) + " " +
        shellQuote(master) + " " +
        shellQuote(source) +
        " 0 0 24.0";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
        return false;

    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe))
        output += buffer;

    const int status = pclose(pipe);
    exitCode = (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    return true;
#endif
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: smartalign_prototype_tests <SmartAlignPostPrototype>\n";
        return 1;
    }

    try {
        constexpr uint32_t sampleRate = 48000;
        constexpr double durationSec = 24.0;
        constexpr int knownDelay = 56;
        const size_t n = static_cast<size_t>(sampleRate * durationSec);

        const auto master = makeSignal(n);
        const auto source = delayed(master, knownDelay);

        const auto base =
            std::filesystem::temp_directory_path() / "smartalign_post_smoke";
        std::filesystem::create_directories(base);

        const auto masterPath = base / "master.wav";
        const auto sourcePath = base / "source.wav";
        writeWav16(masterPath, master, sampleRate);
        writeWav16(sourcePath, source, sampleRate);

        std::string output;
        int status = 1;
        if (!runPrototype(argv[1], masterPath.string(), sourcePath.string(), output, status))
            throw std::runtime_error("No se pudo iniciar SmartAlignPostPrototype");

        const bool exitedOk = status == 0;
        if (!exitedOk) {
            std::cerr << "Prototype returned failure.\n" << output;
            return 2;
        }

        const auto findNumber = [&](const char* key) -> double {
            const std::string token = std::string(key) + "=";
            const size_t p = output.find(token);
            if (p == std::string::npos) throw std::runtime_error(std::string("Missing ") + key);
            const size_t start = p + token.size();
            return std::stod(output.substr(start));
        };

        const double delay = findNumber("DELAY_SAMPLES");
        const double confidence = findNumber("CONFIDENCE");
        const double loadMs = findNumber("LOAD_MS");
        const double analyzeMs = findNumber("ANALYZE_MS");
        const double totalMs = findNumber("TOTAL_MS");

        if (std::abs(delay - knownDelay) > 1.0) {
            std::cerr << "Prototype delay mismatch: got " << delay
                      << " expected " << knownDelay << "\n";
            return 3;
        }
        if (confidence < 0.80) {
            std::cerr << "Prototype confidence too low: " << confidence << "\n";
            return 4;
        }
        if (output.find("POINT=") == std::string::npos) {
            std::cerr << "Prototype produced no DYNAMIC points.\n";
            return 5;
        }
        // This is intentionally a generous CI guard. The test is designed to
        // catch the previous per-frame f.read() regression, not to benchmark
        // machine-to-machine performance.
        if (totalMs > 15000.0) {
            std::cerr << "Prototype smoke test too slow: total=" << totalMs
                      << " ms (load=" << loadMs << ", analyze=" << analyzeMs << ")\n";
            return 6;
        }

        std::cout << "PROTOTYPE_SMOKE delay=" << delay
                  << " confidence=" << confidence
                  << " load_ms=" << loadMs
                  << " analyze_ms=" << analyzeMs
                  << " total_ms=" << totalMs << "\n";

        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 10;
    }
}
