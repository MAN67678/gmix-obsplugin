// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC (Windows) — wire protocol between the capture proxy DLL (producer,
// inside osu!) and the gmix consumer (standalone gmix.exe / the OBS plugin).
//
// Transport: a named pipe, \\.\pipe\gmix_frames (byte-stream, message mode).
// The producer connects after the consumer has created + is waiting on the
// pipe. This directly mirrors the Linux AF_UNIX socket at
// ~/.cache/gmix/frames.sock (see the Linux frame_protocol.hpp for the
// original design notes) with ONE structural difference: Linux passes file
// descriptors as SCM_RIGHTS ancillary data alongside each message; Windows has
// no cross-process fd-passing primitive as clean as that, so instead every
// GPU resource (the export-ring texture, and its keyed mutex) is a NAMED
// D3D11 shared resource, and only the NAME (deterministically derived from
// producerPid + exportSlot, so it never even needs to travel on the wire) plus
// small per-frame scalars travel over the pipe.
//
// Sync primitive note (deviates from the original plan of a separate named
// ID3D11Fence): D3D11 has no OpenSharedFence-by-name API (ID3D11Device5::
// OpenSharedFence only accepts a raw HANDLE, and handles are not valid across
// processes without DuplicateHandle). IDXGIKeyedMutex, by contrast, travels
// WITH the shared texture itself (QueryInterface on the same named/opened
// resource) and needs no separate named object at all -- so each ring-slot
// texture carries its own keyed mutex, and `acquireKey` below is the key the
// consumer must IDXGIKeyedMutex::AcquireSync() with to safely read that slot's
// pixels (the producer wrote them, then released with that same key).
//
// Messages:
//   1. Handshake: producer sends one FrameHandshake on connect; consumer
//      replies with a single byte ack (0 = ok). Lets the consumer learn the
//      producer's swapchain format/extent + pid (needed to build shared-
//      resource names) before any frames arrive.
//   2. Frame: producer sends one FrameHeader per exported frame. The consumer
//      opens (once per exportSlot, then caches -- see ImportedFrame.hpp)
//      the ring slot's shared D3D11 texture by name, QueryInterfaces its
//      IDXGIKeyedMutex, and AcquireSync(acquireKey) before reading.
//
// All integers little-endian (native on x86_64), so the structs are directly
// blittable, same as the Linux side.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>

namespace gmix::ipc {

// Magic so the consumer can sanity-check the producer speaks our protocol.
// Distinct from the Linux magic (0x47584D58) so a stray cross-platform
// connection attempt (there never should be one) fails the check instead of
// silently misparsing.
inline constexpr uint32_t kMagic = 0x47584D57;   // 'G','X','M','W' (Windows)

// Handshake sent once at connection time.
struct FrameHandshake {
    uint32_t magic;           // kMagic
    uint32_t protocolVersion; // bumped on incompatible changes
    uint32_t frameW;          // producer's swapchain extent
    uint32_t frameH;
    uint32_t dxgiFormat;      // DXGI_FORMAT enum value of the shared ring textures
                              // (always DXGI_FORMAT_R8G8B8A8_UNORM today -- see
                              // gl_dx_interop_capture.cpp for why the OpenGL
                              // backbuffer is normalized to this on export)
    uint32_t producerPid;     // producer process id -- the consumer derives every
                              // ring slot's shared-resource name from this + the
                              // slot index (see sharedTextureName() below), so no
                              // name string ever needs to travel on the wire.
};

// Per-frame header.
struct FrameHeader {
    uint32_t magic;            // kMagic
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat;
    uint32_t exportSlot;       // producer's export-ring slot (0..kExportRing-1).
                               // The producer reuses a fixed ring of backing
                               // textures, so the consumer opens each slot once
                               // and reuses the handle -- see FrameTexturePool.
    uint64_t acquireKey;       // key to pass to this slot's IDXGIKeyedMutex::
                               // AcquireSync() before reading (the producer
                               // released with this same key after writing).
    uint64_t frameIndex;       // monotonic producer frame counter
    uint64_t timestampNs;      // QueryPerformanceCounter-derived ns at capture
                               // (CPU domain); drives the latency readout.
    uint64_t rowPitch;         // producer's actual row pitch, in bytes (diagnostic
                               // only -- D3D11 shared textures don't need an
                               // explicit tiling negotiation the way the Vulkan
                               // OPAQUE_FD/LINEAR-tiling path did).
    uint64_t gpuTimestampNs;   // GPU-domain capture time if available, else 0.
                               // Same jitter-free-capture-rate-estimate role as
                               // the Linux side's gpuTimestampNs.
};

inline constexpr uint32_t kProtocolVersion = 1;

// Fixed rendezvous pipe name (distinct from any per-pid notification channel,
// mirroring the Linux side's fixed frames.sock path for the same reason: the
// producer just keeps retrying a connect to this exact name with no prior
// discovery step).
inline std::wstring defaultFramePipeName() {
    return L"\\\\.\\pipe\\gmix_frames";
}

// Deterministic per-slot shared D3D11 texture name. Never sent on the wire --
// both the producer (when creating the ring) and the consumer (when opening a
// slot for the first time, using FrameHeader::exportSlot + the handshake's
// producerPid) compute the identical string. "Local\" (not "Global\") keeps
// this within the current login session, which is required anyway since both
// processes need access to the same GPU/display session.
inline std::wstring sharedTextureName(uint32_t producerPid, uint32_t slot) {
    wchar_t buf[64];
    swprintf(buf, 64, L"Local\\gmix_frame_%u_%02u", producerPid, slot);
    return buf;
}

} // namespace gmix::ipc
