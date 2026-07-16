// ─────────────────────────────────────────────────────────────────────────────
// Cross-process frame export→import round-trip on the real GPU.
//
// Parent (consumer): gmix-style — opens Vulkan, listens on a socket, imports
//   the frame, waits on the timeline semaphore, reads pixels, verifies.
// Child (producer):  layer-style — opens its OWN Vulkan device (same RADV),
//   allocates an exportable image, writes a known color, exports the mem fd
//   and a timeline-semaphore fd, signals the sem, ships both over the socket.
//
// Pass: the consumer's read-back pixels equal the producer's written color.
// ─────────────────────────────────────────────────────────────────────────────
#include "vulkan/context.hpp"
#include "ipc/frame_protocol.hpp"
#include "ipc/frame_sender.hpp"
#include "ipc/frame_receiver.hpp"
#include "ipc/imported_frame.hpp"
#include "test_macros.hpp"

#include <vulkan/vulkan.h>

#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gmix;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;
constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;

std::string sockPath() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    return (xdg ? std::string(xdg) : std::string("/tmp")) + "/gmix-test-crossproc.sock";
}

uint32_t findMemType(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags fl,
                     bool requireExport = false) {
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(p, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & fl) == fl)
            return i;
    return ~0u;
}

// Resolve the two fd export/import entry points we need for the producer side.
struct FdFns {
    PFN_vkGetMemoryFdKHR    getMemoryFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemFd    = nullptr;
};
FdFns loadFns(VkDevice dev) {
    FdFns f;
    f.getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
    f.getSemFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR"));
    return f;
}

