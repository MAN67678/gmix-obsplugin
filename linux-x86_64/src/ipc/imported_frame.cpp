#include "imported_frame.hpp"
#include "../vulkan/context.hpp"

#include <vulkan/vulkan.h>
#include <cstdio>
#include <unistd.h>

namespace gmix {

// ── PooledImage ──────────────────────────────────────────────────────────────
PooledImage::~PooledImage() {
    if (!vk) return;
    VkDevice dev = vk->device();
    if (view)  vkDestroyImageView(dev, view, nullptr);
    if (image) vkDestroyImage(dev, image, nullptr);
    if (mem)   vkFreeMemory(dev, mem, nullptr);
}

// ── PooledSemaphore ──────────────────────────────────────────────────────────
PooledSemaphore::~PooledSemaphore() {
    if (vk && sema) vkDestroySemaphore(vk->device(), sema, nullptr);
}

namespace {

// Import one OPAQUE_FD image-memory into a fresh VkImage+view. Consumes memFd.
std::shared_ptr<PooledImage> importImage(VulkanContext& vk, int memFd,
                                         uint32_t w, uint32_t h, VkFormat format,
                                         uint64_t rowPitch) {
    auto fail = [&](const char* msg) -> std::shared_ptr<PooledImage> {
        std::fprintf(stderr, "gmix: import: %s\n", msg);
        if (memFd >= 0) ::close(memFd);
        return nullptr;
    };
    if (memFd < 0) return fail("missing mem fd");

    VkDevice dev = vk.device();
    auto img = std::make_shared<PooledImage>();
    img->vk = &vk;

    // External-backed image. VK_EXT_image_drm_format_modifier is absent on this
    // RADV/Polaris build, so we use plain LINEAR tiling matching the producer.
    (void)rowPitch;  // accepted for diagnostics; layout matched via LINEAR tiling
    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &img->image) != VK_SUCCESS)
        return fail("image create");

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, img->image, &mr);
    uint32_t typeBits = mr.memoryTypeBits;

    VkPhysicalDeviceMemoryProperties mpp{};
    vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice(), &mpp);
    uint32_t memType = ~0u;
    for (uint32_t i = 0; i < mpp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mpp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType = i; break;
        }
    }
    if (memType == ~0u)
        for (uint32_t i = 0; i < mpp.memoryTypeCount; ++i)
            if (typeBits & (1u << i)) { memType = i; break; }
    if (memType == ~0u) return fail("no compatible mem type for import");

    VkImportMemoryFdInfoKHR imfi{};
    imfi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    imfi.fd = memFd;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType;
    mai.pNext = &imfi;
    if (vkAllocateMemory(dev, &mai, nullptr, &img->mem) != VK_SUCCESS)
        return fail("memory import");   // memFd consumed by the (failed) import
    if (vkBindImageMemory(dev, img->image, img->mem, 0) != VK_SUCCESS) {
        // memFd already consumed by the successful allocate above.
        std::fprintf(stderr, "gmix: import: bind image\n");
        return nullptr;
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(dev, &vci, nullptr, &img->view) != VK_SUCCESS) {
        std::fprintf(stderr, "gmix: import: view create\n");
        return nullptr;
    }
    return img;
}

} // namespace

// ── FrameImagePool ───────────────────────────────────────────────────────────
std::shared_ptr<PooledImage> FrameImagePool::acquire(VulkanContext& vk, uint32_t slot,
                                                     int memFd, uint32_t w, uint32_t h,
                                                     VkFormat format, uint64_t rowPitch) {
    // Guard against a corrupt/garbage slot (protocol mismatch) ballooning the
    // pool. The producer's export ring is small (kExportRing); anything wildly
    // larger is bogus -- import it uncached rather than resizing to it.
    constexpr uint32_t kMaxSlots = 256;
    if (slot >= kMaxSlots) return importImage(vk, memFd, w, h, format, rowPitch);

    if (w != w_ || h != h_ || format != fmt_) {
        slots_.clear();                 // dimensions changed: drop the whole pool
        w_ = w; h_ = h; fmt_ = format;
    }
    if (slot >= slots_.size()) slots_.resize(slot + 1);

    if (slots_[slot]) {                 // already imported this slot: reuse it
        if (memFd >= 0) ::close(memFd); // the new fd is the same backing memory
        return slots_[slot];
    }

    auto img = importImage(vk, memFd, w, h, format, rowPitch);  // consumes memFd
    if (img) slots_[slot] = img;
    return img;
}

void FrameImagePool::clear() {
    slots_.clear();
    w_ = h_ = 0;
    fmt_ = VK_FORMAT_UNDEFINED;
}

// ── SemaphorePool ────────────────────────────────────────────────────────────
std::shared_ptr<PooledSemaphore> SemaphorePool::acquire(VulkanContext& vk,
                                                        uint32_t slot, int semFd) {
    // Import semFd into a fresh imported timeline semaphore. Consumes semFd.
    auto importOne = [&](int fd) -> std::shared_ptr<PooledSemaphore> {
        if (fd < 0) return nullptr;
        auto pImportSem = vk.importSemaphoreFdKHR();
        if (!pImportSem) {
            ::close(fd);
            std::fprintf(stderr, "gmix: import: no vkImportSemaphoreFdKHR\n");
            return nullptr;
        }
        VkDevice dev = vk.device();
        auto s = std::make_shared<PooledSemaphore>();
        s->vk = &vk;
        VkSemaphoreTypeCreateInfo tci{};
        tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tci.initialValue = 0;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &tci;
        if (vkCreateSemaphore(dev, &sci, nullptr, &s->sema) != VK_SUCCESS) {
            ::close(fd);
            std::fprintf(stderr, "gmix: import: semaphore create\n");
            return nullptr;
        }
        VkImportSemaphoreFdInfoKHR isfi{};
        isfi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        isfi.semaphore = s->sema;
        isfi.flags = 0;
        isfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        isfi.fd = fd;
        if (pImportSem(dev, &isfi) != VK_SUCCESS) {
            // OPAQUE_FD import consumes the fd even on failure; the
            // PooledSemaphore dtor frees the (unimported) semaphore handle.
            std::fprintf(stderr, "gmix: import: semaphore import\n");
            return nullptr;
        }
        return s;
    };

    // Guard a corrupt/garbage slot from ballooning the cache (see FrameImagePool).
    constexpr uint32_t kMaxSlots = 256;
    if (slot >= kMaxSlots) return importOne(semFd);

    if (slot >= slots_.size()) slots_.resize(slot + 1);
    if (slots_[slot]) {                 // already imported this slot: reuse it
        if (semFd >= 0) ::close(semFd); // the new fd is the same persistent payload
        return slots_[slot];
    }
    auto s = importOne(semFd);          // consumes semFd
    if (s) slots_[slot] = s;
    return s;
}

void SemaphorePool::clear() {
    slots_.clear();
}

// ── ImportedFrame ────────────────────────────────────────────────────────────
ImportedFrame::~ImportedFrame() = default;   // image + semaphore freed by pools

bool ImportedFrame::init(VulkanContext& vk, std::shared_ptr<PooledImage> img,
                         std::shared_ptr<PooledSemaphore> sem, uint64_t semSignalValue) {
    vk_ = &vk;
    img_ = std::move(img);
    sem_ = std::move(sem);
    sigVal_ = semSignalValue;
    return img_ && sem_ && sem_->sema != VK_NULL_HANDLE;
}

} // namespace gmix
