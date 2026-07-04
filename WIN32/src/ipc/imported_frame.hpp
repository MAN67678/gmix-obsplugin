// ─────────────────────────────────────────────────────────────────────────────
// GMix (Windows) — imported source frame: turns a FrameHeader's (exportSlot,
// acquireKey) into a real ID3D11Texture2D + SRV the blend engine can read.
// D3D11 analogue of linux-x86_64/src/ipc/imported_frame.{hpp,cpp}.
//
// The capture proxy DLL reuses a FIXED ring of named shared textures
// (kExportRing, see gl_dx_interop_capture.hpp) and stamps each frame with its
// export slot. The consumer opens each slot's texture ONCE (FrameTexturePool)
// and reuses it -- exactly the same "avoid a per-frame import" lesson the
// Linux side learned the hard way (see DEV_NOTES.md). Unlike Vulkan's
// OPAQUE_FD + timeline semaphore, there is no separate semaphore object here:
// the synchronization primitive (IDXGIKeyedMutex) is obtained via
// QueryInterface on the SAME shared texture, so opening the texture by name
// is the only import step needed.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct IDXGIKeyedMutex;

namespace gmix {

class D3D11Context;

// One imported source texture (+ SRV + keyed mutex) for a single producer
// export slot. Owned via shared_ptr so a frame still being blended keeps it
// alive even after the pool rebuilds (e.g. on a resize).
struct PooledTexture {
    ID3D11Texture2D*          tex   = nullptr;
    ID3D11ShaderResourceView* srv   = nullptr;
    IDXGIKeyedMutex*          mutex = nullptr;

    PooledTexture() = default;
    ~PooledTexture();
    PooledTexture(const PooledTexture&) = delete;
    PooledTexture& operator=(const PooledTexture&) = delete;
};

// Per-connection cache of PooledTextures keyed by export slot, for the
// current dimensions/format/producerPid. Rebuilds itself if those change.
class FrameTexturePool {
public:
    // Return the pooled texture for `slot`, opening it by name (see
    // gmix::ipc::sharedTextureName) on first use for that slot (rebuilding
    // the whole pool if producerPid/w/h/format changed). Returns nullptr if
    // the open failed.
    std::shared_ptr<PooledTexture> acquire(D3D11Context& ctx, uint32_t producerPid,
                                           uint32_t slot, uint32_t w, uint32_t h,
                                           uint32_t dxgiFormat);

    // Drop all cached textures (call once the connection's GPU work is idle).
    void clear();

private:
    std::vector<std::shared_ptr<PooledTexture>> slots_;
    uint32_t producerPid_ = 0;
    uint32_t w_ = 0, h_ = 0;
    uint32_t fmt_ = 0;
};

class ImportedFrame {
public:
    ImportedFrame() = default;

    // Bind an already-opened (pooled) source texture and record THIS frame's
    // keyed-mutex acquire key. Nothing is owned here -- the texture is
    // borrowed (shared) and released by the pool.
    bool init(std::shared_ptr<PooledTexture> tex, uint64_t acquireKey);

    // The blend engine must IDXGIKeyedMutex::AcquireSync(acquireKey(), ...)
    // before reading srv(), and ReleaseSync(kHandBackKey) once done (see
    // blend_engine.cpp) so the producer can reclaim this ring slot.
    IDXGIKeyedMutex*          keyedMutex() const { return tex_ ? tex_->mutex : nullptr; }
    ID3D11ShaderResourceView* srv()        const { return tex_ ? tex_->srv   : nullptr; }
    uint64_t                  acquireKey() const { return acquireKey_; }
    bool                      valid()      const { return tex_ && tex_->tex != nullptr; }

    // Key the consumer releases the mutex with after reading, handing the
    // slot back to the producer for its next write -- see the AcquireSync/
    // ReleaseSync ping-pong documented in frame_protocol.hpp.
    static constexpr uint64_t kHandBackKey = 0;

private:
    std::shared_ptr<PooledTexture> tex_;
    uint64_t                       acquireKey_ = 0;
};

} // namespace gmix
