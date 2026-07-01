// ─────────────────────────────────────────────────────────────────────────────
// GMix as a native OBS source: "GMix Motion Blur".
//
// Runs the SAME capture-receive + async-compute blend pipeline as the
// standalone `gmix` binary (via gmix_core), but headless -- no Wayland
// window, no swapchain, no v4l2/PipeWire sink. The blend's dst images are
// dma-buf exported (BlendEngine::dmaBufFd, see blend_engine.cpp) and
// imported ONCE as OBS gs_texture_t handles via gs_texture_create_from_dmabuf
// (zero-copy: OBS's GL context reads the same GPU memory gmix's compute
// queue wrote, no CPU readback). video_render() just draws whichever texture
// is currently the front buffer.
//
// Settings (target process name, GPU index) are exposed through the normal
// OBS source-properties dialog (get_properties/get_defaults/update) so they
// persist in the scene collection like any other OBS source -- no separate
// gmix_config.ini to hand-edit for this path.
// ─────────────────────────────────────────────────────────────────────────────
#include <obs-module.h>
#include <graphics/graphics.h>

#include "../vulkan/context.hpp"
#include "../blend/blend_engine.hpp"
#include "../blend/BlendConfig.hpp"
#include "../ipc/frame_receiver.hpp"
#include "../ipc/imported_frame.hpp"
#include "../gmix.hpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <fstream>
#include <filesystem>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-gmix-source", "en-US")

namespace {

constexpr const char* kSettingTarget   = "target_process";
constexpr const char* kSettingGpuIndex = "gpu_index";
constexpr const char* kDefaultTarget   = "osu!";

// The capture LAYER (a separate process, injected into the game) does NOT
// read GMIX_TARGET_PROCESS -- that env var is vestigial. It reads this file
// once at its own vkCreateInstance time and stays disabled for the process's
// whole lifetime if it's empty/missing (see layer/layer_entry.cpp's
// readTargetProcessConfig()/ensureCaptureInitialized()). The standalone
// `gmix` binary used to write it on every run (main.cpp's
// writeTargetProcessConfig); since gmix now runs as an OBS plugin instead,
// THIS is the only remaining writer -- must run before the game's Vulkan
// instance is created, so write it eagerly in create()/update(), not lazily
// after a producer connects.
bool writeTargetProcessConfig(const std::string& target) {
    if (target.empty()) return false;
    const char* home = std::getenv("HOME");
    if (!home) return false;
    std::filesystem::path configDir = std::filesystem::path(home) / ".config" / "gmix";
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) return false;
    std::ofstream out(configDir / "target_process");
    if (!out) return false;
    out << target;
    return out.good();
}

// ── Sliding frame window (verbatim port of main.cpp's FrameQueue) ──────────
// Same ring-buffer/backpressure/EMA-rate design as the standalone consumer:
// the ring always holds the last kCap arrived frames; every tick blends
// whatever is currently in it. See src/main.cpp for the full rationale.
struct RingEntry {
    std::shared_ptr<gmix::ImportedFrame> frame;
    uint64_t timestampNs = 0;
};

struct FrameQueue {
    std::mutex mu;
    std::deque<RingEntry> ring;
    uint32_t trackedW = 0, trackedH = 0;
    std::atomic<bool> pendingResize{false};
    std::atomic<bool> disconnected{false};
    uint32_t resizeW = 0, resizeH = 0;
    std::atomic<uint64_t> arrivals{0};
    std::atomic<uint64_t> emaIntervalNs{0};
    uint64_t lastRateTs = 0;
    bool     lastRateGpu = false;

    static constexpr size_t kCap = gmix::kMaxBlendFrames;

