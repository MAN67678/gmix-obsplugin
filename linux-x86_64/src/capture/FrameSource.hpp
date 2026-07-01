#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>
#include <memory>

namespace gmix {
namespace ipc { class FrameSender; }
namespace capture {

// Abstract capture source interface used by the Vulkan layer / host.
// This is intentionally minimal for the current scaffolding phase.
struct FrameSource {
    virtual ~FrameSource() = default;

    // Initialize the capture backend with a target process name.
    virtual bool init(std::string targetProcess) = 0;

    // Tear down any active capture state.
    virtual void shutdown() = 0;

    // Whether the capture source has been activated for the current process.
    virtual bool isActive() const = 0;

    // Called when the target process creates a VkInstance.
    virtual bool onInstanceCreated(VkInstance instance) = 0;

    // Called when the target process creates a VkDevice. `getMemoryFdKHR`/
    // `getSemaphoreFdKHR` are the next-layer/driver function pointers needed
    // to export frame memory + a timeline semaphore for IPC. `physicalDevice`
    // is needed to query memory types for the exportable allocation, via
    // `getPhysicalDeviceMemoryProperties` -- fetched explicitly through the
    // next layer's GetInstanceProcAddr rather than called as a bare global
    // symbol, since the loader's own handle-validation table doesn't
    // recognize a physical device handle that way from inside an injected
    // layer (calling it as a bare symbol aborts with an "invalid
    // physicalDevice parameter" loader error).
    // `getPhysicalDeviceProperties` / `getPhysicalDeviceQueueFamilyProperties`
    // (fetched the same next-layer way) let the capture query the GPU's
    // timestampPeriod and the present queue family's timestampValidBits, so it
    // can stamp each exported frame with a real GPU-domain capture time.
    virtual bool onDeviceCreated(VkDevice device, VkPhysicalDevice physicalDevice,
                                  PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties,
                                  PFN_vkGetMemoryFdKHR getMemoryFdKHR,
                                  PFN_vkGetSemaphoreFdKHR getSemaphoreFdKHR,
                                  PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties,
                                  PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties) = 0;

    // Called when the target process obtains a queue from the VkDevice.
    virtual bool registerQueue(VkQueue queue, VkDevice device,
                                uint32_t queueFamilyIndex) = 0;

    // Called right before the target process destroys its VkDevice, so any
    // device-dependent export resources (fences, command pools, the export
    // image) get released while the handle is still valid -- waiting until
    // this object's own destructor (which only runs at process-exit static
    // destruction, well after the app has already destroyed its device)
    // would call into an already-invalid device.
    virtual void onDeviceDestroyed() = 0;

    // Called when the target process creates/destroys a swapchain, so the
    // capture source knows which VkImages back it (needed to copy out of the
    // image about to be presented).
    virtual void onSwapchainCreated(VkSwapchainKHR swapchain,
                                     const std::vector<VkImage>& images,
                                     VkFormat format, VkExtent2D extent) = 0;
    virtual void onSwapchainDestroyed(VkSwapchainKHR swapchain) = 0;

    // Called for every vkQueuePresentKHR from the target process. `pPresentInfo`
    // points to the caller's own LOCAL COPY (not the original from the app),
    // and may be mutated in place: if this capture source injects a GPU copy
    // between the app's render-complete signal and the real present, it must
    // rewrite pWaitSemaphores/waitSemaphoreCount to chain through its own
    // completion semaphore -- otherwise the real present can run (and the
    // presentation engine can display the image) before the injected copy's
    // layout-restoring barrier has actually finished executing, corrupting
    // the displayed frame.
    virtual bool onQueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo) = 0;

    // The process name that this source is targeting.
    virtual std::string targetProcessName() const = 0;
};

class VulkanLayerCapture : public FrameSource {
public:
    VulkanLayerCapture();
    ~VulkanLayerCapture() override;

    bool init(std::string targetProcess) override;
    void shutdown() override;
    bool isActive() const override;
    bool onInstanceCreated(VkInstance instance) override;
    bool onDeviceCreated(VkDevice device, VkPhysicalDevice physicalDevice,
                          PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties,
                          PFN_vkGetMemoryFdKHR getMemoryFdKHR,
                          PFN_vkGetSemaphoreFdKHR getSemaphoreFdKHR,
                          PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties,
                          PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties) override;
    bool registerQueue(VkQueue queue, VkDevice device, uint32_t queueFamilyIndex) override;
    void onDeviceDestroyed() override;
    void onSwapchainCreated(VkSwapchainKHR swapchain, const std::vector<VkImage>& images,
                             VkFormat format, VkExtent2D extent) override;
    void onSwapchainDestroyed(VkSwapchainKHR swapchain) override;
    bool onQueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo) override;
    std::string targetProcessName() const override;

private:
    struct SwapchainInfo {
        std::vector<VkImage> images;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{0, 0};
    };