// ── Producer (child) ─────────────────────────────────────────────────────────
int runProducer(const std::string& path) {
    VulkanContext vk;
    if (!vk.init(-1)) { std::fprintf(stderr, "P: vk init\n"); return 1; }
    auto fns = loadFns(vk.device());
    if (!fns.getMemoryFd || !fns.getSemFd) { std::fprintf(stderr, "P: no fd fns\n"); return 1; }

    VkDevice dev = vk.device();

    // Exportable image.
    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = kFmt;
    ici.extent = { kW, kH, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage img{};
    if (vkCreateImage(dev, &ici, nullptr, &img) != VK_SUCCESS) { std::fprintf(stderr,"P: img\n"); return 1; }

    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(dev, img, &mr);
    // Need a type that allows OPAQUE_FD export. Query compatible types.
    VkExternalImageFormatProperties efp{};
    efp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 ifp{}; ifp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2; ifp.pNext = &efp;
    VkPhysicalDeviceExternalImageFormatInfo eifi{};
    eifi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    eifi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 pfi{};
    pfi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2; pfi.pNext = &eifi;
    pfi.format = kFmt; pfi.type = VK_IMAGE_TYPE_2D; pfi.tiling = VK_IMAGE_TILING_OPTIMAL; pfi.usage = ici.usage;
    // The exportable type bits aren't exposed directly here in 1.4; rely on
    // device-local from the requirements — RADV marks all device-local types
    // as exportable for OPAQUE_FD.
    uint32_t mt = findMemType(vk.physicalDevice(), mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkExportMemoryAllocateInfo ema{};
    ema.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    ema.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.pNext = &ema;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem{};
    if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { std::fprintf(stderr,"P: alloc\n"); return 1; }
    vkBindImageMemory(dev, img, mem, 0);

    // Staging upload of solid color (50,100,200,255).
    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = kW*kH*4; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer sb{}; vkCreateBuffer(dev, &bci, nullptr, &sb);
    VkMemoryRequirements bmr{}; vkGetBufferMemoryRequirements(dev, sb, &bmr);
    uint32_t bmt = findMemType(vk.physicalDevice(), bmr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo bmai{}; bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize = bmr.size; bmai.memoryTypeIndex = bmt;
    VkDeviceMemory sbm{}; vkAllocateMemory(dev, &bmai, nullptr, &sbm); vkBindBufferMemory(dev, sb, sbm, 0);
    void* m{}; vkMapMemory(dev, sbm, 0, VK_WHOLE_SIZE, 0, &m);
    auto* px = static_cast<uint8_t*>(m);
    for (uint32_t i = 0; i < kW*kH; ++i) { px[i*4]=50; px[i*4+1]=100; px[i*4+2]=200; px[i*4+3]=255; }
    vkUnmapMemory(dev, sbm);

    VkCommandPool cp{}; VkCommandPoolCreateInfo cpci{}; cpci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = vk.queueFamily(); vkCreateCommandPool(dev, &cpci, nullptr, &cp);
    VkCommandBuffer cmd{}; VkCommandBufferAllocateInfo cai{}; cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool=cp; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{}; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=img; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy reg{}; reg.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount=1; reg.imageExtent={kW,kH,1};
    vkCmdCopyBufferToImage(cmd, sb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    // Leave in GENERAL so the consumer can sample as a storage image.
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    vkQueueSubmit(vk.queue(), 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(vk.queue());
    vkFreeCommandBuffers(dev, cp, 1, &cmd);

    // Export timeline semaphore, signal it AFTER the upload completes.
    VkSemaphoreTypeCreateInfo tci{}; tci.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{}; sci.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; sci.pNext=&tci;
    VkSemaphore sem{}; vkCreateSemaphore(dev, &sci, nullptr, &sem);

    VkPipelineStageFlags sigStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sigi{}; sigi.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sigi.signalSemaphoreCount=1; sigi.pSignalSemaphores=&sem; sigi.pWaitDstStageMask=&sigStage;
    // Submit an empty command buffer signal so the GPU has finished the upload.
    vkQueueSubmit(vk.queue(), 0, nullptr, VK_NULL_HANDLE);
    uint64_t sigVal = 7;
    VkTimelineSemaphoreSubmitInfo tsi{}; tsi.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount=1; tsi.pSignalSemaphoreValues=&sigVal;
    sigi.pNext=&tsi;
    vkQueueSubmit(vk.queue(), 0, &sigi, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.queue());

    // Export the fds.
    VkMemoryGetFdInfoKHR mgfi{}; mgfi.sType=VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    mgfi.memory = mem; mgfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int memFd = -1; fns.getMemoryFd(dev, &mgfi, &memFd);

    VkSemaphoreGetFdInfoKHR sgfi{}; sgfi.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sgfi.semaphore = sem; sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int semFd = -1; fns.getSemFd(dev, &sgfi, &semFd);

    // Ship over IPC.
    ipc::FrameSender s;
    if (!s.connect(path)) { std::fprintf(stderr,"P: connect\n"); return 1; }
    if (!s.sendHandshake(kW, kH, kFmt)) { std::fprintf(stderr,"P: hs\n"); return 1; }
    ipc::FrameHeader hdr{}; hdr.magic=kMagic; hdr.width=kW; hdr.height=kH;
    hdr.vkFormat=kFmt; hdr.semSignalValue=sigVal; hdr.frameIndex=1;
    if (s.sendFrame(hdr, memFd, semFd) != ipc::FrameSender::SendResult::Sent) { std::fprintf(stderr,"P: send\n"); return 1; }
    s.disconnect();
    return 0;
}

} // namespace

TEST_CASE(crossproc_export_import_roundtrip) {
    std::string path = sockPath();
    ::unlink(path.c_str());

    VulkanContext vk;
    CHECK(vk.init(-1));

    ipc::FrameReceiver r;
    CHECK(r.listen(path));

    pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(runProducer(path));

    ipc::FrameHandshake hs{};
    CHECK(r.acceptProducer(hs));

    ipc::RecvFrame rf{};
    CHECK(r.recvFrame(rf));

    ImportedFrame frame;
    CHECK(frame.init(vk, rf.memFd, rf.semFd, hs.frameW, hs.frameH,
                     static_cast<VkFormat>(hs.vkFormat), rf.header.semSignalValue));
    frame.waitForProducer();

    // Read back via a copy to host buffer.
    VkDevice dev = vk.device();
    VkBufferCreateInfo bci{}; bci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size=kW*kH*4; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer sbuf{}; vkCreateBuffer(dev, &bci, nullptr, &sbuf);
    VkMemoryRequirements bmr{}; vkGetBufferMemoryRequirements(dev, sbuf, &bmr);
    uint32_t bmt = findMemType(vk.physicalDevice(), bmr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo bmai{}; bmai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize=bmr.size; bmai.memoryTypeIndex=bmt;
    VkDeviceMemory sbm{}; vkAllocateMemory(dev, &bmai, nullptr, &sbm); vkBindBufferMemory(dev, sbuf, sbm, 0);

    VkCommandPool cp{}; VkCommandPoolCreateInfo cpci{}; cpci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex=vk.queueFamily(); vkCreateCommandPool(dev, &cpci, nullptr, &cp);
    VkCommandBuffer cmd{}; VkCommandBufferAllocateInfo cai{}; cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool=cp; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy creg{}; creg.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    creg.imageSubresource.layerCount=1; creg.imageExtent={kW,kH,1};
    vkCmdCopyImageToBuffer(cmd, frame.image(), VK_IMAGE_LAYOUT_GENERAL, sbuf, 1, &creg);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    vkQueueSubmit(vk.queue(), 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(vk.queue());

    void* m{}; vkMapMemory(dev, sbm, 0, VK_WHOLE_SIZE, 0, &m);
    auto* out = static_cast<uint8_t*>(m);
    int mismatches = 0;
    for (uint32_t i = 0; i < kW*kH; ++i) {
        if (std::abs((int)out[i*4]-50)>1 || std::abs((int)out[i*4+1]-100)>1 ||
            std::abs((int)out[i*4+2]-200)>1 || std::abs((int)out[i*4+3]-255)>1) ++mismatches;
    }
    vkUnmapMemory(dev, sbm);
    vkFreeCommandBuffers(dev, cp, 1, &cmd);
    vkDestroyCommandPool(dev, cp, nullptr);
    vkDestroyBuffer(dev, sbuf, nullptr); vkFreeMemory(dev, sbm, nullptr);

    CHECK(mismatches == 0);
    std::printf("[   info   ] crossproc first pixel RGBA = (%d,%d,%d,%d)  mismatches = %d\n",
                out[0], out[1], out[2], out[3], mismatches);

    r.close();
    int status = 0; ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

int main() {
    std::printf("==== cross-process export/import test ====\n");
    int rc = (g_test_failures == 0) ? 0 : 1;
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return rc;
}
