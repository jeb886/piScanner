#include "V4l2Camera.h"

#include "Output.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <time.h>

namespace {

int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

[[noreturn]] void throwErrno(const std::string& action)
{
    throw std::runtime_error(action + ": " + std::strerror(errno));
}

bool parseVideoNumber(const std::string& path)
{
    if (path.size() < 11 || path.compare(0, 10, "/dev/video") != 0)
        return false;
    return std::all_of(path.begin() + 10, path.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::uint64_t monotonicUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1000;
}

std::uint32_t fourcc(const std::string& format)
{
    if (format == "yuyv")
        return V4L2_PIX_FMT_YUYV;
    if (format == "grey")
        return V4L2_PIX_FMT_GREY;
    if (format == "mjpeg")
        return V4L2_PIX_FMT_MJPEG;
    return 0;
}

std::string formatName(std::uint32_t format)
{
    if (format == V4L2_PIX_FMT_YUYV)
        return "yuyv";
    if (format == V4L2_PIX_FMT_GREY)
        return "grey";
    if (format == V4L2_PIX_FMT_MJPEG)
        return "mjpeg";
    char text[5] = {static_cast<char>(format & 0xff), static_cast<char>((format >> 8) & 0xff),
                    static_cast<char>((format >> 16) & 0xff), static_cast<char>((format >> 24) & 0xff), '\0'};
    return text;
}

int requestedSizeSupport(int fd, std::uint32_t pixelFormat, std::uint32_t width, std::uint32_t height)
{
    bool foundUsableSize = false;
    for (std::uint32_t index = 0;; ++index) {
        v4l2_frmsizeenum size{};
        size.index = index;
        size.pixel_format = pixelFormat;
        if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0)
            return foundUsableSize ? 0 : -1;
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            foundUsableSize = true;
            if (size.discrete.width == width && size.discrete.height == height)
                return 1;
        } else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            foundUsableSize = true;
            if (width >= size.stepwise.min_width && width <= size.stepwise.max_width &&
                height >= size.stepwise.min_height && height <= size.stepwise.max_height)
                return 1;
        } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            const auto& stepwise = size.stepwise;
            foundUsableSize = true;
            if (stepwise.step_width != 0 && stepwise.step_height != 0 && width >= stepwise.min_width &&
                width <= stepwise.max_width && height >= stepwise.min_height && height <= stepwise.max_height &&
                (width - stepwise.min_width) % stepwise.step_width == 0 &&
                (height - stepwise.min_height) % stepwise.step_height == 0)
                return 1;
        }
    }
}

// 1 when the format can deliver at least `fps` frames/sec at the size, 0 when
// it cannot, -1 when the camera does not report frame intervals.
int requestedFpsSupport(int fd, std::uint32_t pixelFormat, std::uint32_t width, std::uint32_t height, int fps)
{
    bool foundUsableInterval = false;
    for (std::uint32_t index = 0;; ++index) {
        v4l2_frmivalenum interval{};
        interval.index = index;
        interval.pixel_format = pixelFormat;
        interval.width = width;
        interval.height = height;
        if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0)
            return foundUsableInterval ? 0 : -1;
        double maxFps = 0;
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            foundUsableInterval = true;
            if (interval.discrete.numerator != 0)
                maxFps = static_cast<double>(interval.discrete.denominator) / interval.discrete.numerator;
        } else if (interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
                   interval.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            foundUsableInterval = true;
            if (interval.stepwise.min.numerator != 0)
                maxFps = static_cast<double>(interval.stepwise.min.denominator) / interval.stepwise.min.numerator;
        }
        if (foundUsableInterval && maxFps >= fps)
            return 1;
    }
}

}

