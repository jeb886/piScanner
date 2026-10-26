#include "FileFrameSource.h"

#include "Output.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

FileFrameSource::FileFrameSource(const std::vector<std::string>& inputs, const Config& config)
    : paths_(expandImagePaths(inputs)), repeat_(config.repeat)
{
    if (paths_.empty())
        throw std::runtime_error("no image files found in the given --source paths");

    PixelFormat fmt = PixelFormat::GREY;
    if (config.pixfmt == "yuyv")
        fmt = PixelFormat::YUYV;
    else if (config.pixfmt == "mjpeg")
        fmt = PixelFormat::MJPEG;
    else if (config.pixfmt != "auto" && config.pixfmt != "grey")
        throw std::invalid_argument("unsupported --pixfmt for file source: " + config.pixfmt);

    for (const auto& path : paths_) {
        Stored stored;
        const bool jpegIn = [&](const std::string& p) {
            auto ext = p.substr(p.find_last_of('.') + 1);
            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext == "jpg" || ext == "jpeg";
        }(path);
        if (fmt == PixelFormat::MJPEG && jpegIn) {
            stored.data = loadFileBytes(path);
            // dimensions come from the JPEG header via turbojpeg at decode
            // time, but Frame needs them — decode a gray copy to learn them.
            const auto gray = loadImage(path);
            stored.width = gray.width;
            stored.height = gray.height;
            stored.stride = gray.width;
            stored.fmt = PixelFormat::MJPEG;
        } else {
            const auto gray = loadImage(path);
            stored.width = gray.width;
            stored.height = gray.height;
            if (fmt == PixelFormat::MJPEG) {
                stored.data = encodeJpeg(gray, 90);
                stored.stride = gray.width;
                stored.fmt = PixelFormat::MJPEG;
            } else if (fmt == PixelFormat::YUYV) {
                stored.data = packYuyv(gray, 0, stored.stride);
                stored.fmt = PixelFormat::YUYV;
            } else {
                stored.data = gray.pixels;
                stored.stride = gray.width;
                stored.fmt = PixelFormat::GREY;
            }
        }
        images_.push_back(std::move(stored));
    }
    if (config.fps > 0)
        intervalUs_ = 1000000 / static_cast<std::uint64_t>(config.fps);
}

bool FileFrameSource::dequeue(Frame& frame, int& index, int timeoutMs)
{
    const std::uint64_t deadline = monotonicNowUs() + static_cast<std::uint64_t>(timeoutMs) * 1000;
    for (;;) {
        if (next_ >= images_.size()) {
            if (repeat_ > 0 && ++pass_ >= repeat_)
                return false;
            next_ = 0;
        }
        const auto now = monotonicNowUs();
        if (nextFrameUs_ == 0 || now >= nextFrameUs_) {
            nextFrameUs_ = now + intervalUs_;
            break;
        }
        if (now >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(
            std::min(nextFrameUs_ - now, deadline - now)));
    }
    const Stored& stored = images_[next_];
    index = static_cast<int>(next_);
    ++next_;
    frame.data = stored.data.data();
    frame.bytes = stored.data.size();
    frame.width = stored.width;
    frame.height = stored.height;
    frame.stride = stored.stride;
    frame.fmt = stored.fmt;
    frame.timestampUs = monotonicNowUs();
    frame.sequence = sequence_++;
    return true;
}

void FileFrameSource::requeue(int)
{
    // File buffers are immutable and shared; nothing to return.
}

bool FileFrameSource::exhausted() const
{
    return repeat_ > 0 && next_ >= images_.size() && pass_ + 1 >= repeat_;
}
