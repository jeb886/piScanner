#include "Config.h"
#include "DecodeFiles.h"
#include "Decoder.h"
#include "Dedupe.h"
#include "FileFrameSource.h"
#include "HidKeys.h"
#include "ImageFile.h"
#include "Output.h"
#include "Scanner.h"
#include "Trigger.h"
#include "V4l2Camera.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <CreateBarcode.h>
#include <WriteBarcode.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#ifdef PISCAN_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression "\n";                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)


GrayImage barcodeFrame(const std::string& text, ZXing::BarcodeFormat format, int x = -1)
{
    const auto barcode = ZXing::CreateBarcodeFromText(text, ZXing::CreatorOptions(format));
    const auto symbol = ZXing::WriteBarcodeToImage(barcode, ZXing::WriterOptions().scale(4));
    GrayImage image{640, 480, std::vector<std::uint8_t>(640 * 480, 255)};
    if (symbol.width() > image.width || symbol.height() > image.height) {
        CHECK(false);
        return image;
    }
    if (x < 0)
        x = (image.width - symbol.width()) / 2;
    const int y = (image.height - symbol.height()) / 2;
    for (int row = 0; row < symbol.height(); ++row)
        std::copy_n(symbol.data() + static_cast<std::size_t>(row) * symbol.width(), symbol.width(),
                    image.pixels.data() + static_cast<std::size_t>(y + row) * image.width + x);
    return image;
}

std::vector<ScanResult> decodeGray(const GrayImage& image)
{
    Config config;
    Decoder decoder(config);
    Frame frame{image.pixels.data(), image.pixels.size(), image.width, image.height, image.width, PixelFormat::GREY, 42, 0};
    return decoder.decode(frame);
}

std::vector<ScanResult> decodeYuyv(const GrayImage& image, int padding = 32)
{
    Config config;
    Decoder decoder(config);
    const int stride = image.width * 2 + padding;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(stride) * image.height, 0);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * stride + x * 2;
            data[offset] = image.pixels[static_cast<std::size_t>(y) * image.width + x];
            data[offset + 1] = 128;
        }
    }
    Frame frame{data.data(), data.size(), image.width, image.height, stride, PixelFormat::YUYV, 42, 0};
    return decoder.decode(frame);
}

void checkDecoded(const std::vector<ScanResult>& results, const std::string& text, const std::string& format)
{
    if (results.empty())
        std::cerr << "no decoded result for expected " << format << " text: " << text << '\n';
    CHECK(!results.empty());
    if (!results.empty()) {
        CHECK(results.front().text == text);
        if (results.front().format.find(format) == std::string::npos)
            std::cerr << "expected format " << format << ", got " << results.front().format << '\n';
        CHECK(results.front().format.find(format) != std::string::npos);
    }
}

std::optional<Config> parse(const std::vector<std::string>& arguments, std::string& error)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    return parseArgs(static_cast<int>(argv.size()), argv.data(), error);
}

void testBarcodeReaders()
{
    const std::vector<std::tuple<std::string, ZXing::BarcodeFormat, std::string>> cases = {
        {"QR test 123", ZXing::BarcodeFormat::QRCode, "QR"},
        {"1234", ZXing::BarcodeFormat::Code128, "Code 128"},
        {"5901234123457", ZXing::BarcodeFormat::EAN13, "EAN"},
    };
    for (const auto& [text, format, formatName] : cases) {
        const auto image = barcodeFrame(text, format);
        checkDecoded(decodeYuyv(image), text, formatName);
        checkDecoded(decodeGray(image), text, formatName);
    }
}

void testRoiAndFormats()
{
    const auto image = barcodeFrame("RIGHT SIDE", ZXing::BarcodeFormat::QRCode, 410);
    Config leftConfig;
    leftConfig.roi = RegionOfInterest{0, 0, 0.5, 1};
    Decoder leftDecoder(leftConfig);
    Frame frame{image.pixels.data(), image.pixels.size(), image.width, image.height, image.width, PixelFormat::GREY, 1, 0};
    CHECK(leftDecoder.decode(frame).empty());

    Config rightConfig;
    rightConfig.roi = RegionOfInterest{0.5, 0, 0.5, 1};
    Decoder rightDecoder(rightConfig);
    checkDecoded(rightDecoder.decode(frame), "RIGHT SIDE", "QR");

    Config restricted;
    restricted.formats = "QRCode";
    Decoder restrictedDecoder(restricted);
    const auto linear = barcodeFrame("1234", ZXing::BarcodeFormat::Code128);
    Frame linearFrame{linear.pixels.data(), linear.pixels.size(), linear.width, linear.height, linear.width,
                      PixelFormat::GREY, 2, 0};
    CHECK(restrictedDecoder.decode(linearFrame).empty());
}