std::uint32_t choosePixelFormat(const Config& config, const std::vector<std::uint32_t>& supported,
                                const std::function<int(std::uint32_t)>& sizeSupport,
                                const std::function<int(std::uint32_t)>& fpsSupport)
{
    std::vector<std::uint32_t> candidates;
    if (config.pixfmt == "auto")
        candidates = {V4L2_PIX_FMT_GREY, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG};
    else
        candidates = {fourcc(config.pixfmt)};

    std::uint32_t sizeFallback = 0;
    std::uint32_t unknownFallback = 0;
    for (const auto candidate : candidates) {
        if (std::find(supported.begin(), supported.end(), candidate) == supported.end())
            continue;
        if (config.pixfmt != "auto")
            return candidate;
        const int size = sizeSupport(candidate);
        if (size < 0 && unknownFallback == 0)
            unknownFallback = candidate;
        if (size <= 0)
            continue;
        if (sizeFallback == 0)
            sizeFallback = candidate;
        // Prefer the first format that can actually reach the requested
        // frame rate: raw YUYV is fine at small sizes but cannot keep up
        // with high-resolution/high-fps capture, where MJPEG wins.
        if (fpsSupport(candidate) > 0)
            return candidate;
    }
    return sizeFallback != 0 ? sizeFallback : unknownFallback;
}

V4l2Camera::V4l2Camera(const Config& config)
{
    fd_ = open(config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0)
        throwErrno("cannot open camera " + config.device);

    try {
        v4l2_capability capability{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0)
            throwErrno("VIDIOC_QUERYCAP failed for " + config.device);
        const std::uint32_t caps =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 || (caps & V4L2_CAP_STREAMING) == 0)
            throw std::runtime_error(config.device + " is not a streaming video capture device");

        std::vector<std::uint32_t> supported;
        for (std::uint32_t index = 0;; ++index) {
            v4l2_fmtdesc description{};
            description.index = index;
            description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(fd_, VIDIOC_ENUM_FMT, &description) < 0)
                break;
            supported.push_back(description.pixelformat);
        }

#ifndef PISCANNER_HAVE_TURBOJPEG
        supported.erase(std::remove(supported.begin(), supported.end(), V4L2_PIX_FMT_MJPEG), supported.end());
#endif
        const auto width = static_cast<std::uint32_t>(config.width);
        const auto height = static_cast<std::uint32_t>(config.height);
        const std::uint32_t selected = choosePixelFormat(
            config, supported,
            [&](std::uint32_t fmt) { return requestedSizeSupport(fd_, fmt, width, height); },
            [&](std::uint32_t fmt) { return requestedFpsSupport(fd_, fmt, width, height, config.fps); });
        if (selected == 0) {
            if (config.pixfmt == "auto")
                throw std::runtime_error("camera has no supported GREY, YUYV, or MJPEG format at the requested size");
            throw std::runtime_error("camera does not support pixel format " + config.pixfmt);
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<std::uint32_t>(config.width);
        format.fmt.pix.height = static_cast<std::uint32_t>(config.height);
        format.fmt.pix.pixelformat = selected;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0)
            throwErrno("VIDIOC_S_FMT failed");
        width_ = static_cast<int>(format.fmt.pix.width);
        height_ = static_cast<int>(format.fmt.pix.height);
        stride_ = static_cast<int>(format.fmt.pix.bytesperline);
        if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
            format_ = PixelFormat::YUYV;
        else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY)
            format_ = PixelFormat::GREY;
        else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG)
            format_ = PixelFormat::MJPEG;
        else
            throw std::runtime_error("camera negotiated unsupported pixel format " + formatName(format.fmt.pix.pixelformat));
        if (stride_ <= 0)
            stride_ = width_ * (format_ == PixelFormat::YUYV ? 2 : 1);
        std::cerr << "Camera: " << config.device << " " << width_ << "x" << height_ << " "
                  << formatName(format.fmt.pix.pixelformat) << '\n';

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parameters.parm.capture.timeperframe.numerator = 1;
        parameters.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(config.fps);
        if (xioctl(fd_, VIDIOC_S_PARM, &parameters) < 0 && config.verbose)
            std::cerr << "warning: camera did not accept requested frame rate\n";

        v4l2_requestbuffers request{};
        request.count = static_cast<std::uint32_t>(config.buffers);
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0)
            throwErrno("VIDIOC_REQBUFS failed");
        if (request.count < static_cast<std::uint32_t>(config.buffers))
            throw std::runtime_error("camera allocated fewer buffers than requested");

        buffers_.resize(request.count);
        for (std::uint32_t i = 0; i < request.count; ++i) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0)
                throwErrno("VIDIOC_QUERYBUF failed");
            buffers_[i].length = buffer.length;
            buffers_[i].address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
            if (buffers_[i].address == MAP_FAILED)
                throwErrno("mmap camera buffer failed");
            if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0)
                throwErrno("VIDIOC_QBUF failed");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
            throwErrno("VIDIOC_STREAMON failed");
        streaming_ = true;

        if (config.exposure) {
            setControl(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL, "manual exposure mode");
            setControl(V4L2_CID_EXPOSURE_ABSOLUTE, *config.exposure, "exposure");
        }
        if (config.focus) {
            setControl(V4L2_CID_FOCUS_AUTO, 0, "autofocus disable");
            setControl(V4L2_CID_FOCUS_ABSOLUTE, *config.focus, "focus");
        }
        if (config.gain)
            setControl(V4L2_CID_GAIN, *config.gain, "gain");
    } catch (...) {
        cleanup();
        throw;
    }
}

