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

// ── GL/WGL types this file needs that <windows.h>/<d3d11.h> don't define ───
using GLenum = unsigned int;
using GLuint = unsigned int;
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
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_BACK = 0x0405;

using PFN_wglGetCurrentContext = HGLRC(__stdcall*)();
using PFN_wglDXSetResourceShareHandleNV = BOOL(__stdcall*)(void*, HANDLE);
using PFN_wglDXOpenDeviceNV   = HANDLE(__stdcall*)(void*);
using PFN_wglDXCloseDeviceNV  = BOOL(__stdcall*)(HANDLE);
using PFN_wglDXRegisterObjectNV   = HANDLE(__stdcall*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PFN_wglDXUnregisterObjectNV = BOOL(__stdcall*)(HANDLE, HANDLE);
using PFN_wglDXLockObjectsNV   = BOOL(__stdcall*)(HANDLE, GLint, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(__stdcall*)(HANDLE, GLint, HANDLE*);
constexpr GLenum WGL_ACCESS_WRITE_DISCARD_NV = 0x00000001;

using PFN_glGenTextures = void(__stdcall*)(GLsizei, GLuint*);
using PFN_glDeleteTextures = void(__stdcall*)(GLsizei, const GLuint*);
using PFN_glGenFramebuffers = void(__stdcall*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers = void(__stdcall*)(GLsizei, const GLuint*);
using PFN_glBindFramebuffer = void(__stdcall*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(__stdcall*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(__stdcall*)(GLenum);
using PFN_glBlitFramebuffer = void(__stdcall*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_glReadBuffer = void(__stdcall*)(GLenum);
using PFN_glReadPixels = void(__stdcall*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

struct GlProcs {
    PFN_wglGetCurrentContext wglGetCurrentContext = nullptr;
    PFN_wglDXOpenDeviceNV wglDXOpenDeviceNV = nullptr;
    PFN_wglDXCloseDeviceNV wglDXCloseDeviceNV = nullptr;
    PFN_wglDXRegisterObjectNV wglDXRegisterObjectNV = nullptr;
    PFN_wglDXUnregisterObjectNV wglDXUnregisterObjectNV = nullptr;
    PFN_wglDXLockObjectsNV wglDXLockObjectsNV = nullptr;
    PFN_wglDXUnlockObjectsNV wglDXUnlockObjectsNV = nullptr;
    PFN_glGenTextures glGenTextures = nullptr;
    PFN_glDeleteTextures glDeleteTextures = nullptr;
    PFN_glGenFramebuffers glGenFramebuffers = nullptr;
    PFN_glDeleteFramebuffers glDeleteFramebuffers = nullptr;
    PFN_glBindFramebuffer glBindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D glFramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus glCheckFramebufferStatus = nullptr;
    PFN_glBlitFramebuffer glBlitFramebuffer = nullptr;
    PFN_glReadBuffer glReadBuffer = nullptr;
    PFN_glReadPixels glReadPixels = nullptr;
    bool interopLoaded = false;
    bool fboLoaded = false;
};

GlProcs g_gl;

void loadGlProcs() {
    using gmix::proxy::resolveGlProc;
    g_gl.wglGetCurrentContext = reinterpret_cast<PFN_wglGetCurrentContext>(resolveGlProc("wglGetCurrentContext"));
    g_gl.glGenTextures    = reinterpret_cast<PFN_glGenTextures>(resolveGlProc("glGenTextures"));
    g_gl.glDeleteTextures = reinterpret_cast<PFN_glDeleteTextures>(resolveGlProc("glDeleteTextures"));
    g_gl.glReadBuffer     = reinterpret_cast<PFN_glReadBuffer>(resolveGlProc("glReadBuffer"));
    g_gl.glReadPixels     = reinterpret_cast<PFN_glReadPixels>(resolveGlProc("glReadPixels"));

    g_gl.glGenFramebuffers      = reinterpret_cast<PFN_glGenFramebuffers>(resolveGlProc("glGenFramebuffers"));
    g_gl.glDeleteFramebuffers   = reinterpret_cast<PFN_glDeleteFramebuffers>(resolveGlProc("glDeleteFramebuffers"));
    g_gl.glBindFramebuffer      = reinterpret_cast<PFN_glBindFramebuffer>(resolveGlProc("glBindFramebuffer"));
    g_gl.glFramebufferTexture2D = reinterpret_cast<PFN_glFramebufferTexture2D>(resolveGlProc("glFramebufferTexture2D"));
    g_gl.glCheckFramebufferStatus = reinterpret_cast<PFN_glCheckFramebufferStatus>(resolveGlProc("glCheckFramebufferStatus"));
    g_gl.glBlitFramebuffer      = reinterpret_cast<PFN_glBlitFramebuffer>(resolveGlProc("glBlitFramebuffer"));
    g_gl.fboLoaded = g_gl.glGenFramebuffers && g_gl.glDeleteFramebuffers && g_gl.glBindFramebuffer &&
                     g_gl.glFramebufferTexture2D && g_gl.glCheckFramebufferStatus && g_gl.glBlitFramebuffer;

    g_gl.wglDXOpenDeviceNV = reinterpret_cast<PFN_wglDXOpenDeviceNV>(resolveGlProc("wglDXOpenDeviceNV"));
    g_gl.wglDXCloseDeviceNV = reinterpret_cast<PFN_wglDXCloseDeviceNV>(resolveGlProc("wglDXCloseDeviceNV"));
    g_gl.wglDXRegisterObjectNV = reinterpret_cast<PFN_wglDXRegisterObjectNV>(resolveGlProc("wglDXRegisterObjectNV"));
    g_gl.wglDXUnregisterObjectNV = reinterpret_cast<PFN_wglDXUnregisterObjectNV>(resolveGlProc("wglDXUnregisterObjectNV"));
    g_gl.wglDXLockObjectsNV = reinterpret_cast<PFN_wglDXLockObjectsNV>(resolveGlProc("wglDXLockObjectsNV"));
    g_gl.wglDXUnlockObjectsNV = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(resolveGlProc("wglDXUnlockObjectsNV"));
    g_gl.interopLoaded = g_gl.wglDXOpenDeviceNV && g_gl.wglDXCloseDeviceNV && g_gl.wglDXRegisterObjectNV &&
                         g_gl.wglDXUnregisterObjectNV && g_gl.wglDXLockObjectsNV && g_gl.wglDXUnlockObjectsNV &&
                         g_gl.fboLoaded;
}

uint64_t nowNs() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return static_cast<uint64_t>(static_cast<double>(ctr.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart));
}

// Throttle floor, same reasoning as the Linux layer's kExportInterval: sits
// well above any real present rate so it never aliases against it.
constexpr auto kExportInterval = std::chrono::microseconds(200);

} // namespace

GlDxInteropCapture& GlDxInteropCapture::instance() {
    static GlDxInteropCapture inst;
    return inst;
}

GlDxInteropCapture::~GlDxInteropCapture() { shutdown(); }

bool GlDxInteropCapture::ensureInit(HDC /*hdc*/) {
    loadGlProcs();

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, 1, D3D11_SDK_VERSION, &d3dDevice_, &got, &d3dContext_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "gmix: capture: D3D11CreateDevice failed (hr=0x%08lx) -- "
                              "capture disabled\n", static_cast<unsigned long>(hr));
        return false;
    }

    interopAvailable_ = g_gl.interopLoaded;
    if (interopAvailable_) {
        interopDeviceHandle_ = g_gl.wglDXOpenDeviceNV(d3dDevice_);
        if (!interopDeviceHandle_) {
            std::fprintf(stderr, "gmix: capture: wglDXOpenDeviceNV failed -- "
                                  "falling back to CPU-readback capture (slower, not zero-copy; "
                                  "see WIN32/README.md)\n");
            interopAvailable_ = false;
        }
    } else {
        std::fprintf(stderr, "gmix: capture: WGL_NV_DX_interop2 unavailable on this driver -- "
                              "falling back to CPU-readback capture (slower, not zero-copy; "
                              "see WIN32/README.md)\n");
    }

    producerPid_ = gmix::proxy::currentProducerPid();

    connectorRunning_ = true;
    connectorThread_ = std::thread([this]() { connectorLoop(); });
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
            if (s->connect(pipeName)) {
                std::lock_guard<std::mutex> lk(senderMu_);
                sender_ = std::move(s);
                handshakeSent_ = false;
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

        // Register a name so the consumer can OpenSharedResourceByName it --
        // see ipc/frame_protocol.hpp's sharedTextureName(). The local handle
        // CreateSharedHandle returns can be closed immediately; the name
        // stays resolvable for as long as `rs.tex` (this process's D3D11
        // resource) is alive.
        IDXGIResource1* dxgiRes1 = nullptr;
        if (rs.tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dxgiRes1)) == S_OK) {
            auto name = ipc::sharedTextureName(producerPid_, static_cast<uint32_t>(i));
            HANDLE tmp = nullptr;
            HRESULT hr = dxgiRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                      name.c_str(), &tmp);
            dxgiRes1->Release();
            if (FAILED(hr)) {
                std::fprintf(stderr, "gmix: capture: CreateSharedHandle failed (slot %d, hr=0x%08lx)\n",
                             i, static_cast<unsigned long>(hr));
                destroyRing();
                return false;
            }
            if (tmp) CloseHandle(tmp);
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
            g_gl.glGenTextures(1, &rs.glTexture);
            rs.interopObject = g_gl.wglDXRegisterObjectNV(interopDeviceHandle_, rs.tex, rs.glTexture,
                                                          GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
            if (!rs.interopObject) {
                std::fprintf(stderr, "gmix: capture: wglDXRegisterObjectNV failed (slot %d) -- "
                                      "falling back to CPU-readback capture\n", i);
                interopAvailable_ = false;
            } else {
                HANDLE interopHandle = rs.interopObject;
                g_gl.wglDXLockObjectsNV(interopDeviceHandle_, 1, &interopHandle);
                g_gl.glGenFramebuffers(1, &rs.glFbo);
                g_gl.glBindFramebuffer(GL_FRAMEBUFFER, rs.glFbo);
                g_gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rs.glTexture, 0);
                GLenum status = g_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
                g_gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g_gl.wglDXUnlockObjectsNV(interopDeviceHandle_, 1, &interopHandle);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    std::fprintf(stderr, "gmix: capture: FBO incomplete (slot %d, status=0x%04x)\n", i, status);
                    interopAvailable_ = false;
                }
            }
        }
    }

    ringW_ = w; ringH_ = h; ringNext_ = 0;
    return true;
}

