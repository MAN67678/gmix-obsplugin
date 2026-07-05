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
        "  gmix --help\n"
        "\n"
        "To activate capture in a running game, use gmix-inject.exe (see capture/\n"
        "next to this build's output) -- it loads GmixCapture.dll into the game\n"
        "process via runtime injection; nothing needs to be installed/copied into\n"
        "the game's own directory.\n";
}

} // namespace gmix
