// ─────────────────────────────────────────────────────────────────────────────
// VkLayer_GMIX — implicit Vulkan capture layer.
//
// Phase-1 stub: implements the loader negotiation contract so the .so loads
// cleanly and the manifest is discoverable. The actual QueuePresent hook,
// frame copy, fd export, and socket send land in phase 8.
//
// Loader contract reference: a layer .so must export
//   vkNegotiateLoaderLayerInterfaceVersion
// and the JSON "functions" entry names (GMIX_GetInstanceProcAddr /
// GMIX_GetDeviceProcAddr here). The loader calls negotiate first, then
// resolves instance/device procs through those.
// ─────────────────────────────────────────────────────────────────────────────
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "capture/FrameSource.hpp"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The Vulkan headers don't define a generic layer-export macro. Use the
// compiler default visibility so the loader can resolve our entry symbols.
#ifndef VK_LAYER_EXPORT
#  define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

// One global per-instance lock guards the layer state map. Fine for v1 —
// instance creation is not a hot path.
std::mutex g_state_mutex;

// Capture state shared by the layer.
std::unique_ptr<gmix::capture::VulkanLayerCapture> g_capture;
std::string g_captureTargetProcess;

// Trampoline to the next layer/loader. We resolve these on first use per
// instance/device and chain through.
struct InstanceState {
    VkInstance                                  instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr                   getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance                        createInstance = nullptr;
    PFN_vkDestroyInstance                       destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices              enumeratePhysicalDevices = nullptr;
};
struct DeviceState {
    VkDevice                                    device = VK_NULL_HANDLE;
    VkPhysicalDevice                            physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr                     getDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice                         destroyDevice = nullptr;
    PFN_vkQueuePresentKHR                       queuePresentKHR = nullptr;
    PFN_vkGetDeviceQueue                        getDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2                       getDeviceQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR                    createSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR                   destroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR                 getSwapchainImagesKHR = nullptr;
    PFN_vkGetMemoryFdKHR                        getMemoryFdKHR = nullptr;
    PFN_vkGetSemaphoreFdKHR                     getSemaphoreFdKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getSurfaceCapabilitiesKHR = nullptr;
};

std::unordered_map<VkInstance, InstanceState*> g_instances;
std::unordered_map<VkDevice, DeviceState*>     g_devices;
std::unordered_map<VkQueue, VkDevice>          g_queueToDevice;

InstanceState* getInstanceState(VkInstance i) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    auto it = g_instances.find(i);
    return it == g_instances.end() ? nullptr : it->second;
}

DeviceState* getDeviceState(VkDevice d) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    auto it = g_devices.find(d);
    return it == g_devices.end() ? nullptr : it->second;
}

std::string getCurrentProcessName() {
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return {};
    path[len] = '\0';
    const char* base = std::strrchr(path, '/');
    return base ? std::string(base + 1) : std::string(path);
}

bool processMatchesTarget(const std::string& target) {
    if (target.empty()) return true;
    std::string name = getCurrentProcessName();
    return !name.empty() && name.find(target) != std::string::npos;
}

std::string readTargetProcessConfig() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::string path = std::string(home) + "/.config/gmix/target_process";
    std::ifstream in(path);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    return line;
}

VkDevice deviceForQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    auto it = g_queueToDevice.find(queue);
    return it == g_queueToDevice.end() ? VK_NULL_HANDLE : it->second;
}

// Synchronous, single-shot. Must be called with g_state_mutex already held
// by the caller (true for every call site today) so construction of
// g_capture and the `if (g_capture)` reads elsewhere never race -- that race
// (a prior async/threaded construction of this same global pointer) was the
// cause of the segfault this used to be disabled for.
void ensureCaptureInitialized() {
    if (g_capture) return;
    g_captureTargetProcess = readTargetProcessConfig();
    if (g_captureTargetProcess.empty()) return;

    auto capture = std::make_unique<gmix::capture::VulkanLayerCapture>();
    if (capture->init(g_captureTargetProcess)) {
        g_capture = std::move(capture);
    }
}