void testMjpeg()
{
#ifdef PISCAN_HAVE_TURBOJPEG
    const auto image = barcodeFrame("JPEG QR", ZXing::BarcodeFormat::QRCode);
    tjhandle compressor = tjInitCompress();
    CHECK(compressor != nullptr);
    if (compressor == nullptr)
        return;
    unsigned char* encoded = nullptr;
    unsigned long encodedSize = 0;
    const int rc = tjCompress2(compressor, image.pixels.data(), image.width, image.width, image.height, TJPF_GRAY,
                               &encoded, &encodedSize, TJSAMP_GRAY, 95, TJFLAG_FASTDCT);
    CHECK(rc == 0);
    if (rc == 0) {
        Config config;
        Decoder decoder(config);
        Frame frame{encoded, encodedSize, image.width, image.height, image.width, PixelFormat::MJPEG, 3, 0};
        checkDecoded(decoder.decode(frame), "JPEG QR", "QR");
    }
    tjFree(encoded);
    tjDestroy(compressor);
#endif
}

void testDedupe()
{
    Dedupe dedupe;
    CHECK(dedupe.shouldEmit("QR", "same", 1000, 100));
    CHECK(!dedupe.shouldEmit("QR", "same", 1050, 100));
    CHECK(dedupe.shouldEmit("QR", "same", 1101, 100));
    CHECK(dedupe.shouldEmit("QR", "other", 1050, 100));
    CHECK(dedupe.shouldEmit("QR", "same", 1200, 0));
}

void testArguments()
{
    std::string error;
    auto defaults = parse({"piscan", "scan"}, error);
    CHECK(defaults.has_value());
    if (defaults) {
        CHECK(defaults->threads == 2);
        CHECK(defaults->buffers == 4);
        CHECK(defaults->pixfmt == "auto");
        CHECK(!defaults->tryHarder);
        CHECK(defaults->tryRotate);
    }
    auto options = parse({"piscan", "scan", "--threads", "3", "--buffers", "5", "--roi", "0.1,0.2,0.5,0.6",
                          "--try-harder", "--no-rotate"},
                         error);
    CHECK(options.has_value());
    if (options) {
        CHECK(options->threads == 3);
        CHECK(options->roi.has_value());
        CHECK(options->tryHarder);
        CHECK(!options->tryRotate);
    }
    CHECK(!parse({"piscan", "scan", "--threads", "3", "--buffers", "4"}, error).has_value());
    CHECK(!parse({"piscan", "scan", "--roi", "0.8,0,0.5,1"}, error).has_value());
    CHECK(!parse({"piscan", "scan", "--pixfmt", "bad"}, error).has_value());
    auto hid = parse({"piscan", "scan", "--output", "hid", "--hid-device", "/dev/hidg3"}, error);
    CHECK(hid.has_value());
    if (hid) {
        CHECK(hid->output == "hid");
        CHECK(hid->hidDevice == "/dev/hidg3");
    }
    CHECK(!parse({"piscan", "scan", "--output", "bogus"}, error).has_value());
}

