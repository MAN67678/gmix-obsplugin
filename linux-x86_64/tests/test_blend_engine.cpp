// ─────────────────────────────────────────────────────────────────────────────
// Standalone validation of the blend compute pipeline on the real GPU.
//
// Creates N synthetic 2D source images of solid RGBA8 colors, uploads them,
// dispatches the blend shader with known weights, reads back the dst image,
// and compares against a CPU reference of the same weighted accumulate.
//
// Run: ./test_blend_engine
// Pass if mean abs error per channel < 1.0 (RGBA8 rounding tolerance).
// ─────────────────────────────────────────────────────────────────────────────
#include "test_macros.hpp"
#include "vulkan/context.hpp"
#include "blend/blend_engine.hpp"
#include "../src/gmix.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace gmix;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return ~0u;
}

// A small RAII wrapper for an uploadable RGBA8 source image + its view.
struct SrcImage {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkImageView    view  = VK_NULL_HANDLE;
    VkDevice       dev   = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;

    bool create(VkDevice d, VkPhysicalDevice p, uint32_t w, uint32_t h,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        dev = d; phys = p;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM; // shader uses rgba8
        ici.extent = { w, h, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(d, &ici, nullptr, &image) != VK_SUCCESS) return false;

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(d, image, &mr);
        uint32_t mt = findMemoryType(phys, mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(d, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(d, image, mem, 0);

        // Upload solid color via a staging buffer.
        VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = w * h * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer sbuf{};
        if (vkCreateBuffer(d, &bci, nullptr, &sbuf) != VK_SUCCESS) return false;
        VkMemoryRequirements bmr{}; vkGetBufferMemoryRequirements(d, sbuf, &bmr);
        uint32_t bmt = findMemoryType(phys, bmr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo bmai{}; bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bmai.allocationSize = bmr.size; bmai.memoryTypeIndex = bmt;
        VkDeviceMemory sbm{};
        vkAllocateMemory(d, &bmai, nullptr, &sbm);
        vkBindBufferMemory(d, sbuf, sbm, 0);
        void* mapped = nullptr; vkMapMemory(d, sbm, 0, VK_WHOLE_SIZE, 0, &mapped);
        auto* px = static_cast<uint8_t*>(mapped);
        for (uint32_t i = 0; i < w * h; ++i) {
            px[i*4+0] = r; px[i*4+1] = g; px[i*4+2] = b; px[i*4+3] = a;
        }
        vkUnmapMemory(d, sbm);

        VkCommandPool cpool{};
        VkCommandPoolCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = 0; // ctx picks queue 0 of graphics family
        // We need the right queue family. Grab it from the engine's context
        // by passing through. For this harness we re-resolve via the device.
        // Simpler: use a temporary pool from any graphics family.
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qf.data());
        uint32_t gqf = 0;
        for (uint32_t q = 0; q < qfc; ++q)
            if (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gqf = q; break; }
        cpci.queueFamilyIndex = gqf;
        vkCreateCommandPool(d, &cpci, nullptr, &cpool);

        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(d, &cai, &cmd);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier toCopy{};
        toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.image = image;
        toCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,nullptr,0,nullptr,1,&toCopy);

        VkBufferImageCopy reg{};
        reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        reg.imageSubresource.layerCount = 1;
        reg.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, sbuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

        VkImageMemoryBarrier toGeneral = toCopy;
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,nullptr,0,nullptr,1,&toGeneral);
        vkEndCommandBuffer(cmd);

        VkQueue queue{}; vkGetDeviceQueue(d, gqf, 0, &queue);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        vkFreeCommandBuffers(d, cpool, 1, &cmd);
        vkDestroyCommandPool(d, cpool, nullptr);
        vkDestroyBuffer(d, sbuf, nullptr);
        vkFreeMemory(d, sbm, nullptr);

        VkImageViewCreateInfo vci{}; vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(d, &vci, nullptr, &view) != VK_SUCCESS) return false;
        return true;
    }

    ~SrcImage() {
        if (view)  vkDestroyImageView(dev, view, nullptr);
        if (image) vkDestroyImage(dev, image, nullptr);
        if (mem)   vkFreeMemory(dev, mem, nullptr);
    }
};

} // namespace

