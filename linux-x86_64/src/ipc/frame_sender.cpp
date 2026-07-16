#include "frame_sender.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace gmix::ipc {

namespace {

// Send exactly `len` bytes, looping on partial writes.
bool sendAll(int fd, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

FrameSender::~FrameSender() { disconnect(); }

bool FrameSender::connect(const std::string& socketPath) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        ::close(fd_); fd_ = -1; return false;
    }
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    // Retry briefly — the consumer may still be binding.
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            // A stalled/slow consumer must never be able to block the
            // producer indefinitely (the layer calls this from inside the
            // target app's present thread, including sendHandshake()'s ack
            // read). Cap both directions to a few ms so a full socket buffer
            // or an unresponsive peer fails fast instead of hanging the caller.
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 5000; // 5ms
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return true;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) break;
        usleep(10 * 1000);   // 10ms
    }
    ::close(fd_); fd_ = -1;
    return false;
}

bool FrameSender::sendHandshake(uint32_t w, uint32_t h, uint32_t vkFormat) {
    if (fd_ < 0) return false;
    FrameHandshake hs{};
    hs.magic = kMagic;
    hs.protocolVersion = kProtocolVersion;
    hs.frameW = w;
    hs.frameH = h;
    hs.vkFormat = vkFormat;
    if (!sendAll(fd_, &hs, sizeof(hs))) return false;

    uint8_t ack = 0;
    return recvAll(fd_, &ack, 1) && ack == 0;
}

bool FrameSender::sendFrame(const FrameHeader& hdr, int memFd, int semFd) {
    if (fd_ < 0) return false;

    // sendmsg with the header as data and the two fds as SCM_RIGHTS ancillary.
    char cmsgBuf[CMSG_SPACE(2 * sizeof(int))] = {};

    iovec iov{};
    iov.iov_base = const_cast<FrameHeader*>(&hdr);
    iov.iov_len  = sizeof(FrameHeader);

    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    auto* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(2 * sizeof(int));
    int fds[2] = { memFd, semFd };
    std::memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

    ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    // Close the fds regardless of outcome — the kernel dups them into the
    // receiver's fd table on successful sendmsg, so our copies are unneeded.
    if (memFd >= 0) ::close(memFd);
    if (semFd >= 0) ::close(semFd);
    return n == static_cast<ssize_t>(sizeof(FrameHeader));
}

void FrameSender::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

} // namespace gmix::ipc