// Every character of a representative barcode charset must map to a keycode,
// and the shifted digits must match US-QWERTY.
void testHidKeys()
{
    std::uint8_t keycode = 0;
    std::uint8_t modifier = 0;
    for (const char ch : std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz -./$+%")) {
        CHECK(hidKeyForChar(static_cast<unsigned char>(ch), keycode, modifier));
    }
    CHECK(hidKeyForChar('a', keycode, modifier) && keycode == 0x04 && modifier == 0);
    CHECK(hidKeyForChar('A', keycode, modifier) && keycode == 0x04 && modifier == 1);
    CHECK(hidKeyForChar('5', keycode, modifier) && keycode == 0x22 && modifier == 0);
    CHECK(hidKeyForChar('%', keycode, modifier) && keycode == 0x22 && modifier == 1);
    CHECK(hidKeyForChar('0', keycode, modifier) && keycode == 0x27 && modifier == 0);
    CHECK(hidKeyForChar('(', keycode, modifier) && keycode == 0x26 && modifier == 1);
    CHECK(hidKeyForChar(')', keycode, modifier) && keycode == 0x27 && modifier == 1);
    CHECK(hidKeyForChar('!', keycode, modifier) && keycode == 0x1E && modifier == 1);
    CHECK(hidKeyForChar('"', keycode, modifier) && keycode == 0x34 && modifier == 1);
    CHECK(hidKeyForChar('\'', keycode, modifier) && keycode == 0x34 && modifier == 0);
    CHECK(hidKeyForChar(' ', keycode, modifier) && keycode == 0x2C);
    CHECK(hidKeyForChar('\n', keycode, modifier) && keycode == 0x28);
    CHECK(!hidKeyForChar(static_cast<unsigned char>(0xC3), keycode, modifier));
}

void testJson()
{
    CHECK(jsonEscape("quote\" slash\\ newline\n tab\t") == "quote\\\" slash\\\\ newline\\n tab\\t");
    CHECK(jsonEscape(std::string("control\x01", 8)) == "control\\u0001");
    CHECK(jsonEscape("café") == "café");
}

void writePgm(const std::string& path, const GrayImage& image)
{
    std::ofstream output(path, std::ios::binary);
    output << "P5\n# smoke image\n" << image.width << ' ' << image.height << "\n255\n";
    output.write(reinterpret_cast<const char*>(image.pixels.data()),
                 static_cast<std::streamsize>(image.pixels.size()));
}

void testBench(const char* executable, const char* pgmPath)
{
    const auto image = barcodeFrame("BENCH-QR", ZXing::BarcodeFormat::QRCode);
    writePgm(pgmPath, image);
    const std::string command = std::string("\"") + executable + "\" bench \"" + pgmPath +
                                "\" --iterations 5 > /dev/null";
    CHECK(std::system(command.c_str()) == 0);
}

// --- File source / real-image tests ---------------------------------------

std::string g_dataDir;

std::set<std::string> expectedTexts(const std::string& dir)
{
    std::set<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".txt")
            continue;
        std::ifstream in(entry.path());
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty() && text != "NONE")
            out.insert(text);
    }
    return out;
}

Config fileConfig(const std::string& command, const std::string& pixfmt, int repeat)
{
    Config config;
    config.command = command;
    config.pixfmt = pixfmt;
    config.repeat = repeat;
    config.buffers = 8;
    config.threads = 2;
    return config;
}

class CollectingSink : public ResultSink {
public:
    void onResult(const ScanResult& result) override
    {
        std::lock_guard lock(mutex_);
        results.push_back(result);
    }
    void onTimeout(std::uint64_t timestampUs) override
    {
        std::lock_guard lock(mutex_);
        timeouts.push_back(timestampUs);
    }
    std::mutex mutex_;
    std::vector<ScanResult> results;
    std::vector<std::uint64_t> timeouts;
};

void testFileFrameSource()
{
    const std::string dir = g_dataDir + "/qr";
    auto config = fileConfig("scan", "auto", 1);
    FileFrameSource source({dir}, config);
    const auto expected = expectedTexts(dir);
    CHECK(source.paths().size() == 10);
    Frame frame;
    int index = -1;
    int count = 0;
    // dequeue() may time out on paced frames — poll until the source drains.
    while (!source.exhausted()) {
        if (!source.dequeue(frame, index, 10))
            continue;
        ++count;
        CHECK(frame.fmt == PixelFormat::GREY);
        CHECK(frame.width > 0 && frame.height > 0 && frame.stride >= frame.width);
        source.requeue(index);
    }
    CHECK(count == static_cast<int>(source.paths().size()));
    CHECK(source.exhausted());

    // Every image decodes to its sidecar text straight through the source.
    Config decodeConfig;
    Decoder decoder(decodeConfig);
    auto source2 = fileConfig("scan", "auto", 1);
    FileFrameSource fresh({dir}, source2);
    std::set<std::string> decoded;
    while (!fresh.exhausted()) {
        if (!fresh.dequeue(frame, index, 10))
            continue;
        for (const auto& result : decoder.decode(frame))
            decoded.insert(result.text);
    }
    for (const auto& text : expected)
        CHECK(decoded.count(text) == 1);
}

