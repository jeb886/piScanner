#pragma once

#include "Config.h"
#include "Trigger.h"

#include <string>

class GpioTrigger final : public Trigger {
public:
    explicit GpioTrigger(const Config& config);
    ~GpioTrigger() override;
    bool wait(int timeoutMs, TriggerEvent& event) override;

private:
    int fd_ = -1;
    bool activeLow_ = true;
    bool initialEvent_ = false;
    TriggerEvent initial_{};
};

class StdinTrigger final : public Trigger {
public:
    bool wait(int timeoutMs, TriggerEvent& event) override;
};

std::string findGpioChip();
