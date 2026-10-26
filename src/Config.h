#pragma once

#include <optional>
#include <string>
#include <vector>

struct RegionOfInterest {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;
};

struct Config {
    std::string command;
    std::string device = "/dev/video0";
    int width = 1280;
    int height = 720;
    int fps = 30;
    std::string pixfmt = "auto";
    int buffers = 4;
    int threads = 2;
    std::string formats;
    bool tryHarder = false;
    bool tryRotate = true;
    bool tryDownscale = true;
    int maxSymbols = 1;
    std::string binarizer = "local";
    std::optional<RegionOfInterest> roi;
    std::string output = "text";
    bool stats = false;
    bool verbose = false;
    std::string trigger = "gpio";
    std::string gpioChip;
    int gpioLine = 17;
    std::string gpioBias = "pull-up";
    int debounceUs = 5000;
    bool activeLow = true;
    std::string triggerMode = "edge";
    int scanTimeoutMs = 2000;
    int dedupeMs = 1000;
    std::optional<int> exposure;
    std::optional<int> focus;
    std::optional<int> gain;
    std::string benchFile;
    int iterations = 100;
};

std::optional<Config> parseArgs(int argc, char* argv[], std::string& error);
std::string usageText();
