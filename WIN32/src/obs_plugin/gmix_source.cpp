// ─────────────────────────────────────────────────────────────────────────────
// GMix as a native OBS source (Windows): "GMix Motion Blur". Windows/D3D11
// port of linux-x86_64/src/obs_plugin/gmix_source.cpp -- same capture-receive
// + async-compute blend pipeline, delivered to OBS zero-copy, but via
// gs_texture_open_shared() (OBS's built-in D3D11 shared-handle import --
// Windows OBS is natively D3D11, so unlike the Linux/OpenGL-OBS side there is
// no dma-buf/GL-interop hop needed here at all) instead of
// gs_texture_create_from_dmabuf().
//
// SINGLETON FIX (see linux-x86_64/etc/DEV_NOTES.md's "KNOWN BUG: GMix only
// renders in the first scene" -- filed there as a known, not-yet-fixed issue
// on the Linux side): every GmixSource instance here shares ONE process-wide
// GmixPipeline (one named-pipe listener, one worker thread, one blend
// engine), ref-counted via shared_ptr. Adding "GMix Motion Blur" to N scenes
// creates N obs_source_info instances, each of which just attaches to the
// same running pipeline and reads its current front texture -- so unlike the
// Linux version, a second/third source instance renders correctly instead of
// silently failing to bind a second listener on the same well-known path.
// ─────────────────────────────────────────────────────────────────────────────
#include <obs-module.h>
#include <graphics/graphics.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // windows.h's min/max macros otherwise shadow std::min/std::max below
#include <windows.h>

#include "../d3d11/context.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-gmix-source", "en-US")

namespace {

constexpr const char* kSettingTarget   = "target_process";
constexpr const char* kSettingGpuIndex = "gpu_index";
constexpr const char* kDefaultTarget   = "osu!";

// The capture proxy DLL (a separate process, dropped next to osu!.exe) reads
// this file once at its own D3D11-device-creation time and stays disabled
// for the process's whole lifetime if it's empty/missing (mirrors the Linux
// layer's readTargetProcessConfig()/ensureCaptureInitialized() -- see
// gl_dx_interop_capture.cpp). Must be written before the game launches, so
// this happens eagerly in create()/update(), not lazily after a producer
// connects.
bool writeTargetProcessConfig(const std::string& target) {
    if (target.empty()) return false;
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) return false;
    std::filesystem::path configDir = std::filesystem::path(localAppData) / "gmix";
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) return false;
    std::ofstream out(configDir / "target_process");
    if (!out) return false;
    out << target;
    return out.good();
}

// ── Sliding frame window (verbatim port of the Linux plugin's FrameQueue) ──
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

void receiverThreadFn(gmix::ipc::FrameReceiver& receiver, gmix::D3D11Context& ctx,
                      FrameQueue& q, gmix::FrameTexturePool& pool) {
    gmix::ipc::FrameHeader hdr{};
    while (receiver.recvFrame(hdr)) {
        bool stale = receiver.hasPendingFrame();
        // Must run even when dropping a stale frame: hdr.sharedHandleValue is
        // a real HANDLE the producer only sends ONCE per slot per connection
        // (see frame_protocol.hpp) -- skipping this on a drop would both leak
        // that handle (duplicated into our process, never closed) and
        // permanently lose the only chance to import this slot for the rest
        // of the connection (every later frame for it carries 0).
        auto tex = pool.acquire(ctx, hdr.exportSlot, hdr.sharedHandleValue, hdr.width, hdr.height, hdr.dxgiFormat);
        if (stale) continue;
        if (!tex) continue;
        auto frame = std::make_shared<gmix::ImportedFrame>();
        if (!frame->init(tex, hdr.acquireKey)) continue;
        q.push(std::move(frame), hdr.width, hdr.height, hdr.timestampNs, hdr.gpuTimestampNs);
    }
    q.disconnected = true;
}