    void push(std::shared_ptr<gmix::ImportedFrame> f, uint32_t w, uint32_t h,
              uint64_t cpuTs, uint64_t gpuTs) {
        std::lock_guard<std::mutex> lk(mu);
        if (w != trackedW || h != trackedH) {
            ring.clear();
            trackedW = w; trackedH = h;
            resizeW = w; resizeH = h;
            pendingResize = true;
            lastRateTs = 0;
        }
        uint64_t rateTs = gpuTs ? gpuTs : cpuTs;
        bool gpuDomain = (gpuTs != 0);
        if (lastRateTs != 0 && gpuDomain == lastRateGpu && rateTs > lastRateTs) {
            uint64_t interval = rateTs - lastRateTs;
            uint64_t prev = emaIntervalNs.load(std::memory_order_relaxed);
            uint64_t ema = (prev == 0) ? interval
                          : static_cast<uint64_t>(prev * 0.9 + interval * 0.1);
            emaIntervalNs.store(ema, std::memory_order_relaxed);
        }
        lastRateTs = rateTs;
        lastRateGpu = gpuDomain;
        ring.push_back({std::move(f), cpuTs});
        while (ring.size() > kCap) ring.pop_front();
        ++arrivals;
    }

    double captureFps() const {
        uint64_t e = emaIntervalNs.load(std::memory_order_relaxed);
        return e > 0 ? 1e9 / static_cast<double>(e) : 0.0;
    }

    std::vector<RingEntry> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return std::vector<RingEntry>(ring.begin(), ring.end());
    }
};

void receiverThreadFn(gmix::ipc::FrameReceiver& receiver, gmix::VulkanContext& vk,
                      FrameQueue& q, gmix::FrameImagePool& pool,
                      gmix::SemaphorePool& semPool) {
    gmix::ipc::RecvFrame rf{};
    while (receiver.recvFrame(rf)) {
        if (receiver.hasPendingFrame()) {
            if (rf.memFd >= 0) ::close(rf.memFd);
            if (rf.semFd >= 0) ::close(rf.semFd);
            continue;
        }
        auto img = pool.acquire(vk, rf.header.exportSlot, rf.memFd,
                                rf.header.width, rf.header.height,
                                static_cast<VkFormat>(rf.header.vkFormat),
                                rf.header.rowPitch);
        auto sem = semPool.acquire(vk, rf.header.exportSlot, rf.semFd);
        auto frame = std::make_shared<gmix::ImportedFrame>();
        if (!frame->init(vk, img, std::move(sem), rf.header.semSignalValue)) continue;
        q.push(std::move(frame), rf.header.width, rf.header.height,
               rf.header.timestampNs, rf.header.gpuTimestampNs);
    }
    q.disconnected = true;
}

// ── Plugin instance state ───────────────────────────────────────────────────
struct GmixSource {
    obs_source_t* source = nullptr;
    std::string targetProcess = kDefaultTarget;
    int32_t gpuIndex = -1;

    std::thread worker;
    std::atomic<bool> stop{false};

    gmix::VulkanContext vk;
    gmix::BlendEngine* blend = nullptr;   // heap: BlendEngine has no default ctor
    bool blendReady = false;

    // gs_texture_t handles imported ONCE per dst buffer (see workerMain()).
    gs_texture_t* tex[gmix::BlendEngine::kDstBuffers] = {};
    std::atomic<int> frontIdx{-1};
    std::mutex texMu;   // guards tex[] creation vs. video_render reading it

    uint32_t width = 0, height = 0;

    ~GmixSource() { delete blend; }
};

