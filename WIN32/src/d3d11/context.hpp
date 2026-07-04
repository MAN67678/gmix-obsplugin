// ─────────────────────────────────────────────────────────────────────────────
// GMix D3D11 context (Windows) — owns the consumer-side ID3D11Device/Context
// and exposes adapter enumeration + selection. D3D11 analogue of
// linux-x86_64/src/vulkan/context.{hpp,cpp}.
//
// Unlike the Vulkan side there is no separate "async compute queue family" --
// D3D11 has a single immediate context per device, so the blend dispatch and
// (if this process ever also presents) any graphics work share it. This is a
// real simplification vs. the Vulkan pipelined-queue design, acceptable here
// because the consumer's only D3D11 work is the compute blend; OBS owns and
// paces the actual presentation via its own device/swapchain (see
// obs_plugin/gmix_source.cpp -- OBS's D3D11 device is a SEPARATE device from
// this one; interop happens via the shared-texture NT handle round-trip
// through gs_texture_open_shared, not a shared device).
//
// enumerateAndPrint() produces output analogous to the Linux spec'd startup
// banner:
//
//     d3d11 device found
//     [gpu0] NVIDIA GeForce RTX 3060 (dedicated)
//     [gpu1] Microsoft Basic Render Driver (software)
//     using [gpu0]
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Device1;
struct ID3D11DeviceContext;
struct IDXGIAdapter1;
struct IDXGIFactory1;

namespace gmix {

struct GpuInfo {
    uint32_t    index = 0;
    IDXGIAdapter1* adapter = nullptr;   // owned by VulkanContext-equivalent below
    std::string name;
    bool        isSoftware = false;
    uint64_t    dedicatedVideoMemory = 0;
};

// D3D11 requires shared textures + their producer/consumer devices to be
// created on the SAME physical adapter (LUID) -- a shared NT handle opened on
// a different adapter's device simply fails. gpuIndex therefore must be
// chosen to match whatever adapter the capture DLL's host process (the game)
// is actually rendering on; see WIN32/etc/DEV_NOTES.md for how the proxy DLL
// discovers and reports that LUID so the OBS plugin can match it.
class D3D11Context {
public:
    D3D11Context() = default;
    ~D3D11Context();

    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    // Enumerate adapters and create a device on the selected one. Prints the
    // adapter list + selection to stdout/OBS log, matching the Linux side's
    // banner. `preferIndex` = -1 -> auto (first non-software, non-Basic-
    // Render-Driver adapter). `preferLuid`, if non-null, overrides
    // preferIndex and picks the adapter whose LUID matches exactly (used to
    // pin the OBS-side device to the same GPU the game is rendering on).
    bool init(int preferIndex, const int64_t* preferLuid = nullptr);

    ID3D11Device*        device()  const { return device_; }
    // ID3D11Device1 is required for OpenSharedResourceByName; guaranteed
    // present on Windows 8+ (Platform Update for Windows 7 lacking it is out
    // of scope). Kept as a separate accessor so call sites that need the
    // by-name open are explicit about the requirement.
    ID3D11Device1*        device1() const { return device1_; }
    ID3D11DeviceContext* immediateContext() const { return context_; }
    const GpuInfo&        selected() const { return selected_; }
    const std::vector<GpuInfo>& gpus() const { return gpus_; }
    int64_t                selectedLuid() const { return selectedLuid_; }

private:
    bool enumerateAdapters();
    bool pickAndCreate(int preferIndex, const int64_t* preferLuid);

    IDXGIFactory1*        factory_ = nullptr;
    ID3D11Device*         device_  = nullptr;
    ID3D11Device1*        device1_ = nullptr;
    ID3D11DeviceContext*  context_ = nullptr;
    std::vector<GpuInfo>  gpus_;
    GpuInfo                selected_;
    int64_t                selectedLuid_ = 0;
};

} // namespace gmix
