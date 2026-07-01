// ─────────────────────────────────────────────────────────────────────────────
// GMix Vulkan context — owns the consumer-side VkInstance/Device and exposes
// physical device enumeration + selection.
//
// enumerateAndPrint() produces the spec'd startup output:
//
//     vulkan device found
//     [gpu0] AMD Radeon RX 480 Graphics (RADV POLARIS10)
//     [gpu1] llvmpipe (LLVM 19.1.7, 256 bits)
//     using [gpu0]
//
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>
#include <cstdint>

namespace gmix {

struct GpuInfo {
    uint32_t              index = 0;
    VkPhysicalDevice      handle = VK_NULL_HANDLE;
    std::string           name;
    VkPhysicalDeviceType  type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t              apiVersion = 0;
    uint32_t              queueFamily = 0;   // graphics+compute family
    bool                  hasCompute = false;
    // A separate (async) compute family, when one exists, so the blend can run
    // on its own hardware queue without blocking the present blit on the
    // graphics queue. Falls back to `queueFamily` when there's no distinct one.
    uint32_t              computeFamily = 0;
    bool                  hasAsyncCompute = false;
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Create the instance and enumerate GPUs. Prints the device list and the
    // selected device to stdout (matches the spec). Returns false if no usable
    // Vulkan device was found. `preferIndex` = -1 → auto (discrete first).
    // `headless` = true skips the Wayland-surface instance extensions and the
    // swapchain/present_id/present_wait device extensions -- for a consumer
    // (e.g. the OBS plugin) that never presents to a window and may run with
    // no Wayland connection at all.
    bool init(int preferIndex, bool headless = false);

    // Accessors used by the blend engine / presenter.
    VkInstance       instance()       const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return selected_.handle; }
    VkDevice         device()         const { return device_; }
    VkQueue          queue()          const { return queue_; }
    uint32_t         queueFamily()    const { return selected_.queueFamily; }
    // Async-compute queue for the blend. Equal to queue()/queueFamily() when the
    // device exposes no distinct compute family (then the pipeline degrades to
    // single-queue but stays correct).
    VkQueue          computeQueue()       const { return computeQueue_; }
    uint32_t         computeQueueFamily() const { return selected_.computeFamily; }
    bool             hasAsyncCompute()    const { return selected_.hasAsyncCompute; }
    // True when VK_KHR_present_id + VK_KHR_present_wait are enabled, so the
    // present loop can pace itself off vkWaitForPresentKHR (vblank-accurate).
    bool             presentWaitEnabled() const { return presentWaitEnabled_; }
    const GpuInfo&   selected()       const { return selected_; }
    const std::vector<GpuInfo>& gpus() const { return gpus_; }

    // Lazy accessor for extension function pointers the engine needs.
    PFN_vkGetMemoryFdKHR            getMemoryFdKHR();
    PFN_vkGetSemaphoreFdKHR         getSemaphoreFdKHR();
    PFN_vkImportSemaphoreFdKHR      importSemaphoreFdKHR();
    
    // Find a memory type index suitable for the given requirements and flags.
    uint32_t findMemoryType(uint32_t memoryTypeBits, VkMemoryPropertyFlags flags);

private:
    bool enumerateGpus();
    bool pickGpu(int preferIndex);
    bool createDevice(bool headless);

    VkInstance       instance_  = VK_NULL_HANDLE;
    VkDevice         device_    = VK_NULL_HANDLE;
    VkQueue          queue_     = VK_NULL_HANDLE;
    VkQueue          computeQueue_ = VK_NULL_HANDLE;  // == queue_ if no async family
    bool             presentWaitEnabled_ = false;
    std::vector<GpuInfo> gpus_;
    GpuInfo          selected_;

    PFN_vkGetMemoryFdKHR        pGetMemoryFdKHR_        = nullptr;
    PFN_vkGetSemaphoreFdKHR     pGetSemaphoreFdKHR_     = nullptr;
    PFN_vkImportSemaphoreFdKHR  pImportSemaphoreFdKHR_  = nullptr;
};

} // namespace gmix
