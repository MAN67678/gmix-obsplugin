# GMix pipeline — from an osu! frame to a pixel in OBS

This is the "what actually happens, and which file is responsible" reference.
Read this if a live setup is doing something unexpected and you need to know
*which piece* to go look at or restart. For blur-quality/algorithm history
(why the shader looks the way it does), see `DEV_NOTES.md` instead — this
file is about the pipeline's plumbing, not the blur math.

## The workflow, stage by stage

```
 ┌────────────┐  vkQueuePresentKHR   ┌───────────────────────────────┐
 │   osu!     │ ───────────────────▶ │ VkLayer_GMIX (Vulkan layer)   │
 │ (Vulkan,   │  intercepted, frame  │ layer/layer_entry.cpp         │
 │  ~1000fps) │  NOT delayed/altered │ src/capture/*                 │
 └────────────┘                      └───────────────┬───────────────┘
                                                       │ export the about-to-
                                                       │ present image as a
                                                       │ DMA-BUF + timeline
                                                       │ semaphore (producer,
                                                       │ zero-copy — no pixel
                                                       │ readback to the CPU)
                                                       ▼
                                     ~/.cache/gmix/frames.sock
                                     (unix socket, SCM_RIGHTS fd passing —
                                      see src/ipc/frame_protocol.hpp)
                                                       │
                                                       ▼
                          ┌──────────────────────────────────────────────┐
                          │ obs-gmix-source.so  (the OBS plugin)          │
                          │ src/obs_plugin/gmix_source.cpp                │
                          │                                                │
                          │  GmixEngine (one process-wide singleton,       │
                          │  shared by every "GMix Motion Blur" source):   │
                          │   ├─ FrameReceiver  — binds frames.sock,       │
                          │   │  imports each producer frame's DMA-BUF     │
                          │   │  (src/ipc/frame_receiver.cpp,              │
                          │   │   src/ipc/imported_frame.cpp)              │
                          │   ├─ worker thread — walks the ring of         │
                          │   │  imported frames, keeps every real frame   │
                          │   │  whose timestamp falls in the trailing     │
                          │   │  1/60s shutter window, and calls           │
                          │   │  BlendEngine::dispatchAsync()              │
                          │   │  (src/blend/blend_engine.cpp) on the       │
                          │   │  GPU's async-compute queue, running        │
                          │   │  shaders/resample_blur.comp                │
                          │   │  (velocity-aware motion blur — see         │
                          │   │  DEV_NOTES.md for the algorithm)           │
                          │   └─ 5 round-robin dst images (output side)    │
                          └───────────────────┬────────────────────────────┘
                                               │ once a blend finishes, its
                                               │ dst image is exported as
                                               │ GMix's OWN dma-buf (output
                                               │ side; each of the 5 dst
                                               │ images is exported ONCE at
                                               │ creation and reused, not
                                               │ re-exported per frame)
                                               ▼
                              gs_texture_create_from_dmabuf()
                              (OBS's public zero-copy import API)
                                               │
                                               ▼
                              obs_source_info::video_render()
                              draws the current front dst texture whenever
                              OBS's own render loop calls it — OBS paces the
                              output, gmix has no clock of its own here
                                               │
                                               ▼
                                 the "GMix Motion Blur" source,
                                 composited/encoded/streamed like any
                                 other OBS source
```

Two things worth internalizing about this design:
- **Nothing above is a copy to CPU memory.** Every arrow across a process
  boundary (layer → plugin, plugin → OBS) is a DMA-BUF file descriptor
  handoff — the GPU memory itself is never read back or copied, both
  processes just get their own Vulkan/OBS handle onto the *same* physical
  memory.
