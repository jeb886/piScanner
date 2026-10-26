#pragma once

#include "Config.h"
#include "Frame.h"

#include <linux/videodev2.h>

#include <functional>
#include <string>
#include <vector>

// Chooses a fourcc pixel format for `config` among `supported`.
// `sizeSupport` / `fpsSupport` return 1 when the format can deliver the
// requested size / frame rate, 0 when it cannot, -1 when unknown.
// In "auto" mode formats are preferred in order GREY, YUYV, MJPEG, but a
// format that cannot reach the requested fps falls behind one that can.
// Returns 0 when nothing works.
std::uint32_t choosePixelFormat(const Config& config, const std::vector<std::uint32_t>& supported,
                                const std::function<int(std::uint32_t)>& sizeSupport,
                                const std::function<int(std::uint32_t)>& fpsSupport);

class V4l2Camera : public FrameSource {
public:
    explicit V4l2Camera(const Config& config);
    ~V4l2Camera();
    V4l2Camera(const V4l2Camera&) = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;

    bool dequeue(Frame& frame, int& index, int timeoutMs) override;
    void requeue(int index) override;
    static void listDevices();

private:
    struct Buffer {
        void* address = nullptr;
        std::size_t length = 0;
    };

    int fd_ = -1;
    std::vector<Buffer> buffers_;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    PixelFormat format_ = PixelFormat::GREY;
    bool streaming_ = false;

    void setControl(std::uint32_t id, int value, const char* name);
    void cleanup();
};