void workerMain(GmixSource* s) {
    gmix::BlendConfig config;   // Flat mode, defaults -- only used for weightsFor()

    while (!s->stop.load()) {
        gmix::ipc::FrameReceiver receiver;
        const std::string sockPath = gmix::ipc::defaultFrameSocketPath();
        if (!receiver.listen(sockPath)) {
            blog(LOG_ERROR, "gmix: failed to listen on %s", sockPath.c_str());
            return;
        }
        blog(LOG_INFO, "gmix: waiting for producer to connect on %s ...", sockPath.c_str());

        gmix::ipc::FrameHandshake hs{};
        if (!receiver.acceptProducer(hs)) {
            receiver.close();
            if (s->stop.load()) return;
            continue;
        }
        blog(LOG_INFO, "gmix: producer connected: %ux%u format=%u",
             hs.frameW, hs.frameH, hs.vkFormat);

        if (!s->blendReady) {
            s->blend = new gmix::BlendEngine(s->vk);
            if (!s->blend->init(hs.frameW, hs.frameH)) {
                blog(LOG_ERROR, "gmix: blend engine init failed");
                return;
            }
            if (!s->blend->dmaBufCapable()) {
                blog(LOG_ERROR, "gmix: blend dst images are not dma-buf exportable on this GPU -- "
                                 "zero-copy delivery to OBS is unavailable");
                return;
            }
            s->width = hs.frameW;
            s->height = hs.frameH;
            s->blendReady = true;
        }

        FrameQueue queue;
        queue.trackedW = hs.frameW;
        queue.trackedH = hs.frameH;
        gmix::FrameImagePool framePool;
        gmix::SemaphorePool  semPool;
        std::thread receiverThread(receiverThreadFn, std::ref(receiver), std::ref(s->vk),
                                   std::ref(queue), std::ref(framePool), std::ref(semPool));

        int frontIdx = -1;
        uint32_t inFlightIdx = 0;
        uint64_t lastBlendArrivals = 0;
        // Cap how often a NEW blend is launched to the shutter design rate
        // (60/s) -- separate from how often we POLL for completion/refresh
        // frontIdx (that stays fast, every ~1ms, for smooth pacing). Without
        // this cap, removing the old fixed-Hz output clock (which incidentally
        // also rate-limited dispatch) let a new blend launch on EVERY new
        // arrival -- up to osu!'s capture rate (600-1000/s) -- burning far
        // more CPU/GPU than the 60fps shutter needs and stealing cycles from
        // the game. Blending faster than the shutter rate has no visual
        // benefit: each output frame is still a 1/60s-window average.
        const auto kMinDispatchInterval = std::chrono::nanoseconds(1'000'000'000ull / 60);
        auto lastDispatchTime = std::chrono::steady_clock::now() - kMinDispatchInterval;

        while (!queue.disconnected && !s->stop.load()) {
            if (queue.pendingResize.exchange(false)) {
                s->blend->waitBlendDone();
                if (!s->blend->init(queue.resizeW, queue.resizeH)) {
                    blog(LOG_ERROR, "gmix: blend engine re-init failed");
                    s->stop = true;
                    break;
                }
                s->width = queue.resizeW;
                s->height = queue.resizeH;
                frontIdx = -1;
                // dst images (and their dma-buf fds) were just recreated --
                // drop the old gs_texture_t imports; workerMain re-imports below.
                std::lock_guard<std::mutex> lk(s->texMu);
                for (auto& t : s->tex) { if (t) { obs_enter_graphics(); gs_texture_destroy(t); obs_leave_graphics(); t = nullptr; } }
                s->frontIdx.store(-1);
            }

            // Import each dst buffer's dma-buf ONCE (lazily, first time we see
            // it, or after the resize teardown above) -- matches the project's
            // established "export/import once, reuse" pattern.
            {
                std::lock_guard<std::mutex> lk(s->texMu);
                for (uint32_t i = 0; i < gmix::BlendEngine::kDstBuffers; ++i) {
                    if (s->tex[i]) continue;
                    int fd = s->blend->dmaBufFd(i);
                    if (fd < 0) continue;
                    uint32_t stride = s->blend->dmaBufStride(i);
                    uint64_t offset = s->blend->dmaBufOffset(i);
                    int fds[1] = { fd };
                    uint32_t strides[1] = { stride };
                    uint32_t offsets[1] = { static_cast<uint32_t>(offset) };
                    uint64_t modifiers[1] = { 0 };  // DRM_FORMAT_MOD_LINEAR
                    obs_enter_graphics();
                    // DRM_FORMAT_ABGR8888 = 'AB24' -- matches VK_FORMAT_R8G8B8A8_UNORM's
                    // byte order (R,G,B,A in memory); GS_RGBA is the matching OBS format.
                    constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241;
                    s->tex[i] = gs_texture_create_from_dmabuf(
                        s->width, s->height, kDrmFormatAbgr8888, GS_RGBA,
                        1, fds, strides, offsets, modifiers);
                    obs_leave_graphics();
                    if (!s->tex[i]) {
                        blog(LOG_ERROR, "gmix: gs_texture_create_from_dmabuf failed for dst[%u]", i);
                    } else {
                        blog(LOG_INFO, "gmix: dst[%u] imported as gs_texture_t (fd=%d stride=%u offset=%llu %ux%u)",
                             i, fd, stride, (unsigned long long)offset, s->width, s->height);
                    }
                }
            }

            if (s->blend->pollBlendDone()) {
                frontIdx = static_cast<int>(inFlightIdx);
                s->frontIdx.store(frontIdx, std::memory_order_release);
                static bool loggedFirstFrame = false;
                if (!loggedFirstFrame) {
                    blog(LOG_INFO, "gmix: first blend retired, front=%d -- video_render should now draw", frontIdx);
                    loggedFirstFrame = true;
                }
            }

            // Cheap atomic check BEFORE paying for queue.snapshot() (a mutex
            // lock + shared_ptr-refcounted copy of up to kCap=64 entries) --
            // at a 1ms poll interval this check fails ~59 times out of 60
            // when nothing new has arrived, so skipping the snapshot in that
            // case avoids ~64 atomic refcount ops per idle tick. Also rate-
            // limited to kMinDispatchInterval (see above) -- new arrivals can
            // exist far more often than that.
            auto nowTick = std::chrono::steady_clock::now();
            uint64_t arrivalsNow = queue.arrivals.load();
            if (!s->blend->blendInFlight() && arrivalsNow != lastBlendArrivals &&
                nowTick - lastDispatchTime >= kMinDispatchInterval) {
                auto window_frames = queue.snapshot();
                if (!window_frames.empty()) {
                    const size_t wsz = window_frames.size();
                    double capFps = queue.captureFps();
                    if (capFps <= 0.0) capFps = 60.0;
                    size_t n = static_cast<size_t>(capFps / 60.0 + 0.5);
                    if (n < 1) n = 1;
                    n = std::min(n, static_cast<size_t>(config.maxBlendFrames()));
                    n = std::min(n, wsz);

                    std::vector<VkImageView> srcViews(n);
                    std::vector<VkSemaphore> waitSems;
                    std::vector<uint64_t>    waitVals;
                    waitSems.reserve(n);
                    waitVals.reserve(n);
                    for (size_t i = 0; i < n; ++i) {
                        auto& f = window_frames[wsz - 1 - i].frame;
                        srcViews[i] = f->view();
                        VkSemaphore sem = f->producerSemaphore();
                        if (sem == VK_NULL_HANDLE) continue;
                        uint64_t v = f->producerWaitValue();
                        bool merged = false;
                        for (size_t k = 0; k < waitSems.size(); ++k) {
                            if (waitSems[k] == sem) { if (v > waitVals[k]) waitVals[k] = v; merged = true; break; }
                        }
                        if (!merged) { waitSems.push_back(sem); waitVals.push_back(v); }
                    }
                    std::vector<float> weights = config.weightsFor(static_cast<int>(n));

                    uint32_t back = (frontIdx == 0) ? 1u : 0u;
                    if (s->blend->dispatchAsync(srcViews.data(), weights.data(),
                                                static_cast<uint32_t>(n), back,
                                                waitSems.data(), waitVals.data(),
                                                static_cast<uint32_t>(waitSems.size()))) {
                        inFlightIdx = back;
                        lastBlendArrivals = arrivalsNow;
                        lastDispatchTime = nowTick;
                    }
                }
            }

            // No internal output clock here, unlike the standalone `gmix`
            // consumer (which owned final output pacing and vblank-locked to
            // it). As an OBS source, OBS's own render loop is what paces the
            // actual output -- video_render() just draws whatever frontIdx
            // currently is, whenever OBS calls it. A fixed-Hz sleep_until
            // grid here would just be a second, unsynchronized clock racing
            // OBS's real vsync, producing duplicated/skipped frames. Instead:
            // spin at a short fixed interval so pollBlendDone()/new-arrival
            // checks stay responsive (a blend retiring a few hundred
            // microseconds late doesn't matter -- OBS reads whatever is
            // front at ITS next render tick), without busy-spinning a core.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        receiver.close();
        if (receiverThread.joinable()) receiverThread.join();
        if (s->stop.load()) break;
        blog(LOG_INFO, "gmix: producer disconnected, waiting for a new one");
    }
}

// ── obs_source_info callbacks ───────────────────────────────────────────────

const char* gmixGetName(void*) { return "GMix Motion Blur"; }

void gmixGetDefaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, kSettingTarget, kDefaultTarget);
    obs_data_set_default_int(settings, kSettingGpuIndex, -1);
}

