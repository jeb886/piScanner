#include "Capture.h"

#include "FileFrameSource.h"
#include "Frame.h"
#include "Log.h"
#include "V4l2Camera.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef PISCANNER_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string lowerExtension(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = path.substr(dot);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

const char* formatName(PixelFormat fmt)
{
    switch (fmt) {
    case PixelFormat::YUYV:
        return "yuyv";
    case PixelFormat::MJPEG:
        return "mjpeg";
    case PixelFormat::GREY:
        break;
    }
    return "grey";
}

// Copies one plane out of a strided frame into tightly packed rows.
std::vector<std::uint8_t> copyRows(const Frame& frame, int bytesPerPixel)
{
    const int rowBytes = frame.width * bytesPerPixel;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(rowBytes) * frame.height);
    for (int y = 0; y < frame.height; ++y)
        std::copy_n(frame.data + static_cast<std::size_t>(y) * frame.stride, rowBytes,
                    out.data() + static_cast<std::size_t>(y) * rowBytes);
    return out;
}

// BT.601 full-range YUV -> RGB, fixed point (coeffs * 256). YUYV packs
// Y0 U Y1 V per pixel pair, so each 4-byte group shares one chroma pair.
std::vector<std::uint8_t> yuyvToRgb(const Frame& frame)
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(frame.width) * frame.height * 3);
    for (int y = 0; y < frame.height; ++y) {
        const auto* row = frame.data + static_cast<std::size_t>(y) * frame.stride;
        for (int x = 0; x < frame.width; x += 2) {
            const int luma0 = row[x * 2];
            const int u = row[x * 2 + 1];
            const bool second = x + 1 < frame.width;
            const int luma1 = second ? row[x * 2 + 2] : luma0;
            const int v = second ? row[x * 2 + 3] : u;
            const int d = u - 128;
            const int e = v - 128;
            const int dr = (359 * e + 128) >> 8;              // 1.402 * (v - 128)
            const int dg = (-(88 * d) - (183 * e) + 128) >> 8; // -0.344u - 0.714v
            const int db = (454 * d + 128) >> 8;              // 1.772 * (u - 128)
            for (int px = 0; px < 2 && x + px < frame.width; ++px) {
                const int luma = px == 0 ? luma0 : luma1;
                auto* dst = rgb.data() + (static_cast<std::size_t>(y) * frame.width + x + px) * 3;
                dst[0] = static_cast<std::uint8_t>(std::clamp(luma + dr, 0, 255));
                dst[1] = static_cast<std::uint8_t>(std::clamp(luma + dg, 0, 255));
                dst[2] = static_cast<std::uint8_t>(std::clamp(luma + db, 0, 255));
            }
        }
    }
    return rgb;
}

#ifdef PISCANNER_HAVE_TURBOJPEG
std::vector<std::uint8_t> mjpegToRgb(const Frame& frame, int& width, int& height)
{
    tjhandle handle = tjInitDecompress();
    if (handle == nullptr)
        throw std::runtime_error("failed to initialize libturbojpeg");
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(handle, frame.data, static_cast<unsigned long>(frame.bytes), &width, &height,
                            &subsamp, &colorspace) != 0) {
        const std::string reason = tjGetErrorStr();
        tjDestroy(handle);
        throw std::runtime_error("cannot read JPEG frame header: " + reason);
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    if (tjDecompress2(handle, frame.data, static_cast<unsigned long>(frame.bytes), rgb.data(), width, 0, height,
                      TJPF_RGB, TJFLAG_FASTDCT) != 0) {
        const std::string reason = tjGetErrorStr();
        tjDestroy(handle);
        throw std::runtime_error("JPEG decompress failed: " + reason);
    }
    tjDestroy(handle);
    return rgb;
}
#endif

void writeBytes(const std::string& path, const std::uint8_t* data, std::size_t size)
{
    std::ofstream output(path, std::ios::binary);
    if (!output || !output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot write image file: " + path);
}

void writeImage(const std::string& path, const std::string& ext, const std::uint8_t* pixels, int width,
                int height, int comp)
{
    int ok = 0;
    if (ext == ".png")
        ok = stbi_write_png(path.c_str(), width, height, comp, pixels, width * comp);
    else if (ext == ".bmp")
        ok = stbi_write_bmp(path.c_str(), width, height, comp, pixels);
    else if (ext == ".tga")
        ok = stbi_write_tga(path.c_str(), width, height, comp, pixels);
    else
        ok = stbi_write_jpg(path.c_str(), width, height, comp, pixels, 90);
    if (!ok)
        throw std::runtime_error("cannot write image file: " + path);
}

} // namespace

int runCapture(const Config& config, const std::string& outPath)
{
    const std::string ext = lowerExtension(outPath);
    if (ext != ".bmp" && ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga")
        throw std::runtime_error("unsupported output extension (use .bmp, .png, .jpg, or .tga): " + outPath);

    std::unique_ptr<FrameSource> source;
    if (config.source.empty())
        source = std::make_unique<V4l2Camera>(config);
    else
        source = std::make_unique<FileFrameSource>(config.source, config);

    // Discard --warmup frames so auto-exposure can settle, then keep one.
    // Poll retries cover paced file sources and cameras slow to deliver the
    // first frame; the deadline scales with warmup/fps plus a fixed grace.
    const auto graceMs = 5000 + static_cast<std::int64_t>(config.warmup) * 2000 / std::max(1, config.fps);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    Frame frame;
    int index = -1;
    for (int remaining = config.warmup + 1; remaining > 0;) {
        if (source->dequeue(frame, index, 200)) {
            if (--remaining == 0)
                break;
            source->requeue(index);
            continue;
        }
        if (source->exhausted())
            throw std::runtime_error("frame source produced fewer images than --warmup + 1");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("timed out waiting for a camera frame");
    }

    const bool jpegPassthrough = frame.fmt == PixelFormat::MJPEG && (ext == ".jpg" || ext == ".jpeg");
    std::vector<std::uint8_t> pixels;
    int width = frame.width;
    int height = frame.height;
    int comp = 0;
    if (jpegPassthrough) {
        // Zero work: an MJPEG frame payload already is a JPEG file.
        writeBytes(outPath, frame.data, frame.bytes);
    } else if (frame.fmt == PixelFormat::GREY) {
        comp = 1;
        pixels = copyRows(frame, 1);
    } else if (frame.fmt == PixelFormat::YUYV) {
        comp = 3;
        pixels = yuyvToRgb(frame);
    } else {
#ifdef PISCANNER_HAVE_TURBOJPEG
        comp = 3;
        pixels = mjpegToRgb(frame, width, height);
#else
        throw std::runtime_error("converting an MJPEG frame to " + ext +
                                 " needs libturbojpeg; capture to .jpg instead");
#endif
    }
    source->requeue(index);

    if (!jpegPassthrough)
        writeImage(outPath, ext, pixels.data(), width, height, comp);

    const std::string size = std::to_string(width) + "x" + std::to_string(height);
    std::cout << "wrote " << outPath << ' ' << size << ' ' << formatName(frame.fmt) << '\n';
    if (logEnabled())
        logLine("capture " + outPath + " " + size + " " + formatName(frame.fmt));
    return 0;
}
