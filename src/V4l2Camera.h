#pragma once

#include "Config.h"
#include "Frame.h"

#include <linux/videodev2.h>

#include <string>
#include <vector>

class V4l2Camera {
public:
    explicit V4l2Camera(const Config& config);
    ~V4l2Camera();
    V4l2Camera(const V4l2Camera&) = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;

    bool dequeue(Frame& frame, int& index, int timeoutMs);
    void requeue(int index);
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
