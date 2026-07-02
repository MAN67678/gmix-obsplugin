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

#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
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

constexpr const char* kSettingTarget      = "target_process";
constexpr const char* kSettingGpuIndex    = "gpu_index";
constexpr const char* kSettingPreset      = "blur_preset";
constexpr const char* kSettingBlurDensity = "blur_density";
constexpr const char* kSettingBrightness  = "blur_brightness";
constexpr const char* kDefaultTarget      = "osu!";

constexpr const char* kPresetFlat      = "flat";
constexpr const char* kPresetLinear    = "linear";
constexpr const char* kPresetCinematic = "cinematic";
constexpr const char* kPresetHeavy     = "heavy";
constexpr const char* kPresetAdvanced  = "advanced";

gmix::BlendConfig::Mode presetSettingToMode(const char* v) {
    if (!v) return gmix::BlendConfig::Mode::Flat;
    if (std::strcmp(v, kPresetLinear) == 0)    return gmix::BlendConfig::Mode::Linear;
    if (std::strcmp(v, kPresetCinematic) == 0) return gmix::BlendConfig::Mode::Cinematic;
    if (std::strcmp(v, kPresetHeavy) == 0)     return gmix::BlendConfig::Mode::Heavy;
    if (std::strcmp(v, kPresetAdvanced) == 0)  return gmix::BlendConfig::Mode::Advanced;
    return gmix::BlendConfig::Mode::Flat;
}

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
// The ring always holds the last kCap arrived frames; every tick blends
// whichever of them fall within the trailing shutter-width TIME window (see
// the dispatch loop in workerMain()) -- not a fixed frame COUNT. An earlier
// version derived a frame count from an EMA-smoothed capture rate, which
// lagged real fps changes (game fps drops, loading screens) and made the
// blend window's real time-span drift, showing up as blur-amount flicker.
// Selecting directly by `timestampNs` against the window edge is exact
// regardless of how bursty/uneven the capture rate is.
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

    static constexpr size_t kCap = gmix::kMaxBlendFrames;

    void push(std::shared_ptr<gmix::ImportedFrame> f, uint32_t w, uint32_t h,
              uint64_t cpuTs, uint64_t /*gpuTs*/) {
        std::lock_guard<std::mutex> lk(mu);
        if (w != trackedW || h != trackedH) {
            ring.clear();
            trackedW = w; trackedH = h;
            resizeW = w; resizeH = h;
            pendingResize = true;
        }
        ring.push_back({std::move(f), cpuTs});
        while (ring.size() > kCap) ring.pop_front();
        ++arrivals;
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

// ── Shared engine (process-wide singleton) ──────────────────────────────────
// OBS creates an independent obs_source_info instance -- its own worker
// thread and FrameReceiver -- for every "GMix Motion Blur" source a user adds
// via the `+` button, but the capture layer only ever connects to ONE
// well-known socket (gmix::ipc::defaultFrameSocketPath()). Two receivers
// racing to bind() that path meant only the first source ever rendered (see
// etc/DEV_NOTES.md, "KNOWN BUG"). Fix: hoist the receiver/blend pipeline into
// one process-wide, ref-counted GmixEngine; each GmixSource just attaches to
// it as a viewer, so N sources are harmless and all render the same feed.
struct GmixEngine {
    std::atomic<int> refCount{0};

    std::thread worker;
    std::atomic<bool> stop{false};

    gmix::VulkanContext vk;
    gmix::BlendEngine* blend = nullptr;   // heap: BlendEngine has no default ctor
    bool blendReady = false;
    int32_t gpuIndex = -1;

    // gs_texture_t handles imported ONCE per dst buffer (see workerMain()).
    gs_texture_t* tex[gmix::BlendEngine::kDstBuffers] = {};
    std::atomic<int> frontIdx{-1};
    std::mutex texMu;   // guards tex[] creation vs. video_render reading it

    uint32_t width = 0, height = 0;

    // Ticked from OBS's own video_tick callback (called once per real render
    // frame, per attached GmixSource). Used to wake the worker's dispatch
    // loop in lockstep with OBS's actual render cadence instead of free-
    // running on its own timer -- two independent ~60Hz clocks racing each
    // other is what caused periodic duplicate/stale-frame judder. Also
    // tracks OBS's real frame interval so the shutter window and dispatch
    // throttle follow OBS's configured output FPS instead of an assumed 60.
    std::mutex              tickMu;
    std::condition_variable tickCv;
    std::atomic<uint64_t>   tickSeq{0};
    std::atomic<double>     obsFrameSec{1.0 / 60.0};

    // Blend preset (Flat/Linear/Cinematic/Heavy/Advanced) + Advanced's "blur
    // density". A property of the shared engine/pipeline, not any individual
    // GmixSource, since all attached sources render the same one feed --
    // matches how gpuIndex above only takes effect for the first instance.
    // Read fresh each dispatch by workerMain() (see the lock in the dispatch
    // block), written by gmixUpdate() whenever the OBS properties change.
    std::mutex        blendConfigMu;
    gmix::BlendConfig blendConfig;

    ~GmixEngine() { delete blend; }
};

std::mutex gEngineMu;
GmixEngine* gEngine = nullptr;   // guarded by gEngineMu

void workerMain(GmixEngine* s);

// First caller (refCount 0->1) spins up the shared worker thread and headless
// Vulkan context; later callers just bump the refcount and attach to the
// already-running pipeline. gpuIndex only takes effect for the first caller.
GmixEngine* acquireEngine(int32_t gpuIndex) {
    std::lock_guard<std::mutex> lk(gEngineMu);
    if (!gEngine) {
        auto* e = new GmixEngine();
        e->gpuIndex = gpuIndex;
        if (!e->vk.init(gpuIndex, /*headless=*/true)) {
            blog(LOG_ERROR, "gmix: headless Vulkan init failed -- source will produce no frames");
            delete e;
            return nullptr;
        }
        // Seed from OBS's actually configured output rate so the shutter
        // window/dispatch throttle are correct from the first frame, rather
        // than the 1/60s fallback default until gmixVideoTick() first fires.
        obs_video_info ovi{};
        if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
            e->obsFrameSec.store(static_cast<double>(ovi.fps_den) / ovi.fps_num,
                                 std::memory_order_relaxed);
        }
        e->worker = std::thread(workerMain, e);
        gEngine = e;
    }
    ++gEngine->refCount;
    return gEngine;
}

