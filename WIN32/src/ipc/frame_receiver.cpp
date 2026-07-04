#include "frame_receiver.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gmix::ipc {

FrameReceiver::~FrameReceiver() { close(); }

bool FrameReceiver::listen(const std::wstring& pipeName) {
    close();

    // Single instance, duplex, byte-stream (fixed-size structs make message
    // mode unnecessary), blocking I/O -- mirrors the Linux side's single
    // AF_UNIX listen backlog of effectively one producer.
    HANDLE h = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                 // max instances
        4096, 4096,        // out/in buffer size hints
        0,                 // default timeout
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    listenPipe_ = h;
    return true;
}

bool FrameReceiver::acceptProducer(FrameHandshake& hs) {
    if (!listenPipe_) return false;
    HANDLE h = static_cast<HANDLE>(listenPipe_);

    BOOL connected = ConnectNamedPipe(h, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) return false;

    DWORD read = 0;
    if (!ReadFile(h, &hs, sizeof(hs), &read, nullptr) || read != sizeof(hs)) return false;
    if (hs.magic != kMagic) return false;

    uint8_t ack = 0;
    DWORD written = 0;
    if (!WriteFile(h, &ack, sizeof(ack), &written, nullptr) || written != sizeof(ack)) return false;
    return true;
}

bool FrameReceiver::recvFrame(FrameHeader& out) {
    if (!listenPipe_) return false;
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(listenPipe_), &out, sizeof(out), &read, nullptr) ||
        read != sizeof(out)) {
        return false;
    }
    return true;
}

bool FrameReceiver::hasPendingFrame() const {
    if (!listenPipe_) return false;
    DWORD bytesAvail = 0;
    if (!PeekNamedPipe(static_cast<HANDLE>(listenPipe_), nullptr, 0, nullptr, &bytesAvail, nullptr))
        return false;
    return bytesAvail >= static_cast<DWORD>(sizeof(FrameHeader));
}

void FrameReceiver::close() {
    if (listenPipe_) {
        HANDLE h = static_cast<HANDLE>(listenPipe_);
        DisconnectNamedPipe(h);
        CloseHandle(h);
        listenPipe_ = nullptr;
    }
}

} // namespace gmix::ipc