TEST_CASE(blend_engine_matches_cpu_reference) {
    VulkanContext vk;
    if (!vk.init(-1)) { CHECK(!"vulkan init failed"); return; }

    // BlendEngine dst is R8G8B8A8_UNORM (matches the shader's rgba8 decl).
    // We feed four solid-color R8G8B8A8 source frames, dispatch with equal
    // weights (0.25 each), read back the dst, and compare against the CPU
    // reference: per-channel weighted average, rounded to nearest uint8.
    BlendEngine blend(vk);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed"); return; }

    struct Col { uint8_t r, g, b, a; };
    const Col cols[4] = {
        {255,   0,   0, 255},   // red
        {  0, 255,   0, 255},   // green
        {  0,   0, 255, 255},   // blue
        {255, 255, 255, 255},   // white
    };

    std::vector<SrcImage> srcs(4);
    std::vector<VkImageView> views(4);
    for (int i = 0; i < 4; ++i) {
        if (!srcs[i].create(vk.device(), vk.physicalDevice(), kW, kH,
                            cols[i].r, cols[i].g, cols[i].b, cols[i].a)) {
            CHECK(!"src image create failed"); return;
        }
        views[i] = srcs[i].view;
    }

    // Equal weights → average. tmix semantics: 0.25 each.
    float weights[4] = { 0.25f, 0.25f, 0.25f, 0.25f };

    VkImageView out = blend.dispatch(views.data(), weights, 4);
    CHECK(out != VK_NULL_HANDLE);

    // Read back the dst pixels.
    std::vector<uint8_t> dst(static_cast<size_t>(kW) * kH * 4);
    if (!blend.readbackDst(dst.data())) { CHECK(!"readback failed"); return; }

    // CPU reference: per-channel weighted average of the 4 source colors.
    // The shader operates in linear [0,1] then writes back; RGBA8_UNORM
    // round-trips through float, so the reference is the float average
    // rounded to nearest.
    auto toF = [](uint8_t v) { return v / 255.0f; };
    auto toU8 = [](float v) {
        int x = static_cast<int>(std::round(v * 255.0f));
        return static_cast<uint8_t>(std::clamp(x, 0, 255));
    };
    float refR = 0, refG = 0, refB = 0, refA = 0;
    for (int i = 0; i < 4; ++i) {
        refR += weights[i] * toF(cols[i].r);
        refG += weights[i] * toF(cols[i].g);
        refB += weights[i] * toF(cols[i].b);
        refA += weights[i] * toF(cols[i].a);
    }
    uint8_t expR = toU8(refR), expG = toU8(refG), expB = toU8(refB), expA = toU8(refA);

    // Every output pixel should equal the reference (solid sources → solid dst).
    int mismatches = 0;
    for (uint32_t p = 0; p < kW * kH; ++p) {
        uint8_t* px = &dst[p * 4];
        if (std::abs((int)px[0] - expR) > 1 || std::abs((int)px[1] - expG) > 1 ||
            std::abs((int)px[2] - expB) > 1 || std::abs((int)px[3] - expA) > 1) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    std::printf("[   info   ] dst avg RGBA = (%d, %d, %d, %d)  ref = (%d, %d, %d, %d)  mismatches = %d / %u\n",
                dst[0], dst[1], dst[2], dst[3], expR, expG, expB, expA, mismatches, kW * kH);
}

// Weighted (unequal) accumulate: heavier weight on red, lighter on green.
TEST_CASE(blend_engine_weighted_accumulate) {
    VulkanContext vk;
    if (!vk.init(-1)) { CHECK(!"vulkan init failed"); return; }

    BlendEngine blend(vk);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed"); return; }

    struct Col { uint8_t r, g, b, a; };
    const Col cols[3] = {
        {255,   0,   0, 255},   // red — heavy
        {  0, 255,   0, 255},   // green — mid
        {  0,   0,   0, 255},   // black — light
    };
    std::vector<SrcImage> srcs(3);
    std::vector<VkImageView> views(3);
    for (int i = 0; i < 3; ++i) {
        if (!srcs[i].create(vk.device(), vk.physicalDevice(), kW, kH,
                            cols[i].r, cols[i].g, cols[i].b, cols[i].a)) {
            CHECK(!"src image create failed"); return;
        }
        views[i] = srcs[i].view;
    }
    // Pre-normalized: 0.5 red + 0.3 green + 0.2 black.
    float weights[3] = { 0.5f, 0.3f, 0.2f };

    if (blend.dispatch(views.data(), weights, 3) == VK_NULL_HANDLE) {
        CHECK(!"dispatch failed"); return;
    }

    std::vector<uint8_t> dst(static_cast<size_t>(kW) * kH * 4);
    if (!blend.readbackDst(dst.data())) { CHECK(!"readback failed"); return; }

    auto toF = [](uint8_t v) { return v / 255.0f; };
    auto toU8 = [](float v) {
        int x = static_cast<int>(std::round(v * 255.0f));
        return static_cast<uint8_t>(std::clamp(x, 0, 255));
    };
    float r = 0, g = 0, b = 0, a = 0;
    for (int i = 0; i < 3; ++i) {
        r += weights[i] * toF(cols[i].r);
        g += weights[i] * toF(cols[i].g);
        b += weights[i] * toF(cols[i].b);
        a += weights[i] * toF(cols[i].a);
    }
    uint8_t expR = toU8(r), expG = toU8(g), expB = toU8(b), expA = toU8(a);

    int mismatches = 0;
    for (uint32_t p = 0; p < kW * kH; ++p) {
        uint8_t* px = &dst[p * 4];
        if (std::abs((int)px[0] - expR) > 1 || std::abs((int)px[1] - expG) > 1 ||
            std::abs((int)px[2] - expB) > 1 || std::abs((int)px[3] - expA) > 1) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    std::printf("[   info   ] weighted dst RGBA = (%d, %d, %d, %d)  ref = (%d, %d, %d, %d)  mismatches = %d\n",
                dst[0], dst[1], dst[2], dst[3], expR, expG, expB, expA, mismatches);
}

// A headless VulkanContext (no Wayland-surface instance extensions, no
// swapchain/present_id/present_wait device extensions) must still stand up a
// device and drive the blend engine -- this is the exact path the OBS plugin
// worker thread relies on, which never presents to a window.
TEST_CASE(headless_context_drives_blend_engine) {
    VulkanContext vk;
    if (!vk.init(-1, /*headless=*/true)) { CHECK(!"headless vulkan init failed"); return; }
    CHECK(!vk.presentWaitEnabled());  // never requested/enabled headless

    BlendEngine blend(vk);
    if (!blend.init(kW, kH)) { CHECK(!"blend init failed (headless)"); return; }

    SrcImage src;
    if (!src.create(vk.device(), vk.physicalDevice(), kW, kH, 10, 20, 30, 255)) {
        CHECK(!"src image create failed"); return;
    }
    VkImageView view = src.view;
    float weight = 1.0f;
    if (blend.dispatch(&view, &weight, 1) == VK_NULL_HANDLE) {
        CHECK(!"dispatch failed (headless)"); return;
    }

    std::vector<uint8_t> dst(kW * kH * 4);
    if (!blend.readbackDst(dst.data())) { CHECK(!"readback failed (headless)"); return; }
    CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30 && dst[3] == 255);
}

// "Latency mode" -- dstBufferCount is now a runtime init() argument (was a
// compile-time kDstBuffers=3 constant) so the OBS plugin can offer
// Fast/Medium/Slow/Very slow (2/3/4/5 buffers). Confirm a non-default count
// actually takes effect: the reported count, every dstImage()/dstView() up
// to that count is a real handle, and dispatchAsync() into the LAST valid
// index (not just index 0) works.
TEST_CASE(blend_engine_runtime_dst_buffer_count) {
    VulkanContext vk;
    if (!vk.init(-1, /*headless=*/true)) { CHECK(!"vulkan init failed"); return; }

    BlendEngine blend(vk);
    constexpr uint32_t kCount = 5;   // "Very slow"
    if (!blend.init(kW, kH, kCount)) { CHECK(!"blend init failed"); return; }
    CHECK_EQ(blend.dstBufferCount(), kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        CHECK(blend.dstImage(i) != VK_NULL_HANDLE);
        CHECK(blend.dstView(i)  != VK_NULL_HANDLE);
    }

    SrcImage src;
    if (!src.create(vk.device(), vk.physicalDevice(), kW, kH, 40, 50, 60, 255)) {
        CHECK(!"src image create failed"); return;
    }
    VkImageView view = src.view;
    float weight = 1.0f;
    // Dispatch into the LAST slot (kCount-1), matching gmix_source.cpp's
    // round-robin, not just the always-tested index 0.
    if (!blend.dispatchAsync(&view, &weight, 1, kCount - 1)) {
        CHECK(!"dispatchAsync into last slot failed"); return;
    }
    blend.waitBlendDone();
    CHECK(blend.dstReadyValue() > 0);

    // An out-of-range index must be rejected, not silently clamp/UB.
    CHECK(!blend.dispatchAsync(&view, &weight, 1, kCount));
}

int main() {
    std::printf("==== blend_engine GPU test ====\n");
    int rc = (g_test_failures == 0) ? 0 : 1;
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return rc;
}
