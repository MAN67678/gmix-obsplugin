#include "gl_dx_interop_capture.hpp"
#include "proxy_common.hpp"
#include "../src/ipc/frame_sender.hpp"
#include "../src/ipc/frame_protocol.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace gmix::capture {

namespace {

// ── GL types this file needs that <windows.h>/<d3d11.h> don't define ───────
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLuint64 = unsigned long long;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned int;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_BACK = 0x0405;
// GL_EXT_memory_object_win32's handle-type enum for a plain shared NT handle
// (matches D3D11_RESOURCE_MISC_SHARED_NTHANDLE, same value osu's reference
// project's producer/gl_hook.cpp uses).
constexpr GLenum GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587;

using PFN_glGenTextures = void(__stdcall*)(GLsizei, GLuint*);
using PFN_glDeleteTextures = void(__stdcall*)(GLsizei, const GLuint*);
using PFN_glBindTexture = void(__stdcall*)(GLenum, GLuint);
using PFN_glReadBuffer = void(__stdcall*)(GLenum);
using PFN_glReadPixels = void(__stdcall*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using PFN_glFinish = void(__stdcall*)();

using PFN_glGenFramebuffers = void(__stdcall*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers = void(__stdcall*)(GLsizei, const GLuint*);
using PFN_glBindFramebuffer = void(__stdcall*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(__stdcall*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(__stdcall*)(GLenum);
using PFN_glBlitFramebuffer = void(__stdcall*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

// GL_EXT_memory_object / GL_EXT_memory_object_win32 -- cross-vendor (AMD,
// Intel, NVIDIA all implement this extension family). Chosen over
// WGL_NV_DX_interop2 specifically because it needs NO per-frame "lock"
// step: glImportMemoryWin32HandleEXT creates a GL texture that ALIASES the
// D3D11 texture's memory ONCE, at ring-setup time; every subsequent frame
// is just an ordinary GL blit into that texture, with zero GL-D3D command-
// stream synchronization call in the hot path. See gl_dx_interop_
// capture.hpp's header comment for why WGL_NV_DX_interop2's Lock/Unlock
// (a real CPU-blocking sync point, called every export) was capping real
// game fps well below what osu! should achieve.
using PFN_glCreateMemoryObjectsEXT = void(__stdcall*)(GLsizei, GLuint*);
using PFN_glDeleteMemoryObjectsEXT = void(__stdcall*)(GLsizei, const GLuint*);
using PFN_glImportMemoryWin32HandleEXT = void(__stdcall*)(GLuint, GLuint64, GLenum, void*);
using PFN_glTexStorageMem2DEXT = void(__stdcall*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);

struct GlProcs {
    PFN_glGenTextures glGenTextures = nullptr;
    PFN_glDeleteTextures glDeleteTextures = nullptr;
    PFN_glBindTexture glBindTexture = nullptr;
    PFN_glReadBuffer glReadBuffer = nullptr;
    PFN_glReadPixels glReadPixels = nullptr;
    PFN_glFinish glFinish = nullptr;

    PFN_glGenFramebuffers glGenFramebuffers = nullptr;
    PFN_glDeleteFramebuffers glDeleteFramebuffers = nullptr;
    PFN_glBindFramebuffer glBindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D glFramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus glCheckFramebufferStatus = nullptr;
    PFN_glBlitFramebuffer glBlitFramebuffer = nullptr;

    PFN_glCreateMemoryObjectsEXT glCreateMemoryObjectsEXT = nullptr;
    PFN_glDeleteMemoryObjectsEXT glDeleteMemoryObjectsEXT = nullptr;
    PFN_glImportMemoryWin32HandleEXT glImportMemoryWin32HandleEXT = nullptr;
    PFN_glTexStorageMem2DEXT glTexStorageMem2DEXT = nullptr;

    bool interopLoaded = false;
    bool fboLoaded = false;
};

GlProcs g_gl;

void loadGlProcs() {
    using gmix::proxy::resolveGlProc;
    g_gl.glGenTextures    = reinterpret_cast<PFN_glGenTextures>(resolveGlProc("glGenTextures"));
    g_gl.glDeleteTextures = reinterpret_cast<PFN_glDeleteTextures>(resolveGlProc("glDeleteTextures"));
    g_gl.glBindTexture    = reinterpret_cast<PFN_glBindTexture>(resolveGlProc("glBindTexture"));
    g_gl.glReadBuffer     = reinterpret_cast<PFN_glReadBuffer>(resolveGlProc("glReadBuffer"));
    g_gl.glReadPixels     = reinterpret_cast<PFN_glReadPixels>(resolveGlProc("glReadPixels"));
    g_gl.glFinish         = reinterpret_cast<PFN_glFinish>(resolveGlProc("glFinish"));

    g_gl.glGenFramebuffers      = reinterpret_cast<PFN_glGenFramebuffers>(resolveGlProc("glGenFramebuffers"));
    g_gl.glDeleteFramebuffers   = reinterpret_cast<PFN_glDeleteFramebuffers>(resolveGlProc("glDeleteFramebuffers"));
    g_gl.glBindFramebuffer      = reinterpret_cast<PFN_glBindFramebuffer>(resolveGlProc("glBindFramebuffer"));
    g_gl.glFramebufferTexture2D = reinterpret_cast<PFN_glFramebufferTexture2D>(resolveGlProc("glFramebufferTexture2D"));
    g_gl.glCheckFramebufferStatus = reinterpret_cast<PFN_glCheckFramebufferStatus>(resolveGlProc("glCheckFramebufferStatus"));
    g_gl.glBlitFramebuffer      = reinterpret_cast<PFN_glBlitFramebuffer>(resolveGlProc("glBlitFramebuffer"));
    g_gl.fboLoaded = g_gl.glGenFramebuffers && g_gl.glDeleteFramebuffers && g_gl.glBindFramebuffer &&
                     g_gl.glFramebufferTexture2D && g_gl.glCheckFramebufferStatus && g_gl.glBlitFramebuffer;

    g_gl.glCreateMemoryObjectsEXT     = reinterpret_cast<PFN_glCreateMemoryObjectsEXT>(resolveGlProc("glCreateMemoryObjectsEXT"));
    g_gl.glDeleteMemoryObjectsEXT     = reinterpret_cast<PFN_glDeleteMemoryObjectsEXT>(resolveGlProc("glDeleteMemoryObjectsEXT"));
    g_gl.glImportMemoryWin32HandleEXT = reinterpret_cast<PFN_glImportMemoryWin32HandleEXT>(resolveGlProc("glImportMemoryWin32HandleEXT"));
    g_gl.glTexStorageMem2DEXT         = reinterpret_cast<PFN_glTexStorageMem2DEXT>(resolveGlProc("glTexStorageMem2DEXT"));
    g_gl.interopLoaded = g_gl.glCreateMemoryObjectsEXT && g_gl.glDeleteMemoryObjectsEXT &&
                         g_gl.glImportMemoryWin32HandleEXT && g_gl.glTexStorageMem2DEXT &&
                         g_gl.fboLoaded;
}

// Throttle floor. Earlier revisions kept this at 4ms specifically to bound
// how often sendFrame()'s blocking WriteFile could stall the game -- that
// concern was justified when EVERY export also paid a WGL_NV_DX_interop2
// Lock/Unlock cost (a real CPU-blocking GPU sync). Now that the interop
// path has NO per-frame lock step (see the header comment), this matches
// the reference project's design of capturing every single present with no
// throttle at all. Kept at a very small floor (not zero) purely to bound
// worst-case WriteFile-under-backpressure cost if the consumer ever falls
// behind; raise/lower based on what the diag logging shows.
constexpr auto kExportInterval = std::chrono::microseconds(500);

// Keyed-mutex ping-pong keys: key 0 = available for the producer to acquire
// and write; key 1 = handed to the consumer (see ImportedFrame::
// kHandBackKey on the consumer side, which releases back to key 0 once done
// reading). Called as a PLAIN D3D11 API call around the GL blit -- there is
// no GL-side view of this mutex (GL_EXT_win32_keyed_mutex doesn't resolve
// on this AMD driver), so correctness requires an explicit glFinish()
// before ReleaseSync: a first attempt used glFlush() here instead (merely
// submits, doesn't wait for completion) and it was NOT safe -- confirmed by
// testing, the consumer's receiver/blend fps collapsed to ~3-4fps with
// wildly stale frames (blend->now latency spiking to 100-280ms), almost
// certainly the keyed mutex being released before the GPU had actually
// finished the blit, racing the consumer's read. glFinish() waits for GL's
// own command queue to drain (not a cross-API device handshake the way
// WGL_NV_DX_interop2's Lock/Unlock is), so it should still be far cheaper
// while actually being correct.
constexpr uint64_t kKeyAvailable = 0;
constexpr uint64_t kKeyToConsumer = 1;
// Kept short: this still blocks the GAME's render thread if a ring slot's
// mutex is still held by a slow consumer. Failing fast and skipping that
// one frame's export is far better than stalling gameplay.
constexpr DWORD kAcquireTimeoutMs = 4;

} // namespace

GlDxInteropCapture& GlDxInteropCapture::instance() {
    static GlDxInteropCapture inst;
    return inst;
}

GlDxInteropCapture::~GlDxInteropCapture() { shutdown(); }

bool GlDxInteropCapture::ensureInit(HDC /*hdc*/) {
    gmix::proxy::debugLog("ensureInit: starting");
    loadGlProcs();
    gmix::proxy::debugLog("ensureInit: loadGlProcs done, interopLoaded=%d fboLoaded=%d",
                          (int)g_gl.interopLoaded, (int)g_gl.fboLoaded);

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, 1, D3D11_SDK_VERSION, &d3dDevice_, &got, &d3dContext_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "gmix: capture: D3D11CreateDevice failed (hr=0x%08lx) -- "
                              "capture disabled\n", static_cast<unsigned long>(hr));
        gmix::proxy::debugLog("ensureInit: D3D11CreateDevice FAILED hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    gmix::proxy::debugLog("ensureInit: D3D11CreateDevice OK, device=%p", (void*)d3dDevice_);

    interopAvailable_ = g_gl.interopLoaded;
    if (interopAvailable_) {
        gmix::proxy::debugLog("ensureInit: GL_EXT_memory_object_win32 available -- "
                              "using zero-copy interop path (no per-frame lock step)");
    } else {
        std::fprintf(stderr, "gmix: capture: GL_EXT_memory_object_win32 unavailable on this driver -- "
                              "falling back to CPU-readback capture (slower, not zero-copy; "
                              "see WIN32/README.md)\n");
        gmix::proxy::debugLog("ensureInit: GL_EXT_memory_object_win32 unavailable -- using CPU-readback path");
    }

    producerPid_ = gmix::proxy::currentProducerPid();

    connectorRunning_ = true;
    connectorThread_ = std::thread([this]() { connectorLoop(); });
    gmix::proxy::debugLog("ensureInit: done, connector thread started");
    return true;
}

void GlDxInteropCapture::connectorLoop() {
    auto pipeName = gmix::ipc::defaultFramePipeName();
    while (connectorRunning_) {
        bool needConnect = false;
        {
            std::lock_guard<std::mutex> lk(senderMu_);
            needConnect = !sender_ || !sender_->isConnected();
        }
        if (needConnect) {
            auto s = std::make_unique<gmix::ipc::FrameSender>();
            bool connected = s->connect(pipeName);
            gmix::proxy::debugLog("connectorLoop: connect attempt -> %s", connected ? "OK" : "failed");
            if (connected) {
                std::lock_guard<std::mutex> lk(senderMu_);
                sender_ = std::move(s);
                handshakeSent_ = false;
                // A new connection means a (possibly different) consumer
                // process -- every ring slot's handle must be re-
                // DuplicateHandle'd into it at least once before use; see
                // frame_protocol.hpp's PROTOCOL HISTORY comment. Guarded by
                // the same senderMu_ lock (already held here) that
                // captureViaInterop()/captureViaReadback() take around their
                // own reads/writes of this flag.
                for (auto& rs : ring_) rs.handleSentThisConnection = false;
            }
        }
        for (int i = 0; i < 10 && connectorRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool GlDxInteropCapture::ensureRing(uint32_t w, uint32_t h) {
    if (ringW_ == w && ringH_ == h && ring_[0].tex) return true;
    destroyRing();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < kExportRing; ++i) {
        auto& rs = ring_[i];
        if (d3dDevice_->CreateTexture2D(&td, nullptr, &rs.tex) != S_OK) {
            std::fprintf(stderr, "gmix: capture: ring texture create failed (slot %d)\n", i);
            destroyRing();
            return false;
        }

        // Create an UNNAMED shared handle -- see frame_protocol.hpp's
        // PROTOCOL HISTORY comment for why NOT OpenSharedResourceByName
        // (broken on some drivers). Kept open for the slot's lifetime:
        // reused both for the GL_EXT_memory_object_win32 import below (this
        // process, same handle) AND re-DuplicateHandle'd into whichever
        // consumer process is currently connected (see connectorLoop()/
        // captureViaInterop()/captureViaReadback()).
        IDXGIResource1* dxgiRes1 = nullptr;
        if (rs.tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dxgiRes1)) == S_OK) {
            HANDLE h = nullptr;
            HRESULT hr = dxgiRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                      nullptr, &h);
            dxgiRes1->Release();
            if (FAILED(hr) || !h) {
                std::fprintf(stderr, "gmix: capture: CreateSharedHandle failed (slot %d, hr=0x%08lx)\n",
                             i, static_cast<unsigned long>(hr));
                destroyRing();
                return false;
            }
            rs.localSharedHandle = h;
        } else {
            std::fprintf(stderr, "gmix: capture: IDXGIResource1 unavailable\n");
            destroyRing();
            return false;
        }

        if (rs.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&rs.mutex)) != S_OK) {
            std::fprintf(stderr, "gmix: capture: keyed mutex QI failed (slot %d)\n", i);
            destroyRing();
            return false;
        }

        if (interopAvailable_) {
            g_gl.glCreateMemoryObjectsEXT(1, &rs.glMemObj);
            // D3D11 has no exact "memory requirements" query the way Vulkan
            // does; a tightly-packed RGBA8 size is a reasonable hint --
            // real allocations for a single, non-suballocated import like
            // this are described by the handle itself, so implementations
            // generally treat this as advisory rather than authoritative.
            GLuint64 memSize = static_cast<GLuint64>(w) * h * 4;
            g_gl.glImportMemoryWin32HandleEXT(rs.glMemObj, memSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                              rs.localSharedHandle);

            g_gl.glGenTextures(1, &rs.glTexture);
            g_gl.glBindTexture(GL_TEXTURE_2D, rs.glTexture);
            g_gl.glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(w),
                                      static_cast<GLsizei>(h), rs.glMemObj, 0);
            g_gl.glBindTexture(GL_TEXTURE_2D, 0);

            g_gl.glGenFramebuffers(1, &rs.glFbo);
            g_gl.glBindFramebuffer(GL_FRAMEBUFFER, rs.glFbo);
            g_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rs.glTexture, 0);
            GLenum status = g_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
            g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                std::fprintf(stderr, "gmix: capture: FBO incomplete (slot %d, status=0x%04x) -- "
                                      "falling back to CPU-readback capture\n", i, status);
                interopAvailable_ = false;
            }
        }
    }

    ringW_ = w; ringH_ = h; ringNext_ = 0;
    return true;
}

