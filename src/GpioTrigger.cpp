#include "GpioTrigger.h"

#include "Output.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <time.h>

namespace {

int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::uint64_t monotonicUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1000;
}

}

std::string findGpioChip()
{
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string path = entry.path().string();
        if (path.find("/dev/gpiochip") != 0)
            continue;
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        gpiochip_info info{};
        const bool matches = xioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
                             std::string(info.label) == "pinctrl-rp1";
        close(fd);
        if (matches)
            return path;
    }
    throw std::runtime_error("no pinctrl-rp1 GPIO chip found");
}

GpioTrigger::GpioTrigger(const Config& config) : activeLow_(config.activeLow)
{
    const std::string chip = config.gpioChip.empty() ? findGpioChip() : config.gpioChip;
    const int chipFd = open(chip.c_str(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0)
        throw std::runtime_error("cannot open GPIO chip " + chip + ": " + std::strerror(errno));

    gpio_v2_line_request request{};
    request.offsets[0] = static_cast<std::uint32_t>(config.gpioLine);
    request.num_lines = 1;
    request.event_buffer_size = 16;
    std::strncpy(request.consumer, "piscan-trigger", sizeof(request.consumer) - 1);
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    if (config.gpioBias == "pull-up")
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    else if (config.gpioBias == "pull-down")
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    else
        request.config.flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    gpio_v2_line_config_attribute debounce{};
    debounce.attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    debounce.attr.debounce_period_us = static_cast<std::uint32_t>(config.debounceUs);
    debounce.mask = 1;
    request.config.attrs[0] = debounce;
    request.config.num_attrs = 1;

    if (xioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        const std::string reason = std::strerror(errno);
        close(chipFd);
        throw std::runtime_error("cannot request GPIO line " + std::to_string(config.gpioLine) + " from " + chip +
                                ": " + reason);
    }
    fd_ = request.fd;
    close(chipFd);

    gpio_v2_line_values values{};
    values.mask = 1;
    if (xioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) == 0) {
        const bool high = (values.bits & 1) != 0;
        const bool pressed = activeLow_ ? !high : high;
        if (pressed) {
            initialEvent_ = true;
            initial_.type = TriggerEventType::Pressed;
            initial_.timestampUs = monotonicUs();
        }
    }
}

GpioTrigger::~GpioTrigger()
{
    if (fd_ >= 0)
        close(fd_);
}

bool GpioTrigger::wait(int timeoutMs, TriggerEvent& event)
{
    if (initialEvent_) {
        initialEvent_ = false;
        event = initial_;
        return true;
    }
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = poll(&descriptor, 1, timeoutMs);
    if (ready < 0) {
        if (errno == EINTR)
            return false;
        throw std::runtime_error(std::string("poll GPIO failed: ") + std::strerror(errno));
    }
    if (ready == 0)
        return false;
    gpio_v2_line_event lineEvent{};
    const auto bytes = read(fd_, &lineEvent, sizeof(lineEvent));
    if (bytes != sizeof(lineEvent))
        throw std::runtime_error("failed to read GPIO event");
    const bool rising = lineEvent.id == GPIO_V2_LINE_EVENT_RISING_EDGE;
    event.type = (rising != activeLow_) ? TriggerEventType::Pressed : TriggerEventType::Released;
    event.timestampUs = lineEvent.timestamp_ns / 1000;
    return true;
}

bool StdinTrigger::wait(int timeoutMs, TriggerEvent& event)
{
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&descriptor, 1, timeoutMs);
    if (ready <= 0)
        return false;
    char value = 0;
    if (read(STDIN_FILENO, &value, 1) != 1)
        return false;
    if (value != '\n' && value != '\r')
        return false;
    event.type = TriggerEventType::Pressed;
    event.timestampUs = monotonicUs();
    return true;
}
