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

## CHANGED (2026-07-03, fifteenth): Blur brightness default raised again, 1.2 -> 1.3, after testing in-game vs. menu content

Direct follow-up to the fourteenth entry. User restarted OBS, tested 1.2
live, then reported back with more granular findings across content types:
1.5 gave "the best blur while in game," but was "too bright on menu" --
menu screens are mostly static but still have enough moving UI (the boost
is motion-gated, not scene-gated) to visibly overshoot at 1.5. No single
default value is right for both in-game and menu content, so settled on
1.3 as a middle-ground default, explicitly leaving the slider for the user
to adjust per what they're actually capturing at the time (their own
words: "let the user adjust it").

Same two places changed as the fourteenth entry, same reasoning for why
both need to move together: `gmix_source.cpp`'s `gmixGetDefaults()`
(`kSettingBrightness`, what new sources actually use) and
`BlendConfig.hpp`'s `shutterStrength` member initializer (engine fallback
before any persisted `~/.config/gmix/blend_config` exists). Same caveat as
before: doesn't retroactively change a value already saved in a scene
collection or the persisted config file. Rebuilt, tests pass (3/3),
installed (md5 verified, `.so` changed since this is C++ not shader).

## CHANGED (2026-07-03, fourteenth): Blur brightness default raised 1.0 -> 1.2

Direct follow-up to the thirteenth entry's live confirmation ("1.20 look
okay, 1.30 would give more dense result... 2.0 brightness was way too
bright... keep it max at 10... that would be funny"): 1.0 (pure
energy-conserving average, no boost at all) tested visibly under-bright for
the Advanced preset now that the boost is motion-gated (twelfth entry) --
it only affects the trail, not the whole image, so getting a visible trail
needs a bit more than "neutral." User confirmed 1.2 as a good baseline and
explicitly wants the 0.1-10 slider range left alone (10 kept in on purpose
as a deliberately-absurd option).

Changed in two places to keep them in sync: `gmix_source.cpp`'s
`gmixGetDefaults()` (`kSettingBrightness` default, what actual new sources
use) and `BlendConfig.hpp`'s `shutterStrength` member initializer (the
engine's fallback before any `~/.config/gmix/blend_config` file exists --
matters on first-ever run only, since `acquireEngine()` reads the persisted
file after that). `ResampleParams::shutterStrength` in `blend_engine.hpp`
intentionally left at 1.0f -- it's a struct default only reached if a
caller doesn't set the field explicitly, which the real pipeline
(`gmix_source.cpp`'s `workerMain()`) always does.

Only existing sources with a source-level `obs_data` that never explicitly
set brightness (i.e. new sources going forward, or ones reset to defaults)
pick up 1.2 -- doesn't retroactively change a value already saved to a
scene collection or `~/.config/gmix/blend_config`. Rebuilt, tests pass
(3/3), installed (md5 verified; this one DID change the .so, unlike the
last several shader-only entries, since gmixGetDefaults() is C++ not
GLSL).

## CONFIRMED (2026-07-03, thirteenth): Advanced preset's accumulation/exposure rework (ninth-twelfth entries) tested good, and is cheaper than the old scheme

User tested the twelfth entry's motion-gated brightness fix live: "looks
better now. 1.20 look okay, 1.30 would give more dense result" (density
here meaning visual trail density, from Blur brightness, not the Blur
density slider), confirmed a Blur brightness of 2.0 also looked good via
screenshot, and confirmed the slider's existing 0.1-10 range should stay as
is (asked to keep the max at 10 "that would be funny" -- i.e. an
intentionally-extreme option is fine to leave in, not a request to raise
or lower it).

Also reported a live PERFORMANCE re-measurement at density=32 (the
slider's max): "no frame drop, impact less than 3% to 5% or even less."
This matters because the README's only prior density-cost numbers (a
~2.2ms -> ~8-11ms blend-time jump) were measured against an EARLIER
accumulation scheme (the original exponential-dominance weighted average,
before the ninth/tenth/eleventh/twelfth entries reworked it to a plain
average + motion-gated boost) -- i.e. those old numbers were never
re-validated against the shader as it exists now, and per this report the
current version is meaningfully cheaper, not just visually corrected.
Updated README's Blur density bullet to note both the old (superseded)
measurement and the new live-tested one, so a future reader doesn't treat
the stale 8-11ms figure as still describing the current shader.

No code change this entry -- documentation only (README + this entry),
closing out the ninth-through-thirteenth chain of Advanced-preset
accumulation fixes for now. The chain in order: weighted-average-with-
falloff (buggy) -> weighted-average-flat (still buggy, same dilution
symptom) -> MAX/lighten (fixed dilution, broke energy conservation) ->
plain average (fixed both) -> motion-gated brightness (fixed the
brightness slider blowing out static content) -> user-confirmed good.

## FIXED (2026-07-03, twelfth): "Blur brightness" was a whole-image exposure control in disguise, not a trail-only one

User called the eleventh entry's fix "a bit too subtle" and, on being told
to raise Blur brightness, sent a screenshot: at 10.0 the ENTIRE Properties
preview was blown out white, not just the moving cursor's trail. "brightness
just change overall brightness now. if i slide it down ir dark if i slide
it up [blown out]."

