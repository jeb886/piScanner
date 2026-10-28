#pragma once

#include "Trigger.h"

#include <atomic>
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
// values and it reports Pressed (scan rect), Aux (capture rect, or any
// tap while in photo mode) and Released. Kept fd-free so tests can drive
// it without hardware.
class TouchDecoder {
public:
    TouchDecoder(float x0, float y0, float x1, float y1,
                 float cx0, float cy0, float cx1, float cy1,
                 int xmin, int xmax, int ymin, int ymax,
                 int rotation = 0, const std::atomic<int>* mode = nullptr);
    // Returns an event to emit, or nullopt while the batch continues.
    std::optional<TriggerEventType> feed(int type, int code, int value);
    std::string lastRaw() const;

private:
    bool inside(float x0, float y0, float x1, float y1) const;
    int rotation_ = 0;
    float rx0_, ry0_, rx1_, ry1_;   // scan button rect
    float cx0_, cy0_, cx1_, cy1_;   // capture button rect
    int axMin_, axRange_, ayMin_, ayRange_;
    const std::atomic<int>* mode_ = nullptr; // 0=ui, 1=photo; optional
    bool down_ = false;
    bool pressSent_ = false;
    bool auxSent_ = false;
    int curX_ = 0, curY_ = 0;
};

// Touchscreen trigger: reads an evdev input device (e.g. the ADS7846 on a
// 3.5" SPI HAT) and emits Pressed/Released for the scan rect and Aux for
// the capture rect. create() returns nullptr when no touchscreen exists.
class TouchTrigger final : public Trigger {
public:
    static std::unique_ptr<TouchTrigger> create(float x0, float y0, float x1, float y1,
                                                float cx0, float cy0, float cx1, float cy1,
                                                int rotation = 0,
                                                const std::atomic<int>* mode = nullptr);
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
