// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC — wire protocol between the capture layer (producer, inside osu!)
// and the gmix process (consumer).
//
// Transport: a unix-domain socket at a path agreed via the GMIX_SOCKET env
// var. The producer connects after the consumer has bound+listen'd.
//
// Messages:
//   1. Handshake: producer sends one FrameHandshake on connect; consumer
//      replies with a single byte ack (0 = ok). Lets the consumer learn the
//      producer's swapchain format/extent before any frames arrive.
//   2. Frame: producer sends a FrameHeader, carrying via SCM_RIGHTS the two
//      file descriptors the consumer needs: the exported image memory fd
//      (OPAQUE_FD, same RADV driver both sides) and the timeline semaphore
//      fd. The consumer imports these with VkImportMemoryFdKHR /
//      VkImportSemaphoreFdKHR, waits on the semaphore (signal value =
//      header.semSignalValue), then samples the image.
//
//      The shared image uses VK_IMAGE_TILING_LINEAR on both sides (matching
//      tiling is required; OPTIMAL's internal layout/compression metadata is
//      implementation-defined and not guaranteed to match between two
//      independently created images sharing memory). The textbook way to
//      make the row layout fully explicit/negotiated -- VK_EXT_image_drm_
//      format_modifier -- is NOT supported by this RADV/Polaris build at
//      all (confirmed via vulkaninfo: present under llvmpipe, absent from
//      the AMD GPU's device extension list), so it isn't usable here.
//      header.rowPitch carries the producer's actual queried pitch
//      (vkGetImageSubresourceLayout) for diagnostic comparison against the
//      consumer's own queried pitch; it is not currently enforced.
//
// All integers little-endian, native is little-endian on x86_64 so the
// structs are directly blittable.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace gmix::ipc {

// Magic so the consumer can sanity-check the producer speaks our protocol.
inline constexpr uint32_t kMagic = 0x47584D58;   // 'G','M','X','\0' LE

// Handshake sent once at connection time.
struct FrameHandshake {
    uint32_t magic;          // kMagic
    uint32_t protocolVersion;// bumped on incompatible changes
    uint32_t frameW;         // producer's swapchain extent
    uint32_t frameH;
    uint32_t vkFormat;       // VkFormat enum value (e.g. B8G8R8A8_UNORM)
};

// Per-frame header. The two file descriptors travel as SCM_RIGHTS ancillary
// data alongside this header in the same sendmsg call.
struct FrameHeader {
    uint32_t magic;            // kMagic
    uint32_t width;
    uint32_t height;
    uint32_t vkFormat;         // VkFormat of the source image
    uint32_t exportSlot;       // producer's export-ring slot (0..kExportRing-1).
                               // The producer reuses a fixed ring of backing
                               // buffers, so the consumer imports each slot once
                               // and reuses it -- the memFd of a repeated slot is
                               // the same underlying memory, so no re-import.
    uint64_t semSignalValue;   // timeline value the producer signaled
    uint64_t frameIndex;       // monotonic producer frame counter
    uint64_t timestampNs;      // CLOCK_MONOTONIC at capture (CPU); drives the latency readout
    uint64_t rowPitch;         // producer's actual DRM_FORMAT_MOD_LINEAR row pitch, in bytes
    uint64_t gpuTimestampNs;   // GPU-domain capture time (vkCmdWriteTimestamp x
                               // timestampPeriod), or 0 if unavailable. Drives the
                               // jitter-free capture-rate / blend-count estimate on
                               // the consumer. Carries an EARLIER frame's GPU time
                               // (the readback is non-blocking, off a completed ring
                               // slot), so only its DELTAS are meaningful -- which is
                               // exactly what the rate estimate consumes.
    float    cursorX;          // Window-normalized cursor position [0,1] at this
    float    cursorY;          // frame's present, read from the target's own SDL3
                               // (SDL_GetMouseState / window size). Normalized (not
                               // pixels) so it's resolution/HiDPI-independent: the
                               // consumer multiplies by the blend-buffer extent to
                               // get the cursor's pixel position IN that frame. The
                               // sequence of these across a blend's source frames is
                               // the cursor's true path through the shutter window.
                               // <0 (e.g. -1) = unavailable this frame (no SDL / no
                               // window focus); the consumer skips such samples.
};

// v3: FrameHeader gained exportSlot (consumer-side import pooling).
// v4: FrameHeader gained gpuTimestampNs (GPU-domain capture-rate timing).
// v5: FrameHeader gained cursorX/cursorY (per-frame true cursor path).
inline constexpr uint32_t kProtocolVersion = 5;

// Fixed rendezvous path for the frame data channel (distinct from LayerIpc's
// per-pid notification socket). Fixed, not per-pid, so the producer can find
// it without any prior discovery step -- the consumer binds here first, the
// producer just keeps retrying a connect to this exact path.
inline std::string defaultFrameSocketPath() {
    const char* home = std::getenv("HOME");
    std::string base = home ? (std::string(home) + "/.cache/gmix") : std::string("/tmp/gmix");
    return base + "/frames.sock";
}

} // namespace gmix::ipc
