// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC (Windows) — wire protocol between the capture proxy DLL (producer,
// inside osu!) and the gmix consumer (standalone gmix.exe / the OBS plugin).
//
// Transport: a named pipe, \\.\pipe\gmix_frames (byte-stream). The producer
// connects after the consumer has created + is waiting on the pipe. This
// directly mirrors the Linux AF_UNIX socket at ~/.cache/gmix/frames.sock
// (see the Linux frame_protocol.hpp for the original design notes) with ONE
// structural difference: Linux passes file descriptors as SCM_RIGHTS
// ancillary data alongside each message; Windows has no fd-passing
// primitive as clean as that, so instead each ring-slot D3D11 texture is
// shared via a `DuplicateHandle`d Win32 HANDLE, whose raw value (valid only
// in the consumer's process, per DuplicateHandle semantics) travels in
// FrameHeader::sharedHandleValue.
//
// PROTOCOL HISTORY -- named sharing does NOT work on all drivers: the
// original design here used `ID3D11Device1::OpenSharedResourceByName` (a
// texture named `Local\gmix_frame_<pid>_<slot>`, looked up by the consumer
// without any handle ever crossing the wire). That was confirmed BROKEN on
// an AMD RX 480 -- `OpenSharedResourceByName` returns E_INVALIDARG
// unconditionally on this driver, even for a same-process/same-device open
// (isolated with a throwaway probe before touching any cross-process code).
// `ID3D11Device1::OpenSharedResource1` (classic handle-based open) works
// fine on the same driver, hence this design: the producer duplicates each
// ring slot's local (unnamed) shared handle into the consumer's process
// once per connection (via `GetNamedPipeServerProcessId` + `DuplicateHandle`
// -- see frame_sender.hpp) and sends the resulting handle value.
//
// Sync primitive note (deviates from the original plan of a separate named
// ID3D11Fence): D3D11 has no OpenSharedFence-by-name API (ID3D11Device5::
// OpenSharedFence only accepts a raw HANDLE, and handles are not valid across
// processes without DuplicateHandle -- which we now do anyway for the
// texture, but a SEPARATE object per frame for the fence would be needless
// overhead). IDXGIKeyedMutex, by contrast, travels WITH the shared texture
// itself (QueryInterface on the same opened resource) and needs no separate
// handle at all -- so each ring-slot texture carries its own keyed mutex,
// and `acquireKey` below is the key the consumer must IDXGIKeyedMutex::
// AcquireSync() with to safely read that slot's pixels (the producer wrote
// them, then released with that same key).
//
// Messages:
//   1. Handshake: producer sends one FrameHandshake on connect; consumer
//      replies with a single byte ack (0 = ok). Lets the consumer learn the
//      producer's swapchain format/extent before any frames arrive.
//   2. Frame: producer sends one FrameHeader per exported frame.
//      `sharedHandleValue` is non-zero only the FIRST time a given
//      `exportSlot` is sent on a connection (the producer tracks this per
//      slot, reset on each new connection -- see gl_dx_interop_capture.cpp);
//      0 means "you've already opened and cached this slot, ignore this
//      field." The consumer opens a non-zero handle via `OpenSharedResource1`,
//      QueryInterfaces its `IDXGIKeyedMutex`, caches both by slot (see
//      ImportedFrame.hpp), and closes the raw handle immediately after
//      (D3D keeps its own reference; the Win32 handle itself is only needed
//      for the OpenSharedResource1 call).
//
// All integers little-endian (native on x86_64), so the structs are directly
// blittable, same as the Linux side.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>

namespace gmix::ipc {

// Monotonic, system-wide-comparable timestamp (QueryPerformanceCounter-
// based). QPC is calibrated once system-wide, not per-process, so
// subtracting a timestamp captured in the producer from one captured in the
// consumer yields a real wall-clock duration despite crossing a process
// boundary -- the same trick the CLI's cross-process latency readout
// already relies on. Producer and consumer both use this exact helper (for
// FrameHeader::timestampNs and all diagnostic-latency computations) so
// there's only one implementation to keep in sync.
inline uint64_t nowNs() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return static_cast<uint64_t>(static_cast<double>(ctr.QuadPart) * 1e9 / static_cast<double>(freq.QuadPart));
}

// Magic so the consumer can sanity-check the producer speaks our protocol.
// Distinct from the Linux magic (0x47584D58) so a stray cross-platform
// connection attempt (there never should be one) fails the check instead of
// silently misparsing. Bumped from the original named-sharing protocol
// version since the wire format changed (see PROTOCOL HISTORY above).
inline constexpr uint32_t kMagic = 0x47584D58;   // 'G','X','M','X' (Windows, v2)

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
    uint32_t producerPid;     // producer process id -- informational/logging only
                              // now (no longer used to derive a shared-resource
                              // name; see PROTOCOL HISTORY above).
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
    uint64_t sharedHandleValue;// non-zero only the first time this exportSlot is
                               // sent on this connection: a HANDLE, already
                               // DuplicateHandle'd into the CONSUMER's process by
                               // the producer, ready for OpenSharedResource1().
                               // 0 means "already opened and cached this slot."
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

inline constexpr uint32_t kProtocolVersion = 2;

// Fixed rendezvous pipe name (distinct from any per-pid notification channel,
// mirroring the Linux side's fixed frames.sock path for the same reason: the
// producer just keeps retrying a connect to this exact name with no prior
// discovery step).
inline std::wstring defaultFramePipeName() {
    return L"\\\\.\\pipe\\gmix_frames";
}

} // namespace gmix::ipc
