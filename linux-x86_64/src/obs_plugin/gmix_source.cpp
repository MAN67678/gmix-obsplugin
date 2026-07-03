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
constexpr const char* kSettingLatencyMode = "latency_mode";
constexpr const char* kDefaultTarget      = "osu!";

constexpr const char* kPresetFlat      = "flat";
constexpr const char* kPresetLinear    = "linear";
constexpr const char* kPresetCinematic = "cinematic";
constexpr const char* kPresetHeavy     = "heavy";
constexpr const char* kPresetAdvanced  = "advanced";

constexpr const char* kLatencyFast     = "fast";
constexpr const char* kLatencyMedium   = "medium";
constexpr const char* kLatencySlow     = "slow";
constexpr const char* kLatencyVerySlow = "very_slow";

// Fast=2 (tightest, least tolerance), Medium=3 (default, matches the
// pre-existing kDstBuffers=3), Slow=4, Very slow=5 -- see BlendEngine's
// dstBufferCount()/kMinDstBuffers/kMaxDstBuffers comment for what this
// actually trades off (buffer-reuse timing tolerance vs. VRAM).
uint32_t latencyModeSettingToBufferCount(const char* v) {
    if (!v) return gmix::BlendEngine::kDefaultDstBuffers;
    if (std::strcmp(v, kLatencyFast) == 0)     return 2;
    if (std::strcmp(v, kLatencySlow) == 0)     return 4;
    if (std::strcmp(v, kLatencyVerySlow) == 0) return 5;
    return gmix::BlendEngine::kDefaultDstBuffers;   // "medium" or unrecognized
}

const char* bufferCountToLatencyModeSetting(uint32_t count) {
    switch (count) {
    case 2:  return kLatencyFast;
    case 4:  return kLatencySlow;
    case 5:  return kLatencyVerySlow;
    default: return kLatencyMedium;
    }
}

gmix::BlendConfig::Mode presetSettingToMode(const char* v) {
    if (!v) return gmix::BlendConfig::Mode::Flat;
    if (std::strcmp(v, kPresetLinear) == 0)    return gmix::BlendConfig::Mode::Linear;
    if (std::strcmp(v, kPresetCinematic) == 0) return gmix::BlendConfig::Mode::Cinematic;
    if (std::strcmp(v, kPresetHeavy) == 0)     return gmix::BlendConfig::Mode::Heavy;
    if (std::strcmp(v, kPresetAdvanced) == 0)  return gmix::BlendConfig::Mode::Advanced;
    return gmix::BlendConfig::Mode::Flat;
}

const char* presetModeToString(gmix::BlendConfig::Mode mode) {
    switch (mode) {
    case gmix::BlendConfig::Mode::Flat:      return kPresetFlat;
    case gmix::BlendConfig::Mode::Linear:    return kPresetLinear;
    case gmix::BlendConfig::Mode::Cinematic: return kPresetCinematic;
    case gmix::BlendConfig::Mode::Heavy:     return kPresetHeavy;
    case gmix::BlendConfig::Mode::Advanced:  return kPresetAdvanced;
    case gmix::BlendConfig::Mode::Raw:       return "raw";
    }
    return "?";
}

// ── Diagnostics: EMA-smoothed rate tracker for the periodic status log ─────
// Mutex-guarded rather than lock-free: this is a low-frequency diagnostic
// path (ticked at most a few hundred times/sec, read once every few
// seconds), and some tick() call sites (gmixVideoRender) may run on OBS's
// render thread rather than the worker thread, so correctness is worth more
// here than shaving a lock.
class RateTracker {
public:
    void tick() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        if (haveLast_) {
            double intervalMs = std::chrono::duration<double, std::milli>(now - last_).count();
            emaMs_ = (emaMs_ <= 0.0) ? intervalMs : (emaMs_ * 0.9 + intervalMs * 0.1);
        }
        last_ = now;
        haveLast_ = true;
    }
    double fps() const {
        std::lock_guard<std::mutex> lk(mu_);
        return emaMs_ > 0.0 ? 1000.0 / emaMs_ : 0.0;
    }
private:
    mutable std::mutex mu_;
    std::chrono::steady_clock::time_point last_;
    bool   haveLast_ = false;
    double emaMs_    = 0.0;
};

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

// ── Persisted engine-wide settings (GPU index, Latency mode) ────────────────
// GPU index and Latency mode (dstBufferCount) are fundamentally PROCESS-WIDE
// -- one shared GmixEngine for however many "GMix Motion Blur" sources exist
// -- but OBS only gives plugins PER-SOURCE settings storage (obs_data_t).
// With N sources, whichever one OBS happens to construct FIRST decides the
// value for ALL of them, which depends on OBS's internal source-load order,
// not anything the user controls. Worse: CONFIRMED live, a brand-new
// "+"-added source always starts from gmixGetDefaults() (OBS calls create()
// before the user can touch Properties), so "remove every source and
// re-add" -- the only way to force a fresh engine -- can never actually
// produce a non-default value either; there's no way to escape the default
// under pure per-source storage. This file breaks that: it's the value the
// NEXT freshly-created engine uses, written ONLY when the user actually
// interacts with the Latency mode dropdown in Properties (see
// gmixLatencyModified()'s modified-callback -- NOT from gmixUpdate()'s
// routine settings-application path, which also runs on ordinary source
// creation/scene-collection load with whatever default/stale values that
// particular source happens to have; writing from THAT path would silently
// clobber a deliberately-set value the moment any other default-configured
// source gets created).
std::filesystem::path engineSettingsConfigPath() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".config" / "gmix" / "engine_settings";
}

