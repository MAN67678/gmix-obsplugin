#include "blend_engine.hpp"
#include "../d3d11/context.hpp"
#include "../ipc/imported_frame.hpp"
#include "../gmix.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace gmix {

namespace {

// Mirrors blend.hlsl's `cbuffer PushConstants`.
struct alignas(16) PushConstants {
    uint32_t frameCount;
    uint32_t frameW;
    uint32_t frameH;
    uint32_t _pad0;
};

std::vector<char> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<char> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    return buf;
}

} // namespace

BlendEngine::BlendEngine(D3D11Context& ctx) : ctx_(ctx) {}

BlendEngine::~BlendEngine() { destroyAll(); }

void BlendEngine::destroyAll() {
    if (eventQuery_) { eventQuery_->Release(); eventQuery_ = nullptr; }
    blendInFlight_ = false;
    if (pcBuf_)      { pcBuf_->Release(); pcBuf_ = nullptr; }
    if (weightsSrv_) { weightsSrv_->Release(); weightsSrv_ = nullptr; }
    if (weightsBuf_) { weightsBuf_->Release(); weightsBuf_ = nullptr; }
    for (uint32_t i = 0; i < kDstBuffers; ++i) {
        // Shared handles returned by GetSharedHandle are owned by the
        // resource, not separately allocated -- nothing to close here.
        dstSharedHandle_[i] = nullptr;
        if (dstUav_[i]) { dstUav_[i]->Release(); dstUav_[i] = nullptr; }
        if (dstTex_[i]) { dstTex_[i]->Release(); dstTex_[i] = nullptr; }
    }
    sharedCapable_ = false;
    if (cs_) { cs_->Release(); cs_ = nullptr; }
    initialized_ = false;
}

bool BlendEngine::init(uint32_t w, uint32_t h) {
    if (initialized_) destroyAll();
    width_ = w; height_ = h;

    if (!createDstTextures())   return false;
    if (!createWeightsBuffer()) return false;
    if (!createComputeShader()) return false;

    ID3D11Device* dev = ctx_.device();
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    if (dev->CreateQuery(&qd, &eventQuery_) != S_OK) {
        std::fprintf(stderr, "gmix: blend: event query create failed\n");
        return false;
    }

    initialized_ = true;
    return true;
}

bool BlendEngine::createDstTextures() {
    ID3D11Device* dev = ctx_.device();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // Matches the shader's RWTexture2D<float4> declaration; the runtime
    // handles UNORM<->float conversion on typed load/store automatically.
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    // Legacy (non-NT) shared handle: gmix_source.cpp opens this from a
    // SEPARATE D3D11 device (OBS's own) but the SAME process, so a plain
    // GetSharedHandle() is sufficient -- no name/NT-handle cross-process
    // machinery needed here (contrast with the producer's export ring in
    // gl_dx_interop_capture.cpp, which genuinely crosses processes).
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    sharedCapable_ = true;
    for (uint32_t i = 0; i < kDstBuffers; ++i) {
        if (dev->CreateTexture2D(&td, nullptr, &dstTex_[i]) != S_OK) {
            std::fprintf(stderr, "gmix: blend: dst texture create failed\n");
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = td.Format;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (dev->CreateUnorderedAccessView(dstTex_[i], &ud, &dstUav_[i]) != S_OK) {
            std::fprintf(stderr, "gmix: blend: dst UAV create failed\n");
            return false;
        }

        IDXGIResource* dxgiRes = nullptr;
        if (dstTex_[i]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes)) == S_OK) {
            HANDLE h = nullptr;
            if (dxgiRes->GetSharedHandle(&h) == S_OK) {
                dstSharedHandle_[i] = h;
            } else {
                std::fprintf(stderr, "gmix: blend: GetSharedHandle failed for dst[%u] -- "
                                      "zero-copy delivery to OBS is unavailable\n", i);
                sharedCapable_ = false;
            }
            dxgiRes->Release();
        } else {
            sharedCapable_ = false;
        }
    }
    return true;
}

