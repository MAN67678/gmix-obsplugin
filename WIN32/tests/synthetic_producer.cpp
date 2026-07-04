// ─────────────────────────────────────────────────────────────────────────────
// GMix synthetic producer (Windows) -- exercises the consumer/OBS-plugin side
// of the pipeline without a real game or the proxy DLL. Connects to the
// REAL, production named pipe (\\.\pipe\gmix_frames) that a running
// obs-gmix-source instance is listening on, creates a small ring of
// cross-process shareable, keyed-mutex D3D11 textures exactly the way
// gl_dx_interop_capture.cpp does (DuplicateHandle-based -- see
// frame_protocol.hpp's PROTOCOL HISTORY comment for why not by name), and
// streams solid-color frames at a high rate (so the consumer's capture-rate
// estimate settles above 60fps and its shutter actually blends multiple
// distinct frames per output tick, not just N=1 passthrough).
//
// Not a pass/fail unit test (no CHECK macros) -- a manual verification tool.
// Run it, then check the target OBS process's log for "producer connected",
// "first blend retired", and the absence of any gmix-prefixed errors; if a
// "GMix Motion Blur" source is visible in OBS's preview, its color should
// visibly cycle/blend rather than sit on one flat color.
//
// Usage: synthetic_producer.exe [seconds] [width] [height]
//   (defaults: 10s, 640x480)
// ─────────────────────────────────────────────────────────────────────────────
#include "ipc/frame_protocol.hpp"
#include "ipc/frame_sender.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace gmix;

namespace {

constexpr int kRing = 8;

struct RingSlot {
    ID3D11Texture2D* tex = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE           localSharedHandle = nullptr;  // kept open for the ring's lifetime
};

uint64_t nowNs() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return static_cast<uint64_t>(static_cast<double>(ctr.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart));
}

// Distinct, saturated colors so a multi-frame blend is visually obvious
// (the average of two of these is a visibly different hue, not just a
// brightness change).
struct Col { uint8_t r, g, b; };
const Col kPalette[] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255},
};

} // namespace

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    uint32_t w = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 640;
    uint32_t h = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 480;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    // Same D3D_DRIVER_TYPE_HARDWARE / default-adapter choice the real proxy
    // DLL's GlDxInteropCapture::ensureInit() makes -- must land on the SAME
    // physical adapter the consumer's D3D11Context auto-selects, since
    // shared handles don't work across adapters.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {
        std::fprintf(stderr, "synthetic_producer: D3D11CreateDevice failed (hr=0x%08lx)\n",
                    static_cast<unsigned long>(hr));
        return 1;
    }

    uint32_t pid = GetCurrentProcessId();
    std::vector<RingSlot> ring(kRing);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < kRing; ++i) {
        if (dev->CreateTexture2D(&td, nullptr, &ring[i].tex) != S_OK) {
            std::fprintf(stderr, "synthetic_producer: ring texture %d create failed\n", i);
            return 1;
        }
        IDXGIResource1* res1 = nullptr;
        if (ring[i].tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res1)) != S_OK) {
            std::fprintf(stderr, "synthetic_producer: IDXGIResource1 QI failed (slot %d)\n", i);
            return 1;
        }
        // Unnamed handle -- see frame_protocol.hpp's PROTOCOL HISTORY
        // comment (OpenSharedResourceByName is broken on some drivers,
        // confirmed on an AMD RX 480). Kept open; re-DuplicateHandle'd into
        // the consumer once per connection, same as gl_dx_interop_capture.cpp.
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      nullptr, &ring[i].localSharedHandle);
        res1->Release();
        if (FAILED(hr) || !ring[i].localSharedHandle) {
            std::fprintf(stderr, "synthetic_producer: CreateSharedHandle failed (slot %d, hr=0x%08lx)\n",
                        i, static_cast<unsigned long>(hr));
            return 1;
        }
        if (ring[i].tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&ring[i].mutex)) != S_OK) {
            std::fprintf(stderr, "synthetic_producer: keyed mutex QI failed (slot %d)\n", i);
            return 1;
        }
    }
    std::printf("synthetic_producer: created %d shared %ux%u textures (pid=%u)\n", kRing, w, h, pid);

    ipc::FrameSender sender;
    std::printf("synthetic_producer: connecting to %ls ...\n", ipc::defaultFramePipeName().c_str());
    if (!sender.connect(ipc::defaultFramePipeName())) {
        std::fprintf(stderr, "synthetic_producer: connect failed -- is an OBS \"GMix Motion Blur\" source "
                              "running and listening?\n");
        return 1;
    }
    if (!sender.sendHandshake(w, h, static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM), pid)) {
        std::fprintf(stderr, "synthetic_producer: handshake failed\n");
        return 1;
    }
    std::printf("synthetic_producer: handshake ok, streaming for %ds ...\n", seconds);

    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    std::vector<bool> handleSent(kRing, false);
    uint64_t frameIndex = 0;
    int slot = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    // ~300fps: fast enough that several distinct frames land inside the
    // consumer's 16.6ms shutter window, so its dispatch actually blends
    // N>1 frames instead of degenerating to passthrough.
    const auto interval = std::chrono::microseconds(3300);
    auto nextTick = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        auto& rs = ring[slot];
        if (rs.mutex->AcquireSync(0, 50) == S_OK) {
            Col c = kPalette[frameIndex % (sizeof(kPalette) / sizeof(kPalette[0]))];
            for (uint32_t i = 0; i < w * h; ++i) {
                px[i * 4 + 0] = c.r; px[i * 4 + 1] = c.g; px[i * 4 + 2] = c.b; px[i * 4 + 3] = 255;
            }
            ctx->UpdateSubresource(rs.tex, 0, nullptr, px.data(), w * 4, 0);
            rs.mutex->ReleaseSync(1);

            ipc::FrameHeader hdr{};
            hdr.magic = ipc::kMagic;
            hdr.width = w; hdr.height = h;
            hdr.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
            hdr.exportSlot = static_cast<uint32_t>(slot);
            // Only duplicate+send this slot's handle the first time it's
            // used -- see frame_protocol.hpp's PROTOCOL HISTORY comment.
            if (!handleSent[slot]) {
                hdr.sharedHandleValue = sender.duplicateHandleToConsumer(rs.localSharedHandle);
                if (hdr.sharedHandleValue != 0) handleSent[slot] = true;
                else std::fprintf(stderr, "synthetic_producer: duplicateHandleToConsumer failed (slot %d)\n", slot);
            }
            hdr.acquireKey = 1;
            hdr.frameIndex = ++frameIndex;
            hdr.timestampNs = nowNs();
            hdr.rowPitch = static_cast<uint64_t>(w) * 4;
            hdr.gpuTimestampNs = 0;
            if (!sender.sendFrame(hdr)) {
                std::fprintf(stderr, "synthetic_producer: sendFrame failed at frame %llu -- consumer disconnected?\n",
                            static_cast<unsigned long long>(frameIndex));
                break;
            }
            slot = (slot + 1) % kRing;
        }
        nextTick += interval;
        std::this_thread::sleep_until(nextTick);
    }

    std::printf("synthetic_producer: sent %llu frames, disconnecting\n", static_cast<unsigned long long>(frameIndex));
    sender.disconnect();
    return 0;
}
