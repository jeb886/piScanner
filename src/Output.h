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
    // Aux input (e.g. touchscreen capture button): a copy of the latest
    // camera frame is delivered; sinks that don't care ignore it.
    virtual void onAux(const Frame& frame) { (void)frame; }
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
    std::string device_;
    bool disconnected_ = false;
    std::mutex mutex_;
};

// Decorates another sink: appends a one-line entry per result/timeout to
// the file log (a no-op unless --log-file opened successfully).
class LogSink final : public ResultSink {
public:
    explicit LogSink(ResultSink& inner) : inner_(inner) {}
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;
    void onAux(const Frame& frame) override { inner_.onAux(frame); }

private:
    ResultSink& inner_;
};

class PwmBeeper;

// Decorates another sink: forwards the result and plays a decode beep.
class BeepSink final : public ResultSink {
public:
    BeepSink(ResultSink& inner, PwmBeeper& beeper) : inner_(inner), beeper_(beeper) {}
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;
    void onAux(const Frame& frame) override { inner_.onAux(frame); }

private:
    ResultSink& inner_;
    PwmBeeper& beeper_;
};

std::string jsonEscape(const std::string& input);
std::uint64_t monotonicNowUs();