void GlDxInteropCapture::destroyRing() {
    for (auto& rs : ring_) {
        if (rs.glFbo && g_gl.glDeleteFramebuffers) { g_gl.glDeleteFramebuffers(1, &rs.glFbo); rs.glFbo = 0; }
        if (rs.glTexture && g_gl.glDeleteTextures) { g_gl.glDeleteTextures(1, &rs.glTexture); rs.glTexture = 0; }
        if (rs.glMemObj && g_gl.glDeleteMemoryObjectsEXT) { g_gl.glDeleteMemoryObjectsEXT(1, &rs.glMemObj); rs.glMemObj = 0; }
        if (rs.mutex) { rs.mutex->Release(); rs.mutex = nullptr; }
        if (rs.localSharedHandle) { CloseHandle(static_cast<HANDLE>(rs.localSharedHandle)); rs.localSharedHandle = nullptr; }
        rs.handleSentThisConnection = false;
        if (rs.tex)   { rs.tex->Release();   rs.tex = nullptr; }
    }
    ringW_ = ringH_ = 0;
}

bool GlDxInteropCapture::captureViaInterop(HDC /*hdc*/, uint32_t w, uint32_t h) {
    int slot = ringNext_;
    ringNext_ = (ringNext_ + 1) % kExportRing;
    auto& rs = ring_[slot];
    if (!rs.tex || !rs.glMemObj) return false;

    uint64_t t0 = ipc::nowNs();

    // Plain D3D11 API call, NOT wrapped in any GL-side lock (see this file's
    // header comment for why: WGL_NV_DX_interop2's Lock/Unlock was the real
    // per-frame cost capping fps well below what osu! should reach).
    ++diagAcquireAttemptCount_;
    if (rs.mutex->AcquireSync(kKeyAvailable, kAcquireTimeoutMs) != S_OK) {
        ++diagAcquireFailCount_;
        return false;
    }

    // Flip Y during the blit: GL's default framebuffer is stored bottom-up,
    // D3D textures are read top-down by the consumer's compute shader --
    // without this the blended output would be vertically mirrored.
    g_gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    g_gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rs.glFbo);
    g_gl.glBlitFramebuffer(0, 0, static_cast<GLint>(w), static_cast<GLint>(h),
                           0, static_cast<GLint>(h), static_cast<GLint>(w), 0,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Wait for the blit to actually COMPLETE before releasing the mutex --
    // see the constants block above for why glFlush() alone was unsafe here.
    g_gl.glFinish();

    rs.mutex->ReleaseSync(kKeyToConsumer);

    uint64_t t1 = ipc::nowNs();
    uint64_t lockNs = t1 - t0;
    diagLockSumNs_ += lockNs;
    if (lockNs > diagLockMaxNs_) diagLockMaxNs_ = lockNs;

    ipc::FrameHeader hdr{};
    hdr.magic = ipc::kMagic;
    hdr.width = w;
    hdr.height = h;
    hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    hdr.exportSlot = static_cast<uint32_t>(slot);
    hdr.acquireKey = kKeyToConsumer;
    hdr.frameIndex = ++frameIndex_;
    hdr.timestampNs = ipc::nowNs();
    hdr.rowPitch = 0;
    hdr.gpuTimestampNs = 0;

    std::lock_guard<std::mutex> lk(senderMu_);
    if (!sender_ || !sender_->isConnected()) return false;
    bool ok = true;
    if (!handshakeSent_) {
        ok = sender_->sendHandshake(w, h, hdr.dxgiFormat, producerPid_);
        if (ok) handshakeSent_ = true;
    }
    // Only duplicate+send this slot's handle the first time it's used on
    // this connection -- see frame_protocol.hpp's PROTOCOL HISTORY comment.
    // rs.handleSentThisConnection is reset by connectorLoop() under this
    // same senderMu_ lock whenever a new connection is established.
    if (ok && !rs.handleSentThisConnection) {
        hdr.sharedHandleValue = sender_->duplicateHandleToConsumer(rs.localSharedHandle);
        if (hdr.sharedHandleValue != 0) rs.handleSentThisConnection = true;
    }
    if (ok) ok = sender_->sendFrame(hdr);
    if (!ok) sender_.reset();

    uint64_t sendNs = ipc::nowNs() - t1;
    diagSendSumNs_ += sendNs;
    if (sendNs > diagSendMaxNs_) diagSendMaxNs_ = sendNs;
    ++diagExportCount_;

    return ok;
}

