// ─────────────────────────────────────────────────────────────────────────────
// GMix temporal blend compute shader (Windows/D3D11). HLSL port of
// linux-x86_64/shaders/blend.comp -- same math, same design constraints:
//
//   out_pixel = Σ (wᵢ · src_pixelᵢ)         (weights pre-normalized: Σwᵢ ≈ 1)
//
// Sources are the imported ring-slot SRVs (see ipc/imported_frame.hpp), read
// with a plain Load() (no sampler filtering) for a pixel-exact accumulate,
// same reasoning as the Vulkan side's imageLoad. One thread per output texel,
// 8x8 groups, matching blend.comp's local_size_x/y.
//
// Indexing note: SM5.0/fxc requires a resource-ARRAY index (srcImages[i]) to
// be a true compile-time literal -- NOT just a loop-invariant-provable value,
// even inside an [unroll]'d loop (`for (uint i = 0; i < MAX_FRAMES; ++i)`
// with a runtime `if (i >= frameCount) break;` still fails to compile: "array
// index must be a literal expression"). SM5.1's dynamic resource indexing
// would fix this cleanly but isn't guaranteed available under D3D11/fxc, so
// instead the accumulate is expanded via the SAMPLE(k) macro below into
// MAX_FRAMES separate statements, each with a literal k -- mechanical, but
// the only approach fxc reliably accepts for "N of a fixed max resource
// array" on D3D11.
// ─────────────────────────────────────────────────────────────────────────────

#define MAX_FRAMES 64   // must match kMaxBlendFrames in src/gmix.hpp

Texture2D<float4>       srcImages[MAX_FRAMES] : register(t0);
RWTexture2D<float4>     dstImage              : register(u0);
StructuredBuffer<float> weights               : register(t64);

cbuffer PushConstants : register(b0) {
    uint frameCount;   // valid sources in srcImages[0..frameCount-1]
    uint frameW;
    uint frameH;
    uint _pad0;
};

// Accumulates srcImages[k] * weights[k] into `acc` iff k < frameCount, using
// a literal k so fxc accepts the resource-array index. Guards against stale
// weight-buffer contents beyond frameCount too (the buffer is only ever
// partially rewritten per dispatch -- see blend_engine.cpp's dispatchAsync()).
#define SAMPLE(k, pix, acc) \
    if ((k) < frameCount) { \
        float w = weights[k]; \
        if (w != 0.0) (acc) += w * srcImages[k].Load(int3((pix), 0)); \
    }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= frameW || dtid.y >= frameH) return;

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    SAMPLE(0, dtid.xy, acc)  SAMPLE(1, dtid.xy, acc)  SAMPLE(2, dtid.xy, acc)  SAMPLE(3, dtid.xy, acc)
    SAMPLE(4, dtid.xy, acc)  SAMPLE(5, dtid.xy, acc)  SAMPLE(6, dtid.xy, acc)  SAMPLE(7, dtid.xy, acc)
    SAMPLE(8, dtid.xy, acc)  SAMPLE(9, dtid.xy, acc)  SAMPLE(10, dtid.xy, acc) SAMPLE(11, dtid.xy, acc)
    SAMPLE(12, dtid.xy, acc) SAMPLE(13, dtid.xy, acc) SAMPLE(14, dtid.xy, acc) SAMPLE(15, dtid.xy, acc)
    SAMPLE(16, dtid.xy, acc) SAMPLE(17, dtid.xy, acc) SAMPLE(18, dtid.xy, acc) SAMPLE(19, dtid.xy, acc)
    SAMPLE(20, dtid.xy, acc) SAMPLE(21, dtid.xy, acc) SAMPLE(22, dtid.xy, acc) SAMPLE(23, dtid.xy, acc)
    SAMPLE(24, dtid.xy, acc) SAMPLE(25, dtid.xy, acc) SAMPLE(26, dtid.xy, acc) SAMPLE(27, dtid.xy, acc)
    SAMPLE(28, dtid.xy, acc) SAMPLE(29, dtid.xy, acc) SAMPLE(30, dtid.xy, acc) SAMPLE(31, dtid.xy, acc)
    SAMPLE(32, dtid.xy, acc) SAMPLE(33, dtid.xy, acc) SAMPLE(34, dtid.xy, acc) SAMPLE(35, dtid.xy, acc)
    SAMPLE(36, dtid.xy, acc) SAMPLE(37, dtid.xy, acc) SAMPLE(38, dtid.xy, acc) SAMPLE(39, dtid.xy, acc)
    SAMPLE(40, dtid.xy, acc) SAMPLE(41, dtid.xy, acc) SAMPLE(42, dtid.xy, acc) SAMPLE(43, dtid.xy, acc)
    SAMPLE(44, dtid.xy, acc) SAMPLE(45, dtid.xy, acc) SAMPLE(46, dtid.xy, acc) SAMPLE(47, dtid.xy, acc)
    SAMPLE(48, dtid.xy, acc) SAMPLE(49, dtid.xy, acc) SAMPLE(50, dtid.xy, acc) SAMPLE(51, dtid.xy, acc)
    SAMPLE(52, dtid.xy, acc) SAMPLE(53, dtid.xy, acc) SAMPLE(54, dtid.xy, acc) SAMPLE(55, dtid.xy, acc)
    SAMPLE(56, dtid.xy, acc) SAMPLE(57, dtid.xy, acc) SAMPLE(58, dtid.xy, acc) SAMPLE(59, dtid.xy, acc)
    SAMPLE(60, dtid.xy, acc) SAMPLE(61, dtid.xy, acc) SAMPLE(62, dtid.xy, acc) SAMPLE(63, dtid.xy, acc)

    // Source frames come from an OPAQUE game swapchain -- their alpha channel
    // is unspecified/arbitrary. The blend's output must always be fully
    // opaque regardless of what the accumulated alpha came out to, otherwise
    // OBS's texture compositing renders it (partially) transparent -- same
    // reasoning as blend.comp's final acc.a = 1.0.
    acc.a = 1.0;
    dstImage[dtid.xy] = acc;
}