void testFileFrameSourceFormats()
{
    const std::string dir = g_dataDir + "/qr";
    for (const char* fmt : {"yuyv", "mjpeg"}) {
        auto config = fileConfig("scan", fmt, 1);
        FileFrameSource source({dir}, config);
        Config decodeConfig;
        Decoder decoder(decodeConfig);
        Frame frame;
        int index = -1;
        int decoded = 0;
        while (!source.exhausted()) {
            if (!source.dequeue(frame, index, 10))
                continue;
            CHECK((fmt == std::string("yuyv") ? frame.fmt == PixelFormat::YUYV : frame.fmt == PixelFormat::MJPEG));
            if (!decoder.decode(frame).empty())
                ++decoded;
        }
        CHECK(decoded == static_cast<int>(source.paths().size()));
    }
}

void testDecodeExpect()
{
    std::ostringstream sink;
    Config config;
    config.command = "decode";
    config.expect = true;
    config.decodeInputs = {g_dataDir + "/upca", g_dataDir + "/qr"};
    CHECK(runDecodeFiles(config, sink) == 0);

    config.decodeInputs = {g_dataDir + "/negative"};
    sink.str("");
    CHECK(runDecodeFiles(config, sink) == 0);

    // Hard set fails under default options, passes with try-harder.
    config.decodeInputs = {g_dataDir + "/hard"};
    sink.str("");
    CHECK(runDecodeFiles(config, sink) == 1);
    config.tryHarder = true;
    sink.str("");
    CHECK(runDecodeFiles(config, sink) == 0);

    // Tampered sidecar must fail.
    const std::string victim = g_dataDir + "/qr";
    Config bad;
    bad.command = "decode";
    bad.expect = true;
    const auto png = victim + "/qrcode-1_1.png";
    const auto txt = expectedTextPath(png);
    const auto backup = loadFileBytes(txt);
    {
        std::ofstream overwrite(txt);
        overwrite << "wrong-text";
    }
    bad.decodeInputs = {png};
    sink.str("");
    const int rc = runDecodeFiles(bad, sink);
    {
        std::ofstream restore(txt, std::ios::binary);
        restore.write(reinterpret_cast<const char*>(backup.data()), backup.size());
    }
    CHECK(rc == 1);
    CHECK(sink.str().find("FAIL") != std::string::npos);
}

void testScannerWithFileSource()
{
    const std::string dir = g_dataDir + "/qr";
    for (const char* fmt : {"auto", "yuyv"}) {
        auto config = fileConfig("scan", fmt, 1);
        config.dedupeMs = 0;
        FileFrameSource source({dir}, config);
        CollectingSink sink;
        Scanner scanner(config, source, sink, nullptr);
        CHECK(scanner.run() == 0);
        std::set<std::string> emitted;
        for (const auto& result : sink.results)
            emitted.insert(result.text);
        for (const auto& text : expectedTexts(dir)) {
            if (emitted.count(text) != 1)
                std::cerr << "missing decoded text from " << fmt << " pipeline: " << text << '\n';
            CHECK(emitted.count(text) == 1);
        }
    }
}