V4l2Camera::~V4l2Camera()
{
    cleanup();
}

void V4l2Camera::cleanup()
{
    if (fd_ >= 0 && streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (const auto& buffer : buffers_) {
        if (buffer.address != nullptr && buffer.address != MAP_FAILED)
            munmap(buffer.address, buffer.length);
    }
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
    streaming_ = false;
    buffers_.clear();
}

void V4l2Camera::setControl(std::uint32_t id, int value, const char* name)
{
    v4l2_control control{};
    control.id = id;
    control.value = value;
    if (xioctl(fd_, VIDIOC_S_CTRL, &control) < 0)
        std::cerr << "warning: camera does not support " << name << " control: " << std::strerror(errno) << '\n';
}

bool V4l2Camera::dequeue(Frame& frame, int& index, int timeoutMs)
{
    pollfd descriptor{fd_, POLLIN | POLLPRI, 0};
    const int ready = poll(&descriptor, 1, timeoutMs);
    if (ready < 0) {
        if (errno == EINTR)
            return false;
        throwErrno("poll camera failed");
    }
    if (ready == 0)
        return false;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        throw std::runtime_error("camera poll reported an error");

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN)
            return false;
        throwErrno("VIDIOC_DQBUF failed");
    }
    if (buffer.index >= buffers_.size())
        throw std::runtime_error("camera returned an invalid buffer index");
    index = static_cast<int>(buffer.index);
    frame.data = static_cast<const std::uint8_t*>(buffers_[buffer.index].address);
    frame.bytes = buffer.bytesused;
    frame.width = width_;
    frame.height = height_;
    frame.stride = stride_;
    frame.fmt = format_;
    frame.sequence = buffer.sequence;
    if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
        frame.timestampUs = static_cast<std::uint64_t>(buffer.timestamp.tv_sec) * 1000000 +
                            static_cast<std::uint64_t>(buffer.timestamp.tv_usec);
    else
        frame.timestampUs = monotonicUs();
    return true;
}

void V4l2Camera::requeue(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= buffers_.size())
        throw std::out_of_range("invalid camera buffer index");
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = static_cast<std::uint32_t>(index);
    if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0)
        throwErrno("VIDIOC_QBUF failed");
}

void V4l2Camera::listDevices()
{
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const auto path = entry.path().string();
        if (parseVideoNumber(path))
            paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    bool found = false;
    for (const auto& path : paths) {
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        v4l2_capability capability{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
            close(fd);
            continue;
        }
        const std::uint32_t caps =
            (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
            close(fd);
            continue;
        }
        found = true;
        std::cout << path << ": " << capability.card << " (" << capability.bus_info << ")\n";
        for (std::uint32_t i = 0;; ++i) {
            v4l2_fmtdesc format{};
            format.index = i;
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(fd, VIDIOC_ENUM_FMT, &format) < 0)
                break;
            std::cout << "  " << formatName(format.pixelformat) << ":";
            for (std::uint32_t sizeIndex = 0;; ++sizeIndex) {
                v4l2_frmsizeenum size{};
                size.index = sizeIndex;
                size.pixel_format = format.pixelformat;
                if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0)
                    break;
                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                    std::cout << ' ' << size.discrete.width << 'x' << size.discrete.height;
                else if (sizeIndex == 0)
                    std::cout << " continuous/stepwise";
            }
            std::cout << '\n';
        }
        close(fd);
    }
    if (!found)
        std::cout << "no capture devices found\n";
}
