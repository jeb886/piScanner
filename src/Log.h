#pragma once

#include <string>

// Optional append-only file log for runtime debugging (--log-file).
// logLine() is a cheap no-op until logInit() opens the file, and is safe
// to call from any thread. The file is rotated once to <path>.1 past a
// small cap: the service logs into a tmpfs dir (/run/piscanner), so growth
// must stay bounded. Messages are meant to be brief and infrequent.
void logInit(const std::string& path);
bool logEnabled();
void logLine(const std::string& message);
