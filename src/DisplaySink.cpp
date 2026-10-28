#include "DisplaySink.h"

#include "Log.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "stb_easy_font.h"

namespace {

// stb_easy_font indexes its glyph table with (char - 32); anything outside
// printable ASCII would read out of bounds, so clamp the charset first.
// Barcode payloads can contain control bytes (e.g. GS) or UTF-8.
std::string sanitize(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
        out.push_back(ch >= ' ' && ch <= '~' ? ch : '?');
    return out;
}

constexpr std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

const std::uint16_t kBg = rgb565(0x10, 0x10, 0x18);
const std::uint16_t kHeader = rgb565(0x1a, 0x2a, 0x4a);
const std::uint16_t kText = rgb565(0xe8, 0xe8, 0xe8);
const std::uint16_t kDim = rgb565(0x88, 0x88, 0x90);
const std::uint16_t kGood = rgb565(0x60, 0xe0, 0x60);
const std::uint16_t kWarn = rgb565(0xe0, 0xa0, 0x40);

int xioctl(int fd, unsigned long request, void* argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

void putPixel(DisplaySurface& s, int x, int y, std::uint16_t color)
{
    if (x < 0 || y < 0 || x >= s.width || y >= s.height || s.pixels == nullptr)
        return;
    if (s.bitsPerPixel == 16) {
        auto* dst = reinterpret_cast<std::uint16_t*>(s.pixels + static_cast<std::size_t>(y) * s.strideBytes + x * 2);
        *dst = color;
    } else if (s.bitsPerPixel == 32) {
        const std::uint8_t r = static_cast<std::uint8_t>(((color >> 11) & 0x1f) << 3);
        const std::uint8_t g = static_cast<std::uint8_t>(((color >> 5) & 0x3f) << 2);
        const std::uint8_t b = static_cast<std::uint8_t>((color & 0x1f) << 3);
        auto* dst = s.pixels + static_cast<std::size_t>(y) * s.strideBytes + x * 4;
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = 0xff;
    }
}

void fillRect(DisplaySurface& s, int x, int y, int w, int h, std::uint16_t color)
{
    const int x1 = std::max(0, x);
    const int y1 = std::max(0, y);
    const int x2 = std::min(s.width, x + w);
    const int y2 = std::min(s.height, y + h);
    for (int row = y1; row < y2; ++row)
        for (int col = x1; col < x2; ++col)
            putPixel(s, col, row, color);
}

// stb_easy_font emits 4 vertices per quad: {float x,y,z; u8 color[4]}.
// Quads are axis-aligned, so filling their bounding box draws the stroke.
void drawText(DisplaySurface& s, int x, int y, const std::string& text, std::uint16_t color, int scale)
{
    const std::string clean = sanitize(text);
    if (clean.empty() || scale <= 0)
        return;
    static thread_local std::vector<char> buffer;
    buffer.resize(clean.size() * 1024 + 64); // quads are 64 bytes each
    const int quads = stb_easy_font_print(0, 0, const_cast<char*>(clean.c_str()), nullptr, buffer.data(),
                                          static_cast<int>(buffer.size()));
    const auto* verts = reinterpret_cast<const float*>(buffer.data());
    for (int q = 0; q < quads; ++q) {
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int v = 0; v < 4; ++v) {
            const std::size_t o = (static_cast<std::size_t>(q) * 4 + v) * 4; // floats per vertex
            minX = std::min(minX, verts[o]);
            maxX = std::max(maxX, verts[o]);
            minY = std::min(minY, verts[o + 1]);
            maxY = std::max(maxY, verts[o + 1]);
        }
        fillRect(s, x + static_cast<int>(minX * scale), y + static_cast<int>(minY * scale),
                 std::max(1, static_cast<int>((maxX - minX) * scale)),
                 std::max(1, static_cast<int>((maxY - minY) * scale)), color);
    }
}

int textCols(const DisplaySurface& s, int scale)
{
    // stb_easy_font advances ~8px per glyph at scale 1 on average; keep a
    // 6px margin on each side.
    return std::max(1, (s.width - 12) / (8 * scale));
}

std::string clipped(const std::string& text, int cols)
{
    if (static_cast<int>(text.size()) <= cols)
        return text;
    return text.substr(0, static_cast<std::size_t>(std::max(0, cols - 1))) + "~";
}

// Picks the panel's fb device for --display auto. With exactly one
// framebuffer present that is the answer; with several (e.g. an HDMI
// console next to the SPI panel) the first entry whose driver name isn't
// the vc4/console framebuffer wins. Returns "" when nothing plausible
// exists.
std::string resolveFbPath(const std::string& device)
{
    if (device != "auto")
        return device;
    std::vector<std::string> fbs;
    std::vector<std::string> panels;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/graphics", error)) {
        const std::string name = entry.path().filename().string();
        // fbN entries only; "fbcon" and friends are not framebuffers.
        if (name.size() < 3 || name.rfind("fb", 0) != 0 ||
            !std::isdigit(static_cast<unsigned char>(name[2])))
            continue;
        std::ifstream in(entry.path() / "name");
        std::string driver;
        if (!(in >> driver))
            continue;
        fbs.push_back(name);
        std::transform(driver.begin(), driver.end(), driver.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (driver.find("vc4") == std::string::npos && driver.find("v3d") == std::string::npos &&
            driver.find("stdout") == std::string::npos && driver.find("simple") == std::string::npos &&
            driver.find("dummy") == std::string::npos)
            panels.push_back(name);
    }
    std::sort(fbs.begin(), fbs.end());
    std::sort(panels.begin(), panels.end());
    if (fbs.size() == 1)
        return "/dev/" + fbs.front();
    if (!panels.empty())
        return "/dev/" + panels.front();
    return {};
}

} // namespace

