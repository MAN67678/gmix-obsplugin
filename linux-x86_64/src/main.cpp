// ─────────────────────────────────────────────────────────────────────────────
// gmix — setup/debug utility for the GMix OBS plugin (obs-gmix-source).
//
// GMix's actual capture+blend+render pipeline runs inside OBS as a plugin
// (src/obs_plugin/gmix_source.cpp); this binary has no capture loop, no
// output sink, and no blend dispatch of its own. It only handles: installing
// the Vulkan capture layer, listing GPUs, and a debug IPC client mode.
// ─────────────────────────────────────────────────────────────────────────────
#include "cli.hpp"
#include "vulkan/context.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

// ipc client entry (defined in src/ipc/ipc_client.cpp)
int mainClient();

using namespace gmix;

namespace {

// One-time: install VkLayer_GMIX manifest + .so into the user's implicit
// layer directory so any Vulkan app launched with ENABLE_GMIX=1 picks it up.
// The .so path written into the JSON is absolute and points at the installed
// location; --install-layer re-runs to refresh it.
int doInstallLayer() {
    std::filesystem::path exePath;
    try {
        exePath = std::filesystem::read_symlink("/proc/self/exe");
    } catch (...) {
        std::fprintf(stderr, "gmix: cannot determine executable path\n");
        return 1;
    }
    std::filesystem::path buildDir = exePath.parent_path();
    std::filesystem::path layerSrcDir = buildDir / "layer";
    std::filesystem::path srcSo = layerSrcDir / "VkLayer_GMIX.so";
    std::filesystem::path srcJson = layerSrcDir / "VkLayer_GMIX.json";
    if (!std::filesystem::exists(srcSo) || !std::filesystem::exists(srcJson)) {
        std::fprintf(stderr, "gmix: build output not found in %s (expecting layer/)\n", buildDir.c_str());
        return 1;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        std::fprintf(stderr, "gmix: HOME not set\n");
        return 1;
    }
    std::filesystem::path destDir = std::filesystem::path(home) / ".local" / "share" / "vulkan" / "implicit_layer.d";
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        std::fprintf(stderr, "gmix: cannot create %s: %s\n", destDir.c_str(), ec.message().c_str());
        return 1;
    }

    std::filesystem::path destSo = destDir / "VkLayer_GMIX.so";
    std::filesystem::copy_file(srcSo, destSo, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "gmix: cannot copy %s to %s: %s\n", srcSo.c_str(), destSo.c_str(), ec.message().c_str());
        return 1;
    }

    // Compose a manifest that points to the installed .so. Use the stable
    // manifest filename `gmix_capture.json` to avoid colliding with other
    // layer installers.
    std::filesystem::path destJson = destDir / "gmix_capture.json";
    std::ofstream out(destJson);
    if (!out) {
        std::fprintf(stderr, "gmix: cannot write manifest %s\n", destJson.c_str());
        return 1;
    }
    out << "{\n"
        << "  \"file_format_version\": \"1.0.0\",\n"
        << "  \"layer\": {\n"
        << "    \"name\": \"VK_LAYER_GMIX\",\n"
        << "    \"type\": \"GLOBAL\",\n"
        << "    \"library_path\": \"" << destSo.string() << "\",\n"
        << "    \"api_version\": \"1.2.0\",\n"
        << "    \"implementation_version\": \"1\",\n"
        << "    \"description\": \"GMix temporal frame capture (osu!/Vulkan)\",\n"
        << "    \"enable_environment\": { \"ENABLE_GMIX\": \"1\" },\n"
        << "    \"disable_environment\": { \"DISABLE_GMIX\": \"1\" },\n"
        << "    \"functions\": {\n"
        << "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\"\n"
        << "    }\n"
        << "  }\n"
        << "}\n";
    out.close();

    std::printf("gmix: installed manifest %s and library %s\n", destJson.c_str(), destSo.c_str());
    return 0;
}

int doListGpus(VulkanContext& vk) {
    if (!vk.init(-1)) return 1;
    return 0;
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
    if (args.installLayer) return doInstallLayer();
    if (args.attach)       return mainClient();

    VulkanContext vk;
    if (args.listGpus) return doListGpus(vk);

    printUsage(std::cout);
    return 0;
}
