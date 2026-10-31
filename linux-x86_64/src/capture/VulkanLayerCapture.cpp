#include "FrameSource.hpp"
#include <cstring>
#include <iostream>
#include <unordered_set>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <string>
#include <dlfcn.h>
#include <unistd.h>
#include "LayerIpc.hpp"
#include "LayerStatus.hpp"
#include "../ipc/frame_sender.hpp"
#include "../ipc/frame_protocol.hpp"

namespace gmix {
namespace capture {

namespace {

// ── True cursor position, read from the target's OWN already-loaded SDL3 ──────
// The capture layer runs in-process, so libSDL3.so is already mapped into our
// address space. SDL has itself processed the platform (Wayland/X11) pointer
// events, so SDL_GetMouseState() returns the window-relative cursor position as
// of this present with zero compositor-protocol work and identical behavior on
// either backend. We normalize by the window's logical size (SDL_GetWindowSize
// via the mouse-focus window) so the value is resolution/HiDPI-independent --
// the pixel density cancels, and the consumer just multiplies by the captured
// frame's pixel extent. Returns false (caller stores a <0 sentinel) if SDL
// isn't resolvable, there's no focused window, or the window has zero size.
bool readCursorNormalized(float& nx, float& ny) {
    using PFN_GetMouseState = uint32_t (*)(float*, float*);
    using PFN_GetMouseFocus = void*    (*)();
    using PFN_GetWindowSize = int      (*)(void*, int*, int*);

    struct SDL {
        PFN_GetMouseState getMouseState = nullptr;
        PFN_GetMouseFocus getMouseFocus = nullptr;
        PFN_GetWindowSize getWindowSize = nullptr;
        bool ok = false;
    };
    static const SDL sdl = []() -> SDL {
        // osu-framework's .NET native loader dlopen()s libSDL3.so by ABSOLUTE
        // PATH with local scope, so its symbols are in neither the global
        // (RTLD_DEFAULT) scope nor matchable by bare soname. Find the exact
        // resident path in our own memory map and re-dlopen THAT: already
        // loaded, so it just bumps the refcount (no second copy, no re-init)
        // and hands back a handle we can dlsym.
        void* h = dlopen("libSDL3.so", RTLD_NOLOAD | RTLD_LAZY);
        if (!h) {
            std::ifstream maps("/proc/self/maps");
            std::string line;
            while (std::getline(maps, line)) {
                std::string::size_type slash = line.find('/');
                if (slash == std::string::npos) continue;
                if (line.find("libSDL3.so") == std::string::npos) continue;
                std::string path = line.substr(slash);
                h = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
                if (!h) h = dlopen(path.c_str(), RTLD_LAZY);
                break;
            }
        }
        if (!h) return {};
        SDL s;
        s.getMouseState = reinterpret_cast<PFN_GetMouseState>(dlsym(h, "SDL_GetMouseState"));
        s.getMouseFocus = reinterpret_cast<PFN_GetMouseFocus>(dlsym(h, "SDL_GetMouseFocus"));
        s.getWindowSize = reinterpret_cast<PFN_GetWindowSize>(dlsym(h, "SDL_GetWindowSize"));
        s.ok = s.getMouseState && s.getMouseFocus && s.getWindowSize;
        return s;
    }();
    if (!sdl.ok) return false;

    void* win = sdl.getMouseFocus();
    if (!win) return false;              // pointer isn't over our window right now
    int ww = 0, wh = 0;
    sdl.getWindowSize(win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;

    float mx = 0.f, my = 0.f;
    sdl.getMouseState(&mx, &my);         // window-relative, logical points
    nx = mx / static_cast<float>(ww);
    ny = my / static_cast<float>(wh);
    return true;
}

// The blend shader declares GLSL `rgba8` (= VK_FORMAT_R8G8B8A8_UNORM) for its
// storage image bindings, so the export image is always allocated in that
// format. The real swapchain format is very likely VK_FORMAT_B8G8R8A8_*
// (different channel order, same texel size) -- maybeExportFrame() uses
// vkCmdBlitImage (not vkCmdCopyImage) to get there, since a blit performs a
// proper per-channel semantic conversion between same-size formats with
// different channel order, where a raw copy would just reinterpret the bytes
// and swap red/blue.
bool isSupportedExportFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
    }
}

constexpr VkFormat kExportFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Throttle: a MINIMUM spacing between export attempts. It must sit well above
// the game's real present rate, or it ALIASES against it: at osu!'s ~1000fps the
// presents land ~1ms apart with jitter, so a 1ms floor skipped roughly every
// other present (a beat-frequency effect) and collapsed capture to ~600fps.
// 200us (=5000fps cap) is far above any real present rate, so every present is
// captured and the rings + the consumer-side drop logic become the only
// limiters on how many frames actually make it through.
// Producer-side export throttle is also the producer rate cap: one capture
// per >=222.223us, i.e. <= ~4500 exports/s -- matching the consumer's
// receive cap (kMinAcceptInterval in the obs plugin's receiverThreadFn) so
// the producer never floods frames the receiver would just cap away.
// (Previously 200us/"~5000fps" back when the consumer had no cap.)
constexpr auto kExportInterval = std::chrono::nanoseconds(500000); // 2000fps cap

uint32_t findDeviceLocalMemoryType(PFN_vkGetPhysicalDeviceMemoryProperties getProps,
                                   VkPhysicalDevice phys, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties mp{};
    getProps(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (typeBits & (1u << i)) return i;
    return ~0u;
}

} // namespace

VulkanLayerCapture::VulkanLayerCapture() = default;
VulkanLayerCapture::~VulkanLayerCapture() {
    shutdown();
}

bool VulkanLayerCapture::init(std::string targetProcess) {
    targetProcess_ = std::move(targetProcess);
    active_ = false;
    instance_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    trackedQueues_.clear();
    presentCount_ = 0;
    ipc_ = std::make_unique<LayerIpc>();
    if (ipc_->start()) {
        try {
            const char* home = std::getenv("HOME");
            if (home) {
                std::filesystem::path cacheDir = std::filesystem::path(home) / ".cache" / "gmix";
                std::error_code ec;
                std::filesystem::create_directories(cacheDir, ec);
                std::filesystem::path socketFile = cacheDir / "socket";
                std::ofstream out(socketFile);
                if (out) out << ipc_->socketPath();
            }
        } catch (...) {}
    }
    return !targetProcess_.empty();
}

void VulkanLayerCapture::shutdown() {
    active_ = false;
    instance_ = VK_NULL_HANDLE;
    trackedQueues_.clear();
    presentCount_ = 0;
    if (ipc_) {
        ipc_->stop();
        ipc_.reset();
    }

    connectorRunning_ = false;
    if (connectorThread_.joinable()) connectorThread_.join();
    {
        std::lock_guard<std::mutex> lk(senderMu_);
        sender_.reset();
    }
    // Must run before clearing device_ -- it's keyed on that handle.
    destroyExportResources();
    device_ = VK_NULL_HANDLE;
}

bool VulkanLayerCapture::isActive() const {
    return active_;
}

bool VulkanLayerCapture::onInstanceCreated(VkInstance instance) {
    if (instance_ != VK_NULL_HANDLE) return false;
    instance_ = instance;
    active_ = true;
    std::fprintf(stderr, "VkLayer_GMIX: target process '%s' created VkInstance %p\n",
                 targetProcess_.c_str(), static_cast<void*>(instance));

    connectorRunning_ = true;
    connectorThread_ = std::thread([this]() { connectorLoop(); });
    // Visible in OBS's "apps with the GMix layer loaded" list even before the
    // OBS source exists / connects.
    LayerStatus::instance().set("waiting", targetProcess_, true);
    return true;
}

bool VulkanLayerCapture::onDeviceCreated(VkDevice device, VkPhysicalDevice physicalDevice,
                                         PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties,
                                         PFN_vkGetMemoryFdKHR getMemoryFdKHR,
                                         PFN_vkGetSemaphoreFdKHR getSemaphoreFdKHR,
                                         PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties,
                                         PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties) {
    if (!active_ || device_ != VK_NULL_HANDLE) return false;
    device_ = device;
    physDevice_ = physicalDevice;
    getPhysicalDeviceMemoryProperties_ = getPhysicalDeviceMemoryProperties;
    getMemoryFdKHR_ = getMemoryFdKHR;
    getSemaphoreFdKHR_ = getSemaphoreFdKHR;
    getPhysicalDeviceProperties_ = getPhysicalDeviceProperties;
    getPhysicalDeviceQueueFamilyProperties_ = getPhysicalDeviceQueueFamilyProperties;
    std::fprintf(stderr, "VkLayer_GMIX: target process '%s' created VkDevice %p\n",
                 targetProcess_.c_str(), static_cast<void*>(device));
    return true;
}

bool VulkanLayerCapture::registerQueue(VkQueue queue, VkDevice device, uint32_t queueFamilyIndex) {
    if (!active_) return false;
    if (queue == VK_NULL_HANDLE || device != device_) return false;
    trackedQueues_.insert(queue);
    {
        std::lock_guard<std::mutex> lk(exportMu_);
        queueFamilies_[queue] = queueFamilyIndex;
    }
    std::fprintf(stderr, "VkLayer_GMIX: target process '%s' tracking queue %p\n",
                 targetProcess_.c_str(), static_cast<void*>(queue));
    return true;
}

void VulkanLayerCapture::onDeviceDestroyed() {
    destroyExportResources();
    {
        std::lock_guard<std::mutex> lk(exportMu_);
        device_ = VK_NULL_HANDLE;
        physDevice_ = VK_NULL_HANDLE;
        getPhysicalDeviceMemoryProperties_ = nullptr;
        getMemoryFdKHR_ = nullptr;
        getSemaphoreFdKHR_ = nullptr;
        getPhysicalDeviceProperties_ = nullptr;
        getPhysicalDeviceQueueFamilyProperties_ = nullptr;
        queueFamilies_.clear();
    }
    trackedQueues_.clear();
}

void VulkanLayerCapture::onSwapchainCreated(VkSwapchainKHR swapchain,
                                            const std::vector<VkImage>& images,
                                            VkFormat format, VkExtent2D extent) {
    std::lock_guard<std::mutex> lk(swapMu_);
    SwapchainInfo info;
    info.images = images;
    info.format = format;
    info.extent = extent;
    swapchains_[swapchain] = std::move(info);
}

void VulkanLayerCapture::onSwapchainDestroyed(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lk(swapMu_);
    swapchains_.erase(swapchain);
}

bool VulkanLayerCapture::onQueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo) {
    if (!active_) return false;
    if (trackedQueues_.find(queue) == trackedQueues_.end()) return false;
    ++presentCount_;

    // This used to unconditionally fprintf(stderr, ...) AND open/append/close
    // ~/.cache/gmix/presents.log on EVERY present -- i.e. hundreds to 1000+
    // times a second, entirely synchronous, inside the game's own present
    // thread (this function runs as part of the hooked vkQueuePresentKHR,
    // before the real present proceeds). That's real, blocking disk I/O and
    // syscall overhead stalling the game on every frame, independent of any
    // GPU/compute cost -- confirmed as a live cause of osu! stutter, and the
    // debug log file itself had silently grown to 5+ GB from being left on
    // permanently since it was added for one-off "quick testing and
    // verification". Removed entirely; the one diagnostic worth keeping
    // (the consumer's blend pipeline actually falling behind and dropping
    // frames) is logged instead, rate-limited, from where that actually
    // happens -- see the drop branch in gmix_source.cpp's receiverThreadFn().

    maybeExportFrame(queue, pPresentInfo);
    return true;
}

std::string VulkanLayerCapture::targetProcessName() const {
    return targetProcess_;
}

// ── frame export ─────────────────────────────────────────────────────────────

void VulkanLayerCapture::connectorLoop() {
    auto path = ipc::defaultFrameSocketPath();
    while (connectorRunning_) {
        bool needConnect = false;
        {
            std::lock_guard<std::mutex> lk(senderMu_);
            needConnect = !sender_ || !sender_->isConnected();
        }
        if (needConnect) {
            // connect() retries internally for ~2s on its own before giving
            // up -- fine here since this is the background thread, never the
            // present thread.
            auto s = std::make_unique<ipc::FrameSender>();
            if (s->connect(path)) {
                std::lock_guard<std::mutex> lk(senderMu_);
                sender_ = std::move(s);
                handshakeSent_.store(false, std::memory_order_release);
                LayerStatus::instance().set("capturing", targetProcess_, true);
            } else {
                LayerStatus::instance().set("waiting", targetProcess_, true);
            }
        }

        // ── the handshake happens HERE, never on the present thread ────────
        // sendHandshake() does a blocking send + ack read (bounded by the
        // connection's 5ms SO_SNDTIMEO, ~10ms worst case); running that on
        // the app's present thread used to be a hiccup in the game exactly
        // whenever OBS attached. The present thread only publishes the
        // frame size it wants (pendingW_/H_, set without blocking from
        // the swapchain info); this thread round-trips the handshake and
        // only then do exports on the present thread begin.
        std::shared_ptr<ipc::FrameSender> handshaker;
        {
            std::lock_guard<std::mutex> lk(senderMu_);
            handshaker = sender_;   // keeps it alive through the blocking call
        }
        if (handshaker && handshaker->isConnected() &&
            !handshakeSent_.load(std::memory_order_acquire)) {
            uint32_t w = 0, h = 0;
            {
                std::lock_guard<std::mutex> lk(pendingMu_);
                if (pendingHandshake_) { w = pendingW_; h = pendingH_; }
            }
            if (w != 0 && h != 0) {
                if (handshaker->sendHandshake(w, h, static_cast<uint32_t>(kExportFormat))) {
                    handshakeSent_.store(true, std::memory_order_release);
                } else {
                    // Dead/broken connection: drop it; the next loop
                    // iteration reconnects and retries the handshake.
                    std::lock_guard<std::mutex> lk(senderMu_);
                    if (sender_ == handshaker) sender_.reset();
                }
            }
        }
        for (int i = 0; i < 10 && connectorRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

VkCommandPool VulkanLayerCapture::ensureCmdPool(uint32_t queueFamilyIndex) {
    auto it = cmdPools_.find(queueFamilyIndex);
    if (it != cmdPools_.end()) return it->second;

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamilyIndex;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device_, &cpci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    cmdPools_[queueFamilyIndex] = pool;
    return pool;
}

bool VulkanLayerCapture::ensureExportImage(uint32_t w, uint32_t h) {
    if (!getPhysicalDeviceMemoryProperties_) return false;
    if (exportImage_[0] != VK_NULL_HANDLE && exportW_ == w && exportH_ == h) return true;

    // Size changed (or first call): tear down any existing ring and rebuild.
    for (int k = 0; k < kExportRing; ++k) {
        if (exportImage_[k] != VK_NULL_HANDLE) { vkDestroyImage(device_, exportImage_[k], nullptr); exportImage_[k] = VK_NULL_HANDLE; }
        if (exportMem_[k]   != VK_NULL_HANDLE) { vkFreeMemory(device_, exportMem_[k], nullptr);   exportMem_[k]   = VK_NULL_HANDLE; }
    }
    exportNext_ = 0;

    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kExportFormat;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    // VK_EXT_image_drm_format_modifier (which would make the row layout
    // explicit/negotiated) is not supported by this RADV/Polaris build at
    // all (confirmed via vulkaninfo -- absent from the AMD GPU's device
    // extension list, only present under llvmpipe). Falling back to plain
    // LINEAR tiling: deterministic for identical parameters on the same
    // device/driver, and the consumer is told the exact rowPitch.
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExportMemoryAllocateInfo ema{};
    ema.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    ema.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    auto cleanupPartial = [&]() {
        for (int k = 0; k < kExportRing; ++k) {
            if (exportImage_[k] != VK_NULL_HANDLE) { vkDestroyImage(device_, exportImage_[k], nullptr); exportImage_[k] = VK_NULL_HANDLE; }
            if (exportMem_[k]   != VK_NULL_HANDLE) { vkFreeMemory(device_, exportMem_[k], nullptr);   exportMem_[k]   = VK_NULL_HANDLE; }
        }
    };

    for (int k = 0; k < kExportRing; ++k) {
        if (vkCreateImage(device_, &ici, nullptr, &exportImage_[k]) != VK_SUCCESS) { cleanupPartial(); return false; }

        if (k == 0) {
            VkImageSubresource subres{};
            subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, exportImage_[0], &subres, &layout);
            exportRowPitch_ = layout.rowPitch;
        }

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device_, exportImage_[k], &mr);
        uint32_t memType = findDeviceLocalMemoryType(getPhysicalDeviceMemoryProperties_, physDevice_, mr.memoryTypeBits);
        if (memType == ~0u) { cleanupPartial(); return false; }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &ema;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType;
        if (vkAllocateMemory(device_, &mai, nullptr, &exportMem_[k]) != VK_SUCCESS) { cleanupPartial(); return false; }
        if (vkBindImageMemory(device_, exportImage_[k], exportMem_[k], 0) != VK_SUCCESS) { cleanupPartial(); return false; }
    }

    exportW_ = w;
    exportH_ = h;
    return true;
}

// Create the persistent per-slot export timeline semaphores once. They are
// independent of image size, so unlike the export images they are NOT rebuilt
// on resize -- which is what lets the consumer import each slot's semaphore a
// single time and keep reusing it across resizes.
bool VulkanLayerCapture::ensureExportSemaphores() {
    if (exportSem_[0] != VK_NULL_HANDLE) return true;   // already created
    VkExportSemaphoreCreateInfo esci{};
    esci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreTypeCreateInfo tci{};
    tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    tci.pNext = &esci;
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &tci;
    for (int k = 0; k < kExportRing; ++k) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &exportSem_[k]) != VK_SUCCESS) {
            for (int j = 0; j < k; ++j) {
                vkDestroySemaphore(device_, exportSem_[j], nullptr);
                exportSem_[j] = VK_NULL_HANDLE;
            }
            return false;
        }
    }
    return true;
}

