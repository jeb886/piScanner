#include "DecodeFiles.h"

#include "Config.h"
#include "Decoder.h"
#include "ImageFile.h"
#include "Output.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string trimOneNewline(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    return text;
}

} // namespace

int runDecodeFiles(const Config& config, std::ostream& output)
{
    const auto paths = expandImagePaths(config.decodeInputs);
    if (paths.empty())
        throw std::runtime_error("no image files found");

    Decoder decoder(config);
    int failures = 0;
    int checked = 0;

    for (const auto& path : paths) {
        GrayImage image;
        try {
            image = loadImage(path);
        } catch (const std::exception& exception) {
            output << path << ": load error: " << exception.what() << '\n';
            ++failures;
            continue;
        }
        Frame frame{image.pixels.data(), image.pixels.size(), image.width, image.height, image.width,
                    PixelFormat::GREY,        monotonicNowUs(),   0};
        std::vector<ScanResult> results;
        try {
            results = decoder.decode(frame);
        } catch (const std::exception& exception) {
            output << path << ": decode error: " << exception.what() << '\n';
            ++failures;
            continue;
        }

        const std::string found = results.empty() ? "" : results.front().text;
        const std::string format = results.empty() ? "" : results.front().format;

        if (!config.expect) {
            if (results.empty())
                output << path << ": no barcode\n";
            else
                output << path << ": " << format << ' ' << found << '\n';
            continue;
        }

        ++checked;
        const std::string sidecar = expectedTextPath(path);
        std::ifstream in(sidecar);
        if (!in) {
            output << path << ": FAIL missing sidecar " << sidecar << '\n';
            ++failures;
            continue;
        }
        const std::string expected = trimOneNewline(
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        if (expected == "NONE") {
            if (results.empty()) {
                output << path << ": PASS no barcode\n";
            } else {
                output << path << ": FAIL expected no barcode, got " << format << ' ' << found << '\n';
                ++failures;
            }
        } else if (found == expected) {
            output << path << ": PASS " << format << ' ' << found << '\n';
        } else {
            output << path << ": FAIL expected \"" << expected << "\", got "
                   << (results.empty() ? "no barcode" : "\"" + found + "\" (" + format + ")") << '\n';
            ++failures;
        }
    }

    if (config.expect) {
        output << "decode --expect: " << (checked - failures) << '/' << checked << " passed\n";
        return failures == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
