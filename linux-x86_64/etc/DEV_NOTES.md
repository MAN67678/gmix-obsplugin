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

## FIXED (2026-07-03): Cinematic preset washed out to a flat smear at real gameplay frame counts

User reviewed recorded footage and reported Cinematic "look less cinematic
then last version, like clearly just ghosting." Root cause confirmed with
real numbers, not just eyeballing: `generatePresetCurve()`'s Cinematic case
used a FIXED gaussian `mult=1.5` in normalized `t ∈ [0,1]` space. That's a
fixed RELATIVE width -- as `N` (real frames landing in the shutter window,
routinely 30-64 during actual high-fps gameplay) grows, the gaussian doesn't
get narrower in ABSOLUTE frame-count terms, it just gets sampled more
finely across the same relative shape. Measured: the center frame's share
of total blend weight fell from 23% at N=8 to just 2.8% at N=64 -- at
realistic gameplay N, no part of the window meaningfully dominated, so it
read as a flat smear instead of a soft photographic falloff with a visible
recent-ish peak.

**Fix:** `mult` now scales with `N` (`mult = 1.5 * (N / 16.0)`, in
`src/blend/weight_generator.cpp`) so the gaussian's width stays roughly
constant in ABSOLUTE frame count instead of relative position -- center
share now holds steady around ~10-15% regardless of N (verified: 8→15.4%,
16→11.3%, 32→10.6%, 64→10.1%, vs. the old 23.2%/11.3%/5.6%/2.8%). Linear and
Heavy were NOT touched -- Linear is deliberately full-window-span by design
(not meant to be sharply peaked), and Heavy's one-sided exponential `tau =
N/4` already scales with N the same way Cinematic needed to.

Added a regression test (`preset_cinematic_center_share_stable_across_N` in
`tests/test_weight_generator.cpp`) asserting the peak weight stays above 8%
of the curve's max across N E {8,16,32,64} -- would have caught the original
fixed-mult collapse at N=64 (2.8%, well under the 8% floor).

Verified: builds and links clean, `ctest` 3/3 (80 checks, up from 76 --
new test added). Installed while OBS was RUNNING -- safe on Linux, needs a
restart to take effect. **Not yet verified live** -- next test should show
Cinematic holding a visibly softer-but-still-present peak/falloff during
real gameplay recording, not the flat ghosting reported before this fix.

## FIXED (2026-07-03): lock-order inversion causing OBS "not responding" spam

User reported OBS's window manager/compositor repeatedly flagging OBS as
"not responding" despite it actually continuing to work fine afterward --
asked to check whether it was OBS itself or "the integration" (this plugin).

Found a real, textbook lock-order inversion in `gmix_source.cpp`: THREE
places held `s->texMu` (this plugin's own mutex, guarding `s->tex[]`) WHILE
calling into OBS's graphics API (`obs_enter_graphics()` /
`gs_texture_destroy()` / `gs_texture_create_from_dmabuf()`) --
`releaseEngine()`'s teardown, the non-resize import branch, and the
resize-swap block. Meanwhile `gmixVideoRender()` runs on OBS's OWN render
thread, which already holds OBS's graphics context by the time OBS calls it,
and then tries to acquire `texMu`. That's the two locks acquired in opposite
order on two different threads -- the classic AB-BA pattern: worker thread
holds `texMu`, waits on OBS's graphics context; OBS's render thread holds
the graphics context (implicitly, mid-frame), waits on `texMu`. Under
contention this stalls OBS's render thread for a stretch long enough to
trip a compositor's unresponsive-app heuristic, without being a permanent
deadlock (matches the reported symptom: repeated warnings, but it "keeps
working" afterward -- consistent with contention/stalling rather than a
true unbreakable cycle, though the risk of a genuine hang was real too).

**Fix, same pattern in all three spots:** never hold `texMu` across a call
into OBS's graphics API. For destruction: capture the old texture pointers
under a brief `texMu` lock (pointer-only, no graphics calls), release the
lock, THEN call `obs_enter_graphics()`/`gs_texture_destroy()` on the
captured pointers with the lock already released. For import (creation):
snapshot `s->tex[]` under a brief lock, call `importDstBuffers()` on the
UNLOCKED snapshot (which is where `obs_enter_graphics()`/
`gs_texture_create_from_dmabuf()` happen), then merge newly-created
pointers back into `s->tex[]` under another brief lock (pointer-only again).
`texMu` now only ever guards short, graphics-API-free pointer operations.

(The two pendingTex-only cleanup sites -- the second-resize-race fix and the
post-disconnect cleanup, both added earlier this session -- were already
safe: pendingTex is thread-local to the worker thread, never touched by
video_render, so no texMu is involved there at all. Only the three sites
that touch the SHARED `s->tex[]`/`gEngine->tex[]` needed this fix.)

Verified: builds and links clean, `ctest` 3/3 (76 checks) -- no test
exercises OBS's actual threading model, so this can't be unit-tested; it's a
static lock-ordering argument, not something observed failing in this
session's own testing (the "not responding" reports predate this fix in the
conversation but weren't reproduced/confirmed against a debugger). Installed
while OBS was NOT running. **Not yet verified live** -- next session should
watch for whether the "not responding" reports stop recurring; if they
continue, this wasn't the (only) cause and needs further investigation
(e.g. attaching a debugger during a stall to get real thread backtraces,
rather than continuing to infer from the OBS log alone -- the log won't show
lock contention).

## CONFIRMED (2026-07-03): Advanced preset's blur density is a real, non-trivial GPU cost

After the blend/draw latency split above shipped, the status log showed a
~44s window (04:39:58-04:40:42 in that session) where `blend_latency` sat at
a clean, stable ~8.3-11.2ms instead of the usual ~2.2ms baseline, with
`draw_latency` correspondingly dropping (~6-8ms vs. the usual ~14.5ms) --
the two stayed self-balancing within roughly the same total budget the whole
time, and `blend`/`drawn` fps never dropped from 60.0. Hypothesized this was
the user testing the Advanced (optical-flow) preset with blur density turned
up -- `resample_blur.comp` does `n * blurDensity` texture taps per pixel
instead of `blend.comp`'s single accumulate pass, so a real, substantial,
STABLE (not noisy) cost increase is exactly what you'd expect from that
shader specifically, as opposed to e.g. GPU contention (which would look
noisy/inconsistent, not a clean plateau). **User confirmed**: "subsampling
really take a lot of blend time." Good news buried in this: even at that
elevated cost, the pipeline absorbed it with zero fps drop and no stall --
the fixed-budget design (tick-gated dispatch, self-balancing blend/draw
split) has headroom for Advanced's extra cost, at least at whatever density
was in use in that test. Documented the measured cost in README's preset
section so it's not a surprise. No code change from this entry -- it's a
confirmation of already-shipped instrumentation, not a new fix. If Advanced
usage grows, worth eventually logging the actual density value alongside a
future stall so cost-vs-density isn't just inferred from timing correlation.

## CHANGED (2026-07-03): split the single latency metric into blend/draw segments

After the tick-gating fix (below) made `drawn` track `blend` cleanly, the
user reviewed the log and pointed out the single `latencyMs` number (2-19ms,
often near-zero) didn't make sense against "one frame of osu! capture should
be ~16.6ms" -- correctly identifying that the metric wasn't measuring what
it looked like it measured. Root cause: it computed "newest captured frame's
own timestamp -> blend retiring", which conflates two different things --
the (fine, expected) PHASE offset between osu!'s and OBS's two independently-
paced ~60Hz clocks (near-zero when producer fps is high and a fresh frame is
always seconds^-1 away, up to a full ~16.6ms and essentially RANDOM when
producer fps is low/unfocused, since there's no relationship between when a
frame arrives and when OBS's tick fires) with actual pipeline processing
cost. That's why it read low-but-jittery instead of a stable number.

User's mental model for what to measure instead (their words, lightly
edited): `osu! present -> [shutter window, ~16.6ms by design, not measured]
-> [blend cost: should be small and CONSISTENT] -> [draw latency: retire to
actually shown by OBS] -> overall should land ~2 frames behind osu!'s last
present`. Implemented as two new engine-level metrics, replacing the old
single `latencyMs`:

- **`blend_latency`**: dispatch -> `pollBlendDone()` retiring. Pure GPU/CPU
  processing cost of one blend. Local-only computation in `workerMain()`
  (`inFlightDispatchTimeNs`, a plain local var -- both ends touched by the
  same thread, no atomics needed for the computation itself, only the
  published result `s->blendLatencyMs` is atomic since the status log reads
  it). Should be small and stable; a rising or jittery number here is the
  first place to look for a genuine GPU-contention-with-the-game problem.
- **`draw_latency`**: retire -> `gmixVideoRender()` actually observing the
  new `frontIdx` (same "did it change since last draw" gate as `drawnRate`).
  Cross-thread (`workerMain` writes `s->frontReadyTimeNs` at retirement,
  `gmixVideoRender` -- OBS's render thread -- reads it), hence atomic.
  Expected to land somewhere in ~0-16.6ms and average around half an OBS
  frame interval, since a blend can retire at any point relative to OBS's
  next real render call -- that's bounded by OBS's own cadence, not
  something gmix's pipeline controls, and isn't a bug by itself.

Status log format changed from `latency=X.Xms` to `blend_latency=X.Xms
draw_latency=X.Xms`.

Verified: builds and links clean, `ctest` 3/3 (76 checks). Installed while
OBS was RUNNING (user actively testing) -- safe on Linux, but needs an OBS
restart to take effect; flagged. **Not yet verified live** -- next test
should confirm `blend_latency` is small (low single-digit ms) and STABLE
(not swinging with producer fps the way the old metric did), and
`draw_latency` roughly matches the "~0 to one OBS frame interval" prediction.

## FIXED (2026-07-03): dispatch schedule drifted below OBS's real target fps

The new per-stage status log (see its own entry below) made this diagnosable
from actual numbers instead of by feel. User reported pacing "solid ~95% of
the time" but occasionally visibly stalled; the OBS log showed video settings
at a clean **`fps: 60/1`**, but the `gmix: status:` line's `blend`/`drawn`
fields sat at **~55.5-58.8fps for the entire session** -- not an occasional
dip, a CONSTANT few-percent shortfall below the real target.

**Root cause:** the dispatch throttle in `workerMain()` set
`lastDispatchTime = nowTick` (the actual, jittery wake time) after every
dispatch, then gated the next one on `nowTick - lastDispatchTime >=
minDispatchInterval`. Any small per-cycle overhead -- the worker thread's
wake latency off `tickCv` (a condition variable, not a real-time-scheduled
wake), the GPU submit cost, `queue.snapshot()`'s mutex+copy -- pushes that
reference point a little later every single cycle, and NOTHING pulls it back
toward the true grid. The drift compounds indefinitely: this is why the
achieved rate was persistently a few percent under target rather than merely
occasionally dropping a frame.

**Fix:** advance `lastDispatchTime` by exactly one `minDispatchInterval`
(the nominal step) instead of snapping it to `nowTick`, so the schedule stays
pinned to the original grid regardless of individual cycles' jitter -- this
self-corrects: if one cycle runs late, the very next check already sees more
than one interval elapsed and fires immediately, rather than losing that time
forever. A catch-up cap (`if (nowTick - lastDispatchTime > 2 *
minDispatchInterval) lastDispatchTime = nowTick - minDispatchInterval;`)
resets the schedule after a REAL stall (fell more than 2 intervals behind) so
a genuine multi-frame hang doesn't turn into a burst of rapid-fire catch-up
dispatches once it recovers.

**Result confirmed live** (user restarted OBS and retested): `blend` now
sits at ~59.9-60.7fps for the whole session (was 55.5-58.8), matching the
OBS-configured `60/1` almost exactly, and `latency` dropped to a tight
2.3-4.4ms (was up to 19ms during idle periods). The fix worked as intended.

## FIXED (2026-07-03 follow-up): fixed-grid dispatch reintroduced the two-clock beat one layer up

The fix above self-corrected `blend`'s average rate, but the SAME status log
that confirmed it also revealed a new, more visible problem: `drawn` now
trails `blend` noticeably during high-producer-fps (gameplay) periods --
e.g. `blend=60.0fps drawn=49.6fps` -- while matching closely during low-fps/
idle periods (`blend=60.0fps drawn=60.0fps`). This gap existed before too,
just masked: with `blend` itself running under target, `drawn`'s own
shortfall was indistinguishable from `blend`'s.

**Root cause:** the fix above pinned the dispatch schedule to a
`lastDispatchTime += minDispatchInterval` grid anchored on
`std::chrono::steady_clock` -- a perfectly steady, INDEPENDENT clock. OBS's
own real per-frame cadence has natural jitter even at a clean configured
60/1 (scheduler noise, other sources, compositor). Two nominally-equal-rate
but independently-paced clocks beat against each other -- exactly the
judder mechanism already fixed once before (`gmixVideoTick`-driven wakeup
replacing a free-running sleep timer, see the "smoothness/stability pass"
entry below) -- just reintroduced one layer up, at the dispatch-throttle
level this time. Specifically: gmix's steady grid would occasionally race
slightly ahead of OBS's real callback timing (most likely whenever there's
always fresh capture data ready to blend, i.e. high producer fps, since
that's when the throttle is the active constraint rather than data
availability), completing a SECOND blend before OBS ever called
`video_render` to observe the first one -- that first blend's output was
computed but never actually shown.

**Fix:** stopped gating dispatch on elapsed wall-clock time entirely.
`workerMain()` now gates on `s->tickSeq` (already tracked, bumped by
`gmixVideoTick()` on every real OBS render frame) -- a dispatch is allowed
only once `tickSeq` has changed since the last dispatch
(`lastDispatchedTickSeq`, a local replacing `lastDispatchTime`/
`minDispatchInterval` entirely). This ties dispatch 1:1 to OBS's ACTUAL
callback cadence, whatever its real jitter, instead of an assumed nominal
duration -- there is no second clock left to beat against. The shutter
window WIDTH is unaffected (still `s->obsFrameSec.load() * 1e9`, a stable
target duration, not a pacing throttle).

Verified: builds and links clean (no unused-variable warnings from removing
`minDispatchInterval`/`lastDispatchTime`), `ctest` 3/3 (76 checks). Installed
while OBS was NOT running, so it'll apply on next launch without a restart
needed mid-session. **Not yet verified live** -- next test should show
`drawn` tracking `blend` much more closely during high-producer-fps/gameplay
periods specifically (that's where the old gap was concentrated), not just
during low-fps/idle periods where it already matched.

## ADDED (2026-07-03): per-stage FPS + latency status logging

User reported pacing is now solid "like 95% of the time" but still feels
occasionally stalled, and asked to re-check the producer/consumer clock --
led to the `obs_get_video_info()` fix above. To make the REMAINING 5% (or any
future regression) actually diagnosable from the OBS log instead of by feel,
added a periodic (`kStatusLogInterval` = 2s) status line, logged once per
connection from `workerMain()` (not per-source -- rates are engine-level,
shared across however many scenes the source is in):

    gmix: status: producer=938.2fps consumer=912.4fps blend=59.8fps drawn=59.1fps latency=14.2ms

- **producer** -- `RateTracker` ticked in `receiverThreadFn()` on every frame
  the capture layer actually sent over the socket, BEFORE the backlog-drop
  check. The game/capture layer's real export rate.
- **consumer** -- ticked only for frames that survive the drop and get
  pushed into the ring. What's actually available to the blend; a big gap
  vs. producer is expected/healthy (that's the backlog-drop working, see
  `FrameReceiver::hasPendingFrame()`), not a problem by itself.
- **blend** -- ticked in `workerMain()` each time `pollBlendDone()` retires a
  blend. The achieved output-UPDATE rate -- this is the one to watch for the
  "stall" symptom: should track `obsFrameSec`'s rate closely; a sustained gap
  means blends are taking longer than one shutter interval (GPU contention
  with the game, likely) and OBS is re-showing a stale front.
- **drawn** -- ticked in `gmixVideoRender()` only when the drawn `frontIdx`
  actually CHANGED since the last call (via `lastDrawnFrontIdx.exchange()`),
  not on every video_render call (which would just echo OBS's own configured
  fps and tell you nothing). Should track `blend` closely; a gap between
  `blend` and `drawn` would point at something on OBS's own render/present
  side rather than gmix's pipeline.
- **latency** -- ms from the NEWEST source frame used in a blend (its
  capture `timestampNs`) to that blend retiring. Both ends are
  `std::chrono::steady_clock` (`CLOCK_MONOTONIC`), which -- unlike e.g.
  `CLOCK_PROCESS_CPUTIME_ID` -- is consistent across processes on Linux, so
  this is a valid measurement despite producer (capture layer, injected in
  the game) and consumer (the OBS plugin) being different processes.

New `RateTracker` class (`gmix_source.cpp`, anonymous namespace) is a small
EMA-smoothed (α=0.1, matching the project's established EMA pattern)
interval tracker, mutex-guarded rather than lock-free -- deliberately, since
`tick()` is called from three different threads (receiver thread, worker
thread, and OBS's render thread via `gmixVideoRender()`) and this is a
low-frequency diagnostic path where correctness matters more than shaving a
lock.

Verified: builds and links clean, `ctest` 3/3 (76 checks) still pass -- no
test exercises the new logging (nothing to assert against without a live
producer). Plugin installed; OBS was NOT running at install time so it
applied cleanly. **Not yet verified live** -- next session/user test should
confirm the status line actually appears every ~2s in the OBS log during
capture, with plausible numbers (producer in the hundreds/fps, blend/drawn
close to OBS's configured fps, latency in the ~10-20ms range per the
project's historical numbers).

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

**2026-07-03, presets confirmed live + Advanced marked BETA:** user tested
all 5 presets against real gameplay after the fixes above and confirmed the
plain-blend ones behave as designed -- Heavy gives a visible trailing ghost
(expected), Cinematic reads as a sharper/softer-falloff blur than Flat, Flat
is the plain/normal blur. Per user direction, **Advanced is BETA and NOT the
default day-to-day choice** -- Flat/Linear/Cinematic/Heavy are the
well-tested path going forward. This is a documentation/status change only
(README + this file), not a code gate -- Advanced is still fully selectable
in the OBS dropdown, just labeled BETA so a user picking it knows what
they're opting into. If asked to actually restrict/hide it later (e.g. behind
a real feature flag), that would be a `gmix_source.cpp` properties-UI change,
not done yet.

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