void VulkanLayerCapture::destroyExportResources() {
    std::lock_guard<std::mutex> exportLock(exportMu_);
    if (device_ == VK_NULL_HANDLE) return;
    for (int i = 0; i < kRingSize; ++i) {
        if (ringFence_[i] != VK_NULL_HANDLE) vkDestroyFence(device_, ringFence_[i], nullptr);
        ringFence_[i] = VK_NULL_HANDLE;
        if (ringPresentChainSem_[i] != VK_NULL_HANDLE) vkDestroySemaphore(device_, ringPresentChainSem_[i], nullptr);
        ringPresentChainSem_[i] = VK_NULL_HANDLE;
        ringValid_[i] = false;
    }
    for (auto& kv : cmdPools_) vkDestroyCommandPool(device_, kv.second, nullptr);
    cmdPools_.clear();
    if (tsQueryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, tsQueryPool_, nullptr);
        tsQueryPool_ = VK_NULL_HANDLE;
    }
    timestampsSupported_ = false;
    for (int k = 0; k < kExportRing; ++k) {
        if (exportImage_[k] != VK_NULL_HANDLE) { vkDestroyImage(device_, exportImage_[k], nullptr); exportImage_[k] = VK_NULL_HANDLE; }
        if (exportMem_[k]   != VK_NULL_HANDLE) { vkFreeMemory(device_, exportMem_[k], nullptr);   exportMem_[k]   = VK_NULL_HANDLE; }
        if (exportSem_[k]   != VK_NULL_HANDLE) { vkDestroySemaphore(device_, exportSem_[k], nullptr); exportSem_[k] = VK_NULL_HANDLE; }
    }
}