// ── Process-wide pipeline singleton (the multi-scene fix) ──────────────────
class GmixPipeline {
public:
    static std::shared_ptr<GmixPipeline> acquire(int32_t gpuIndex, const std::string& targetProcess) {
        std::lock_guard<std::mutex> lk(s_mu);
        if (auto existing = s_instance.lock()) return existing;
        auto p = std::shared_ptr<GmixPipeline>(new GmixPipeline());
        if (!p->start(gpuIndex, targetProcess)) return nullptr;
        s_instance = p;
        return p;
    }

    ~GmixPipeline() {
        stop_ = true;
        // stop_ alone cannot wake the worker thread if it's currently
        // blocked inside a synchronous ConnectNamedPipe/ReadFile call (e.g.
        // no producer has connected yet) -- confirmed empirically: removing
        // the last "GMix Motion Blur" source while idle left the worker
        // thread permanently stuck forever, silently (OBS's UI stayed
        // responsive throughout -- the actual source teardown/destructor
        // call happens on a background thread, not the UI thread, so a
        // stuck join() here is invisible), squatting the pipe name for the
        // rest of the OBS session (every subsequent attempt to re-add the
        // source logged "failed to create named pipe").
        //
        // CloseHandle(receiver_'s pipe) from THIS (destructor) thread does
        // NOT reliably interrupt that blocking call on ANOTHER thread --
        // confirmed both by hitting the bug again after trying exactly that,
        // and by Microsoft's own documentation: closing a handle out from
        // under a thread blocked in synchronous (non-overlapped) I/O on it
        // is unsupported/undefined. CancelSynchronousIo() is the actual
        // documented mechanism -- it targets the THREAD (not the handle)
        // and forces whatever blocking synchronous call it's in to return
        // with ERROR_OPERATION_ABORTED.
        if (worker_.joinable()) {
            CancelSynchronousIo(worker_.native_handle());
            worker_.join();
        }
        receiver_.close();
        std::lock_guard<std::mutex> lk(texMu_);
        obs_enter_graphics();
        for (auto& t : tex_) { if (t) gs_texture_destroy(t); t = nullptr; }
        obs_leave_graphics();
        delete blend_;
    }

    gs_texture_t* frontTexture() {
        int idx = frontIdx_.load(std::memory_order_acquire);
        if (idx < 0) return nullptr;
        std::lock_guard<std::mutex> lk(texMu_);
        return tex_[idx];
    }
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

private:
    GmixPipeline() = default;

    bool start(int32_t gpuIndex, const std::string& targetProcess) {
        if (!writeTargetProcessConfig(targetProcess)) {
            blog(LOG_WARNING, "gmix: failed to write %%LOCALAPPDATA%%\\gmix\\target_process -- "
                               "the capture proxy DLL will not activate in the game");
        }
        if (!ctx_.init(gpuIndex)) {
            blog(LOG_ERROR, "gmix: D3D11 init failed -- source will produce no frames");
            return false;
        }
        worker_ = std::thread(&GmixPipeline::workerMain, this);
        return true;
    }

