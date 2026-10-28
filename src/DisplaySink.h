#pragma once

#include "Config.h"
#include "Output.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Drawable target for renderDisplayFrame(): a framebuffer in native layout.
struct DisplaySurface {
    int width = 0;
    int height = 0;
    int strideBytes = 0;   // bytes per row
    int bitsPerPixel = 16; // 16 = RGB565, 32 = XRGB8888
    std::uint8_t* pixels = nullptr;
};

// One past decode shown in the history rows.
struct DisplayEntry {
    std::string format;
    std::string text;
    double readyMs = 0;
};

// Everything the frame renderer needs; the sink owns it under a mutex.
struct DisplayState {
    std::optional<DisplayEntry> last;
    std::deque<DisplayEntry> history; // newest first
    std::size_t historyLimit = 4;
    std::uint64_t results = 0;
    std::uint64_t timeouts = 0;
    bool timedOut = false; // the most recent event was a scan timeout
};

// Repaints the whole screen into `surface`. No fbdev involvement, so tests
// run it on a plain heap buffer; anything outside the surface is clipped.
void renderDisplayFrame(DisplaySurface& surface, const DisplayState& state);

// Decorates another sink: forwards results/timeouts first, then repaints a
// small SPI framebuffer panel (e.g. a 480x320 ILI9486 exposed through DRM
// fbdev emulation) with the latest result and a short history. When the
// panel can't be found or opened the sink warns once and just forwards, so
// a missing display never breaks scanning.
class DisplaySink final : public ResultSink {
public:
    DisplaySink(ResultSink& inner, const Config& config);
    ~DisplaySink() override;
    DisplaySink(const DisplaySink&) = delete;
    DisplaySink& operator=(const DisplaySink&) = delete;
    void onResult(const ScanResult& result) override;
    void onTimeout(std::uint64_t timestampUs) override;

private:
    void open(const Config& config);
    void close() noexcept;
    void paint(); // called with mutex_ held

    ResultSink& inner_;
    int fd_ = -1;
    std::uint8_t* fb_ = nullptr;
    std::size_t fbBytes_ = 0;
    std::vector<std::uint8_t> backing_;
    DisplaySurface surface_{};
    DisplayState state_;
    std::mutex mutex_;
};
