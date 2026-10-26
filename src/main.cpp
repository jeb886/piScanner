#include "Bench.h"
#include "Config.h"
#include "DecodeFiles.h"
#include "FileFrameSource.h"
#include "GpioTrigger.h"
#include "Output.h"
#include "Scanner.h"
#include "V4l2Camera.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char* argv[])
{
    std::string error;
    const auto parsed = parseArgs(argc, argv, error);
    if (!parsed) {
        std::cerr << "error: " << error << "\n\n" << usageText();
        return 2;
    }
    const Config& config = *parsed;
    if (config.command == "help") {
        std::cout << usageText();
        return 0;
    }
    if (config.command == "version") {
        std::cout << "piscan " << PISCAN_VERSION << '\n';
        return 0;
    }
    if (config.command == "devices") {
        try {
            V4l2Camera::listDevices();
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "error: " << exception.what() << '\n';
            return 1;
        }
    }
    if (config.command == "bench") {
        try {
            return runBench(config, std::cout);
        } catch (const std::exception& exception) {
            std::cerr << "error: " << exception.what() << '\n';
            return 1;
        }
    }
    if (config.command == "decode") {
        try {
            return runDecodeFiles(config, std::cout);
        } catch (const std::exception& exception) {
            std::cerr << "error: " << exception.what() << '\n';
            return 1;
        }
    }

    std::unique_ptr<Trigger> trigger;
    try {
        if (config.command == "trigger") {
            if (config.trigger == "gpio")
                trigger = std::make_unique<GpioTrigger>(config);
            else
                trigger = std::make_unique<StdinTrigger>();
        }
        std::unique_ptr<FrameSource> source;
        if (config.source.empty())
            source = std::make_unique<V4l2Camera>(config);
        else
            source = std::make_unique<FileFrameSource>(config.source, config);
        std::unique_ptr<ResultSink> sink;
        if (config.output == "json")
            sink = std::make_unique<JsonSink>(config.command == "trigger");
        else if (config.output == "hid")
            sink = std::make_unique<HidSink>(config.hidDevice);
        else
            sink = std::make_unique<TextSink>();
        std::signal(SIGINT, requestShutdownSignal);
        std::signal(SIGTERM, requestShutdownSignal);
        Scanner scanner(config, *source, *sink, trigger.get());
        return scanner.run();
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << '\n';
        return 1;
    }
}