obs_properties_t* gmixGetProperties(void*) {
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, kSettingTarget,
                            "Capture target (process name fragment)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, kSettingGpuIndex, "GPU index (-1 = auto)", -1, 15, 1);
    return props;
}

void gmixUpdate(void* data, obs_data_t* settings) {
    auto* s = static_cast<GmixSource*>(data);
    // Settings persist in the scene collection automatically via obs_data_t;
    // GMIX_TARGET_PROCESS is read by the capture LAYER (a separate process,
    // injected into the game), not this plugin, so changing it here only
    // takes effect on the next time the game is (re)launched with it applied
    // (see config/gmix_launch.sh) -- this plugin's copy is informational/
    // for a future "launch game" convenience button, not consumed by the
    // running worker thread.
    const char* target = obs_data_get_string(settings, kSettingTarget);
    s->targetProcess = (target && *target) ? target : kDefaultTarget;
    s->gpuIndex = static_cast<int32_t>(obs_data_get_int(settings, kSettingGpuIndex));
    if (!writeTargetProcessConfig(s->targetProcess)) {
        blog(LOG_WARNING, "gmix: failed to write ~/.config/gmix/target_process -- "
                           "the capture layer will not activate in the game");
    }
}

void* gmixCreate(obs_data_t* settings, obs_source_t* source) {
    auto* s = new GmixSource();
    s->source = source;
    gmixUpdate(s, settings);

    if (!s->vk.init(s->gpuIndex, /*headless=*/true)) {
        blog(LOG_ERROR, "gmix: headless Vulkan init failed -- source will produce no frames");
        return s;   // non-null: an inert-but-valid source, per OBS's create() contract
    }
    s->worker = std::thread(workerMain, s);
    return s;
}

