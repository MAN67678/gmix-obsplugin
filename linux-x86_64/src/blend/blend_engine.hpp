// ─────────────────────────────────────────────────────────────────────────────
// GMix blend engine — wraps the temporal-blend compute shader.
//
// On each output frame the present loop calls dispatch() with up to N source
// image views and their pre-normalized weights. The result lands in the
// engine's persistent dst image, which the presenter then blits to the
// swapchain. srcCount may vary per call (dynamic N).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace gmix {

class VulkanContext;

class BlendEngine {
public:
    explicit BlendEngine(VulkanContext& vk);
    ~BlendEngine();

    BlendEngine(const BlendEngine&) = delete;
    BlendEngine& operator=(const BlendEngine&) = delete;

    // Lazily create the pipeline + dst image sized for (w,h). Call after the
    // output dimensions are known (and again if they change).
    bool init(uint32_t w, uint32_t h);

    // Blend `srcCount` source images with the given weights into dst.
    // srcViews:    pointer to srcCount VkImageView handles (each a 2D view of
    //              an imported source frame, format BGRA8/RGBA8). Index 0 is
    //              the newest frame.
    // weights:     srcCount pre-normalized weights (Σ ≈ 1).
    // srcCount:    1..MAX_FRAMES. 1 → passthrough (just copies src 0).
    // Synchronous: submits the blend and blocks until it completes (tests /
    // readback). Returns dst buffer 0's view. The pipelined present path uses
    // dispatchAsync() below instead.
    VkImageView dispatch(VkImageView* srcViews, const float* weights,
                         uint32_t srcCount);

    // ── Pipelined (asynchronous) dispatch ──────────────────────────────────
    // The dst is DOUBLE-BUFFERED so the present thread can keep blitting the
    // last finished result (the "front") while a new blend runs into the other
    // buffer (the "back"). dispatchAsync() records + submits into dst[dstIdx]
    // and returns WITHOUT waiting; the caller polls pollBlendDone() on later
    // ticks and, once true, treats dstIdx as the new front. This decouples the
    // present cadence from the (variable, sometimes >16.6ms) blend cost: a slow
    // blend just means the front is re-presented an extra tick, never a stall.
    // Only one blend may be in flight at a time (single fence/cmd buffer);
    // call dispatchAsync only when blendInFlight() is false.
    //
    // waitSems/waitVals/waitCount: timeline semaphores (the source frames'
    // producer render-complete signals) the compute submit waits BEFORE reading
    // the sources -- so producer timing is handled on the GPU, not by a host
    // wait on the present thread. Pass 0 for none (tests).
    static constexpr uint32_t kDstBuffers = 2;
    bool dispatchAsync(VkImageView* srcViews, const float* weights,
                       uint32_t srcCount, uint32_t dstIdx,
                       const VkSemaphore* waitSems = nullptr,
                       const uint64_t* waitVals = nullptr, uint32_t waitCount = 0);
    bool blendInFlight() const { return blendInFlight_; }
    // True once the in-flight blend's GPU work has completed (non-blocking).
    // Clears the in-flight flag as a side effect when it returns true.
    bool pollBlendDone();
    // Block until the in-flight blend completes (used on resize/teardown).
    void waitBlendDone();

    // Timeline semaphore the blend signals on the compute queue. The presenter
    // must wait it for `dstReadyValue()` on the graphics queue before blitting
    // the front buffer, so the blend's writes are visible across the queues.
    VkSemaphore blendTimeline()  const { return blendTimeline_; }
    // The timeline value reached by the blend that is currently the front buffer
    // (i.e. the value to wait for when presenting it). Updated as a side effect
    // of pollBlendDone()/waitBlendDone() retiring an in-flight blend.
    uint64_t    dstReadyValue()  const { return frontValue_; }

    VkImage    dstImage()              const { return dstImage_[0]; }
    VkImageView dstView()              const { return dstView_[0]; }
    VkImage    dstImage(uint32_t idx)  const { return dstImage_[idx]; }
    VkImageView dstView(uint32_t idx)  const { return dstView_[idx]; }
    uint32_t   width()        const { return width_; }
    uint32_t   height()       const { return height_; }

    // Read back the dst image into a caller-provided host buffer (RGBA8,
    // tightly packed, width*height*4 bytes). Synchronously waits for the
    // last dispatch. Used by tests; the presenter does its own blit.
    bool readbackDst(uint8_t* outPixels);

    // ── Zero-copy dma-buf export (for the OBS plugin) ──────────────────────
    // True once init() has confirmed R8G8B8A8_UNORM supports LINEAR+STORAGE
    // on this device and both dst buffers were successfully exported.
    bool dmaBufCapable() const { return dmaBufCapable_; }
    // Exported fd for dst buffer `idx` (0 or 1), owned by BlendEngine -- do
    // not close it; dup() it if the caller needs an independent lifetime.
    // -1 if !dmaBufCapable() or the export failed for that buffer.
    int      dmaBufFd(uint32_t idx)     const { return dmaBufFd_[idx]; }
    uint32_t dmaBufStride(uint32_t idx) const { return dmaBufStride_[idx]; }
    uint64_t dmaBufOffset(uint32_t idx) const { return dmaBufOffset_[idx]; }

private:
    bool createDstImage();
    bool createDescriptorSet();
    bool createPipeline();
    void destroyTransient();
    void destroyAll();

    VulkanContext& vk_;
    VkDescriptorSetLayout dsLayout_     = VK_NULL_HANDLE;
    VkPipelineLayout      pipeLayout_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_     = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_     = VK_NULL_HANDLE;
    VkDescriptorSet       descSet_      = VK_NULL_HANDLE;

    VkCommandPool         cmdPool_      = VK_NULL_HANDLE;
    // One persistent command buffer, reset+reused per dispatch (only ever
    // re-recorded once the previous dispatch's fence has signaled), instead of
    // the old alloc/free-every-frame churn that added per-tick jitter.
    VkCommandBuffer       cmd_          = VK_NULL_HANDLE;

    VkImage        dstImage_[kDstBuffers] = {};
    VkDeviceMemory dstMem_[kDstBuffers]   = {};
    VkImageView    dstView_[kDstBuffers]  = {};
    VkSampler  dstSampler_  = VK_NULL_HANDLE;   // not needed; kept for future blit
    bool       blendInFlight_ = false;

    // dma-buf export state (see dmaBufCapable()/dmaBufFd() above).
    bool     dmaBufCapable_          = false;
    int      dmaBufFd_[kDstBuffers]     = { -1, -1 };
    uint32_t dmaBufStride_[kDstBuffers] = {};
    uint64_t dmaBufOffset_[kDstBuffers] = {};

    // Weights UBO (binding 2). Persistently mapped; rewritten per dispatch.
    VkBuffer       weightsBuf_  = VK_NULL_HANDLE;
    VkDeviceMemory weightsMem_  = VK_NULL_HANDLE;
    float*         weightsMapped_ = nullptr;

    // Timeline semaphore signaled by each blend on the compute queue; the
    // present blit waits the relevant value on the graphics queue. Replaces the
    // old per-dispatch fence + binary semaphore (a fence gives no cross-queue
    // memory dependency; a timeline does, and can be waited repeatedly so the
    // same front buffer can be re-presented for multiple ticks).
    VkSemaphore blendTimeline_ = VK_NULL_HANDLE;
    uint64_t    blendValue_     = 0;   // last value submitted (monotonic)
    uint64_t    inFlightValue_  = 0;   // value the in-flight blend will signal
    uint64_t    frontValue_     = 0;   // value of the last retired (front) blend

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;
};

} // namespace gmix
