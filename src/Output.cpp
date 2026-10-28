#include "Output.h"

#include "Log.h"
#include "PwmBeeper.h"
#include "HidKeys.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unistd.h>
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

namespace {

// 6-key-roll-over boot-protocol keyboard report: modifiers, reserved, keys.
void sendReport(int fd, std::uint8_t modifier, std::uint8_t keycode)
{
    const std::uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    if (write(fd, report, sizeof(report)) != static_cast<ssize_t>(sizeof(report)))
        throw std::system_error(errno, std::generic_category(), "write to HID gadget failed");
}

void typeChar(int fd, char ch)
{
    std::uint8_t keycode = 0;
    std::uint8_t modifier = 0;
    if (!hidKeyForChar(static_cast<unsigned char>(ch), keycode, modifier))
        return; // untypeable character: skip it
    sendReport(fd, modifier, keycode);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sendReport(fd, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

}

HidSink::HidSink(const std::string& device)
{
    // /dev/hidg0 appears only after the gadget service enumerates it; the
    // piScanner unit can race it on boot, so wait a while before failing.
    for (int attempt = 0; attempt < 60; ++attempt) {
        fd_ = open(device.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd_ >= 0)
            return;
        if (errno != ENOENT && errno != EACCES && errno != ENODEV)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    throw std::runtime_error("cannot open " + device + " for HID output (is the USB gadget configured?)");
}

HidSink::~HidSink()
{
    if (fd_ >= 0)
        close(fd_);
}

namespace {

std::string logSafeText(const std::string& text)
{
    std::string clean = text.substr(0, 160);
    for (auto& ch : clean) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f)
            ch = '?';
    }
    return clean;
}

}

void LogSink::onResult(const ScanResult& result)
{
    // Headline time is end-to-end readiness: trigger press (or frame
    // capture when untriggered) until the decoded string reaches the
    // sink. zxing= is the library decode time inside that span.
    const auto now = monotonicNowUs();
    const auto baseUs = result.triggerTimestampUs != 0 ? result.triggerTimestampUs : result.frameTimestampUs;
    const double readyMs = now > baseUs ? static_cast<double>(now - baseUs) / 1000.0 : 0.0;
    std::ostringstream line;
    line << "decode " << result.format << ' ' << std::fixed << std::setprecision(1) << readyMs << "ms \""
         << logSafeText(result.text) << "\" zxing=" << result.decodeMs << "ms";
    logLine(line.str());
    inner_.onResult(result);
}

void LogSink::onTimeout(std::uint64_t timestampUs)
{
    logLine("scan timeout");
    inner_.onTimeout(timestampUs);
}

void BeepSink::onResult(const ScanResult& result)
{
    inner_.onResult(result);
    beeper_.beep();
}

void BeepSink::onTimeout(std::uint64_t timestampUs)
{
    inner_.onTimeout(timestampUs);
}

void HidSink::onResult(const ScanResult& result)
{
    std::lock_guard lock(mutex_);
    for (const char ch : result.text)
        typeChar(fd_, ch);
    typeChar(fd_, '\n');
}

void HidSink::onTimeout(std::uint64_t)
{
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
