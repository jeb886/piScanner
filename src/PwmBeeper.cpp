#include "PwmBeeper.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

constexpr int kLowHz = 2471;
constexpr int kMediumHz = 2752;
constexpr int kHighHz = 3056;
constexpr auto kSysfsRoot = "/sys/class/pwm";
constexpr auto kSetupHint = " (enable dtoverlay=pwm,pin=18,func=2 and reboot)";

int pwmChannelForGpio(int gpio)
{
    switch (gpio) {
    case 12:
        return 0;
    case 13:
        return 1;
    case 18:
        return 2;
    case 19:
        return 3;
    default:
        return -1;
    }
}

int readNpwm(const std::string& chipPath)
{
    std::ifstream input(chipPath + "/npwm");
    int count = 0;
    if (!(input >> count))
        return -1;
    return count;
}

std::string findPwmChip(const Config& config)
{
    if (!config.beeperPwmChip.empty())
        return config.beeperPwmChip;

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(kSysfsRoot, error)) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("pwmchip", 0) != 0 || readNpwm(entry.path().string()) != 4)
            continue;

        char resolved[PATH_MAX] = {};
        const auto devicePath = entry.path() / "device";
        if (::realpath(devicePath.c_str(), resolved) != nullptr &&
            std::string(resolved).ends_with("98000.pwm"))
            return entry.path().string();
    }

    throw std::runtime_error("cannot find the RP1 PWM chip" + std::string(kSetupHint));
}

int openSysfs(const std::string& path)
{
    return ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
}

bool writeFd(int fd, std::string_view value, int& error)
{
    for (;;) {
        const ssize_t written = ::write(fd, value.data(), value.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0) {
            error = errno;
            return false;
        }
        if (static_cast<std::size_t>(written) != value.size()) {
            error = EIO;
            return false;
        }
        return true;
    }
}

} // namespace

PwmBeeper::PwmBeeper(const Config& config) : durationMs_(config.beeperMs)
{
    try {
        chipPath_ = findPwmChip(config);
        channel_ = config.beeperPwmChannel >= 0 ? config.beeperPwmChannel
                                                : pwmChannelForGpio(config.beeperLine);
        const int npwm = readNpwm(chipPath_);
        if (channel_ < 0 || npwm <= channel_) {
            throw std::runtime_error("PWM channel " + std::to_string(channel_) + " is unavailable on " + chipPath_ +
                                     std::string(kSetupHint));
        }

        const int exportFd = openSysfs(chipPath_ + "/export");
        if (exportFd < 0)
            throw std::runtime_error("cannot open " + chipPath_ + "/export" + std::string(kSetupHint));

        int writeError = 0;
        const std::string channelText = std::to_string(channel_);
        if (writeFd(exportFd, channelText, writeError)) {
            exportedByUs_ = true;
        } else if (writeError != EBUSY) {
            close(exportFd);
            throw std::runtime_error("cannot export PWM channel " + channelText + " from " + chipPath_ + ": " +
                                     std::strerror(writeError) + std::string(kSetupHint));
        }
        close(exportFd);

        const auto pwmPath = chipPath_ + "/pwm" + std::to_string(channel_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            periodFd_ = openSysfs(pwmPath + "/period");
            dutyCycleFd_ = openSysfs(pwmPath + "/duty_cycle");
            enableFd_ = openSysfs(pwmPath + "/enable");
            if (periodFd_ >= 0 && dutyCycleFd_ >= 0 && enableFd_ >= 0)
                break;
            if (periodFd_ >= 0)
                close(periodFd_);
            if (dutyCycleFd_ >= 0)
                close(dutyCycleFd_);
            if (enableFd_ >= 0)
                close(enableFd_);
            periodFd_ = dutyCycleFd_ = enableFd_ = -1;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("cannot open writable PWM channel " + pwmPath + std::string(kSetupHint));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (!writePwmValue(enableFd_, "0"))
            throw std::runtime_error("cannot disable PWM channel " + pwmPath);
        worker_ = std::thread(&PwmBeeper::run, this);
    } catch (...) {
        cleanup();
        throw;
    }
}

PwmBeeper::~PwmBeeper()
{
    cleanup();
}

bool PwmBeeper::writePwmValue(int fd, const std::string& value) const noexcept
{
    for (;;) {
        const ssize_t written = ::pwrite(fd, value.data(), value.size(), 0);
        if (written < 0 && errno == EINTR)
            continue;
        return written >= 0 && static_cast<std::size_t>(written) == value.size();
    }
}

bool PwmBeeper::setTone(int frequencyHz) noexcept
{
    const auto periodNs = (1'000'000'000LL + frequencyHz / 2) / frequencyHz;
    const auto period = std::to_string(periodNs);
    const auto dutyCycle = std::to_string(periodNs / 2);
    if (!writePwmValue(dutyCycleFd_, "0") || !writePwmValue(periodFd_, period) ||
        !writePwmValue(dutyCycleFd_, dutyCycle) || !writePwmValue(enableFd_, "1")) {
        disable();
        return false;
    }
    return true;
}

void PwmBeeper::disable() noexcept
{
    if (enableFd_ >= 0)
        writePwmValue(enableFd_, "0");
}

bool PwmBeeper::waitForTone()
{
    std::unique_lock lock(mutex_);
    return !cv_.wait_for(lock, std::chrono::milliseconds(durationMs_), [&] { return stop_; });
}

bool PwmBeeper::playTone(int frequencyHz)
{
    if (!setTone(frequencyHz))
        return false;
    const bool completed = waitForTone();
    disable();
    return completed;
}

bool PwmBeeper::playDecodeTone()
{
    if (!setTone(kMediumHz))
        return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs_);
    for (;;) {
        std::unique_lock lock(mutex_);
        if (stop_) {
            lock.unlock();
            disable();
            return false;
        }
        cv_.wait_until(lock, deadline, [&] { return stop_ || beepPending_; });
        if (stop_) {
            lock.unlock();
            disable();
            return false;
        }
        if (beepPending_) {
            beepPending_ = false;
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs_);
            continue;
        }
        lock.unlock();
        disable();
        return true;
    }
}

void PwmBeeper::startup()
{
    {
        std::lock_guard lock(mutex_);
        if (startupStarted_)
            return;
        startupStarted_ = true;
        startupPending_ = true;
    }
    cv_.notify_one();
}

void PwmBeeper::beep()
{
    {
        std::lock_guard lock(mutex_);
        beepPending_ = true;
    }
    cv_.notify_one();
}

void PwmBeeper::run()
{
    for (;;) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || startupPending_ || beepPending_; });
        if (stop_)
            return;
        if (startupPending_) {
            startupPending_ = false;
            lock.unlock();
            if (!playTone(kLowHz) || !playTone(kMediumHz) || !playTone(kHighHz))
                return;
            continue;
        }
        beepPending_ = false;
        lock.unlock();
        if (!playDecodeTone())
            return;
    }
}

void PwmBeeper::cleanup() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();

    disable();
    if (periodFd_ >= 0)
        close(periodFd_);
    if (dutyCycleFd_ >= 0)
        close(dutyCycleFd_);
    if (enableFd_ >= 0)
        close(enableFd_);
    periodFd_ = dutyCycleFd_ = enableFd_ = -1;

    if (exportedByUs_) {
        const int unexportFd = openSysfs(chipPath_ + "/unexport");
        if (unexportFd >= 0) {
            int writeError = 0;
            const std::string channelText = std::to_string(channel_);
            writeFd(unexportFd, channelText, writeError);
            close(unexportFd);
        }
        exportedByUs_ = false;
    }
}
