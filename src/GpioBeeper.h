#pragma once

#include "Config.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// Drives a simple two-wire GPIO beeper (active buzzer module, e.g. GPIO
// high = on). beep() is non-blocking: a worker thread pulses the line for
// config.beeperMs milliseconds, and extra calls while pulsing extend the
// tone instead of queueing up.
class GpioBeeper {
public:
    explicit GpioBeeper(const Config& config);
    ~GpioBeeper();
    GpioBeeper(const GpioBeeper&) = delete;
    GpioBeeper& operator=(const GpioBeeper&) = delete;

    void beep();

private:
    void set(bool on);
    void run();

    int fd_ = -1;
    int durationMs_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool sounding_ = false;
};
