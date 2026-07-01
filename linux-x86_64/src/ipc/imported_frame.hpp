// ─────────────────────────────────────────────────────────────────────────────
// GMix — imported source frame: turns an fd pair received over IPC into a
// real VkImage the blend engine can sample.
//
// The capture layer reuses a FIXED ring of backing buffers (kExportRing) and
// stamps each frame with its export slot. The image memory for a given slot is
// the same underlying object every time it comes round, so the consumer imports
// each slot's image ONCE (FrameImagePool) and reuses it -- avoiding a per-frame
// vkCreateImage / vkAllocateMemory, which was the main cause of latency spikes.
// The producer's per-slot timeline semaphore is ALSO persistent now, so the
// consumer imports it once per slot too (SemaphorePool) -- ImportedFrame just
// borrows the pooled image + pooled semaphore and holds this frame's wait value.
// This removed the per-frame vkCreateSemaphore / vkImportSemaphoreFdKHR /
// vkDestroySemaphore that capped consumer import throughput.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace gmix {

class VulkanContext;

// One imported source image (memory + image + view) for a single producer
// export slot. Owned via shared_ptr so a frame still being blended keeps it
// alive even after the pool rebuilds (e.g. on a resize).
struct PooledImage {
    VulkanContext* vk    = nullptr;
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkImageView    view  = VK_NULL_HANDLE;

    PooledImage() = default;
    ~PooledImage();
    PooledImage(const PooledImage&) = delete;
    PooledImage& operator=(const PooledImage&) = delete;
};

// Per-connection cache of PooledImages keyed by export slot, for the current
// dimensions/format. Rebuilds itself if those change.
class FrameImagePool {
public:
    // Return the pooled image for `slot`, importing `memFd` on first use for
    // that slot (rebuilding the whole pool if w/h/format changed). ALWAYS
    // consumes memFd: it is imported on a miss, or closed on a hit / on failure.
    // Returns nullptr if the import failed.
    std::shared_ptr<PooledImage> acquire(VulkanContext& vk, uint32_t slot, int memFd,
                                          uint32_t w, uint32_t h, VkFormat format,
                                          uint64_t rowPitch);

    // Drop all cached images (call after the connection's GPU work is idle).
    void clear();

private:
    std::vector<std::shared_ptr<PooledImage>> slots_;
    uint32_t w_ = 0, h_ = 0;
    VkFormat fmt_ = VK_FORMAT_UNDEFINED;
};

// One imported timeline semaphore for a single producer export slot. The
// producer's per-slot semaphore is persistent, so we import it ONCE and reuse
// it for every frame on that slot -- only the wait VALUE is per-frame.
struct PooledSemaphore {
    VulkanContext* vk   = nullptr;
    VkSemaphore    sema = VK_NULL_HANDLE;

    PooledSemaphore() = default;
    ~PooledSemaphore();
    PooledSemaphore(const PooledSemaphore&) = delete;
    PooledSemaphore& operator=(const PooledSemaphore&) = delete;
};

// Per-connection cache of imported semaphores keyed by export slot. Unlike the
// image pool it needs no dimension tracking -- the producer's export semaphores
// are size-independent and persist across resizes.
class SemaphorePool {
public:
    // Import `semFd` into a timeline semaphore on first sighting of `slot`,
    // reuse it (closing the redundant fd) afterwards. ALWAYS consumes semFd.
    // Returns nullptr on failure.
    std::shared_ptr<PooledSemaphore> acquire(VulkanContext& vk, uint32_t slot, int semFd);

    // Drop all cached semaphores (call after the connection's GPU work is idle).
    void clear();

private:
    std::vector<std::shared_ptr<PooledSemaphore>> slots_;
};

class ImportedFrame {
public:
    ImportedFrame() = default;
    ~ImportedFrame();

    ImportedFrame(const ImportedFrame&) = delete;
    ImportedFrame& operator=(const ImportedFrame&) = delete;

    // Bind an already-imported (pooled) source image + pooled timeline semaphore
    // and record THIS frame's signal value to wait for. Nothing is owned here --
    // both the image and the semaphore are borrowed (shared) and freed by their
    // pools.
    bool init(VulkanContext& vk, std::shared_ptr<PooledImage> img,
              std::shared_ptr<PooledSemaphore> sem, uint64_t semSignalValue);

    // The producer's render-complete timeline semaphore + the value to wait for.
    // Lets the BLEND submit wait for the producer on the GPU (compute queue)
    // instead of blocking the present thread with a host wait -- keeps producer
    // timing entirely off the pacing-critical path.
    VkSemaphore producerSemaphore() const { return sem_ ? sem_->sema : VK_NULL_HANDLE; }
    uint64_t    producerWaitValue() const { return sigVal_; }

    VkImageView view()    const { return img_ ? img_->view : VK_NULL_HANDLE; }
    VkImage     image()   const { return img_ ? img_->image : VK_NULL_HANDLE; }
    bool        valid()   const { return img_ && img_->image != VK_NULL_HANDLE; }

private:
    VulkanContext*                   vk_     = nullptr;
    std::shared_ptr<PooledImage>     img_;
    std::shared_ptr<PooledSemaphore> sem_;
    uint64_t                         sigVal_ = 0;
};

} // namespace gmix