bool GlDxInteropCapture::captureViaReadback(HDC /*hdc*/, uint32_t w, uint32_t h) {
    int slot = ringNext_;
    ringNext_ = (ringNext_ + 1) % kExportRing;
    auto& rs = ring_[slot];
    if (!rs.tex) return false;

    // No GL interop involved in this fallback path -- straight D3D11
    // IDXGIKeyedMutex, matching the consumer's expectations exactly.
    if (rs.mutex->AcquireSync(kKeyAvailable, kAcquireTimeoutMs) != S_OK) return false;

    // Slow path: read the backbuffer to host memory, flipping rows (GL is
    // bottom-up, D3D top-down -- same reasoning as the interop path's Y
    // flip, done here on the CPU instead of via glBlitFramebuffer), then
    // upload into the shared D3D11 texture. Not zero-copy; see
    // WIN32/README.md for when this path is used.
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    g_gl.glReadBuffer(GL_BACK);
    g_gl.glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<uint8_t> flipped(pixels.size());
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(flipped.data() + y * rowBytes, pixels.data() + (h - 1 - y) * rowBytes, rowBytes);
    }
    d3dContext_->UpdateSubresource(rs.tex, 0, nullptr, flipped.data(), static_cast<UINT>(rowBytes), 0);

    rs.mutex->ReleaseSync(kKeyToConsumer);

    ipc::FrameHeader hdr{};
    hdr.magic = ipc::kMagic;
    hdr.width = w;
    hdr.height = h;
    hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    hdr.exportSlot = static_cast<uint32_t>(slot);
    hdr.acquireKey = kKeyToConsumer;
    hdr.frameIndex = ++frameIndex_;
    hdr.timestampNs = ipc::nowNs();
    hdr.rowPitch = rowBytes;
    hdr.gpuTimestampNs = 0;

    std::lock_guard<std::mutex> lk(senderMu_);
    if (!sender_ || !sender_->isConnected()) return false;
    bool ok = true;
    if (!handshakeSent_) {
        ok = sender_->sendHandshake(w, h, hdr.dxgiFormat, producerPid_);
        if (ok) handshakeSent_ = true;
    }
    if (ok && !rs.handleSentThisConnection) {
        hdr.sharedHandleValue = sender_->duplicateHandleToConsumer(rs.localSharedHandle);
        if (hdr.sharedHandleValue != 0) rs.handleSentThisConnection = true;
    }
    if (ok) ok = sender_->sendFrame(hdr);
    if (!ok) sender_.reset();
    return ok;
}

