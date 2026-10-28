#pragma once

#include "Trigger.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ORs several Trigger sources (e.g. GPIO button + touchscreen button).
// One worker thread per inner trigger funnels events into a shared queue.
class MultiTrigger final : public Trigger {
public:
    explicit MultiTrigger(std::vector<std::unique_ptr<Trigger>> sources);
    ~MultiTrigger() override;
    bool wait(int timeoutMs, TriggerEvent& event) override;

private:
    std::vector<std::unique_ptr<Trigger>> sources_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<TriggerEvent> queue_;
    bool stopping_ = false;
};

// Touchscreen trigger: reads an evdev input device (e.g. the ADS7846 on a
// 3.5" SPI HAT) and emits Pressed/Released when a touch lands inside a
// normalized screen rect (fractions of the panel size, so it matches the
// rendered button at any resolution). Opens nothing when no resistive
// touchscreen exists — construct via create() which may return nullptr.
class TouchTrigger final : public Trigger {
public:
    // Normalized rect (0..1 of panel width/height). abs coords are mapped
    // through the device's reported min/max ranges.
    static std::unique_ptr<TouchTrigger> create(float x0, float y0, float x1, float y1);
    ~TouchTrigger() override;
    bool wait(int timeoutMs, TriggerEvent& event) override;

private:
    TouchTrigger(int fd, float x0, float y0, float x1, float y1,
                 int xmin, int xmax, int ymin, int ymax);
    int fd_ = -1;
    float rx0_, ry0_, rx1_, ry1_;
    int axMin_, axRange_, ayMin_, ayRange_;
    bool down_ = false;
    bool inside_ = false;
    int curX_ = 0, curY_ = 0;
};

// Scans /dev/input/event* for an ABS_X+ABS_Y touchscreen (ADS7846/XPT2046
// style). Returns the device path or "".
std::string findTouchDevice();
