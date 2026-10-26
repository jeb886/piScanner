#include "GpioBeeper.h"

#include "GpioTrigger.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}
}

GpioBeeper::GpioBeeper(const Config& config) : durationMs_(config.beeperMs)
{
    const std::string chip = config.gpioChip.empty() ? findGpioChip() : config.gpioChip;
    const int chipFd = open(chip.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0)
        throw std::runtime_error("cannot open GPIO chip " + chip + ": " + std::strerror(errno));

    gpio_v2_line_request request{};
    request.offsets[0] = static_cast<std::uint32_t>(config.beeperLine);
    request.num_lines = 1;
    std::strncpy(request.consumer, "piscan-beeper", sizeof(request.consumer) - 1);
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (xioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const std::string reason = std::strerror(errno);
        close(chipFd);
        throw std::runtime_error("cannot request beeper GPIO line " + std::to_string(config.beeperLine) +
                                 " from " + chip + ": " + reason);
    }
    fd_ = request.fd;
    close(chipFd);
    set(false);
    worker_ = std::thread(&GpioBeeper::run, this);
}

GpioBeeper::~GpioBeeper()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    set(false);
    if (fd_ >= 0)
        close(fd_);
}

void GpioBeeper::set(bool on)
{
    gpio_v2_line_values values{};
    values.mask = 1;
    values.bits = on ? 1 : 0;
    xioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

void GpioBeeper::beep()
{
    {
        std::lock_guard lock(mutex_);
        sounding_ = true;
    }
    cv_.notify_one();
}

void GpioBeeper::run()
{
    for (;;) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || sounding_; });
        if (stop_)
            return;
        lock.unlock();
        set(true);
        // Stay on for the duration; a beep() arriving while sounding
        // restarts the window (extends the tone).
        for (;;) {
            lock.lock();
            sounding_ = false;
            const bool more = cv_.wait_for(lock, std::chrono::milliseconds(durationMs_),
                                           [&] { return stop_ || sounding_; });
            if (stop_)
                return;
            if (!more) {
                lock.unlock();
                set(false);
                break;
            }
            lock.unlock();
        }
    }
}
