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

// Open one shared D3D11 texture (by a HANDLE already DuplicateHandle'd into
// this process -- see frame_protocol.hpp's PROTOCOL HISTORY comment for why
// not a name) + its keyed mutex. Always closes `handleValue`: the raw Win32
// handle is only needed for the OpenSharedResource1 call itself, per the
// documented "close the handle once you have the resource" pattern -- D3D
// keeps its own reference. Returns nullptr on any failure.
std::shared_ptr<PooledTexture> openTextureByHandle(D3D11Context& ctx, uint32_t slot,
                                                   uint64_t handleValue, uint32_t dxgiFormat) {
    HANDLE h = reinterpret_cast<HANDLE>(handleValue);
    auto pt = std::make_shared<PooledTexture>();

    HRESULT hr = ctx.device1()->OpenSharedResource1(
        h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pt->tex));
    CloseHandle(h);
    if (FAILED(hr) || !pt->tex) {
        std::fprintf(stderr, "gmix: import: OpenSharedResource1 failed for slot %u "
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

std::shared_ptr<PooledTexture> FrameTexturePool::acquire(D3D11Context& ctx, uint32_t slot,
                                                         uint64_t sharedHandleValue,
                                                         uint32_t w, uint32_t h, uint32_t dxgiFormat) {
    if (w != w_ || h != h_ || dxgiFormat != fmt_) {
        slots_.clear();   // dimensions changed: drop the whole pool
        w_ = w; h_ = h; fmt_ = dxgiFormat;
    }
    // Guard against a corrupt/garbage slot (protocol mismatch) ballooning the
    // pool -- mirrors the Linux FrameImagePool's kMaxSlots guard.
    constexpr uint32_t kMaxSlots = 256;
    if (slot >= kMaxSlots) {
        return sharedHandleValue ? openTextureByHandle(ctx, slot, sharedHandleValue, dxgiFormat) : nullptr;
    }

    if (slot >= slots_.size()) slots_.resize(slot + 1);
    if (sharedHandleValue == 0) return slots_[slot];   // "already cached" -- may be null if producer erred

    auto pt = openTextureByHandle(ctx, slot, sharedHandleValue, dxgiFormat);
    if (pt) slots_[slot] = pt;
    return pt;
}

void FrameTexturePool::clear() {
    slots_.clear();
    w_ = h_ = fmt_ = 0;
}

bool ImportedFrame::init(std::shared_ptr<PooledTexture> tex, uint64_t acquireKey) {
    tex_ = std::move(tex);
    acquireKey_ = acquireKey;
    return tex_ && tex_->tex != nullptr;
}

} // namespace gmix