Root cause: `outc.rgb *= shutterStrength` applied unconditionally to EVERY
output pixel, moving or not. For a genuinely static pixel, `acc/count`
(the plain average from the eleventh entry) equals that pixel's raw value
regardless of how many identical taps went into it -- so a static
background pixel got multiplied by shutterStrength exactly the same as a
real motion-blurred one. The slider was never actually a "trail" control;
it was a global exposure control that happened to also touch the trail.

**Fix:** gate the boost by `vlen` (the per-pixel motion magnitude already
computed for the resampling/oversampling above it). `motionAmt =
clamp(vlen / 1.0, 0, 1)`; `boost = mix(1.0, shutterStrength, motionAmt)`.
A motionless pixel gets boost=1.0 always (shutterStrength has literally zero
effect on it, at any slider value), a pixel with >=~1px of estimated
inter-frame motion gets the full slider effect, and it ramps smoothly
between the two rather than a hard on/off cutoff (avoids a visible edge
where the trail "turns on"). Now raising Blur brightness should visibly
brighten only the moving trail, leaving the rest of the scene's exposure
alone.

Same file/mechanism as the tenth/eleventh entries (`resample_blur.comp`
only, no C++ changes, `.so` unaffected -- confirmed same md5 before/after,
only `.spv` rebuilt). No new automated test (GPU shader math). Fourth
consecutive fix to this shader's accumulation/exposure logic in one
session -- see the "failure mode" note at the end of the eleventh entry,
which still applies.

## FIXED (2026-07-03, eleventh): Advanced preset's MAX/"lighten" blend brightened ALL moving content instead of blurring it -- switched to a plain energy-conserving average

User tested the tenth entry's MAX/lighten fix live and sent screenshots:
approach circles and slider bodies with velocity visibly gained a bright
white halo/outline glow that the same content didn't have on the left
(reference) side. Diagnosis, direct from the user: "everything with
velocity not bright up instead of just blur, that's why i set default to
1.0, didn't know it was a bug" -- they'd left Blur brightness at the
documented-neutral 1.0 in good faith; the actual bug was that "neutral" was
never really neutral under MAX.

Root cause: MAX/"lighten" is not energy-conserving. A real motion blur
(camera shutter integrating light over an exposure) can only spread
brightness out and DIM it per pixel -- it can never exceed the brightest
instant it's blending. MAX has the opposite property: once ANY tap along a
path lights a pixel, that pixel stays at that brightness NO MATTER how many
darker taps also land there -- there's no dimming mechanism at all. For a
sparse single-pixel-ish cursor this reads as "preserved brightness"
(desirable), but for anything wider (an approach circle's ring, a slider
body's outline) it reads as unwanted brightening/glow, because MAX is
selecting the brightest EDGE PIXEL across many overlapping shifted copies
of a shape that's already bright at its edges by design -- exactly what the
screenshots showed.

**Fix:** replaced MAX with a plain, unweighted average (`acc/count`) --
truly energy-conserving, matching how a real shutter integrates light: the
result can never exceed the brightest single tap, only spread and dim it.
This works for the cursor too now (not just wider content) because of the
oversampling added earlier (dense sub-pixel taps along the motion vector)
-- enough taps land on/near a moving object's own footprint that it isn't
diluted into invisibility by a plain average the way a single tap among
many mostly-background samples would have been in the pre-oversampling
version of this shader. "Blur brightness" remains a direct post-multiply on
the final averaged color (1.0 truly neutral now, matching what the user
already expected it to mean).

Same file/mechanism as the tenth entry (`resample_blur.comp` only, no C++
changes, only the `.spv` needs a rebuild -- already-installed `.so` is
unaffected). No new automated test (GPU shader math). This is the third
consecutive fix to Advanced's accumulation scheme in one session (ninth:
weighted-average-with-flat-weights still diluted; tenth: MAX fixed dilution
but broke energy conservation; eleventh: plain average gets both right) --
worth remembering if Advanced still doesn't look right next time: the
FAILURE MODE to check for is either "discrete separated blobs instead of a
continuous streak" (a dilution/weighted-average symptom) or "moving content
brighter than it should be" (a non-energy-conserving/MAX-like symptom) --
these two properties trade off against each other and a fix for one can
reintroduce the other if not careful.

## FIXED (2026-07-03, tenth): Advanced preset's WEIGHTED-AVERAGE accumulation was the real ghosting cause -- switched to MAX/"lighten" blend

User tested the ninth entry's fix live and sent screenshots: the cursor
trail was a row of distinct, evenly-spaced small circles, not a continuous
streak -- asked directly "did you smooth it to motion blur with
oversampling yet?"

Root cause, finally isolated correctly: it was never the WEIGHT SHAPE
(recency falloff in the eighth entry, still-flat-but-still-averaged in the
ninth) -- it was weighted AVERAGING itself (`acc / wsum`) as the
accumulation method. Any average dilutes a single bright tap's contribution
by how many other (mostly dark-background) taps got summed alongside it.
Between two real oversample tap positions, the divided result necessarily
dips back toward background before rising again at the next tap -- a series
of separate bumps along the path, exactly the "row of dots" in the
screenshots, regardless of what per-tap weight scheme feeds the average.
This is why both the eighth and ninth fixes "worked" in the sense of being
correctly reasoned individually but didn't fix the actual symptom: neither
touched the fact that it was still an average.

