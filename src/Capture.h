#pragma once

#include "Config.h"

#include <string>

// capture: grabs a single frame from the camera (or a --source file mock)
// and writes it as an image file — a debugging utility, nothing is decoded.
// Output format comes from the outPath extension: .bmp, .png, .jpg/.jpeg,
// .tga. Prints "wrote <path> <w>x<h> <fmt>" on success; throws on error.
int runCapture(const Config& config, const std::string& outPath);
