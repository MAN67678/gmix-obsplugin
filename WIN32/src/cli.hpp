#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace gmix {

struct CliArgs {
    bool help = false;
    bool listGpus = false;
    // Directory to install the capture proxy into (e.g. osu!'s install dir).
    // Empty = --install-proxy not requested.
    std::string installProxyDir;
    int32_t gpuIndex = -1;
};

CliArgs parseCli(int argc, char** argv);
void printUsage(std::ostream& os);

} // namespace gmix