void renderDisplayFrame(DisplaySurface& surface, const DisplayState& state)
{
    if (surface.pixels == nullptr)
        return;
    fillRect(surface, 0, 0, surface.width, surface.height, kBg);

    const int buttonTop = static_cast<int>(kTriggerButton.y0 * surface.height);

    int y = 4;
    fillRect(surface, 0, 0, surface.width, 26, kHeader);
    drawText(surface, 6, 7, "piScanner", kText, 2);
    if (state.timedOut)
        drawText(surface, surface.width - 110, 7, "TIMEOUT", kWarn, 2);
    y = 34;

    if (!state.last) {
        drawText(surface, 6, y + 20, "waiting for a scan...", kDim, 2);
    } else {
        // Decode data first (white, wrapped), then "542ms | EAN-8" green.
        const int cols = textCols(surface, 2);
        std::string rest = state.last->text;
        while (!rest.empty() && y < buttonTop - 90) {
            const std::string row = clipped(rest, cols);
            drawText(surface, 6, y, row, kText, 2);
            y += 24; // a scale-2 glyph row is ~20px tall
            rest = rest.size() > static_cast<std::size_t>(cols) ? rest.substr(static_cast<std::size_t>(cols)) : "";
        }
        std::ostringstream status;
        status << static_cast<long>(state.last->readyMs + 0.5) << "ms | " << state.last->format;
        drawText(surface, 6, y + 2, status.str(), kGood, 2);
        y += 24;

        if (state.stats.count > 0) {
            std::ostringstream stats;
            stats << "avg " << static_cast<long>(state.stats.sumMs / static_cast<double>(state.stats.count) + 0.5)
                  << "ms  min " << static_cast<long>(state.stats.minMs + 0.5)
                  << "  max " << static_cast<long>(state.stats.maxMs + 0.5);
            drawText(surface, 6, y, stats.str(), kDim, 1);
            y += 16;
        }

        y = buttonTop - 8 - static_cast<int>(state.history.size()) * 16;
        y = std::max(y, 120);
        for (const auto& entry : state.history) {
            if (y > buttonTop - 16)
                break;
            const int hcols = textCols(surface, 1);
            std::ostringstream row;
            row << entry.format << " " << static_cast<long>(entry.readyMs + 0.5) << "ms  " << entry.text;
            drawText(surface, 6, y, clipped(row.str(), hcols), kDim, 1);
            y += 16;
        }
    }

    fillRect(surface, 6, buttonTop + 4, surface.width - 12, surface.height - buttonTop - 8, kGood);
    const std::string label = "SCAN";
    const int labelW = static_cast<int>(label.size()) * 8 * 3;
    drawText(surface, (surface.width - labelW) / 2, buttonTop + (surface.height - buttonTop - 24) / 2,
             label, kBg, 3);

    // Red square capture button on the right, same height as SCAN.
    const int cx0 = static_cast<int>(kCaptureButton.x0 * surface.width);
    const int cy0 = static_cast<int>(kCaptureButton.y0 * surface.height);
    const int cw = static_cast<int>((kCaptureButton.x1 - kCaptureButton.x0) * surface.width);
    const int ch = static_cast<int>((kCaptureButton.y1 - kCaptureButton.y0) * surface.height);
    fillRect(surface, cx0 + 2, cy0 + 2, cw - 4, ch - 4, rgb565(0xe0, 0x30, 0x30));
}

