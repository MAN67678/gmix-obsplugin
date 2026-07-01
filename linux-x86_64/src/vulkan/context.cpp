#include "context.hpp"

#include <vulkan/vulkan_wayland.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace gmix {

namespace {

const char* deviceTypeLabel(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    default:                                     return "other";
    }
}

bool hasQueueFamily(VkQueueFlags flags, VkQueueFlags want) {
    return (flags & want) == want;
}

} // namespace

VulkanContext::~VulkanContext() {
    if (device_)   vkDestroyDevice(device_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanContext::init(int preferIndex, bool headless) {
    // ── Instance ─────────────────────────────────────────────────────────────
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "gmix";
    ai.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    ai.pEngineName = "gmix";
    ai.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    ai.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    // WSI surface extensions — required for the Wayland output window. Skipped
    // headless: a plugin consumer never presents and may have no Wayland
    // connection to query at all.
    const char* instExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    if (!headless) {
        ci.enabledExtensionCount   = 2;
        ci.ppEnabledExtensionNames = instExts;
    }

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: vkCreateInstance failed (%d)\n", r);
        return false;
    }

    if (!enumerateGpus()) return false;
    if (!pickGpu(preferIndex)) return false;
    if (!createDevice(headless)) return false;

    // Extension fn pointers for cross-process fd passing (lazy lookup).
    pGetMemoryFdKHR_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    pGetSemaphoreFdKHR_ = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
    pImportSemaphoreFdKHR_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
    return true;
}

bool VulkanContext::enumerateGpus() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        std::fprintf(stderr, "gmix: no Vulkan-capable GPUs found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> handles(count);
    vkEnumeratePhysicalDevices(instance_, &count, handles.data());

    gpus_.clear();
    gpus_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        GpuInfo g{};
        g.index = i;
        g.handle = handles[i];

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(handles[i], &props);
        g.name = props.deviceName;
        g.type = props.deviceType;
        g.apiVersion = props.apiVersion;

        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handles[i], &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(handles[i], &qfc, qf.data());

        // Prefer a family with both COMPUTE and GRAPHICS (we use compute
        // shaders; graphics queue is required for the Wayland swapchain).
        for (uint32_t q = 0; q < qfc; ++q) {
            if (hasQueueFamily(qf[q].queueFlags,
                               VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) {
                g.queueFamily = q;
                g.hasCompute = true;
                break;
            }
        }
        // Fallback: compute-only family.
        if (!g.hasCompute) {
            for (uint32_t q = 0; q < qfc; ++q) {
                if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    g.queueFamily = q;
                    g.hasCompute = true;
                    break;
                }
            }
        }

        // Prefer a DEDICATED async-compute family (compute, no graphics) for the
        // blend so it runs on its own hardware queue and a slow blend can't block
        // the present blit on the graphics queue. Fall back to the graphics
        // family (single-queue, still correct) if there isn't one.
        g.computeFamily = g.queueFamily;
        for (uint32_t q = 0; q < qfc; ++q) {
            if (q != g.queueFamily && qf[q].queueCount > 0 &&
                (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                !(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                g.computeFamily = q;
                g.hasAsyncCompute = true;
                break;
            }
        }
        gpus_.push_back(g);
    }
    return true;
}

bool VulkanContext::pickGpu(int preferIndex) {
    // Spec'd output.
    std::printf("vulkan device found\n");
    for (const auto& g : gpus_) {
        std::printf("[gpu%u] %s\n", g.index, g.name.c_str());
    }

    const GpuInfo* choice = nullptr;

    if (preferIndex >= 0) {
        for (const auto& g : gpus_)
            if ((int)g.index == preferIndex) { choice = &g; break; }
        if (!choice) {
            std::fprintf(stderr, "gmix: -gpu=%d not found\n", preferIndex);
            return false;
        }
    } else {
        // Auto: prefer discrete, then integrated, then anything with compute.
        auto rank = [](const GpuInfo& g) {
            if (!g.hasCompute) return 99;
            switch (g.type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 3;
            default:                                     return 4;
            }
        };
        const GpuInfo* best = nullptr;
        int bestRank = 100;
        for (const auto& g : gpus_) {
            int r = rank(g);
            if (r < bestRank) { bestRank = r; best = &g; }
        }
        choice = best;
    }

    if (!choice || !choice->hasCompute) {
        std::fprintf(stderr, "gmix: no GPU with a compute queue\n");
        return false;
    }
    selected_ = *choice;
    std::printf("using [gpu%u]  (%s)\n",
                selected_.index, deviceTypeLabel(selected_.type));
    return true;
}

bool VulkanContext::createDevice(bool headless) {
    std::fprintf(stderr, "gmix: createDevice() called\n");
    float prio = 1.0f;
    // Graphics+present queue, and (when available) a separate async-compute
    // queue for the blend.
    std::vector<VkDeviceQueueCreateInfo> qis;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = selected_.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    qis.push_back(qi);
    if (selected_.hasAsyncCompute) {
        VkDeviceQueueCreateInfo qc{};
        qc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qc.queueFamilyIndex = selected_.computeFamily;
        qc.queueCount = 1;
        qc.pQueuePriorities = &prio;
        qis.push_back(qc);
    }

    // External memory + timeline semaphores are core in 1.2, but the
    // _KHR names need to be requested explicitly on some loaders.
    //
    // NOTE: the base VK_KHR_external_memory/external_semaphore extensions
    // only define the generic concept (handle-type enums etc); the actual
    // vkGetMemoryFdKHR/vkGetSemaphoreFdKHR/vkImportSemaphoreFdKHR entry
    // points come from the separate _fd extensions below, which are always
    // extension-gated (never promoted to core) -- without them the
    // GetDeviceProcAddr lookups for those functions are not guaranteed to
    // resolve, and exporting/importing frame fds silently has no effect.
    std::vector<const char*> exts = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
        // dma-buf export: hand blend frames to PipeWire/OBS as zero-copy GPU
        // buffers (no host readback). Support-filtered below; RADV/Polaris has
        // it (but NOT VK_EXT_image_drm_format_modifier, so use LINEAR tiling).
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    // WSI: swapchain + present pacing are only meaningful for a consumer that
    // presents to a window. Skipped headless (no Wayland surface exists to
    // present to, and requesting them needlessly enables present_id/
    // present_wait device features that will never be used).
    if (!headless) {
        exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }

    // Filter to extensions actually supported by this device.
    uint32_t dcount = 0;
    vkEnumerateDeviceExtensionProperties(selected_.handle, nullptr, &dcount, nullptr);
    std::vector<VkExtensionProperties> avail(dcount);
    vkEnumerateDeviceExtensionProperties(selected_.handle, nullptr, &dcount, avail.data());
    auto supported = [&](const char* n) {
        for (auto& e : avail) if (std::strcmp(e.extensionName, n) == 0) return true;
        return false;
    };
    std::vector<const char*> want;
    for (const char* e : exts) if (supported(e)) want.push_back(e);

    // Present-pacing is only usable if BOTH extensions are present (present_wait
    // depends on present_id to name the present to wait for).
    presentWaitEnabled_ = supported(VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                          supported(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (headless) presentWaitEnabled_ = false;  // not requested/enabled above

    VkPhysicalDeviceTimelineSemaphoreFeatures tl{};
    tl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    tl.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &tl;

    // Enable the present_id / present_wait features when available (must be
    // requested explicitly, like timelineSemaphore above).
    VkPhysicalDevicePresentIdFeaturesKHR pidF{};
    pidF.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    pidF.presentId = VK_TRUE;
    VkPhysicalDevicePresentWaitFeaturesKHR pwF{};
    pwF.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    pwF.presentWait = VK_TRUE;
    if (presentWaitEnabled_) {
        pidF.pNext = &pwF;
        pwF.pNext = tl.pNext;   // chain after the existing features
        tl.pNext = &pidF;
    }

    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = &f2;
    di.queueCreateInfoCount = static_cast<uint32_t>(qis.size());
    di.pQueueCreateInfos = qis.data();
    di.enabledExtensionCount = static_cast<uint32_t>(want.size());
    di.ppEnabledExtensionNames = want.data();

    // Call vkCreateDevice directly - the layer will intercept via the trampoline
    VkResult r = vkCreateDevice(selected_.handle, &di, nullptr, &device_);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: vkCreateDevice failed (%d)\n", r);
        return false;
    }
    vkGetDeviceQueue(device_, selected_.queueFamily, 0, &queue_);
    if (selected_.hasAsyncCompute) {
        vkGetDeviceQueue(device_, selected_.computeFamily, 0, &computeQueue_);
        std::fprintf(stderr, "gmix: using async-compute queue (family %u) for the blend\n",
                     selected_.computeFamily);
    } else {
        computeQueue_ = queue_;   // single-queue fallback
    }
    std::fprintf(stderr, "gmix: present pacing: %s\n",
                 presentWaitEnabled_ ? "vkWaitForPresentKHR (vblank-locked)"
                                     : "CPU timer (present_wait unavailable)");
    return true;
}

PFN_vkGetMemoryFdKHR        VulkanContext::getMemoryFdKHR()        { return pGetMemoryFdKHR_; }
PFN_vkGetSemaphoreFdKHR     VulkanContext::getSemaphoreFdKHR()     { return pGetSemaphoreFdKHR_; }
PFN_vkImportSemaphoreFdKHR  VulkanContext::importSemaphoreFdKHR()  { return pImportSemaphoreFdKHR_; }

uint32_t VulkanContext::findMemoryType(uint32_t memoryTypeBits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(selected_.handle, &props);
    
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((memoryTypeBits & (1 << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    
    return 0;  // fallback (shouldn't happen)
}

} // namespace gmix
