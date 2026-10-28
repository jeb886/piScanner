#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

enum class TriggerEventType {
    Pressed,
    Released,
    Aux // secondary button: capture image / exit image view
};

struct TriggerEvent {
    TriggerEventType type = TriggerEventType::Pressed;
    std::uint64_t timestampUs = 0;
};

class Trigger {
public:
    virtual ~Trigger() = default;
    virtual bool wait(int timeoutMs, TriggerEvent& event) = 0;
};

// Test trigger: emits a Press each time the previous scan session has
// completed (the watched session-event counter catches up to the press
// count), up to `count` presses total — the first press fires immediately.
// Paired with a FileFrameSource (which pauses while disarmed) this decodes
// one image per press, deterministically.
class ScriptedTrigger : public Trigger {
public:
    ScriptedTrigger(std::atomic<std::uint64_t>& sessions, int count)
        : sessions_(sessions), count_(count)
    {
    }
    bool wait(int timeoutMs, TriggerEvent& event) override
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (presses_ >= count_ || sessions_.load() < static_cast<std::uint64_t>(presses_)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++presses_;
        event.type = TriggerEventType::Pressed;
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        event.timestampUs = static_cast<std::uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
        return true;
    }

private:
    std::atomic<std::uint64_t>& sessions_;
    int count_;
    int presses_ = 0;
};
