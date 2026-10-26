#pragma once

#include "Decoder.h"

#include <cstdint>
#include <iostream>
#include <mutex>

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

std::string jsonEscape(const std::string& input);
std::uint64_t monotonicNowUs();