    void workerMain() {
        gmix::BlendConfig config;   // Flat mode, defaults -- only used for weightsFor()

        while (!stop_.load()) {
            // receiver_.listen() closes any previous instance internally
            // (see FrameReceiver::listen()), so reusing the member across
            // reconnect cycles is safe -- same object every iteration.
            auto pipeName = gmix::ipc::defaultFramePipeName();
            if (!receiver_.listen(pipeName)) {
                blog(LOG_ERROR, "gmix: failed to create named pipe %ls", pipeName.c_str());
                return;
            }
            blog(LOG_INFO, "gmix: waiting for producer to connect...");

            gmix::ipc::FrameHandshake hs{};
            if (!receiver_.acceptProducer(hs)) {
                receiver_.close();
                if (stop_.load()) return;
                continue;
            }
            blog(LOG_INFO, "gmix: producer connected: %ux%u pid=%u", hs.frameW, hs.frameH, hs.producerPid);

            if (!blendReady_) {
                blend_ = new gmix::BlendEngine(ctx_);
                if (!blend_->init(hs.frameW, hs.frameH)) {
                    blog(LOG_ERROR, "gmix: blend engine init failed");
                    return;
                }
                if (!blend_->sharedCapable()) {
                    blog(LOG_ERROR, "gmix: blend dst textures are not shareable on this device -- "
                                     "zero-copy delivery to OBS is unavailable");
                    return;
                }
                width_ = hs.frameW;
                height_ = hs.frameH;
                blendReady_ = true;
            }

            FrameQueue queue;
            queue.trackedW = hs.frameW;
            queue.trackedH = hs.frameH;
            gmix::FrameTexturePool texPool;
            std::thread receiverThread(receiverThreadFn, std::ref(receiver_), std::ref(ctx_),
                                       std::ref(queue), std::ref(texPool));

            int frontIdx = -1;
            uint32_t inFlightIdx = 0;
            uint64_t lastBlendArrivals = 0;
            // Cap new-blend launches to the shutter design rate (60/s) -- see
            // the Linux plugin's identical comment on this same constant for
            // the full rationale (avoids burning CPU/GPU dispatching far
            // faster than the shutter needs, stealing cycles from the game).
            const auto kMinDispatchInterval = std::chrono::nanoseconds(1'000'000'000ull / 60);
            auto lastDispatchTime = std::chrono::steady_clock::now() - kMinDispatchInterval;

            while (!queue.disconnected && !stop_.load()) {
                if (queue.pendingResize.exchange(false)) {
                    blend_->waitBlendDone();
                    if (!blend_->init(queue.resizeW, queue.resizeH)) {
                        blog(LOG_ERROR, "gmix: blend engine re-init failed");
                        stop_ = true;
                        break;
                    }
                    width_ = queue.resizeW;
                    height_ = queue.resizeH;
                    frontIdx = -1;
                    std::lock_guard<std::mutex> lk(texMu_);
                    for (auto& t : tex_) { if (t) { obs_enter_graphics(); gs_texture_destroy(t); obs_leave_graphics(); t = nullptr; } }
                    frontIdx_.store(-1);
                }

                // Import each dst buffer's shared handle ONCE (lazily, first
                // time we see it, or after the resize teardown above).
                {
                    std::lock_guard<std::mutex> lk(texMu_);
                    for (uint32_t i = 0; i < gmix::BlendEngine::kDstBuffers; ++i) {
                        if (tex_[i]) continue;
                        void* handle = blend_->dstSharedHandle(i);
                        if (!handle) continue;
                        obs_enter_graphics();
                        tex_[i] = gs_texture_open_shared(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle)));
                        obs_leave_graphics();
                        if (!tex_[i]) {
                            blog(LOG_ERROR, "gmix: gs_texture_open_shared failed for dst[%u]", i);
                        } else {
                            blog(LOG_INFO, "gmix: dst[%u] imported as gs_texture_t (%ux%u)", i, width_, height_);
                        }
                    }
                }

                if (blend_->pollBlendDone()) {
                    frontIdx = static_cast<int>(inFlightIdx);
                    frontIdx_.store(frontIdx, std::memory_order_release);
                    static bool loggedFirstFrame = false;
                    if (!loggedFirstFrame) {
                        blog(LOG_INFO, "gmix: first blend retired, front=%d -- video_render should now draw", frontIdx);
                        loggedFirstFrame = true;
                    }
                }

                auto nowTick = std::chrono::steady_clock::now();
                uint64_t arrivalsNow = queue.arrivals.load();
                if (!blend_->blendInFlight() && arrivalsNow != lastBlendArrivals &&
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

                        std::vector<gmix::ImportedFrame*> frames(n);
                        for (size_t i = 0; i < n; ++i) frames[i] = window_frames[wsz - 1 - i].frame.get();
                        std::vector<float> weights = config.weightsFor(static_cast<int>(n));

                        uint32_t back = (frontIdx == 0) ? 1u : 0u;
                        if (blend_->dispatchAsync(frames.data(), weights.data(),
                                                  static_cast<uint32_t>(n), back)) {
                            inFlightIdx = back;
                            lastBlendArrivals = arrivalsNow;
                            lastDispatchTime = nowTick;
                        }
                    }
                }

                // No internal output clock -- OBS's own render loop paces
                // actual output; this just keeps pollBlendDone()/new-arrival
                // checks responsive without busy-spinning a core. Same
                // reasoning as the Linux plugin's identical comment.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            receiver_.close();
            if (receiverThread.joinable()) receiverThread.join();
            if (stop_.load()) break;
            blog(LOG_INFO, "gmix: producer disconnected, waiting for a new one");
        }
    }

    gmix::D3D11Context ctx_;
    gmix::BlendEngine* blend_ = nullptr;
    bool blendReady_ = false;

    // A member (not a workerMain()-loop-local) specifically so ~GmixPipeline()
    // can force-unblock the worker thread on shutdown -- see its comment.
    gmix::ipc::FrameReceiver receiver_;

    std::thread worker_;
    std::atomic<bool> stop_{false};

    gs_texture_t* tex_[gmix::BlendEngine::kDstBuffers] = {};
    std::atomic<int> frontIdx_{-1};
    std::mutex texMu_;

    uint32_t width_ = 0, height_ = 0;

    static inline std::mutex s_mu;
    static inline std::weak_ptr<GmixPipeline> s_instance;
};

