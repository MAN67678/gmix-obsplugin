#include "LayerIpc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace gmix::capture;

LayerIpc::LayerIpc() = default;
LayerIpc::~LayerIpc() { stop(); }

static std::string makeSocketPath() {
    const char* home = std::getenv("HOME");
    if (!home) return std::string("/tmp/gmix_layer.sock");
    std::filesystem::path cache = std::filesystem::path(home) / ".cache" / "gmix";
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    pid_t pid = getpid();
    return (cache / (std::string("gmix_layer_") + std::to_string(pid) + ".sock")).string();
}

bool LayerIpc::start() {
    if (running_) return true;
    socketPath_ = makeSocketPath();

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socketPath_.c_str());

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 1) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread([this]() {
        while (running_) {
            // Poll with a timeout rather than blocking in accept() --
            // close()/shutdown() on listenFd_ from stop() aren't guaranteed
            // to wake a thread parked in accept() on Linux, which would hang
            // the join() in stop() forever. Polling lets us re-check
            // running_ on a short cadence instead.
            struct pollfd pfd{listenFd_, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 200 /*ms*/);
            if (!running_) break;
            if (pr <= 0) continue; // timeout or interrupted; recheck running_
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) break;
            {
                std::lock_guard<std::mutex> lk(sendMu_);
                if (clientFd_ >= 0) ::close(clientFd_);
                clientFd_ = fd;
            }
            // Keep the connection open until client disconnects.
            // Block here until remote closes; but also allow stop() to close fd.
            char buf[1];
            while (running_) {
                ssize_t r = ::recv(clientFd_, buf, 1, MSG_PEEK | MSG_DONTWAIT);
                if (r == 0) break; // peer closed
                if (r < 0) {
                    // no data available; sleep briefly
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }
            std::lock_guard<std::mutex> lk2(sendMu_);
            if (clientFd_ >= 0) { ::close(clientFd_); clientFd_ = -1; }
        }
    });
    return true;
}

void LayerIpc::stop() {
    running_ = false;
    if (listenFd_ >= 0) {
        // shutdown() before close(): close() alone doesn't reliably wake a
        // thread blocked in accept() on this fd on Linux, which would hang
        // the join() below forever.
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (!socketPath_.empty()) {
        ::unlink(socketPath_.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(sendMu_);
        if (clientFd_ >= 0) { ::close(clientFd_); clientFd_ = -1; }
    }
    if (acceptThread_.joinable()) acceptThread_.join();
}

bool LayerIpc::sendMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lk(sendMu_);
    if (clientFd_ < 0) return false;
    ssize_t w = ::send(clientFd_, msg.data(), msg.size(), 0);
    if (w < 0) return false;
    // terminator
    ::send(clientFd_, "\n", 1, 0);
    return true;
}

std::string LayerIpc::socketPath() const { return socketPath_; }