bool writeEngineSettingsConfig(int32_t gpuIndex, uint32_t dstBufferCount) {
    auto path = engineSettingsConfigPath();
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path);
    if (!out) return false;
    out << gpuIndex << ' ' << dstBufferCount;
    return out.good();
}

// Returns false (leaving gpuIndex/dstBufferCount untouched) if the file
// doesn't exist yet -- the very first engine ever created on this machine,
// before any Latency mode change has ever been persisted -- so the caller's
// own (first-source's) values are used, matching the pre-existing behavior.
bool readEngineSettingsConfig(int32_t& gpuIndex, uint32_t& dstBufferCount) {
    auto path = engineSettingsConfigPath();
    std::ifstream in(path);
    if (!in) return false;
    int32_t gi; uint32_t db;
    if (!(in >> gi >> db)) return false;
    gpuIndex = gi;
    dstBufferCount = db;
    return true;
}

// ── Persisted blend config (preset/density/brightness) ─────────────────────
// A SEPARATE file from engine_settings, not because the mechanism differs --
// it's the exact same pattern -- but because blend config changes far more
// often (every preset/slider tweak) than gpuIndex/Latency mode, and applying
// live vs. needing a full engine rebuild are genuinely different concerns.
//
// CONFIRMED LIVE this was still needed even AFTER blendConfig started
// applying live via genuine Properties interaction (see
// applyBlendConfigFromSettings()): changing Latency mode still requires
// removing every source to rebuild the engine (inherent, unchanged), and
// that rebuild constructs a BRAND NEW GmixEngine whose blendConfig starts
// at its hard default (Flat) -- there was nowhere for the just-set Advanced
// preset to have persisted TO, since it only ever lived in the now-deleted
// engine's memory. Same fix as engine_settings: write on every genuine
// change, read when constructing a fresh engine.
std::filesystem::path blendConfigPath() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".config" / "gmix" / "blend_config";
}

void writeBlendConfigFile(const gmix::BlendConfig& cfg) {
    auto path = blendConfigPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream out(path);
    if (!out) return;
    out << presetModeToString(cfg.mode) << ' ' << cfg.blurDensity << ' ' << cfg.shutterStrength;
}

