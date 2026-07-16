// ─────────────────────────────────────────────────────────────────────────────
// IPC round-trip test.
//
// Forks: child acts as producer (connects, handshakes, sends a frame with two
// real fds), parent acts as consumer (listens, accepts, receives the frame).
// Verifies the fds arrive intact and readable. No GPU involved.
// ─────────────────────────────────────────────────────────────────────────────
#include "ipc/frame_protocol.hpp"
#include "ipc/frame_sender.hpp"
#include "ipc/frame_receiver.hpp"
#include "test_macros.hpp"

#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace gmix;
using namespace gmix::ipc;

namespace {

// Unique socket path in the user's runtime dir so multiple test runs don't
// collide and so it's reachable from the sandbox.
std::string socketPath() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string base = xdg ? xdg : "/tmp";
    return base + "/gmix-test-ipc.sock";
}

// Producer (child) role.
int runProducer(const std::string& path) {
    FrameSender s;
    if (!s.connect(path)) { std::fprintf(stderr, "P: connect failed\n"); return 1; }
    if (!s.sendHandshake(1920, 1080, 37 /*VK_FORMAT_B8G8R8A8_UNORM*/)) {
        std::fprintf(stderr, "P: handshake failed\n"); return 1;
    }
    // Two eventfds as stand-ins for the image-memory + timeline-semaphore fds.
    int memFd = eventfd(0xDEADBEEF, 0);
    int semFd = eventfd(0xCAFEBABE, 0);
    FrameHeader hdr{};
    hdr.magic = kMagic;
    hdr.width = 1920; hdr.height = 1080;
    hdr.vkFormat = 37;
    hdr.semSignalValue = 5;
    hdr.frameIndex = 42;
    hdr.timestampNs = 12345;
    if (!s.sendFrame(hdr, memFd, semFd)) {
        std::fprintf(stderr, "P: sendFrame failed\n"); return 1;
    }
    // Sender closes its copies after sendmsg — fds still valid in receiver.
    s.disconnect();
    return 0;
}

} // namespace

TEST_CASE(ipc_roundtrip_handshake_and_frame) {
    std::string path = socketPath();
    ::unlink(path.c_str());

    FrameReceiver r;
    CHECK(r.listen(path));
    CHECK(r.isListening());

    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // Child — producer.
        _exit(runProducer(path));
    }

    // Parent — consumer.
    FrameHandshake hs{};
    CHECK(r.acceptProducer(hs));
    CHECK_EQ(hs.magic, kMagic);
    CHECK_EQ(hs.protocolVersion, kProtocolVersion);
    CHECK_EQ(hs.frameW, 1920u);
    CHECK_EQ(hs.frameH, 1080u);
    CHECK_EQ(hs.vkFormat, 37u);

    RecvFrame f{};
    CHECK(r.recvFrame(f));
    CHECK(f.memFd >= 0);
    CHECK(f.semFd >= 0);
    CHECK_EQ(f.header.frameIndex, 42ull);
    CHECK_EQ(f.header.semSignalValue, 5ull);

    // The fds are eventfds in the producer's initial-counter form. Read them
    // back to confirm the data survived the cross-process dup.
    eventfd_t val = 0;
    CHECK(::eventfd_read(f.memFd, &val) == 0);
    CHECK_EQ(val, 0xDEADBEEFull);
    CHECK(::eventfd_read(f.semFd, &val) == 0);
    CHECK_EQ(val, 0xCAFEBABEull);

    ::close(f.memFd);
    ::close(f.semFd);
    r.close();

    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

int main() {
    std::printf("==== ipc round-trip test ====\n");
    int rc = (g_test_failures == 0) ? 0 : 1;
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return rc;
}
