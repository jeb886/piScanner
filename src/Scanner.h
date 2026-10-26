#pragma once

#include "Config.h"
#include "Output.h"
#include "V4l2Camera.h"
#include "Trigger.h"

#include <atomic>
#include <memory>
#include <string>

class Scanner {
public:
    Scanner(const Config& config, V4l2Camera& camera, ResultSink& sink, Trigger* trigger);
    ~Scanner();
    int run();
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void requestShutdownSignal(int signal);
