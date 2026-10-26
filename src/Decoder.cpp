#include "Decoder.h"

#include <Barcode.h>
#include <BarcodeFormat.h>
#include <ReadBarcode.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

#ifdef PISCAN_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

Decoder::Decoder(const Config& config) : roi_(config.roi)
{
    if (!config.formats.empty())
        options_.formats(ZXing::BarcodeFormatsFromString(config.formats));
    options_.setTryHarder(config.tryHarder)
        .setTryRotate(config.tryRotate)
        .setTryDownscale(config.tryDownscale)
        .setMaxNumberOfSymbols(static_cast<std::uint8_t>(config.maxSymbols));
    if (config.binarizer == "global")
        options_.binarizer(ZXing::Binarizer::GlobalHistogram);
    else if (config.binarizer == "local")
        options_.binarizer(ZXing::Binarizer::LocalAverage);
    else if (config.binarizer == "bool")
        options_.binarizer(ZXing::Binarizer::BoolCast);
    else if (config.binarizer == "fixed")
        options_.binarizer(ZXing::Binarizer::FixedThreshold);
    else
        throw std::invalid_argument("unknown binarizer: " + config.binarizer);

#ifdef PISCAN_HAVE_TURBOJPEG
    turboJpeg_ = tjInitDecompress();
    if (turboJpeg_ == nullptr)
        throw std::runtime_error("failed to initialize libturbojpeg");
#endif
}

Decoder::~Decoder()
{
#ifdef PISCAN_HAVE_TURBOJPEG
    if (turboJpeg_ != nullptr)
        tjDestroy(static_cast<tjhandle>(turboJpeg_));
#endif
}

ZXing::ImageView Decoder::toLuma(const Frame& frame)
{
    if (frame.data == nullptr || frame.width <= 0 || frame.height <= 0)
        throw std::invalid_argument("invalid frame");
    if (frame.fmt == PixelFormat::YUYV) {
        if (frame.stride < frame.width * 2 ||
            frame.bytes < static_cast<std::size_t>(frame.stride) * (frame.height - 1) +
                              static_cast<std::size_t>(frame.width) * 2)
            throw std::invalid_argument("YUYV frame is smaller than its dimensions");
        return {frame.data, frame.width, frame.height, ZXing::ImageFormat::Lum, frame.stride, 2};
    }
    if (frame.fmt == PixelFormat::GREY) {
        if (frame.stride < frame.width ||
            frame.bytes < static_cast<std::size_t>(frame.stride) * (frame.height - 1) + frame.width)
            throw std::invalid_argument("GREY frame is smaller than its dimensions");
        return {frame.data, frame.width, frame.height, ZXing::ImageFormat::Lum, frame.stride, 1};
    }
#ifdef PISCAN_HAVE_TURBOJPEG
    if (frame.fmt == PixelFormat::MJPEG) {
        if (frame.bytes == 0)
            throw std::invalid_argument("MJPEG frame is empty");
        lumaBuffer_.resize(static_cast<std::size_t>(frame.width) * frame.height);
        const int rc = tjDecompress2(static_cast<tjhandle>(turboJpeg_),
                                     const_cast<unsigned char*>(frame.data),
                                     static_cast<unsigned long>(frame.bytes), lumaBuffer_.data(), frame.width,
                                     frame.width, frame.height, TJPF_GRAY, TJFLAG_FASTDCT);
        if (rc != 0)
            throw std::runtime_error(std::string("MJPEG decode failed: ") + tjGetErrorStr());
        return {lumaBuffer_.data(), frame.width, frame.height, ZXing::ImageFormat::Lum};
    }
#endif
    throw std::runtime_error("MJPEG support is not available in this build");
}

std::vector<ScanResult> Decoder::decode(const Frame& frame)
{
    auto image = toLuma(frame);
    int roiX = 0;
    int roiY = 0;
    if (roi_) {
        roiX = std::clamp(static_cast<int>(roi_->x * frame.width), 0, frame.width - 1);
        roiY = std::clamp(static_cast<int>(roi_->y * frame.height), 0, frame.height - 1);
        const int width = std::clamp(static_cast<int>(roi_->width * frame.width), 1, frame.width - roiX);
        const int height = std::clamp(static_cast<int>(roi_->height * frame.height), 1, frame.height - roiY);
        image = image.cropped(roiX, roiY, width, height);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto barcodes = ZXing::ReadBarcodes(image, options_);
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    std::vector<ScanResult> results;
    for (const auto& barcode : barcodes) {
        if (!barcode.isValid())
            continue;
        const auto& position = barcode.position();
        const std::array points{position.topLeft(), position.topRight(), position.bottomRight(), position.bottomLeft()};
        ScanResult result;
        result.format = ZXing::ToString(barcode.format());
        result.text = barcode.text();
        result.frameTimestampUs = frame.timestampUs;
        result.decodeMs = elapsed;
        for (std::size_t i = 0; i < points.size(); ++i)
            result.position[i] = {points[i].x + roiX, points[i].y + roiY};
        results.push_back(std::move(result));
    }
    return results;
}