// ── wrapped instance creation ────────────────────────────────────────────────
VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { fprintf(log, "wrap_CreateInstance called\n"); fclose(log); }
    std::lock_guard<std::mutex> lk(g_state_mutex);

    // Walk the pNext chain looking for the loader's VK_LAYER_LINK_INFO node,
    // which gives us the next layer's GetInstanceProcAddr.
    PFN_vkGetInstanceProcAddr fpGIPA = nullptr;
    for (const void* p = pCreateInfo->pNext; p != nullptr; ) {
        auto* hdr = static_cast<const VkBaseInStructure*>(p);
        auto* lic = reinterpret_cast<const VkLayerInstanceCreateInfo*>(p);
        if (hdr->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            lic->function == VK_LAYER_LINK_INFO) {
            fpGIPA = lic->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            // Unhook our entry so the next layer/driver links cleanly.
            const_cast<VkLayerInstanceCreateInfo*>(lic)->u.pLayerInfo =
                lic->u.pLayerInfo->pNext;
            break;
        }
        p = hdr->pNext;
    }
    if (!fpGIPA) return VK_ERROR_INITIALIZATION_FAILED;

    auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        fpGIPA(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    auto* st = new InstanceState();
    st->instance = *pInstance;
    st->getInstanceProcAddr = fpGIPA;
    st->createInstance = fpCreateInstance;
    st->destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        fpGIPA(*pInstance, "vkDestroyInstance"));
    st->enumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        fpGIPA(*pInstance, "vkEnumeratePhysicalDevices"));
    g_instances[*pInstance] = st;

    ensureCaptureInitialized();
    if (g_capture && processMatchesTarget(g_captureTargetProcess)) {
        g_capture->onInstanceCreated(*pInstance);
    }

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { 
        fprintf(log, "wrap_CreateDevice called\n");
        fflush(log); 
        fclose(log); 
    }
    std::lock_guard<std::mutex> lk(g_state_mutex);

    // Walk pNext for loader link info (device-level). VK_STRUCTURE_TYPE_
    // LOADER_DEVICE_CREATE_INFO is shared by VK_LOADER_DATA_CALLBACK,
    // VK_LAYER_LINK_INFO, and VK_LOADER_FEATURES nodes, each with a different
    // active member of the `u` union -- must also check `function` before
    // touching `u.pLayerInfo`, or we read garbage from the wrong union member.
    const VkLayerDeviceCreateInfo* deviceLink = nullptr;
    for (const void* p = pCreateInfo->pNext; p != nullptr; ) {
        auto* hdr = static_cast<const VkBaseInStructure*>(p);
        if (hdr->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* ldci = reinterpret_cast<const VkLayerDeviceCreateInfo*>(p);
            if (ldci->function == VK_LAYER_LINK_INFO) {
                deviceLink = ldci;
                break;
            }
        }
        p = hdr->pNext;
    }
    
    // If no device link, just pass through directly (layer might not be in device chain)
    if (!deviceLink) {
        if (log = fopen("/tmp/gmix_layer_debug.log", "a")) {
            fprintf(log, "  no device link info, calling next vkCreateDevice directly\n");
            fflush(log); fclose(log);
        }
        // Call the function directly from the trampoline - it will find the driver
        // This works because we're part of the implicit layer chain on instance creation
        return vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    // We have link info - use it properly
    auto fpGIPA = deviceLink->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto fpGDPA = deviceLink->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const_cast<VkLayerDeviceCreateInfo*>(deviceLink)->u.pLayerInfo = 
        deviceLink->u.pLayerInfo->pNext;

    auto fpCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        fpGIPA(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!fpCreateDevice) {
        if (log = fopen("/tmp/gmix_layer_debug.log", "a")) {
            fprintf(log, "  ERROR: failed to get vkCreateDevice from next layer\n");
            fflush(log); fclose(log);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // ── Transparently inject what our own frame export needs ───────────────
    // The target app (e.g. osu!) has no reason to request
    // VK_KHR_external_memory_fd / VK_KHR_external_semaphore_fd or enable the
    // timelineSemaphore feature itself. Splice them into a local copy of its
    // own VkDeviceCreateInfo before forwarding to the real driver -- the
    // same technique overlay/capture layers (Steam, OBS, etc.) use to add
    // capability the app didn't ask for, as long as the driver supports it.
    // We never remove or replace anything the app already requested.
    std::vector<const char*> augmentedExts(
        pCreateInfo->ppEnabledExtensionNames,
        pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    {
        // vkEnumerateDeviceExtensionProperties dispatches via the instance,
        // not a bare global symbol -- same reasoning as the
        // vkGetPhysicalDeviceMemoryProperties fix below: calling it as a
        // bare symbol from inside this injected layer hits the loader's own
        // terminator validation, which doesn't recognize the physicalDevice
        // handle that way and aborts.
        VkInstance anyInstanceForExtQuery = VK_NULL_HANDLE;
        if (!g_instances.empty()) anyInstanceForExtQuery = g_instances.begin()->second->instance;
        auto pEnumDeviceExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            fpGIPA(anyInstanceForExtQuery, "vkEnumerateDeviceExtensionProperties"));
        uint32_t devExtCount = 0;
        std::vector<VkExtensionProperties> devExts;
        if (pEnumDeviceExts) {
            pEnumDeviceExts(physicalDevice, nullptr, &devExtCount, nullptr);
            devExts.resize(devExtCount);
            pEnumDeviceExts(physicalDevice, nullptr, &devExtCount, devExts.data());
        }
        auto deviceSupports = [&](const char* name) {
            for (auto& e : devExts) if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        auto alreadyRequested = [&](const char* name) {
            for (auto* e : augmentedExts) if (std::strcmp(e, name) == 0) return true;
            return false;
        };
        static const char* kWanted[] = {
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        };
        for (const char* want : kWanted) {
            if (!alreadyRequested(want) && deviceSupports(want)) augmentedExts.push_back(want);
        }
    }

    VkDeviceCreateInfo augmentedInfo = *pCreateInfo;
    augmentedInfo.enabledExtensionCount = static_cast<uint32_t>(augmentedExts.size());
    augmentedInfo.ppEnabledExtensionNames = augmentedExts.data();

    // Ensure the timelineSemaphore *feature* is enabled, not just the
    // extension string -- walk the app's own pNext chain for an existing
    // VkPhysicalDeviceVulkan12Features or VkPhysicalDeviceTimelineSemaphore
    // Features node and patch it in place if found (mutating the app's own
    // struct, same pattern already used for the loader-link pNext nodes
    // above); otherwise splice in a new node ahead of whatever pNext the
    // app already set, preserving it.
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.timelineSemaphore = VK_TRUE;
    bool foundTimelineFeature = false;
    for (const void* p = pCreateInfo->pNext; p != nullptr; ) {
        auto* hdr = static_cast<const VkBaseInStructure*>(p);
        if (hdr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            const_cast<VkPhysicalDeviceVulkan12Features*>(
                reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p))->timelineSemaphore = VK_TRUE;
            foundTimelineFeature = true;
            break;
        }
        if (hdr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            const_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(
                reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(p))->timelineSemaphore = VK_TRUE;
            foundTimelineFeature = true;
            break;
        }
        p = hdr->pNext;
    }
    if (!foundTimelineFeature) {
        timelineFeatures.pNext = const_cast<void*>(pCreateInfo->pNext);
        augmentedInfo.pNext = &timelineFeatures;
    }

    VkResult r = fpCreateDevice(physicalDevice, &augmentedInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) {
        if (log = fopen("/tmp/gmix_layer_debug.log", "a")) {
            fprintf(log, "  vkCreateDevice failed: %d\n", r);
            fflush(log); fclose(log);
        }
        return r;
    }

    // Track device for present interception
    auto* ds = new DeviceState();
    ds->device = *pDevice;
    ds->getDeviceProcAddr = fpGDPA;
    ds->destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
        fpGDPA(*pDevice, "vkDestroyDevice"));
    ds->queuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
        fpGDPA(*pDevice, "vkQueuePresentKHR"));
    ds->getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        fpGDPA(*pDevice, "vkGetDeviceQueue"));
    ds->getDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        fpGDPA(*pDevice, "vkGetDeviceQueue2"));
    ds->createSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        fpGDPA(*pDevice, "vkCreateSwapchainKHR"));
    ds->destroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        fpGDPA(*pDevice, "vkDestroySwapchainKHR"));
    ds->getSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        fpGDPA(*pDevice, "vkGetSwapchainImagesKHR"));
    ds->getMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        fpGDPA(*pDevice, "vkGetMemoryFdKHR"));
    ds->getSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        fpGDPA(*pDevice, "vkGetSemaphoreFdKHR"));
    ds->physicalDevice = physicalDevice;
    {
        // Instance-level function; needs a real instance handle (see the
        // vkGetPhysicalDeviceMemoryProperties fetch below for why).
        VkInstance anyInst = VK_NULL_HANDLE;
        if (!g_instances.empty()) anyInst = g_instances.begin()->second->instance;
        ds->getSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            fpGIPA(anyInst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    }
    g_devices[*pDevice] = ds;

    if (g_capture && processMatchesTarget(g_captureTargetProcess)) {
        // vkGetPhysicalDeviceMemoryProperties is instance-level; fetch it via
        // the next layer's GetInstanceProcAddr (already have fpGIPA in scope)
        // rather than ever calling it as a bare global symbol from within
        // this injected layer. Unlike vkCreateDevice, this loader does NOT
        // resolve it for a NULL instance -- a real instance handle is needed.
        // g_state_mutex is already held by this function's own lock_guard
        // (declared at the top of wrap_CreateDevice), so read g_instances
        // directly -- re-locking the same non-recursive mutex here would
        // deadlock.
        VkInstance anyInstance = VK_NULL_HANDLE;
        if (!g_instances.empty()) anyInstance = g_instances.begin()->second->instance;
        auto getPhysDevMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            fpGIPA(anyInstance, "vkGetPhysicalDeviceMemoryProperties"));
        // Same instance-level fetch for the timestamp-timing queries.
        auto getPhysDevProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            fpGIPA(anyInstance, "vkGetPhysicalDeviceProperties"));
        auto getPhysDevQFProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            fpGIPA(anyInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        g_capture->onDeviceCreated(*pDevice, physicalDevice, getPhysDevMemProps,
                                   ds->getMemoryFdKHR, ds->getSemaphoreFdKHR,
                                   getPhysDevProps, getPhysDevQFProps);
    }

    return VK_SUCCESS;
}


