#pragma once

#include <ostream>
#include <string>
#include <vector>

struct Config;

// `piscan decode`: decode each image file (single-threaded, deterministic)
// and print one line per file. With Config.expect, compares to <name>.txt
// sidecars ("NONE" means no barcode expected) and returns nonzero on any
// mismatch or missing sidecar.
int runDecodeFiles(const Config& config, std::ostream& output);