void testTriggerWithFileSource()
{
    // Press on every frame: each QR image produces one result.
    const std::string dir = g_dataDir + "/qr";
    auto config = fileConfig("trigger", "auto", 1);
    config.dedupeMs = 0; // two images share a text; we want every frame counted
    FileFrameSource source({dir}, config);
    CollectingSink sink;
    std::atomic<std::uint64_t> sessions{0};
    ScriptedTrigger trigger(sessions, static_cast<int>(source.paths().size()));
    Scanner scanner(config, source, sink, &trigger, &sessions);
    CHECK(scanner.run() == 0);
    std::set<std::string> emitted;
    for (const auto& result : sink.results)
        emitted.insert(result.text);
    const auto expected = expectedTexts(dir);
    for (const auto& text : expected) {
        if (emitted.count(text) != 1)
            std::cerr << "trigger pipeline missing text: " << text.substr(0, 60) << '\n';
        CHECK(emitted.count(text) == 1);
    }

    // Negative-only source: a press times out instead of producing a result.
    auto neg = fileConfig("trigger", "auto", 0);
    neg.scanTimeoutMs = 300;
    FileFrameSource negSource({g_dataDir + "/negative"}, neg);
    CollectingSink negSink;
    std::atomic<std::uint64_t> negSessions{0};
    ScriptedTrigger negTrigger(negSessions, 1); // press once, then wait
    Scanner negScanner(neg, negSource, negSink, &negTrigger, &negSessions);
    std::thread runner([&] { negScanner.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        {
            std::lock_guard lock(negSink.mutex_);
            if (!negSink.timeouts.empty())
                break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    requestShutdownSignal(0);
    runner.join();
    CHECK(!negSink.timeouts.empty());
    CHECK(negSink.results.empty());
}

void testDecodeCli(const char* executable)
{
    const std::string pass = std::string("\"") + executable + "\" decode \"" + g_dataDir +
                             "/qr\" --expect > /dev/null";
    CHECK(std::system(pass.c_str()) == 0);
    const std::string fail = std::string("\"") + executable + "\" decode \"" + g_dataDir +
                             "/hard\" --expect > /dev/null";
    CHECK(std::system(fail.c_str()) == 256); // exit code 1
}

}

// Pixel-format negotiation: auto prefers uncompressed formats but falls
// through to MJPEG when raw formats cannot reach the requested fps.
void testPixelFormatChoice()
{
    Config config;
    config.pixfmt = "auto";
    config.width = 1920;
    config.height = 1080;
    const auto yuyv = V4L2_PIX_FMT_YUYV;
    const auto grey = V4L2_PIX_FMT_GREY;
    const auto mjpeg = V4L2_PIX_FMT_MJPEG;
    const std::vector<std::uint32_t> all{grey, yuyv, mjpeg};

    // Brio-like: 1080p30 across all formats but YUYV caps at 30 fps, so a
    // 90 fps request must pick MJPEG.
    config.fps = 90;
    auto capped30 = [](std::uint32_t f) { return f == V4L2_PIX_FMT_MJPEG ? 1 : 0; };
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 1; }, capped30) == mjpeg);

    // Same camera at 30 fps: keep the zero-copy YUYV path.
    config.fps = 30;
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 1; }, [](std::uint32_t) { return 1; }) == grey);

    // fps unknown (-1): fall back to size preference order.
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 1; }, [](std::uint32_t) { return -1; }) == grey);

    // Nothing meets fps: still prefer a size-capable format.
    config.fps = 240;
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 1; }, [](std::uint32_t) { return 0; }) == grey);

    // No YUYV/GREY: MJPEG wins.
    CHECK(choosePixelFormat(config, {mjpeg}, [](std::uint32_t) { return 1; }, [](std::uint32_t) { return 1; }) == mjpeg);

    // Unsupported everything: 0.
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 0; }, [](std::uint32_t) { return -1; }) == 0);

    // Explicit pixfmt is honored even when slow.
    config.pixfmt = "mjpeg";
    CHECK(choosePixelFormat(config, all, [](std::uint32_t) { return 0; }, [](std::uint32_t) { return -1; }) == mjpeg);
}

int main(int argc, char* argv[])
{
    testBarcodeReaders();
    testRoiAndFormats();
    testMjpeg();
    testDedupe();
    testArguments();
    testHidKeys();
    testPixelFormatChoice();
    testJson();
    if (argc == 4) {
        g_dataDir = argv[3];
        testFileFrameSource();
        testFileFrameSourceFormats();
        testDecodeExpect();
        testScannerWithFileSource();
        testTriggerWithFileSource();
        testDecodeCli(argv[1]);
        testBench(argv[1], argv[2]);
    } else {
        CHECK(false);
    }
    if (failures != 0)
        std::cerr << failures << " test assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
