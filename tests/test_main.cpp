#include "Config.h"
#include "Decoder.h"
#include "Dedupe.h"
#include "Output.h"

#include <CreateBarcode.h>
#include <WriteBarcode.h>

#include <algorithm>
#include <cstdlib>
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

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

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

}

int main(int argc, char* argv[])
{
    testBarcodeReaders();
    testRoiAndFormats();
    testMjpeg();
    testDedupe();
    testArguments();
    testJson();
    if (argc == 3)
        testBench(argv[1], argv[2]);
    else
        CHECK(false);
    if (failures != 0)
        std::cerr << failures << " test assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