**Fix:** replaced the weighted-average accumulation in
`resample_blur.comp` with a MAX ("lighten"/screen blend): track the
brightest (by luma) tap seen across every real frame x sub-sample position,
output that. No division, no dilution -- once any tap along the motion path
lights a pixel, that pixel stays lit at that brightness no matter how many
darker background taps also land on it. This is what makes dense
oversampling (the "Blur density" slider) actually smooth the streak now
(overlapping tap footprints tile into one continuous strip) instead of just
adding more samples to dilute the average against.

Side effect: "Blur brightness" (`shutterStrength`) no longer has a
tap-selection role to play (MAX doesn't dilute, so there's nothing left for
an exponential dominance term to fix) -- repurposed as a direct post-multiply
brightness scalar on the composited trail (1.0 neutral, higher blows out,
lower dims), same slider/range, new meaning. Updated in README's Blur
presets section. `ResampleParams::falloff`/`BlendConfig::falloff` remain
unused dead fields (see ninth entry).

No C++ changes needed this time -- purely a `resample_blur.comp` change, so
only the shader recompiled (`build/shaders/resample_blur.spv`); the
already-installed `.so` was unaffected (confirmed same md5 before/after)
and picks up the new `.spv` automatically since `GMIX_SHADER_DIR` is an
absolute build-dir path baked into the binary at compile time, not a
separately-installed asset. No new automated test (GPU shader math, no
Vulkan-backed harness for Advanced yet).

## FIXED (2026-07-03, ninth): Advanced preset's per-frame recency falloff was itself the ghosting cause -- removed, now flat weighting like Flat preset

User tested the eighth entry's fix (falloff scaled by n/kRefN) live and reported
it "look pretty ghosting" still, with a direct, correct diagnosis: "advance
should use same weight as flat preset."

The eighth fix addressed the falloff term's N-scaling but left the falloff
mechanism itself in place -- and the mechanism was the actual bug. Each real
frame i in the window was weighted by `pow(1 - i/n, falloff)`, fading older
frames toward zero. Combined with per-frame spatial resampling (frame i's
data read at an offset along the motion vector), this produced several
distinct, independently-fading copies of the moving object rather than one
continuous streak -- textbook ghosting, structurally the same complaint as
the original Cinematic bug ("clearly just ghosting") even after the N-scale
fix, because the falloff *shape itself*, not just its width, was the
problem.

**Fix:** removed the recency falloff entirely from `resample_blur.comp`.
Every real frame in the window is now weighted flat (equal), the same
philosophy as the Flat preset's uniform 1/N average. The exponential
brightness-dominance term (`exp(luma * shutterStrength)`, still driven by
the "Blur brightness" slider) is unchanged and is now the ONLY weighting
left -- it's what turns a bright pixel (e.g. a cursor) at any tap position
into the dominant contributor for that output pixel, producing the
directional glow. `ResampleParams::falloff` / `BlendConfig::falloff` are
left in place (unused by the shader now) rather than doing an ABI-churning
removal for a currently-single-purpose field; `gmix_source.cpp`'s
`workerMain()` no longer sets `resample.falloff` at all (see the comment
there).

No new automated test -- same reasoning as the eighth entry (GPU shader
math, no Vulkan-backed numerical harness for Advanced yet). Rebuilt (spv
regenerated fresh under `build/shaders/`, confirmed via the binary's baked-
in `GMIX_SHADER_DIR` path), tests pass (3/3), installed. Awaiting live
confirmation this actually reads as smoother, not just theoretically
correct.

## FIXED (2026-07-03, eighth): Advanced (optical-flow) preset had the same N-dependent washout bug as the old Cinematic preset

User asked directly whether Advanced could have "the same issue as cinematic
preset before, the ghosting issue" — it did, same root cause class, found by
inspection + derivation (not yet from a live report).

