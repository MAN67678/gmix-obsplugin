// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC — consumer side (lives in the gmix process).
//
// Usage:
//   FrameReceiver r;
//   r.listen(socketPath);     // bind + listen; producer will connect
//   FrameHandshake hs;  r.acceptProducer(hs);   // blocking
//   ... per frame:
//     RecvFrame f;  if (r.recvFrame(f)) { /* f.memFd, f.semFd, f.header */ }
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "frame_protocol.hpp"

#include <cstdint>
#include <string>

namespace gmix::ipc {

struct RecvFrame {
    FrameHeader header;
    int memFd = -1;   // consumer owns; must close after importing
    int semFd = -1;
};

class FrameReceiver {
public:
    FrameReceiver() = default;
    ~FrameReceiver();

    FrameReceiver(const FrameReceiver&) = delete;
    FrameReceiver& operator=(const FrameReceiver&) = delete;

    // Bind + listen at socketPath. Returns false on failure. Cleans up any
    // stale socket file at the path first.
    bool listen(const std::string& socketPath);

    // Blocking accept of a single producer. Fills `hs` with the handshake.
    // Returns false if no listener or accept failed.
    bool acceptProducer(FrameHandshake& hs);

    // Blocking receive of one frame. Returns false on EOF/error/disconnect.
    bool recvFrame(RecvFrame& out);

    // True if another frame is already buffered on the socket (i.e. the
    // consumer is behind). Lets the receiver cheaply drop backlog and keep
    // only the freshest frame, bounding end-to-end latency. Non-blocking.
    bool hasPendingFrame() const;

    bool isListening() const { return listenFd_ >= 0; }
    void close();

private:
    int listenFd_ = -1;   // the listening socket
    int connFd_   = -1;   // the accepted producer connection
};

} // namespace gmix::ipc
