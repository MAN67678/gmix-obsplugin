// GPU memory bandwidth sanity benchmark. Exercises the same class of
// GPU-side copy the zero-copy capture pipeline relies on (device-to-device
// copy between device-local images on a compute queue -- no CPU, no
// readback), and reports the effective read+write bandwidth to stdout so a
// ctest run doubles as a "how fast is this GPU's memory" measurement:
//
//     gpu_bandwidth: 2x 4096x4096 RGBA8 device-local, 60 copies in one submit
//     gpu_bandwidth: 97.4 GB/s (read+write, 4096x4096 RGBA8)
//
// Not a substitute for profiling (no pipelining across the fence wait), but
// every GPU this pipeline runs on should be orders of magnitude above the
// assertion floor, so a regression in device selection (e.g. silently ending
// up on llvmpipe) is caught immediately.
#include "vulkan/context.hpp"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #cond);                                               \
            ++failures;                                                      \
        }                                                                     \
    } while (0)

static double secondsBetween(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

int main()
{
    gmix::VulkanContext vk;
    if (!vk.init(-1, /*headless=*/true)) {
        std::fprintf(stderr, "no usable Vulkan device -- cannot benchmark\n");
        return 1;
    }
    VkDevice dev = vk.device();

    constexpr uint32_t W = 4096, H = 4096, BPP = 4;
    constexpr uint64_t imageBytes = uint64_t(W) * H * BPP;   // 64 MiB
    constexpr int kCopies = 60;   // ~15 GiB of traffic in the timed phase

    // ── two device-local RGBA8 transfer-capable images ──────────────────────
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { W, H, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage imgs[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory mems[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (int i = 0; i < 2; ++i) {
        if (vkCreateImage(dev, &ici, nullptr, &imgs[i]) != VK_SUCCESS) { ++failures; break; }
        VkMemoryRequirements mrq{};
        vkGetImageMemoryRequirements(dev, imgs[i], &mrq);

        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice(), &mp);
        uint32_t idx = ~0u;
        for (uint32_t m = 0; m < mp.memoryTypeCount; ++m)
            if ((mrq.memoryTypeBits & (1u << m)) &&
                (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                idx = m;
                break;
            }
        CHECK(idx != ~0u);
        if (idx == ~0u) break;

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mrq.size;
        mai.memoryTypeIndex = idx;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS ||
            vkBindImageMemory(dev, imgs[i], mem, 0) != VK_SUCCESS) {
            ++failures;
            vkFreeMemory(dev, mem, nullptr);
            break;
        }
        mems[i] = mem;
    }
    if (imgs[0] == VK_NULL_HANDLE || imgs[1] == VK_NULL_HANDLE) {
        for (int i = 0; i < 2; ++i) {
            if (imgs[i] != VK_NULL_HANDLE) vkDestroyImage(dev, imgs[i], nullptr);
            if (mems[i] != VK_NULL_HANDLE) vkFreeMemory(dev, mems[i], nullptr);
        }
        return 1;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = vk.computeQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool) == VK_SUCCESS);

    VkCommandBufferBeginInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // Record `copies` alternating full-image copies (so no command ever reads
    // what the immediately preceding command wrote) in one command buffer,
    // then submit on the compute queue and fence-wait for completion.
    auto runCopies = [&](int copies) -> bool {
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) return false;
        if (vkBeginCommandBuffer(cmd, &bci) != VK_SUCCESS) { vkFreeCommandBuffers(dev, pool, 1, &cmd); return false; }

        // UNDEFINED -> GENERAL for both images up front; GENERAL is valid for
        // BOTH the source and the destination of vkCmdCopyImage.
        VkImageMemoryBarrier init[2] = {};
        for (int i = 0; i < 2; ++i) {
            init[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            init[i].srcAccessMask = 0;
            init[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            init[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            init[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            init[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            init[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            init[i].image = imgs[i];
            init[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, init);

        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { W, H, 1 };
        for (int i = 0; i < copies; ++i) {
            VkImage src = (i % 2 == 0) ? imgs[0] : imgs[1];
            VkImage dst = (i % 2 == 0) ? imgs[1] : imgs[0];
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        }
        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) { vkFreeCommandBuffers(dev, pool, 1, &cmd); return false; }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        const bool ok = vkQueueSubmit(vk.computeQueue(), 1, &si, fence) == VK_SUCCESS &&
                        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, pool, 1, &cmd);
        return ok;
    };

    // Warm-up (driver cache/clock ramp), then the timed phase.
    CHECK(runCopies(4));

    double gbPerSec = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(runCopies(kCopies));
    const auto t1 = std::chrono::steady_clock::now();
    // Each copy reads imageBytes and writes imageBytes.
    const double bytes = 2.0 * double(imageBytes) * kCopies;
    gbPerSec = bytes / secondsBetween(t0, t1) / 1e9;

    std::printf("gpu_bandwidth: 2x %ux%u RGBA8 device-local, %d copies in one submit\n",
                W, H, kCopies);
    std::printf("gpu_bandwidth: %.1f GB/s (read+write, %u MiB images)\n",
                gbPerSec, static_cast<unsigned>(imageBytes >> 20));

    // The floor is deliberately generous: even llvmpipe manages multi-GB/s on
    // its simulated device-to-device copies, and a real GPU is far above it.
    // A failure here is a warning that the WRONG context was picked -- check
    // which GPU vk.init() chose (printed above by the context itself, e.g.
    // "using [gpu0]").
    CHECK(gbPerSec > 2.0);

    for (int i = 0; i < 2; ++i) {
        if (imgs[i] != VK_NULL_HANDLE) vkDestroyImage(dev, imgs[i], nullptr);
        if (mems[i] != VK_NULL_HANDLE) vkFreeMemory(dev, mems[i], nullptr);
    }
    vkDestroyCommandPool(dev, pool, nullptr);

    if (failures == 0) std::printf("gpu_bandwidth: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
