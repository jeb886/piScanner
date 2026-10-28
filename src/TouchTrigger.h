#pragma once

#include "Trigger.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct input_event;

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

// Pure event-decoding half of TouchTrigger: feed it evdev input_event
// values and it reports Pressed/Released for touches inside a normalized
// rect. Kept fd-free so tests can drive it without hardware.
class TouchDecoder {
public:
    TouchDecoder(float x0, float y0, float x1, float y1, int xmin, int xmax, int ymin, int ymax,
                 int rotation = 0);
    // Returns an event to emit, or nullopt while the batch continues.
    std::optional<TriggerEventType> feed(int type, int code, int value);
    std::string lastRaw() const;

private:
    bool inside() const;
    int rotation_ = 0;
    float rx0_, ry0_, rx1_, ry1_;
    int axMin_, axRange_, ayMin_, ayRange_;
    bool down_ = false;
    bool pressSent_ = false;
    int curX_ = 0, curY_ = 0;
};

// Touchscreen trigger: reads an evdev input device (e.g. the ADS7846 on a
// 3.5" SPI HAT) and emits Pressed/Released when a touch lands inside a
// normalized screen rect (fractions of the panel size, so it matches the
// rendered button at any resolution). create() returns nullptr when no
// resistive touchscreen exists.
class TouchTrigger final : public Trigger {
public:
    static std::unique_ptr<TouchTrigger> create(float x0, float y0, float x1, float y1, int rotation = 0);
    ~TouchTrigger() override;
    bool wait(int timeoutMs, TriggerEvent& event) override;

private:
    TouchTrigger(int fd, TouchDecoder decoder);
    int fd_ = -1;
    TouchDecoder decoder_;
};

// Scans /dev/input/event* for an ABS_X+ABS_Y touchscreen (ADS7846/XPT2046
// style). Returns the device path or "".
std::string findTouchDevice();
