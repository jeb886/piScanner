#include "Config.h"

#include <BarcodeFormat.h>

#include <getopt.h>

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace {

bool parseInt(const char* text, int& value)
{
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < -2147483648L || parsed > 2147483647L)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parseRoi(const std::string& value, RegionOfInterest& roi)
{
    std::istringstream input(value);
    char comma1 = 0;
    char comma2 = 0;
    char comma3 = 0;
    if (!(input >> roi.x >> comma1 >> roi.y >> comma2 >> roi.width >> comma3 >> roi.height) ||
        comma1 != ',' || comma2 != ',' || comma3 != ',' || input.peek() != std::char_traits<char>::eof()) {
        return false;
    }
    return roi.x >= 0 && roi.y >= 0 && roi.width > 0 && roi.height > 0 && roi.x < 1 && roi.y < 1 &&
           roi.x + roi.width <= 1 && roi.y + roi.height <= 1;
}

}

std::optional<Config> parseArgs(int argc, char* argv[], std::string& error)
{
    error.clear();
    Config config;
    if (argc < 2) {
        error = "missing subcommand";
        return std::nullopt;
    }
    config.command = argv[1];
    if (config.command == "--help" || config.command == "-h") {
        config.command = "help";
        return config;
    }
    if (config.command == "--version") {
        config.command = "version";
        return config;
    }
    if (config.command != "devices" && config.command != "scan" && config.command != "trigger" &&
        config.command != "decode" && config.command != "bench") {
        error = "unknown subcommand: " + config.command;
        return std::nullopt;
    }

    enum {
        OPT_WIDTH = 1000,
        OPT_HEIGHT,
        OPT_FPS,
        OPT_PIXFMT,
        OPT_BUFFERS,
        OPT_THREADS,
        OPT_FORMATS,
        OPT_TRY_HARDER,
        OPT_NO_ROTATE,
        OPT_NO_DOWNSCALE,
        OPT_MAX_SYMBOLS,
        OPT_BINARIZER,
        OPT_ROI,
        OPT_OUTPUT,
        OPT_HID_DEVICE,
        OPT_BEEPER_LINE,
        OPT_BEEPER_MS,
        OPT_BEEPER_PWMCHIP,
        OPT_BEEPER_PWMCHANNEL,
        OPT_STATS,
        OPT_TRIGGER,
        OPT_GPIO_CHIP,
        OPT_GPIO_LINE,
        OPT_GPIO_BIAS,
        OPT_DEBOUNCE_US,
        OPT_ACTIVE_LOW,
        OPT_NO_ACTIVE_LOW,
        OPT_TRIGGER_MODE,
        OPT_SCAN_TIMEOUT_MS,
        OPT_DEDUPE_MS,
        OPT_EXPOSURE,
        OPT_FOCUS,
        OPT_GAIN,
        OPT_ITERATIONS,
        OPT_SOURCE,
        OPT_REPEAT,
        OPT_EXPECT,
        OPT_VERSION
    };
    static const option options[] = {
        {"device", required_argument, nullptr, 'd'},
        {"width", required_argument, nullptr, OPT_WIDTH},
        {"height", required_argument, nullptr, OPT_HEIGHT},
        {"fps", required_argument, nullptr, OPT_FPS},
        {"pixfmt", required_argument, nullptr, OPT_PIXFMT},
        {"buffers", required_argument, nullptr, OPT_BUFFERS},
        {"threads", required_argument, nullptr, OPT_THREADS},
        {"formats", required_argument, nullptr, OPT_FORMATS},
        {"try-harder", no_argument, nullptr, OPT_TRY_HARDER},
        {"no-rotate", no_argument, nullptr, OPT_NO_ROTATE},
        {"no-downscale", no_argument, nullptr, OPT_NO_DOWNSCALE},
        {"max-symbols", required_argument, nullptr, OPT_MAX_SYMBOLS},
        {"binarizer", required_argument, nullptr, OPT_BINARIZER},
        {"roi", required_argument, nullptr, OPT_ROI},
        {"output", required_argument, nullptr, OPT_OUTPUT},
        {"hid-device", required_argument, nullptr, OPT_HID_DEVICE},
        {"beeper-line", required_argument, nullptr, OPT_BEEPER_LINE},
        {"beeper-ms", required_argument, nullptr, OPT_BEEPER_MS},
        {"beeper-pwmchip", required_argument, nullptr, OPT_BEEPER_PWMCHIP},
        {"beeper-pwm-channel", required_argument, nullptr, OPT_BEEPER_PWMCHANNEL},
        {"stats", no_argument, nullptr, OPT_STATS},
        {"verbose", no_argument, nullptr, 'v'},
        {"gpio-trigger", required_argument, nullptr, OPT_TRIGGER},
        {"trigger", required_argument, nullptr, OPT_TRIGGER},
        {"gpio-chip", required_argument, nullptr, OPT_GPIO_CHIP},
        {"gpio-line", required_argument, nullptr, OPT_GPIO_LINE},
        {"gpio-bias", required_argument, nullptr, OPT_GPIO_BIAS},
        {"debounce-us", required_argument, nullptr, OPT_DEBOUNCE_US},
        {"active-low", no_argument, nullptr, OPT_ACTIVE_LOW},
        {"no-active-low", no_argument, nullptr, OPT_NO_ACTIVE_LOW},
        {"trigger-mode", required_argument, nullptr, OPT_TRIGGER_MODE},
        {"scan-timeout-ms", required_argument, nullptr, OPT_SCAN_TIMEOUT_MS},
        {"dedupe-ms", required_argument, nullptr, OPT_DEDUPE_MS},
        {"exposure", required_argument, nullptr, OPT_EXPOSURE},
        {"focus", required_argument, nullptr, OPT_FOCUS},
        {"gain", required_argument, nullptr, OPT_GAIN},
        {"iterations", required_argument, nullptr, OPT_ITERATIONS},
        {"source", required_argument, nullptr, OPT_SOURCE},
        {"repeat", required_argument, nullptr, OPT_REPEAT},
        {"expect", no_argument, nullptr, OPT_EXPECT},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, OPT_VERSION},
        {nullptr, 0, nullptr, 0},
    };

    auto assignInt = [&](const char* name, int& destination) {
        if (!parseInt(optarg, destination)) {
            error = std::string("invalid value for --") + name + ": " + optarg;
            return false;
        }
        return true;
    };

    opterr = 0;
    optind = 2;
    for (;;) {
        const int option = getopt_long(argc, argv, "d:vh", options, nullptr);
        if (option == -1)
            break;
        switch (option) {
        case 'd':
            config.device = optarg;
            break;
        case 'v':
            config.verbose = true;
            break;
        case 'h':
            config.command = "help";
            return config;
        case OPT_WIDTH:
            if (!assignInt("width", config.width))
                return std::nullopt;
            break;
        case OPT_HEIGHT:
            if (!assignInt("height", config.height))
                return std::nullopt;
            break;
        case OPT_FPS:
            if (!assignInt("fps", config.fps))
                return std::nullopt;
            break;
        case OPT_PIXFMT:
            config.pixfmt = optarg;
            break;
        case OPT_BUFFERS:
            if (!assignInt("buffers", config.buffers))
                return std::nullopt;
            break;
        case OPT_THREADS:
            if (!assignInt("threads", config.threads))
                return std::nullopt;
            break;
        case OPT_FORMATS:
            config.formats = optarg;
            break;
        case OPT_TRY_HARDER:
            config.tryHarder = true;
            break;
        case OPT_NO_ROTATE:
            config.tryRotate = false;
            break;
        case OPT_NO_DOWNSCALE:
            config.tryDownscale = false;
            break;
        case OPT_MAX_SYMBOLS:
            if (!assignInt("max-symbols", config.maxSymbols))
                return std::nullopt;
            break;
        case OPT_BINARIZER:
            config.binarizer = optarg;
            break;
        case OPT_ROI: {
            RegionOfInterest roi;
            if (!parseRoi(optarg, roi)) {
                error = "ROI must be x,y,w,h fractions within 0..1";
                return std::nullopt;
            }
            config.roi = roi;
            break;
        }
        case OPT_OUTPUT:
            config.output = optarg;
            break;
        case OPT_HID_DEVICE:
            config.hidDevice = optarg;
            break;
        case OPT_BEEPER_LINE:
            if (!assignInt("beeper-line", config.beeperLine))
                return std::nullopt;
            break;
        case OPT_BEEPER_MS:
            if (!assignInt("beeper-ms", config.beeperMs))
                return std::nullopt;
            break;
        case OPT_BEEPER_PWMCHIP:
            config.beeperPwmChip = optarg;
            break;
        case OPT_BEEPER_PWMCHANNEL:
            if (!assignInt("beeper-pwm-channel", config.beeperPwmChannel))
                return std::nullopt;
            break;
        case OPT_STATS:
            config.stats = true;
            break;
        case OPT_TRIGGER:
            config.trigger = optarg;
            break;
        case OPT_GPIO_CHIP:
            config.gpioChip = optarg;
            break;
        case OPT_GPIO_LINE:
            if (!assignInt("gpio-line", config.gpioLine))
                return std::nullopt;
            break;
        case OPT_GPIO_BIAS:
            config.gpioBias = optarg;
            break;
        case OPT_DEBOUNCE_US:
            if (!assignInt("debounce-us", config.debounceUs))
                return std::nullopt;
            break;
        case OPT_ACTIVE_LOW:
            config.activeLow = true;
            break;
        case OPT_NO_ACTIVE_LOW:
            config.activeLow = false;
            break;
        case OPT_TRIGGER_MODE:
            config.triggerMode = optarg;
            break;
        case OPT_SCAN_TIMEOUT_MS:
            if (!assignInt("scan-timeout-ms", config.scanTimeoutMs))
                return std::nullopt;
            break;
        case OPT_DEDUPE_MS:
            if (!assignInt("dedupe-ms", config.dedupeMs))
                return std::nullopt;
            break;
        case OPT_EXPOSURE: {
            int value = 0;
            if (!assignInt("exposure", value))
                return std::nullopt;
            config.exposure = value;
            break;
        }
        case OPT_FOCUS: {
            int value = 0;
            if (!assignInt("focus", value))
                return std::nullopt;
            config.focus = value;
            break;
        }
        case OPT_GAIN: {
            int value = 0;
            if (!assignInt("gain", value))
                return std::nullopt;
            config.gain = value;
            break;
        }
        case OPT_ITERATIONS:
            if (!assignInt("iterations", config.iterations))
                return std::nullopt;
            break;
        case OPT_SOURCE:
            config.source.emplace_back(optarg);
            break;
        case OPT_REPEAT:
            if (!assignInt("repeat", config.repeat))
                return std::nullopt;
            break;
        case OPT_EXPECT:
            config.expect = true;
            break;
        case OPT_VERSION:
            config.command = "version";
            return config;
        case '?':
        default:
            error = "unknown or incomplete option";
            return std::nullopt;
        }
    }

    if (config.command == "bench") {
        if (optind >= argc) {
            error = "bench requires a PGM file";
            return std::nullopt;
        }
        config.benchFile = argv[optind++];
        if (optind < argc) {
            error = "unexpected positional argument: " + std::string(argv[optind]);
            return std::nullopt;
        }
    } else if (config.command == "decode") {
        while (optind < argc)
            config.decodeInputs.emplace_back(argv[optind++]);
        if (config.decodeInputs.empty()) {
            error = "decode requires at least one image file or directory";
            return std::nullopt;
        }
    } else if (optind < argc) {
        error = "unexpected positional argument: " + std::string(argv[optind]);
        return std::nullopt;
    }

    if (config.width <= 0 || config.height <= 0 || config.fps <= 0 || config.buffers <= 0 || config.threads <= 0 ||
        config.maxSymbols <= 0 || config.maxSymbols > 255 || config.iterations <= 0 || config.gpioLine < 0 ||
        config.debounceUs < 0 ||
        config.scanTimeoutMs <= 0 || config.dedupeMs < 0) {
        error = "numeric options must be positive (dedupe and debounce may be zero)";
        return std::nullopt;
    }
    if (config.buffers < config.threads + 2 && (config.command == "scan" || config.command == "trigger")) {
        error = "--buffers must be at least --threads + 2";
        return std::nullopt;
    }
    if (config.pixfmt != "auto" && config.pixfmt != "yuyv" && config.pixfmt != "grey" && config.pixfmt != "mjpeg") {
        error = "--pixfmt must be yuyv, grey, mjpeg, or auto";
        return std::nullopt;
    }