DisplaySink::DisplaySink(ResultSink& inner, const Config& config, std::shared_ptr<std::atomic<int>> mode)
    : inner_(inner), rotation_(config.displayRotate), mode_(std::move(mode))
{
    if (!mode_)
        mode_ = std::make_shared<std::atomic<int>>(0);
    state_.historyLimit = static_cast<std::size_t>(std::max(1, config.displayLines));
    open(config);
}

DisplaySink::~DisplaySink()
{
    close();
}

void DisplaySink::open(const Config& config)
{
    const std::string path = resolveFbPath(config.displayDevice);
    if (path.empty()) {
        std::cerr << "warning: no framebuffer device found; display disabled\n";
        logLine("display none found");
        return;
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "warning: cannot open " << path << ": " << std::strerror(errno)
                  << "; display disabled\n";
        logLine("display open failed " + path);
        return;
    }

    fb_var_screeninfo var{};
    fb_fix_screeninfo fix{};
    if (xioctl(fd_, FBIOGET_VSCREENINFO, &var) < 0 || xioctl(fd_, FBIOGET_FSCREENINFO, &fix) < 0) {
        std::cerr << "warning: " << path << " is not a framebuffer; display disabled\n";
        close();
        return;
    }
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        std::cerr << "warning: " << path << " uses unsupported " << var.bits_per_pixel
                  << "bpp; display disabled\n";
        close();
        return;
    }

    fbWidth_ = static_cast<int>(var.xres);
    fbHeight_ = static_cast<int>(var.yres);
    fbStride_ = static_cast<int>(fix.line_length);
    surface_.bitsPerPixel = static_cast<int>(var.bits_per_pixel);
    // Rotated UIs render into a portrait buffer; paint() maps it into the
    // landscape framebuffer.
    if (rotation_ == 90 || rotation_ == 270) {
        surface_.width = fbHeight_;
        surface_.height = fbWidth_;
    } else {
        surface_.width = fbWidth_;
        surface_.height = fbHeight_;
    }
    surface_.strideBytes = surface_.width * (surface_.bitsPerPixel / 8);
    fbBytes_ = static_cast<std::size_t>(fix.smem_len);
    if (fbBytes_ == 0)
        fbBytes_ = static_cast<std::size_t>(fbStride_) * fbHeight_;

    fb_ = static_cast<std::uint8_t*>(mmap(nullptr, fbBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (fb_ == MAP_FAILED) {
        fb_ = nullptr;
        std::cerr << "warning: mmap " << path << " failed: " << std::strerror(errno)
                  << "; display disabled\n";
        close();
        return;
    }

    backing_.resize(static_cast<std::size_t>(surface_.strideBytes) * surface_.height);
    surface_.pixels = backing_.data();
    logLine("display " + path + " " + std::to_string(fbWidth_) + "x" + std::to_string(fbHeight_) +
            " " + std::to_string(surface_.bitsPerPixel) + "bpp rot=" + std::to_string(rotation_));
    paint();
}

void DisplaySink::close() noexcept
{
    if (fb_ != nullptr)
        munmap(fb_, fbBytes_);
    fb_ = nullptr;
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

void DisplaySink::paint()
{
    if (surface_.pixels == nullptr)
        return;
    // Photo mode: blit the captured image (already converted to surface
    // format at render size) instead of the scanner UI.
    if (mode_ && mode_->load() == 1 && !photo_.empty()) {
        const auto copyBytes = std::min(photo_.size(), backing_.size());
        std::memcpy(backing_.data(), photo_.data(), copyBytes);
    } else {
        renderDisplayFrame(surface_, state_);
    }
    const int bpp = surface_.bitsPerPixel / 8;
    if (rotation_ == 0) {
        const auto rows = std::min<std::size_t>(fbBytes_ / static_cast<std::size_t>(std::max(1, fbStride_)),
                                                backing_.size() / static_cast<std::size_t>(std::max(1, surface_.strideBytes)));
        for (std::size_t y = 0; y < rows; ++y)
            std::memcpy(fb_ + y * fbStride_, backing_.data() + y * surface_.strideBytes,
                        static_cast<std::size_t>(surface_.width) * bpp);
        return;
    }
    // Rotated blit: portrait surface -> landscape framebuffer.
    for (int y = 0; y < fbHeight_; ++y) {
        for (int x = 0; x < fbWidth_; ++x) {
            int sx, sy;
            if (rotation_ == 270) {
                sx = surface_.width - 1 - y;
                sy = x;
            } else if (rotation_ == 90) {
                sx = y;
                sy = surface_.height - 1 - x;
            } else { // 180
                sx = surface_.width - 1 - x;
                sy = surface_.height - 1 - y;
            }
            auto* dst = fb_ + static_cast<std::size_t>(y) * fbStride_ + static_cast<std::size_t>(x) * bpp;
            const auto* src = backing_.data() + static_cast<std::size_t>(sy) * surface_.strideBytes +
                              static_cast<std::size_t>(sx) * bpp;
            std::memcpy(dst, src, bpp);
        }
    }
}

void DisplaySink::onResult(const ScanResult& result)
{
    inner_.onResult(result);
    // Same end-to-end figure LogSink reports: trigger press (or frame
    // capture when untriggered) until the result reached this sink.
    const auto now = monotonicNowUs();
    const auto baseUs = result.triggerTimestampUs != 0 ? result.triggerTimestampUs : result.frameTimestampUs;
    const double readyMs = now > baseUs ? static_cast<double>(now - baseUs) / 1000.0 : 0.0;
    std::lock_guard lock(mutex_);
    DisplayEntry entry{result.format, result.text, readyMs};
    state_.stats.add(readyMs);
    if (state_.last)
        state_.history.push_front(*state_.last);
    state_.last = entry;
    while (state_.history.size() > state_.historyLimit)
        state_.history.pop_back();
    state_.timedOut = false;
    ++state_.results;
    paint();
}

void DisplaySink::onTimeout(std::uint64_t timestampUs)
{
    inner_.onTimeout(timestampUs);
    std::lock_guard lock(mutex_);
    state_.timedOut = true;
    ++state_.timeouts;
    paint();
}

namespace {

// Frame -> surface-sized pixel buffer. Supports YUYV and GREY (the
// service uses YUYV); MJPEG and empty frames produce an empty buffer.
std::vector<std::uint8_t> frameToSurface(const Frame& frame, const DisplaySurface& surface)
{
    if (frame.data == nullptr || frame.width <= 0 || frame.height <= 0 ||
        surface.width <= 0 || surface.height <= 0)
        return {};
    if (frame.fmt == PixelFormat::MJPEG)
        return {}; // capture path only handles raw formats
    const int bpp = surface.bitsPerPixel / 8;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(surface.width) * surface.height * bpp, 0);
    for (int y = 0; y < surface.height; ++y) {
        const int sy = std::min(y * frame.height / surface.height, frame.height - 1);
        for (int x = 0; x < surface.width; ++x) {
            const int sx = std::min(x * frame.width / surface.width, frame.width - 1);
            std::uint8_t r = 0, g = 0, b = 0;
            if (frame.fmt == PixelFormat::GREY) {
                const std::uint8_t v = frame.data[static_cast<std::size_t>(sy) * frame.stride + sx];
                r = g = b = v;
            } else { // YUYV pairs
                const std::size_t o = static_cast<std::size_t>(sy) * frame.stride + static_cast<std::size_t>(sx & ~1) * 2;
                const int yv = frame.data[o + (sx & 1) * 2];
                const int u = frame.data[o + 1] - 128;
                const int vv = frame.data[o + 3] - 128;
                const int cr = (yv * 256 + 359 * vv) >> 8;
                const int cg = (yv * 256 - 88 * u - 183 * vv) >> 8;
                const int cb = (yv * 256 + 454 * u) >> 8;
                r = static_cast<std::uint8_t>(std::clamp(cr, 0, 255));
                g = static_cast<std::uint8_t>(std::clamp(cg, 0, 255));
                b = static_cast<std::uint8_t>(std::clamp(cb, 0, 255));
            }
            auto* dst = out.data() + (static_cast<std::size_t>(y) * surface.width + x) * bpp;
            if (bpp == 2) {
                const std::uint16_t p = rgb565(r, g, b);
                dst[0] = static_cast<std::uint8_t>(p & 0xff);
                dst[1] = static_cast<std::uint8_t>(p >> 8);
            } else {
                dst[0] = b;
                dst[1] = g;
                dst[2] = r;
                dst[3] = 0xff;
            }
        }
    }
    return out;
}

} // namespace

void DisplaySink::onAux(const Frame& frame)
{
    std::lock_guard lock(mutex_);
    if (mode_->load() == 1) {
        // Exit tap: back to the scanner UI.
        mode_->store(0);
        paint();
        return;
    }
    photo_ = frameToSurface(frame, surface_);
    if (photo_.empty()) {
        logLine("capture unsupported frame format");
        return;
    }
    mode_->store(1);
    logLine("capture shown " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
    paint();
}