- **OBS is the only clock.** The capture layer runs at whatever rate osu!
  presents (uncapped, often 1000+ fps); GMix's worker thread reacts to new
  arrivals and to OBS's `video_tick`, but nothing in gmix free-runs on its
  own timer — an earlier version did, and it caused audible/visible judder
  from two independent ~60Hz clocks beating against each other (see
  DEV_NOTES.md's second entry).
- **Under osu-winello the producer side lives in Steam's pressure-vessel
  container** (yawl → sniper runtime → wine → DXVK), not on the host. None
  of the arrows changes: pressure-vessel re-exports the host's implicit
  Vulkan layers into the container (the MangoHud/vkBasalt mechanism), the
  game process shares the host `$HOME` (so `~/.cache/gmix/frames.sock` and
  `~/.config/gmix/target_process` are the same files on both sides), and
  dma-buf fds pass over a unix socket regardless of the container boundary —
  it is the same kernel and the same RADV device either way.

## Where things actually live on disk

Everything gmix touches, and which piece reads/writes it. If a setup is
"stuck" (blur not appearing, second scene not working, GPU index not
sticking), this is the checklist: check the file, check who owns it, check
whether that piece is actually running.

| Path | Written by | Read by | Purpose |
|---|---|---|---|
| `~/.config/gmix/target_process` | the OBS plugin, on source create/update (`gmix_source.cpp`'s "Capture target" setting) | `VkLayer_GMIX` (`layer/layer_entry.cpp`), once per process it's loaded into | Substring-matched against the current process's own names — `/proc/self/exe`'s basename, `/proc/self/comm` and every `argv[0]` (`cmdline`) entry — to decide whether *this* Vulkan process should activate capture at all. The extra two sources exist for Wine: an osu!stable process is really `wine-preloader`, with `osu!.exe` only visible as the task name / Windows argv[0]. If this file is missing/empty, the layer activates for EVERY Vulkan process it's loaded into. |
| `~/.config/gmix/engine_settings` | the OBS plugin, when you change "GPU index" in Properties | the OBS plugin, when a NEW `GmixEngine` is created (i.e. when the first "GMix Motion Blur" source is added after all previous ones were removed) | Persists the GPU index choice across engine restarts. Process-wide, not per-source — see README's "Blur settings" section for why changing it needs a remove-all/re-add cycle. |
| `~/.cache/gmix/frames.sock` | bound by the OBS plugin's `FrameReceiver` (consumer) | connected to by `VkLayer_GMIX` (producer) | The actual frame data channel — dma-buf fd + timeline semaphore handoff per captured frame. Only one listener can bind this at a time; this is why (historically) a second independently-created plugin instance would sit forever "waiting for producer" — fixed by making `GmixEngine` a process-wide singleton, see DEV_NOTES.md. |
| `~/.cache/gmix/gmix_layer_<pid>.sock` | `LayerIpc` inside the capture layer (`src/capture/LayerIpc.cpp`), one per captured process | `gmix --attach` (`src/ipc/ipc_client.cpp`) | A separate DEBUG-ONLY notification channel (status/log messages), not the frame path above. Only relevant if you're using the `gmix` CLI directly to poke at a running layer; the OBS plugin doesn't use it. |
| `~/.local/share/vulkan/implicit_layer.d/VkLayer_GMIX.json` | `gmix --install-layer` (copies `layer/VkLayer_GMIX.json`, pointing at the built `.so` next to it) | the Vulkan loader, for every Vulkan process on the system | Standard implicit-layer registration. The layer only does anything if `ENABLE_GMIX=1` is also set in that process's environment (see the manifest's `enable_environment`) — this is what makes it inert for every OTHER Vulkan app on your machine. |
| `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source/` | you, manually (`cp`, see README) — there is **no** CMake install step | OBS, at startup, like any other third-party plugin | Where the actual `obs-gmix-source.so` + `data/locale/en-US.ini` OBS loads from. **The single most common "I rebuilt but nothing changed" trap**: rebuilding only updates `build/obs_plugin/...`; OBS keeps running the old copy here until you `cp` it over and restart OBS. Exception: a shader-only change doesn't need this — see below. |
| `build/shaders/resample_blur.spv` | `cmake --build build` (compiles `shaders/resample_blur.comp` via `glslangValidator`) | `obs-gmix-source.so`, at every `BlendEngine::init()`, via an **absolute path** baked into the `.so` at compile time (`GMIX_SHADER_DIR`) | Because the path is absolute and points straight at your build tree (not a separately-installed asset), a shader-only edit + rebuild is picked up by the *already-installed* `.so` automatically — no `cp`/reinstall needed for those changes, only for changes to any `.cpp` file. |
| `~/.local/share/osuconfig/configs/00-gmix.cfg` | `config/gmix_launch.sh`, only when `wine_launcher` is set | `osu-wine`, under `set -a` → the game process | How `ENABLE_GMIX=1` / `GMIX_TARGET_PROCESS` reach an osu-winello launch without wrapping the command: exported in the launcher's shell, inherited by yawl → pressure-vessel → wine → DXVK's host Vulkan loader. It is named `00-` because `osu-wine` runs `source "$dir/"*.cfg` and bash's `source` only sources the FIRST match (the rest become `$1`, `$2`...), so any file sorting after another one is silently never applied; the file re-sources the other configs itself. Delete it to disable capture. |
| `~/.cache/gmix/layers/<pid>.status` | `VkLayer_GMIX` in each process that loads it (`src/capture/LayerStatus.hpp`), removed on clean exit, mtime bumped ~1/s | the OBS plugin's Properties dialog (`gmix_source.cpp`'s "Apps with the GMix layer loaded" list) | The layer's self-report: `pid`, `comm`, `exe`, `state` (`not-target` / `waiting` / `capturing`), `target`, `matched`. A file (not `/proc` scanning) because Flatpak OBS lives in its own PID namespace and can't see host processes, while `~/.cache/gmix` is shared. A file whose mtime stops advancing is flagged `[stale - process gone?]`. |
| `flatpak override --user` state (not a gmix-owned file — see `flatpak overrides` in `~/.local/share/flatpak/overrides/<app-id>`) | `config/gmix_launch.sh` | the Flatpak sandbox, for every launch of that app from then on | Grants persistent `--filesystem` access to the layer dir + `~/.cache/gmix` + `~/.config/gmix`, plus persistent `ENABLE_GMIX`/`VK_LAYER_PATH`/`XDG_DATA_DIRS` env vars, so a sandboxed osu!lazer picks up capture on every normal launch without a special wrapper command. Only relevant for Flatpak targets; a native (non-sandboxed) build just needs the env vars set directly. |

