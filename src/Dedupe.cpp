#include "Dedupe.h"

#include <algorithm>

bool Dedupe::shouldEmit(const std::string& format, const std::string& text, std::uint64_t timestampUs,
                        std::uint64_t windowUs)
{
    const auto key = format + '\0' + text;
    if (windowUs == 0)
        return true;
    latestTimestampUs_ = std::max(latestTimestampUs_, timestampUs);
    for (auto current = lastSeen_.begin(); current != lastSeen_.end();) {
        if (latestTimestampUs_ >= current->second && latestTimestampUs_ - current->second > windowUs)
            current = lastSeen_.erase(current);
        else
            ++current;
    }
    const auto it = lastSeen_.find(key);
    if (it != lastSeen_.end()) {
        const auto difference = timestampUs >= it->second ? timestampUs - it->second : it->second - timestampUs;
        if (difference <= windowUs)
            return false;
    }
    lastSeen_[key] = std::max(timestampUs, it == lastSeen_.end() ? 0 : it->second);
    return true;
}