bool BlendEngine::createWeightsBuffer() {
    ID3D11Device* dev = ctx_.device();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(float) * kMaxBlendFrames;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(float);
    if (dev->CreateBuffer(&bd, nullptr, &weightsBuf_) != S_OK) {
        std::fprintf(stderr, "gmix: blend: weights buffer create failed\n");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = kMaxBlendFrames;
    if (dev->CreateShaderResourceView(weightsBuf_, &sd, &weightsSrv_) != S_OK) {
        std::fprintf(stderr, "gmix: blend: weights SRV create failed\n");
        return false;
    }

    D3D11_BUFFER_DESC pcbd{};
    pcbd.ByteWidth = sizeof(PushConstants);
    pcbd.Usage = D3D11_USAGE_DYNAMIC;
    pcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    pcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (dev->CreateBuffer(&pcbd, nullptr, &pcBuf_) != S_OK) {
        std::fprintf(stderr, "gmix: blend: push-constant buffer create failed\n");
        return false;
    }
    return true;
}

bool BlendEngine::createComputeShader() {
    ID3D11Device* dev = ctx_.device();
    // GMIX_SHADER_DIR is the build/shaders directory, set via a compile def
    // (same pattern as the Linux side's GMIX_SHADER_DIR for blend.spv).
    auto code = readFile(GMIX_SHADER_DIR "\\blend.cso");
    if (code.empty()) {
        std::fprintf(stderr, "gmix: blend: cannot read " GMIX_SHADER_DIR "\\blend.cso\n");
        return false;
    }
    if (dev->CreateComputeShader(code.data(), code.size(), nullptr, &cs_) != S_OK) {
        std::fprintf(stderr, "gmix: blend: compute shader create failed\n");
        return false;
    }
    return true;
}

bool BlendEngine::dispatchAsync(ImportedFrame* const* frames, const float* weights,
                                uint32_t count, uint32_t dstIdx) {
    if (!initialized_) return false;
    if (count == 0 || count > static_cast<uint32_t>(kMaxBlendFrames)) return false;
    if (dstIdx >= kDstBuffers) return false;

    ID3D11DeviceContext* ic = ctx_.immediateContext();

    // ── Acquire each source frame's ring-slot keyed mutex ───────────────────
    // Short timeout: by the time a FrameHeader has arrived over the pipe the
    // producer has already released this slot, so acquisition should be
    // near-instant. A real timeout here means the producer stalled or died --
    // skip the whole dispatch rather than hanging the consumer's tick loop.
    constexpr DWORD kAcquireTimeoutMs = 50;
    std::vector<uint32_t> acquired(count, 0);
    bool allAcquired = true;
    for (uint32_t i = 0; i < count; ++i) {
        auto* mutex = frames[i]->keyedMutex();
        if (!mutex || mutex->AcquireSync(frames[i]->acquireKey(), kAcquireTimeoutMs) != S_OK) {
            allAcquired = false;
            break;
        }
        acquired[i] = 1;
    }
    if (!allAcquired) {
        for (uint32_t i = 0; i < count; ++i) {
            if (acquired[i]) frames[i]->keyedMutex()->ReleaseSync(ImportedFrame::kHandBackKey);
        }
        return false;
    }

    // ── Push weights into the dynamic buffer ────────────────────────────────
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (ic->Map(weightsBuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) != S_OK) {
        for (uint32_t i = 0; i < count; ++i) frames[i]->keyedMutex()->ReleaseSync(ImportedFrame::kHandBackKey);
        return false;
    }
    std::memcpy(mapped.pData, weights, sizeof(float) * count);
    ic->Unmap(weightsBuf_, 0);

    // ── Bind resources and dispatch ──────────────────────────────────────────
    std::vector<ID3D11ShaderResourceView*> srcSrvs(kMaxBlendFrames, nullptr);
    for (uint32_t i = 0; i < count; ++i) srcSrvs[i] = frames[i]->srv();
    ic->CSSetShaderResources(0, kMaxBlendFrames, srcSrvs.data());
    ic->CSSetShaderResources(64, 1, &weightsSrv_);
    ID3D11UnorderedAccessView* uav = dstUav_[dstIdx];
    ic->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ic->CSSetShader(cs_, nullptr, 0);

    PushConstants pc{ count, width_, height_, 0 };
    D3D11_MAPPED_SUBRESOURCE pcMapped{};
    ic->Map(pcBuf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &pcMapped);
    std::memcpy(pcMapped.pData, &pc, sizeof(pc));
    ic->Unmap(pcBuf_, 0);
    ic->CSSetConstantBuffers(0, 1, &pcBuf_);

    uint32_t gx = (width_  + 7) / 8;
    uint32_t gy = (height_ + 7) / 8;
    ic->Dispatch(gx, gy, 1);

    // Unbind so the next tick's descriptor writes don't hazard against a
    // still-bound resource (D3D11 debug layer flags read/write-after-bind
    // otherwise), then hand the ring slots back to the producer. Per
    // IDXGIKeyedMutex semantics, ReleaseSync only becomes GPU-visible once
    // the commands issued above that reference the resource have completed --
    // it's safe to call immediately without waiting on the CPU.
    ID3D11ShaderResourceView* nullSrvs[kMaxBlendFrames] = {};
    ic->CSSetShaderResources(0, kMaxBlendFrames, nullSrvs);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ic->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    for (uint32_t i = 0; i < count; ++i) frames[i]->keyedMutex()->ReleaseSync(ImportedFrame::kHandBackKey);

    ic->End(eventQuery_);
    blendInFlight_ = true;
    return true;
}

bool BlendEngine::pollBlendDone() {
    if (!blendInFlight_) return false;
    BOOL done = FALSE;
    HRESULT hr = ctx_.immediateContext()->GetData(eventQuery_, &done, sizeof(done), 0);
    if (hr == S_OK && done) {
        blendInFlight_ = false;
        return true;
    }
    return false;
}

void BlendEngine::waitBlendDone() {
    if (!blendInFlight_) return;
    BOOL done = FALSE;
    while (ctx_.immediateContext()->GetData(eventQuery_, &done, sizeof(done), 0) != S_OK || !done) {
        Sleep(0);
    }
    blendInFlight_ = false;
}

} // namespace gmix
