// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC (Windows) — consumer side (lives in gmix.exe / the OBS plugin).
// Named-pipe analogue of linux-x86_64/src/ipc/frame_receiver.hpp.
//
// Usage:
//   FrameReceiver r;
//   r.listen(gmix::ipc::defaultFramePipeName());
//   FrameHandshake hs; r.acceptProducer(hs);   // blocking
//   ... per frame:
//     FrameHeader h; if (r.recvFrame(h)) { /* open h.exportSlot's texture */ }
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "frame_protocol.hpp"

#include <cstdint>
#include <string>

namespace gmix::ipc {

class FrameReceiver {
public:
    FrameReceiver() = default;
    ~FrameReceiver();

    FrameReceiver(const FrameReceiver&) = delete;
    FrameReceiver& operator=(const FrameReceiver&) = delete;

    // Create the named pipe instance and wait to be ready. Returns false on
    // failure (name already in use by a stuck prior instance, etc).
    bool listen(const std::wstring& pipeName);

    // Blocking accept of a single producer connection. Fills `hs`.
    bool acceptProducer(FrameHandshake& hs);

    // Blocking receive of one frame header. Returns false on disconnect/error.
    bool recvFrame(FrameHeader& out);

    // True if another full frame header is already buffered on the pipe (the
    // consumer is behind). Lets the caller cheaply drop backlog and keep only
    // the freshest frame, bounding end-to-end latency -- same role as the
    // Linux side's hasPendingFrame(). Non-blocking.
    bool hasPendingFrame() const;

    bool isListening() const { return listenPipe_ != nullptr; }
    void close();

private:
    void* listenPipe_ = nullptr;   // HANDLE of the (possibly connected) pipe instance
};

} // namespace gmix::ipc
