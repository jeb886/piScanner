#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class Dedupe {
public:
    bool shouldEmit(const std::string& format, const std::string& text, std::uint64_t timestampUs,
                    std::uint64_t windowUs);

private:
    std::unordered_map<std::string, std::uint64_t> lastSeen_;
    std::uint64_t latestTimestampUs_ = 0;
};
