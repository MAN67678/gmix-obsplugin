// ─────────────────────────────────────────────────────────────────────────────
// GMix IPC — producer side (lives in the capture layer, inside osu!).
//
// Usage:
//   FrameSender s;
//   s.connect("/run/user/1000/gmix.sock");   // from GMIX_SOCKET env
//   s.sendHandshake(w, h, format);
//   ... per captured frame:
//     s.sendFrame(header, memFd, semFd);
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

    // Blocking connect to the consumer's listening socket. Returns false on
    // failure (consumer not up yet, etc.).
    bool connect(const std::string& socketPath);

    // Send the one-time handshake. Blocks until the consumer acks.
    bool sendHandshake(uint32_t w, uint32_t h, uint32_t vkFormat);

    // Send a frame: header via the data payload, the two fds via SCM_RIGHTS.
    // Takes ownership of the fds (closes them after sendmsg).
    bool sendFrame(const FrameHeader& hdr, int memFd, int semFd);

    bool isConnected() const { return fd_ >= 0; }
    void disconnect();

private:
    int fd_ = -1;
};

} // namespace gmix::ipc
