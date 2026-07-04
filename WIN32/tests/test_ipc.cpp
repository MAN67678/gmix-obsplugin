// ─────────────────────────────────────────────────────────────────────────────
// IPC round-trip test (Windows named pipe). Analogue of
// linux-x86_64/tests/test_ipc.cpp, adapted for the Windows transport: no
// fd-passing exists here (see ipc/frame_protocol.hpp's header comment), so
// instead of verifying fds survive a cross-process dup, this verifies the
// FrameHandshake/FrameHeader scalar fields survive the pipe round-trip, and
// that FrameReceiver::hasPendingFrame() correctly detects backlog -- the
// mechanism the real worker thread relies on to bound latency (see
// receiverThreadFn in obs_plugin/gmix_source.cpp).
//
// Uses a std::thread for the producer role instead of fork() (no fork on
// Windows) -- both a named-pipe server (CreateNamedPipe) and client
// (CreateFile) can run in the same process on different threads, which is
// exactly what the real connector thread + OBS worker thread already do.
// ─────────────────────────────────────────────────────────────────────────────
#include "ipc/frame_protocol.hpp"
#include "ipc/frame_sender.hpp"
#include "ipc/frame_receiver.hpp"
#include "test_macros.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace gmix;
using namespace gmix::ipc;

namespace {

std::wstring testPipeName() {
    return L"\\\\.\\pipe\\gmix-test-ipc";
}

} // namespace

TEST_CASE(ipc_roundtrip_handshake_and_frame) {
    auto path = testPipeName();

    FrameReceiver r;
    CHECK(r.listen(path));
    CHECK(r.isListening());

    std::thread producer([&]() {
        FrameSender s;
        bool connected = s.connect(path);
        CHECK(connected);
        if (!connected) return;
        bool handshakeOk = s.sendHandshake(1920, 1080, 87 /*DXGI_FORMAT_B8G8R8A8_UNORM*/, 1234);
        CHECK(handshakeOk);
        if (!handshakeOk) return;

        FrameHeader hdr{};
        hdr.magic = kMagic;
        hdr.width = 1920; hdr.height = 1080;
        hdr.dxgiFormat = 87;
        hdr.exportSlot = 3;
        hdr.acquireKey = 7;
        hdr.frameIndex = 42;
        hdr.timestampNs = 12345;
        hdr.rowPitch = 999;
        hdr.gpuTimestampNs = 555;
        CHECK(s.sendFrame(hdr));
        s.disconnect();
    });

    FrameHandshake hs{};
    CHECK(r.acceptProducer(hs));
    CHECK_EQ(hs.magic, kMagic);
    CHECK_EQ(hs.protocolVersion, kProtocolVersion);
    CHECK_EQ(hs.frameW, 1920u);
    CHECK_EQ(hs.frameH, 1080u);
    CHECK_EQ(hs.dxgiFormat, 87u);
    CHECK_EQ(hs.producerPid, 1234u);

    FrameHeader f{};
    CHECK(r.recvFrame(f));
    CHECK_EQ(f.exportSlot, 3u);
    CHECK_EQ(f.acquireKey, 7ull);
    CHECK_EQ(f.frameIndex, 42ull);
    CHECK_EQ(f.timestampNs, 12345ull);
    CHECK_EQ(f.rowPitch, 999ull);
    CHECK_EQ(f.gpuTimestampNs, 555ull);

    r.close();
    producer.join();
}

// The consumer worker thread drops any frame it's already behind on (see
// FrameReceiver::hasPendingFrame() and receiverThreadFn's use of it) --
// verify that a frame sent while the previous one hasn't been read yet is
// correctly reported as pending.
TEST_CASE(ipc_backlog_detected_via_has_pending_frame) {
    auto path = testPipeName();

    FrameReceiver r;
    CHECK(r.listen(path));

    std::thread producer([&]() {
        FrameSender s;
        bool connected = s.connect(path);
        CHECK(connected);
        if (!connected) return;
        bool handshakeOk = s.sendHandshake(64, 64, 87, 999);
        CHECK(handshakeOk);
        if (!handshakeOk) return;

        FrameHeader h1{}; h1.magic = kMagic; h1.frameIndex = 1;
        FrameHeader h2{}; h2.magic = kMagic; h2.frameIndex = 2;
        // Sent back-to-back with no consumer read in between -- the
        // realistic scenario the drop logic targets (producer exports far
        // faster than the consumer can import).
        CHECK(s.sendFrame(h1));
        CHECK(s.sendFrame(h2));
        s.disconnect();
    });

    FrameHandshake hs{};
    CHECK(r.acceptProducer(hs));

    FrameHeader first{};
    CHECK(r.recvFrame(first));
    CHECK_EQ(first.frameIndex, 1ull);

    // Give the producer thread time to finish writing the second frame
    // before checking -- hasPendingFrame() is a non-blocking peek, and this
    // test only asserts on the (highly likely) case that it's already fully
    // in the pipe's buffer by this point.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r.hasPendingFrame());

    FrameHeader second{};
    CHECK(r.recvFrame(second));
    CHECK_EQ(second.frameIndex, 2ull);
    CHECK(!r.hasPendingFrame());

    r.close();
    producer.join();
}

int main() {
    std::printf("==== ipc round-trip test ====\n");
    int rc = (g_test_failures == 0) ? 0 : 1;
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return rc;
}
