# GMix (Windows) — developer handoff / status

> Companion to `../../linux-x86_64/etc/DEV_NOTES.md`. Read that one first for
> the original architecture's hard-won lessons -- almost all of them carry
> over unchanged (see the mapping table below). This file only covers what's
> genuinely different on Windows, plus what's built vs. not yet built here.

## Status: builds clean under MSVC; capture/blend pipeline not yet run against a real game/OBS

This started as a from-scratch source port written without access to a
Windows build environment, then was actually compiled and smoke-tested with
a real MSVC Build Tools + Windows SDK install (`cmake -G Ninja` + `link.exe`,
no OBS SDK available so `GMIX_BUILD_OBS_PLUGIN` stayed OFF). Verified so far:

- `gmix_core` (D3D11 context, blend engine, IPC receive, imported-frame pool)
  and `gmix.exe` compile and link cleanly.
- `test_weight_generator`: 43/43 checks pass.
- `gmix.exe --list-gpus`: real `D3D11CreateDevice` succeeds, enumerates
  actual adapters (tested on an AMD RX 480 -- the D3D11-side blend/consumer
  path needs no NVIDIA extension, unlike the GL/D3D11 interop producer path).
- `gmix.exe --install-proxy <dir>`: copies `opengl32.dll` + `opengl32_orig.dll`
  correctly.
- The proxy `opengl32.dll` builds and its export table is exactly the
  intended shape (verified via `dumpbin /exports`): `wglSwapBuffers` is a
  real local export, ~367 others (`glBegin`, `wglCreateContext`, etc.) are
  genuine forwarders to `opengl32_orig`.

