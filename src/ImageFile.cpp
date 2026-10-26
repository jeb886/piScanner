#include "ImageFile.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <cctype>

#ifdef PISCAN_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

namespace fs = std::filesystem;

GrayImage loadPgm(const std::vector<std::uint8_t>& bytes, const std::string& path); // defined below

GrayImage loadImage(const std::string& path)
{
    const auto bytes = loadFileBytes(path);
    if (bytes.empty())
        throw std::runtime_error("empty file: " + path);

    // Binary PGM (P5): parse natively since it's the bench/test format.
    if (bytes.size() > 2 && bytes[0] == 'P' && bytes[1] == '5')
        return loadPgm(bytes, path);

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (data == nullptr)
        throw std::runtime_error("cannot decode image " + path + ": " + stbi_failure_reason());
    GrayImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < image.pixels.size(); ++i)
        image.pixels[i] = rgbToLum(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
    stbi_image_free(data);
    return image;
}

GrayImage loadPgm(const std::vector<std::uint8_t>& bytes, const std::string& path)
{
    std::size_t pos = 2;
    std::function<std::string()> token = [&]() -> std::string {
        while (pos < bytes.size() && std::isspace(bytes[pos]))
            ++pos;
        if (pos < bytes.size() && bytes[pos] == '#') {
            while (pos < bytes.size() && bytes[pos] != '\n')
                ++pos;
            return token();
        }
        const std::size_t start = pos;
        while (pos < bytes.size() && !std::isspace(bytes[pos]))
            ++pos;
        return std::string(reinterpret_cast<const char*>(bytes.data() + start), pos - start);
    };
    const int width = std::stoi(token());
    const int height = std::stoi(token());
    if (std::stoi(token()) != 255)
        throw std::runtime_error("PGM maxval must be 255: " + path);
    ++pos; // single whitespace after maxval
    const std::size_t need = static_cast<std::size_t>(width) * height;
    if (width <= 0 || height <= 0 || bytes.size() - pos < need)
        throw std::runtime_error("truncated PGM: " + path);
    GrayImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(bytes.begin() + pos, bytes.begin() + pos + need);
    return image;
}

std::vector<std::uint8_t> loadFileBytes(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open file: " + path);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("cannot read file: " + path);
    return bytes;
}

namespace {
const char* kImageExtensions[] = {".pgm", ".png", ".jpg", ".jpeg", ".bmp", ".tga"};

bool isImageFile(const fs::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::find(std::begin(kImageExtensions), std::end(kImageExtensions), ext) !=
           std::end(kImageExtensions);
}
} // namespace

std::vector<std::string> expandImagePaths(const std::vector<std::string>& inputs)
{
    std::vector<std::string> out;
    for (const auto& input : inputs) {
        const fs::path path(input);
        if (fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.is_regular_file() && isImageFile(entry.path()))
                    out.push_back(entry.path().string());
            }
        } else if (fs::is_regular_file(path)) {
            out.push_back(input);
        } else {
            throw std::runtime_error("no such file or directory: " + input);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::uint8_t> encodeJpeg(const GrayImage& image, int quality)
{
#ifdef PISCAN_HAVE_TURBOJPEG
    tjhandle handle = tjInitCompress();
    if (!handle)
        throw std::runtime_error("failed to initialize libturbojpeg");
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    const int rc = tjCompress2(handle, image.pixels.data(), image.width, image.width, image.height, TJPF_GRAY,
                               &buffer, &size, TJSAMP_GRAY, quality, TJFLAG_FASTDCT);
    if (rc != 0) {
        std::string reason = tjGetErrorStr();
        tjDestroy(handle);
        throw std::runtime_error("JPEG encode failed: " + reason);
    }
    std::vector<std::uint8_t> out(buffer, buffer + size);
    tjFree(buffer);
    tjDestroy(handle);
    return out;
#else
    (void)image;
    (void)quality;
    throw std::runtime_error("MJPEG support is not available in this build");
#endif
}

std::vector<std::uint8_t> packYuyv(const GrayImage& image, int padBytes, int& stride)
{
    stride = image.width * 2 + padBytes;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(stride) * image.height);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto dst = static_cast<std::size_t>(y) * stride + x * 2;
            data[dst] = image.pixels[static_cast<std::size_t>(y) * image.width + x];
            data[dst + 1] = 128;
        }
    }
    return data;
}

std::uint8_t rgbToLum(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint8_t>((r * 299 + g * 587 + b * 114 + 500) / 1000);
}

std::string expectedTextPath(const std::string& imagePath)
{
    return (fs::path(imagePath).parent_path() / (fs::path(imagePath).stem().string() + ".txt")).string();
}
