#pragma once

#include "Config.h"
#include "Frame.h"
#include "ImageFile.h"

#include <string>
#include <vector>

// Mock camera: serves frames built from image files on disk instead of a
// V4L2 device. Frames are handed out in the order the paths were given
// (sorted) and repeat `config.repeat` times total (0 = forever). Frame
// payloads are immutable, so requeue() is a no-op and dequeue() never
// blocks past the timeout once the list is drained.
class FileFrameSource : public FrameSource {
public:
    // inputs: image files and/or directories (expanded + sorted).
    // pixfmt: requested PixelFormat — GREY/YUYV frames are packed from the
    // decoded grayscale image; MJPEG keeps/compresses JPEG payloads.
    explicit FileFrameSource(const std::vector<std::string>& inputs, const Config& config);

    bool dequeue(Frame& frame, int& index, int timeoutMs) override;
    void requeue(int index) override;
    bool exhausted() const override;
    bool waitsWhileDisarmed() const override { return true; }

    const std::vector<std::string>& paths() const { return paths_; }

private:
    struct Stored {
        std::vector<std::uint8_t> data;
        int width = 0;
        int height = 0;
        int stride = 0;
        PixelFormat fmt = PixelFormat::GREY;
    };

    std::vector<std::string> paths_;
    std::vector<Stored> images_;
    std::size_t next_ = 0;
    int pass_ = 0;
    int repeat_ = 1; // total passes over the list; 0 = infinite
    std::uint32_t sequence_ = 0;
    std::uint64_t nextFrameUs_ = 0; // pacing deadline, 0 = next frame due now
    std::uint64_t intervalUs_ = 0;  // --fps pacing; 0 = unpaced
};
