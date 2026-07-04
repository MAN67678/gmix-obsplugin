// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC (Windows) — producer side (lives in the opengl32 proxy DLL, inside
// osu!.exe). Named-pipe analogue of linux-x86_64/src/ipc/frame_sender.hpp.
//
// Usage:
//   FrameSender s;
//   s.connect(gmix::ipc::defaultFramePipeName());
//   s.sendHandshake(w, h, dxgiFormat, GetCurrentProcessId());
//   ... per captured frame:
//     s.sendFrame(header);
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "frame_protocol.hpp"

#include <cstdint>
#include <string>

namespace gmix::ipc {

class FrameSender {
public:
    FrameSender() = default;
    ~FrameSender();

    FrameSender(const FrameSender&) = delete;
    FrameSender& operator=(const FrameSender&) = delete;

    // Attempts to open the consumer's named pipe as a client. Retries
    // internally for ~2s (matching the Linux side's connect() budget) before
    // giving up. Returns false if the consumer isn't listening yet.
    bool connect(const std::wstring& pipeName);

    // Send the one-time handshake. Blocks until the consumer acks.
    bool sendHandshake(uint32_t w, uint32_t h, uint32_t dxgiFormat, uint32_t producerPid);

    // Send one frame header. No handles/fds travel with it -- the consumer
    // opens the ring slot's shared texture by name (see frame_protocol.hpp).
    bool sendFrame(const FrameHeader& hdr);

    bool isConnected() const { return pipe_ != nullptr; }
    void disconnect();

private:
    void* pipe_ = nullptr;   // HANDLE, kept void* to avoid <windows.h> in the header
};

} // namespace gmix::ipc
