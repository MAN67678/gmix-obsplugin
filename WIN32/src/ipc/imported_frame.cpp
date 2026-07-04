#include "imported_frame.hpp"
#include "frame_protocol.hpp"
#include "../d3d11/context.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <cstdio>

namespace gmix {

PooledTexture::~PooledTexture() {
    if (mutex) mutex->Release();
    if (srv)   srv->Release();
    if (tex)   tex->Release();
}

namespace {

// Open one named shared D3D11 texture + its keyed mutex. Returns nullptr on
// any failure (name not found yet -- e.g. the producer hasn't created this
// slot -- or an adapter/LUID mismatch between producer and consumer devices).
std::shared_ptr<PooledTexture> openTextureByName(D3D11Context& ctx, uint32_t producerPid,
                                                 uint32_t slot, uint32_t dxgiFormat) {
    auto name = ipc::sharedTextureName(producerPid, slot);
    auto pt = std::make_shared<PooledTexture>();

    HRESULT hr = ctx.device1()->OpenSharedResourceByName(
        name.c_str(), DXGI_SHARED_RESOURCE_READ,
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pt->tex));
    if (FAILED(hr) || !pt->tex) {
        std::fprintf(stderr, "gmix: import: OpenSharedResourceByName failed for slot %u "
                              "(hr=0x%08lx) -- producer/consumer on different GPUs?\n",
                     slot, static_cast<unsigned long>(hr));
        return nullptr;
    }

    if (pt->tex->QueryInterface(__uuidof(IDXGIKeyedMutex),
                               reinterpret_cast<void**>(&pt->mutex)) != S_OK || !pt->mutex) {
        std::fprintf(stderr, "gmix: import: slot %u has no IDXGIKeyedMutex\n", slot);
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (ctx.device()->CreateShaderResourceView(pt->tex, &svd, &pt->srv) != S_OK) {
        std::fprintf(stderr, "gmix: import: SRV create failed for slot %u\n", slot);
        return nullptr;
    }
    return pt;
}

} // namespace

std::shared_ptr<PooledTexture> FrameTexturePool::acquire(D3D11Context& ctx, uint32_t producerPid,
                                                         uint32_t slot, uint32_t w, uint32_t h,
                                                         uint32_t dxgiFormat) {
    if (producerPid != producerPid_ || w != w_ || h != h_ || dxgiFormat != fmt_) {
        slots_.clear();   // producer restarted or dimensions changed: drop the whole pool
        producerPid_ = producerPid; w_ = w; h_ = h; fmt_ = dxgiFormat;
    }
    // Guard against a corrupt/garbage slot (protocol mismatch) ballooning the
    // pool -- mirrors the Linux FrameImagePool's kMaxSlots guard.
    constexpr uint32_t kMaxSlots = 256;
    if (slot >= kMaxSlots) return openTextureByName(ctx, producerPid, slot, dxgiFormat);

    if (slot >= slots_.size()) slots_.resize(slot + 1);
    if (slots_[slot]) return slots_[slot];   // already opened this slot: reuse it

    auto pt = openTextureByName(ctx, producerPid, slot, dxgiFormat);
    if (pt) slots_[slot] = pt;
    return pt;
}

void FrameTexturePool::clear() {
    slots_.clear();
    producerPid_ = w_ = h_ = fmt_ = 0;
}

bool ImportedFrame::init(std::shared_ptr<PooledTexture> tex, uint64_t acquireKey) {
    tex_ = std::move(tex);
    acquireKey_ = acquireKey;
    return tex_ && tex_->tex != nullptr;
}

} // namespace gmix
