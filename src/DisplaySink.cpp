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

    int y = 4;
    fillRect(surface, 0, 0, surface.width, 26, kHeader);
    drawText(surface, 6, 7, "piScanner", kText, 2);
    if (state.timedOut)
        drawText(surface, surface.width - 110, 7, "TIMEOUT", kWarn, 2);
    y = 34;

    if (!state.last) {
        drawText(surface, 6, y + 20, "waiting for a scan...", kDim, 2);
        return;
    }

    drawText(surface, 6, y, state.last->format, kGood, 3);
    y += 34;
    const int cols = textCols(surface, 2);
    std::string rest = state.last->text;
    while (!rest.empty() && y < surface.height - 60) {
        const std::string row = clipped(rest, cols);
        drawText(surface, 6, y, row, kText, 2);
        y += 24; // a scale-2 glyph row is ~20px tall
        rest = rest.size() > static_cast<std::size_t>(cols) ? rest.substr(static_cast<std::size_t>(cols)) : "";
    }
    std::ostringstream ms;
    ms << static_cast<long>(state.last->readyMs + 0.5) << "ms";
    drawText(surface, 6, y + 2, ms.str(), kDim, 2);
    y += 24;

    y = surface.height - 14 - static_cast<int>(state.history.size()) * 16;
    y = std::max(y, 130);
    for (const auto& entry : state.history) {
        if (y > surface.height - 20)
            break;
        const int hcols = textCols(surface, 1);
        drawText(surface, 6, y, clipped(entry.format + "  " + entry.text, hcols), kDim, 1);
        y += 16;
    }
}

DisplaySink::DisplaySink(ResultSink& inner, const Config& config) : inner_(inner)
{
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

    surface_.width = static_cast<int>(var.xres);
    surface_.height = static_cast<int>(var.yres);
    surface_.strideBytes = static_cast<int>(fix.line_length);
    surface_.bitsPerPixel = static_cast<int>(var.bits_per_pixel);
    fbBytes_ = static_cast<std::size_t>(fix.smem_len);
    if (fbBytes_ == 0)
        fbBytes_ = static_cast<std::size_t>(surface_.strideBytes) * surface_.height;

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
    logLine("display " + path + " " + std::to_string(surface_.width) + "x" +
            std::to_string(surface_.height) + " " + std::to_string(surface_.bitsPerPixel) + "bpp");
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
    renderDisplayFrame(surface_, state_);
    const auto rows = std::min<std::size_t>(fbBytes_ / static_cast<std::size_t>(std::max(1, surface_.strideBytes)),
                                            backing_.size() / static_cast<std::size_t>(std::max(1, surface_.strideBytes)));
    std::memcpy(fb_, backing_.data(), rows * static_cast<std::size_t>(surface_.strideBytes));
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
