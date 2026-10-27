#pragma once

#include "Config.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Drives a passive buzzer through a Linux sysfs hardware-PWM channel.
// startup() and beep() are non-blocking; decode beeps that arrive while a
// tone is active extend its duration instead of queueing separate tones.
class PwmBeeper {
public:
    explicit PwmBeeper(const Config& config);
    ~PwmBeeper();
    PwmBeeper(const PwmBeeper&) = delete;
    PwmBeeper& operator=(const PwmBeeper&) = delete;

    void startup();
    void beep();

private:
    bool writePwmValue(int fd, const std::string& value) const noexcept;
    bool setTone(int frequencyHz) noexcept;
    void disable() noexcept;
    bool waitForTone();
    bool playTone(int frequencyHz);
    bool playDecodeTone();
    void run();
    void cleanup() noexcept;

    std::string chipPath_;
    int channel_ = -1;
    int periodFd_ = -1;
    int dutyCycleFd_ = -1;
    int enableFd_ = -1;
    bool exportedByUs_ = false;
    int durationMs_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool startupStarted_ = false;
    bool startupPending_ = false;
    bool beepPending_ = false;
};