// ── export gate telemetry (debug aid; delete when the capture reaches v1.0) ──
// Every early return in maybeExportFrame() historically vanished without a
// trace, which made "connected but zero frames in OBS" impossible to chase
// (exactly the osu!stable-at-4500fps deadlock seen on 2026-10-31: handshake
// landed, one frame crossed, then all exports stopped for unknown reasons).
// Gate counters fire on the app's present thread; logging stays on a 1/s
// rate-limited line in the app's own stderr (winello.log under wine).
namespace expgate {
struct telemetry {
    enum Label { OK, WAIT_HS, NO_SENDER, NO_SWAP, BAD_FORMAT, NO_QF,
                 IMG_FAIL, SEM_FAIL, POOL_FAIL, RING_FAIL, FENCE_BUSY,
                 SUBMIT_FAIL, FD_FAIL, NLABELS };
    static constexpr const char* kNames[NLABELS] = {
        "ok", "waitingFS", "noSender", "noSwapInfo", "badFormat", "noQF",
        "exportImgFail", "semFail", "cmdPoolFail", "ringAllocFail",
        "fenceBusy", "submitFail", "fdFail" };
    static void mark(Label l) {
        counters_[l].fetch_add(1, std::memory_order_relaxed);
        maybePrint();
    }
    static void maybePrint() {
        auto now = std::chrono::steady_clock::now();
        auto last = lastPrint_.load(std::memory_order_relaxed);
        if (now.time_since_epoch().count() - last <
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::seconds(1)).count())
            return;
        // Race-prone CAS on the clock; losers stay quiet. No mutex on the hot
        // path: printing from inside the present hook happens once a second.
        if (!lastPrint_.compare_exchange_strong(last, now.time_since_epoch().count(),
                                                std::memory_order_relaxed))
            return;
        std::string line = "GMIX export:";
        bool any = false;
        for (int i = 0; i < NLABELS; ++i) {
            uint64_t v = counters_[i].exchange(0, std::memory_order_relaxed);
            if (v != 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " %s=%llu", kNames[i],
                              static_cast<unsigned long long>(v));
                line += buf;
                any = true;
            }
        }
        // GPU blit duration (see maybeExportFrame): two per-slot GPU
        // timestamps (pre-blit barrier + bottom-of-pipe) diffed on the
        // device's own clock and scaled by its timestamp period. This is the
        // per-frame GPU time the capture inserts into the frame's critical
        // path; if it is small, the layer is innocent and the remaining
        // slowdown comes from Zink rendering itself.
        uint64_t bc = blitCnt_.exchange(0, std::memory_order_relaxed);
        if (bc) {
            uint64_t bs = blitSumNs_.exchange(0, std::memory_order_relaxed);
            uint64_t bx = blitMaxNs_.exchange(0, std::memory_order_relaxed);
            char bbuf[128];
            std::snprintf(bbuf, sizeof(bbuf),
                          " blitAvg=%.0fus blitMax=%.0fus n=%llu",
                          static_cast<double>(bs) / bc / 1000.0,
                          static_cast<double>(bx) / 1000.0,
                          static_cast<unsigned long long>(bc));
            line += bbuf;
            any = true;
        }
        if (any) std::fprintf(stderr, "%s\n", line.c_str());
    }
    static void recordGpuBlitNs(uint64_t ns) {
        if (!ns) return;
        blitSumNs_.fetch_add(ns, std::memory_order_relaxed);
        blitCnt_.fetch_add(1, std::memory_order_relaxed);
        uint64_t maxv = blitMaxNs_.load(std::memory_order_relaxed);
        while (ns > maxv &&
               !blitMaxNs_.compare_exchange_weak(maxv, ns, std::memory_order_relaxed)) {}
    }
    static std::atomic<uint64_t> counters_[NLABELS];
    static std::atomic<uint64_t> lastPrint_;
    static std::atomic<uint64_t> blitSumNs_;
    static std::atomic<uint64_t> blitCnt_;
    static std::atomic<uint64_t> blitMaxNs_;
};
std::atomic<uint64_t> telemetry::counters_[telemetry::NLABELS] = {};
std::atomic<uint64_t> telemetry::lastPrint_{0};
std::atomic<uint64_t> telemetry::blitSumNs_{0};
std::atomic<uint64_t> telemetry::blitCnt_{0};
std::atomic<uint64_t> telemetry::blitMaxNs_{0};
}