`resample_blur.comp` weights each real frame's contribution by
`recency = 1 - i/n` (i = frames back from newest, n = actual real frame
count in the shutter window) raised to a fixed exponent `falloff` (constant
1.0, not exposed in the UI). Exactly like the old Cinematic gaussian's fixed
`mult`, a fixed exponent applied to a ratio NORMALIZED by n flattens in
ABSOLUTE frame terms as n grows: frame-1-back's weight relative to the
newest goes from 0.875 at n=8 to 0.984 at n=64 with falloff=1.0 held
constant. At high real-capture-rate n, this means many real frames near the
front of the window get near-equal weight instead of the newest one
dominating -- diluting the directional streak into a flatter smear. Same
symptom class as the Cinematic bug ("look less cinematic... clearly just
ghosting"), just not yet reported live for Advanced since it's BETA and
gets far less real-world mileage.

**Fix:** scale `falloff` by `n / kRefN` (kRefN=16, same reference point
used in the Cinematic fix) at the dispatch call site in `gmix_source.cpp`
(`workerMain()`, right where `ResampleParams` is built), instead of passing
the constant straight through. Host-side scalar computation, no shader
change needed. No new automated test added -- this is GPU shader math that
would need a Vulkan-backed numerical test like the Cinematic one had, and
the fix mirrors an already-tested pattern; flagging here for whoever adds
Advanced-specific shader tests later.

## FIXED (2026-07-03, seventh follow-up): remove-and-re-add appeared to "still load default" — actually a stale Properties dialog, engine was correct

User report: "set to whatever it is then remove it then add again and it
shoe defualt again i repeat and it still load defualt again." Looked like
the sixth/fifth-follow-up persistence fixes (`~/.config/gmix/engine_settings`,
`~/.config/gmix/blend_config`) hadn't actually stuck this time.

Checked the log line-by-line across the whole session (two full remove/
re-add cycles, ~4 minutes): `preset=advanced` and 5 dst buffers were in
effect immediately after each re-creation, and both persisted-config files
were confirmed read (`gmix: using persisted engine settings...` / `gmix:
using persisted blend config...`). The actual `GmixEngine` — and therefore
the real blur behavior the user sees in a recording — was correct the whole
time.

The real bug was elsewhere: a freshly `+`-added `GmixSource` always starts
from `gmixGetDefaults()` (Flat/Medium) in its own `obs_data_t`, since OBS
calls `create()` before Properties can be opened/edited. When that new
source's `create()` attaches to an ALREADY-RUNNING engine (i.e.
`acquireEngine()`'s refcount ends up > 1, not a fresh engine), the engine
itself is fine, but the new source's Properties dialog still shows its own
stale default `obs_data` — Flat/Medium — instead of what's actually
running. Indistinguishable from a real functional regression by eye.

**Fix:** in `gmixCreate()`, after `acquireEngine()`, if
`s->engine->refCount.load() > 1` (attached to an existing engine rather than
having just created one), snapshot the engine's real `blendConfig` +
`gpuIndex` + `dstBufferCount` and push them into this source's own
`settings` via `obs_data_set_string/int/double` (preset, blur density,
brightness, GPU index, and Latency mode via a new
`bufferCountToLatencyModeSetting()` reverse mapping of
`latencyModeSettingToBufferCount()`), then re-run `gmixUpdate(s, settings)`
so the rest of `s`'s parsed fields match too. Now the Properties dialog for
a newly attached source reflects the real running state immediately.

No blend-engine logic changed — diagnostics/sync-on-attach only, so no new
test added (matches the pattern of the persistence-only fixes above).

## CHANGED (2026-07-03, sixth follow-up): latency budget now derives from Latency mode, applies to every preset

The `blend_latency`/`draw_latency` budget warning was hardcoded to "4
output frames, Advanced preset only" -- disconnected from the actual
Latency mode setting, and blind to non-Advanced presets even though the
SAME budget concept applies to them too (just less likely to matter, since
their blend cost is far lower). Per user request, formalized the full
pipeline and tied the budget to the mechanism that actually provides the
tolerance:

    [capture, ~16.6ms shutter window, BY DESIGN, not budgeted]
      -> [blend: dispatch->retire, pure GPU/CPU cost]
      -> [draw: retire->OBS shows it, bounded by OBS's cadence]
      -> [obs]

Budget = `(dstBufferCount - 1)` output-frame intervals, checked against
`blend_latency + draw_latency` together (not each alone) -- Fast(2
buffers)=1 frame, Medium(3)=2, Slow(4)=3, Very slow(5)=4, exactly the
numbers the user specified. This is the SAME number `dstBufferCount`
already encodes as "extra dispatch generations of grace before a buffer
gets reused" (see `BlendEngine::dstBufferCount()`'s comment) -- Latency
mode IS fundamentally "how much blend/draw timing variance are you willing
to tolerate," so the diagnostic budget and the actual buffering mechanism
now use the same number instead of two unrelated ones. Applies to every
preset now, not gated to `mode == Advanced`; the warning message also
suggests a slower Latency mode as a remedy alongside lowering blur density,
since raising `dstBufferCount` is now the direct, correct lever for a
bigger budget.

Verified: builds and links clean, `ctest` 3/3 (18 checks, unaffected --
diagnostics-only, no blend-engine behavior changed). Installed while OBS
was NOT running. **Not yet verified live** -- next session should confirm
the warning message reports the right budget number for whatever Latency
mode is active (e.g. Fast should warn much more readily than Very slow at
the same blend cost).

## FIXED (2026-07-03, fifth follow-up): preset was STILL lost across a Latency-mode-forced engine rebuild

The 4th follow-up's fix (below) genuinely worked for its own test: preset
now applies live and survives a new source being ADDED alongside existing
ones. But the user's actual workflow combines both features -- changing
Latency mode ALSO requires removing every source (that part is inherent,
unchanged) -- and confirmed live: after that remove-all/re-add cycle
(needed to pick up the new Latency mode), preset came back as `flat`
again, even though it had just been live-set to `advanced` moments before.

**Root cause, genuinely different from the 4th follow-up's:** removing
every source doesn't just risk a NEW source's defaults overwriting a
shared value (already fixed) -- it destroys the `GmixEngine` object
ENTIRELY. `blendConfig` only ever lived in that object's memory, with no
persisted backing store, unlike `gpuIndex`/`dstBufferCount` (which read
from `~/.config/gmix/engine_settings`). So the FRESH engine built on
re-add necessarily starts from `blendConfig`'s hard default (Flat) --
there was nowhere for the just-set Advanced to have been read back FROM.

**Fix:** same persisted-file pattern as `engine_settings`, applied to
blend config -- a new `~/.config/gmix/blend_config` file (`preset
density brightness`, written by `writeBlendConfigFile()`/read by
`readBlendConfigFile()`). `applyBlendConfigFromSettings()` now writes it
right after every live `blendConfig` update; `acquireEngine()` reads it
when constructing a FRESH engine (mirroring exactly how it already reads
`engine_settings` for gpuIndex/dstBufferCount), seeding `e->blendConfig`
before the default-Flat value would otherwise stick. Separate file from
`engine_settings` rather than merging them -- blend config changes far
more often (every slider tweak) than gpuIndex/Latency mode, and "applies
live" vs. "needs a full engine rebuild" are different enough concerns to
keep visibly separate on disk.

Verified: builds and links clean, `ctest` 3/3 (18 checks, unaffected).
Installed while OBS was RUNNING -- needs a restart to apply. **Not yet
verified live** -- next session should confirm: set Advanced, change
Latency mode (forcing a remove/re-add), and see BOTH survive the rebuild
together -- `preset=advanced` in the very first status line after
reconnecting, not `preset=flat` recovering only after a manual reselect.

## FIXED (2026-07-03, fourth follow-up): adding a source silently reset the shared blur preset too

Same bug CLASS as the Latency mode saga above, confirmed live one more
time: user set Advanced, then added a (re-added) source, and it came back
showing `preset=flat` in the status log for ~10 seconds until manually
reselecting Advanced. Root cause: `gmixUpdate()`'s routine settings-
application path used to unconditionally write `s->engine->blendConfig`
EVERY time it ran -- including the automatic call OBS makes when a source
is created (using THAT source's own default/saved settings, "Flat" for a
fresh one) -- so simply adding a second source reset the shared preset for
every scene, even with another already-existing source deliberately set to
Advanced. Same shape as the Latency mode bug: N per-source copies of what's
actually ONE shared value, and whichever source was most recently
touched -- even just by being created -- silently wins.

Asked the user how they wanted this handled (same auto-fire-suppression
fix as Latency mode, accepting the same tradeoff of losing auto-restore-on-
scene-reload; vs. leaving it and just documenting the gotcha). Chose the
former, for consistency with Latency mode.

**Fix:** removed the `s->engine->blendConfig = cfg;` write from
`gmixUpdate()`'s routine path ENTIRELY. It now happens ONLY from genuine
Properties-dialog interaction, via THREE new modified-callbacks --
`gmixPresetModified()` (extended, previously only toggled slider
visibility), `gmixDensityModified()`, `gmixBrightnessModified()` (both
new, registered on the density/brightness sliders which had no modified-
callback before) -- each gated by its OWN auto-fire-suppression flag
(`gGmixNextPresetModifiedIsAutoFire` etc., same pattern as
`gGmixNextLatencyModifiedIsAutoFire`, all armed together in
`gmixGetProperties()`). The actual write is a new shared helper,
`applyBlendConfigFromSettings()`, which reads straight from the global
`gEngine` (guarded by `gEngineMu`) rather than needing a `GmixSource*`,
since blendConfig is engine-wide regardless of which source's dialog
triggered the change.

Note the preset-visibility toggle (density/brightness sliders shown only
for Advanced) stays UNCONDITIONAL in `gmixPresetModified()` -- only the
blendConfig WRITE is gated by the auto-fire flag, since the visibility
logic needs to run on the auto-fire too (that's what makes the dialog look
right the instant it's opened).

**Accepted tradeoff (explicitly chosen by the user):** a saved scene
collection with a source set to Advanced no longer auto-restores that
preset on OBS restart -- `blendConfig` now starts at its
default-constructed value (Flat) until a genuine Properties interaction
sets it, for the SAME reason Latency mode needs a manual re-pick after
restart. Engine-wide settings that are stored redundantly per-source
apparently cannot auto-apply safely with multiple sources in play; this
project is choosing "predictable, requires one manual step" over
"sometimes silently applies the wrong source's saved value."

Note the two settings differ in WHEN they need reapplying, worth being
precise about: Latency mode is baked into the engine at creation time, so
it still needs the "change it, then remove every source and re-add" dance
every time (unchanged by this fix -- that part is inherent, not a bug).
Preset/density/brightness are NOT tied to engine creation at all -- they
apply live via `blendConfigMu` any time `applyBlendConfigFromSettings()`
runs -- so the fix here is a pure improvement: adding a NEW source no
longer needs any compensating action, the shared preset just stays
whatever it already was.

Verified: builds and links clean, `ctest` 3/3 (18 checks, unaffected --
no blend-engine behavior changed, purely an OBS-properties-plumbing fix).
Installed while OBS was RUNNING -- safe on Linux, needs a restart to
apply. **Not yet verified live** -- next session should confirm: after
setting Advanced, adding a NEW "GMix Motion Blur" source no longer resets
the shared preset back to Flat (no remove/re-add needed to check this,
unlike Latency mode).

## FIXED (2026-07-03, third follow-up): the persisted config's WRITE side was firing on more than just user clicks

The persisted-config fix (below) got the READ side right -- the log
confirmed `using persisted engine settings ... dstBufferCount=5` followed
immediately by `engine created with ... dstBufferCount=5`, exactly as
designed. But right after, EVERY TIME a source got re-added, a spurious
`Latency mode changed -- saved ... dstBufferCount=3` line appeared within
~20-50ms, silently clobbering the just-read value back down to that new
source's own stale/default one. The user had to manually re-pick the
dropdown after every single remove/re-add cycle to compensate (visible in
the log as a repeating "removed -> engine created at old value -> user
re-picks -> saved -> removed again..." cycle).

**Root cause:** the design assumed `obs_property_set_modified_callback2`'s
callback fires ONLY on genuine user interaction with the control. Confirmed
wrong: OBS also fires it once automatically as part of building/validating
a freshly-created `obs_properties_t` (e.g. right when a new source's
properties are first initialized) -- independent of whether a human ever
opens the Properties dialog. `gmixPresetModified()` relies on the exact
same mechanism to keep the density/brightness sliders' visibility correct
from the moment the dialog exists, which is presumably WHY OBS fires it
eagerly -- but that same eagerness is exactly wrong for a callback whose
job is "detect a deliberate settings change," not "keep UI state
consistent."

**Fix:** `gGmixNextLatencyModifiedIsAutoFire`, a plain bool (not a mutex-
guarded one -- all OBS properties-dialog callbacks run on the single UI
thread, confirmed safe). `gmixGetProperties()` sets it `true` right before
returning the freshly-built `obs_properties_t` (arming the guard for the
ONE synthetic fire OBS is about to make); `gmixLatencyModified()` checks it
first and, if set, consumes it (resets to `false`) and returns WITHOUT
writing -- so only the SECOND and later calls (genuine clicks within that
same dialog session) actually persist anything.

Verified: builds and links clean, `ctest` 3/3 (18 checks, unaffected).
Installed while OBS was RUNNING (user actively recording) -- safe on
Linux, needs a restart to apply. **Not yet verified live** -- next session
should confirm a SINGLE dropdown pick + remove/re-add now reliably produces
the chosen buffer count on the first try, without the user needing to
re-pick after every cycle. Also worth double-checking `gmixPresetModified()`
doesn't have an analogous problem -- it doesn't WRITE anything (just toggles
slider visibility), so an extra auto-fire there is harmless, which is
presumably why it was never noticed until Latency mode's write-based design
exposed it.

## FIXED (2026-07-03, second follow-up): remove-and-re-add couldn't actually apply a non-default Latency mode either

The diagnostics from the FIRST follow-up (below) worked exactly as
designed and confirmed the diagnosis live -- but then the user followed the
"remove every source and re-add" instructions and Latency mode STILL came
back at Medium(3), with the same "will NOT take effect" warnings when
trying Slow again on the freshly re-added source. That instruction was
incomplete: a brand-new "+"-added source always starts from
`gmixGetDefaults()`, because OBS calls `create()` (and therefore
`acquireEngine()`) before the user has any chance to touch Properties.
Under PURE per-source settings storage, there was no path at all to a
non-default value ever taking effect for a fresh engine -- "remove and
re-add" just recreated the same dead end.

**Fix:** added `~/.config/gmix/engine_settings` (same pattern as the
existing `target_process` config the capture layer reads), a tiny
persisted `gpuIndex dstBufferCount` pair.
- `acquireEngine()` now reads this file FIRST when creating a fresh engine
  (`readEngineSettingsConfig()`), overriding whatever the specific calling
  source's own per-source values were, and falls back to the caller's
  values only if the file doesn't exist yet (first-ever run).
- The file is written ONLY from a genuine Latency-mode-dropdown interaction
  in the Properties dialog -- a new `gmixLatencyModified()` modified-
  callback (`obs_property_set_modified_callback2` on the latency list
  property), mirroring `gmixPresetModified()`'s pattern. Deliberately NOT
  written from `gmixUpdate()`'s routine settings-application path (which
  also runs on ordinary source creation and scene-collection load): if it
  wrote there, a brand-new default-configured second source would silently
  clobber a deliberately-persisted non-default value the moment it's
  created, defeating the whole mechanism. The modified callback is the
  correct signal because OBS only fires it on actual UI interaction, never
  during silent settings application.

With this, the actual working sequence is: open Properties on ANY source
(existing or new), change Latency mode (this alone persists it, silently,
to the config file, with a `gmix: Latency mode changed -- saved...` log
line), then remove every "GMix Motion Blur" source and re-add ONE -- the
fresh engine now reads the persisted file instead of that new source's own
defaults.

Verified: builds and links clean, `ctest` 3/3 (18 checks, unaffected --
diagnostics/persistence-only change, no new blend-engine behavior to
assert). Installed while OBS was NOT running. Confirmed no stale
`~/.config/gmix/engine_settings` existed before this change (checked
`~/.config/gmix/` -- only `target_process` was present), so this session's
tests start from a clean slate for it. **Not yet verified live** -- next
session should actually walk the corrected sequence above and confirm
`dst[0]`-`dst[3]` (4 buffers) get imported after setting Slow + remove/
re-add, not just 3.

## FIXED (2026-07-03 follow-up): Latency mode changes were silently ignored, with zero feedback

User set Latency mode to Slow before recording; the log showed only
`dst[0]`-`dst[2]` imported (3 buffers = Medium, the default) the whole
session. Root cause: exactly the limitation already documented for
`dstBufferCount`/`gpuIndex` ("fixed once the first source is created") --
but with TWO "GMix Motion Blur" sources already existing (from a saved
scene collection), whichever one OBS happens to construct FIRST on load
wins, using THAT source's own saved setting. If the user edited Slow on one
source but the other (still saved at Medium) got constructed first, the
change silently never took effect -- no error, no log line, nothing.
Same silent-no-op also happens just from editing Latency mode on an
ALREADY-EXISTING source's Properties dialog after the engine is already
running (the far more likely actual scenario here, since the user
described "setting it before recording" on an existing source, not
creating a new one).

**Fix (diagnostics, not a behavior change -- the underlying "fixed at first
creation" design is unchanged, this just makes it visible):**
- `acquireEngine()`: logs `gmix: engine created with gpuIndex=%d
  dstBufferCount=%u ...` on first creation (so the ACTUAL value in effect is
  always in the log, not just inferred from counting `dst[N]` lines), and a
  `LOG_WARNING` if a LATER source's requested gpuIndex/dstBufferCount
  differs from what's already locked in.
- `gmixUpdate()`: if `s->engine` already exists and the newly-parsed
  `dstBufferCount` differs from `s->engine->dstBufferCount`, warns
  immediately (this is the path that fires for "edited an existing source's
  Properties dialog", which is what actually happened here) --
  `gmix: Latency mode change to %u buffers will NOT take effect -- the
  shared engine is already running at %u buffers ... Remove every GMix
  Motion Blur source and re-add to apply the new value.`

**Practical fix for the user's actual session:** to make Slow (or any
Latency mode change) take effect, remove EVERY "GMix Motion Blur" source
(both of them, from both scenes) and re-add -- the fresh engine created at
that point reads whichever source's CURRENTLY-saved setting gets
constructed first. Simply restarting OBS does NOT help, since the same two
sources reload from the same saved scene collection with the same
ordering ambiguity.

**Not implemented, worth considering if this keeps biting:** a more robust
design would source gpuIndex/dstBufferCount from ONE global location (e.g.
a small `~/.config/gmix/engine_settings` file, written by `gmixUpdate()`
the same way `writeTargetProcessConfig()` already does for the capture
layer's target process) instead of N per-source `obs_data_t` copies that
can disagree -- would make "last edited wins" deterministic regardless of
OBS's source-construction order, though an already-running engine still
couldn't pick up a value change without a full teardown/recreate (that part
is inherent, not fixable by relocating where the setting lives).

Verified: builds and links clean, `ctest` 3/3 (18 checks in
`test_blend_engine`, unaffected -- this is a diagnostics-only change, no
new test needed since there's no new runtime behavior to assert, just log
output). Installed while OBS was RUNNING -- needs a restart to take effect.
**Not yet verified live** -- next session should confirm the new engine-
created log line appears, and that the warning fires when Latency mode is
changed on an existing source with the engine already running.

## ADDED (2026-07-03, branch `optical-flow-lucas-kanade`): "Latency mode" OBS setting (runtime dst buffer count)

User observed latency drifting non-linearly over a session (up, plateau,
back down -- possibly the game itself, possibly system-level, not fully
root-caused) and asked for one more frame of budget when Advanced is
active, "or make it an option, latency mode fast | medium (default) | slow
| very slow." Implemented as the general option rather than an Advanced-only
special case -- more consistent, and the actual mechanism (buffer count)
isn't specific to Advanced anyway.

**What it actually controls:** `BlendEngine::kDstBuffers` (previously a
compile-time `static constexpr uint32_t = 3`) is now a RUNTIME value,
`dstBufferCount()`, chosen via `init(w, h, dstBufferCount)`. More buffers
= more round-robin slots before a dispatch target gets reused = more
tolerance for the blend's own timing to vary (GPU contention with the game,
thermal/scheduling drift) without a slow blend's write racing a still-in-
progress OBS read of that same buffer -- see the extended comment on
`dstBufferCount()` in `blend_engine.hpp`. Cost: one more capture-resolution
RGBA8 image in VRAM per step, and a larger theoretical worst-case front-
buffer staleness. Mapping: Fast=2 (tightest, the pre-triple-buffer-fix
behavior), Medium=3 (default, unchanged from before), Slow=4, Very slow=5.

**Scope of the refactor:** `BlendEngine`'s five `[kDstBuffers]`-sized fixed
arrays (`dstImage_`, `dstMem_`, `dstView_`, `dmaBufFd_`, `dmaBufStride_`,
`dmaBufOffset_`) became `std::vector`s, resized in `createDstImage()` to
whatever `numDstBuffers_` `init()` was called with (clamped to
`[kMinDstBuffers=2, kMaxDstBuffers=5]`). On the `gmix_source.cpp` side:
`GmixEngine::tex` became a `std::vector<gs_texture_t*>` (sized once in
`acquireEngine()`, before the worker thread starts); every local
`gs_texture_t*[kDstBuffers]` array in `workerMain()`/`releaseEngine()`
(`pendingTex`, `texSnapshot`, `oldTex`) became a `std::vector` sized to
`s->dstBufferCount`; the `nextWriteIdx` round-robin modulo uses
`s->dstBufferCount` instead of the old compile-time constant. Descriptor
set/pool sizing needed NO changes -- the dst image is bound ONE AT A TIME
per dispatch (`dstView_[dstIdx]`), not as a fixed-size array registered
simultaneously like the source images are, so buffer count never touched
GPU descriptor layout.

**New OBS setting:** `kSettingLatencyMode` = `"latency_mode"`, a "Latency
mode" dropdown in Properties (Fast/Medium/Slow/Very slow), NOT preset-
specific -- applies engine-wide like GPU index, fixed once the first "GMix
Motion Blur" source is created (same limitation GPU index already has:
changing it later, or setting it differently on a second source, has no
effect until every source is removed and re-added).

Added `blend_engine_runtime_dst_buffer_count` to `tests/test_blend_engine.cpp`
-- inits with 5 buffers, confirms `dstBufferCount()` reports it, every
`dstImage()`/`dstView()` up to that count is a real handle (not just index 0,
which every other test already covered), `dispatchAsync()` into the LAST
valid slot works, and an out-of-range index is rejected rather than
silently clamped/UB.

Verified: builds and links clean, `ctest` now 4 test binaries' worth (3
existing + 1 new case) all passing -- confirmed live at 5 buffers
(`dst[0]` through `dst[4]` all exported dma-buf fds successfully in the
test run). Installed while OBS was RUNNING -- safe on Linux, needs a
restart to take effect. **Not yet verified live in OBS itself** -- next
session should confirm the "Latency mode" dropdown appears, actually
changes the number of `dst[N]` import lines logged on connect, and that
Slow/Very slow measurably reduces whatever the observed latency-drift
symptom was (the actual root cause of that drift is still not confirmed --
this is a mitigation/tolerance increase, not a diagnosed fix).

## ADDED (2026-07-03, branch `optical-flow-lucas-kanade`): Lucas-Kanade windowed optical flow for Advanced

User asked for deep research (no code) on making low-velocity motion look
smoother than a plain window/game capture. Full writeup wasn't preserved
here verbatim, but the actionable conclusion: GMix is post-render-capture
only (no depth buffer/motion vectors/camera control), which rules out most
modern techniques (TAA-style camera jitter, G-buffer extrapolation). The
best-fit, lowest-risk improvement within that constraint: the Advanced
preset's single-point brightness-constancy motion estimate
(`resample_blur.comp`) is noisiest exactly at low velocity, since the
temporal difference `Iₜ` is tiny there and a one-pixel estimate is
dominated by noise rather than signal (the classic aperture problem).

**Implemented:** replaced the single-point estimate with Lucas-Kanade --
aggregate the brightness-constancy constraint (least-squares normal
equations) over a 3x3 local window instead of one point, solved via
Cramer's rule; falls back to `v=0` (sharp, no trail) when the window is too
flat/uniform to constrain a direction (near-singular 2x2 matrix), same
fallback behavior as before, just reached more reliably. Shader-only
change (`shaders/resample_blur.comp`); no C++/push-constant changes needed,
window radius is a compile-time constant (`R=1`) not exposed as a setting.

**Latency budget, per user direction:** be mindful that this is real extra
GPU cost -- if Advanced's blend_latency exceeds half an output frame
interval, that's fine/expected: Advanced gets a LOOSER end-to-end latency
budget than the default presets on purpose (~3-4 output frames instead of
the tight ~2-frame target established for Flat/Linear/Cinematic/Heavy).
Implemented as: (1) the status log now prefixes with `preset=%s` so an
elevated `blend_latency` is immediately attributable to Advanced instead of
requiring after-the-fact correlation-hunting (which was needed earlier this
session to confirm the Cinematic-preset cost); (2) a rate-limited (log-once-
per-crossing) `LOG_WARNING` fires only if Advanced's `blend_latency +
draw_latency` exceeds 4x the output frame interval -- i.e. even the relaxed
budget, not the tight one. No new gating/throttling code was added:
`BlendEngine`'s single-blend-in-flight + tick-gated dispatch design already
lets a slow blend take as long as it needs (at the cost of `blend`/`drawn`
fps dropping, not a stall or corruption), so the "budget" here is
diagnostic/documentation, not a new enforcement mechanism -- deliberately,
since a hand-rolled latency cap done wrong is exactly what caused the
drawn-trailing-blend judder bug fixed earlier this session.

Verified: shader compiles clean via glslangValidator (grew ~9.2KB->10.9KB,
consistent with the added window loop), full build/link clean, `ctest` 3/3
(80 checks). Installed while OBS was NOT running. **Not yet verified live**
-- next session should confirm: (1) low-velocity motion in Advanced mode
looks visibly smoother/more stable than before (less erratic trail
direction), (2) `blend_latency` for Advanced stays reasonable in practice
(watch the new `preset=` field in the status log), (3) the budget warning
doesn't fire under normal use (if it does routinely, the 3x3 window or
default blur density may need to come down).

Work is on a dedicated branch (`optical-flow-lucas-kanade`, branched off
`master` at the point where multi-scene/smoothness/presets/lock-order/
Cinematic fixes were all tested-but-uncommitted) specifically so this
experimental change can be developed/tested in isolation from that
already-confirmed-working state.

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
