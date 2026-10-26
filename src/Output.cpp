#include "Output.h"

#include <iomanip>
#include <sstream>
#include <time.h>

std::uint64_t monotonicNowUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1000;
}

std::string jsonEscape(const std::string& input)
{
    std::ostringstream out;
    for (const unsigned char ch : input) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

void TextSink::onResult(const ScanResult& result)
{
    std::lock_guard lock(mutex_);
    std::cout << result.text << '\n' << std::flush;
}

void TextSink::onTimeout(std::uint64_t)
{
    std::lock_guard lock(mutex_);
    std::cout << "timeout\n" << std::flush;
}

JsonSink::JsonSink(bool triggerMode) : triggerMode_(triggerMode) {}

void JsonSink::onResult(const ScanResult& result)
{
    std::lock_guard lock(mutex_);
    const auto now = monotonicNowUs();
    const auto latency = now >= result.frameTimestampUs ? now - result.frameTimestampUs : 0;
    std::cout << "{\"format\":\"" << jsonEscape(result.format) << "\",\"text\":\"" << jsonEscape(result.text)
              << "\",\"decode_ms\":" << std::fixed << std::setprecision(3) << result.decodeMs
              << ",\"latency_ms\":" << static_cast<double>(latency) / 1000.0;
    if (triggerMode_) {
        const auto elapsed = now >= result.triggerTimestampUs ? now - result.triggerTimestampUs : 0;
        std::cout << ",\"trigger_to_result_ms\":" << static_cast<double>(elapsed) / 1000.0;
    }
    std::cout << ",\"position\":[";
    for (std::size_t i = 0; i < result.position.size(); ++i) {
        if (i != 0)
            std::cout << ',';
        std::cout << '[' << result.position[i].x << ',' << result.position[i].y << ']';
    }
    std::cout << "]}\n" << std::flush;
}

void JsonSink::onTimeout(std::uint64_t timestampUs)
{
    std::lock_guard lock(mutex_);
    std::cout << "{\"event\":\"timeout\",\"timestamp_us\":" << timestampUs << "}\n" << std::flush;
}