// Last caller (refCount 1->0) tears the shared pipeline down.
void releaseEngine() {
    std::lock_guard<std::mutex> lk(gEngineMu);
    if (!gEngine || --gEngine->refCount > 0) return;
    gEngine->stop = true;
    if (gEngine->worker.joinable()) gEngine->worker.join();
    {
        std::lock_guard<std::mutex> lk2(gEngine->texMu);
        obs_enter_graphics();
        for (auto& t : gEngine->tex) { if (t) gs_texture_destroy(t); t = nullptr; }
        obs_leave_graphics();
    }
    delete gEngine;
    gEngine = nullptr;
}

// ── Plugin instance state ───────────────────────────────────────────────────
// Thin per-source handle: settings (persisted per-source by OBS) plus a
// reference into the one shared GmixEngine. video_render()/get_width/
// get_height all read through `engine`.
struct GmixSource {
    obs_source_t* source = nullptr;
    std::string targetProcess = kDefaultTarget;
    int32_t gpuIndex = -1;
    GmixEngine* engine = nullptr;
};

void workerMain(GmixEngine* s) {
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
        // Round-robins the dispatch target across ALL kDstBuffers dst images
        // instead of a strict front/back ping-pong -- see the kDstBuffers==3
        // comment in blend_engine.hpp for why: pollBlendDone() only proves
        // gmix's own write finished, not that OBS has finished reading the
        // PREVIOUS front buffer, so cycling through 3 slots gives one extra
        // dispatch generation of grace before a buffer is reused.
        uint32_t nextWriteIdx = 0;
        // Cap how often a NEW blend is launched, and the width of the shutter
        // window blended per dispatch, to OBS's OWN configured output rate
        // (s->obsFrameSec, kept live by gmixVideoTick()) rather than an
        // assumed 60Hz, so both track reality if OBS runs at 30/144/etc.
        // Without the cap, a new blend would launch on EVERY new arrival --
        // up to osu!'s capture rate (600-1000/s) -- for no visual benefit,
        // since each output frame is still a one-shutter-window average.
        auto lastDispatchTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);

        // Imports each dst buffer's dma-buf ONCE (lazily, first time seen for
        // its current generation) into `arr` -- matches the project's
        // established "export/import once, reuse" pattern.
        auto importDstBuffers = [&](gs_texture_t** arr, const char* tag) {
            for (uint32_t i = 0; i < gmix::BlendEngine::kDstBuffers; ++i) {
                if (arr[i]) continue;
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
                arr[i] = gs_texture_create_from_dmabuf(
                    s->blend->width(), s->blend->height(), kDrmFormatAbgr8888, GS_RGBA,
                    1, fds, strides, offsets, modifiers);
                obs_leave_graphics();
                if (!arr[i]) {
                    blog(LOG_ERROR, "gmix: gs_texture_create_from_dmabuf failed for dst[%u]%s", i, tag);
                } else {
                    blog(LOG_INFO, "gmix: dst[%u]%s imported as gs_texture_t (fd=%d stride=%u offset=%llu %ux%u)",
                         i, tag, fd, stride, (unsigned long long)offset, s->blend->width(), s->blend->height());
                }
            }
        };

        // On a mid-stream resize (osu! changing resolution), blend->init()
        // below destroys+recreates gmix's OWN dst images/dma-buf fds at the
        // new size, but the OLD gs_texture_t imports OBS already holds stay
        // valid -- dma-buf memory is kernel/GEM-refcounted across every
        // importer, not exclusively owned by gmix's Vulkan device memory.
        // So instead of nulling s->tex/frontIdx immediately (which blanked
        // video_render() for the whole re-init+reconnect), new textures are
        // staged here and only swapped into s->tex once the FIRST post-
        // resize blend is ready -- video_render() keeps drawing the old
        // front buffer, at the old size, right up to that swap.
        gs_texture_t* pendingTex[gmix::BlendEngine::kDstBuffers] = {};
        bool awaitingSwap = false;

        while (!queue.disconnected && !s->stop.load()) {
            if (queue.pendingResize.exchange(false)) {
                s->blend->waitBlendDone();
                if (!s->blend->init(queue.resizeW, queue.resizeH)) {
                    blog(LOG_ERROR, "gmix: blend engine re-init failed");
                    s->stop = true;
                    break;
                }
                // A second resize can land here before the first one's swap
                // (confirmed happening live: two resize events landed 4ms
                // apart in one session) -- if pendingTex[] already holds
                // textures staged for that still-pending swap, destroy them
                // here rather than just nulling the pointers, or their
                // gs_texture_t handles (and VRAM) leak silently every time
                // this races. pendingTex is thread-local (only this worker
                // thread ever touches it, unlike s->tex), so no texMu needed.
                for (auto& t : pendingTex) {
                    if (t) { obs_enter_graphics(); gs_texture_destroy(t); obs_leave_graphics(); t = nullptr; }
                }
                frontIdx = -1;
                nextWriteIdx = 0;
                awaitingSwap = true;
            }

            if (awaitingSwap) {
                importDstBuffers(pendingTex, " (staged for resize swap)");
            } else {
                std::lock_guard<std::mutex> lk(s->texMu);
                importDstBuffers(s->tex, "");
            }

            if (s->blend->pollBlendDone()) {
                frontIdx = static_cast<int>(inFlightIdx);
                if (awaitingSwap) {
                    std::lock_guard<std::mutex> lk(s->texMu);
                    for (uint32_t i = 0; i < gmix::BlendEngine::kDstBuffers; ++i) {
                        if (s->tex[i]) { obs_enter_graphics(); gs_texture_destroy(s->tex[i]); obs_leave_graphics(); }
                        s->tex[i] = pendingTex[i];
                        pendingTex[i] = nullptr;
                    }
                    s->width = s->blend->width();
                    s->height = s->blend->height();
                    awaitingSwap = false;
                    blog(LOG_INFO, "gmix: resize complete, swapped in new %ux%u textures", s->width, s->height);
                }
                s->frontIdx.store(frontIdx, std::memory_order_release);
                static bool loggedFirstFrame = false;
                if (!loggedFirstFrame) {
                    blog(LOG_INFO, "gmix: first blend retired, front=%d -- video_render should now draw", frontIdx);
                    loggedFirstFrame = true;
                }
            }

            // Cheap atomic check BEFORE paying for queue.snapshot() (a mutex
            // lock + shared_ptr-refcounted copy of up to kCap=64 entries) --
            // at OBS's tick rate this check fails most ticks when nothing new
            // has arrived, so skipping the snapshot in that case avoids ~64
            // atomic refcount ops per idle tick. Also rate-limited to OBS's
            // own frame interval (see above) -- new arrivals can exist far
            // more often than that.
            auto nowTick = std::chrono::steady_clock::now();
            auto minDispatchInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(s->obsFrameSec.load(std::memory_order_relaxed)));
            uint64_t arrivalsNow = queue.arrivals.load();
            if (!s->blend->blendInFlight() && arrivalsNow != lastBlendArrivals &&
                nowTick - lastDispatchTime >= minDispatchInterval) {
                // Live snapshot of the blend preset -- gmixUpdate() can change
                // it at any time (OBS properties dialog), and it's shared
                // across every attached GmixSource, so read it fresh here
                // rather than caching a stale copy for the connection's
                // lifetime.
                gmix::BlendConfig config;
                { std::lock_guard<std::mutex> lk(s->blendConfigMu); config = s->blendConfig; }

                auto window_frames = queue.snapshot();
                if (!window_frames.empty()) {
                    const size_t wsz = window_frames.size();
                    // Select by TIME, not frame count: every ring entry whose
                    // capture timestamp falls within the trailing shutter
                    // window (OBS's real frame interval) behind the newest
                    // arrival. Exact regardless of how the capture rate
                    // fluctuates -- see the FrameQueue comment above.
                    uint64_t shutterNs = static_cast<uint64_t>(
                        s->obsFrameSec.load(std::memory_order_relaxed) * 1e9);
                    uint64_t newestTs = window_frames.back().timestampNs;
                    size_t n = 0;
                    while (n < wsz && newestTs - window_frames[wsz - 1 - n].timestampNs <= shutterNs)
                        ++n;
                    if (n < 1) n = 1;
                    n = std::min(n, static_cast<size_t>(config.maxBlendFrames()));

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

                    gmix::ResampleParams resample{};
                    resample.enabled         = config.usesResamplePath();
                    resample.subSamples      = config.blurDensity;
                    resample.shutterStrength = config.shutterStrength;
                    resample.falloff         = config.falloff;

                    uint32_t back = nextWriteIdx;
                    if (s->blend->dispatchAsync(srcViews.data(), weights.data(),
                                                static_cast<uint32_t>(n), back,
                                                waitSems.data(), waitVals.data(),
                                                static_cast<uint32_t>(waitSems.size()),
                                                resample)) {
                        inFlightIdx = back;
                        nextWriteIdx = (nextWriteIdx + 1) % gmix::BlendEngine::kDstBuffers;
                        lastBlendArrivals = arrivalsNow;
                        lastDispatchTime = nowTick;
                    }
                }
            }

            // No internal output clock here, unlike the standalone `gmix`
            // consumer (which owned final output pacing and vblank-locked to
            // it). As an OBS source, OBS's own render loop is what paces the
            // actual output -- video_render() just draws whatever frontIdx
            // currently is, whenever OBS calls it. A free-running fixed-Hz
            // timer here would be a SECOND, unsynchronized ~60Hz clock racing
            // OBS's real render tick -- two independent clocks at nominally
            // the same rate beat against each other, producing periodic
            // duplicate/stale-frame judder. Instead, block until whichever
            // happens first: gmixVideoTick() (OBS's own per-frame callback,
            // called on every attached GmixSource, notifies s->tickCv) or a
            // new capture arrival; a short timeout is just a safety net (e.g.
            // so a disconnect is still noticed promptly if neither fires).
            {
                std::unique_lock<std::mutex> lk(s->tickMu);
                uint64_t seenTick = s->tickSeq.load(std::memory_order_relaxed);
                uint64_t seenArrivals = queue.arrivals.load(std::memory_order_relaxed);
                s->tickCv.wait_for(lk, std::chrono::milliseconds(2), [&] {
                    return s->tickSeq.load(std::memory_order_relaxed) != seenTick ||
                           queue.arrivals.load(std::memory_order_relaxed) != seenArrivals ||
                           queue.disconnected.load() || s->stop.load();
                });
            }
        }

        receiver.close();
        if (receiverThread.joinable()) receiverThread.join();
        // If the producer disconnected mid-resize (awaitingSwap still true),
        // pendingTex[] is about to go out of scope on the next loop iteration
        // -- destroy any staged-but-never-swapped-in textures now, same leak
        // as the one fixed above, just triggered by disconnect instead of a
        // second resize.
        for (auto& t : pendingTex) {
            if (t) { obs_enter_graphics(); gs_texture_destroy(t); obs_leave_graphics(); t = nullptr; }
        }
        if (s->stop.load()) break;
        blog(LOG_INFO, "gmix: producer disconnected, waiting for a new one");
    }
}

