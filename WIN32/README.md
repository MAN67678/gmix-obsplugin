# gmix-obsplugin (Windows)

Windows port of the zero-copy OBS motion-blur plugin (see `../linux-x86_64/`
for the working Linux/Vulkan original). **Phase 1, targeting osu!stable
(OpenGL) → a D3D11 gmix/OBS pipeline.** osu!lazer (native D3D11) is planned
as phase 2 and is not implemented yet.

## How it fits together (phase 1)

```
osu!.exe (OpenGL) -> proxy opengl32.dll (wglSwapBuffers hook)
   -> GL/D3D11 interop blit into a ring of named, shared D3D11 textures
   -> named pipe (\\.\pipe\gmix_frames) carries only per-frame headers
   -> obs-gmix-source plugin (D3D11 compute blend)
   -> gs_texture_open_shared -> OBS renders it directly ("GMix Motion Blur")
```

Unlike the Linux side (OpenGL-based OBS, so the plugin needs a dma-buf/GL
import hop), Windows OBS is natively D3D11 -- so here the *producer* (an
OpenGL game) is the side that needs an interop hop, not the consumer.

## ⚠ Biggest platform-compatibility gap: NVIDIA-only zero-copy

The producer's zero-copy path uses `WGL_NV_DX_interop2` to write osu!'s
OpenGL backbuffer directly into a shared D3D11 texture without a CPU
round-trip. **This extension is effectively NVIDIA-only** — AMD and Intel
drivers do not implement it in practice. On any other GPU, capture
automatically falls back to a CPU-readback path (`glReadPixels` →
`ID3D11DeviceContext::UpdateSubresource`): still correct, but not zero-copy,
and meaningfully slower at osu!'s typical uncapped framerates. If you're on
AMD/Intel, expect a real CPU/framerate cost from running GMix — this is the
Windows-side analogue of the Linux README's CPU-cost caveat, but worse here
because the whole capture step, not just the blend, runs on the CPU.

## System requirements

- Windows 10 (1803+) / Windows 11 — needed for `ID3D11Device1`
  (`OpenSharedResourceByName`) and `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
  support, both required by the IPC design (see `etc/DEV_NOTES.md`).
- GPU: any D3D11.1-capable GPU works for the blend/consumer side. **NVIDIA
  strongly recommended** for the zero-copy producer path (see above).
- osu!stable (OpenGL renderer).
- OBS Studio (a build whose bundled `libobs` exposes `gs_texture_open_shared`
  — any recent Windows OBS release).

## Dependencies

Build-time only:
- CMake >= 3.18, MSVC (Visual Studio 2019+ Developer environment).
- Windows SDK (`d3d11_1.h`, `dxgi1_2.h`, `fxc.exe`, `dumpbin.exe`).
- PowerShell (for `proxy_dll/generate_def.ps1` -- generates the proxy DLL's
  full-passthrough `.def` from the real system `opengl32.dll`'s own export
  table at build time; see that script's header comment for why this can't
  be a hand-written static file).
- OBS Studio's development headers (`obs.h`, `obs-module.h`) and import
  library (`obs.lib`) to build `obs-gmix-source`.

## Build

```
cd gmix-obsplugin/WIN32
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DGMIX_BUILD_OBS_PLUGIN=ON ^
  -DOBS_INCLUDE_DIR=C:\path\to\obs-studio\libobs ^
  -DOBS_IMPORT_LIB=C:\path\to\obs.lib
cmake --build build --config Release
```

This produces:
- `build\Release\gmix.exe` — the setup/debug CLI
- `build\Release\proxy\opengl32.dll` — the capture proxy (not yet installed
  anywhere; see below)
- `build\Release\obs_plugin\bin\64bit\obs-gmix-source.dll` — the OBS plugin

## Install

1. Copy `obs-gmix-source.dll` into your OBS plugins folder (typically
   `C:\Program Files\obs-studio\obs-plugins\64bit\`).
2. Restart OBS, add a **"GMix Motion Blur"** source to a scene (once — it
   persists in your scene collection).
3. Install the capture proxy into osu!stable's install directory:

       gmix.exe --install-proxy "C:\path\to\osu!"

   This drops **two files** into that directory: `opengl32.dll` (the proxy)
   and `opengl32_orig.dll` (a renamed copy of the real system driver DLL the
   proxy forwards everything else to). Re-run this after any osu! update
   that might touch its own directory contents.
4. Launch osu!stable normally. Gameplay should appear live (motion-blurred)
   in the "GMix Motion Blur" source's OBS preview.

## Known issues / limitations

- **NVIDIA-only zero-copy producer path** — see above.
- **osu!lazer is not supported yet** (phase 2, not implemented — see
  `etc/DEV_NOTES.md`'s "Likely next directions").
- The multi-scene bug the Linux README documents (adding "GMix Motion Blur"
  as a *new* source per scene only renders in the first one) is **fixed** on
  this port at the design level — see `etc/DEV_NOTES.md`'s singleton
  pipeline note — but has not been tested against a real OBS build yet.
- Producer and consumer must run on the **same GPU adapter** (a D3D11
  limitation of cross-process shared handles) — if you have a multi-GPU
  laptop, make sure osu! and OBS are both pinned to the same adapter (the
  `gpu_index` OBS source setting, and the discrete/integrated GPU preference
  in Windows Graphics Settings for osu!.exe, should agree).
