# gmix-obs-plugin

osu! realtime frameblending / motion blur for streamers, delivered as a
**native OBS Studio plugin**. GMix pulls every frame osu! renders (often
1000+ fps) straight off the GPU via a Vulkan capture layer and blends the
frames inside each 1/60 s into one output frame — real camera-shutter motion
blur from real frame data — then hands the result to OBS **zero-copy** (GPU
dma-buf export/import, no CPU readback, no virtual camera, no kernel module).

status: working end-to-end (zero-copy render verified live against
osu!lazer on RADV/Polaris10), and live-tested against a real streaming
session with the motion blur quality confirmed against a reference video.
osu!stable via [osu-winello](https://github.com/NelloKudo/osu-winello)
(Wine + DXVK) is supported too — see "osu!stable via osu-winello" below.
A real cause of in-game stutter was found and fixed 2026-07-05 (see
"System requirements" below and `etc/DEV_NOTES.md`) -- confirmed live
afterward as smooth at ~1000fps with no perceptible drop from gmix's own
work.

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
   `flatpak override`, no manual re-launch flags needed after setup), and
   with osu!stable installed through osu-winello (Wine's pressure-vessel
   container re-exports the layer, so no wrapper command is needed there
   either).
5. Let you tune the motion blur's density and brightness via OBS's own
   source Properties dialog -- see "Blur settings" below.

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
osu! (Vulkan) -> GMix capture layer (VkLayer_GMIX) -> IPC (dma-buf handoff,
   zero-copy) -> obs-gmix-source plugin (headless Vulkan blend, async
   compute, velocity-aware motion blur) -> dma-buf export/import
   -> OBS renders it directly ("GMix Motion Blur" source, paced by OBS's
   own render loop -- gmix has no output clock of its own)
