// ─────────────────────────────────────────────────────────────────────────────
// gmix (Windows) — setup/debug utility for the GMix OBS plugin
// (obs-gmix-source). Windows port of linux-x86_64/src/main.cpp.
//
// GMix's actual capture+blend+render pipeline runs inside OBS as a plugin
// (src/obs_plugin/gmix_source.cpp) and inside the game as the injected
// capture DLL (proxy_dll/, loaded via gmix-inject.exe -- see inject.cpp's
// header comment); this binary only lists D3D11 adapters.
// ─────────────────────────────────────────────────────────────────────────────
#include "cli.hpp"
#include "d3d11/context.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <exception>
#include <iostream>

using namespace gmix;

namespace {

int doListGpus() {
    D3D11Context ctx;
    return ctx.init(-1) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    try {
        args = parseCli(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "gmix: %s\n", e.what());
        return 2;
    }

    if (args.help) {
        printUsage(std::cout);
        return 0;
    }
    if (args.listGpus) return doListGpus();

    printUsage(std::cout);
    return 0;
}
