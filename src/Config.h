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
    std::string hidDevice = "/dev/hidg0";
    std::string logFile;
    int beeperLine = -1;   // PWM GPIO for the beeper; -1 = disabled
    int beeperMs = 80;     // beep duration
    std::string beeperPwmChip;
    int beeperPwmChannel = -1; // -1 = derive from beeperLine
    bool stats = false;
    bool verbose = false;
    std::string trigger = "gpio";
    std::string gpioChip;
    int gpioLine = 16;
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
    std::string captureFile;
    int warmup = 8; // capture: frames to discard before the one written out
    // File-source (mock camera) options for scan/trigger/capture: image files
    // and/or directories fed through the pipeline instead of /dev/videoN.
    std::vector<std::string> source;
    int repeat = 1;           // passes over the image list; 0 = loop forever
    // SPI panel output (fbdev path or "auto"); empty = no display.
    std::string displayDevice;
    int displayLines = 4;     // history rows shown under the latest result
    int displayRotate = 0;    // UI rotation in degrees: 0, 90, 180, 270
    bool expect = false;      // decode: compare results to .txt sidecars
    std::vector<std::string> decodeInputs; // decode: positional files/dirs
};

std::optional<Config> parseArgs(int argc, char* argv[], std::string& error);
std::string usageText();
