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
5. Let you tune the blend (a custom weight curve instead of a flat average)
   via OBS's own source Properties dialog, saved with your scene collection.

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

## Notes

- Zero-copy relies on `VK_EXT_external_memory_dma_buf` + `VK_IMAGE_TILING_LINEAR`
  (this GPU has no `VK_EXT_image_drm_format_modifier`) on the producer/layer
  side, and OBS's public `gs_texture_create_from_dmabuf` on the import side.
- A previous version used a v4l2loopback virtual camera as the output sink.
  That path (and its vendored out-of-tree kernel module) has been removed
  entirely — it was implicated in a kernel panic and is superseded by the
  native OBS plugin's zero-copy delivery.
