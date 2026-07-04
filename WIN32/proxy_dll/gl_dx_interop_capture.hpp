// ─────────────────────────────────────────────────────────────────────────────
// GMix capture (Windows/osu!stable) — GL→D3D11 interop capture, the producer
// side of the port. Lives inside the proxy opengl32.dll, injection-free (see
// opengl32_proxy.cpp), analogous in role to linux-x86_64's
// src/capture/VulkanLayerCapture.{hpp,cpp} but hooking wglSwapBuffers instead
// of vkQueuePresentKHR, and writing into a RING of cross-process shareable
// D3D11 textures instead of Vulkan OPAQUE_FD exports (shared via
// DuplicateHandle, not by name -- see frame_protocol.hpp's PROTOCOL HISTORY
// comment for why OpenSharedResourceByName was dropped).
//
// Requires WGL_NV_DX_interop2 -- confirmed NVIDIA-only in practice (AMD/Intel
// drivers do not implement it). See WIN32/README.md for the CPU-readback
// fallback path this class falls back to when the extension is unavailable
// (correctness, not zero-copy).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct HDC__;
using HDC = HDC__*;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIKeyedMutex;

namespace gmix::ipc { class FrameSender; }

namespace gmix::capture {

class GlDxInteropCapture {
public:
    static GlDxInteropCapture& instance();

    // Called from the proxy's wglSwapBuffers export, once per present. Best-
    // effort and never blocks the render thread on a stalled/absent consumer
    // (mirrors the Vulkan layer's maybeExportFrame() contract) -- on any
    // failure it just skips that frame's export and tries again next present.
    void onSwapBuffers(HDC hdc);

    void shutdown();

private:
    GlDxInteropCapture() = default;
    ~GlDxInteropCapture();
    GlDxInteropCapture(const GlDxInteropCapture&) = delete;

    bool ensureInit(HDC hdc);
    bool ensureRing(uint32_t w, uint32_t h);
    void destroyRing();
    void connectorLoop();
    // CPU-readback fallback path used when WGL_NV_DX_interop2 isn't
    // available on this driver -- correctness over zero-copy. See
    // WIN32/README.md's "biggest platform-compatibility gap" note.
    bool captureViaReadback(HDC hdc, uint32_t w, uint32_t h);
    bool captureViaInterop(HDC hdc, uint32_t w, uint32_t h);

    std::once_flag  initOnce_;
    bool            initOk_ = false;
    bool            interopAvailable_ = false;

    // Own D3D11 device, separate from the game's GL context and from
    // whatever device gmix/OBS creates on the consumer side -- this process
    // (osu!.exe) only ever needs it to host the export ring + (if available)
    // the GL/D3D interop registration.
    ID3D11Device*        d3dDevice_  = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;
    void*                interopDeviceHandle_ = nullptr;   // HANDLE from wglDXOpenDeviceNV

    static constexpr int kExportRing = 48;   // see DEV_NOTES.md note #1 -- must
                                              // exceed the consumer's max blend
                                              // window (kMaxBlendFrames=64's
                                              // realistic in-use subset) + margin
    struct RingSlot {
        ID3D11Texture2D* tex = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        void*            localSharedHandle = nullptr;  // unnamed HANDLE from CreateSharedHandle,
                                                        // kept open for the slot's lifetime so it
                                                        // can be re-DuplicateHandle'd into a new
                                                        // consumer on every (re)connection
        bool             handleSentThisConnection = false;  // reset on (re)connect -- see connectorLoop()
        void*            interopObject = nullptr;  // HANDLE from wglDXRegisterObjectNV
        unsigned int     glTexture = 0;
        unsigned int     glFbo = 0;                // FBO wrapping glTexture, for the blit path
    };
    RingSlot ring_[kExportRing];
    int      ringNext_ = 0;
    uint32_t ringW_ = 0, ringH_ = 0;

    std::mutex                        senderMu_;
    std::unique_ptr<gmix::ipc::FrameSender> sender_;
    bool                               handshakeSent_ = false;
    std::thread                       connectorThread_;
    std::atomic<bool>                 connectorRunning_{false};

    uint64_t frameIndex_ = 0;
    uint32_t producerPid_ = 0;

    // Throttle: matches the Linux layer's kExportInterval reasoning -- a
    // floor well above any real present rate so it never aliases against it.
    std::chrono::steady_clock::time_point lastExportAttempt_{};
};

} // namespace gmix::capture
