# gmix-obs-plugin

osu! realtime frameblending / motion blur for streamers, delivered as a
**native OBS Studio plugin**. GMix pulls every frame osu! renders (often
1000+ fps) straight off the GPU via a Vulkan capture layer and blends the
frames inside each 1/60 s into one output frame — real camera-shutter motion
blur from real frame data — then hands the result to OBS **zero-copy** (GPU
dma-buf export/import, no CPU readback, no virtual camera, no kernel module).

status: working end-to-end (zero-copy render verified live against osu!lazer
on RADV/Polaris10). Frame pacing is smooth; on CPU-bound systems, running
gmix's blend thread alongside osu! costs some of osu!'s own framerate
headroom (see System Requirements below) -- not yet optimized further.

## What GMix is (and isn't)

GMix is a **visual/streaming effect tool**, not a gameplay tool. It only
ever touches frames *after* osu! has already rendered them — it has no
concept of scores, timing windows, judgements, or input.

**GMix CAN:**
1. Capture every frame osu! renders, including well above your monitor's
   refresh rate (e.g. 1000fps uncapped in menus), by hooking the Vulkan
   present call — the same class of hook OBS's own game-capture, RenderDoc,
   MangoHud, or a Steam overlay uses.
2. Blend those frames into a smooth, camera-shutter-style motion blur for
   your stream/recording — a real average of real rendered frames, not a
   synthetic/AI effect.
3. Hand that blended video straight to OBS as a live source, zero-copy
   (no virtual camera device, no extra encode/decode round-trip).
4. Work with osu!lazer's Flatpak sandbox on Linux (persistent capture via
   `flatpak override`, no manual re-launch flags needed after setup).
5. Let you pick a blend preset (Flat / Linear / Cinematic / Heavy, plus a
   BETA "Advanced" mode) via OBS's own source Properties dialog, saved with
   your scene collection -- see "Blend presets" below.

**GMix CANNOT:**
1. Read or write osu!'s process memory, beatmap data, replay data, or any
   internal game state — it never touches osu!'s address space at all.
2. Give any gameplay information or advantage — no unhidden objects, no
   timing assistance, no input automation. It has no path back into the
   game; data only flows outward (rendered frame -> gmix -> OBS).
3. Modify judgements, hit windows, scores, or anything the osu! client or
   server computes — those all happen entirely inside osu!'s own process,
   which GMix never touches.
4. Interact with osu!'s online/multiplayer protocol, servers, leaderboards,
   or replay verification in any way — this is a purely local, client-side,
   post-render visual effect for streaming.
5. Function as a general aimbot/wallhack/macro platform — it has no input
   injection, no memory scanning, and no hooks into anything but the Vulkan
   *presentation* call (`vkQueuePresentKHR`), i.e. the copy of the frame
   that's about to be shown on screen anyway.

(Technical note, not a legal claim: because GMix never reads or writes game
memory, it isn't the kind of tool that memory-integrity anti-cheat checks
are built to catch. Whether any specific server considers unusual Vulkan
layers acceptable is a policy question for that server, not something this
README can promise on your behalf.)

## How it fits together

```
osu! (Vulkan) -> GMix capture layer (VkLayer_GMIX) -> IPC -> obs-gmix-source
   plugin (headless Vulkan blend, async compute) -> dma-buf export/import
   -> OBS renders it directly ("GMix Motion Blur" source)
```

There is no standalone "gmix window" or virtual camera anymore -- GMix runs
*inside* OBS. The `gmix` CLI binary that remains is just a small setup/debug
utility (installs the capture layer, lists GPUs, a debug IPC client) — it
has no capture loop or output sink of its own.

## System requirements

**Linux (tested baseline — confirmed usable, CPU-bound):**
- CPU: Intel Core i5-3470 (4 cores @ 3.20 GHz) or better. This is the
  measured bottleneck: gmix's blend thread competes with the game for CPU
  time, and on this exact CPU osu!'s own framerate drops from ~1000fps
  (idle) / ~600fps (typical) to ~700fps (idle) / ~500fps (typical) while
  gmix is active. A faster/more-core CPU should recover most of that.
- RAM: 8 GB
- GPU: AMD/RADV or similar with `VK_EXT_external_memory_dma_buf` support,
  2016-era or newer (tested on an AMD Radeon RX 480 / Polaris10). Needs
  Vulkan 1.2, a distinct async-compute queue family, and (for the zero-copy
  path) `VK_IMAGE_TILING_LINEAR` support for `VK_FORMAT_R8G8B8A8_UNORM`
  storage images if the device lacks `VK_EXT_image_drm_format_modifier`.
- OBS Studio (Flatpak `com.obsproject.Studio` tested; any build whose
  bundled `libobs` exposes `gs_texture_create_from_dmabuf` — OBS 27+ —
  should work).

**Windows:** not yet ported — spec will be listed in `../WIN32/README.md`
once that happens.

