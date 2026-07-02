# GMix — developer handoff / status

> Read this first if you're picking the project up. It captures the working
> state and the non-obvious things that were hard-won, so they don't get
> re-debugged from scratch.

**There is no CMake install step for the OBS plugin.** `cmake --build build`
only produces `build/obs_plugin/bin/64bit/obs-gmix-source.so` +
`data/locale/en-US.ini` — OBS never sees a rebuild until those are manually
copied to `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/
obs-gmix-source/{bin/64bit,data/locale}/` (see README's install section for
the exact `cp` commands). Bitten by this on 2026-07-02: two source fixes were
built but not copied, so a live-in-OBS test of the first one appeared to
fail when it had simply never been installed. Always re-copy (and restart
OBS, since it's not running) after rebuilding, before drawing any conclusion
from in-OBS behavior.

## FIXED (2026-07-03): gs_texture_t leak found via OBS log review

Reviewing an actual OBS session log (`~/.var/app/com.obsproject.Studio/config/
obs-studio/logs/`) after the multi-scene + smoothness + resize-hitch fixes
above turned up a real bug they introduced, CONFIRMED happening live (not
theoretical): two `gmix: dst[N] (staged for resize swap) imported...` triples
logged 4ms apart with an identical target resolution, i.e. two
`queue.pendingResize` events landed back-to-back before the first one's swap
into `s->tex[]` completed (likely osu! firing two swapchain-resize events for
the same target during a resolution change).

The resize handler in `workerMain()` responded to the SECOND event by doing
`for (auto& t : pendingTex) t = nullptr;` -- discarding the pointers to the
`gs_texture_t` objects the FIRST event had just imported, without calling
`gs_texture_destroy()` on them. Silent leak of an OBS texture (and its VRAM)
every time two resizes race like this. Same leak class also existed if a
producer disconnected while `awaitingSwap` was still true (pendingTex simply
goes out of scope on the next reconnect loop iteration, contents never freed
since `gs_texture_t*` is a raw pointer with no destructor).

**Fix:** both sites now destroy any non-null `pendingTex[]` entries (with
`obs_enter_graphics()`/`obs_leave_graphics()`) before discarding/reusing the
array -- the second-resize-race site in the `queue.pendingResize.exchange()`
block, and a new cleanup right after the inner dispatch loop exits (covers
disconnect-during-resize). No lock needed for either: `pendingTex` is
thread-local to the worker thread, unlike `s->tex` which video_render() also
reads.

**Lesson for next time:** the OBS log at `~/.var/app/com.obsproject.Studio/
config/obs-studio/logs/` is genuinely worth reading after a live test
session, not just asking the user "did it work" -- this bug produced no
crash, no visible artifact, and no user-facing symptom in a short recording
session; it would only compound into a real VRAM leak over a long streaming
session with a game that resizes its window/resolution repeatedly. Also
noted while reading logs: `blend_engine.cpp`'s own diagnostics
(`std::fprintf(stderr, "gmix: blend: ...")`) do NOT appear in the OBS log at
all (only `blog()`-based messages from `gmix_source.cpp` do) -- if you need
to debug something inside `BlendEngine` specifically, `stderr` isn't
captured here; you'd need to launch OBS from a terminal instead.

Verified: builds and links clean, `ctest` still 3/3 (76 checks) -- no
existing test exercises the resize-race path directly, this was caught by
log inspection, not a test. Not yet re-verified live (would need to
reproduce the same rapid-double-resize condition and confirm no more
"staged for resize swap" duplicate-fd pairs without an intervening destroy).

## ADDED (2026-07-03): blend presets, incl. the velocity-aware 'Advanced' path

Per user request, brought back the "optical awareness" motion blur that a
much earlier simplification pass deliberately stripped out (see
`gmix-project-variants` memory item (12): "strip everything, put everything
in flat, no oversampling" -- at the time it was standalone-`gmix`-only code,
before the OBS-plugin rewrite existed at all). It was ported from
`~/Documents/GMIX-project-Linux-V2` (the most recent copy that still had it),
NOT reintroduced from scratch, and adapted onto this repo's current
ASYNC/multi-buffered `BlendEngine::dispatchAsync()` design (V2's own
`dispatch()` was a fully synchronous single-buffer submit -- that part was
NOT ported, only the shader + weight-curve math).

**New OBS properties** on "GMix Motion Blur": **Blur preset** (dropdown:
Flat/Linear/Cinematic/Heavy/Advanced) and **Blur density** (int slider 4-32,
visible only when Advanced is selected via `obs_property_set_modified_callback2`
in `gmixGetProperties()`). See README's new "Blend presets" section for the
user-facing description of each.

**Files touched:**
- `shaders/resample_blur.comp` (new) -- verbatim port of V2's shader: same
  descriptor bindings (0=srcImages, 1=dstImage, 2=weights SSBO, all already
  shared with `blend.comp`, so NO new descriptor bindings were needed) and
  the same per-pixel optical-flow-proxy motion estimate + exponential-
  brightness-dominance accumulate. `CMakeLists.txt` compiles it alongside
  `blend.comp` (`gmix_shader_resample_blur` target).
- `src/blend/blend_engine.{hpp,cpp}` -- `ResampleParams` (namespace-scope, NOT
  nested in `BlendEngine`: a nested type's default member initializers aren't
  usable as a default FUNCTION ARGUMENT of another member of the same still-
  incomplete enclosing class -- hit this compile error firsthand, moved it out
  as the standard fix). `dispatchAsync()` gained a trailing `ResampleParams`
  parameter; `PushConstants` gained the resample-only fields (`subSamples`,
  `shutterStrength`, `falloff`, `texelSizeX/Y` -- `blend.comp`'s own PC struct
  just doesn't declare them, which is fine within the same push-constant
  range). `resamplePipeline_` builds from `resample_blur.spv` in
  `createPipeline()`, optional (missing SPIR-V just falls back to the plain
  pipeline, doesn't fail `init()`). Also fixed a latent bug found while in
  here: `dmaBufFd_[kDstBuffers] = { -1, -1 }` only -1-filled 2 of the 3 slots
  after the earlier kDstBuffers 2->3 bump (slot 2 defaulted to 0, a
  valid-looking fd, instead of -1) -- now zero-initialized in the header and
  explicitly -1-filled in the constructor body.
- `src/blend/weight_generator.{hpp,cpp}` -- added `PresetShape`
  {Linear,Cinematic,Heavy} + `generatePresetCurve`/`generateFromPreset`,
  ported from V2's danser-style `floor(1.0) + shape(t)*100` curves (a bare
  0..1 shape function effectively stops blending anything past ~6-8 frames --
  the +1.0 floor keeps every frame in the window meaningfully weighted). Did
  NOT port V2's `PresetType`/`parsePreset` CLI-string-parsing machinery --
  OBS's dropdown gives fixed string values directly, so `presetSettingToMode()`
  in `gmix_source.cpp` maps them straight to `BlendConfig::Mode`, no generic
  parser needed.
- `src/blend/BlendConfig.hpp` -- `Mode` gained `Linear/Cinematic/Heavy/Advanced`
  (was `Flat`/`Raw` only); added `blurDensity/shutterStrength/falloff` +
  `usesResamplePath()`; `weightsFor()` dispatches on the new modes.
- `src/obs_plugin/gmix_source.cpp` -- `GmixEngine` gained a mutex-guarded
  `blendConfig` (a property of the SHARED engine/pipeline, not any one
  `GmixSource`, matching how `gpuIndex` already only takes effect for the
  first instance -- all attached sources render the same one feed). The
  worker's dispatch loop takes a fresh locked snapshot of it every dispatch
  (previously a local `BlendConfig config` was declared once and NEVER
  actually wired to any settings -- always silently Flat). `gmixUpdate()`
  writes `target`/`gpuIndex` AND now `blendConfig` from the OBS properties;
  called a second time from `gmixCreate()` after `acquireEngine()` so a saved
  non-Flat preset (e.g. reloading a scene collection) takes effect
  immediately rather than defaulting to Flat until the dialog is next
  touched (s->engine is null on gmixUpdate's first call, before
  acquireEngine()).
- `tests/test_weight_generator.cpp` -- added coverage for the new preset
  curves (sum-to-one, Linear peaks at center, Heavy decays from newest) and
  `BlendConfig::usesResamplePath()`.

Verified: builds and links clean, `ctest` 76 checks / 0 failures (was ~15
checks pre-change). Plugin installed to OBS's flatpak plugin dir. **Not yet
verified live** -- in particular: (1) actually select Advanced in OBS and
confirm the directional-streak look renders (not just falls back silently to
plain blend on a `resamplePipeline_` build failure -- check the OBS log for
"resample_blur unavailable" if it looks like a plain blend), (2) confirm the
Blur density slider only shows for Advanced and actually changes the trail
density, (3) sanity-check Linear/Cinematic/Heavy visually differ from Flat.
Per the ORIGINAL V2 dev notes this shader "did NOT beat the plain shutter
blend" in a prior evaluation -- it's being brought back as a user-selectable
option/style choice this time, not as a default or a claimed strict
improvement, so that earlier verdict isn't a blocker, just context.

**2026-07-03 follow-up:** user confirmed the preset system works, but
`shutterStrength` (the Advanced path's exp() brightness-dominance exponent)
had a hardcoded default of 4.0 -- copied verbatim from V2's `BlendConfig`
default and never actually exposed in the OBS UI, so it also couldn't be
tuned down. 4.0 blows out brightness/contrast for most content. Fixed:
default dropped to 1.0 (`BlendConfig::shutterStrength` and
`BlendEngine::ResampleParams::shutterStrength` in `blend_engine.hpp`, kept in
sync though only the former actually reaches the OBS path) and added a
**Blur brightness** float slider (0.1-10.0, `kSettingBrightness` =
`"blur_brightness"` in `gmix_source.cpp`) alongside Blur density, same
Advanced-only visibility toggle in `gmixPresetModified()`. Not yet verified
live at the new 1.0 default or via the slider.

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

## FIXED (2026-07-02): smoothness/stability pass on the OBS plugin's dispatch loop

Code review of `src/obs_plugin/gmix_source.cpp`'s `workerMain()` turned up
several sources of judder/flicker, all fixed together since they were
entangled in the same loop:

1. **Blend window was frame-COUNT based, not time-window based**, contrary to
   the design principle above ("Shutter = fixed TIME window, not fixed frame
   count"). It derived `n = round(capFps / 60)` from an EMA-smoothed capture
   rate (`FrameQueue::captureFps()`, α=0.1), which lags real fps changes
   (game fps drops, loading screens) — during a transition the blended
   window's real time-span drifted from 1/60s, showing up as blur-amount
   flicker. **Fix:** `FrameQueue` no longer tracks an EMA rate at all; the
   dispatch loop now walks the ring from newest backwards and includes every
   frame whose `timestampNs` falls within the trailing shutter window,
   exactly. This also obsoletes a hysteresis fix that was considered for the
   old `round()`-near-a-boundary flicker — there's no rounding left to jitter.

2. **Two free-running ~60Hz clocks beating against each other.** The worker
   thread dispatched on its own timer, completely unsynced from OBS's actual
   render tick; `video_render()` just draws whatever `frontIdx` happens to be
   when OBS calls it. Two independent clocks at nominally the same rate beat
   against each other, producing periodic duplicate/stale-frame judder even
   though both were nominally 60Hz. **Fix:** `obs_source_info::video_tick` is
   now wired up (`gmixVideoTick()`); every attached `GmixSource` calls it once
   per real OBS render frame, which notifies `GmixEngine::tickCv` and records
   the real elapsed seconds into `GmixEngine::obsFrameSec`. The worker's
   per-iteration wait (previously `sleep_for(1ms)`) now blocks on that cv
   (woken by a tick OR a new capture arrival, 2ms safety-net timeout), so its
   dispatch decisions happen in lockstep with OBS's actual render clock.

3. **Hardcoded 60Hz assumption.** Both the shutter width and the dispatch
   throttle interval were a literal `1'000'000'000/60`. If OBS's canvas/output
   is configured for anything other than 60fps (30, 144, ...), the internal
   cadence still targeted 60Hz, causing steady-state duplication/skipping
   against OBS's real output rate. **Fix:** both now read
   `GmixEngine::obsFrameSec`, seeded once from `obs_get_video_info()` at
   engine creation and kept live by `gmixVideoTick()`'s `seconds` argument, so
   they track OBS's actually configured FPS.

Verified live in OBS (2026-07-02): user confirmed functional and smooth,
with room left to improve further (see next section).

## FIXED (2026-07-02): two more smoothness items from a follow-up research pass

A background research agent (internet + code) was asked what else could be
improved after the above pass shipped and tested smooth-but-improvable. It
flagged two concrete, code-confirmed items (full report not preserved here,
just the outcome):

1. **Double-buffer write-after-read hazard.** `pollBlendDone()` (a host-side
   Vulkan timeline query) only proves gmix's OWN compute write finished --
   nothing proves OBS's graphics queue has actually finished READING the
   previous front buffer via its dma-buf-imported `gs_texture_t` by the time
   a strict 2-buffer ping-pong reuses it as the next write target (there is
   no real cross-queue/cross-process fence on that read). **Fix:**
   `BlendEngine::kDstBuffers` raised from 2 to 3 (`blend_engine.hpp`), and the
   dispatch target in `workerMain()` changed from a front/back toggle to a
   round-robin (`nextWriteIdx`, cycling 0→1→2→0...), giving one full extra
   dispatch generation of grace before a buffer is reused. Cheap (one more
   capture-resolution RGBA8 image); does not formally eliminate the race
   (no real fence exists for it), just widens the margin.

2. **Resize mid-stream blanked the source.** On `queue.pendingResize`, the
   old code nulled `s->frontIdx`/destroyed `s->tex[]` immediately, then
   `blend->init()` tore down and rebuilt gmix's dst images at the new size --
   so `video_render()` drew nothing for the whole re-init+first-blend window.
   **Fix:** new textures are now imported into a local `pendingTex[]` staging
   array (not touching the live `s->tex[]`) while `awaitingSwap` is true;
   `video_render()` keeps drawing the OLD front buffer, at the old size, the
   whole time -- this is safe because dma-buf memory is kernel/GEM-refcounted
   across every importer, so OBS's existing texture stays valid even after
   gmix's own `VkDeviceMemory` behind it is freed and reallocated by
   `blend->init()`. The swap into `s->tex[]` (destroying the old textures,
   updating `s->width/height`) happens atomically inside the first
   post-resize `pollBlendDone()` == true branch.

Verified: builds and links clean, `ctest` still 3/3 green (no test exercises
either the buffer-count or the resize path directly). **Not yet verified live
in OBS** — in particular, actually trigger a resize (e.g. toggle osu!
fullscreen/windowed mid-capture) to confirm no hitch, and general smoothness
watching for whether the 3-buffer change actually reduced flicker.

## Likely next directions (not yet built)

- Producer-side oversample-equivalent / lower import cost so the shutter window
  packs more distinct frames during very high game fps (currently bounded by
  consumer import throughput, see note 2).
- Wire the not-yet-used `LayerIpc` notification socket or remove it.
- Make `-blur-ms` adjustable without relaunch (e.g. read a control file).

## FIXED (2026-07-02): GMix only rendered in the first scene it was added to

**Fix applied:** implemented option (a) below. `src/obs_plugin/gmix_source.cpp`
now has a process-wide, ref-counted `GmixEngine` singleton (`acquireEngine()`/
`releaseEngine()`, guarded by `gEngineMu`) that owns the one `FrameReceiver` +
`BlendEngine` + worker thread + imported `gs_texture_t` set. `GmixSource`
(the per-`obs_source_info`-instance struct) is now just a thin handle holding
settings plus a pointer into the shared engine; `gmixCreate()` calls
`acquireEngine()`, `gmixDestroy()` calls `releaseEngine()`, and
`gmixVideoRender()`/`gmixGetWidth()`/`gmixGetHeight()` all read through
`s->engine`. First source to be created spins up the engine (and owns its
`gpuIndex`); later sources just attach and share the same live feed; the
engine tears down when the last source is destroyed. Verified: builds and
links clean. Not yet re-tested live in OBS with 2+ scenes (do that before
calling this fully closed — see "still to verify" below).

**Still to verify:** launch OBS, add "GMix Motion Blur" via `+` to two
different scenes (two independent instances, not "Add Existing Source"), and
confirm both scenes render the live feed with no `listen` failure in the OBS
log.

<details>
<summary>Original bug report (kept for history)</summary>

**Symptom:** add "GMix Motion Blur" to scene A -> works. Add "GMix Motion
Blur" to scene B (via `+` -> GMix Motion Blur again, creating a SECOND
source) -> scene B stays black/blank. Deleting the source from whichever
scene isn't working and re-adding it can get it going again, but only one
scene at a time ever actually displays gameplay.

**Suspected root cause:** every `GmixSource` instance's worker thread calls
`gmix::ipc::FrameReceiver::listen()` on the same well-known, hardcoded unix
socket path (`gmix::ipc::defaultFrameSocketPath()`, i.e.
`~/.cache/gmix/frames.sock`) — see `workerMain()` in
`src/obs_plugin/gmix_source.cpp`. `bind()` on a given path can only succeed
for ONE listener at a time. Using OBS's `+` button to add "GMix Motion
Blur" in a second scene creates a **second, independent** `obs_source_info`
instance (its own `create()`/worker thread/receiver), not a reference to the
first one — so the second instance's `listen()` call fails (the socket is
already bound by the first instance), and it just sits forever logging
"waiting for producer to connect" while never actually receiving anything
(the OBS log should confirm this — check for a `listen` failure or a stuck
"waiting for producer" line with no matching "producer connected" for the
second instance).

**Workaround for users (put in README):** don't create a second "GMix
Motion Blur" source per scene. Instead, add the SAME existing source to
additional scenes: right-click the target scene -> **Add Existing Source**
(or drag the source from the Sources list while holding the scene target) ->
pick the existing "GMix Motion Blur" source. This reuses the one
`GmixSource` instance (and its one socket listener) across scenes, since a
single OBS source can be a member of multiple scenes simultaneously.

**Real fix (not yet done):** either (a) make the plugin itself
single-instance — e.g. a process-wide static/singleton owning the one
receiver+blend pipeline, with each `GmixSource` `create()` call just
attaching to it as a viewer (ref-counted), so creating N source objects is
harmless and all of them render the same live feed; or (b) have
`listen()`'s failure path fall back to a `connect()` attempt against an
already-listening sibling instance's socket (more complex, not recommended
over (a)). Option (a) is the natural fix and should be done before this
ships more broadly — filed here instead of fixed immediately per user
request ("will look at that later on").

</details>
