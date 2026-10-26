#pragma once

#include <cstddef>
#include <cstdint>

enum class PixelFormat {
    YUYV,
    GREY,
    MJPEG
};

struct Frame {
    const std::uint8_t* data = nullptr;
    std::size_t bytes = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat fmt = PixelFormat::GREY;
    std::uint64_t timestampUs = 0;
    std::uint32_t sequence = 0;
};