void GlDxInteropCapture::destroyRing() {
    for (auto& rs : ring_) {
        if (rs.interopObject) {
            HANDLE h = rs.interopObject;
            if (g_gl.wglDXUnregisterObjectNV) g_gl.wglDXUnregisterObjectNV(interopDeviceHandle_, h);
            rs.interopObject = nullptr;
        }
        if (rs.glFbo && g_gl.glDeleteFramebuffers) { g_gl.glDeleteFramebuffers(1, &rs.glFbo); rs.glFbo = 0; }
        if (rs.glTexture && g_gl.glDeleteTextures) { g_gl.glDeleteTextures(1, &rs.glTexture); rs.glTexture = 0; }
        if (rs.mutex) { rs.mutex->Release(); rs.mutex = nullptr; }
        if (rs.tex)   { rs.tex->Release();   rs.tex = nullptr; }
    }
    ringW_ = ringH_ = 0;
}

bool GlDxInteropCapture::captureViaInterop(HDC /*hdc*/, uint32_t w, uint32_t h) {
    int slot = ringNext_;
    ringNext_ = (ringNext_ + 1) % kExportRing;
    auto& rs = ring_[slot];
    if (!rs.tex || !rs.interopObject) return false;

    // Producer's half of the keyed-mutex ping-pong: acquire key 0 (available
    // once the consumer has released this slot back -- see
    // ImportedFrame::kHandBackKey), write, release with key 1 (the value the
    // consumer's FrameHeader.acquireKey will name).
    constexpr DWORD kAcquireTimeoutMs = 50;
    if (rs.mutex->AcquireSync(0, kAcquireTimeoutMs) != S_OK) return false;

    HANDLE interopHandle = rs.interopObject;
    if (!g_gl.wglDXLockObjectsNV(interopDeviceHandle_, 1, &interopHandle)) {
        rs.mutex->ReleaseSync(0);
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

    g_gl.wglDXUnlockObjectsNV(interopDeviceHandle_, 1, &interopHandle);
    rs.mutex->ReleaseSync(1);

    ipc::FrameHeader hdr{};
    hdr.magic = ipc::kMagic;
    hdr.width = w;
    hdr.height = h;
    hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    hdr.exportSlot = static_cast<uint32_t>(slot);
    hdr.acquireKey = 1;
    hdr.frameIndex = ++frameIndex_;
    hdr.timestampNs = nowNs();
    hdr.rowPitch = 0;
    hdr.gpuTimestampNs = 0;

    std::lock_guard<std::mutex> lk(senderMu_);
    if (!sender_ || !sender_->isConnected()) return false;
    bool ok = true;
    if (!handshakeSent_) {
        ok = sender_->sendHandshake(w, h, hdr.dxgiFormat, producerPid_);
        if (ok) handshakeSent_ = true;
    }
    if (ok) ok = sender_->sendFrame(hdr);
    if (!ok) sender_.reset();
    return ok;
}

bool GlDxInteropCapture::captureViaReadback(HDC /*hdc*/, uint32_t w, uint32_t h) {
    int slot = ringNext_;
    ringNext_ = (ringNext_ + 1) % kExportRing;
    auto& rs = ring_[slot];
    if (!rs.tex) return false;

    constexpr DWORD kAcquireTimeoutMs = 50;
    if (rs.mutex->AcquireSync(0, kAcquireTimeoutMs) != S_OK) return false;

    // Slow path: read the backbuffer to host memory, flipping rows (GL is
    // bottom-up, D3D top-down -- same reasoning as the interop path's Y
    // flip, done here on the CPU instead of via glBlitFramebuffer), then
    // upload into the shared D3D11 texture. Not zero-copy; see
    // WIN32/README.md for when this path is used (non-NVIDIA GPUs).
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    g_gl.glReadBuffer(GL_BACK);
    g_gl.glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::vector<uint8_t> flipped(pixels.size());
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(flipped.data() + y * rowBytes, pixels.data() + (h - 1 - y) * rowBytes, rowBytes);
    }
    d3dContext_->UpdateSubresource(rs.tex, 0, nullptr, flipped.data(), static_cast<UINT>(rowBytes), 0);

    rs.mutex->ReleaseSync(1);

    ipc::FrameHeader hdr{};
    hdr.magic = ipc::kMagic;
    hdr.width = w;
    hdr.height = h;
    hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    hdr.exportSlot = static_cast<uint32_t>(slot);
    hdr.acquireKey = 1;
    hdr.frameIndex = ++frameIndex_;
    hdr.timestampNs = nowNs();
    hdr.rowPitch = rowBytes;
    hdr.gpuTimestampNs = 0;

    std::lock_guard<std::mutex> lk(senderMu_);
    if (!sender_ || !sender_->isConnected()) return false;
    bool ok = true;
    if (!handshakeSent_) {
        ok = sender_->sendHandshake(w, h, hdr.dxgiFormat, producerPid_);
        if (ok) handshakeSent_ = true;
    }
    if (ok) ok = sender_->sendFrame(hdr);
    if (!ok) sender_.reset();
    return ok;
}

void GlDxInteropCapture::onSwapBuffers(HDC hdc) {
    std::call_once(initOnce_, [&]() { initOk_ = ensureInit(hdc); });
    if (!initOk_) return;

    auto now = std::chrono::steady_clock::now();
    if (lastExportAttempt_.time_since_epoch().count() != 0 && now - lastExportAttempt_ < kExportInterval) return;
    lastExportAttempt_ = now;

    {
        std::lock_guard<std::mutex> lk(senderMu_);
        if (!sender_ || !sender_->isConnected()) return;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
    uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
    if (w == 0 || h == 0) return;

    if (!ensureRing(w, h)) return;

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
    if (interopDeviceHandle_ && g_gl.wglDXCloseDeviceNV) {
        g_gl.wglDXCloseDeviceNV(interopDeviceHandle_);
        interopDeviceHandle_ = nullptr;
    }
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_)  { d3dDevice_->Release();  d3dDevice_ = nullptr; }
}

} // namespace gmix::capture
