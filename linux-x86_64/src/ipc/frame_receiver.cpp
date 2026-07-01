#include "frame_receiver.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace gmix::ipc {

FrameReceiver::~FrameReceiver() { close(); }

bool FrameReceiver::listen(const std::string& socketPath) {
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        ::close(listenFd_); listenFd_ = -1; return false;
    }
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    // Remove a stale socket file from a previous run.
    ::unlink(socketPath.c_str());

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "gmix: ipc: bind(%s) failed: %s\n",
                     socketPath.c_str(), std::strerror(errno));
        ::close(listenFd_); listenFd_ = -1; return false;
    }
    if (::listen(listenFd_, 1) < 0) {
        ::close(listenFd_); listenFd_ = -1; return false;
    }
    return true;
}

bool FrameReceiver::acceptProducer(FrameHandshake& hs) {
    if (listenFd_ < 0) return false;
    connFd_ = ::accept(listenFd_, nullptr, nullptr);
    if (connFd_ < 0) return false;

    auto recvAll = [&](void* buf, size_t len) -> bool {
        auto* p = static_cast<uint8_t*>(buf);
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::recv(connFd_, p + got, len - got, 0);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            if (n == 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    };

    if (!recvAll(&hs, sizeof(hs))) return false;
    if (hs.magic != kMagic) {
        std::fprintf(stderr, "gmix: ipc: bad handshake magic\n");
        return false;
    }
    if (hs.protocolVersion != kProtocolVersion) {
        std::fprintf(stderr, "gmix: ipc: protocol mismatch (got %u, want %u)\n",
                     hs.protocolVersion, kProtocolVersion);
        return false;
    }
    // Ack.
    uint8_t ack = 0;
    return ::send(connFd_, &ack, 1, MSG_NOSIGNAL) == 1;
}

bool FrameReceiver::recvFrame(RecvFrame& out) {
    if (connFd_ < 0) return false;

    // recvmsg: the FrameHeader is the data payload, the two fds come in as
    // SCM_RIGHTS ancillary data in the same message.
    alignas(cmsghdr) char cmsgBuf[CMSG_SPACE(2 * sizeof(int))] = {};
    iovec iov{};
    iov.iov_base = &out.header;
    iov.iov_len  = sizeof(FrameHeader);

    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    out.memFd = -1;
    out.semFd = -1;

    ssize_t n;
    do {
        n = ::recvmsg(connFd_, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(sizeof(FrameHeader))) return false;
    if (out.header.magic != kMagic) {
        std::fprintf(stderr, "gmix: ipc: bad frame magic\n");
        return false;
    }

    // Extract the fds from the ancillary data.
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fds[2] = { -1, -1 };
            std::memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
            out.memFd = fds[0];
            out.semFd = fds[1];
            break;
        }
    }
    if (out.memFd < 0 || out.semFd < 0) {
        std::fprintf(stderr, "gmix: ipc: frame missing fds\n");
        return false;
    }
    return true;
}

bool FrameReceiver::hasPendingFrame() const {
    if (connFd_ < 0) return false;
    struct pollfd pfd{connFd_, POLLIN, 0};
    return ::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

void FrameReceiver::close() {
    if (connFd_   >= 0) { ::close(connFd_);   connFd_   = -1; }
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
}

} // namespace gmix::ipc