using ExportTelemetry = expgate::telemetry;
#define GMXMARK(lbl) ExportTelemetry::mark(ExportTelemetry::lbl)

void VulkanLayerCapture::maybeExportFrame(VkQueue queue, VkPresentInfoKHR* pPresentInfo) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) return;

    // Some apps present from more than one thread; everything below touches
    // device_/getMemoryFdKHR_/getSemaphoreFdKHR_/queueFamilies_/the ring
    // buffer/command pools/export image, none of which is otherwise safe for
    // concurrent access.
    std::lock_guard<std::mutex> exportLock(exportMu_);
    if (!getMemoryFdKHR_ || !getSemaphoreFdKHR_) return;

    auto now = std::chrono::steady_clock::now();
    if (lastExportAttempt_.time_since_epoch().count() != 0 &&
        now - lastExportAttempt_ < kExportInterval) {
        return;
    }
    lastExportAttempt_ = now;

    // Is a consumer connected? (Connecting itself happens off this thread.)
    ipc::FrameSender* sender = nullptr;
    {
        std::lock_guard<std::mutex> lk(senderMu_);
        if (sender_ && sender_->isConnected()) sender = sender_.get();
    }
    if (!sender) { GMXMARK(NO_SENDER); return; }

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t imageIndex = pPresentInfo->pImageIndices[0];

    SwapchainInfo info;
    {
        std::lock_guard<std::mutex> lk(swapMu_);
        auto it = swapchains_.find(swapchain);
        if (it == swapchains_.end() || imageIndex >= it->second.images.size()) { GMXMARK(NO_SWAP); return; }
        info = it->second; // copy: small vector of VkImage handles
    }
    if (!isSupportedExportFormat(info.format)) { GMXMARK(BAD_FORMAT); return; }

    // Handshake alive check BEFORE any export work: sendHandshake() does a
    // blocking send + ack read (bounded by the connection's 5ms SO_SNDTIMEO,
    // ~10ms worst case) and used to run RIGHT HERE on the app's present
    // thread after every (re)connect -- a hiccup in the game exactly when
    // OBS attached. It now happens on the connector thread (see
    // connectorLoop); this thread only publishes the swapchain size (zero
    // blocking) and skips exporting until the handshake has landed, at
    // which point captures start with no extra present-thread work at all.
    if (!handshakeSent_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingW_ = info.extent.width;
        pendingH_ = info.extent.height;
        pendingHandshake_ = true;
        GMXMARK(WAIT_HS);
        return;
    }

    auto qfIt = queueFamilies_.find(queue);
    if (qfIt == queueFamilies_.end()) { GMXMARK(NO_QF); return; }
    uint32_t queueFamily = qfIt->second;

    if (!ensureExportImage(info.extent.width, info.extent.height)) { GMXMARK(IMG_FAIL); return; }
    if (!ensureExportSemaphores()) { GMXMARK(SEM_FAIL); return; }

    VkCommandPool pool = ensureCmdPool(queueFamily);
    if (pool == VK_NULL_HANDLE) { GMXMARK(POOL_FAIL); return; }

    // Lazily (re)create the ring's command buffers/fences against this pool.
    if (ringPoolFamily_ != queueFamily) {
        for (int i = 0; i < kRingSize; ++i) {
            if (ringFence_[i] != VK_NULL_HANDLE) { vkDestroyFence(device_, ringFence_[i], nullptr); ringFence_[i] = VK_NULL_HANDLE; }
            if (ringPresentChainSem_[i] != VK_NULL_HANDLE) { vkDestroySemaphore(device_, ringPresentChainSem_[i], nullptr); ringPresentChainSem_[i] = VK_NULL_HANDLE; }
            ringValid_[i] = false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kRingSize;
        if (vkAllocateCommandBuffers(device_, &cai, ringCmd_) != VK_SUCCESS) { GMXMARK(RING_FAIL); return; }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkSemaphoreCreateInfo binSci{};
        binSci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (int i = 0; i < kRingSize; ++i) {
            if (vkCreateFence(device_, &fci, nullptr, &ringFence_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &binSci, nullptr, &ringPresentChainSem_[i]) != VK_SUCCESS) {
                GMXMARK(RING_FAIL);
                return;
            }
        }

        // ── GPU timestamp support for THIS present queue family ──────────────
        // One timestamp query per ring slot. Only enabled if the family reports
        // a non-zero timestampValidBits (some transfer-only queues report 0, on
        // which vkCmdWriteTimestamp is invalid) and a usable period. If anything
        // is missing we leave timestampsSupported_ false and fall back to the
        // CPU timestamp -- the consumer handles gpuTimestampNs == 0 gracefully.
        if (tsQueryPool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, tsQueryPool_, nullptr);
            tsQueryPool_ = VK_NULL_HANDLE;
        }
        timestampsSupported_ = false;
        timestampPeriodNs_ = 0.0;
        timestampValidBits_ = 0;
        if (getPhysicalDeviceProperties_ && getPhysicalDeviceQueueFamilyProperties_) {
            VkPhysicalDeviceProperties props{};
            getPhysicalDeviceProperties_(physDevice_, &props);
            timestampPeriodNs_ = static_cast<double>(props.limits.timestampPeriod);
            uint32_t qfCount = 0;
            getPhysicalDeviceQueueFamilyProperties_(physDevice_, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qfp(qfCount);
            if (qfCount) getPhysicalDeviceQueueFamilyProperties_(physDevice_, &qfCount, qfp.data());
            if (queueFamily < qfCount && qfp[queueFamily].timestampValidBits > 0 &&
                timestampPeriodNs_ > 0.0) {
                timestampValidBits_ = qfp[queueFamily].timestampValidBits;
                VkQueryPoolCreateInfo qpci{};
                qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qpci.queryCount = kRingSize * 2;
                if (vkCreateQueryPool(device_, &qpci, nullptr, &tsQueryPool_) == VK_SUCCESS)
                    timestampsSupported_ = true;
            }
        }

        ringPoolFamily_ = queueFamily;
        ringNext_ = 0;
    }

    int slot = ringNext_;
    uint64_t gpuTsToSend = 0;   // GPU-domain capture time read back from this slot
    if (ringValid_[slot]) {
        // Non-blocking poll -- if this slot's previous submission hasn't
        // finished yet, skip this present's export entirely rather than
        // ever waiting on the present thread.
        VkResult fr = vkGetFenceStatus(device_, ringFence_[slot]);
        if (fr != VK_SUCCESS) { GMXMARK(FENCE_BUSY); return; }
        // The same signaled fence also means this slot's timestamp query (from
        // its PREVIOUS capture) is now resolved, so we can read it back without
        // blocking. That GPU time belongs to an earlier frame, but the consumer
        // only uses deltas between successive gpuTimestampNs values for the
        // capture-rate estimate, and those deltas are the true inter-frame GPU
        // intervals regardless of the constant ring-length offset.
        if (timestampsSupported_ && tsQueryPool_ != VK_NULL_HANDLE) {
            uint64_t t2[2] = {0, 0};
            if (vkGetQueryPoolResults(device_, tsQueryPool_, 2 * static_cast<uint32_t>(slot), 2,
                                      sizeof(t2), &t2, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                // Masks are per-value; the wrap-aware DIFF must be taken in
                // the full tick domain (delta can legitimately exceed the
                // truncated bits when the counter rolls over between the two
                // reads).
                uint64_t period = timestampValidBits_ < 64
                    ? (uint64_t(1) << timestampValidBits_)
                    : 0;   // 0 = 64-bit clock, no wrap possible
                uint64_t tickDelta;
                if (t2[1] >= t2[0])
                    tickDelta = t2[1] - t2[0];
                else if (period > 0)
                    tickDelta = period - t2[0] + t2[1];
                else
                    tickDelta = 0;
                // Capture-time GPU clock for the consumer's capture-rate
                // estimate: the slot's END timestamp converted via the device
                // period, same as the single-timestamp scheme this replaces
                // (successive deltas = true inter-frame GPU intervals).
                gpuTsToSend = static_cast<uint64_t>(
                    static_cast<double>(t2[1]) * timestampPeriodNs_);
                // Pure GPU cost of the inserted capture work: bottom-of-pipe
                // (end, after blit + restore barrier) minus just-before-the
                // -blit, both on the device's own clock. Fence signaled =
                // both queries resolved, so the readback never blocks.
                ExportTelemetry::recordGpuBlitNs(
                    static_cast<uint64_t>(static_cast<double>(tickDelta) * timestampPeriodNs_));
            }
        }
    }
    ringNext_ = (ringNext_ + 1) % kRingSize;

    // Pick the next EXPORT-image ring slot (distinct, larger ring than the
    // command/fence ring above). Each exported frame gets its own backing
    // memory so the consumer's blend window sees genuinely different pixels
    // per frame rather than N aliases of the latest one.
    int eslot = exportNext_;
    exportNext_ = (exportNext_ + 1) % kExportRing;
    VkImage exportImg = exportImage_[eslot];
    VkDeviceMemory exportMemSlot = exportMem_[eslot];

    VkImage srcImage = info.images[imageIndex];
    VkCommandBuffer cmd = ringCmd_[slot];

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Reset this slot's timestamp query before (re)writing it. Queries must be
    // reset before each use; doing it here (outside any render pass) is valid.
    if (timestampsSupported_ && tsQueryPool_ != VK_NULL_HANDLE)
        vkCmdResetQueryPool(cmd, tsQueryPool_, 2 * static_cast<uint32_t>(slot), 2);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = srcImage;
    toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier dstToTransfer{};
    dstToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstToTransfer.srcAccessMask = 0;
    dstToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstToTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToTransfer.image = exportImg;
    dstToTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier preCopy[2] = { toSrc, dstToTransfer };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, preCopy);
    // START of the capture's inserted GPU work (the pre-blit barrier just
    // passed; the blit follows in submission order). Diffed against the
    // bottom-of-pipe timestamp at the end -> blitAvg/blitMax telemetry.
    if (timestampsSupported_ && tsQueryPool_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, tsQueryPool_,
                            2 * static_cast<uint32_t>(slot));

    // Blit, not copy: the real swapchain format is very likely
    // VK_FORMAT_B8G8R8A8_* while exportImage_ is fixed at R8G8B8A8_UNORM
    // (matching the blend shader's `rgba8` storage-image declaration).
    // vkCmdCopyImage does a raw byte copy with no channel reinterpretation,
    // which produces a red/blue swap when the formats' channel order
    // differs. vkCmdBlitImage performs a proper per-channel semantic
    // conversion between same-size-different-order formats (same technique
    // WaylandWindow::present() already relies on for its own RGBA8->BGRA8
    // conversion), so colors come out correct regardless of which order the
    // real swapchain uses.
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)info.extent.width, (int32_t)info.extent.height, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1] = { (int32_t)info.extent.width, (int32_t)info.extent.height, 1 };
    vkCmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   exportImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    VkImageMemoryBarrier srcBack{};
    srcBack.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBack.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBack.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBack.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBack.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.image = srcImage;
    srcBack.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier dstToGeneral{};
    dstToGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstToGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dstToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.image = exportImg;
    dstToGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier postCopy[2] = { srcBack, dstToGeneral };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 2, postCopy);

    // Record the GPU time the capture finished. BOTTOM_OF_PIPE = after all prior
    // commands (the blit + restoring barriers) have completed on the GPU.
    if (timestampsSupported_ && tsQueryPool_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            tsQueryPool_, 2 * static_cast<uint32_t>(slot) + 1);

    vkEndCommandBuffer(cmd);

    // Signal this export slot's PERSISTENT timeline semaphore (created once in
    // ensureExportSemaphores with an OPAQUE_FD export handle type) to an
    // increasing value. It is never created/destroyed per frame -- the consumer
    // imports it once per slot and just waits the per-frame value, which is what
    // removed the per-frame semaphore create/import/destroy churn on both sides.
    VkSemaphore sem = exportSem_[eslot];
    uint64_t sigVal = ++frameIndex_;
    // Mixing a timeline semaphore (sem) and a binary one (the present-chain
    // semaphore) in one signal list is valid -- every signal slot needs an
    // entry in pSignalSemaphoreValues, but the implementation ignores the
    // value for non-timeline semaphores.
    VkSemaphore signalSems[2] = { sem, ringPresentChainSem_[slot] };
    uint64_t signalVals[2] = { sigVal, 0 };
    VkTimelineSemaphoreSubmitInfo tsi{};
    tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = 2;
    tsi.pSignalSemaphoreValues = signalVals;

    // Cap on the app's own wait-semaphore count we can chain through; if
    // ever exceeded (extremely unlikely -- apps typically present with one),
    // skip the synchronized path rather than truncating real wait semaphores.
    constexpr uint32_t kMaxChainWaits = 8;
    VkPipelineStageFlags waitStages[kMaxChainWaits];
    bool canChainWaits = pPresentInfo->waitSemaphoreCount <= kMaxChainWaits;
    if (canChainWaits) {
        for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i)
            waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tsi;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = signalSems;
    if (canChainWaits && pPresentInfo->waitSemaphoreCount > 0) {
        // Consume the app's own present-wait semaphores ourselves (valid --
        // we are now the sole waiter) so our copy only runs once the app's
        // render-complete signal has actually landed, then chain the real
        // present through our own signal below instead.
        si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
        si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
        si.pWaitDstStageMask = waitStages;
    }

    vkResetFences(device_, 1, &ringFence_[slot]);
    // Submitted on the app's own present queue, right where it would have
    // called the real present. Without the wait/signal chaining above, the
    // real present call below (using the app's ORIGINAL wait semaphores)
    // would have no idea we inserted extra GPU work, and the presentation
    // engine could display the image before our barrier restoring it to
    // PRESENT_SRC_KHR has actually finished executing -- that race is what
    // produced the tiled/shuffled-block corruption seen before this fix.
    if (vkQueueSubmit(queue, 1, &si, ringFence_[slot]) != VK_SUCCESS) {
        GMXMARK(SUBMIT_FAIL);
        return;   // persistent per-slot semaphore is reused; nothing to destroy
    }
    ringValid_[slot] = true;

    if (canChainWaits) {
        // Rewrite the real present to wait on our completion instead of the
        // app's original semaphores (which we just consumed above).
        pPresentInfo->waitSemaphoreCount = 1;
        pPresentInfo->pWaitSemaphores = &ringPresentChainSem_[slot];
    }

    VkMemoryGetFdInfoKHR mgfi{};
    mgfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    mgfi.memory = exportMemSlot;
    mgfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int memFd = -1;
    getMemoryFdKHR_(device_, &mgfi, &memFd);

    VkSemaphoreGetFdInfoKHR sgfi{};
    sgfi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sgfi.semaphore = sem;
    sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int semFd = -1;
    getSemaphoreFdKHR_(device_, &sgfi, &semFd);
    // `sem` is the persistent per-slot export semaphore; this fd is a fresh
    // handle referencing its payload and is closed by sendFrame once the kernel
    // has dup'd it across to the consumer.

    if (memFd < 0 || semFd < 0) {
        GMXMARK(FD_FAIL);
        if (memFd >= 0) ::close(memFd);
        if (semFd >= 0) ::close(semFd);
        return;
    }

    // Handshake already happened on the connector thread (handshakeSent_ was
    // checked at the top of this function) -- this is the plain frame send.
    ipc::FrameHeader hdr{};
    hdr.magic = ipc::kMagic;
    hdr.width = exportW_;
    hdr.height = exportH_;
    hdr.vkFormat = static_cast<uint32_t>(kExportFormat);
    hdr.exportSlot = static_cast<uint32_t>(eslot);
    hdr.semSignalValue = sigVal;
    hdr.frameIndex = frameIndex_;
    hdr.timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    hdr.rowPitch = exportRowPitch_;
    hdr.gpuTimestampNs = gpuTsToSend;
    // True cursor position for THIS frame's present (see readCursorNormalized
    // + the cursorX/Y field docs in frame_protocol.hpp). <0 sentinel when
    // unavailable so the consumer can skip it when reconstructing the path.
    float cnx = -1.f, cny = -1.f;
    readCursorNormalized(cnx, cny);
    hdr.cursorX = cnx;
    hdr.cursorY = cny;

    // sendFrame closes memFd/semFd itself on both success and skip (the
    // kernel dup'd the fd handles into the consumer's fd table). Only a hard
    // failure (dead connection) comes back here. Skipped (receiver
    // backpressure) is NOT a connection failure -- only Failed drops the
    // connection for the connector thread to rebuild.
    const bool ok =
        sender->sendFrame(hdr, memFd, semFd) != ipc::FrameSender::SendResult::Failed;

    if (!ok) {
        GMXMARK(SUBMIT_FAIL);
        // Drop the connection -- the connector thread will reconnect.
        std::lock_guard<std::mutex> lk(senderMu_);
        if (sender_.get() == sender) sender_.reset();
    } else {
        GMXMARK(OK);
    }
}

} // namespace capture
} // namespace gmix
