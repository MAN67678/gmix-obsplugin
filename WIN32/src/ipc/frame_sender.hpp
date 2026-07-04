// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC (Windows) — producer side (lives in the opengl32 proxy DLL, inside
// osu!.exe). Named-pipe analogue of linux-x86_64/src/ipc/frame_sender.hpp.
//
// Usage:
//   FrameSender s;
//   s.connect(gmix::ipc::defaultFramePipeName());
//   s.sendHandshake(w, h, dxgiFormat, GetCurrentProcessId());
//   ... per ring slot, first time it's used on this connection:
//     uint64_t v = s.duplicateHandleToConsumer(localSharedHandle);
//   ... per captured frame:
//     s.sendFrame(header);   // header.sharedHandleValue = v (or 0 if already sent)
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

    // Send one frame header.
    bool sendFrame(const FrameHeader& hdr);

    // Duplicates a LOCAL (unnamed) shared-resource HANDLE into the connected
    // consumer's process, using GetNamedPipeServerProcessId to find its pid
    // (this producer is the pipe's CLIENT; the consumer is the server) --
    // see frame_protocol.hpp's PROTOCOL HISTORY comment for why this exists
    // instead of a named OpenSharedResourceByName lookup. Returns the
    // duplicated handle's raw value (meaningful only in the CONSUMER's
    // process -- do not use it locally), or 0 on failure. The target
    // process handle is opened once (on first call after connect()) and
    // cached until disconnect()/reconnect.
    uint64_t duplicateHandleToConsumer(void* localHandle);

    bool isConnected() const { return pipe_ != nullptr; }
    void disconnect();

private:
    void* pipe_ = nullptr;           // HANDLE, kept void* to avoid <windows.h> in the header
    void* consumerProcess_ = nullptr; // HANDLE to the consumer process, opened lazily
};

} // namespace gmix::ipc
