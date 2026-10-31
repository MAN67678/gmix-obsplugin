// Round-trip for the layer's per-process status file -- the file the OBS
// plugin reads to fill its "apps with the GMix layer loaded" dropdown (see
// src/capture/LayerStatus.hpp). Cheap, no GPU, no sockets.
#include "capture/LayerStatus.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static std::string slurp(const fs::path& p)
{
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return {};
    std::string out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int main()
{
    char tmpl[] = "/tmp/gmix_layer_status_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::perror("mkdtemp");
        return 1;
    }
    setenv("HOME", dir, 1);

    auto& st = gmix::capture::LayerStatus::instance();
    st.set("waiting", "osu!", true);

    const fs::path path = fs::path(dir) / ".cache" / "gmix" / "layers" /
                          (std::to_string(static_cast<long>(getpid())) + ".status");

    const std::string body = slurp(path);
    CHECK(!body.empty());
    CHECK(body.find("pid=") == 0);
    CHECK(body.find("comm=") != std::string::npos);
    CHECK(body.find("state=waiting") != std::string::npos);
    CHECK(body.find("target=osu!") != std::string::npos);
    CHECK(body.find("matched=1") != std::string::npos);

    // A state change rewrites the file in place (atomically).
    st.set("capturing", "osu!", true);
    CHECK(slurp(path).find("state=capturing") != std::string::npos);

    // heartbeat() must bump mtime, at most once a second.
    const auto before = fs::last_write_time(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    st.heartbeat();
    const auto after = fs::last_write_time(path);
    CHECK(after != before);

    // No status file for a process that never called set() is implied by the
    // rate limiter starting empty; nothing to assert here.

    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures == 0) std::printf("layer_status: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
