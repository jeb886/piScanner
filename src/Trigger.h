#pragma once

#include <cstdint>

enum class TriggerEventType {
    Pressed,
    Released
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
