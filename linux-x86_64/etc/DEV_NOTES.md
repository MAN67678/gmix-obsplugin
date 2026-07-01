# GMix — developer handoff / status

> Read this first if you're picking the project up. It captures the working
> state and the non-obvious things that were hard-won, so they don't get
> re-debugged from scratch.

## What works right now (v1)

End-to-end, verified against real osu! on Linux/Wayland/Vulkan (RADV, RX 480):

    capture layer (inside osu!) → OPAQUE_FD over unix socket → gmix consumer
    → sliding-window ring → 1/60s shutter blend (compute) → Wayland 60fps out

- Real per-frame temporal motion blur. Clean 60fps output, OBS-capturable.
- End-to-end latency ~11 ms, **steady** (shown live on the status line).
- Blur amount = a virtual camera shutter, `-blur-ms` (default **16.6** = 1/60s).

### Run it

    ./build/gmix --install-layer                 # one-time per build
    ./build/gmix -i search=osu! -o w1280 h1024 -preset=flat   # consumer
    ENABLE_GMIX=1 ./osu\!/osu.AppImage           # producer (any order; auto-reconnects)

Relaunch only `gmix` to retune live — the layer in osu! reconnects on its own.
Tests: `ctest --test-dir build` (4 tests, all green; they exercise the simple
blend path only).

## The non-obvious things (don't re-discover these)

1. **Producer MUST use a RING of export images** (`kExportRing = 48` in
   `VulkanLayerCapture` / `FrameSource.hpp`), NOT one persistent image.
   OPAQUE_FD export *aliases* the underlying memory with the consumer's import,
   so re-exporting a single image every frame made all N frames in the
   consumer's blend window point at the SAME memory = only the latest pixels =
   averaging N identical images = **no motion blur**. This was THE root cause of
   "blur doesn't show." The ring must exceed the consumer's max blend window
   (32) + in-flight margin, hence 48. (~240 MB VRAM @1080p on osu!'s device.)

2. **Consumer must drop socket backlog** (`FrameReceiver::hasPendingFrame()`
   used in `receiverThreadFn`). The producer exports up to ~1000 fps but
   per-frame import (vkCreateImage + memory/semaphore import) is slower, so
   without dropping, frames pile up and latency grows **unbounded (~1 s per
   second)**. The receiver now imports only the freshest frame and cheaply
   discards backlog (close fds, skip import). This is what keeps latency ~11ms.

3. **Shutter = fixed TIME window, not fixed frame count.** Selection is by
   `timestampNs` in the tick loop (`shutterMs`, `-blur-ms`). A fixed frame count
   gives wildly inconsistent blur because the capture rate varies with the
   game's fps (32 frames span 30 ms at 1000fps but 550 ms at 55fps). Fixed time
   = consistent blur, naturally ~no blur on idle/menu screens.

4. **The blend math is a plain normalized weighted sum** (`blend.comp`). That is
   literally what danser (the reference osu! renderer) does — danser's blur
   quality comes from dense sub-frame *oversampling*, not fancy math. With real
   captured frames, the weight CURVE controls trail length: peaked (Heavy's
   one-sided decay) = short trail; flat = longest. Don't add cleverness here
   expecting more blur; feed it more distinct frames instead.

5. **No DRM format modifier on this GPU.** `VK_EXT_image_drm_format_modifier`
   is absent on RADV/Polaris (confirmed via vulkaninfo; only llvmpipe has it).
   So the shared image uses plain `VK_IMAGE_TILING_LINEAR` and the producer's
   actual `rowPitch` (from `vkGetImageSubresourceLayout`) is sent to the
   consumer in the frame header. Don't reintroduce a modifier path.

6. **Known pre-existing validation warning:** `VUID-vkCmdDraw-None-09600`
   (image-layout) fires on the *simple* path too — it predates the blur work
   and is not caused by it. Low priority; investigate separately.

## Open item: the experimental `-resample` velocity path

There's a SECOND, velocity-aware blur path (per-pixel optical-flow proxy +
directional sub-sampling): `shaders/resample_blur.comp`, the
`resamplePipeline_` in `blend_engine`, `BlendConfig::useResample` /
`usesResamplePath()`, and the `-resample` / `-subsamples` / `-falloff` /
`-shutter` CLI flags. It is **off by default** and gated behind `-resample`.

**Status: did NOT beat the plain shutter blend** and is not the v1 path. It was
left in (off) rather than risk a removal-regression at the finish line.

**Decision pending — pick one:**
- KEEP as an experiment (current state; harmless, off by default), or
- STRIP it cleanly: remove `resample_blur.comp` (+ its `gmix_compile_shader`
  line in CMakeLists), `resamplePipeline_` and the `ResampleParams` plumbing in
  `blend_engine.{hpp,cpp}`, the `useResample/subSamples/shutterStrength/falloff`
  fields + helpers in `BlendConfig.hpp`, and the `-resample/-subsamples/
  -falloff/-shutter` flags in `cli.cpp`. The plain weighted blend + `-blur-ms`
  is the whole feature; nothing else depends on the resample path.

## Likely next directions (not yet built)

- Producer-side oversample-equivalent / lower import cost so the shutter window
  packs more distinct frames during very high game fps (currently bounded by
  consumer import throughput, see note 2).
- Wire the not-yet-used `LayerIpc` notification socket or remove it.
- Make `-blur-ms` adjustable without relaunch (e.g. read a control file).