## Dependencies

Runtime (always needed, whether you build from source or use a release):
- A Vulkan driver with `VK_EXT_external_memory_dma_buf` (Mesa RADV on
  Linux covers this for AMD GPUs).
- OBS Studio.
- osu! (osu!lazer, native or Flatpak; the AppImage-based osu!stable path
  also works if you point `config/gmix_config.ini` at it).

Build-time only (not needed if you're installing from a release archive):
- CMake >= 3.18 and Ninja (or another CMake generator).
- A C++20 compiler (tested with GCC 14).
- The Vulkan SDK/loader + headers (`libvulkan-dev` or equivalent).
- `glslangValidator` (compiles the blend compute shader to SPIR-V at build
  time).
- OBS Studio's development headers (`obs.h`, `obs-module.h`, etc.) — for
  the Flatpak OBS these ship inside the Flatpak install itself, see below.

## Option A — Install from a release (no build tools needed)

1. Download the release archive for your platform and extract it somewhere.
2. **`cd` into the extracted folder** — every command below is written
   relative to it (i.e. run them from inside the folder you just extracted,
   not from wherever the archive/zip itself is sitting):

       cd /path/to/extracted/gmix-obsplugin-linux-x86_64   # <- your actual extracted path

3. Install the OBS plugin (Flatpak OBS shown; adjust the path for a native
   OBS install, typically `~/.config/obs-studio/plugins/`):

       mkdir -p ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/{bin/64bit,data/locale}
       cp obs_plugin/bin/64bit/obs-gmix-source.so ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/bin/64bit/
       cp obs_plugin/data/locale/en-US.ini ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/data/locale/

4. Restart OBS, then add a **"GMix Motion Blur"** source to a scene (once —
   it persists in your scene collection).
5. Register the capture layer + enable capture for your game (edit
   `config/gmix_config.ini`'s `[capture]` section first — target process
   name, and either `appimage` or `flatpak_app_id`):

       ./config/gmix_launch.sh

6. Launch the game normally. Gameplay should appear live (motion-blurred)
   in the "GMix Motion Blur" source's OBS preview.

## Option B — Build from source

    git clone https://github.com/MAN67678/gmix-obsplugin
    cd gmix-obsplugin/linux-x86_64
    cmake -S . -B build -G Ninja \
      -DGMIX_BUILD_OBS_PLUGIN=ON \
      -DOBS_INCLUDE_DIR=/path/to/obs/include   # e.g. the Flatpak OBS's .../files/include/obs
    cmake --build build

This produces:
- `build/gmix` — the setup/debug CLI
- `build/layer/VkLayer_GMIX.so` (+ manifest) — the Vulkan capture layer
- `build/obs_plugin/bin/64bit/obs-gmix-source.so` (+ `data/locale/en-US.ini`)
  — the OBS plugin

Then follow steps 3-6 of Option A, but run them from inside `build/`
instead of the release folder (i.e. `cp build/obs_plugin/...` and
`./config/gmix_launch.sh` from the repo's `linux-x86_64/` directory, which
is exactly what the commands above already are relative to).

Finding the Flatpak OBS's `OBS_INCLUDE_DIR`:

    find /var/lib/flatpak/app/com.obsproject.Studio -maxdepth 6 -type d -name obs 2>/dev/null | grep include

## Blend presets

Set from the "GMix Motion Blur" source's Properties dialog in OBS ("Blur
preset"), saved with your scene collection:

- **Flat** (default) — uniform average of the real frames in the shutter
  window. Minimal ghosting, the safest/subtlest look.
- **Linear** — symmetric triangle weighting peaking at the center frame.
- **Cinematic** — symmetric gaussian falloff. Soft, photographic.
- **Heavy** — one-sided exponential decay from the newest frame. A long,
  visible trailing ghost.
- **Advanced** ("optical awareness") — **BETA, not the recommended/default
  choice.** Velocity-aware motion blur: estimates a per-pixel motion
  direction each frame and smears real captured pixels along it, oversampling
  along that direction (dense overlapping taps) and averaging them so the
  taps' footprints tile into one continuous, energy-conserving streak
  (closer to a real camera-shutter motion blur) instead of a row of separate
  ghost copies or an artificially brightened one. Reveals two sliders:
  **Blur density** (4-32) — taps per real frame along the motion
  direction; higher packs the streak denser (and smoother — this is the
  oversampling rate, and also what keeps small/fast content like the cursor
  from visibly fading into the average). Earlier live measurements (before
  the plain-average/motion-gated-brightness rework above) saw blend time go
  from a ~2.2ms baseline to ~8-11ms with density turned up; re-measured live
  after that rework at density=32: no frame drops, overall performance
  impact ~3-5% or less — the current shader is meaningfully cheaper than
  the earlier weighted/MAX accumulation schemes it replaced, not just
  visually correct. Still a real, non-zero cost, just a much smaller one
  now; and **Blur brightness** (0.1-10,
  default **1.3**) — exposure boost applied ONLY where real motion was
  detected (gated by the estimated per-pixel motion magnitude), so it
  brightens/dims the trail without touching static, non-moving parts of
  the scene. 1.0 is neutral (pure average, no boost) but tested visibly
  under-bright live; 1.5 tested best for in-game motion but too bright on
  menu screens (mostly static content, but still enough moving UI to
  trigger the boost) — no single value suits both, so 1.3 is the default
  and the slider is there to tune per what you're capturing. The max of 10
  is left in deliberately as a "how absurd can it get" option, not a
  serious setting.
  Flat/Linear/Cinematic/Heavy all use the plain averaging shader and are the
  well-tested, day-to-day path; only Advanced routes to the separate
  optical-flow shader, which has seen far less real-world testing (a
  previous evaluation of this same shader found it "did NOT beat the plain
  shutter blend" — see `etc/DEV_NOTES.md`). Treat it as an experiment to try,
  not a preset to default to. Its motion estimate uses a small windowed
  (Lucas-Kanade) optical flow rather than a single-pixel estimate, which is
  specifically meant to be more stable at low velocities.

## Latency mode

Also in Properties, independent of the blur preset: **Latency mode**
(Fast/Medium/Slow/Very slow) controls how many GPU dst buffers the blend
cycles through before reusing one — more buffers give more tolerance for
the blend's own timing to vary (e.g. GPU contention with the game, or
drift over a long session) without risking a slow blend racing an
in-progress read, at the cost of a bit more VRAM (Fast=2, Medium=3
[default], Slow=4, Very slow=5 — each step is one more capture-resolution
image).

This is a process-wide setting (like GPU index), not a per-source one, so
changing it takes two steps:

1. Open Properties on any "GMix Motion Blur" source (existing or new) and
   pick a Latency mode. This saves your choice for next time, but does
   **not** affect whatever's already running.
2. Remove **every** "GMix Motion Blur" source (from every scene) and add
   one back. The fresh instance picks up the value you just saved in step 1.

Just restarting OBS does *not* apply a change — the same sources reload
with the same settings they already had. If you skip step 1, or step 2
doesn't fully remove every instance, check the OBS log for a `gmix:`
warning explaining what happened.

Latency mode also sets the acceptable end-to-end latency budget the status
log checks against: `(buffer count − 1)` output-frame intervals — Fast=1,
Medium=2, Slow=3, Very slow=4. If `blend_latency + draw_latency` exceeds
that, you'll see a `gmix: preset=... end-to-end (blend+draw) latency
exceeds the budget` warning; the fix is either a lower blur density
(Advanced) or a slower Latency mode for more headroom.

## Known issues

**Properties dialog can show stale values right after adding a source**
(cosmetic only, not a functional bug). If you already have a "GMix Motion
Blur" source running with non-default settings (say, Advanced preset,
Very slow latency) and then add another instance — either a new `+`-added
source or after a remove/re-add — that new instance's Properties dialog
can briefly display the plugin's hard defaults (Flat preset, Medium
latency) instead of what's actually running. The blur output itself is
correct; only the dialog's displayed dropdown values can lag. To be sure
a setting change is both saved and correctly reflected everywhere:

1. Open Properties on any "GMix Motion Blur" source, change the setting,
   and click **OK** — this saves your choice to disk
   (`~/.config/gmix/engine_settings`, `~/.config/gmix/blend_config`) so it
   survives future source/engine recreation.
2. Remove **every** "GMix Motion Blur" source (from every scene) and add
   one back. The fresh instance both applies and correctly displays the
   value you just saved in step 1.

This is the same two-step already required to change **Latency mode** (see
below) — Latency mode never applies without it, since it resizes the GPU
buffer pool; preset/density/brightness actually apply live without this
step, but re-adding is still the reliable way to confirm the dialog itself
shows the right thing.

(Previously: adding "GMix Motion Blur" as a NEW source
per scene only rendered in the first scene, since each plugin instance
raced to bind the same capture socket. Fixed by hoisting the capture/blend
pipeline into one process-wide shared engine that all source instances
attach to — see `etc/DEV_NOTES.md`. You can now use `+` -> "GMix Motion
Blur" in as many scenes as you like, or still reuse one source via
**Add Existing Source**; both work.)

## Notes

- Zero-copy relies on `VK_EXT_external_memory_dma_buf` + `VK_IMAGE_TILING_LINEAR`
  (this GPU has no `VK_EXT_image_drm_format_modifier`) on the producer/layer
  side, and OBS's public `gs_texture_create_from_dmabuf` on the import side.
- A previous version used a v4l2loopback virtual camera as the output sink.
  That path (and its vendored out-of-tree kernel module) has been removed
  entirely — it was implicated in a kernel panic and is superseded by the
  native OBS plugin's zero-copy delivery.
