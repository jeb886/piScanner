#pragma once

#include "Decoder.h"

#include <cstdint>
#include <iostream>
#include <mutex>
#include <memory>

class ResultSink {
public:
    virtual ~ResultSink() = default;
    virtual void onResult(const ScanResult& result) = 0;
    virtual void onTimeout(std::uint64_t timestampUs) = 0;
};

class TextSink final : public ResultSink {
public:
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;

private:
    std::mutex mutex_;
};

class JsonSink final : public ResultSink {
public:
    explicit JsonSink(bool triggerMode);
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;

private:
    bool triggerMode_;
    std::mutex mutex_;
};

// Writes each decoded text to a USB HID keyboard gadget endpoint
// (/dev/hidg0) so the Pi types the barcode into whatever host it is
// plugged into, followed by Enter. US-QWERTY layout.
class HidSink final : public ResultSink {
public:
    explicit HidSink(const std::string& device);
    ~HidSink() override;
    HidSink(const HidSink&) = delete;
    HidSink& operator=(const HidSink&) = delete;
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;

private:
    int fd_ = -1;
    std::mutex mutex_;
};

class GpioBeeper;

// Decorates another sink: forwards the result and pulses the beeper.
class BeepSink final : public ResultSink {
public:
    BeepSink(ResultSink& inner, GpioBeeper& beeper) : inner_(inner), beeper_(beeper) {}
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;

private:
    ResultSink& inner_;
    GpioBeeper& beeper_;
};

std::string jsonEscape(const std::string& input);
std::uint64_t monotonicNowUs();
