#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/wait.h>
#endif

namespace {

void writeU16(std::ofstream& f, uint16_t v)
{
    f.put(static_cast<char>(v & 0xff));
    f.put(static_cast<char>((v >> 8) & 0xff));
}

void writeU32(std::ofstream& f, uint32_t v)
{
    f.put(static_cast<char>(v & 0xff));
    f.put(static_cast<char>((v >> 8) & 0xff));
    f.put(static_cast<char>((v >> 16) & 0xff));
    f.put(static_cast<char>((v >> 24) & 0xff));
}

void writeFloatWav(
    const std::filesystem::path& path,
    const std::vector<float>& data,
    int sampleRate)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot create wav");

    const uint32_t dataBytes =
        static_cast<uint32_t>(data.size() * sizeof(float));

    f.write("RIFF", 4);
    writeU32(f, 36u + dataBytes);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    writeU32(f, 16);
    writeU16(f, 3);
    writeU16(f, 1);
    writeU32(f, static_cast<uint32_t>(sampleRate));
    writeU32(f, static_cast<uint32_t>(sampleRate * sizeof(float)));
    writeU16(f, sizeof(float));
    writeU16(f, 32);

    f.write("data", 4);
    writeU32(f, dataBytes);
    f.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(dataBytes));
}

double lagrange4(
    const std::vector<float>& x,
    double pos)
{
    if (pos < 1.0 ||
        pos >= static_cast<double>(x.size() - 2))
        return 0.0;

    const std::size_t i =
        static_cast<std::size_t>(std::floor(pos));
    const double f =
        pos - static_cast<double>(i);

    const double c0 = -f * (f - 1.0) * (f - 2.0) / 6.0;
    const double c1 =  (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
    const double c2 = -(f + 1.0) * f * (f - 2.0) / 2.0;
    const double c3 =  (f + 1.0) * f * (f - 1.0) / 6.0;

    return c0 * x[i - 1] +
           c1 * x[i] +
           c2 * x[i + 1] +
           c3 * x[i + 2];
}

std::vector<float> makeSignal(std::size_t n)
{
    std::vector<float> x(n);
    uint32_t state = 0x18473921u;

    auto rnd = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0;
    };

    double slow = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        slow = 0.998 * slow + 0.002 * rnd();
        x[i] = static_cast<float>(0.82 * rnd() + 0.18 * slow);
    }
    return x;
}

std::vector<float> delaySignal(
    const std::vector<float>& x,
    double delay)
{
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t i = 3; i + 2 < x.size(); ++i)
        y[i] = static_cast<float>(
            lagrange4(x, static_cast<double>(i) - delay));
    return y;
}

std::vector<float> varyingDelaySignal(
    const std::vector<float>& x,
    double startDelay,
    double endDelay)
{
    std::vector<float> y(x.size(), 0.0f);

    const double denom =
        static_cast<double>(
            std::max<std::size_t>(1, x.size() - 1));

    for (std::size_t i = 3; i + 2 < x.size(); ++i) {
        const double u =
            static_cast<double>(i) / denom;

        const double delay =
            startDelay +
            (endDelay - startDelay) * u;

        y[i] = static_cast<float>(
            lagrange4(
                x,
                static_cast<double>(i) - delay));
    }

    return y;
}

