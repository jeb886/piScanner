#include "TouchTrigger.h"

#include "Log.h"

#include <linux/input.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

std::uint64_t nowUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int absMax(int fd, int axis)
{
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(axis), &info) < 0)
        return -1;
    return info.maximum;
}
int absMin(int fd, int axis)
{
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(axis), &info) < 0)
        return -1;
    return info.minimum;
}

} // namespace

MultiTrigger::MultiTrigger(std::vector<std::unique_ptr<Trigger>> sources)
    : sources_(std::move(sources))
{
    workers_.reserve(sources_.size());
    for (auto& source : sources_) {
        Trigger* t = source.get();
        workers_.emplace_back([this, t] {
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopping_)
                        return;
                }
                TriggerEvent event;
                if (t->wait(50, event)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        queue_.push_back(event);
                    }
                    cv_.notify_one();
                }
            }
        });
    }
}

MultiTrigger::~MultiTrigger()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_)
        worker.join();
}

bool MultiTrigger::wait(int timeoutMs, TriggerEvent& event)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !queue_.empty(); }))
        return false;
    event = queue_.front();
    queue_.pop_front();
    return true;
}

std::string findTouchDevice()
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/input", error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("event", 0) != 0)
            continue;
        const std::string path = entry.path().string();
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        char devName[128] = {};
        ioctl(fd, EVIOCGNAME(sizeof(devName)), devName);
        unsigned long absBits = 0;
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), &absBits);
        const bool hasX = absBits & (1UL << ABS_X);
        const bool hasY = absBits & (1UL << ABS_Y);
        ::close(fd);
        // Resistive panels report ABS_X/ABS_Y plus a name containing the
        // controller or "touch"; accept either signal.
        const std::string lower = std::string(devName);
        if (hasX && hasY &&
            (lower.find("7846") != std::string::npos || lower.find("ouch") != std::string::npos ||
             lower.find("xpt") != std::string::npos))
            return path;
    }
    return {};
}

std::unique_ptr<TouchTrigger> TouchTrigger::create(float x0, float y0, float x1, float y1)
{
    const std::string path = findTouchDevice();
    if (path.empty())
        return nullptr;
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    const int xmin = absMin(fd, ABS_X), xmax = absMax(fd, ABS_X);
    const int ymin = absMin(fd, ABS_Y), ymax = absMax(fd, ABS_Y);
    if (xmax <= xmin || ymax <= ymin) {
        ::close(fd);
        return nullptr;
    }
    logLine("touch " + path);
    return std::unique_ptr<TouchTrigger>(new TouchTrigger(fd, x0, y0, x1, y1, xmin, xmax, ymin, ymax));
}

TouchTrigger::TouchTrigger(int fd, float x0, float y0, float x1, float y1,
                           int xmin, int xmax, int ymin, int ymax)
    : fd_(fd), rx0_(x0), ry0_(y0), rx1_(x1), ry1_(y1),
      axMin_(xmin), axRange_(xmax - xmin), ayMin_(ymin), ayRange_(ymax - ymin)
{
}

TouchTrigger::~TouchTrigger()
{
    if (fd_ >= 0)
        ::close(fd_);
}

bool TouchTrigger::wait(int timeoutMs, TriggerEvent& event)
{
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, timeoutMs) <= 0)
        return false;
    input_event events[16];
    const ssize_t got = ::read(fd_, events, sizeof(events));
    if (got <= 0)
        return false;
    for (std::size_t i = 0; i + sizeof(input_event) <= static_cast<std::size_t>(got); i += sizeof(input_event)) {
        const input_event& e = *reinterpret_cast<const input_event*>(reinterpret_cast<const char*>(events) + i);
        if (e.type == EV_ABS && e.code == ABS_X)
            curX_ = e.value;
        else if (e.type == EV_ABS && e.code == ABS_Y)
            curY_ = e.value;
        else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
            if (e.value == 1) {
                const float nx = static_cast<float>(curX_ - axMin_) / static_cast<float>(axRange_);
                const float ny = static_cast<float>(curY_ - ayMin_) / static_cast<float>(ayRange_);
                down_ = true;
                inside_ = nx >= rx0_ && nx <= rx1_ && ny >= ry0_ && ny <= ry1_;
                if (inside_) {
                    event.type = TriggerEventType::Pressed;
                    event.timestampUs = nowUs();
                    return true;
                }
            } else if (down_) {
                down_ = false;
                if (inside_) {
                    inside_ = false;
                    event.type = TriggerEventType::Released;
                    event.timestampUs = nowUs();
                    return true;
                }
            }
        }
    }
    return false;
}