**Not yet verified**: the actual capture pipeline end-to-end (proxy DLL
injected into a real osu!stable process, GL/D3D11 interop or readback
capture, named-pipe handoff, D3D11 compute blend, OBS import) -- none of
that has been exercised against a running game or OBS build yet. Three real
bugs were found and fixed purely by getting a clean compile (see "Bugs found
by actually building this" below); more are likely waiting in the
not-yet-exercised runtime paths.

### Bugs found by actually building this (fixed)

1. **`blend.hlsl`: SM5.0 forbids non-literal resource-array indexing, even
   inside an `[unroll]` loop with a compile-time-bounded trip count.** The
   original `for (i=0; i<frameCount; ++i) srcImages[i]...` (frameCount is a
   runtime cbuffer value) failed to compile ("array index must be a literal
   expression" / "forced to unroll loop, but unrolling failed"). Fixed by
   expanding the accumulate into 64 literal-indexed statements via a
   `SAMPLE(k, ...)` macro instead of a real loop -- see the shader's header
   comment for the full reasoning.
2. **`gl_dx_interop_capture.cpp`: local `HANDLE h` variables shadowed the
   `uint32_t h` (height) parameter** in `ensureRing()`/`captureViaInterop()`,
   causing "redefinition of formal parameter" / wrong-type errors at the
   `wglDXLockObjectsNV(..., &h)` call sites. Renamed the locals to
   `interopHandle`.
3. **The proxy DLL's generated `.def` forwarders failed to link**
   ("unresolved external symbol" for every single forwarded GL function).
   Root cause, confirmed by isolating it in a minimal repro: MSVC's `name =
   module.name` `.def` forwarder syntax requires `name` to already resolve
   as a known import symbol from SOME linked import library -- it is NOT a
   purely textual/free-form forward the way the module name suggests (the
   module name in the `.def` line is written into the export table verbatim
   regardless of which import library actually satisfied the resolution
   check). Fixed by linking the Windows SDK's own `opengl32.lib` import
   library into the proxy target (never called -- `proxy_common.cpp` only
   ever calls the real DLL via `GetProcAddress` -- just present so the
   linker's forwarder-validity check passes), which in turn required giving
   the proxy's own import lib a distinct `ARCHIVE_OUTPUT_NAME` (it would
   otherwise collide with the SDK's `opengl32.lib` by filename in the same
   build directory). See `generate_def.ps1`'s header comment and the
   `CMakeLists.txt` comments at `target_link_libraries(gmix_proxy_opengl32 ...)`.
   Also separately: the `.ps1` script's `New-Object
   System.Collections.Generic.HashSet[string]($Overridden)` call hit a
   PowerShell overload-resolution quirk with a single-element array argument
   (it picked the `HashSet(int capacity)` constructor and threw trying to
   convert the string to an int) -- replaced with a plain hashtable used as
   a set.
4. **`D3D11Context::init()` hard-failed on any machine without the "Graphics
   Tools" optional Windows feature installed**, because `_DEBUG`-gated
   `D3D11_CREATE_DEVICE_DEBUG` was unconditional in a Debug build and
   `D3D11CreateDevice` returns `DXGI_ERROR_SDK_COMPONENT_MISSING` instead of
   just running undebugged when that component is absent. Fixed by retrying
   once without the debug flag on that specific failure.

## What's built (phase 1 scope, osu!stable/OpenGL producer)

- `src/gmix.hpp`, `src/blend/weight_generator.*`, `src/blend/BlendConfig.hpp`
  — verbatim ports (pure math, no platform dependency).
- `src/ipc/frame_protocol.hpp`, `frame_sender.*`, `frame_receiver.*` — named
  pipe (`\\.\pipe\gmix_frames`) transport.
- `src/d3d11/context.*` — adapter enumeration + device creation.
- `src/ipc/imported_frame.*` — opens a producer's named shared texture +
  its keyed mutex.
- `src/blend/blend_engine.*` + `shaders/blend.hlsl` — D3D11 compute blend.
- `proxy_dll/` — the injection-free `opengl32.dll` capture proxy (hooks
  `wglSwapBuffers`, GL/D3D11 interop or CPU-readback fallback into the export
  ring, named-pipe producer side).
- `src/obs_plugin/gmix_source.cpp` — the OBS plugin, **with the multi-scene
  bug fixed at the design level** (see below) rather than reproduced.
- `src/cli.*`, `src/main.cpp` — `gmix.exe --install-proxy` / `--list-gpus`.

## What's NOT built

- **osu!lazer / native D3D11 producer (phase 2).** Not designed in detail;
  see the top-level plan this port followed. Would add a second proxy DLL
  (`dxgi.dll` or `d3d11.dll`) hooking `IDXGISwapChain::Present`, feeding the
  SAME ring/blend/OBS pipeline with no interop hop.
- **The debug `--attach` IPC client** the Linux `gmix` CLI has (`ipc_client.cpp`)
  -- out of scope for this pass; `gmix.exe` here only does `--install-proxy`/
  `--list-gpus`.
- **`test_blend_engine`/`test_ipc` equivalents** (GPU dispatch / named-pipe
  round-trip tests) -- only the pure-math `test_weight_generator` was ported.
  Writing these requires an actual D3D11 device / pipe harness to iterate
  against, which wasn't available while drafting this port.
- Multi-GPU-adapter retry in the proxy's D3D11 device creation (currently
  picks the default hardware adapter; if that's not the same adapter the
  game's GL context is actually bound to, `wglDXOpenDeviceNV` will fail and
  capture falls back to CPU-readback rather than trying other adapters).

## The non-obvious things (Windows-specific; see the Linux DEV_NOTES.md for
## the ones that carry over UNCHANGED -- ring-not-single-buffer, drop
## backlog, fixed-time shutter, plain weighted blend)

1. **D3D11 has no "open a fence by name" API**, so the original plan's idea
   of a named `ID3D11Fence` (direct timeline-semaphore analogue) doesn't
   work: `ID3D11Device5::OpenSharedFence` only accepts a raw `HANDLE`, and
   handles aren't valid across processes without `DuplicateHandle`.
   **`IDXGIKeyedMutex` is used instead** -- it travels WITH the shared
   texture (just `QueryInterface` on the same opened resource), so opening
   the texture by name is the only import step needed. See
   `src/ipc/frame_protocol.hpp`'s header comment for the full reasoning and
   the Acquire(0)/Release(1) ↔ Acquire(1)/Release(0) ping-pong protocol.

2. **The proxy DLL's forwarding `.def` is generated, not hand-written**
   (`proxy_dll/generate_def.ps1`, run by CMake at build time via
   `dumpbin /exports` on the real system `opengl32.dll`). Hand-typing ~400
   forward lines for every GL1.1 export is both tedious and a correctness
   risk; forwards also deliberately target `opengl32_orig.<name>` (a renamed
   sibling file `gmix.exe --install-proxy` places next to the proxy), NOT
   `opengl32.<name>` -- forwarding to the bare system name would resolve via
   normal DLL search order back to the proxy itself (infinite self-reference)
   since shadowing the system DLL via search order is the whole point of this
   technique.

3. **GL's default framebuffer is stored bottom-up; D3D reads textures
   top-down.** The interop capture path's `glBlitFramebuffer` call flips Y
   explicitly (`gl_dx_interop_capture.cpp`'s `captureViaInterop`); the
   CPU-readback fallback flips rows manually during the copy. Skipping this
   produces a vertically mirrored blend on the consumer side -- easy to miss
   in early testing since a static/symmetric test scene can look fine
   mirrored.

4. **Producer and consumer must be on the SAME GPU adapter (LUID).** D3D11
   shared handles (named or not) don't work across adapters. This has no
   real Vulkan-side analogue (OPAQUE_FD sharing has the same underlying
   constraint, but single-GPU Linux desktops made it moot in practice) --
   flag it prominently for anyone with a multi-GPU or hybrid-graphics laptop
   testing this. `D3D11Context::init()`'s `preferLuid` parameter exists so
   the OBS plugin side COULD pin itself to match the game's adapter once
   there's a way to learn that LUID (not yet wired up -- currently both
   sides just auto-pick the default hardware adapter, which is correct on
   the common single-discrete-GPU case this was drafted against).

5. **`WGL_NV_DX_interop2` is NVIDIA-only in practice.** See
   `../README.md`'s "biggest platform-compatibility gap" section. The
   CPU-readback fallback (`captureViaReadback`) exists specifically so the
   plugin still functions (correctly, just not zero-copy) on AMD/Intel.

## KNOWN BUG on Linux, FIXED HERE: "GMix only renders in the first scene"

The Linux plugin's per-`GmixSource`-instance worker thread each calls
`FrameReceiver::listen()` on the same hardcoded socket path, so a second
OBS source instance's `listen()` silently fails (see the Linux DEV_NOTES.md
section of the same name for the full symptom/root-cause writeup). This
port's `gmix_source.cpp` implements the "real fix" that file recommends
(option (a)): a single process-wide `GmixPipeline` (one named-pipe listener,
one worker thread, one blend engine, ref-counted via `shared_ptr`) that every
`GmixSource` instance attaches to via `GmixPipeline::acquire()`. Adding
"GMix Motion Blur" to N scenes creates N source instances, all reading the
same pipeline's current front texture -- no listener collision, no
first-scene-only limitation. **Not yet tested against a real multi-scene OBS
setup** (see "Status" above).

## Likely next directions (not yet built)

- osu!lazer / D3D11-native producer (phase 2 -- see `../README.md`).
- Multi-adapter retry in the proxy's device creation + a way for the OBS
  plugin to learn and pin to the game's adapter LUID (see note 4 above).
- `test_blend_engine`/`test_ipc`-equivalent automated tests once a build
  environment is available to iterate against.
- The debug `--attach` IPC client (`gmix.exe`), if it turns out to be useful
  for diagnosing a stuck producer/consumer connection in the field.
