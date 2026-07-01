#include "blend_engine.hpp"
#include "../vulkan/context.hpp"
#include "../gmix.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>
#include <unistd.h>

namespace gmix {

namespace {

// Push-constant block mirrored from shaders/blend.comp. Only 12 bytes —
// weights moved to a UBO (32 floats overflow the 128-byte PC minimum).
struct alignas(4) PushConstants {
    uint32_t frameCount;
    uint32_t frameW;
    uint32_t frameH;
};
static_assert(sizeof(PushConstants) <= 128,
              "push constants must fit the guaranteed 128-byte limit");

std::vector<char> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<char> buf(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    return buf;
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                        VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return ~0u;
}

} // namespace

BlendEngine::BlendEngine(VulkanContext& vk) : vk_(vk) {}

BlendEngine::~BlendEngine() {
    destroyAll();
}

// Full teardown, safe to call from the destructor or to reset the engine for
// re-init at a new size (the producer's swapchain can be recreated at a
// different resolution mid-session -- osu! does this repeatedly during
// startup -- and BlendEngine must be able to follow rather than leak or
// reuse stale handles sized for the old resolution).
void BlendEngine::destroyAll() {
    VkDevice dev = vk_.device();
    if (dev == VK_NULL_HANDLE) return;
    if (weightsMapped_) { vkUnmapMemory(dev, weightsMem_); weightsMapped_ = nullptr; }
    if (blendTimeline_) { vkDestroySemaphore(dev, blendTimeline_, nullptr); blendTimeline_ = VK_NULL_HANDLE; }
    blendValue_ = inFlightValue_ = frontValue_ = 0;
    if (dstSampler_)    { vkDestroySampler(dev, dstSampler_, nullptr); dstSampler_ = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < kDstBuffers; ++i) {
        if (dstView_[i])  { vkDestroyImageView(dev, dstView_[i], nullptr); dstView_[i] = VK_NULL_HANDLE; }
        if (dstImage_[i]) { vkDestroyImage(dev, dstImage_[i], nullptr); dstImage_[i] = VK_NULL_HANDLE; }
        if (dstMem_[i])   { vkFreeMemory(dev, dstMem_[i], nullptr); dstMem_[i] = VK_NULL_HANDLE; }
        if (dmaBufFd_[i] >= 0) { ::close(dmaBufFd_[i]); dmaBufFd_[i] = -1; }
        dmaBufStride_[i] = 0;
        dmaBufOffset_[i] = 0;
    }
    dmaBufCapable_ = false;
    if (weightsBuf_)    { vkDestroyBuffer(dev, weightsBuf_, nullptr); weightsBuf_ = VK_NULL_HANDLE; }
    if (weightsMem_)    { vkFreeMemory(dev, weightsMem_, nullptr); weightsMem_ = VK_NULL_HANDLE; }
    // cmd_ is freed implicitly with its pool.
    cmd_ = VK_NULL_HANDLE;
    blendInFlight_ = false;
    if (cmdPool_)       { vkDestroyCommandPool(dev, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
    destroyTransient();
    initialized_ = false;
}

void BlendEngine::destroyTransient() {
    VkDevice dev = vk_.device();
    if (dev == VK_NULL_HANDLE) return;
    if (pipeline_)         vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(dev, pipeLayout_, nullptr);
    if (dsLayout_)   vkDestroyDescriptorSetLayout(dev, dsLayout_, nullptr);
    if (descPool_)   vkDestroyDescriptorPool(dev, descPool_, nullptr);
    pipeline_   = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    dsLayout_   = VK_NULL_HANDLE;
    descPool_   = VK_NULL_HANDLE;
    descSet_    = VK_NULL_HANDLE;
}

bool BlendEngine::init(uint32_t w, uint32_t h) {
    if (initialized_) destroyAll();
    width_ = w; height_ = h;

    VkDevice dev = vk_.device();

    // Command pool for our dispatch buffers, on the (async) compute family so
    // the blend submits to a hardware queue independent of the graphics/present
    // queue. dst images are created CONCURRENT across both families below.
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk_.computeQueueFamily();
    if (vkCreateCommandPool(dev, &cpi, nullptr, &cmdPool_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: cmdPool create failed\n");
        return false;
    }

    // One persistent command buffer, reused (reset) per dispatch.
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cbai, &cmd_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: cmd buffer alloc failed\n");
        return false;
    }

    // Timeline semaphore: the blend signals an increasing value on the compute
    // queue; the present blit waits the relevant value on the graphics queue.
    VkSemaphoreTypeCreateInfo stci{};
    stci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stci.initialValue = 0;
    VkSemaphoreCreateInfo spi{};
    spi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    spi.pNext = &stci;
    if (vkCreateSemaphore(dev, &spi, nullptr, &blendTimeline_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: timeline semaphore create failed\n");
        return false;
    }
    blendValue_ = inFlightValue_ = frontValue_ = 0;

    if (!createDstImage())    return false;
    if (!createDescriptorSet()) return false;
    if (!createPipeline())    return false;

    initialized_ = true;
    return true;
}

bool BlendEngine::createDstImage() {
    VkDevice dev = vk_.device();

    // Zero-copy export: this GPU (RADV/Polaris10) has no
    // VK_EXT_image_drm_format_modifier, so an exportable dma-buf must use
    // VK_IMAGE_TILING_LINEAR (confirmed viable end-to-end: the OBS plugin's
    // gs_texture_create_from_dmabuf import renders live gameplay from these
    // buffers). Storage-image writes on LINEAR tiling are slower than
    // OPTIMAL, but the OBS plugin is the only remaining consumer of this
    // image, and it needs dma-buf export far more than it needs OPTIMAL's
    // memory layout. Fall back to OPTIMAL (non-exportable) if the format
    // doesn't support STORAGE on LINEAR tiling on this device, so init()
    // still degrades instead of silently producing a broken image.
    VkFormatProperties fmtProps{};
    vkGetPhysicalDeviceFormatProperties(vk_.physicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &fmtProps);
    dmaBufCapable_ = (fmtProps.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    if (!dmaBufCapable_) {
        std::fprintf(stderr,
            "gmix: blend: R8G8B8A8_UNORM has no LINEAR+STORAGE support on this "
            "device -- dst images will NOT be dma-buf exportable (falling back "
            "to OPTIMAL tiling)\n");
    }

    VkExternalMemoryImageCreateInfo emici{};
    emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (dmaBufCapable_) ici.pNext = &emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    // R8G8B8A8_UNORM matches the shader's `rgba8` storage-image declaration
    // exactly (storage images require an exact format match, no swizzle).
    // The Wayland presenter will convert to the swapchain's BGRA via a blit
    // with component swizzle.
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { width_, height_, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = dmaBufCapable_ ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    // General layout for storage image write.
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The blend writes these on the compute queue; the presenter blits them on
    // the graphics queue. When those are different families, declare the image
    // CONCURRENT across both so neither side needs a queue-ownership transfer
    // (we already CPU-gate via the timeline so there's no actual concurrent
    // access -- CONCURRENT just removes the transfer-barrier requirement).
    uint32_t families[2] = { vk_.queueFamily(), vk_.computeQueueFamily() };
    if (vk_.hasAsyncCompute() && families[0] != families[1]) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = families;
    } else {
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    // Two dst buffers (front/back) for the pipelined present path; each is an
    // independent storage-image render target the blend writes and the
    // presenter blits from.
    for (uint32_t i = 0; i < kDstBuffers; ++i) {
        if (vkCreateImage(dev, &ici, nullptr, &dstImage_[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "gmix: blend: dst image create failed\n");
            return false;
        }

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(dev, dstImage_[i], &mr);
        uint32_t memType = findMemoryType(vk_.physicalDevice(), mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == ~0u) {
            std::fprintf(stderr, "gmix: blend: no device-local mem type\n");
            return false;
        }
        VkMemoryDedicatedAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dai.image = dstImage_[i];
        VkExportMemoryAllocateInfo emai{};
        emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        emai.pNext = &dai;
        emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        if (dmaBufCapable_) mai.pNext = &emai;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType;
        if (vkAllocateMemory(dev, &mai, nullptr, &dstMem_[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "gmix: blend: dst mem alloc failed\n");
            return false;
        }
        vkBindImageMemory(dev, dstImage_[i], dstMem_[i], 0);

        // Export the dma-buf fd ONCE, right after creation -- persistent for
        // the buffer's lifetime, not re-exported per frame (this project
        // already learned the "export once, reuse" lesson the hard way for
        // producer export semaphores and consumer frame imports).
        if (dmaBufCapable_) {
            auto getFd = vk_.getMemoryFdKHR();
            if (!getFd) {
                std::fprintf(stderr, "gmix: blend: vkGetMemoryFdKHR unavailable, dst[%u] not exportable\n", i);
            } else {
                VkMemoryGetFdInfoKHR gfi{};
                gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
                gfi.memory = dstMem_[i];
                gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                if (getFd(dev, &gfi, &dmaBufFd_[i]) != VK_SUCCESS || dmaBufFd_[i] < 0) {
                    std::fprintf(stderr, "gmix: blend: dma-buf export failed for dst[%u]\n", i);
                    dmaBufFd_[i] = -1;
                } else {
                    VkImageSubresource sub{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
                    VkSubresourceLayout lay{};
                    vkGetImageSubresourceLayout(dev, dstImage_[i], &sub, &lay);
                    dmaBufStride_[i] = static_cast<uint32_t>(lay.rowPitch);
                    dmaBufOffset_[i] = lay.offset;
                    std::fprintf(stderr,
                        "gmix: blend: dst[%u] exported dma-buf fd=%d stride=%u offset=%llu\n",
                        i, dmaBufFd_[i], dmaBufStride_[i], (unsigned long long)dmaBufOffset_[i]);
                }
            }
        }

        // View.
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = dstImage_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vci, nullptr, &dstView_[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "gmix: blend: dst view failed\n");
            return false;
        }
    }
    return true;
}

bool BlendEngine::createDescriptorSet() {
    VkDevice dev = vk_.device();

    // Binding 0: srcImages[MAX_FRAMES] — fixed-size storage-image array. We
    // only write srcCount slots per dispatch; unused bindings stay valid
    // (VK_NULL_HANDLE images are never read because the shader loop is bounded
    // by frameCount in push constants). Avoids the descriptor-indexing feature.
    // Binding 1: dstImage (storage image).
    // Binding 2: weights UBO.
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[0].descriptorCount = kMaxBlendFrames;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsLayout_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: dsLayout failed\n");
        return false;
    }

    VkDescriptorPoolSize ps[2]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps[0].descriptorCount = kMaxBlendFrames + 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo ppi{};
    ppi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ppi.maxSets = 1;
    ppi.poolSizeCount = 2;
    ppi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &ppi, nullptr, &descPool_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: descPool failed\n");
        return false;
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsLayout_;
    if (vkAllocateDescriptorSets(dev, &ai, &descSet_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: descSet alloc failed\n");
        return false;
    }

    // ── Weights buffer: persistently mapped, host-visible + coherent ────────
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(float) * kMaxBlendFrames;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &weightsBuf_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: weights buf failed\n");
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, weightsBuf_, &mr);
    uint32_t memType = findMemoryType(vk_.physicalDevice(), mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == ~0u) {
        std::fprintf(stderr, "gmix: blend: no host-visible mem type\n");
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType;
    if (vkAllocateMemory(dev, &mai, nullptr, &weightsMem_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: weights mem failed\n");
        return false;
    }
    vkBindBufferMemory(dev, weightsBuf_, weightsMem_, 0);
    if (vkMapMemory(dev, weightsMem_, 0, VK_WHOLE_SIZE, 0,
                    reinterpret_cast<void**>(&weightsMapped_)) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: weights map failed\n");
        return false;
    }
    std::memset(weightsMapped_, 0, sizeof(float) * kMaxBlendFrames);

    // Bind the weights SSBO to the descriptor set once (persistent).
    VkDescriptorBufferInfo wbi{};
    wbi.buffer = weightsBuf_;
    wbi.offset = 0;
    wbi.range  = sizeof(float) * kMaxBlendFrames;
    VkWriteDescriptorSet wWrite{};
    wWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wWrite.dstSet = descSet_;
    wWrite.dstBinding = 2;
    wWrite.descriptorCount = 1;
    wWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wWrite.pBufferInfo = &wbi;
    vkUpdateDescriptorSets(dev, 1, &wWrite, 0, nullptr);

    return true;
}

bool BlendEngine::createPipeline() {
    VkDevice dev = vk_.device();

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipeLayout_) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: blend: pipeLayout failed\n");
        return false;
    }

    // Build a compute pipeline from a SPIR-V file, reusing pipeLayout_.
    // Returns VK_NULL_HANDLE on any failure (caller decides if that's fatal).
    auto buildPipeline = [&](const char* spvPath) -> VkPipeline {
        auto code = readFile(spvPath);
        if (code.empty()) {
            std::fprintf(stderr, "gmix: blend: cannot read %s\n", spvPath);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = code.size();
        smi.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod{};
        if (vkCreateShaderModule(dev, &smi, nullptr, &mod) != VK_SUCCESS) {
            std::fprintf(stderr, "gmix: blend: shader module failed (%s)\n", spvPath);
            return VK_NULL_HANDLE;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = mod;
        cpi.stage.pName = "main";
        cpi.layout = pipeLayout_;
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &p);
        vkDestroyShaderModule(dev, mod, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "gmix: blend: pipeline create failed for %s (%d)\n", spvPath, r);
            return VK_NULL_HANDLE;
        }
        return p;
    };

    // GMIX_SHADER_DIR is the build/shaders directory, set via a compile def.
    pipeline_ = buildPipeline(GMIX_SHADER_DIR "/blend.spv");
    if (pipeline_ == VK_NULL_HANDLE) return false;
    return true;
}

VkImageView BlendEngine::dispatch(VkImageView* srcViews, const float* weights,
                                  uint32_t srcCount) {
    // Synchronous wrapper (tests / readback): kick the async blend into buffer
    // 0 and block until it finishes.
    if (!dispatchAsync(srcViews, weights, srcCount, 0))
        return VK_NULL_HANDLE;
    waitBlendDone();
    return dstView_[0];
}

bool BlendEngine::dispatchAsync(VkImageView* srcViews, const float* weights,
                                uint32_t srcCount, uint32_t dstIdx,
                                const VkSemaphore* waitSems, const uint64_t* waitVals,
                                uint32_t waitCount) {
    if (!initialized_) return false;
    if (srcCount == 0 || srcCount > (uint32_t)kMaxBlendFrames) return false;
    if (dstIdx >= kDstBuffers) return false;
    // Caller contract: the previous blend must have completed (single fence +
    // single command buffer), so we never reset state still in GPU use.

    VkDevice dev = vk_.device();

    // ── Push weights into the mapped UBO ────────────────────────────────────
    std::memcpy(weightsMapped_, weights, sizeof(float) * srcCount);

    // ── Update descriptor set with the current srcViews + dst ───────────────
    // We declare MAX_FRAMES slots at binding 0 but only use srcCount of them
    // per dispatch. The shader loop is bounded by frameCount, so unused slots
    // are never read — but the validation layer can't prove that statically,
    // so we fill every unused slot with the first valid view to keep the
    // descriptors technically "valid". Reading them would just re-add frame 0
    // weighted by 0 (weights[i]=0 for i>=srcCount), contributing nothing.
    std::vector<VkDescriptorImageInfo> srcInfos(kMaxBlendFrames);
    for (uint32_t i = 0; i < kMaxBlendFrames; ++i) {
        srcInfos[i].sampler = VK_NULL_HANDLE;
        srcInfos[i].imageView = srcViews[i < srcCount ? i : 0];
        srcInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView = dstView_[dstIdx];   // write into the chosen back buffer
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet_;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = kMaxBlendFrames;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = srcInfos.data();
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dstInfo;
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

    // ── Record the persistent command buffer (reset by begin) ───────────────
    VkCommandBuffer cmd = cmd_;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Transition dst → GENERAL.
    VkImageMemoryBarrier dstBar{};
    dstBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dstBar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBar.image = dstImage_[dstIdx];
    dstBar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &dstBar);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_,
                            0, 1, &descSet_, 0, nullptr);

    PushConstants pc{};
    pc.frameCount = srcCount;
    pc.frameW = width_;
    pc.frameH = height_;
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(PushConstants), &pc);

    // (workgroups) = ceil(extent / 8)
    uint32_t gx = (width_  + 7) / 8;
    uint32_t gy = (height_ + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Transition dst → TRANSFER_SRC_OPTIMAL for the presenter to blit from.
    dstBar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    dstBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dstBar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &dstBar);

    vkEndCommandBuffer(cmd);

    // ── Submit on the compute queue WITHOUT waiting ──────────────────────────
    // Wait list: the source frames' producer render-complete timeline semaphores
    // (so the blend reads them only once the producer has finished writing) --
    // this is the GPU-side replacement for the old host waitForProducer() that
    // used to block the present thread. Signal: our own timeline to an increasing
    // value; the present loop polls the counter (pollBlendDone) and the present
    // blit waits this value on the graphics queue (cross-queue visibility).
    std::vector<VkPipelineStageFlags> waitStages(waitCount,
                                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    uint64_t signalValue = ++blendValue_;
    VkTimelineSemaphoreSubmitInfo tssi{};
    tssi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tssi.waitSemaphoreValueCount = waitCount;
    tssi.pWaitSemaphoreValues = waitVals;
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues = &signalValue;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tssi;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitCount ? waitStages.data() : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &blendTimeline_;

    if (vkQueueSubmit(vk_.computeQueue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        --blendValue_;   // submit failed; the value was not consumed
        return false;
    }
    inFlightValue_ = signalValue;
    blendInFlight_ = true;
    return true;
}

bool BlendEngine::pollBlendDone() {
    if (!blendInFlight_) return false;
    uint64_t cur = 0;
    if (vkGetSemaphoreCounterValue(vk_.device(), blendTimeline_, &cur) == VK_SUCCESS &&
        cur >= inFlightValue_) {
        blendInFlight_ = false;
        frontValue_    = inFlightValue_;   // this blend is now the front buffer
        return true;
    }
    return false;
}

void BlendEngine::waitBlendDone() {
    if (!blendInFlight_) return;
    VkSemaphoreWaitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &blendTimeline_;
    wi.pValues = &inFlightValue_;
    vkWaitSemaphores(vk_.device(), &wi, UINT64_MAX);
    blendInFlight_ = false;
    frontValue_    = inFlightValue_;
}

bool BlendEngine::readbackDst(uint8_t* outPixels) {
    if (!initialized_) return false;
    VkDevice dev = vk_.device();

    // Staging buffer for the readback.
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = static_cast<VkDeviceSize>(width_) * height_ * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer sbuf{};
    if (vkCreateBuffer(dev, &bci, nullptr, &sbuf) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, sbuf, &mr);
    uint32_t mt = findMemoryType(vk_.physicalDevice(), mr.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory sbm{};
    if (vkAllocateMemory(dev, &mai, nullptr, &sbm) != VK_SUCCESS) {
        vkDestroyBuffer(dev, sbuf, nullptr); return false;
    }
    vkBindBufferMemory(dev, sbuf, sbm, 0);

    // dstImage_[0] is in TRANSFER_SRC_OPTIMAL after the sync dispatch — copy it.
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferImageCopy reg{};
    reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount = 1;
    reg.imageExtent = { width_, height_, 1 };
    vkCmdCopyImageToBuffer(cmd, dstImage_[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sbuf, 1, &reg);
    vkEndCommandBuffer(cmd);

    // Submit on the compute queue (cmdPool_ is a compute-family pool) with a
    // local fence; the prior sync dispatch already waited the blend, so the dst
    // contents are final.
    VkFence rbFence = VK_NULL_HANDLE;
    VkFenceCreateInfo rfci{}; rfci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &rfci, nullptr, &rbFence);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_.computeQueue(), 1, &si, rbFence);
    vkWaitForFences(dev, 1, &rbFence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, rbFence, nullptr);
    vkFreeCommandBuffers(dev, cmdPool_, 1, &cmd);

    void* mapped = nullptr;
    vkMapMemory(dev, sbm, 0, VK_WHOLE_SIZE, 0, &mapped);
    std::memcpy(outPixels, mapped, static_cast<size_t>(bci.size));
    vkUnmapMemory(dev, sbm);

    vkDestroyBuffer(dev, sbuf, nullptr);
    vkFreeMemory(dev, sbm, nullptr);
    return true;
}

} // namespace gmix
