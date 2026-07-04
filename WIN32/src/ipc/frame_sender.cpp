#include "frame_sender.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <thread>

namespace gmix::ipc {

FrameSender::~FrameSender() { disconnect(); }

bool FrameSender::connect(const std::wstring& pipeName) {
    disconnect();

    // Retry for ~2s, same connect budget as the Linux AF_UNIX side -- the
    // consumer may not have created the pipe yet (this runs on the capture
    // DLL's background connector thread, never the render/present thread).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            pipe_ = h;
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Another client is connecting right now; wait our turn.
        WaitNamedPipeW(pipeName.c_str(), 200);
    }
    return false;
}

bool FrameSender::sendHandshake(uint32_t w, uint32_t h, uint32_t dxgiFormat, uint32_t producerPid) {
    if (!pipe_) return false;
    FrameHandshake hs{};
    hs.magic = kMagic;
    hs.protocolVersion = kProtocolVersion;
    hs.frameW = w;
    hs.frameH = h;
    hs.dxgiFormat = dxgiFormat;
    hs.producerPid = producerPid;

    DWORD written = 0;
    if (!WriteFile(pipe_, &hs, sizeof(hs), &written, nullptr) || written != sizeof(hs)) {
        disconnect();
        return false;
    }

    uint8_t ack = 0xFF;
    DWORD read = 0;
    if (!ReadFile(pipe_, &ack, sizeof(ack), &read, nullptr) || read != sizeof(ack) || ack != 0) {
        disconnect();
        return false;
    }
    return true;
}

bool FrameSender::sendFrame(const FrameHeader& hdr) {
    if (!pipe_) return false;
    DWORD written = 0;
    if (!WriteFile(pipe_, &hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr)) {
        disconnect();
        return false;
    }
    return true;
}

uint64_t FrameSender::duplicateHandleToConsumer(void* localHandle) {
    if (!pipe_ || !localHandle) return 0;

    if (!consumerProcess_) {
        DWORD serverPid = 0;
        // This producer is the pipe's CLIENT (connect()ed via CreateFileW);
        // the consumer is the server (created the pipe via CreateNamedPipeW).
        if (!GetNamedPipeServerProcessId(static_cast<HANDLE>(pipe_), &serverPid)) return 0;
        HANDLE h = OpenProcess(PROCESS_DUP_HANDLE, FALSE, serverPid);
        if (!h) return 0;
        consumerProcess_ = h;
    }

    HANDLE remote = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(localHandle),
                         static_cast<HANDLE>(consumerProcess_), &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(remote);
}

void FrameSender::disconnect() {
    if (consumerProcess_) {
        CloseHandle(static_cast<HANDLE>(consumerProcess_));
        consumerProcess_ = nullptr;
    }
    if (pipe_) {
        CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
}

} // namespace gmix::ipc
