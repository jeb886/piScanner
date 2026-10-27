#include "Log.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <time.h>

namespace {

const long maxLogBytes = 256 * 1024;

std::mutex mutex;
FILE* file = nullptr;
std::string filePath;
long fileBytes = 0;

std::string timestamp()
{
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    tm local{};
    localtime_r(&now.tv_sec, &local);
    char text[64];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d.%03d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(now.tv_nsec / 1000000));
    return text;
}

} // namespace

void logInit(const std::string& path)
{
    std::lock_guard lock(mutex);
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    FILE* opened = std::fopen(path.c_str(), "a");
    if (opened == nullptr)
        throw std::runtime_error("cannot open log file " + path);
    file = opened;
    filePath = path;
    fileBytes = std::ftell(file);
    if (fileBytes < 0)
        fileBytes = 0;
}

bool logEnabled()
{
    std::lock_guard lock(mutex);
    return file != nullptr;
}

void logLine(const std::string& message)
{
    std::lock_guard lock(mutex);
    if (file == nullptr)
        return;
    if (fileBytes > maxLogBytes) {
        std::fclose(file);
        std::rename(filePath.c_str(), (filePath + ".1").c_str());
        file = std::fopen(filePath.c_str(), "w");
        fileBytes = 0;
        if (file == nullptr)
            return;
    }
    const std::string line = timestamp() + ' ' + message + '\n';
    fileBytes += static_cast<long>(std::fwrite(line.data(), 1, line.size(), file));
    std::fflush(file);
}
