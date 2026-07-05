# gmix-obsplugin (Windows)

Windows port of the zero-copy OBS motion-blur plugin (see `../linux-x86_64/`
for the working Linux/Vulkan original). **Phase 1, targeting osu!stable
(OpenGL) → a D3D11 gmix/OBS pipeline.** osu!lazer (native D3D11) is planned
as phase 2 and is not implemented yet.

## How it fits together (phase 1)

```
gmix-inject.exe -> CreateRemoteThread+LoadLibraryW -> GmixCapture.dll injected into osu!.exe
   -> inline hook on gdi32!SwapBuffers (+ opengl32!wglSwapBuffers, secondary)
   -> GL/D3D11 interop blit into a ring of shared D3D11 textures
      (WGL_NV_DX_interop2; CPU-readback fallback if unavailable on some driver)
   -> named pipe (\\.\pipe\gmix_frames) carries per-frame headers +
      a DuplicateHandle'd texture handle (first use per slot/connection)
   -> obs-gmix-source plugin (D3D11 compute blend)
   -> gs_texture_open_shared -> OBS renders it directly ("GMix Motion Blur")
```

Unlike the Linux side (OpenGL-based OBS, so the plugin needs a dma-buf/GL
import hop), Windows OBS is natively D3D11 -- so here the *producer* (an
OpenGL game) is the side that needs an interop hop, not the consumer.

## History: why this is runtime injection, not a dropped-in DLL

The original design shadowed `opengl32.dll` next to the game's exe (same
"drop a file, implicit activation" philosophy as the Linux Vulkan layer) and
hooked its `wglSwapBuffers` export. Confirmed against real, running
osu!stable that this **never intercepted anything**: its .NET/OpenTK
renderer calls `gdi32!SwapBuffers`, not `wglSwapBuffers` — this is the
standard, documented way OpenGL double-buffering works on Windows, not an
osu!-specific quirk, so most OpenGL apps' actual present call never touches
`wglSwapBuffers` regardless of DLL shadowing. Even if it had called the right
function, .NET P/Invoke resolves native calls dynamically with no static
PE import-table slot, so IAT-style patching would also have caught nothing.

The fix: genuine runtime injection (`gmix-inject.exe`, classic
`CreateRemoteThread`+`LoadLibraryW`) loading `GmixCapture.dll` into the
already-running game process, which then installs an **inline hook**
(patches the first few bytes of the real exported function's machine code
with a `jmp` to a detour, after relocating the displaced instructions into a
trampoline — see `proxy_dll/inline_hook.cpp`) directly on `gdi32!SwapBuffers`.
This works regardless of how the caller resolved the function (P/Invoke,
static import, `GetProcAddress`, anything) because it patches the function's
actual code, not an import table. No game files are ever touched, and the
game is fully playable/exits normally whether or not this DLL is ever
injected. **The hook mechanism itself is confirmed working end-to-end
against real osu!stable** (both hooks fire, D3D11 device + capture ring
initialize, frames reach OBS and blend) — but see "⚠ Known issues" below:
real-world performance/smoothness of the resulting capture is NOT yet
resolved. See `etc/DEV_NOTES.md` for the full history.

## ⚠ Known issues: real-world performance/smoothness NOT resolved

As of the last session, the pipeline runs end-to-end but does not perform
well: osu!'s "unlimited" framerate setting normally reaches thousands of
fps, and this capture keeps it well below that, and the OBS preview visibly
looks stale/laggy at times during real play. The last diagnostic added
found a constant ~81% `IDXGIKeyedMutex::AcquireSync` failure rate,
independent of load, with a documented but UNTESTED hypothesis (a capacity
mismatch between the producer's 48-slot export ring and the consumer's
64-deep frame queue). **Read `etc/DEV_NOTES.md`'s "UNRESOLVED at end of
session" heading before trusting anything below this line describes a
smooth experience.**

## Platform-compatibility note: zero-copy driver dependency

