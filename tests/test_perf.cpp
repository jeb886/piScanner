#include "Config.h"
#include "Decoder.h"
#include "FileFrameSource.h"
#include "ImageFile.h"
#include "Output.h"
#include "Scanner.h"
#include "Trigger.h"

#include <CreateBarcode.h>
#include <WriteBarcode.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

int failures = 0;
std::string g_dataDir;

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression "\n";                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

std::vector<double> runTimes(const std::function<void()>& body, int iterations)
{
    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        timings.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(timings.begin(), timings.end());
    return timings;
}

void report(const std::string& name, const std::vector<double>& timings)
{
    const double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();
    const auto pct = [&](double f) {
        return timings[std::min(timings.size() - 1,
                                static_cast<std::size_t>(std::ceil(f * timings.size()) - 1))];
    };
    std::cout << "PERF " << name << " n=" << timings.size() << " mean_ms=" << mean << " p50_ms=" << pct(0.5)
              << " p99_ms=" << pct(0.99) << " min_ms=" << timings.front() << '\n';
}

GrayImage barcodeFrame(const std::string& text, ZXing::BarcodeFormat format, int width, int height)
{
    const auto barcode = ZXing::CreateBarcodeFromText(text, ZXing::CreatorOptions(format));
    const auto symbol = ZXing::WriteBarcodeToImage(
        barcode, ZXing::WriterOptions().scale(std::max(2, std::min(width, height) / 200)));
    GrayImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height, 255)};
    const int x = (width - symbol.width()) / 2;
    const int y = (height - symbol.height()) / 2;
    for (int row = 0; row < symbol.height(); ++row)
        std::copy_n(symbol.data() + static_cast<std::size_t>(row) * symbol.width(), symbol.width(),
                    image.pixels.data() + static_cast<std::size_t>(y + row) * image.width + x);
    return image;
}

// Core decoder cost per resolution on an easy QR. This is the dominant
// per-frame cost on a Brio: at 1080p the frame is ~2 MP of luma.
void perfDecodeByResolution()
{
    for (const auto& [width, height] : {std::pair{640, 480}, std::pair{1280, 720}, std::pair{1920, 1080}}) {
        const auto image = barcodeFrame("PERF-QR", ZXing::BarcodeFormat::QRCode, width, height);
        Config config;
        Decoder decoder(config);
        Frame frame{image.pixels.data(), image.pixels.size(), width, height, width, PixelFormat::GREY, 1, 0};
        const auto timings = runTimes([&] { decoder.decode(frame); }, 25);
        report("decode_qr_" + std::to_string(width) + "x" + std::to_string(height), timings);
        // Generous sanity budget: decodes must stay well under a frame
        // interval even on slow hardware; catches pathological regressions.
        CHECK(timings[timings.size() / 2] < 2000);
    }
}

// MJPEG frames must be decompressesed to gray before zxing runs; measure the
// full MJPEG-frame decode path (turbojpeg + barcode) at 1080p.
void perfMjpegPath()
{
#ifdef PISCAN_HAVE_TURBOJPEG
    const auto image = barcodeFrame("PERF-MJPEG", ZXing::BarcodeFormat::QRCode, 1920, 1080);
    const auto jpeg = encodeJpeg(image, 90);
    std::cout << "PERF mjpeg_input_bytes=" << jpeg.size() << '\n';
    Config config;
    config.pixfmt = "mjpeg";
    Decoder decoder(config);
    Frame frame{jpeg.data(), jpeg.size(), 1920, 1080, 1920, PixelFormat::MJPEG, 1, 0};
    const auto timings = runTimes([&] { decoder.decode(frame); }, 20);
    report("decode_mjpeg_1920x1080", timings);
#else
    std::cout << "PERF mjpeg skipped (no turbojpeg)\n";
#endif
}

// Real-photo decode cost: the curated sample set is what the camera actually
// sees, warts and all.
void perfSampleSet()
{
    Config config;
    Decoder decoder(config);
    for (const std::string sub : {"qr", "upca"}) {
        const auto paths = expandImagePaths({g_dataDir + "/" + sub});
        std::vector<double> all;
        for (const auto& path : paths) {
            const auto image = loadImage(path);
            Frame frame{image.pixels.data(), image.pixels.size(), image.width, image.height, image.width,
                        PixelFormat::GREY,        1,                 0};
            const auto one = runTimes([&] { decoder.decode(frame); }, 7);
            all.insert(all.end(), one.begin(), one.end());
        }
        std::sort(all.begin(), all.end());
        report("decode_samples_" + sub, all);
    }
}

// End-to-end trigger latency: press -> decoded text out the sink, on the
// file source at 60 fps pacing. This is the number a user feels.
void perfTriggerLatency()
{
    const std::string dir = g_dataDir + "/qr";
    Config config;
    config.command = "trigger";
    config.pixfmt = "auto";
    config.fps = 120; // pace fast; latency measures the pipeline, not pacing
    config.repeat = 1;
    config.buffers = 8;
    config.threads = 2;
    config.dedupeMs = 0;

    FileFrameSource source({dir}, config);
    struct Sink : ResultSink {
        void onResult(const ScanResult& result) override
        {
            std::lock_guard lock(mutex_);
            latenciesUs.push_back(monotonicNowUs() - result.triggerTimestampUs);
        }
        void onTimeout(std::uint64_t) override {}
        std::mutex mutex_;
        std::vector<std::uint64_t> latenciesUs;
    } sink;

    std::atomic<std::uint64_t> sessions{0};
    const int presses = static_cast<int>(source.paths().size());
    ScriptedTrigger trigger(sessions, presses);
    Scanner scanner(config, source, sink, &trigger, &sessions);
    CHECK(scanner.run() == 0);
    {
        std::lock_guard lock(sink.mutex_);
        CHECK(sink.latenciesUs.size() == static_cast<std::size_t>(presses));
        std::vector<double> ms(sink.latenciesUs.begin(), sink.latenciesUs.end());
        for (auto& v : ms)
            v /= 1000.0;
        std::sort(ms.begin(), ms.end());
        report("trigger_press_to_result", ms);
    }
}
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: piscan_perf_tests <tests/data dir>\n";
        return 1;
    }
    g_dataDir = argv[1];
    perfDecodeByResolution();
    perfMjpegPath();
    perfSampleSet();
    perfTriggerLatency();
    if (failures != 0)
        std::cerr << failures << " perf assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
