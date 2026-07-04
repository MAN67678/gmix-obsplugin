// ─────────────────────────────────────────────────────────────────────────────
// Standalone validation of the D3D11 blend compute pipeline on the real GPU.
// Analogue of linux-x86_64/tests/test_blend_engine.cpp.
//
// Creates N synthetic solid-color source textures (each with a keyed mutex,
// matching the shape ImportedFrame expects -- see ipc/imported_frame.hpp),
// dispatches the blend shader with known weights, reads back the dst
// texture, and compares against a CPU reference of the same weighted
// accumulate.
//
// Run: test_blend_engine.exe
// Pass if every output pixel is within RGBA8 rounding tolerance (<=1/255).
// ─────────────────────────────────────────────────────────────────────────────
#include "test_macros.hpp"
#include "d3d11/context.hpp"
#include "blend/blend_engine.hpp"
#include "ipc/imported_frame.hpp"
#include "../src/gmix.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gmix;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;

struct Col { uint8_t r, g, b, a; };

// Builds a solid-color source texture wrapped exactly as the real IPC path
// would produce it (a PooledTexture with tex/srv/mutex populated -- see
// FrameTexturePool::acquire in ipc/imported_frame.cpp), then simulates the
// producer's half of the keyed-mutex handoff (Acquire the initially-available
// key 0, write, Release with key 1) so the returned frame is immediately
// ready for BlendEngine::dispatchAsync() to consume, exactly like a real
// frame that just arrived over the named pipe.
std::shared_ptr<ImportedFrame> makeSrcFrame(D3D11Context& ctx, uint32_t w, uint32_t h, Col c) {
    auto pt = std::make_shared<PooledTexture>();
    ID3D11Device* dev = ctx.device();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Keyed mutex requires a SHARED misc flag even though this test never
    // actually crosses a process boundary -- same requirement production
    // ring textures have (see gl_dx_interop_capture.cpp's ensureRing()).
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (dev->CreateTexture2D(&td, nullptr, &pt->tex) != S_OK) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (dev->CreateShaderResourceView(pt->tex, &svd, &pt->srv) != S_OK) return nullptr;

    if (pt->tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&pt->mutex)) != S_OK)
        return nullptr;

    // Simulate the producer: key 0 is available on a freshly created keyed
    // mutex; MUST acquire it BEFORE writing -- a KEYEDMUTEX resource written
    // without holding the mutex is silently dropped by the driver (confirmed
    // empirically: an UpdateSubresource issued before the first AcquireSync
    // on this exact GPU/driver left the texture at its zero-initialized
    // default, which looked identical to a compute-shader read bug until
    // isolated). Acquire, write, then release with key 1 -- the value
    // ImportedFrame::acquireKey() below tells the blend engine to wait for.
    if (pt->mutex->AcquireSync(0, 1000) != S_OK) return nullptr;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (uint32_t i = 0; i < w * h; ++i) {
        px[i * 4 + 0] = c.r; px[i * 4 + 1] = c.g; px[i * 4 + 2] = c.b; px[i * 4 + 3] = c.a;
    }
    ctx.immediateContext()->UpdateSubresource(pt->tex, 0, nullptr, px.data(), w * 4, 0);
    pt->mutex->ReleaseSync(1);

    auto frame = std::make_shared<ImportedFrame>();
    if (!frame->init(pt, /*acquireKey=*/1)) return nullptr;
    return frame;
}

bool readbackTexture(D3D11Context& ctx, ID3D11Texture2D* tex, uint32_t w, uint32_t h,
                     std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (ctx.device()->CreateTexture2D(&desc, nullptr, &staging) != S_OK) return false;
    ctx.immediateContext()->CopyResource(staging, tex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (ctx.immediateContext()->Map(staging, 0, D3D11_MAP_READ, 0, &mapped) != S_OK) {
        staging->Release();
        return false;
    }
    out.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4,
                   static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                   static_cast<size_t>(w) * 4);
    }
    ctx.immediateContext()->Unmap(staging, 0);
    staging->Release();
    return true;
}

float toF(uint8_t v) { return v / 255.0f; }
uint8_t toU8(float v) {
    int x = static_cast<int>(std::round(v * 255.0f));
    return static_cast<uint8_t>(std::clamp(x, 0, 255));
}

} // namespace