The producer's zero-copy path uses `WGL_NV_DX_interop2`
(`wglDXOpenDeviceNV`/`wglDXRegisterObjectNV`) plus `GL_EXT_memory_object_win32`
for the actual texture import, to write osu!'s OpenGL backbuffer directly
into a shared D3D11 texture without a CPU round-trip. Despite the "NV" in
its name and its reputation as an NVIDIA-only extension, **`WGL_NV_DX_interop2`
was confirmed to work correctly on an AMD RX 480** via both a standalone
functional probe and a live osu!stable capture session — don't trust general
vendor-support claims over an actual call; verify on the target driver
directly (see `etc/DEV_NOTES.md` for the full story, including why
`wglDXLockObjectsNV`/`UnlockObjectsNV` specifically were dropped in favor of
`GL_EXT_memory_object_win32`'s lock-free import, once they turned out to be
a real per-frame performance cost). If `wglDXOpenDeviceNV` is unavailable on
some other driver, capture automatically falls back to a CPU-readback path
(`glReadPixels` → `ID3D11DeviceContext::UpdateSubresource`): still correct,
but not zero-copy, and meaningfully slower at osu!'s typical uncapped
framerates.

## System requirements

- Windows 10 (1803+) / Windows 11 — needed for `ID3D11Device1`
  (`OpenSharedResource1`) and `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
  support, both required by the IPC design (see `etc/DEV_NOTES.md`).
- GPU: any D3D11.1-capable GPU works for the blend/consumer side. Zero-copy
  producer capture needs `WGL_NV_DX_interop2` support (confirmed present on
  both NVIDIA and current AMD drivers; a CPU-readback fallback covers
  anything that lacks it).
- osu!stable (OpenGL renderer), **32-bit build** — the injector and capture
  DLL must match the target process's architecture (osu!stable runs as a
  32-bit/WOW64 process even on 64-bit Windows; build with the x86 toolchain,
  see below).
- OBS Studio (a build whose bundled `libobs` exposes `gs_texture_open_shared`
  — any recent Windows OBS release).

## Dependencies

Build-time only:
- CMake >= 3.18, MSVC (Visual Studio 2019+ Developer environment).
- Windows SDK (`d3d11_1.h`, `dxgi1_2.h`, `fxc.exe`).
- OBS Studio's development headers (`obs.h`, `obs-module.h`) and import
  library (`obs.lib`) to build `obs-gmix-source`.

## Build

osu!stable is a **32-bit process**, so `GmixCapture.dll`/`gmix-inject.exe`
must be built x86 even on 64-bit Windows. Use the x64-hosted, x86-targeting
toolchain (`vcvarsamd64_x86.bat`), not the native x64 one:

```
cd gmix-obsplugin\WIN32
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
cmake -G Ninja -S . -B build-x86 -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86 --target gmix_capture gmix-inject gmix
```

This produces:
- `build-x86\capture\GmixCapture.dll` — the injected capture layer
- `build-x86\capture\gmix-inject.exe` — the injector
- `build-x86\gmix.exe` — the setup/debug CLI (`--list-gpus`)

The OBS plugin (`obs-gmix-source`, a 64-bit DLL — OBS itself is 64-bit) is a
separate, ordinary x64 build:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DGMIX_BUILD_OBS_PLUGIN=ON ^
  -DOBS_INCLUDE_DIR=C:\path\to\obs-studio\libobs ^
  -DOBS_IMPORT_LIB=C:\path\to\obs.lib
cmake --build build --config Release --target obs-gmix-source
```

producing `build\obs_plugin\bin\64bit\obs-gmix-source.dll`.

## Install / run

