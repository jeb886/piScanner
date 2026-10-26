#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Decoded 8-bit grayscale image.
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // row-major, no padding
};

// Loads an image file and converts it to grayscale. Supports binary P5 PGM
// directly and common formats (JPEG, PNG, BMP, TGA) through stb_image.
// Throws std::runtime_error on unsupported/corrupt input.
GrayImage loadImage(const std::string& path);

// Loads raw file bytes (used for MJPEG mock frames, where the payload must
// stay compressed).
std::vector<std::uint8_t> loadFileBytes(const std::string& path);

// Expands inputs that are directories into sorted lists of image files
// (extension match); files pass through unchanged. Empty list is an error
// thrown by the caller, not here.
std::vector<std::string> expandImagePaths(const std::vector<std::string>& inputs);

// Encodes a grayscale image as JPEG via libturbojpeg. Throws if turbojpeg
// is unavailable in this build or encoding fails.
std::vector<std::uint8_t> encodeJpeg(const GrayImage& image, int quality);

// Packs grayscale pixels into a YUYV buffer (neutral chroma = 128), with an
// optional extra byte padding per row appended at the end of each row's
// luma+chroma span. Returns the bytes and sets stride.
std::vector<std::uint8_t> packYuyv(const GrayImage& image, int padBytes, int& stride);

// Converts the original color image to grayscale in-place given the raw
// RGB(A) channel count. (BT.601 luma, rounded.)
std::uint8_t rgbToLum(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Expected-text sidecar path for a sample image: "<stem>.txt" next to it.
std::string expectedTextPath(const std::string& imagePath);