    // Best-effort: exports the swapchain image about to be presented, over
    // IPC, to whatever `gmix` consumer is connected. Never blocks the present
    // thread on GPU work or on a stalled consumer -- see comments in the .cpp.
    void maybeExportFrame(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
    bool ensureExportImage(uint32_t w, uint32_t h);
    bool ensureExportSemaphores();
    VkCommandPool ensureCmdPool(uint32_t queueFamilyIndex);
    void destroyExportResources();
    void connectorLoop();

    std::string targetProcess_;
    bool active_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    std::unordered_set<VkQueue> trackedQueues_;
    unsigned long long presentCount_ = 0;
    // IPC server used to notify the external gmix client of present events.
    std::unique_ptr<class LayerIpc> ipc_;

    // ── frame export state ──────────────────────────────────────────────────
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties_ = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFdKHR_ = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFdKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties_ = nullptr;
    std::unordered_map<VkQueue, uint32_t> queueFamilies_;

    // ── GPU-domain frame timing ─────────────────────────────────────────────
    // A timestamp query per ring slot: the capture cmd buffer writes the GPU
    // time the blit finished; we read a completed slot's value back without
    // blocking and send it (as ns) so the consumer can measure the true capture
    // rate free of present-thread scheduling jitter. Disabled (gpuTimestampNs=0)
    // if the present queue family doesn't support timestamps.
    VkQueryPool tsQueryPool_ = VK_NULL_HANDLE;
    double      timestampPeriodNs_ = 0.0;   // ns per GPU timestamp tick
    uint32_t    timestampValidBits_ = 0;
    bool        timestampsSupported_ = false;

    std::mutex swapMu_;
    std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchains_;

    // Guards the whole export critical section below (ring buffer, command
    // pools, export image) -- some apps present from more than one thread,
    // and none of this state was otherwise safe for concurrent access.
    std::mutex exportMu_;

    // RING of export images, NOT a single one. OPAQUE_FD export shares the
    // underlying memory with the consumer's import, so re-exporting one
    // persistent image every frame would make every consumer frame alias the
    // SAME memory (= only the latest pixels) -- the multi-frame blend would
    // then average N identical images and show no motion. Each ring slot is a
    // distinct allocation that retains that frame's pixels until it's cycled
    // back. Must exceed the consumer's blend window (32) plus in-flight margin
    // so a slot is never overwritten while the consumer still references it.
    static constexpr int kExportRing = 48;
    VkImage exportImage_[kExportRing] = {};
    VkDeviceMemory exportMem_[kExportRing] = {};
    // Persistent exported timeline semaphore per export slot. Created ONCE per
    // device (ensureExportSemaphores) -- NOT per frame, and NOT torn down on
    // resize, since semaphores are size-independent. Each export signals its
    // slot's semaphore to an increasing value; the consumer imports each slot's
    // semaphore ONCE and just waits the per-frame value. This removed the
    // per-frame semaphore create/import/destroy that was the consumer's
    // import-throughput ceiling (so it can keep more of osu!'s real frames).
    VkSemaphore exportSem_[kExportRing] = {};
    int exportNext_ = 0;
    uint32_t exportW_ = 0;
    uint32_t exportH_ = 0;
    // Actual row pitch the driver chose for the export images (queried via
    // vkGetImageSubresourceLayout; identical across slots since same dims),
    // sent to the consumer per frame so it can import with the same layout.
    uint64_t exportRowPitch_ = 0;

    std::unordered_map<uint32_t, VkCommandPool> cmdPools_;

    // Small fixed ring of reusable (command buffer, fence) pairs so we never
    // need to allocate-and-leak a new command buffer every present, and never
    // block waiting for one to free up -- if the next slot's fence isn't
    // signaled yet, that present's export is just skipped. Sized with more
    // headroom than the original ~120fps throttle needed, now that
    // kExportInterval targets close to native framerate (up to ~1000fps).
    static constexpr int kRingSize = 8;
    VkCommandBuffer ringCmd_[kRingSize] = {};
    VkFence ringFence_[kRingSize] = {};
    // Binary semaphore per ring slot: our copy submission waits on the app's
    // own original present-wait semaphores and signals this one; the real
    // present is then rewritten to wait on this instead of the app's
    // semaphores, so it can't run ahead of our injected copy + barriers.
    VkSemaphore ringPresentChainSem_[kRingSize] = {};
    bool ringValid_[kRingSize] = {};
    int ringNext_ = 0;
    uint32_t ringPoolFamily_ = ~0u;

    uint64_t frameIndex_ = 0;
    std::chrono::steady_clock::time_point lastExportAttempt_{};

    std::mutex senderMu_;
    std::unique_ptr<gmix::ipc::FrameSender> sender_;
    bool handshakeSent_ = false;
    std::thread connectorThread_;
    std::atomic<bool> connectorRunning_{false};
};

} // namespace capture
} // namespace gmix
