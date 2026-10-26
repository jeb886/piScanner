#pragma once

#include "Config.h"
#include "Frame.h"

#include <ImageView.h>
#include <ReaderOptions.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ScanPoint {
    int x = 0;
    int y = 0;
};

struct ScanResult {
    std::string format;
    std::string text;
    std::array<ScanPoint, 4> position{};
    std::uint64_t frameTimestampUs = 0;
    std::uint64_t triggerTimestampUs = 0;
    double decodeMs = 0;
};

class Decoder {
public:
    explicit Decoder(const Config& config);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    std::vector<ScanResult> decode(const Frame& frame);
    ZXing::ImageView toLuma(const Frame& frame);

private:
    ZXing::ReaderOptions options_;
    std::optional<RegionOfInterest> roi_;
    std::vector<std::uint8_t> lumaBuffer_;
#ifdef PISCAN_HAVE_TURBOJPEG
    void* turboJpeg_ = nullptr;
#endif
};