```

"For osu! (Vulkan)" includes osu!stable under Wine: osu-winello runs it on
DXVK, i.e. D3D9 translated to Vulkan, and that present call is exactly what
the same layer hooks — no second capture backend. The layer's target match
checks `/proc/self/comm` and `argv[0]` in addition to `/proc/self/exe`
because under Wine the Unix process is `wine-preloader` and the game's module
name only appears in the former two.

There is no standalone "gmix window" or virtual camera anymore -- GMix runs
*inside* OBS. The `gmix` CLI binary that remains is just a small setup/debug
utility (installs the capture layer, lists GPUs, a debug IPC client) — it
has no capture loop or output sink of its own.

For the full stage-by-stage version of this diagram (with actual filenames
at each step), plus a complete map of every file/socket/directory GMix
touches at runtime and who reads/writes it — the reference to reach for if
a setup is misbehaving and you need to know which piece to look at — see
[`etc/PIPELINE.md`](etc/PIPELINE.md).

## System requirements

**Linux (tested baseline — confirmed usable):**
- CPU: Intel Core i5-3470 (4 cores @ 3.20 GHz) or better. Confirmed live at
  ~1000fps, both idle and during typical gameplay, with no perceptible
  stutter. gmix's blend thread is real, ongoing CPU work alongside the game
  even so; a faster/
  more-core CPU gives more headroom.
- RAM: 8 GB
- GPU: AMD/RADV or similar with `VK_EXT_external_memory_dma_buf` support,
  2016-era or newer (tested on an AMD Radeon RX 480 / Polaris10). Needs
  Vulkan 1.2, a distinct async-compute queue family, and (for the zero-copy
  path) `VK_IMAGE_TILING_LINEAR` support for `VK_FORMAT_R8G8B8A8_UNORM`
  storage images if the device lacks `VK_EXT_image_drm_format_modifier`.
- OBS Studio (Flatpak `com.obsproject.Studio` tested; any build whose
  bundled `libobs` exposes `gs_texture_create_from_dmabuf` — OBS 27+ —
  should work).

**Windows:** not coming soon. A `../WIN32/` folder exists as a placeholder
for a future port, but there's no active work on it and no timeline — the
blocker is the developer's own hardware/driver setup: no Windows machine
with a suitable GPU/driver combination to build and test a capture layer
against any of Windows' relevant graphics APIs (osu! can run on Vulkan,
D3D11, or OpenGL there, so a real port would need all three covered, not
just one), not a design decision against Windows itself. For now, GMix is
Linux-exclusive; that'll be revisited if/when the hardware situation
changes.

## Dependencies

Runtime (always needed, whether you build from source or use a release):
- A Vulkan driver with `VK_EXT_external_memory_dma_buf` (Mesa RADV on
  Linux covers this for AMD GPUs).
- OBS Studio.
- osu! — osu!lazer (native/Flatpak/AppImage), or osu!stable via
  [osu-winello](https://github.com/NelloKudo/osu-winello). osu!stable must
  render through DXVK (`"*d3d9"="native"` in the prefix, which osu-winello
  sets up by default); on Wine's builtin d3d9 it renders with OpenGL and
  there is no Vulkan present to capture.

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
   name, and exactly one of `flatpak_app_id` (osu!lazer Flatpak, e.g.
   `sh.ppy.osu`), `wine_launcher` (osu!stable via osu-winello, e.g.
   `osu-wine`) or `appimage`):

       ./config/gmix_launch.sh

6. Launch the game normally. Gameplay should appear live (motion-blurred)
   in the "GMix Motion Blur" source's OBS preview.

**Checking that the layer actually reached the game:** open the source's
Properties. The **"Apps with the GMix layer loaded"** dropdown lists every
process that has loaded `VkLayer_GMIX`, with its state — `capturing into
OBS`, `target, waiting for OBS to connect`, or `layer loaded, NOT the
capture target (target is '...')` — plus a **Refresh detected apps** button
and a receiver status line. If the game isn't listed at all, the layer is
not enabled in that process (wrong environment/launcher), so there is
nothing for the plugin to receive; if it's listed as not-target, the
`target` fragment doesn't match. Picking an app in the dropdown writes its
name into "Capture target" (takes effect the next time the app starts).

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

## osu!stable via osu-winello (Wine + DXVK)

osu!stable renders through **OpenGL** (mesa `libGL`/`libEGL` under Wine — there
is no D3D9/DXVK swapchain on this setup, and GL never presents through
Vulkan, so there is no `vkQueuePresentKHR`/`vkCreateSwapchainKHR` call for the
layer to hook). The bridge is **Zink**, mesa's GL-on-Vulkan driver:
`00-gmix.cfg` exports `MESA_LOADER_DRIVER_OVERRIDE=zink`, which routes
osu!'s GL through Vulkan — every present then flows through the layer's
hooks like any native Vulkan app (verified end-to-end: OBS logs
`producer connected (1920x1080) -- zero-copy dma-buf receive active`).

Capture costs ~0.35ms of GPU time inside each *captured* frame (measured
from the two per-slot GPU timestamps); frames arrive into a 2000fps cap on
both the producer (export throttle) and receiver (accept cap) — at present
rates below the cap every frame is captured and pays that cost; a live
counter of what the layer did every second is printed as
`GMIX export: ok=N blitAvg=... blitMax=...` to the game's stderr
(`winello.log` under osu-winello).

Two Wine-specific details are handled for you:

- **Enablement.** `osu-wine` sources the `*.cfg` files in
  `~/.local/share/osuconfig/configs` with `set -a` (auto-export) — but bash's
  `source` only sources the *first* file of a glob (the rest become `$1`,
  `$2`, ...), so `gmix_launch.sh` writes **`00-gmix.cfg`**: it sorts first on
  purpose and re-sources the other config files itself so their settings keep
  working. Capture is then on for every normal launch, desktop entry
  included, and survives osu-winello updates (that file is yours, not the
  script's). The script verifies this by running the same `source` command
  `osu-wine` uses and checking that `ENABLE_GMIX` ends up exported.
- **Process matching.** Inside Wine the Unix process is `wine-preloader`, not
  `osu!.exe`; the layer therefore matches the configured target against
  `/proc/self/exe`, `/proc/self/comm` and `argv[0]`.

Setup is the normal one, with `wine_launcher` set:

    [capture]
    target        = osu!
    wine_launcher = osu-wine
    appimage      =
    flatpak_app_id =

    ./config/gmix_launch.sh        # installs the layer + writes 00-gmix.cfg
    osu-wine                       # then launch as usual

Notes and limits:

- A running game only picks this up on its next launch (the layer is loaded
  at `vkCreateInstance`, and pressure-vessel re-exports host layers when its
  container starts).
- If the prefix uses Wine's builtin d3d9 instead of DXVK, osu! renders via
  OpenGL and nothing can be captured; `gmix_launch.sh` warns if it can see
  the prefix's `d3d9.dll` and it doesn't look like DXVK.
- Confirm the layer reached the game with the Properties dialog's "Apps
  with the GMix layer loaded" list (see step 6 above): osu!stable appears as
  `osu!.exe` while running.
- The cursor-position channel is populated from osu!lazer's own SDL3, which
  osu!stable doesn't have, so the "Cursor path" blur mode has no cursor
  samples there (its default width is 0, i.e. off). Optical-flow motion blur,
  the default, is unaffected — osu!stable draws its cursor into the frame
  itself anyway, so it is blurred along with everything else.

## Blur settings

The plugin has exactly one blend mode: velocity-aware ("optical awareness")
motion blur -- estimates a per-pixel motion vector for **each real captured
frame individually** (a windowed Lucas-Kanade optical flow between every
consecutive pair of real frames, not just one global estimate) and smears
that frame's pixels along its own local motion, oversampling along that
direction (dense overlapping taps) and averaging them so the taps' footprints
tile into one continuous, energy-conserving streak (closer to a real
camera-shutter motion blur) instead of a row of separate ghost copies or an
artificially brightened one. Per-frame estimates are smoothed against their
neighbors and gated by a corner-detection check (only trust the estimate
where the local image actually constrains a 2D direction, e.g. the cursor
sprite — not a bare edge/line, which is the classic optical-flow "aperture
problem" and was a real source of artifacts before this check existed) — see
`etc/DEV_NOTES.md` for the full history of why the shader looks the way it
does. (Earlier versions offered several Flat/Linear/Cinematic/Heavy
weighted-average presets alongside this; removed 2026-07-03 to simplify down
to the one mode actually in use.)

Set from the "GMix Motion Blur" source's Properties dialog in OBS, saved
with your scene collection:

- **Blur density** (4-64) — the CEILING on taps per real frame along that
  frame's own motion direction; higher allows a denser streak for genuinely
  fast motion. The shader caps the taps it actually spends per pixel to that
  pixel's real local displacement (near-static content costs ~1 tap
  regardless of this slider), so raising the ceiling only costs GPU time
  where there's real fast motion to resolve, not across the whole frame —
  live-tested at density=64 with no framerate regression.
- **Blur brightness** (0.1-10, default **1.0**) — exposure boost applied
  ONLY where real motion was detected (gated by the estimated per-pixel
  motion magnitude), so it brightens/dims the trail without touching
  static, non-moving parts of the scene. 1.0 is neutral (pure average, no
  boost) and, with the per-frame motion estimation above actually filling
  in the trail properly, is now a perfectly usable default rather than
  visibly under-bright — tune it up if you want a hotter/more visible trail.
  The max of 10 is left in deliberately as a "how absurd can it get" option
  (confirmed live: it blows out the trail hard enough to hurt your OWN
  in-game visibility/accuracy, not a serious setting for real use).

Both sliders apply **live** to whatever's currently running — no need to
remove/re-add a source to see a change take effect.

**GPU index** (-1 = auto), also in Properties, is different: it's a
process-wide setting (one shared engine for however many "GMix Motion
Blur" source instances exist), fixed for the engine's whole lifetime once
the first source creates it. Changing it takes two steps:

1. Open Properties on any "GMix Motion Blur" source and change GPU index.
   This saves your choice to `~/.config/gmix/engine_settings` for next
   time, but does **not** affect whatever's already running.
2. Remove **every** "GMix Motion Blur" source (from every scene) and add
   one back. The fresh engine picks up the value you just saved in step 1.

Just restarting OBS does *not* apply a change — the same sources reload
with the same settings they already had. If you skip step 1, or step 2
doesn't fully remove every instance, check the OBS log for a `gmix:`
warning explaining what happened.

The blend engine also multi-buffers its output (5 GPU dst images, cycled
round-robin) to tolerate variance in the blend's own timing (GPU contention
with the game, drift over a long session) without a slow blend racing an
in-progress OBS read. This is fixed (not user-configurable) and sets the
end-to-end latency budget the status log checks against: 4 output-frame
intervals. If `blend_latency + draw_latency` exceeds that, you'll see a
`gmix: end-to-end (blend+draw) latency exceeds the budget` warning; the fix
is a lower Blur density.

## Known issues

None currently open. Adding "GMix Motion Blur" as a NEW source per scene
used to only render in the first scene, since each plugin instance raced to
bind the same capture socket; fixed by hoisting the capture/blend pipeline
into one process-wide shared engine that all source instances attach to —
see `etc/DEV_NOTES.md`. You can use `+` -> "GMix Motion Blur" in as many
scenes as you like, or reuse one source via **Add Existing Source**; both
work for the blur output itself, **but** for GPU index changes and for the
Properties dialog to reliably display the actually-running config, stick to
ONE "GMix Motion Blur" source (added via `+` once, then reused across
scenes via **Add Existing Source**) rather than several independent
`+`-added instances — see `etc/DEV_NOTES.md`'s entries on this.

## Notes

- Zero-copy relies on `VK_EXT_external_memory_dma_buf` + `VK_IMAGE_TILING_LINEAR`
  (this GPU has no `VK_EXT_image_drm_format_modifier`) on the producer/layer
  side, and OBS's public `gs_texture_create_from_dmabuf` on the import side.
  There is no CPU copy anywhere on the live path (one GPU-side blit on the
  producer, fd-passing over the socket, GPU import on the OBS side); when a
  producer connects, the plugin confirms it in the OBS log with
  `gmix: producer connected (WxH) -- zero-copy dma-buf receive active`.
- Frame handoff is capped at ~4500 fps on both sides (export throttle on the
  producer, hard receive cap in the plugin), so a game running at "unlimited"
  fps cannot flood the receive path either way. The test suite includes
  `ctest -R gpu_bandwidth`, which measures the GPU's device-to-device copy
  bandwidth (RX 480: ~122 GB/s) to confirm the frame handoff is nowhere near
  memory-bound.
- A previous version used a v4l2loopback virtual camera as the output sink.
  That path (and its vendored out-of-tree kernel module) has been removed
  entirely — it was implicated in a kernel panic and is superseded by the
  native OBS plugin's zero-copy delivery.
- The capture layer no longer does any per-present logging. It used to
  (unconditionally, every intercepted present call) write to both stderr
  and a `~/.cache/gmix/presents.log` debug file — real, synchronous disk
  I/O inside the game's own present thread, and a genuine cause of in-game
  stutter independent of GPU load (fixed 2026-07-05, see "System
  requirements" above). If you're on an old build and see a multi-GB
  `~/.cache/gmix/presents.log`, that's this bug; it's safe to delete and
  won't come back on a current build. The one diagnostic that replaced it
  is a rate-limited `gmix: blend buffer overload` warning in the OBS log,
   logged only when the blend pipeline is actually dropping backlogged
   frames (i.e. a real overload, not routine per-frame noise). A second,
   1-in-60-present log line in `vkQueuePresentKHR` also survived that fix;
   it is now opt-in via `GMIX_LAYER_DEBUG=1` (along with the layer's
   `/tmp/gmix_layer_debug.log` tracing) so the default hot path does no I/O
   at all. The one line the layer still prints to stderr is a single
   `VkLayer_GMIX: no capture -- process names [...]` diagnostic per Vulkan
   instance, which is what to look at if capture doesn't start (it shows both
   what the process is called and the target it was matched against — e.g.
   Wine's `osu!.exe` vs target `osu!`).