## Source layout (build-time files, not runtime state)

| Path | What it is |
|---|---|
| `src/obs_plugin/gmix_source.cpp` | The whole OBS plugin: `GmixEngine` (singleton owning the receiver/blend/worker), `GmixSource` (thin per-instance handle), the Properties dialog, `video_render`/`video_tick` callbacks. Start here for anything OBS-facing. |
| `src/blend/blend_engine.cpp` / `.hpp`, `BlendConfig.hpp` | The Vulkan compute dispatch: builds the pipeline from `resample_blur.spv`, owns the 5 round-robin dst images + their dma-buf exports, `dispatchAsync()`/`pollBlendDone()`. `BlendConfig` is the density/brightness struct the properties dialog fills in. |
| `shaders/resample_blur.comp` | The ONLY blend shader (the "Advanced"/velocity-aware mode — see DEV_NOTES.md for why it's the only one left). All the actual blur-quality logic (per-pair Lucas-Kanade flow, temporal smoothing, the Shi-Tomasi trust check) lives here. |
| `src/ipc/frame_receiver.cpp` / `.hpp`, `imported_frame.cpp` / `.hpp`, `frame_protocol.hpp` | Consumer-side IPC: accept a producer connection, receive per-frame dma-buf fd + semaphore, import into gmix's Vulkan context, pool imports by ring slot. |
| `src/ipc/frame_sender.cpp` / `.hpp` | Producer-side IPC: the capture layer's half of the same protocol — connects out to `frames.sock` and sends each captured frame. |
| `src/capture/VulkanLayerCapture.cpp`, `FrameSource.hpp` | The actual per-frame capture logic run inside the hooked `vkQueuePresentKHR`: exports the presenting image as a dma-buf, hands it to `frame_sender`. |
| `src/capture/LayerIpc.cpp` / `.hpp` | The layer's debug-only per-pid notification socket (see the runtime-file table above). |
| `src/capture/LayerStatus.hpp` | Header-only per-process status report (`~/.cache/gmix/layers/<pid>.status`) the layer writes and the OBS plugin's detected-apps dropdown reads. Not on the frame path. |
| `layer/layer_entry.cpp`, `VkLayer_GMIX.json` | The Vulkan implicit-layer entry points (`vkGetInstanceProcAddr` etc., loader negotiation) and its manifest. Reads `~/.config/gmix/target_process` to decide whether to activate for the current process. |
| `src/vulkan/context.cpp` / `.hpp` | Shared Vulkan device/queue setup (instance, device, graphics + async-compute queue) used by both the `gmix` CLI and the OBS plugin via the `gmix_core` object library. |
| `src/cli.cpp` / `.hpp`, `src/main.cpp`, `src/ipc/ipc_client.cpp` | The `gmix` CLI: `--install-layer` / `--list-gpus` / `--attach`. No capture loop or output sink of its own — see README. |
| `config/gmix_config.ini` | User-edited: capture target process name, plus whichever launcher kind applies — `flatpak_app_id` (lazer Flatpak), `wine_launcher` (osu-winello) or `appimage`. Read only by `gmix_launch.sh`, not by any C++ code. |
| `config/gmix_launch.sh` | One-time setup script: installs/refreshes the capture layer, then arranges persistent capture enablement for the configured launcher — Flatpak sandbox overrides (see above) or osu-winello's `00-gmix.cfg` (and verifies the sourcing it depends on). It also warns if an osu-winello prefix's `d3d9.dll` is not DXVK (no Vulkan present ⇒ nothing to capture). |
| `CMakeLists.txt` | Build config. Notably: no install step for the OBS plugin (deliberate — see the runtime-file table's note on this), the shader-to-SPIR-V custom command, and `GMIX_BUILD_OBS_PLUGIN`/`OBS_INCLUDE_DIR` for opting into the plugin target. |