#ifndef PISCANNER_HAVE_TURBOJPEG
    if (config.pixfmt == "mjpeg") {
        error = "MJPEG requested but this build has no libturbojpeg support";
        return std::nullopt;
    }
#endif
    if (config.beeperMs <= 0) {
        error = "--beeper-ms must be > 0";
        return std::nullopt;
    }
    if (config.beeperLine >= 0 && config.beeperLine != 12 && config.beeperLine != 13 && config.beeperLine != 18 &&
        config.beeperLine != 19) {
        error = "--beeper-line must be a hardware-PWM GPIO (12, 13, 18 or 19); the default is 18 (header pin 12)";
        return std::nullopt;
    }
    if (config.beeperPwmChannel < -1 || config.beeperPwmChannel > 3) {
        error = "--beeper-pwm-channel must be -1 or a PWM channel from 0 to 3";
        return std::nullopt;
    }
    if (config.output != "text" && config.output != "json" && config.output != "hid") {
        error = "--output must be text, json, or hid";
        return std::nullopt;
    }
    if (config.binarizer != "local" && config.binarizer != "global" && config.binarizer != "bool" &&
        config.binarizer != "fixed") {
        error = "--binarizer must be local, global, bool, or fixed";
        return std::nullopt;
    }
    if (config.command == "trigger" && config.trigger != "gpio" && config.trigger != "stdin") {
        error = "--trigger must be gpio or stdin";
        return std::nullopt;
    }
    if (config.repeat < 0) {
        error = "--repeat must be >= 0 (0 means loop forever)";
        return std::nullopt;
    }
    if (!config.source.empty() && config.command != "scan" && config.command != "trigger") {
        error = "--source is only valid with scan or trigger";
        return std::nullopt;
    }
    if (config.expect && config.command != "decode") {
        error = "--expect is only valid with decode";
        return std::nullopt;
    }
    if (config.gpioBias != "pull-up" && config.gpioBias != "pull-down" && config.gpioBias != "disable") {
        error = "--gpio-bias must be pull-up, pull-down, or disable";
        return std::nullopt;
    }
    if (config.triggerMode != "edge" && config.triggerMode != "level") {
        error = "--trigger-mode must be edge or level";
        return std::nullopt;
    }
    if (!config.formats.empty()) {
        try {
            ZXing::BarcodeFormatsFromString(config.formats);
        } catch (const std::exception& exception) {
            error = exception.what();
            return std::nullopt;
        }
    }
    return config;
}

