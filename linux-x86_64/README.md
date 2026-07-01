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

**Windows:** not yet ported — spec will be listed here once that happens.

## Build

    cmake -S . -B build -G Ninja \
      -DGMIX_BUILD_OBS_PLUGIN=ON \
      -DOBS_INCLUDE_DIR=/path/to/obs/include   # e.g. the Flatpak OBS's .../files/include/obs
    cmake --build build

This produces:
- `build/gmix` — the setup/debug CLI
- `build/layer/VkLayer_GMIX.so` (+ manifest) — the Vulkan capture layer
- `build/obs_plugin/bin/64bit/obs-gmix-source.so` (+ `data/locale/en-US.ini`)
  — the OBS plugin

## Setup

1. **Install the plugin into OBS.** For the Flatpak OBS:

       mkdir -p ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/{bin/64bit,data/locale}
       cp build/obs_plugin/bin/64bit/obs-gmix-source.so ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/bin/64bit/
       cp build/obs_plugin/data/locale/en-US.ini ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/data/locale/

   Restart OBS, then add a **"GMix Motion Blur"** source to a scene (once —
   it persists in your scene collection).

2. **Register the capture layer + enable capture for your game.** Edit
   `config/gmix_config.ini`'s `[capture]` section (target process name, and
   either `appimage` or `flatpak_app_id`), then:

       ./config/gmix_launch.sh

   For a Flatpak target this grants it PERSISTENT capture access via
   `flatpak override` (filesystem access to the layer/IPC-socket dirs, plus
   `ENABLE_GMIX`/`GMIX_TARGET_PROCESS`/`VK_LAYER_PATH`/`XDG_DATA_DIRS` env
   vars) — after running it once, launch the game normally (app menu,
   `flatpak run`, etc.); capture is always on, no special launch command
   needed.

3. Launch the game. Gameplay should appear live (motion-blurred) in the
   "GMix Motion Blur" source's OBS preview.

## Notes

- Zero-copy relies on `VK_EXT_external_memory_dma_buf` + `VK_IMAGE_TILING_LINEAR`
  (this GPU has no `VK_EXT_image_drm_format_modifier`) on the producer/layer
  side, and OBS's public `gs_texture_create_from_dmabuf` on the import side.
- A previous version used a v4l2loopback virtual camera as the output sink.
  That path (and its vendored out-of-tree kernel module) has been removed
  entirely — it was implicated in a kernel panic and is superseded by the
  native OBS plugin's zero-copy delivery.
