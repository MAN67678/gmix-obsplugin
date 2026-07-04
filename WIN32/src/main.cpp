// ─────────────────────────────────────────────────────────────────────────────
// gmix (Windows) — setup/debug utility for the GMix OBS plugin
// (obs-gmix-source). Windows port of linux-x86_64/src/main.cpp.
//
// GMix's actual capture+blend+render pipeline runs inside OBS as a plugin
// (src/obs_plugin/gmix_source.cpp) and inside the game as the proxy DLL
// (proxy_dll/); this binary only handles installing the proxy DLL pair next
// to a game's exe and listing D3D11 adapters.
// ─────────────────────────────────────────────────────────────────────────────
#include "cli.hpp"
#include "d3d11/context.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>

using namespace gmix;

namespace {

// Copies <exe-dir>/proxy/opengl32.dll (our capture proxy, built by the
// proxy_dll target) and a fresh copy of the real system opengl32.dll (renamed
// to opengl32_orig.dll -- see generate_def.ps1's header comment for why a
// distinct filename is required) into the target game directory.
int doInstallProxy(const std::string& gameDir) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path buildDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path proxySrc = buildDir / "proxy" / "opengl32.dll";
    if (!std::filesystem::exists(proxySrc)) {
        std::fprintf(stderr, "gmix: build output not found at %ls (expecting proxy/opengl32.dll)\n",
                     proxySrc.c_str());
        return 1;
    }

    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::filesystem::path realOpenGL32 = std::filesystem::path(sysDir) / "opengl32.dll";
    if (!std::filesystem::exists(realOpenGL32)) {
        std::fprintf(stderr, "gmix: real opengl32.dll not found at %ls\n", realOpenGL32.c_str());
        return 1;
    }

    std::filesystem::path destDir(gameDir);
    std::error_code ec;
    if (!std::filesystem::exists(destDir, ec) || !std::filesystem::is_directory(destDir, ec)) {
        std::fprintf(stderr, "gmix: %s is not a directory\n", gameDir.c_str());
        return 1;
    }

    std::filesystem::copy_file(proxySrc, destDir / "opengl32.dll",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "gmix: failed to copy proxy DLL: %s\n", ec.message().c_str());
        return 1;
    }
    std::filesystem::copy_file(realOpenGL32, destDir / "opengl32_orig.dll",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "gmix: failed to copy real opengl32.dll: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("gmix: installed opengl32.dll + opengl32_orig.dll into %s\n", gameDir.c_str());
    std::printf("gmix: launch the game normally -- capture activates once an OBS \"GMix Motion Blur\" "
                "source is added (it writes %%LOCALAPPDATA%%\\gmix\\target_process before the game starts)\n");
    return 0;
}

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
    if (!args.installProxyDir.empty()) return doInstallProxy(args.installProxyDir);
    if (args.listGpus) return doListGpus();

    printUsage(std::cout);
    return 0;
}