void gmixDestroy(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    s->stop = true;
    if (s->worker.joinable()) s->worker.join();
    {
        std::lock_guard<std::mutex> lk(s->texMu);
        obs_enter_graphics();
        for (auto& t : s->tex) { if (t) gs_texture_destroy(t); t = nullptr; }
        obs_leave_graphics();
    }
    delete s;
}

uint32_t gmixGetWidth(void* data)  { return static_cast<GmixSource*>(data)->width; }
uint32_t gmixGetHeight(void* data) { return static_cast<GmixSource*>(data)->height; }

void gmixVideoRender(void* data, gs_effect_t* effect) {
    auto* s = static_cast<GmixSource*>(data);
    int idx = s->frontIdx.load(std::memory_order_acquire);
    if (idx < 0) return;
    std::lock_guard<std::mutex> lk(s->texMu);
    gs_texture_t* tex = s->tex[idx];
    if (!tex) return;
    // Non-custom-draw sources (no OBS_SOURCE_CUSTOM_DRAW in output_flags) are
    // called with `effect` ALREADY active/looping by OBS's own render core --
    // wrapping this in our own gs_effect_loop() is a re-entrant activation
    // that gs_effect_loop() rejects ("An effect is already active", logged
    // once per render tick), silently skipping gs_draw_sprite every time.
    // Just set the texture and draw directly.
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, tex);
    gs_draw_sprite(tex, 0, s->width, s->height);
}

struct obs_source_info gGmixSourceInfo = {};

}  // namespace

bool obs_module_load(void) {
    gGmixSourceInfo.id            = "gmix_source";
    gGmixSourceInfo.type          = OBS_SOURCE_TYPE_INPUT;
    gGmixSourceInfo.output_flags  = OBS_SOURCE_VIDEO;
    gGmixSourceInfo.get_name      = gmixGetName;
    gGmixSourceInfo.create        = gmixCreate;
    gGmixSourceInfo.destroy       = gmixDestroy;
    gGmixSourceInfo.get_width     = gmixGetWidth;
    gGmixSourceInfo.get_height    = gmixGetHeight;
    gGmixSourceInfo.get_defaults  = gmixGetDefaults;
    gGmixSourceInfo.get_properties = gmixGetProperties;
    gGmixSourceInfo.update        = gmixUpdate;
    gGmixSourceInfo.video_render  = gmixVideoRender;
    obs_register_source(&gGmixSourceInfo);
    blog(LOG_INFO, "gmix: obs-gmix-source loaded");
    return true;
}