std::string shellQuote(const std::string& s)
{
#ifdef _WIN32
    std::string q = "\"";
    for (char c : s) {
        if (c == '"')
            q += "\\\"";
        else
            q += c;
    }
    q += "\"";
    return q;
#else
    std::string q = "'";
    for (char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    q += "'";
    return q;
#endif
}

bool runCommand(
    const std::string& exe,
    const std::string& commandArgs,
    std::string& output,
    int& exitCode)
{
#ifdef _WIN32
    const auto outPath =
        std::filesystem::temp_directory_path() /
        "smartalign_phase_core_cli.txt";

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

    SetHandleInformation(
        outHandle,
        HANDLE_FLAG_INHERIT,
        HANDLE_FLAG_INHERIT);

    std::string commandLine =
        "\"" + exe + "\" " + commandArgs;

    std::vector<char> mutableCommandLine(
        commandLine.begin(),
        commandLine.end());
    mutableCommandLine.push_back('\0');

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outHandle;
    si.hStdError = outHandle;

    const BOOL created = CreateProcessA(
        nullptr,
        mutableCommandLine.data(),
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
    GetExitCodeProcess(
        pi.hProcess,
        &processCode);
    exitCode =
        static_cast<int>(processCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::ifstream in(
        outPath,
        std::ios::binary);

    output.assign(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    return true;
#else
    const std::string command =
        shellQuote(exe) + " " + commandArgs;

    FILE* pipe =
        popen(command.c_str(), "r");

    if (!pipe)
        return false;

    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe))
        output += buffer;

    const int status = pclose(pipe);
    exitCode =
        WIFEXITED(status)
            ? WEXITSTATUS(status)
            : 1;

    return true;
#endif
}

double field(
    const std::string& output,
    const std::string& key)
{
    const std::string prefix = key + "=";
    const std::size_t p = output.find(prefix);
    if (p == std::string::npos)
        throw std::runtime_error("missing " + key);

    const std::size_t start = p + prefix.size();
    const std::size_t end = output.find('\n', start);
    return std::stod(
        output.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start));
}

std::vector<double> pointDelays(
    const std::string& output)
{
    std::vector<double> result;
    std::istringstream in(output);
    std::string line;

    while (std::getline(in, line)) {
        if (line.rfind("POINT=", 0) != 0)
            continue;

        std::stringstream ss(line.substr(6));
        std::string item;
        std::vector<double> values;

        while (std::getline(ss, item, ','))
            values.push_back(std::stod(item));

        if (values.size() >= 3)
            result.push_back(values[2]);
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr
            << "Usage: smartalign_prototype_tests "
               "<SmartAlignPostPrototype>\n";
        return 2;
    }

    const auto base =
        std::filesystem::temp_directory_path() /
        "smartalign_phase_core_tests";

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base);

    constexpr int sr = 48000;
    constexpr double duration = 12.0;

    try {
        const std::size_t n =
            static_cast<std::size_t>(sr * duration);

        const auto master =
            makeSignal(n);

        const auto source =
            delaySignal(master, 173.35);

        const auto masterPath = base / "master.wav";
        const auto sourcePath = base / "source.wav";

        writeFloatWav(masterPath, master, sr);
        writeFloatWav(sourcePath, source, sr);

        // STATIC phase smoke test.
        {
            std::string output;
            int status = 1;

            const std::string args =
                shellQuote(masterPath.string()) + " " +
                shellQuote(sourcePath.string()) +
                " 0 0 12 1 1 STATIC";

            if (!runCommand(argv[1], args, output, status))
                throw std::runtime_error(
                    "could not launch prototype");

            if (status != 0) {
                std::cerr << output;
                return 3;
            }

            const double delay =
                field(output, "DELAY_SAMPLES");
            const double confidence =
                field(output, "CONFIDENCE");

            if (!std::isfinite(delay) ||
                std::abs(delay - 173.35) > 0.9) {
                std::cerr
                    << "static phase mismatch: "
                    << delay << "\n";
                return 4;
            }

            if (!std::isfinite(confidence) ||
                confidence < 0.72) {
                std::cerr
                    << "static phase confidence too low: "
                    << confidence << "\n";
                return 5;
            }
        }

        // D_PLAYRATE regression: the WAV itself remains unchanged while
        // REAPER plays SOURCE at 0.999. Project-time resampling must reveal
        // the accumulated temporal drift.
        {
            std::string output;
            int status = 1;

            const std::string args =
                shellQuote(masterPath.string()) + " " +
                shellQuote(sourcePath.string()) +
                " 0 0 12 1 0.999 AUTO";

            if (!runCommand(argv[1], args, output, status))
                throw std::runtime_error(
                    "could not launch rate-aware prototype");

            if (status != 0) {
                std::cerr << output;
                return 6;
            }

            const auto delays =
                pointDelays(output);

            if (delays.size() < 4 ||
                output.find("MODE_USED=DYNAMIC") ==
                    std::string::npos) {
                std::cerr
                    << "rate-aware run did not produce "
                       "a dynamic phase curve\n"
                    << output;
                return 7;
            }

            const double first = delays.front();
            const double last = delays.back();

            // 12 seconds * 0.001 = roughly 12 ms of accumulated drift.
            // The phase core may not place anchors exactly at project
            // boundaries, so require a substantial fraction.
            if (last - first < 6.0 * sr / 1000.0) {
                std::cerr
                    << "rate-aware drift too small: "
                    << first << " -> " << last << " samples\n";
                return 8;
            }

            std::cout
                << "PROTOTYPE_STATIC_PHASE delay="
                << first << " confidence="
                << field(output, "CONFIDENCE") << "\n"
                << "PROTOTYPE_RATE_AWARE first="
                << first
                << " last="
                << last
                << " drift="
                << (last - first)
                << " samples\n";
        }


        // DYNAMIC_RENDER must correct a known time-varying phase trajectory
        // and validate the result by re-analyzing the rendered WAV.
        {
            const auto dynamicSource =
                varyingDelaySignal(
                    master,
                    72.0,
                    228.0);

            const auto dynamicSourcePath =
                base / "dynamic_source.wav";
            const auto correctedPath =
                base / "dynamic_corrected.wav";

            writeFloatWav(
                dynamicSourcePath,
                dynamicSource,
                sr);

            std::string output;
            int status = 1;

            const std::string args =
                shellQuote(masterPath.string()) + " " +
                shellQuote(dynamicSourcePath.string()) +
                " 0 0 12 1 1 DYNAMIC_RENDER " +
                shellQuote(correctedPath.string());

            if (!runCommand(
                    argv[1],
                    args,
                    output,
                    status)) {
                throw std::runtime_error(
                    "could not launch dynamic renderer");
            }

            if (status != 0) {
                std::cerr << output;
                return 9;
            }

            if (output.find("MODE_USED=DYNAMIC") ==
                    std::string::npos ||
                output.find("POST_VALID=1") ==
                    std::string::npos) {
                std::cerr
                    << "dynamic render was not validated\n"
                    << output;
                return 10;
            }

            const double residual =
                field(
                    output,
                    "POST_DELAY_SAMPLES");

            if (!std::isfinite(residual) ||
                std::abs(residual) > 2.0) {
                std::cerr
                    << "dynamic render residual too large: "
                    << residual << " samples\n"
                    << output;
                return 11;
            }

            if (!std::filesystem::exists(
                    correctedPath)) {
                std::cerr
                    << "dynamic renderer did not create output WAV\n";
                return 12;
            }
        }

        // DYNAMIC_RENDER must also honor an externally supplied curve.
        // The REAPER PHASE BATCH layer passes the already analyzed scene
        // trajectory, preventing the renderer from silently re-analyzing a
        // different scene/overlap and producing a different correction.
        {
            const curvePath =
                base / "scene_curve.csv";

            {
                std::ofstream curve(curvePath);
                curve
                    << "0.000000000,72.000000000\n"
                    << "3.000000000,111.000000000\n"
                    << "6.000000000,150.000000000\n"
                    << "9.000000000,189.000000000\n"
                    << "12.000000000,228.000000000\n";
            }

            const auto dynamicSource =
                varyingDelaySignal(
                    master,
                    72.0,
                    228.0);

            const auto dynamicSourcePath =
                base / "curve_file_source.wav";
            const auto curveCorrectedPath =
                base / "curve_file_corrected.wav";

            writeFloatWav(
                dynamicSourcePath,
                dynamicSource,
                sr);

            std::string output;
            int status = 1;

            const std::string args =
                shellQuote(masterPath.string()) + " " +
                shellQuote(dynamicSourcePath.string()) +
                " 0 0 12 1 1 DYNAMIC_RENDER " +
                shellQuote(curveCorrectedPath) + " " +
                shellQuote(curvePath);

            if (!runCommand(
                    argv[1],
                    args,
                    output,
                    status)) {
                throw std::runtime_error(
                    "could not launch curve-file dynamic renderer");
            }

            if (status != 0 ||
                output.find("POST_VALID=1") ==
                    std::string::npos ||
                output.find("RENDER_CURVE_SOURCE=ANALYZED_JOB") ==
                    std::string::npos) {
                std::cerr
                    << "curve-file dynamic render was not validated\n"
                    << output;
                return 17;
            }

            const double residual =
                field(
                    output,
                    "POST_DELAY_SAMPLES");

            if (!std::isfinite(residual) ||
                std::abs(residual) > 2.0) {
                std::cerr
                    << "curve-file dynamic residual too large: "
                    << residual
                    << " samples\n"
                    << output;
                return 18;
            }

            if (!std::filesystem::exists(
                    curveCorrectedPath)) {
                std::cerr
                    << "curve-file dynamic renderer did not create output WAV\n";
                return 19;
            }
        }

        // DYNAMIC_RENDER regression with a non-1.0 SOURCE playrate.
        // The renderer must keep the padded-buffer offset in native SOURCE
        // seconds while advancing through the source using the project-time
        // playback rate.
        {
            constexpr double renderDuration = 10.0;
            constexpr double sourceRate = 1.0203;

            const auto rateDynamicSource =
                delaySignal(
                    master,
                    120.0);

            const auto rateDynamicSourcePath =
                base / "dynamic_rate_source.wav";
            const auto rateCorrectedPath =
                base / "dynamic_rate_corrected.wav";

            writeFloatWav(
                rateDynamicSourcePath,
                rateDynamicSource,
                sr);

            std::string output;
            int status = 1;

            const std::string args =
                shellQuote(masterPath.string()) + " " +
                shellQuote(rateDynamicSourcePath.string()) +
                " 0 0 10 1 " +
                std::to_string(sourceRate) +
                " DYNAMIC_RENDER " +
                shellQuote(rateCorrectedPath.string());

            if (!runCommand(
                    argv[1],
                    args,
                    output,
                    status)) {
                throw std::runtime_error(
                    "could not launch rate-aware dynamic renderer");
            }

            if (status != 0) {
                std::cerr << output;
                return 13;
            }

            if (output.find("MODE_USED=DYNAMIC") ==
                    std::string::npos ||
                output.find("POST_VALID=1") ==
                    std::string::npos) {
                std::cerr
                    << "rate-aware dynamic render was not validated\n"
                    << output;
                return 14;
            }

            const double residual =
                field(
                    output,
                    "POST_DELAY_SAMPLES");

            if (!std::isfinite(residual) ||
                std::abs(residual) > 2.0) {
                std::cerr
                    << "rate-aware dynamic residual too large: "
                    << residual << " samples\n"
                    << output;
                return 15;
            }

            if (!std::filesystem::exists(
                    rateCorrectedPath)) {
                std::cerr
                    << "rate-aware dynamic renderer did not create output WAV\n";
                return 16;
            }
        }

        std::filesystem::remove_all(base, ec);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(base, ec);
        return 10;
    }
}
