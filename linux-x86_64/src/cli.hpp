// ─────────────────────────────────────────────────────────────────────────────
// GMix — CLI parsing for the minimal `gmix` utility binary.
//
// GMix itself runs as an OBS Studio plugin (obs-gmix-source); this binary is
// just a small setup/debug tool used by config/gmix_launch.sh and for
// diagnostics -- it does not capture, blend, or output anything itself.
//
// Flags:
//   --install-layer   one-time: install VkLayer_GMIX to ~/.local/share/vulkan/implicit_layer.d
//   --list-gpus       print enumerated Vulkan devices and exit
//   --attach          connect to a running capture layer's debug IPC and print messages
//   -h, --help
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <iosfwd>
#include <string>

namespace gmix {

struct CliArgs {
    bool installLayer = false;
    bool listGpus     = false;
    bool attach       = false;
    bool help         = false;
};

// Parse argv. Exits the process with a message on hard errors
// (missing value, unknown flag). Returns CliArgs on success.
CliArgs parseCli(int argc, char** argv);

// Print usage to stream.
void printUsage(std::ostream& os);

} // namespace gmix
