#include "Bench.h"

#include "Decoder.h"
#include "Output.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string readToken(std::istream& input)
{
    std::string token;
    for (;;) {
        int next = input.peek();
        if (next == EOF)
            throw std::runtime_error("truncated PGM header");
        if (std::isspace(static_cast<unsigned char>(next))) {
            input.get();
            continue;
        }
        if (next == '#') {
            std::string ignored;
            std::getline(input, ignored);
            continue;
        }
        break;
    }
    while (input.good()) {
        const int next = input.peek();
        if (next == EOF || std::isspace(static_cast<unsigned char>(next)))
            break;
        token.push_back(static_cast<char>(input.get()));
    }
    if (token.empty())
        throw std::runtime_error("invalid PGM header");
    const int delimiter = input.get();
    if (delimiter == '\r' && input.peek() == '\n')
        input.get();
    return token;
}

std::vector<std::uint8_t> readPgm(const std::string& path, int& width, int& height)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open PGM file " + path);
    if (readToken(input) != "P5")
        throw std::runtime_error("bench input must be a binary P5 PGM");
    try {
        width = std::stoi(readToken(input));
        height = std::stoi(readToken(input));
        if (readToken(input) != "255")
            throw std::runtime_error("PGM maxval must be 255");
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("invalid PGM dimensions or maxval");
    } catch (const std::out_of_range&) {
        throw std::runtime_error("PGM dimensions are out of range");
    }
    if (width <= 0 || height <= 0)
        throw std::runtime_error("PGM dimensions must be positive");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height);
    if (!input.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size())))
        throw std::runtime_error("truncated PGM pixel data");
    return pixels;
}

}

int runBench(const Config& config, std::ostream& output)
{
    int width = 0;
    int height = 0;
    auto pixels = readPgm(config.benchFile, width, height);
    Frame frame{pixels.data(), pixels.size(), width, height, width, PixelFormat::GREY, monotonicNowUs(), 0};
    Decoder decoder(config);
    std::vector<double> timings;
    std::vector<ScanResult> results;
    timings.reserve(static_cast<std::size_t>(config.iterations));
    for (int i = 0; i < config.iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        results = decoder.decode(frame);
        timings.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(timings.begin(), timings.end());
    double sum = 0;
    for (const double timing : timings)
        sum += timing;
    const auto percentile = [&](double fraction) {
        const auto index =
            std::min(timings.size() - 1, static_cast<std::size_t>(std::ceil(fraction * timings.size()) - 1));
        return timings[index];
    };
    output << "iterations=" << timings.size() << " mean_ms=" << sum / static_cast<double>(timings.size())
           << " p50_ms=" << percentile(0.50) << " p99_ms=" << percentile(0.99) << " min_ms=" << timings.front()
           << '\n';
    for (const auto& result : results)
        output << result.format << '\t' << result.text << '\n';
    return 0;
}
