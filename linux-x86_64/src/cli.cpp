#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace gmix {

void printUsage(std::ostream& os) {
    os <<
        "gmix — setup/debug utility for the GMix OBS plugin\n"
        "\n"
        "GMix itself runs as an OBS Studio plugin (obs-gmix-source), added as a\n"
        "source in OBS. This binary only handles one-time setup and diagnostics.\n"
        "\n"
        "USAGE:\n"
        "    gmix --install-layer\n"
        "\n"
        "FLAGS:\n"
        "    --install-layer   install VkLayer_GMIX to ~/.local/share/vulkan/implicit_layer.d\n"
        "    --list-gpus       enumerate Vulkan devices and exit\n"
        "    --attach          connect to a running capture layer's debug IPC and print messages\n"
        "    -h, --help        show this message\n"
        "\n"
        "GMIX layer activation (see config/gmix_launch.sh for the full setup):\n"
        "    ENABLE_GMIX=1 ./my_vulkan_app\n";
}

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "gmix: %s\n", msg.c_str());
    std::fprintf(stderr, "run 'gmix --help' for usage.\n");
    std::exit(2);
}

} // namespace

CliArgs parseCli(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")   { a.help = true; return a; }
        if (arg == "--install-layer")         { a.installLayer = true; continue; }
        if (arg == "--list-gpus")             { a.listGpus = true;     continue; }
        if (arg == "--attach")                { a.attach = true;       continue; }
        fail("unknown argument: '" + arg + "'");
    }
    return a;
}

} // namespace gmix
