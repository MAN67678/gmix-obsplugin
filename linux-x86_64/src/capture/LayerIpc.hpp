#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>

namespace gmix {
namespace capture {

class LayerIpc {
public:
    LayerIpc();
    ~LayerIpc();

    // Start listening on a unix domain socket. Returns true on success.
    bool start();
    void stop();

    // Send a UTF-8 message to the connected client (non-blocking best-effort).
    bool sendMessage(const std::string& msg);

    // The filesystem path of the socket (for clients to read).
    std::string socketPath() const;

private:
    std::string socketPath_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::thread acceptThread_;
    std::atomic<bool> running_ { false };
    std::mutex sendMu_;
};

} // namespace capture
} // namespace gmix
