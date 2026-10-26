#pragma once

#include "Config.h"
#include "Frame.h"
#include "Output.h"
#include "Trigger.h"

#include <atomic>
#include <memory>
#include <string>

class Scanner {
public:
    // sessionEvents (optional): atomically updated with the count of
    // completed trigger scan sessions (result emitted or timeout).
    // ScriptedTrigger uses it to press once per session, deterministically.
    Scanner(const Config& config, FrameSource& source, ResultSink& sink, Trigger* trigger,
            std::atomic<std::uint64_t>* sessionEvents = nullptr);
    ~Scanner();
    int run();
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void requestShutdownSignal(int signal);