VKAPI_ATTR void VKAPI_CALL wrap_DestroyDevice(
    VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    DeviceState* st = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            st = it->second;
            g_devices.erase(it);
        }
    }
    // Release any device-dependent capture/export resources while `device`
    // is still valid -- g_capture's own destructor only runs at process-exit
    // static destruction, well after the real device is gone.
    if (g_capture) g_capture->onDeviceDestroyed();
    if (st && st->destroyDevice) st->destroyDevice(device, pAllocator);
    delete st;
}

void recordQueueDevice(VkQueue queue, VkDevice device) {
    if (queue == VK_NULL_HANDLE || device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_queueToDevice[queue] = device;
}

VKAPI_ATTR void VKAPI_CALL wrap_GetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue)
{
    DeviceState* st = getDeviceState(device);
    if (!st || !st->getDeviceQueue) return;
    st->getDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        recordQueueDevice(*pQueue, device);
        std::fprintf(stderr, "VkLayer_GMIX: recorded queue %p -> device %p (GetDeviceQueue)\n",
                     static_cast<void*>(*pQueue), static_cast<void*>(device));
        if (g_capture && g_capture->isActive()) {
            g_capture->registerQueue(*pQueue, device, queueFamilyIndex);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL wrap_GetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue)
{
    DeviceState* st = getDeviceState(device);
    if (!st || !st->getDeviceQueue2) return;
    st->getDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        recordQueueDevice(*pQueue, device);
        std::fprintf(stderr, "VkLayer_GMIX: recorded queue %p -> device %p (GetDeviceQueue2)\n",
                     static_cast<void*>(*pQueue), static_cast<void*>(device));
        if (g_capture && g_capture->isActive()) {
            g_capture->registerQueue(*pQueue, device, pQueueInfo->queueFamilyIndex);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    DeviceState* st = getDeviceState(device);
    if (!st || !st->createSwapchainKHR) return VK_ERROR_DEVICE_LOST;

    // Our capture needs to vkCmdBlitImage *out of* the swapchain image, which
    // requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT -- apps have no reason to
    // request that themselves. Inject it into a local copy of the app's own
    // VkSwapchainCreateInfoKHR if the surface actually supports it (querying
    // unsupported usage would make swapchain creation fail outright).
    VkSwapchainCreateInfoKHR augmented = *pCreateInfo;
    if (!(augmented.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && st->getSurfaceCapabilitiesKHR) {
        VkSurfaceCapabilitiesKHR caps{};
        if (st->getSurfaceCapabilitiesKHR(st->physicalDevice, pCreateInfo->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            augmented.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
    }

    VkResult r = st->createSwapchainKHR(device, &augmented, pAllocator, pSwapchain);
    if (r != VK_SUCCESS) return r;

    if (g_capture && st->getSwapchainImagesKHR && pSwapchain && *pSwapchain != VK_NULL_HANDLE) {
        uint32_t count = 0;
        st->getSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        std::vector<VkImage> images(count);
        if (count > 0) st->getSwapchainImagesKHR(device, *pSwapchain, &count, images.data());
        g_capture->onSwapchainCreated(*pSwapchain, images, pCreateInfo->imageFormat,
                                      pCreateInfo->imageExtent);
        std::fprintf(stderr, "VkLayer_GMIX: swapchain created %p extent=%ux%u images=%u oldSwapchain=%p\n",
                     (void*)*pSwapchain, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                     count, (void*)pCreateInfo->oldSwapchain);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL wrap_DestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    if (swapchain != VK_NULL_HANDLE)
        std::fprintf(stderr, "VkLayer_GMIX: swapchain destroyed %p\n", (void*)swapchain);
    if (g_capture) g_capture->onSwapchainDestroyed(swapchain);
    DeviceState* st = getDeviceState(device);
    if (st && st->destroySwapchainKHR) st->destroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL wrap_QueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    static int presentCount = 0;
    if (presentCount++ % 60 == 0) {
        FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
        if (log) { fprintf(log, "wrap_QueuePresentKHR called (#%d)\n", presentCount); fclose(log); }
    }

    // Local mutable copy: onQueuePresent may rewrite the wait-semaphore
    // fields to chain the real present through its own injected GPU work
    // (see FrameSource.hpp). The app's own pPresentInfo is never modified.
    VkPresentInfoKHR localInfo = *pPresentInfo;
    if (g_capture && g_capture->isActive()) {
        g_capture->onQueuePresent(queue, &localInfo);
    }

    VkDevice device = deviceForQueue(queue);
    if (device == VK_NULL_HANDLE) return VK_ERROR_DEVICE_LOST;
    DeviceState* st = getDeviceState(device);
    if (!st || !st->queuePresentKHR) return VK_ERROR_DEVICE_LOST;
    return st->queuePresentKHR(queue, &localInfo);
}

VKAPI_ATTR void VKAPI_CALL wrap_DestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    InstanceState* st;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        auto it = g_instances.find(instance);
        if (it == g_instances.end()) return;
        st = it->second;
        g_instances.erase(it);
    }
    if (st->destroyInstance) st->destroyInstance(instance, pAllocator);
    delete st;
}

// ── GetInstanceProcAddr dispatch ─────────────────────────────────────────────
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getProc(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkCreateInstance")  == 0) return reinterpret_cast<PFN_vkVoidFunction>(wrap_CreateInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(wrap_DestroyInstance);
    // vkCreateDevice must be intercepted here (not via GetDeviceProcAddr --
    // there's no VkDevice yet when it's called) so a DeviceState gets
    // created; without it, wrap_GetDeviceQueue/wrap_QueuePresentKHR below
    // silently no-op on a missing DeviceState and leave the caller's handle
    // uninitialized.
    if (std::strcmp(name, "vkCreateDevice")    == 0) return reinterpret_cast<PFN_vkVoidFunction>(wrap_CreateDevice);
    return nullptr;
}


} // namespace

// ─── Exported proc dispatchers (must be in extern "C" for proper symbol export)
extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GMIX_GetInstanceProcAddr(
    VkInstance instance, const char* pName)
{
    FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { 
        fprintf(log, "GMIX_GetInstanceProcAddr: pName=%s START\n", pName ? pName : "NULL");
        fflush(log);
        fclose(log); 
    }
    if (auto w = getProc(pName)) {
        log = fopen("/tmp/gmix_layer_debug.log", "a");
        if (log) { fprintf(log, "GMIX_GetInstanceProcAddr: pName=%s (wrapped)\n", pName ? pName : "NULL"); fclose(log); }
        return w;
    }
    // Pass-through everything else to the next layer/driver.
    if (InstanceState* st = getInstanceState(instance)) {
        auto result = st->getInstanceProcAddr(instance, pName);
        log = fopen("/tmp/gmix_layer_debug.log", "a");
        if (log) { fprintf(log, "GMIX_GetInstanceProcAddr: pName=%s END (result=%p)\n", pName ? pName : "NULL", result); fclose(log); }
        return result;
    }
    log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { fprintf(log, "GMIX_GetInstanceProcAddr: pName=%s (no state)\n", pName ? pName : "NULL"); fclose(log); }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GMIX_GetDeviceProcAddr(
    VkDevice device, const char* pName)
{
    FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { fprintf(log, "GMIX_GetDeviceProcAddr: pName=%s\n", pName ? pName : "NULL"); fclose(log); }
    if (!pName) return nullptr;
    // Log all device proc requests for debugging
    FILE* log2 = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log2) { fprintf(log2, "GMIX_GetDeviceProcAddr: requesting=%s\n", pName); fclose(log2); }
    
    // Provide our wrappers for functions we intercept.
    if (std::strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_CreateDevice);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_DestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_GetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_GetDeviceQueue2);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_QueuePresentKHR);
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_CreateSwapchainKHR);
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(wrap_DestroySwapchainKHR);

    // If we have a DeviceState for this device, forward to that chain.
    if (DeviceState* st = getDeviceState(device)) {
        if (st->getDeviceProcAddr)
            return st->getDeviceProcAddr(device, pName);
        return nullptr;
    }

    // During device creation the DeviceState may not yet exist. Try to
    // resolve the next layer's vkGetDeviceProcAddr via any known
    // InstanceState's getInstanceProcAddr and forward the lookup.
    InstanceState* ist = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (!g_instances.empty()) ist = g_instances.begin()->second;
    }
    if (ist && ist->getInstanceProcAddr) {
        auto fp = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            ist->getInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr"));
        if (fp) return fp(device, pName);
    }

    // Last resort: dlsym(RTLD_NEXT) to reach the next layer directly.
    // This is needed if the InstanceState chain isn't available yet during device creation.
    dlerror();  // Clear any previous error
    void* sym = dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    const char* dlErr = dlerror();
    if (sym && !dlErr) {
        // Cast to the correct function signature
        typedef PFN_vkVoidFunction (*pfnGetDeviceProcAddr)(VkDevice, const char*);
        pfnGetDeviceProcAddr nextFunc = reinterpret_cast<pfnGetDeviceProcAddr>(sym);
        return nextFunc(device, pName);
    }

    return nullptr;
}

// ── Loader negotiation entry ─────────────────────────────────────────────────
// The loader calls this to learn which interface version we support and to
// receive our GetInstanceProcAddr/GetDeviceProcAddr.

VK_LAYER_EXPORT VkResult VKAPI_CALL
GMIX_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersion) {
    FILE* log = fopen("/tmp/gmix_layer_debug.log", "a");
    if (log) { fprintf(log, "GMIX_NegotiateLoaderLayerInterfaceVersion called\n"); fclose(log); }
    if (!pVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersion->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    pVersion->loaderLayerInterfaceVersion = 2;
    pVersion->pfnGetInstanceProcAddr      = GMIX_GetInstanceProcAddr;
    pVersion->pfnGetDeviceProcAddr        = GMIX_GetDeviceProcAddr;
    pVersion->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersion) {
    return GMIX_NegotiateLoaderLayerInterfaceVersion(pVersion);
}

// Bare symbol exports referenced by older loaders / the manifest "functions".
VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return GMIX_GetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return GMIX_GetDeviceProcAddr(device, pName);
}

} // extern "C"
