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

// Frame source abstraction: a real V4L2 camera or a file-backed mock used
// for testing. dequeue() blocks up to timeoutMs; returns false on timeout
// or end of stream. requeue() returns a buffer to the pool (a no-op for
// immutable file buffers). exhausted() is true once no more frames will
// ever be produced.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool dequeue(Frame& frame, int& index, int timeoutMs) = 0;
    virtual void requeue(int index) = 0;
    virtual bool exhausted() const { return false; }
    // True when the source should pause producing frames while no scan
    // session is armed (file sources pause so each press reads the next
    // image; a live camera must keep draining its driver buffers).
    virtual bool waitsWhileDisarmed() const { return false; }
};
