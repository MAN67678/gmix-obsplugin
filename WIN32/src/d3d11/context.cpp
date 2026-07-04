#include "context.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gmix {

namespace {
std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}
} // namespace

D3D11Context::~D3D11Context() {
    if (device1_) device1_->Release();
    if (context_) context_->Release();
    if (device_)  device_->Release();
    for (auto& g : gpus_) if (g.adapter) g.adapter->Release();
    if (factory_) factory_->Release();
}

bool D3D11Context::enumerateAdapters() {
    if (CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory_)) != S_OK) {
        std::fprintf(stderr, "gmix: d3d11: CreateDXGIFactory1 failed\n");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (uint32_t i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        GpuInfo info;
        info.index = i;
        info.adapter = adapter;   // keep a ref; released in the destructor
        info.name = wideToUtf8(desc.Description);
        info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        gpus_.push_back(info);
    }
    return !gpus_.empty();
}

bool D3D11Context::pickAndCreate(int preferIndex, const int64_t* preferLuid) {
    int chosen = -1;
    if (preferLuid) {
        for (auto& g : gpus_) {
            DXGI_ADAPTER_DESC1 desc{};
            g.adapter->GetDesc1(&desc);
            int64_t luid = (static_cast<int64_t>(desc.AdapterLuid.HighPart) << 32) |
                           static_cast<uint32_t>(desc.AdapterLuid.LowPart);
            if (luid == *preferLuid) { chosen = static_cast<int>(g.index); break; }
        }
        if (chosen < 0) {
            std::fprintf(stderr, "gmix: d3d11: requested LUID not found among adapters -- "
                                  "falling back to auto-select\n");
        }
    }
    if (chosen < 0 && preferIndex >= 0 && static_cast<size_t>(preferIndex) < gpus_.size()) {
        chosen = preferIndex;
    }
    if (chosen < 0) {
        for (auto& g : gpus_) {
            if (!g.isSoftware) { chosen = static_cast<int>(g.index); break; }
        }
    }
    if (chosen < 0) chosen = 0;

    selected_ = gpus_[static_cast<size_t>(chosen)];

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(selected_.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device_, &got, &context_);
    // D3D11_CREATE_DEVICE_DEBUG requires the "Graphics Tools" optional
    // Windows feature; a Debug build on a machine without it fails device
    // creation entirely (DXGI_ERROR_SDK_COMPONENT_MISSING) rather than just
    // running undebugged. Retry once without the debug flag so a Debug build
    // still works -- only actual GPU/driver failures should be fatal here.
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        std::fprintf(stderr, "gmix: d3d11: debug layer unavailable (hr=0x%08lx) -- "
                              "retrying without D3D11_CREATE_DEVICE_DEBUG\n",
                     static_cast<unsigned long>(hr));
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(selected_.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device_, &got, &context_);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "gmix: d3d11: D3D11CreateDevice failed (hr=0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    // ID3D11Device1 is required for OpenSharedResourceByName (see
    // ImportedFrame.cpp) -- fail loudly rather than silently degrading, same
    // philosophy as the Linux side's explicit dmaBufCapable() check.
    if (device_->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1_)) != S_OK) {
        std::fprintf(stderr, "gmix: d3d11: ID3D11Device1 unavailable -- named shared-resource "
                              "import is not supported on this system\n");
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    selected_.adapter->GetDesc1(&desc);
    selectedLuid_ = (static_cast<int64_t>(desc.AdapterLuid.HighPart) << 32) |
                    static_cast<uint32_t>(desc.AdapterLuid.LowPart);
    return true;
}

bool D3D11Context::init(int preferIndex, const int64_t* preferLuid) {
    if (!enumerateAdapters()) return false;

    std::printf("d3d11 device found\n");
    for (auto& g : gpus_) {
        std::printf("[gpu%u] %s%s\n", g.index, g.name.c_str(), g.isSoftware ? " (software)" : "");
    }

    if (!pickAndCreate(preferIndex, preferLuid)) return false;
    std::printf("using [gpu%u]\n", selected_.index);
    return true;
}

} // namespace gmix
