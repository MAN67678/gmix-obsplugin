// ─────────────────────────────────────────────────────────────────────────────
// GMix blend engine (Windows/D3D11) — wraps the temporal-blend compute shader.
// D3D11 analogue of linux-x86_64/src/blend/blend_engine.{hpp,cpp}.
//
// Unlike the Vulkan side, D3D11 has one immediate context (no separate async-
// compute queue), so "asynchronous" dispatch here means: record + Dispatch
// on the immediate context, then track completion with an ID3D11Query
// (D3D11_QUERY_EVENT) instead of a timeline semaphore value. The double-
// buffered dst design (front/back) is unchanged -- see dispatchAsync().
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Query;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace gmix {

class D3D11Context;
class ImportedFrame;

class BlendEngine {
public:
    explicit BlendEngine(D3D11Context& ctx);
    ~BlendEngine();

    BlendEngine(const BlendEngine&) = delete;
    BlendEngine& operator=(const BlendEngine&) = delete;

    // Lazily create the compute shader + dst textures sized for (w,h). Call
    // after the output dimensions are known (and again if they change).
    bool init(uint32_t w, uint32_t h);

    static constexpr uint32_t kDstBuffers = 2;

    // Blend up to `count` source frames (each already opened via
    // FrameTexturePool/ImportedFrame -- see ipc/imported_frame.hpp) with the
    // given pre-normalized weights into dst buffer `dstIdx`. Acquires each
    // frame's keyed mutex before reading and releases it (handing the ring
    // slot back to the producer) right after recording the dispatch -- see
    // ImportedFrame::kHandBackKey. Returns false (and dispatches nothing) if
    // any frame's mutex can't be acquired within a short timeout (producer
    // stalled/dead) rather than blocking the caller indefinitely.
    //
    // Only one blend may be in flight at a time; call only when
    // blendInFlight() is false.
    bool dispatchAsync(ImportedFrame* const* frames, const float* weights,
                       uint32_t count, uint32_t dstIdx);
    bool blendInFlight() const { return blendInFlight_; }
    // True once the in-flight blend's GPU work has completed (non-blocking
    // ID3D11Query poll). Clears the in-flight flag as a side effect.
    bool pollBlendDone();
    // Block until the in-flight blend completes (used on resize/teardown).
    void waitBlendDone();

    ID3D11Texture2D* dstTexture(uint32_t idx) const { return dstTex_[idx]; }
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

    // ── Zero-copy shared-handle export (for the OBS plugin) ────────────────
    // True once init() has successfully created both dst buffers with
    // D3D11_RESOURCE_MISC_SHARED. gmix_source.cpp only needs this to be true
    // once (same process, different D3D11 device from OBS's own) -- see
    // WIN32/src/obs_plugin/gmix_source.cpp.
    bool sharedCapable() const { return sharedCapable_; }
    // Raw (legacy, non-NT) shared handle for dst buffer `idx`. Valid for
    // gs_texture_open_shared() within THIS process only -- not a named/
    // cross-process handle like the producer's export-ring textures.
    void* dstSharedHandle(uint32_t idx) const { return dstSharedHandle_[idx]; }

private:
    bool createDstTextures();
    bool createComputeShader();
    bool createWeightsBuffer();
    void destroyAll();

    D3D11Context& ctx_;

    ID3D11ComputeShader* cs_ = nullptr;

    ID3D11Texture2D*           dstTex_[kDstBuffers] = {};
    ID3D11UnorderedAccessView* dstUav_[kDstBuffers] = {};
    void*                      dstSharedHandle_[kDstBuffers] = {};
    bool                       sharedCapable_ = false;

    ID3D11Buffer*             weightsBuf_ = nullptr;   // DYNAMIC structured buffer
    ID3D11ShaderResourceView* weightsSrv_ = nullptr;
    ID3D11Buffer*             pcBuf_      = nullptr;   // DYNAMIC constant buffer (PushConstants)

    ID3D11Query* eventQuery_ = nullptr;
    bool         blendInFlight_ = false;

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;
};

} // namespace gmix