// Returns false (leaving cfg untouched) if the file doesn't exist yet --
// e.g. the very first engine ever created, before any preset change has
// ever been persisted -- so the caller's own default (Flat) is used.
bool readBlendConfigFile(gmix::BlendConfig& cfg) {
    auto path = blendConfigPath();
    std::ifstream in(path);
    if (!in) return false;
    std::string modeStr;
    uint32_t density = 0;
    float brightness = 0.0f;
    if (!(in >> modeStr >> density >> brightness)) return false;
    cfg.mode = presetSettingToMode(modeStr.c_str());
    cfg.blurDensity = density;
    cfg.shutterStrength = brightness;
    return true;
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
                      gmix::SemaphorePool& semPool,
                      RateTracker& producerRate, RateTracker& consumerRate) {
    gmix::ipc::RecvFrame rf{};
    while (receiver.recvFrame(rf)) {
        // Ticked here, before the drop-check below: this is every frame the
        // capture layer actually sent, i.e. the producer/game's real export
        // rate, regardless of whether the consumer keeps up with it.
        producerRate.tick();
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
        consumerRate.tick();   // survived the drop -- actually available to the blend
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
    // "Latency mode" OBS setting (Fast/Medium/Slow/Very slow -> 2/3/4/5 dst
    // buffers, see BlendEngine's dstBufferCount() comment for the actual
    // tradeoff). Engine-wide, fixed for its lifetime once the first source
    // sets it -- same precedent as gpuIndex above. Clamped in acquireEngine().
    uint32_t dstBufferCount = gmix::BlendEngine::kDefaultDstBuffers;

    // gs_texture_t handles imported ONCE per dst buffer (see workerMain()).
    // Sized to dstBufferCount in acquireEngine(), before the worker thread
    // (or any GmixSource) can touch it.
    std::vector<gs_texture_t*> tex;
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

    // ── Diagnostics: FPS at each pipeline stage + per-stage latency ─────────
    // The pipeline has three conceptually distinct latency segments:
    //   osu! present -> [ shutter window, ~16.6ms BY DESIGN, not measured --
    //                     the blend intentionally uses frames up to one
    //                     output-frame-interval behind the newest present;
    //                     see the FrameQueue/shutterNs comments above ]
    //                 -> [ blendLatencyMs: dispatch -> retire. Pure GPU/CPU
    //                     processing cost. Should be SMALL and CONSISTENT --
    //                     this is the one to watch for a genuine pipeline
    //                     problem, e.g. GPU contention with the game. ]
    //                 -> [ drawLatencyMs: retire -> actually drawn by OBS.
    //                     Bounded by OBS's own render cadence -- since
    //                     dispatch is tick-gated (see the dstBufferCount/
    //                     tickSeq comments) but a blend can still retire at ANY
    //                     point relative to OBS's next video_render call,
    //                     this legitimately averages roughly half an OBS
    //                     frame interval and can be jittery near a full
    //                     interval -- that's expected, not a bug. ]
    // An earlier version measured "newest captured frame's timestamp to
    // retire" as a single "latencyMs" -- that conflated the (inherent,
    // fine) capture-to-dispatch PHASE offset between osu!'s and OBS's two
    // independently-paced ~60Hz clocks with actual processing cost, which
    // is why it read as low-but-jittery (2-19ms) instead of showing a
    // stable blend cost -- see etc/DEV_NOTES.md. Replaced with the two
    // segments above, which is what's actually actionable.
    //
    // producerRate : ticked in receiverThreadFn on every frame the CAPTURE
    //                LAYER actually sent over the socket (before any drop) --
    //                the producer/game's real export rate.
    // consumerRate : ticked in receiverThreadFn only for frames that survive
    //                the backlog-drop and get pushed into the ring -- the
    //                rate actually available to the blend.
    // blendRate    : ticked in workerMain each time pollBlendDone() retires a
    //                blend -- the achieved output-update rate.
    // drawnRate    : ticked in gmixVideoRender only when the drawn frontIdx
    //                actually CHANGED since the last draw (across however
    //                many scenes the source is in) -- how often OBS is
    //                actually shown a new frame, not just how often it calls
    //                video_render (which would just echo OBS's own fps).
    // frontReadyTimeNs : steady_clock ns at the moment a blend retired --
    //                written by workerMain, read by gmixVideoRender (a
    //                DIFFERENT thread, OBS's render thread) to compute
    //                drawLatencyMs, hence atomic.
    RateTracker producerRate;
    RateTracker consumerRate;
    RateTracker blendRate;
    RateTracker drawnRate;
    std::atomic<int>      lastDrawnFrontIdx{-1};
    std::atomic<uint64_t> frontReadyTimeNs{0};
    std::atomic<double>   blendLatencyMs{0.0};
    std::atomic<double>   drawLatencyMs{0.0};

    ~GmixEngine() { delete blend; }
};

std::mutex gEngineMu;
GmixEngine* gEngine = nullptr;   // guarded by gEngineMu

void workerMain(GmixEngine* s);

// First caller (refCount 0->1) spins up the shared worker thread and headless
// Vulkan context; later callers just bump the refcount and attach to the
// already-running pipeline. gpuIndex/dstBufferCount only take effect for the
// first caller.
GmixEngine* acquireEngine(int32_t gpuIndex, uint32_t dstBufferCount) {
    std::lock_guard<std::mutex> lk(gEngineMu);
    if (!gEngine) {
        // Prefer the persisted config over this specific caller's own values
        // -- see the engineSettingsConfigPath() comment for why: per-source
        // storage can't reliably express a process-wide setting. If the file
        // doesn't exist yet (nothing has ever been persisted), fall through
        // to this caller's own values, matching the original behavior.
        bool usedPersisted = readEngineSettingsConfig(gpuIndex, dstBufferCount);
        auto* e = new GmixEngine();
        e->gpuIndex = gpuIndex;
        e->dstBufferCount = std::clamp(dstBufferCount,
                                       gmix::BlendEngine::kMinDstBuffers,
                                       gmix::BlendEngine::kMaxDstBuffers);
        e->tex.assign(e->dstBufferCount, nullptr);
        if (usedPersisted) {
            blog(LOG_INFO, "gmix: using persisted engine settings from "
                           "~/.config/gmix/engine_settings (gpuIndex=%d dstBufferCount=%u)",
                 gpuIndex, e->dstBufferCount);
        }
        // Same idea for blend config (preset/density/brightness) -- see
        // blendConfigPath()'s comment. e->blendConfig already default-
        // constructs to Flat if the file doesn't exist yet.
        if (readBlendConfigFile(e->blendConfig)) {
            blog(LOG_INFO, "gmix: using persisted blend config from "
                           "~/.config/gmix/blend_config (preset=%s density=%u brightness=%.1f)",
                 presetModeToString(e->blendConfig.mode), e->blendConfig.blurDensity,
                 e->blendConfig.shutterStrength);
        }
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
        blog(LOG_INFO,
             "gmix: engine created with gpuIndex=%d dstBufferCount=%u (Latency mode) -- "
             "these are now FIXED for every attached source until ALL \"GMix Motion Blur\" "
             "sources are removed (changing them on an existing source, or adding a source "
             "with a different value, has no effect on the already-running engine)",
             gpuIndex, e->dstBufferCount);
    } else if (gEngine->gpuIndex != gpuIndex || gEngine->dstBufferCount != dstBufferCount) {
        // This source's own saved settings differ from what's already locked
        // in -- silently ignoring that (matching gpuIndex's existing,
        // documented precedent) previously gave no indication anything was
        // even requested, let alone ignored. Confirmed happening live: user
        // set Latency mode to Slow (4 buffers) on one source but the engine
        // had already been created at Medium (3) by whichever source OBS
        // constructed first when the scene collection loaded -- no error, no
        // buffers=4 in the log, just silent. This warning is that visibility.
        blog(LOG_WARNING,
             "gmix: this source wants gpuIndex=%d dstBufferCount=%u, but the already-running "
             "engine is fixed at gpuIndex=%d dstBufferCount=%u (set by whichever source was "
             "created first) -- IGNORED. Remove every \"GMix Motion Blur\" source and re-add "
             "to apply the new value.",
             gpuIndex, dstBufferCount, gEngine->gpuIndex, gEngine->dstBufferCount);
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
    // Same lock-order-inversion fix as the resize/import paths in workerMain():
    // never hold texMu across obs_enter_graphics()/gs_texture_destroy().
    std::vector<gs_texture_t*> oldTex(gEngine->tex.size(), nullptr);
    {
        std::lock_guard<std::mutex> lk2(gEngine->texMu);
        for (size_t i = 0; i < gEngine->tex.size(); ++i) {
            oldTex[i] = gEngine->tex[i];
            gEngine->tex[i] = nullptr;
        }
    }
    obs_enter_graphics();
    for (auto t : oldTex) { if (t) gs_texture_destroy(t); }
    obs_leave_graphics();
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
    uint32_t dstBufferCount = gmix::BlendEngine::kDefaultDstBuffers;
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
            if (!s->blend->init(hs.frameW, hs.frameH, s->dstBufferCount)) {
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
                                   std::ref(queue), std::ref(framePool), std::ref(semPool),
                                   std::ref(s->producerRate), std::ref(s->consumerRate));

        int frontIdx = -1;
        uint32_t inFlightIdx = 0;
        // steady_clock ns at the moment the CURRENTLY in-flight dispatch was
        // submitted -- read back when it retires to compute s->blendLatencyMs
        // (pure processing cost, dispatch -> retire; see the pollBlendDone()
        // block below).
        uint64_t inFlightDispatchTimeNs = 0;
        uint64_t lastBlendArrivals = 0;
        // Round-robins the dispatch target across ALL s->dstBufferCount dst
        // images instead of a strict front/back ping-pong -- see
        // dstBufferCount()'s comment in blend_engine.hpp for why:
        // pollBlendDone() only proves gmix's own write finished, not that
        // OBS has finished reading the PREVIOUS front buffer, so cycling
        // through more slots gives that many extra dispatch generations of
        // grace before a buffer is reused ("Latency mode" OBS setting).
        uint32_t nextWriteIdx = 0;
        // Cap how often a NEW blend is launched to "at most once per real OBS
        // video_tick", tracked via s->tickSeq (bumped by gmixVideoTick() on
        // every real render frame) -- NOT via an elapsed-wall-time interval.
        // An earlier version gated on `now - lastDispatchTime >=
        // minDispatchInterval` (a fixed nominal duration derived from OBS's
        // CONFIGURED fps). That fixed a systematic under-target drift (see
        // etc/DEV_NOTES.md), but then measurably showed up as `drawn` fps
        // trailing `blend` fps during high-producer-fps periods: OBS's own
        // real per-frame cadence has natural jitter even at a clean configured
        // 60/1, so gmix's independently-clocked, perfectly-steady dispatch
        // grid would occasionally race ahead of it, completing a SECOND blend
        // before OBS ever called video_render to observe the first one --
        // the classic "two nominally-equal but independently-paced clocks
        // beat against each other" failure mode, just reintroduced one layer
        // up from where it was originally fixed (video_tick-driven wakeup).
        // Gating on tickSeq instead ties dispatch 1:1 to OBS's ACTUAL
        // real callback cadence, whatever its jitter, eliminating the second
        // clock outright. The shutter window WIDTH still uses obsFrameSec
        // (a stable target duration, not a pacing throttle) -- unaffected.
        //
        // KNOWN, ACCEPTED minor inefficiency: video_tick fires MORE than once
        // per real frame while an OBS recording is active (confirmed live,
        // reproduced across 4 separate recording sessions in one log --
        // `blend` measured 67-97fps instead of 60 for the whole duration of
        // each recording, snapping back to 60 within ~2s of it stopping;
        // likely an extra render/tick pass for the recording output pipeline
        // alongside the live preview). This wastes some GPU time on blends
        // nobody ever sees -- `drawn` stays pinned at exactly 60fps the whole
        // time regardless, so it's NOT a visible artifact or a stall, just
        // avoidable work. Deliberately left as-is: the "correct" fix needs
        // either certainty about libobs's exact multi-output tick semantics
        // (not verified) or a wall-clock safety cap, and a wall-clock cap
        // done wrong is EXACTLY what caused the drawn-trailing-blend judder
        // bug fixed above -- not worth the regression risk for a
        // non-visible inefficiency. Revisit only if it's shown to cause a
        // real problem (e.g. GPU contention during recording).
        uint64_t lastDispatchedTickSeq = s->tickSeq.load(std::memory_order_relaxed);
        // Periodic FPS/latency summary -- see the RateTracker fields on
        // GmixEngine for what each stage measures.
        auto lastStatusLogTime = std::chrono::steady_clock::now();
        constexpr auto kStatusLogInterval = std::chrono::seconds(2);
        bool loggedLatencyBudgetWarning = false;   // see the Advanced-preset latency-budget check below

        // Imports each dst buffer's dma-buf ONCE (lazily, first time seen for
        // its current generation) into `arr` -- matches the project's
        // established "export/import once, reuse" pattern.
        auto importDstBuffers = [&](gs_texture_t** arr, const char* tag) {
            for (uint32_t i = 0; i < s->dstBufferCount; ++i) {
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
        std::vector<gs_texture_t*> pendingTex(s->dstBufferCount, nullptr);
        bool awaitingSwap = false;

        while (!queue.disconnected && !s->stop.load()) {
            if (queue.pendingResize.exchange(false)) {
                s->blend->waitBlendDone();
                if (!s->blend->init(queue.resizeW, queue.resizeH, s->dstBufferCount)) {
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
                importDstBuffers(pendingTex.data(), " (staged for resize swap)");
            } else {
                // NEVER hold texMu across obs_enter_graphics()/gs_texture_*
                // (which importDstBuffers calls internally) -- gmixVideoRender()
                // runs on OBS's own render thread, which already holds OBS's
                // graphics context by the time it's called and then tries to
                // acquire texMu. Holding texMu here first and THEN requesting
                // the graphics context is the exact reverse order -- a classic
                // lock-order inversion that can stall OBS's render thread
                // (symptom: OBS's compositor/window-manager briefly flags it
                // "not responding" under contention, without a permanent hang).
                // Snapshot s->tex under a brief lock, import into the snapshot
                // WITHOUT holding texMu, then merge the newly-created pointers
                // back under another brief lock -- texMu is never held while
                // calling into OBS's graphics API.
                std::vector<gs_texture_t*> texSnapshot(s->tex.size());
                {
                    std::lock_guard<std::mutex> lk(s->texMu);
                    for (size_t i = 0; i < s->tex.size(); ++i)
                        texSnapshot[i] = s->tex[i];
                }
                importDstBuffers(texSnapshot.data(), "");
                {
                    std::lock_guard<std::mutex> lk(s->texMu);
                    for (size_t i = 0; i < s->tex.size(); ++i) {
                        if (!s->tex[i] && texSnapshot[i]) s->tex[i] = texSnapshot[i];
                    }
                }
            }

            if (s->blend->pollBlendDone()) {
                frontIdx = static_cast<int>(inFlightIdx);
                s->blendRate.tick();
                uint64_t nowNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                if (inFlightDispatchTimeNs != 0 && nowNs > inFlightDispatchTimeNs) {
                    s->blendLatencyMs.store((nowNs - inFlightDispatchTimeNs) / 1e6,
                                            std::memory_order_relaxed);
                }
                // Timestamp this retirement for gmixVideoRender() (a DIFFERENT
                // thread) to compute drawLatencyMs against once it actually
                // observes this new frontIdx.
                s->frontReadyTimeNs.store(nowNs, std::memory_order_relaxed);
                if (awaitingSwap) {
                    // Same lock-order-inversion fix as the import branch above:
                    // swap the pointers under a brief texMu lock (no graphics
                    // calls), THEN destroy the displaced old textures with
                    // texMu already released.
                    std::vector<gs_texture_t*> oldTex(s->tex.size(), nullptr);
                    {
                        std::lock_guard<std::mutex> lk(s->texMu);
                        for (size_t i = 0; i < s->tex.size(); ++i) {
                            oldTex[i] = s->tex[i];
                            s->tex[i] = pendingTex[i];
                            pendingTex[i] = nullptr;
                        }
                        s->width = s->blend->width();
                        s->height = s->blend->height();
                    }
                    obs_enter_graphics();
                    for (auto t : oldTex) { if (t) gs_texture_destroy(t); }
                    obs_leave_graphics();
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
            uint64_t curTickSeq = s->tickSeq.load(std::memory_order_relaxed);
            uint64_t arrivalsNow = queue.arrivals.load();
            if (!s->blend->blendInFlight() && arrivalsNow != lastBlendArrivals &&
                curTickSeq != lastDispatchedTickSeq) {
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
                    // resample.falloff intentionally NOT set (left at
                    // ResampleParams' default): resample_blur.comp no longer
                    // reads a per-frame recency falloff at all -- every real
                    // frame in the window is weighted flat, like the Flat
                    // preset, to fix a ghosting regression (see DEV_NOTES
                    // 2026-07-03 "ninth" entry). The field stays in
                    // BlendConfig/ResampleParams/the push-constant struct for
                    // now (ABI churn isn't worth it for a dead field), just
                    // unused on this path.

                    uint32_t back = nextWriteIdx;
                    if (s->blend->dispatchAsync(srcViews.data(), weights.data(),
                                                static_cast<uint32_t>(n), back,
                                                waitSems.data(), waitVals.data(),
                                                static_cast<uint32_t>(waitSems.size()),
                                                resample)) {
                        inFlightIdx = back;
                        inFlightDispatchTimeNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                nowTick.time_since_epoch()).count());
                        nextWriteIdx = (nextWriteIdx + 1) % s->dstBufferCount;
                        lastBlendArrivals = arrivalsNow;
                        lastDispatchedTickSeq = curTickSeq;
                    }
                }
            }

            {
                // Full pipeline, in order: [capture] -- osu! present -> the
                // shutter window, ~1 output frame, BY DESIGN, not measured
                // (this IS the intentional camera-shutter behavior, not
                // latency to budget against) -- [blend] -- dispatch -> retire,
                // pure GPU/CPU cost, this IS what the budget below bounds --
                // [draw] -- retire -> OBS actually shows it, bounded by OBS's
                // own render cadence but STILL counted against the same
                // budget (a slow draw delays the pixels just as much as a
                // slow blend does) -- [obs].
                //
                // The budget itself is derived from Latency mode
                // (s->dstBufferCount), not hardcoded, and applies to EVERY
                // preset, not just Advanced: `dstBufferCount - 1` frame
                // intervals -- Fast(2 buffers)=1, Medium(3)=2, Slow(4)=3,
                // Very slow(5)=4. This is the SAME number the buffer count
                // already encodes as "how many extra dispatch generations of
                // grace before a buffer gets reused" (see
                // BlendEngine::dstBufferCount()'s comment) -- Latency mode is
                // fundamentally "how much blend/draw timing variance are you
                // willing to tolerate," so the diagnostic budget and the
                // actual buffering mechanism should use the same number
                // instead of an unrelated hardcoded one. Surfacing the active
                // preset in the status log makes an elevated blend_latency
                // self-explanatory instead of requiring the kind of
                // after-the-fact correlation-hunting this session needed to
                // confirm the Cinematic-preset cost earlier. Only warn if it
                // blows even this budget -- that's the point where a slow
                // blend/draw starts meaningfully lagging the live capture,
                // not just costing more GPU time within an already-accepted
                // allowance.
                gmix::BlendConfig::Mode curMode;
                { std::lock_guard<std::mutex> lk(s->blendConfigMu); curMode = s->blendConfig.mode; }
                double frameMs = s->obsFrameSec.load(std::memory_order_relaxed) * 1000.0;
                double blendMs = s->blendLatencyMs.load(std::memory_order_relaxed);
                double drawMs  = s->drawLatencyMs.load(std::memory_order_relaxed);
                double budgetFrames = static_cast<double>(s->dstBufferCount) - 1.0;

                bool overBudget = (blendMs + drawMs) > frameMs * budgetFrames;
                if (overBudget && !loggedLatencyBudgetWarning) {
                    blog(LOG_WARNING,
                         "gmix: preset=%s end-to-end (blend+draw) latency %.1fms exceeds the "
                         "~%.1fms (%.0f-frame, from Latency mode) budget -- consider a lower "
                         "blur density (Advanced) or a slower Latency mode for more tolerance",
                         presetModeToString(curMode), blendMs + drawMs, frameMs * budgetFrames,
                         budgetFrames);
                    loggedLatencyBudgetWarning = true;
                } else if (!overBudget) {
                    loggedLatencyBudgetWarning = false;
                }

                if (nowTick - lastStatusLogTime >= kStatusLogInterval) {
                    blog(LOG_INFO,
                         "gmix: status: preset=%s producer=%.1ffps consumer=%.1ffps blend=%.1ffps "
                         "drawn=%.1ffps blend_latency=%.1fms draw_latency=%.1fms",
                         presetModeToString(curMode), s->producerRate.fps(), s->consumerRate.fps(),
                         s->blendRate.fps(), s->drawnRate.fps(), blendMs, drawMs);
                    lastStatusLogTime = nowTick;
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
    // 1.3, not 1.0 -- 1.0 (pure energy-conserving average, no boost) tested
    // visibly under-bright for the Advanced preset's motion trail. 1.2 was
    // an earlier live-confirmed baseline, since raised to 1.3: 1.5 gave the
    // best in-game blur but was too bright on menu screens (mostly static,
    // bright UI -- the boost is motion-gated but menus still have plenty of
    // moving elements), and there's no single value that's right for both,
    // so 1.3 splits the difference as the default and the slider is there
    // for the user to tune per-content. Only affects Advanced
    // (Flat/Linear/Cinematic/Heavy ignore this field).
    obs_data_set_default_double(settings, kSettingBrightness, 1.3);
    obs_data_set_default_string(settings, kSettingLatencyMode, kLatencyMedium);
}

// CONFIRMED LIVE: OBS fires a property's modified-callback once automatically
// as part of building/validating a fresh obs_properties_t (e.g. right after a
// source is created), NOT only on genuine user interaction with the control.
// That auto-fire was clobbering the persisted Latency mode config the instant
// a freshly re-added source got its properties initialized, silently undoing
// whatever had just been read from the file at engine-creation time (the
// user had to re-pick the dropdown after every single remove/re-add cycle to
// compensate) -- and separately, was ALSO the reason a brand-new source
// unconditionally reset the shared blend preset back to Flat the moment it
// was created (see applyBlendConfigFromSettings()'s comment). Each of these
// flags is reset by gmixGetProperties() (called once per properties_t build)
// and consumed by the FIRST modified-callback invocation that follows for
// that specific control -- all OBS properties-dialog callbacks run on the
// single UI thread, so a plain flag reliably distinguishes that one
// synthetic fire from every later GENUINE interaction within the same
// dialog session (which DOES need to write -- that's the whole feature).
// Separate flags per control since each control's modified-callback gets its
// own auto-fire, not one shared fire for the whole dialog.
bool gGmixNextLatencyModifiedIsAutoFire    = false;
bool gGmixNextPresetModifiedIsAutoFire     = false;
bool gGmixNextDensityModifiedIsAutoFire    = false;
bool gGmixNextBrightnessModifiedIsAutoFire = false;

// Blend preset/density/brightness are a property of the SHARED engine (see
// GmixEngine::blendConfig's comment), not of any one source -- so this reads
// straight from the global gEngine rather than needing a GmixSource*, and is
// called only from genuine modified-callback interactions (see the
// auto-fire comment above): CONFIRMED LIVE that writing this unconditionally
// from gmixUpdate()'s routine path let a brand-new source's own default
// settings silently reset the shared preset for every scene the instant it
// was created, even with another source deliberately set to Advanced.
void applyBlendConfigFromSettings(obs_data_t* settings) {
    std::lock_guard<std::mutex> lk(gEngineMu);
    if (!gEngine) return;
    gmix::BlendConfig cfg;
    cfg.mode = presetSettingToMode(obs_data_get_string(settings, kSettingPreset));
    cfg.blurDensity = static_cast<uint32_t>(
        std::clamp<long long>(obs_data_get_int(settings, kSettingBlurDensity), 4, 32));
    cfg.shutterStrength = static_cast<float>(
        std::clamp(obs_data_get_double(settings, kSettingBrightness), 0.1, 10.0));
    {
        std::lock_guard<std::mutex> lk2(gEngine->blendConfigMu);
        gEngine->blendConfig = cfg;
    }
    // Persist so the NEXT freshly-created engine (e.g. after a Latency mode
    // change forces every source to be removed and re-added) starts from
    // this value instead of the hard Flat default -- see blendConfigPath()'s
    // comment for why this is needed even though the live-apply above
    // already works within the CURRENT engine's lifetime.
    writeBlendConfigFile(cfg);
}

// Blur density/brightness only mean anything for the Advanced (optical-flow)
// preset -- hide them for every other preset so the dialog doesn't show dead
// controls. Visibility toggling happens unconditionally (needed even on the
// auto-fire, to set up correct INITIAL visibility) -- only the blendConfig
// write is gated.
bool gmixPresetModified(void*, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    const char* preset = obs_data_get_string(settings, kSettingPreset);
    const bool advanced = preset && std::strcmp(preset, kPresetAdvanced) == 0;
    obs_property_set_visible(obs_properties_get(props, kSettingBlurDensity), advanced);
    obs_property_set_visible(obs_properties_get(props, kSettingBrightness), advanced);
    if (gGmixNextPresetModifiedIsAutoFire) {
        gGmixNextPresetModifiedIsAutoFire = false;
    } else {
        applyBlendConfigFromSettings(settings);
    }
    return true;   // properties layout changed (visibility) -- ask OBS to redraw
}

bool gmixDensityModified(void*, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    if (gGmixNextDensityModifiedIsAutoFire) {
        gGmixNextDensityModifiedIsAutoFire = false;
        return false;
    }
    applyBlendConfigFromSettings(settings);
    return false;
}

bool gmixBrightnessModified(void*, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    if (gGmixNextBrightnessModifiedIsAutoFire) {
        gGmixNextBrightnessModifiedIsAutoFire = false;
        return false;
    }
    applyBlendConfigFromSettings(settings);
    return false;
}

bool gmixLatencyModified(void*, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    if (gGmixNextLatencyModifiedIsAutoFire) {
        gGmixNextLatencyModifiedIsAutoFire = false;
        return false;
    }
    int32_t gpuIndex = static_cast<int32_t>(obs_data_get_int(settings, kSettingGpuIndex));
    uint32_t dstBufferCount = latencyModeSettingToBufferCount(
        obs_data_get_string(settings, kSettingLatencyMode));
    writeEngineSettingsConfig(gpuIndex, dstBufferCount);
    blog(LOG_INFO,
         "gmix: Latency mode changed -- saved gpuIndex=%d dstBufferCount=%u to "
         "~/.config/gmix/engine_settings for the NEXT engine creation (the current "
         "engine, if any, is unaffected until every \"GMix Motion Blur\" source is "
         "removed and re-added)",
         gpuIndex, dstBufferCount);
    return false;   // no properties-layout change needed
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

    obs_property_t* density = obs_properties_add_int_slider(props, kSettingBlurDensity,
                                  "Blur density (Advanced: oversampling taps/frame)", 4, 32, 1);
    obs_property_set_modified_callback2(density, gmixDensityModified, nullptr);
    obs_property_t* brightness = obs_properties_add_float_slider(props, kSettingBrightness,
                                    "Blur brightness (Advanced: trail exposure)", 0.1, 10.0, 0.1);
    obs_property_set_modified_callback2(brightness, gmixBrightnessModified, nullptr);

    // Not preset-specific -- applies engine-wide (like GPU index). Changing
    // it here saves to ~/.config/gmix/engine_settings (gmixLatencyModified())
    // for the NEXT freshly-created engine; the CURRENT engine (if any) stays
    // fixed until every "GMix Motion Blur" source is removed and re-added.
    // More buffers = more tolerance for blend-timing variance (GPU
    // contention, thermal/scheduling drift over a long session) at the cost
    // of VRAM and a larger worst-case front-buffer staleness; see
    // BlendEngine's dstBufferCount() comment for the mechanism.
    obs_property_t* latency = obs_properties_add_list(
        props, kSettingLatencyMode, "Latency mode (buffering, applies to next new engine)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(latency, "Fast (2 buffers, least tolerance)", kLatencyFast);
    obs_property_list_add_string(latency, "Medium (3 buffers, default)", kLatencyMedium);
    obs_property_list_add_string(latency, "Slow (4 buffers)", kLatencySlow);
    obs_property_list_add_string(latency, "Very slow (5 buffers, most tolerance)", kLatencyVerySlow);
    obs_property_set_modified_callback2(latency, gmixLatencyModified, nullptr);

    // Arm the auto-fire guard for the ONE synthetic modified-callback
    // invocation OBS makes per control while building/validating this fresh
    // obs_properties_t -- see gGmixNextLatencyModifiedIsAutoFire's comment.
    gGmixNextLatencyModifiedIsAutoFire    = true;
    gGmixNextPresetModifiedIsAutoFire     = true;
    gGmixNextDensityModifiedIsAutoFire    = true;
    gGmixNextBrightnessModifiedIsAutoFire = true;

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
    s->dstBufferCount = latencyModeSettingToBufferCount(
        obs_data_get_string(settings, kSettingLatencyMode));
    // GPU index / Latency mode are fixed for the shared engine's whole
    // lifetime once the FIRST source creates it (see acquireEngine()) -- so
    // editing them on an ALREADY-EXISTING source's Properties dialog (as
    // opposed to setting them before ever creating a source) silently has NO
    // EFFECT on the already-running engine. Confirmed happening live: user
    // set Latency mode to Slow, engine stayed at Medium the whole session,
    // no error anywhere. Surface it here instead of leaving it silent.
    if (s->engine && s->engine->dstBufferCount != s->dstBufferCount) {
        blog(LOG_WARNING,
             "gmix: Latency mode change to %u buffers will NOT take effect -- the shared "
             "engine is already running at %u buffers (set by whichever \"GMix Motion Blur\" "
             "source was created first). Remove every GMix Motion Blur source and re-add to "
             "apply the new value.",
             s->dstBufferCount, s->engine->dstBufferCount);
    }
    if (!writeTargetProcessConfig(s->targetProcess)) {
        blog(LOG_WARNING, "gmix: failed to write ~/.config/gmix/target_process -- "
                           "the capture layer will not activate in the game");
    }

    // Blend preset is a property of the shared engine (see GmixEngine's
    // blendConfig comment), NOT of this specific source -- it is deliberately
    // NOT written from this routine path anymore. CONFIRMED LIVE: this used
    // to unconditionally overwrite s->engine->blendConfig every time
    // gmixUpdate() ran, including the AUTOMATIC call OBS makes when a new/
    // re-added source is created (using THAT source's own default/saved
    // settings) -- so simply adding a second source silently reset the
    // shared preset back to Flat for every scene, even though another
    // already-existing source had deliberately set Advanced. The write now
    // happens ONLY from genuine Properties-dialog interaction --
    // gmixPresetModified()/gmixDensityModified()/gmixBrightnessModified(),
    // using the same auto-fire-suppression pattern as Latency mode (see
    // gGmixNextLatencyModifiedIsAutoFire's comment for why a modified-
    // callback firing is NOT by itself proof of genuine user interaction).
    // Tradeoff, same as Latency mode and accepted for the same reason:
    // reloading a saved scene collection no longer auto-restores a non-Flat
    // preset -- it must be reselected once per OBS session.
}

void* gmixCreate(obs_data_t* settings, obs_source_t* source) {
    auto* s = new GmixSource();
    s->source = source;
    gmixUpdate(s, settings);   // parses gpuIndex/dstBufferCount, needed below

    s->engine = acquireEngine(s->gpuIndex, s->dstBufferCount);
    // engine == nullptr means headless Vulkan init failed (first source only);
    // return non-null anyway: an inert-but-valid source, per OBS's create() contract.

    // CONFIRMED LIVE: this source's OWN saved settings (target/gpuIndex/
    // Latency mode, all just parsed above) can be totally stale relative to
    // an ALREADY-RUNNING engine this call just attached to -- e.g. a fresh
    // "+"-added source always starts from gmixGetDefaults() (Flat/Medium),
    // even when it successfully attaches to an engine that's actually
    // running Advanced/Very-slow (restored from the persisted config files).
    // The ENGINE's real behavior was already correct in that case -- this
    // was purely the Properties dialog showing stale defaults, which reads
    // exactly like a functional failure even though it isn't one. refCount
    // > 1 (checked AFTER acquireEngine's own increment) means we attached to
    // something that already existed, as opposed to just having created it
    // fresh (refCount == 1) -- only then is there something to sync FROM.
    if (s->engine && s->engine->refCount.load(std::memory_order_relaxed) > 1) {
        gmix::BlendConfig cfgSnapshot;
        { std::lock_guard<std::mutex> lk(s->engine->blendConfigMu); cfgSnapshot = s->engine->blendConfig; }
        obs_data_set_string(settings, kSettingPreset, presetModeToString(cfgSnapshot.mode));
        obs_data_set_int(settings, kSettingBlurDensity, cfgSnapshot.blurDensity);
        obs_data_set_double(settings, kSettingBrightness, cfgSnapshot.shutterStrength);
        obs_data_set_int(settings, kSettingGpuIndex, s->engine->gpuIndex);
        obs_data_set_string(settings, kSettingLatencyMode,
                            bufferCountToLatencyModeSetting(s->engine->dstBufferCount));
        gmixUpdate(s, settings);   // re-parse so s->targetProcess/gpuIndex/dstBufferCount match too
    }
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

    // Ticked only when the drawn frontIdx actually CHANGED since the last
    // draw (across however many scenes this source is in, and however many
    // GmixSource instances call video_render per real OBS frame) -- measures
    // how often OBS is actually shown a NEW frame, not just how often it
    // calls video_render (which would just echo OBS's own configured fps).
    if (e->lastDrawnFrontIdx.exchange(idx, std::memory_order_relaxed) != idx) {
        e->drawnRate.tick();
        // retire -> actually drawn. Bounded by OBS's own render cadence, not
        // gmix's pipeline -- see the drawLatencyMs comment on GmixEngine.
        uint64_t readyNs = e->frontReadyTimeNs.load(std::memory_order_relaxed);
        if (readyNs != 0) {
            uint64_t nowNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (nowNs > readyNs) {
                e->drawLatencyMs.store((nowNs - readyNs) / 1e6, std::memory_order_relaxed);
            }
        }
    }
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
