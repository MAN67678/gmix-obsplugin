// ─────────────────────────────────────────────────────────────────────────────
// gmix::capture::LayerStatus -- "which apps have the GMix layer loaded?",
// published as one small file per process: ~/.cache/gmix/layers/<pid>.status
//
// Written by the CAPTURE LAYER (inside the game's process, which may itself be
// inside a pressure-vessel/Flatpak sandbox) and read by the OBS plugin to fill
// its "apps with the GMix layer loaded" dropdown.
//
// A file is used instead of /proc or a live socket because OBS (Flatpak) and
// the game may be in different PID namespaces: ~/.cache/gmix is the one
// directory both sides already share (it holds frames.sock), so a plain file
// works for native and sandboxed OBS alike.
//
// Not hot-path: the file is rewritten only when the state changes
// ("not-target" -> "waiting" -> "capturing"), and heartbeat() only bumps its
// mtime at most once a second so the plugin can tell a live process from a
// stale file left behind by a crash/SIGKILL.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace gmix::capture {

class LayerStatus {
public:
    static LayerStatus& instance()
    {
        static LayerStatus s;
        return s;
    }

    // state:   "capturing"  = this process is the target and OBS is connected
    //          "waiting"    = this process is the target, no OBS consumer yet
    //          "not-target" = layer loaded here, but the name didn't match
    // target:  the capture target fragment the layer was told to look for.
    void set(const std::string& state, const std::string& target, bool matched)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ensureDirLocked()) return;
        pathLocked();

        if (wrote_ && writtenPid_ == getpid() && state == state_ &&
            target == target_ && matched == matched_) {
            touchLocked();
            return;
        }
        state_ = state;
        target_ = target;
        matched_ = matched;
        writeLocked();
    }

    // Cheap liveness ping (once per second at most). Called from the present
    // hook; does nothing at all when this process never wrote a status file.
    void heartbeat()
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!wrote_ || writtenPid_ != getpid()) return;
        pathLocked();
        touchLocked();
    }

    LayerStatus(const LayerStatus&) = delete;
    LayerStatus& operator=(const LayerStatus&) = delete;

private:
    LayerStatus() = default;
    ~LayerStatus()
    {
        // Only the process that wrote the file removes it -- a fork'd child
        // inherits this singleton and must not delete its parent's status.
        if (wrote_ && writtenPid_ == getpid()) unlink(path_.c_str());
    }

    bool ensureDirLocked()
    {
        if (!dir_.empty()) return true;
        const char* home = std::getenv("HOME");
        if (!home || !*home) return false;
        std::filesystem::path dir =
            std::filesystem::path(home) / ".cache" / "gmix" / "layers";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
        dir_ = dir.string();
        return true;
    }

    void pathLocked()
    {
        path_ = dir_ + "/" + std::to_string(static_cast<long>(getpid())) + ".status";
    }

    void touchLocked()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - lastTouch_ < std::chrono::seconds(1)) return;
        lastTouch_ = now;
        // Missing file (deleted by hand / by the plugin): rewrite it.
        if (utimensat(AT_FDCWD, path_.c_str(), nullptr, 0) != 0) writeLocked();
    }

    void writeLocked()
    {
        const std::string comm = readFirstLine("/proc/self/comm");
        const std::string exe  = readLink("/proc/self/exe");
        const std::string tmp  = path_ + ".tmp";

        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "pid=%d\ncomm=%s\nexe=%s\nstate=%s\ntarget=%s\nmatched=%d\n",
                     static_cast<int>(getpid()), comm.c_str(), exe.c_str(),
                     state_.c_str(), target_.c_str(), matched_ ? 1 : 0);
        if (std::fclose(f) != 0) {
            unlink(tmp.c_str());
            return;
        }
        // Atomic replace: the plugin must never parse a half-written file.
        if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
            unlink(tmp.c_str());
            return;
        }
        wrote_ = true;
        writtenPid_ = getpid();
        lastTouch_ = std::chrono::steady_clock::now();
    }

    static std::string readFirstLine(const char* path)
    {
        FILE* f = std::fopen(path, "r");
        if (!f) return {};
        char buf[512] = {};
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::string s(buf, n);
        const auto nl = s.find('\n');
        if (nl != std::string::npos) s.resize(nl);
        return s;
    }

    static std::string readLink(const char* path)
    {
        char buf[4096] = {};
        const ssize_t n = ::readlink(path, buf, sizeof(buf) - 1);
        return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
    }

    std::mutex mu_;
    std::string dir_;
    std::string path_;
    std::string state_;
    std::string target_;
    bool  matched_ = false;
    bool  wrote_   = false;
    pid_t writtenPid_ = 0;
    std::chrono::steady_clock::time_point lastTouch_{};
};

} // namespace gmix::capture