void GlDxInteropCapture::onSwapBuffers(HDC hdc) {
    std::call_once(initOnce_, [&]() { initOk_ = ensureInit(hdc); });
    if (!initOk_) return;

    // Diag: count EVERY present, before anything else runs -- this is the
    // game's actual render rate, independent of (and always >=) the
    // throttled export rate below. If this is far below what osu! should be
    // achieving unthrottled, the cost is being paid somewhere in/before this
    // function itself (e.g. the inline-hook trampoline), not in the
    // throttled capture path.
    ++presentCount_;

    // Throttled (~once/sec) status line -- onSwapBuffers runs every present,
    // so anything unconditional here would flood the log. Reports exactly
    // the state needed to diagnose "capture never activates": is a consumer
    // connected, what size is being captured, etc. -- plus (per request) the
    // game's actual present fps and the producer's throttled export fps,
    // both computed as deltas over this same ~1s window so they're directly
    // comparable to each other and to the consumer-side receiver/blend/draw
    // fps numbers gmix_source.cpp logs.
    static std::chrono::steady_clock::time_point lastStatusLog{};
    static uint64_t lastStatusFrameIndex = 0;
    static uint64_t lastStatusPresentCount = 0;
    auto nowForLog = std::chrono::steady_clock::now();
    auto sinceLastLog = nowForLog - lastStatusLog;
    bool doStatusLog = sinceLastLog > std::chrono::seconds(1);
    double producerFps = 0.0;
    double gameFps = 0.0;
    // Per-export timing breakdown (see captureViaInterop's diagLock*/diagSend*
    // accumulation) -- averaged/maxed over THIS window's exports, then reset,
    // so it reflects only what happened since the last log line.
    double avgLockMs = 0.0, maxLockMs = 0.0, avgSendMs = 0.0, maxSendMs = 0.0;
    double acquireFailPct = 0.0;
    uint64_t acquireFails = 0, acquireAttempts = 0;
    if (doStatusLog) {
        double elapsedS = std::chrono::duration<double>(sinceLastLog).count();
        if (lastStatusLog.time_since_epoch().count() != 0 && elapsedS > 0.0) {
            producerFps = static_cast<double>(frameIndex_ - lastStatusFrameIndex) / elapsedS;
            gameFps = static_cast<double>(presentCount_ - lastStatusPresentCount) / elapsedS;
        }
        lastStatusFrameIndex = frameIndex_;
        lastStatusPresentCount = presentCount_;
        lastStatusLog = nowForLog;

        if (diagExportCount_ > 0) {
            avgLockMs = (diagLockSumNs_ / static_cast<double>(diagExportCount_)) / 1e6;
            avgSendMs = (diagSendSumNs_ / static_cast<double>(diagExportCount_)) / 1e6;
        }
        maxLockMs = diagLockMaxNs_ / 1e6;
        maxSendMs = diagSendMaxNs_ / 1e6;
        diagLockSumNs_ = diagLockMaxNs_ = diagSendSumNs_ = diagSendMaxNs_ = 0;
        diagExportCount_ = 0;

        acquireFails = diagAcquireFailCount_;
        acquireAttempts = diagAcquireAttemptCount_;
        if (acquireAttempts > 0) {
            acquireFailPct = 100.0 * static_cast<double>(acquireFails) / static_cast<double>(acquireAttempts);
        }
        diagAcquireFailCount_ = diagAcquireAttemptCount_ = 0;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastExportAttempt_.time_since_epoch().count() != 0 && now - lastExportAttempt_ < kExportInterval) {
        if (doStatusLog) gmix::proxy::debugLog("onSwapBuffers: throttled, game fps=%.1f producer fps=%.1f",
                                               gameFps, producerFps);
        return;
    }
    lastExportAttempt_ = now;

    bool senderConnected = false;
    {
        std::lock_guard<std::mutex> lk(senderMu_);
        senderConnected = sender_ && sender_->isConnected();
    }
    if (!senderConnected) {
        if (doStatusLog) gmix::proxy::debugLog("onSwapBuffers: no consumer connected yet, game fps=%.1f", gameFps);
        return;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) {
        if (doStatusLog) gmix::proxy::debugLog("onSwapBuffers: WindowFromDC(hdc=%p) returned null", (void*)hdc);
        return;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
    uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
    if (w == 0 || h == 0) {
        if (doStatusLog) gmix::proxy::debugLog("onSwapBuffers: zero-size client rect (%ux%u)", w, h);
        return;
    }

    if (!ensureRing(w, h)) {
        if (doStatusLog) gmix::proxy::debugLog("onSwapBuffers: ensureRing(%u,%u) FAILED", w, h);
        return;
    }

    if (doStatusLog) {
        gmix::proxy::debugLog("onSwapBuffers: capturing %ux%u via %s, game fps=%.1f producer fps=%.1f | "
                              "lock avg=%.3fms max=%.3fms, send avg=%.3fms max=%.3fms | "
                              "acquire fails=%llu/%llu (%.1f%%)", w, h,
                             interopAvailable_ ? "interop" : "readback", gameFps, producerFps,
                             avgLockMs, maxLockMs, avgSendMs, maxSendMs,
                             (unsigned long long)acquireFails, (unsigned long long)acquireAttempts, acquireFailPct);
    }
    if (interopAvailable_) captureViaInterop(hdc, w, h);
    else                   captureViaReadback(hdc, w, h);
}

void GlDxInteropCapture::shutdown() {
    connectorRunning_ = false;
    if (connectorThread_.joinable()) connectorThread_.join();
    {
        std::lock_guard<std::mutex> lk(senderMu_);
        sender_.reset();
    }
    destroyRing();
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_)  { d3dDevice_->Release();  d3dDevice_ = nullptr; }
}

} // namespace gmix::capture
