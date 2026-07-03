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

// Velocity-aware ("optical awareness") motion blur params -- the 'Advanced'
// preset. Routes the dispatch to resample_blur.comp instead of the plain
// weighted average (blend.comp): estimates a per-pixel motion vector from the
// newest frame pair and smears each real frame's pixels along it, filling the
// gaps between sparse real positions into a continuous directional streak.
// `weights` passed to dispatchAsync() is ignored on this path (recency
// weighting is computed in-shader from `falloff`).
//
// Namespace-scope, not nested in BlendEngine: a nested type's default member
// initializers aren't usable as a default FUNCTION ARGUMENT of another member
// of the same still-incomplete enclosing class (they're only valid in a
// complete-class context, i.e. after BlendEngine itself finishes) -- moving
// it out here is the standard fix, not a design preference.
struct ResampleParams {
    bool     enabled         = false;
    uint32_t subSamples      = 4;      // "blur density": taps per frame, 4..32
    float    shutterStrength = 1.0f;   // exp() brightness-dominance
    float    falloff         = 1.0f;   // recency falloff exponent
};

class BlendEngine {
public:
    explicit BlendEngine(VulkanContext& vk);
    ~BlendEngine();

    BlendEngine(const BlendEngine&) = delete;
    BlendEngine& operator=(const BlendEngine&) = delete;

    // Lazily create the pipeline + dst image sized for (w,h). Call after the
    // output dimensions are known (and again if they change).
    // dstBufferCount: how many dst images to multi-buffer (see the
    // dstBufferCount()/kMinDstBuffers/kMaxDstBuffers comment below for what
    // this trades off) -- the "Latency mode" OBS
    // setting (Fast/Medium/Slow/Very slow -> 2/3/4/5). Fixed for the engine's
    // whole lifetime once chosen (like gpuIndex): re-init() on a resize
    // reuses whatever count was passed the FIRST time, since callers pass
    // the same value on every call in practice; clamped to
    // [kMinDstBuffers, kMaxDstBuffers].
    bool init(uint32_t w, uint32_t h, uint32_t dstBufferCount = kDefaultDstBuffers);

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
    // The dst is multi-buffered (dstBufferCount(), see below) so a consumer
    // can keep reading the last finished result (the "front") while a new
    // blend runs into
    // another buffer. dispatchAsync() records + submits into dst[dstIdx] and
    // returns WITHOUT waiting; the caller polls pollBlendDone() on later
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
    //
    // pollBlendDone() (a host-side timeline query) only proves gmix's OWN
    // compute write finished -- for the OBS-plugin consumer, nothing proves
    // the consumer's GPU has actually finished READING the previous front
    // buffer by the time it's cycled back around as a write target
    // (video_render() just records a draw call; there's no real cross-queue/
    // cross-process fence on that read at all). A strict 2-buffer ping-pong
    // reuses a buffer on the very next dispatch after it stops being front --
    // zero grace. Round-robining across MORE buffers (see the dispatch-target
    // selection in gmix_source.cpp's workerMain()) gives that many EXTRA
    // dispatch generations of grace before reuse -- cheap (one more
    // capture-resolution RGBA8 image per step) insurance against that race,
    // and separately, slack for the blend's own cost to vary (GPU contention
    // with the game, thermal/scheduling drift over a long session) without a
    // slow blend's dst write racing a still-in-progress OBS read. This is the
    // "Latency mode" OBS setting's actual mechanism: more buffers = more
    // tolerance for timing variance, at the cost of (a) more VRAM (one
    // capture-resolution RGBA8 image each) and (b) the theoretical worst-case
    // staleness of the front buffer growing by one more dispatch interval.
    // Fast=2 (tightest, least tolerance -- the pre-fix behavior), Medium=3
    // (default), Slow=4, Very slow=5.
    static constexpr uint32_t kMinDstBuffers     = 2;
    static constexpr uint32_t kMaxDstBuffers     = 5;
    static constexpr uint32_t kDefaultDstBuffers = 3;
    // Actual count in effect for THIS engine instance, set by init(). Loops
    // that used to iterate the old compile-time kDstBuffers now iterate this.
    uint32_t dstBufferCount() const { return numDstBuffers_; }
    bool dispatchAsync(VkImageView* srcViews, const float* weights,
                       uint32_t srcCount, uint32_t dstIdx,
                       const VkSemaphore* waitSems = nullptr,
                       const uint64_t* waitVals = nullptr, uint32_t waitCount = 0,
                       ResampleParams resample = {});
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
    // Optional: if resample_blur.spv is missing/fails to build, the engine
    // still works -- dispatchAsync() with resample.enabled just falls back to
    // the plain pipeline_ rather than failing init().
    VkPipeline            resamplePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_     = VK_NULL_HANDLE;
    VkDescriptorSet       descSet_      = VK_NULL_HANDLE;

    VkCommandPool         cmdPool_      = VK_NULL_HANDLE;
    // One persistent command buffer, reset+reused per dispatch (only ever
    // re-recorded once the previous dispatch's fence has signaled), instead of
    // the old alloc/free-every-frame churn that added per-tick jitter.
    VkCommandBuffer       cmd_          = VK_NULL_HANDLE;

    // Sized to numDstBuffers_ (set by init()), not a compile-time constant --
    // see the "Latency mode" comment on dstBufferCount() above. std::vector
    // instead of a fixed-size array specifically so that sizing is a runtime
    // decision, not baked into the type.
    uint32_t numDstBuffers_ = kDefaultDstBuffers;
    std::vector<VkImage>        dstImage_;
    std::vector<VkDeviceMemory> dstMem_;
    std::vector<VkImageView>    dstView_;
    VkSampler  dstSampler_  = VK_NULL_HANDLE;   // not needed; kept for future blit
    bool       blendInFlight_ = false;

    // dma-buf export state (see dmaBufCapable()/dmaBufFd() above). Resized to
    // numDstBuffers_ and -1-filled (dmaBufFd_ only) in createDstImage(),
    // BEFORE any slot is assigned a real fd -- a not-yet-exported slot must
    // read back -1, never 0 (a valid-looking fd number), which is exactly
    // the bug a fixed-size `{ -1, -1 }` initializer list caused here before
    // (silently zero-filled any slot beyond the ones literally listed).
    bool                  dmaBufCapable_ = false;
    std::vector<int>      dmaBufFd_;
    std::vector<uint32_t> dmaBufStride_;
    std::vector<uint64_t> dmaBufOffset_;

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