1. Copy `obs-gmix-source.dll` into your OBS plugins folder (typically
   `C:\Program Files\obs-studio\obs-plugins\64bit\`).
2. Restart OBS, add a **"GMix Motion Blur"** source to a scene (once — it
   persists in your scene collection). OBS starts listening on
   `\\.\pipe\gmix_frames` as soon as the plugin loads.
3. Launch osu!stable normally (no files need to be touched or installed into
   its directory).
4. Run the injector: `gmix-inject.exe` (defaults to targeting `osu!.exe` and
   `GmixCapture.dll` next to itself; pass a different process name/DLL path
   as args if needed). Re-run any time osu! restarts — injection doesn't
   persist across process exit, by design (see "unload cleanly" below).
5. Gameplay should appear live (motion-blurred) in the "GMix Motion Blur"
   source's OBS preview within a couple seconds. Check
   `%TEMP%\gmix_proxy_debug.log` (inside the capture DLL, per-injected-process)
   and the OBS log (`gmix: ...` lines) if it doesn't.

## Diagnostics

Both sides log fps/latency once per second:

- **Producer** (`%TEMP%\gmix_proxy_debug.log`): `producer fps=N` on the
  throttled `onSwapBuffers: capturing ...` status line — the actual export
  rate achieved, gated by `kExportInterval` (see below).
- **Consumer** (OBS log, `gmix: diag: ...` lines): producer/receiver/blend/
  draw fps, backlog-dropped fps, and three latency numbers — capture→receive,
  capture→blend-dispatch, and how stale the currently-displayed blended
  frame is (`blend->now`). Latencies use a shared `gmix::ipc::nowNs()`
  (`QueryPerformanceCounter`), which is valid to subtract across the
  producer/consumer process boundary since QPC is calibrated system-wide.

If `receiver` tracks well below `producer`, or `dropped` is consistently
nonzero, the consumer can't keep up with the producer's export rate — the
`FrameReceiver::hasPendingFrame()` backlog-drop logic is working as intended
in that case (bounding latency at the cost of dropped frames), but it's a
sign the export rate or blend cost needs attention.

**Export-rate throttle (`kExportInterval` in `gl_dx_interop_capture.cpp`,
currently ~250fps/4ms):** `onSwapBuffers` runs *inline* with osu!'s own
render thread (it's called from inside the hooked `gdi32!SwapBuffers`), and
`sendFrame()` does a synchronous, blocking `WriteFile` on the named pipe. At
the original 200µs (~5000fps) throttle — copied from the Linux layer, where
the equivalent send is far cheaper and isn't the rate-limiting step — real
osu!stable gameplay dropped to ~10fps because a slow consumer stalled the
game's own render thread on that blocking write. If you need denser
sub-frame sampling than ~250fps provides, make the pipe write actually
asynchronous (overlapped I/O) rather than lowering this back down.

## Clean unload

The capture DLL never unhooks its inline patch or restores original bytes —
this is intentional: `DLL_PROCESS_DETACH` for this DLL only ever happens at
process exit (nothing calls `FreeLibrary` on it), at which point the entire
process address space is torn down anyway, taking the patched bytes with it.
Restoring bytes at that point would only risk racing a thread mid-call
through them, for no benefit. Closing osu! normally is sufficient to fully
remove the hook and any GMix presence from the system — no cleanup step, no
leftover files, nothing written to the game's own directory.

## Known issues / limitations

- **Real-world performance/smoothness is unresolved** — see the "⚠ Known
  issues" section near the top and `etc/DEV_NOTES.md`'s "UNRESOLVED at end
  of session" heading. Don't treat any other bullet in this list, or
  anything above it in this file, as evidence the plugin is smooth/
  production-ready in practice.
- **Zero-copy producer path depends on `WGL_NV_DX_interop2` support** — see
  above; confirmed working on both NVIDIA and current AMD drivers in
  testing, but falls back to (slower, correct) CPU readback automatically
  if unavailable on some other driver.
- **osu!lazer is not supported yet** (phase 2, not implemented — see
  `etc/DEV_NOTES.md`'s "Likely next directions").
- The multi-scene bug the Linux README documents (adding "GMix Motion Blur"
  as a *new* source per scene only renders in the first one) is **fixed** on
  this port (singleton pipeline, ref-counted) and confirmed via testing.
- Producer and consumer must run on the **same GPU adapter** (a D3D11
  limitation of cross-process shared handles) — if you have a multi-GPU
  laptop, make sure osu! and OBS are both pinned to the same adapter (the
  `gpu_index` OBS source setting, and the discrete/integrated GPU preference
  in Windows Graphics Settings for osu!.exe, should agree).
- `gmix-inject.exe` must be re-run every time osu! restarts (no
  auto-reinject/persistence mechanism exists or is planned — matches the
  "no touching osu file" design constraint).