// ── Plugin instance state (one per OBS source, all sharing one GmixPipeline) ─
struct GmixSource {
    obs_source_t* source = nullptr;
    std::string targetProcess = kDefaultTarget;
    int32_t gpuIndex = -1;
    std::shared_ptr<GmixPipeline> pipeline;
};

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
    // The pipeline is process-wide (one named pipe, one target game), so only
    // the FIRST source's settings actually take effect if multiple "GMix
    // Motion Blur" sources exist with different target_process values --
    // same caveat the Linux plugin documents for its config file being
    // machine-global rather than per-source.
    const char* target = obs_data_get_string(settings, kSettingTarget);
    s->targetProcess = (target && *target) ? target : kDefaultTarget;
    s->gpuIndex = static_cast<int32_t>(obs_data_get_int(settings, kSettingGpuIndex));
}

void* gmixCreate(obs_data_t* settings, obs_source_t* source) {
    auto* s = new GmixSource();
    s->source = source;
    gmixUpdate(s, settings);
    s->pipeline = GmixPipeline::acquire(s->gpuIndex, s->targetProcess);
    if (!s->pipeline) {
        blog(LOG_ERROR, "gmix: pipeline start failed -- source will produce no frames");
    }
    return s;   // non-null even on failure: an inert-but-valid source
}

void gmixDestroy(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    delete s;   // dropping the shared_ptr tears the pipeline down once the last source releases it
}

uint32_t gmixGetWidth(void* data)  {
    auto* s = static_cast<GmixSource*>(data);
    return s->pipeline ? s->pipeline->width() : 0;
}
uint32_t gmixGetHeight(void* data) {
    auto* s = static_cast<GmixSource*>(data);
    return s->pipeline ? s->pipeline->height() : 0;
}

void gmixVideoRender(void* data, gs_effect_t* effect) {
    auto* s = static_cast<GmixSource*>(data);
    if (!s->pipeline) return;
    gs_texture_t* tex = s->pipeline->frontTexture();
    if (!tex) return;
    // Non-custom-draw sources are called with `effect` already active/looping
    // by OBS's own render core -- same note as the Linux plugin's identical
    // comment on why we don't wrap this in our own gs_effect_loop().
    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, tex);
    gs_draw_sprite(tex, 0, s->pipeline->width(), s->pipeline->height());
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
    blog(LOG_INFO, "gmix: obs-gmix-source (Windows) loaded");
    return true;
}