TEST_CASE(blend_engine_matches_cpu_reference) {
    D3D11Context ctx;
    if (!ctx.init(-1)) { CHECK(!"d3d11 init failed"); return; }

    BlendEngine blend(ctx);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed"); return; }
    CHECK(blend.sharedCapable());

    const Col cols[4] = {
        {255,   0,   0, 255},   // red
        {  0, 255,   0, 255},   // green
        {  0,   0, 255, 255},   // blue
        {255, 255, 255, 255},   // white
    };

    std::vector<std::shared_ptr<ImportedFrame>> owners(4);
    std::vector<ImportedFrame*> frames(4);
    for (int i = 0; i < 4; ++i) {
        owners[i] = makeSrcFrame(ctx, kW, kH, cols[i]);
        if (!owners[i]) { CHECK(!"src frame create failed"); return; }
        frames[i] = owners[i].get();
    }

    float weights[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
    if (!blend.dispatchAsync(frames.data(), weights, 4, 0)) { CHECK(!"dispatch failed"); return; }
    blend.waitBlendDone();

    std::vector<uint8_t> dst;
    if (!readbackTexture(ctx, blend.dstTexture(0), kW, kH, dst)) { CHECK(!"readback failed"); return; }

    float refR = 0, refG = 0, refB = 0, refA = 0;
    for (int i = 0; i < 4; ++i) {
        refR += weights[i] * toF(cols[i].r);
        refG += weights[i] * toF(cols[i].g);
        refB += weights[i] * toF(cols[i].b);
        refA += weights[i] * toF(cols[i].a);
    }
    uint8_t expR = toU8(refR), expG = toU8(refG), expB = toU8(refB), expA = toU8(refA);

    int mismatches = 0;
    for (uint32_t p = 0; p < kW * kH; ++p) {
        uint8_t* px = &dst[static_cast<size_t>(p) * 4];
        if (std::abs(px[0] - expR) > 1 || std::abs(px[1] - expG) > 1 ||
            std::abs(px[2] - expB) > 1 || std::abs(px[3] - expA) > 1) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    std::printf("[   info   ] dst RGBA = (%d, %d, %d, %d)  ref = (%d, %d, %d, %d)  mismatches = %d / %u\n",
               dst[0], dst[1], dst[2], dst[3], expR, expG, expB, expA, mismatches, kW * kH);
}

// Weighted (unequal) accumulate: heavier weight on red, lighter on black.
TEST_CASE(blend_engine_weighted_accumulate) {
    D3D11Context ctx;
    if (!ctx.init(-1)) { CHECK(!"d3d11 init failed"); return; }

    BlendEngine blend(ctx);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed"); return; }

    const Col cols[3] = {
        {255,   0,   0, 255},   // red -- heavy
        {  0, 255,   0, 255},   // green -- mid
        {  0,   0,   0, 255},   // black -- light
    };
    std::vector<std::shared_ptr<ImportedFrame>> owners(3);
    std::vector<ImportedFrame*> frames(3);
    for (int i = 0; i < 3; ++i) {
        owners[i] = makeSrcFrame(ctx, kW, kH, cols[i]);
        if (!owners[i]) { CHECK(!"src frame create failed"); return; }
        frames[i] = owners[i].get();
    }

    float weights[3] = { 0.5f, 0.3f, 0.2f };
    if (!blend.dispatchAsync(frames.data(), weights, 3, 0)) { CHECK(!"dispatch failed"); return; }
    blend.waitBlendDone();

    std::vector<uint8_t> dst;
    if (!readbackTexture(ctx, blend.dstTexture(0), kW, kH, dst)) { CHECK(!"readback failed"); return; }

    float r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < 3; ++i) {
        r += weights[i] * toF(cols[i].r);
        g += weights[i] * toF(cols[i].g);
        b += weights[i] * toF(cols[i].b);
        a += weights[i] * toF(cols[i].a);
    }
    uint8_t expR = toU8(r), expG = toU8(g), expB = toU8(b), expA = toU8(a);

    int mismatches = 0;
    for (uint32_t p = 0; p < kW * kH; ++p) {
        uint8_t* px = &dst[static_cast<size_t>(p) * 4];
        if (std::abs(px[0] - expR) > 1 || std::abs(px[1] - expG) > 1 ||
            std::abs(px[2] - expB) > 1 || std::abs(px[3] - expA) > 1) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    std::printf("[   info   ] weighted dst RGBA = (%d, %d, %d, %d)  ref = (%d, %d, %d, %d)  mismatches = %d\n",
               dst[0], dst[1], dst[2], dst[3], expR, expG, expB, expA, mismatches);
}

// Single-source dispatch should reproduce the source exactly (weight 1.0) --
// the N=1 passthrough case BlendConfig::weightsFor() also special-cases.
TEST_CASE(blend_engine_single_source_passthrough) {
    D3D11Context ctx;
    if (!ctx.init(-1)) { CHECK(!"d3d11 init failed"); return; }

    BlendEngine blend(ctx);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed"); return; }

    Col c{10, 20, 30, 255};
    auto frame = makeSrcFrame(ctx, kW, kH, c);
    if (!frame) { CHECK(!"src frame create failed"); return; }
    ImportedFrame* frames[1] = { frame.get() };
    float weight = 1.0f;

    if (!blend.dispatchAsync(frames, &weight, 1, 0)) { CHECK(!"dispatch failed"); return; }
    blend.waitBlendDone();

    std::vector<uint8_t> dst;
    if (!readbackTexture(ctx, blend.dstTexture(0), kW, kH, dst)) { CHECK(!"readback failed"); return; }
    CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30 && dst[3] == 255);
}

int main() {
    std::printf("==== blend_engine GPU test ====\n");
    int rc = (g_test_failures == 0) ? 0 : 1;
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return rc;
}