std::string usageText()
{
    return "Usage:\n"
           "  piscanner devices\n"
           "  piscanner scan [options]\n"
           "  piscanner trigger [options]\n"
           "  piscanner decode <files/dirs> [--expect] [decoder options]\n"
           "  piscanner bench <file.pgm> [--iterations N] [decoder options]\n"
           "\n"
           "Common options: --device PATH --width N --height N --fps N --pixfmt auto|yuyv|grey|mjpeg\n"
           "  --buffers N --threads N --formats LIST --try-harder --no-rotate --no-downscale\n"
           "  --max-symbols N --binarizer local|global|bool|fixed --roi x,y,w,h\n"
           "  --output text|json|hid --hid-device PATH --beeper-line N --beeper-ms N --beeper-pwmchip PATH --beeper-pwm-channel N\n"
           "  --stats --exposure N --focus N --gain N -v --help --version\n"
           "Trigger options: --trigger gpio|stdin --gpio-chip PATH --gpio-line N --gpio-bias pull-up|pull-down|disable\n"
           "  --debounce-us N --active-low|--no-active-low --trigger-mode edge|level --scan-timeout-ms N\n"
           "File source (scan/trigger, replaces the camera): --source IMG_OR_DIR (repeatable)\n"
           "  --repeat N (passes over the images, 0=forever). decode: compares to <name>.txt\n"
           "  sidecars with --expect (sidecar text NONE means no barcode expected).\n";
}