// ── obs_source_info callbacks ───────────────────────────────────────────────

const char* gmixGetName(void*) { return "GMix Motion Blur"; }

void gmixGetDefaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, kSettingTarget, kDefaultTarget);
    obs_data_set_default_int(settings, kSettingGpuIndex, -1);
    obs_data_set_default_string(settings, kSettingPreset, kPresetFlat);
    obs_data_set_default_int(settings, kSettingBlurDensity, 4);
    obs_data_set_default_double(settings, kSettingBrightness, 1.0);
}

// Blur density/brightness only mean anything for the Advanced (optical-flow)
// preset -- hide them for every other preset so the dialog doesn't show dead
// controls.
bool gmixPresetModified(void*, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    const char* preset = obs_data_get_string(settings, kSettingPreset);
    const bool advanced = preset && std::strcmp(preset, kPresetAdvanced) == 0;
    obs_property_set_visible(obs_properties_get(props, kSettingBlurDensity), advanced);
    obs_property_set_visible(obs_properties_get(props, kSettingBrightness), advanced);
    return true;   // properties layout changed (visibility) -- ask OBS to redraw
}

obs_properties_t* gmixGetProperties(void*) {
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, kSettingTarget,
                            "Capture target (process name fragment)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, kSettingGpuIndex, "GPU index (-1 = auto)", -1, 15, 1);

    obs_property_t* preset = obs_properties_add_list(props, kSettingPreset, "Blur preset",
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(preset, "Flat", kPresetFlat);
    obs_property_list_add_string(preset, "Linear", kPresetLinear);
    obs_property_list_add_string(preset, "Cinematic", kPresetCinematic);
    obs_property_list_add_string(preset, "Heavy", kPresetHeavy);
    obs_property_list_add_string(preset, "Advanced (optical awareness)", kPresetAdvanced);
    obs_property_set_modified_callback2(preset, gmixPresetModified, nullptr);

    obs_properties_add_int_slider(props, kSettingBlurDensity,
                                  "Blur density (Advanced: oversampling taps/frame)", 4, 32, 1);
    obs_properties_add_float_slider(props, kSettingBrightness,
                                    "Blur brightness (Advanced: trail exposure)", 0.1, 10.0, 0.1);
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

    // Blend preset is a property of the shared engine (see GmixEngine's
    // blendConfig comment) -- s->engine is null on the very first call (this
    // runs from gmixCreate() before acquireEngine()); the engine picks up
    // whatever's in obs_data_t on the NEXT settings change in that case,
    // which is fine since GmixEngine::blendConfig already defaults to Flat.
    if (s->engine) {
        gmix::BlendConfig cfg;
        cfg.mode = presetSettingToMode(obs_data_get_string(settings, kSettingPreset));
        cfg.blurDensity = static_cast<uint32_t>(
            std::clamp<long long>(obs_data_get_int(settings, kSettingBlurDensity), 4, 32));
        cfg.shutterStrength = static_cast<float>(
            std::clamp(obs_data_get_double(settings, kSettingBrightness), 0.1, 10.0));
        std::lock_guard<std::mutex> lk(s->engine->blendConfigMu);
        s->engine->blendConfig = cfg;
    }
}

void* gmixCreate(obs_data_t* settings, obs_source_t* source) {
    auto* s = new GmixSource();
    s->source = source;
    gmixUpdate(s, settings);   // parses gpuIndex, needed below; blendConfig write is a no-op here (s->engine still null)

    s->engine = acquireEngine(s->gpuIndex);
    // engine == nullptr means headless Vulkan init failed (first source only);
    // return non-null anyway: an inert-but-valid source, per OBS's create() contract.

    // Re-apply now that s->engine exists, so a saved non-Flat preset (e.g.
    // reloading a scene collection where this source was set to Advanced)
    // takes effect immediately instead of silently defaulting to Flat until
    // the properties dialog is next touched.
    if (s->engine) gmixUpdate(s, settings);
    return s;
}

void gmixDestroy(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    if (s->engine) releaseEngine();
    delete s;
}

uint32_t gmixGetWidth(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    return s->engine ? s->engine->width : 0;
}
uint32_t gmixGetHeight(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    return s->engine ? s->engine->height : 0;
}

// Called by OBS once per real render frame, for EVERY attached GmixSource
// (i.e. up to N times per real tick if a user has the source in N scenes) --
// harmless: each call just wakes the shared worker's wait (which re-checks
// its own throttle before actually doing anything) and refreshes
// obsFrameSec. This is what lets the worker's dispatch/poll cadence track
// OBS's REAL render clock (any configured FPS) instead of an assumed
// free-running 60Hz timer racing against it -- see the wait in workerMain().
//
// obsFrameSec is read from obs_get_video_info() (OBS's CONFIGURED, stable
// output interval), NOT from this callback's own `seconds` argument (the
// actual elapsed time since the last tick). An earlier version stored
// `seconds` directly -- but that's a live, per-tick MEASUREMENT, not a
// target: any transient hitch elsewhere in OBS (another source, encoder,
// compositor stealing a frame) produces one anomalously large `seconds`
// value, which would transiently balloon both the shutter width and the
// dispatch throttle interval for that tick -- a real cause of an
// occasional small visible stall/duplicate frame, confirmed as the
// mechanism (not just theorized) after the user reported ~5%-of-the-time
// stalls even with pacing otherwise solid. The configured value is stable
// by construction and only changes if the user actually changes OBS's
// output settings, so using it removes this whole class of self-inflicted
// jitter.
void gmixVideoTick(void* data, float /*seconds*/) {
    auto* s = static_cast<GmixSource*>(data);
    if (!s->engine) return;
    GmixEngine* e = s->engine;
    obs_video_info ovi{};
    if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0) {
        e->obsFrameSec.store(static_cast<double>(ovi.fps_den) / ovi.fps_num,
                             std::memory_order_relaxed);
    }
    e->tickSeq.fetch_add(1, std::memory_order_relaxed);
    e->tickCv.notify_all();
}

void gmixVideoRender(void* data, gs_effect_t* effect) {
    auto* s = static_cast<GmixSource*>(data);
    if (!s->engine) return;
    GmixEngine* e = s->engine;
    int idx = e->frontIdx.load(std::memory_order_acquire);
    if (idx < 0) return;
    std::lock_guard<std::mutex> lk(e->texMu);
    gs_texture_t* tex = e->tex[idx];
    if (!tex) return;
    // Non-custom-draw sources (no OBS_SOURCE_CUSTOM_DRAW in output_flags) are
    // called with `effect` ALREADY active/looping by OBS's own render core --
    // wrapping this in our own gs_effect_loop() is a re-entrant activation
    // that gs_effect_loop() rejects ("An effect is already active", logged
    // once per render tick), silently skipping gs_draw_sprite every time.
    // Just set the texture and draw directly.
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, tex);
    gs_draw_sprite(tex, 0, e->width, e->height);
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
    gGmixSourceInfo.video_tick    = gmixVideoTick;
    gGmixSourceInfo.video_render  = gmixVideoRender;
    obs_register_source(&gGmixSourceInfo);
    blog(LOG_INFO, "gmix: obs-gmix-source loaded");
    return true;
}
