// ─────────────────────────────────────────────────────────────────────────────
// GMix capture (Windows/osu!stable) — GL→D3D11 interop capture, the producer
// side of the port. Lives inside GmixCapture.dll, loaded via runtime
// injection (see inject.cpp/capture_main.cpp), analogous in role to
// linux-x86_64's src/capture/VulkanLayerCapture.{hpp,cpp} but hooking
// gdi32!SwapBuffers (inline hook, see inline_hook.hpp) instead of
// vkQueuePresentKHR, and writing into a RING of cross-process shareable
// D3D11 textures instead of Vulkan OPAQUE_FD exports (shared via
// DuplicateHandle -- see frame_protocol.hpp's PROTOCOL HISTORY comment).
//
// GL import mechanism: GL_EXT_memory_object_win32 (glImportMemoryWin32HandleEXT)
// -- imports our OWN local D3D11 ring texture's shared handle into a GL
// texture that ALIASES the same GPU memory, with NO per-frame "lock" step.
//
// HISTORY (three revisions, corrected same day, 2026-07-05):
//  1. WGL_NV_DX_interop2 (wglDXOpenDeviceNV/wglDXRegisterObjectNV/
//     wglDXLockObjectsNV) -- assumed NVIDIA-only, switched away from.
//  2. GL_EXT_memory_object_win32 + GL_EXT_win32_keyed_mutex -- the keyed-
//     mutex half doesn't resolve on this AMD driver at all, so this fell
//     back to CPU readback and was reverted.
//  3. Reverted BACK to WGL_NV_DX_interop2 after a functional probe proved it
//     genuinely works on this driver -- confirmed capturing via the real
//     interop path. BUT real in-game testing then showed the game's actual
//     present rate capped at ~600-800fps despite osu!'s "unlimited" setting
//     normally reaching thousands. Comparing against a reference project
//     (GMIX-Project-WIN32, producer/gl_hook.cpp) that achieves thousands of
//     fps with NO per-frame throttle at all revealed why: it uses
//     GL_EXT_memory_object_win32 (import, no lock step) + GL_EXT_
//     semaphore_win32 (glSignalSemaphoreEXT -- an ASYNC GPU-side signal,
//     never a CPU wait) and never calls anything resembling wglDXLock/
//     UnlockObjectsNV. WGL_NV_DX_interop2's Lock/Unlock calls are
//     documented to synchronize the D3D and GL command streams -- a real,
//     CPU-blocking cost paid on the game's own render thread every export,
//     on top of the keyed-mutex CPU-blocking AcquireSync already used for
//     the cross-process handoff. THIS revision (4) removes that cost:
//     back to GL_EXT_memory_object_win32 for texture import (no lock step
//     of any kind needed for the import itself), with the existing
//     IDXGIKeyedMutex still used for the cross-process protocol (unchanged
//     wire format/consumer expectations) but called as a plain D3D11 API
//     call around the GL blit -- no GL-D3D command-stream sync primitive in
//     the loop at all, matching the reference project's fire-and-forget,
//     zero-CPU-stall design. Falls back to CPU-readback (captureViaReadback)
//     if GL_EXT_memory_object_win32 is unavailable on some other driver.
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

    // Called from the capture DLL's gdi32!SwapBuffers/wglSwapBuffers inline
    // hook detours, once per present. Best-effort and never blocks the
    // render thread on a stalled/absent consumer (mirrors the Vulkan
    // layer's maybeExportFrame() contract) -- on any failure it just skips
    // that frame's export and tries again next present.
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
    // CPU-readback fallback path used when GL_EXT_memory_object_win32 isn't
    // available on this driver -- correctness over zero-copy. See
    // WIN32/README.md.
    bool captureViaReadback(HDC hdc, uint32_t w, uint32_t h);
    bool captureViaInterop(HDC hdc, uint32_t w, uint32_t h);

    std::once_flag  initOnce_;
    bool            initOk_ = false;
    bool            interopAvailable_ = false;

    // Own D3D11 device, separate from the game's GL context and from
    // whatever device gmix/OBS creates on the consumer side -- this process
    // (osu!.exe) only ever needs it to host the export ring.
    ID3D11Device*        d3dDevice_  = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;

    static constexpr int kExportRing = 48;   // see DEV_NOTES.md note #1 -- must
                                              // exceed the consumer's max blend
                                              // window (kMaxBlendFrames=64's
                                              // realistic in-use subset) + margin
    struct RingSlot {
        ID3D11Texture2D* tex = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        void*            localSharedHandle = nullptr;  // unnamed HANDLE from CreateSharedHandle,
                                                        // kept open for the slot's lifetime, re-
                                                        // DuplicateHandle'd into a new consumer on
                                                        // every (re)connection
        bool             handleSentThisConnection = false;  // reset on (re)connect -- see connectorLoop()
        unsigned int     glMemObj = 0;    // GL memory object imported from localSharedHandle
        unsigned int     glTexture = 0;   // GL texture backed by glMemObj (aliases the D3D11 texture)
        unsigned int     glFbo = 0;       // FBO wrapping glTexture, for the blit path
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
    // NOTE: the reference project needs no throttle at all because its
    // per-frame cost is just a blit + async semaphore signal (no CPU-
    // blocking sync). This revision keeps a throttle only because sendFrame()
    // still does a blocking named-pipe WriteFile; if this still caps fps
    // too low after removing the WGL lock/unlock cost, the send itself needs
    // to become async (overlapped I/O) next, not this throttle raised.
    std::chrono::steady_clock::time_point lastExportAttempt_{};

    // Diag: incremented on EVERY onSwapBuffers call, before the export
    // throttle's early-return -- i.e. the game's actual present rate,
    // independent of and always >= the throttled "producer fps" export
    // rate. Not atomic: onSwapBuffers only ever runs on the game's own
    // render thread (it's called from inside the hooked SwapBuffers/
    // wglSwapBuffers function itself).
    uint64_t presentCount_ = 0;

    // Diag: per-export timing breakdown, to find out WHERE time goes inside
    // a single captureViaInterop/Readback call when it runs (not throttle-
    // skipped) -- specifically whether the GPU-sync half (keyed-mutex
    // acquire + blit + release) or the IPC-send half (sendFrame's blocking
    // WriteFile) dominates. Summed per ~1s reporting window in
    // onSwapBuffers, reset there; not atomic, same single-thread reasoning
    // as presentCount_ above.
    uint64_t diagLockSumNs_ = 0, diagLockMaxNs_ = 0;
    uint64_t diagSendSumNs_ = 0, diagSendMaxNs_ = 0;
    uint64_t diagExportCount_ = 0;

    // Diag: how many AcquireSync calls TIMED OUT this window (consumer
    // hasn't released that ring slot back to key0 yet) vs. how many attempts
    // were made in total. A high fail ratio means the consumer can't drain
    // the ring fast enough -- each failed attempt still burns up to
    // kAcquireTimeoutMs blocking the game's own render thread, so this can
    // by itself explain a present-rate collapse even with cheap per-export
    // GL work (see the "still looks stale/laggy" investigation).
    uint64_t diagAcquireFailCount_ = 0;
    uint64_t diagAcquireAttemptCount_ = 0;
};

} // namespace gmix::capture
