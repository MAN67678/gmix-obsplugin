#include "cli.hpp"

#include <cstring>
#include <ostream>
#include <stdexcept>

namespace gmix {

CliArgs parseCli(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            args.help = true;
        } else if (a == "--list-gpus") {
            args.listGpus = true;
        } else if (a == "--install-proxy") {
            if (i + 1 >= argc) throw std::invalid_argument("--install-proxy requires a directory argument");
            args.installProxyDir = argv[++i];
        } else if (a == "--gpu") {
            if (i + 1 >= argc) throw std::invalid_argument("--gpu requires an index argument");
            args.gpuIndex = std::stoi(argv[++i]);
        } else {
            throw std::invalid_argument("unknown argument: " + a);
        }
    }
    return args;
}

void printUsage(std::ostream& os) {
    os <<
        "gmix (Windows) -- setup/debug utility for the GMix OBS plugin\n"
        "\n"
        "Usage:\n"
        "  gmix --list-gpus\n"
        "      List D3D11 adapters (same numbering the OBS plugin's GPU index setting uses).\n"
        "\n"
        "  gmix --install-proxy <game-directory> [--gpu N]\n"
        "      Install the capture proxy (opengl32.dll + a renamed copy of the real\n"
        "      driver DLL, opengl32_orig.dll) into <game-directory> (e.g. osu!'s\n"
        "      install folder). Re-run any time the game updates its own files.\n"
        "\n"
        "  gmix --help\n";
}

} // namespace gmix
