# GMix (Windows) — developer handoff / status

> Companion to `../../linux-x86_64/etc/DEV_NOTES.md`. Read that one first for
> the original architecture's hard-won lessons -- almost all of them carry
> over unchanged (see the mapping table below). This file only covers what's
> genuinely different on Windows, plus what's built vs. not yet built here.

## Status (2026-07-05, end of session): pipeline runs end-to-end against REAL osu!stable + real OBS, but real-world performance/smoothness is NOT resolved -- read "UNRESOLVED at end of session" below before touching this again

## UNRESOLVED at end of session: real in-game fps still degraded, OBS output still "stale/laggy"

**Do not assume the perf work described further down in this file is
complete.** It fixed real, confirmed bugs (see the timeline below) but the
end state, confirmed by the user actually playing, is still not right:
osu!'s "unlimited" framerate setting should reach thousands of fps; with
this capture active it doesn't, and the OBS preview visibly looks
stale/laggy at times.

**Prime suspect, NOT YET fixed or even confirmed, just diagnosed:** the very
last diagnostic added this session (`captureViaInterop`'s
`diagAcquireFailCount_`/`diagAcquireAttemptCount_`, logged as `acquire
fails=N/M (X%)` in `%TEMP%\gmix_proxy_debug.log`) showed a **constant ~81%
`IDXGIKeyedMutex::AcquireSync` failure rate** -- independent of whether the
game was running fast (~215fps) or in its degraded ~60fps state. Since each
failed acquire burns the full `kAcquireTimeoutMs` (4ms) blocking the GAME's
own render thread before giving up, an 81% fail rate at ~200-250 attempts/
sec accounts for a huge fraction of every second being spent on blocking
waits that never succeed -- almost certainly the real explanation for why
fps never gets close to "thousands," and very plausibly related to the
stale-looking OBS output too (whatever fraction of exports DO succeed are
competing with this contention).

**Leading hypothesis for the root cause (untested):** a capacity mismatch
between the producer's export ring and the consumer's frame queue depth:
- Producer: `kExportRing = 48` in `gl_dx_interop_capture.hpp` -- only 48
  physical D3D11 texture slots exist, cycled round-robin.
- Consumer: `FrameQueue::kCap = gmix::kMaxBlendFrames` (64, from
  `gmix.hpp`) in `gmix_source.cpp` -- the sliding window can hold up to 64
  `RingEntry`/`ImportedFrame` objects alive at once, and **evicts by COUNT**
  (`while (ring.size() > kCap) ring.pop_front();`), not by whether a frame
  was actually consumed by a blend dispatch.
- Since 64 > 48, the consumer can legitimately be holding onto (not yet
  evicted, hence not yet destructed, hence not yet having released its
  `IDXGIKeyedMutex`) more distinct frame entries than there are physical
  ring slots. Once the producer wraps around and reuses a slot that some
  still-alive `ImportedFrame` elsewhere in the consumer's 64-deep window
  also points at, `AcquireSync` on that slot will fail until that stale
  entry is finally evicted/destructed -- exactly the kind of persistent,
  load-independent contention the diagnostic showed.
- **Next step for whoever picks this up:** either shrink `FrameQueue::kCap`
  to something safely below `kExportRing`, or (probably better) raise
  `kExportRing` well above 64 (the header comment already claims 48 "must
  exceed the consumer's max blend window" but doesn't actually satisfy
  that against the real 64 cap), or change `FrameQueue`'s eviction to be
  based on actual keyed-mutex release rather than raw count. Then re-check
  the `acquire fails=` diagnostic -- it should drop to ~0% if this was the
  real cause.

**Secondary, likely-unrelated-but-real finding (also untested/unfixed):**
the "shutter" is not a literal 16.6ms timestamp-filtered window. The
consumer estimates how many ring entries to feed into a blend dispatch via
`n = round(queue.captureFps() / 60.0)` in `gmix_source.cpp`'s `workerMain`
-- an EMA'd average-rate-based frame COUNT, not a check of each frame's
actual `timestampNs` against "now minus 16.6ms." This works fine when the
capture rate is stable, but during exactly the kind of rate instability
this session was chasing, the estimate can be stale/wrong, feeding a blend
window that doesn't actually correspond to a real 16.6ms shutter. Flagged
by the user directly (see their capture/blend/draw pipeline model note);
worth a real fix (filter `window_frames` by `nowNs() - timestampNs <=
shutterNs` instead of trusting the rate estimate) independent of the
acquire-fail-rate bug above.

### Session timeline (what was actually fixed vs. not, in order)

1. Runtime injection (`gmix-inject.exe`) + inline hook on `gdi32!SwapBuffers`
   -- **fixed and confirmed working** against real osu!stable (see the
   pivot section below for why this replaced DLL shadowing).
2. GL interop mechanism churned through three revisions before landing:
   `WGL_NV_DX_interop2` (assumed NVIDIA-only, wrong) -> `GL_EXT_
   memory_object_win32`+`GL_EXT_win32_keyed_mutex` (keyed-mutex half doesn't
   resolve on this AMD driver, fell back to slow CPU readback) -> reverted
   back to `WGL_NV_DX_interop2` after a real functional probe proved it
   works on this driver -- **confirmed working, zero-copy, via live
   capture**.
3. Original export throttle (200us/~5000fps cap, copied from the Linux
   Vulkan layer) caused real osu! gameplay to drop to ~10fps, because
   `sendFrame()`'s blocking `WriteFile` runs inline on the game's own render
   thread -- **fixed** by lowering to 4ms/~250fps and shortening the
   keyed-mutex acquire timeout from 50ms to 4ms.
4. User reported real in-game frame time still ~7ms (vs. osu!'s normal
   sub-1ms unlimited) and asked for game-fps vs. producer-fps + per-export
   timing diagnostics -- **added** (`presentCount_`, `diagLockSumNs_`/
   `diagSendSumNs_` and friends, logged in `gl_dx_interop_capture.cpp`).
5. User pointed at a reference project (`GMIX-Project-WIN32`) achieving
   "thousands of fps" with no per-frame throttle at all; comparing its
   `producer/gl_hook.cpp` showed it never calls anything like
   `wglDXLockObjectsNV`/`UnlockObjectsNV` (a real CPU-blocking cross-API
   sync point) -- switched the GL import mechanism to `GL_EXT_
   memory_object_win32` (no per-frame lock step needed) while keeping the
   existing `IDXGIKeyedMutex` protocol, called as a plain D3D11 API call
   around the GL blit with `glFlush()` before release.
6. That `glFlush()` version was **NOT SAFE** -- confirmed by testing: OBS-
   side receiver/blend fps collapsed to ~3-4fps with `blend->now` latency
   spiking to 100-280ms, because `glFlush()` only submits work, it doesn't
   wait for completion, so the keyed mutex was released before the GPU
   actually finished the blit, racing the consumer's read. **Fixed** by
   switching to `glFinish()` (waits for GL's own queue to drain).
7. Even after the `glFinish()` fix, the user still saw staleness ("still
   looks stale/laggy at times") -- added the acquire-fail-rate diagnostic
   described above, which found the ~81% constant failure rate. **Session
   ended here, unresolved** -- see the capacity-mismatch hypothesis above
   for the next step.

**Lesson for next time, stated plainly:** verify extension/API support and
performance claims with actual functional tests and real hardware
measurements, not vendor reputation, extension-string presence, or a
reference implementation's claimed numbers taken at face value -- and when
a fix is proposed, get it CONFIRMED by the user actually playing before
calling it done. Several "this should fix it" turns in this session were
wrong or incomplete; the honest state is captured above, not in any
earlier optimistic status line in this file.

This started as a from-scratch source port written without access to a
Windows build environment, then was actually compiled and smoke-tested with
a real MSVC Build Tools + Windows SDK install (`cmake -G Ninja` + `link.exe`).
`GMIX_BUILD_OBS_PLUGIN` was later turned on against a real OBS Studio source
checkout + its already-built `libobs` (`C:\Users\man-win\dev\obs-studio`,
`obs.lib` at `build_x64\libobs\RelWithDebInfo\obs.lib`) -- see "obs-gmix-source
builds against the real SDK" below. Verified so far:

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
- `test_ipc` (named-pipe handshake/FrameHeader round-trip + backlog-drop
  detection via `hasPendingFrame()`): 31/31 checks pass.
- `test_blend_engine` (real D3D11 compute dispatch on the AMD RX 480: solid
  4-source equal-weight blend, 3-source unequal-weight blend, 1-source
  passthrough): 4/4 checks pass, **exact** pixel match (0 mismatches) against
  the CPU reference in all three cases -- this is the strongest evidence yet
  that the actual blend math (weights buffer, cbuffer, resource-array
  indexing workaround, UAV write) is correct on real hardware.

### obs-gmix-source builds against the real SDK

Confirmed against `C:\Users\man-win\dev\obs-studio` (a full OBS Studio source
checkout with `libobs` already built): `obs-module.h`/`obs.h`/
`graphics/graphics.h` compile as expected, `gs_texture_open_shared`'s real
signature (`gs_texture_t *gs_texture_open_shared(uint32_t handle)`) matches
what `gmix_source.cpp` already assumed, and the resulting
`obs-gmix-source.dll` exports exactly the 6 symbols `OBS_DECLARE_MODULE()`/
`OBS_MODULE_USE_DEFAULT_LOCALE()` are supposed to generate
(`obs_module_load`, `obs_module_ver`, etc. -- verified via `dumpbin /exports`)
and correctly imports real runtime entry points from `obs.dll` (`dumpbin
/imports` shows `obs_data_get_string`, `obs_register_source_s`,
`obs_enter_graphics`, etc.). To reproduce elsewhere, configure with:

    -DGMIX_BUILD_OBS_PLUGIN=ON
    -DOBS_INCLUDE_DIR=<obs-studio checkout>/libobs
    -DOBS_IMPORT_LIB=<obs-studio checkout>/build_x64/libobs/RelWithDebInfo/obs.lib
    -DOBS_CONFIG_INCLUDE_DIR=<obs-studio checkout>/build_x64/config

(`OBS_CONFIG_INCLUDE_DIR` is new -- see bug #6 below; an installed OBS SDK
package rather than a from-source checkout may not need it.)

Two more real bugs found getting this far (added to the numbered list below):
a missing include path for `obsconfig.h` (a file CMake generates into
libobs's *build* tree, not its source tree), and a `std::min`/`windows.h`
`min` macro collision in `gmix_source.cpp`. Also had to fix
`obs-gmix-source`'s output directory, which silently landed in the build
root instead of `obs_plugin/bin/64bit/` (a `MODULE`-vs-`SHARED` CMake output-
directory-property difference, not an OBS-specific issue).

### obs-gmix-source runs correctly inside a real, cleanly-installed OBS process

Confirmed by actually installing it and launching real `obs64.exe`
(OBS Studio 32.1.2 retail install) and reading its log:

    gmix: obs-gmix-source (Windows) loaded
    ...
    Loaded Modules:
        obs-gmix-source.dll
    ...
    gmix: waiting for producer to connect...
    ...
    - source: 'GMix Motion Blur' (gmix_source)

That last "waiting for producer to connect..." line is `workerMain()`'s own
`blog()` call (verified by grepping the exact string in the source -- an
earlier, unrelated leftover prototype plugin also happened to log a
similarly-worded line, which caused a false-positive read the first time
through; see "the ID-collision detour" below for how that was caught and
resolved). Reaching that line means, live, inside a real OBS process:
`gmixCreate()` ran, `GmixPipeline::acquire()` started, `D3D11Context::init()`
successfully created a device from *inside obs64.exe* (not just a standalone
test harness), the worker thread spawned, and `FrameReceiver::listen()`
successfully created the named pipe -- the entire OBS-side runtime path
except actually receiving a frame (nothing was connected to the pipe in this
test; that needs the producer side, i.e. a real game or a synthetic
FrameSender). OBS stayed responsive throughout, no crash, no errors logged.

**The ID-collision detour (worth knowing about for next time):** the first
attempt found a *different, unrelated* leftover OBS plugin already installed
at `C:\Program Files\obs-studio\obs-plugins\64bit\gmix-obs-source.dll` (note
the hyphen -- an earlier, separate GMix prototype, not this port) that
registers a source with the exact same id, `"gmix_source"`. Since it loaded
first, our module's `obs_register_source()` call was silently rejected
(`"Source 'gmix_source' already exists! Duplicate library?"`), and a
misleadingly similar-looking "waiting for producer" log line from *that*
plugin was initially misread as ours (it uses a hyphenated pipe name,
`\\.\pipe\gmix-frames`, vs. our underscored `\\.\pipe\gmix_frames` --
grepping the *exact* string, not just "gmix", is what caught the mistake).
Diagnosed by temporarily renaming `gGmixSourceInfo.id` to a unique test value
and confirming clean registration; permanently resolved by removing the old
plugin. **`WIN32/setup.bat`** (new) automates this for anyone else hitting
the same collision: it self-elevates via UAC, removes both the old
prototype's files and any previous copy of this port's plugin, and installs
the freshly built `obs-gmix-source.dll` + locale data. It deliberately does
**not** auto-launch OBS afterward -- an elevated script cannot reliably start
a normal, non-elevated OBS session (confirmed empirically: `start ""
obs64.exe` from inside the UAC-elevated script silently produced no running
OBS process at all); start OBS normally yourself after the script finishes.

### Full producer->consumer zero-copy pipeline verified end-to-end (synthetic producer)

`WIN32/tests/synthetic_producer.cpp` (new -- see its header comment) plays
the producer role without a real game: it creates its own ring of
cross-process shareable, keyed-mutex D3D11 textures, connects to the real
production pipe a running `obs-gmix-source` instance is listening on, and
streams solid-color frames. Getting this to actually work surfaced two
substantial, real bugs -- one a driver limitation invalidating a core design
decision, one a genuine shutdown deadlock -- both now fixed and confirmed by
the OBS log showing, for the first time, `gmix: first blend retired, front=0
-- video_render should now draw`: a real frame was imported cross-process,
blended on the GPU, and handed to OBS as a live texture.

**Bug A -- `ID3D11Device1::OpenSharedResourceByName` is broken on this AMD
driver, unconditionally, even same-process/same-device.** The entire
original texture-sharing design (a texture named
`Local\gmix_frame_<pid>_<slot>`, looked up by the consumer with no handle
ever crossing the wire -- chosen specifically to avoid `DuplicateHandle`)
failed with `E_INVALIDARG` on every attempt. Isolated with a throwaway
same-process probe (create a named shared handle, then immediately try to
open it by name from the SAME device that created it) before touching any
cross-process code -- confirmed `OpenSharedResourceByName` fails
unconditionally on this AMD RX 480 driver, while classic handle-based
`OpenSharedResource1` succeeds immediately. **Fixed by switching the entire
wire protocol** (bumped `kProtocolVersion`/`kMagic`) from named lookup to
classic `DuplicateHandle`: the producer calls `GetNamedPipeServerProcessId`
(it's the pipe's client; the consumer/OBS is the server) + `OpenProcess` +
`DuplicateHandle` to get a HANDLE valid in the consumer's process, and sends
that raw value in a new `FrameHeader::sharedHandleValue` field -- only on the
FIRST frame sent for a given ring slot per connection (0 thereafter, meaning
"already cached", tracked via `RingSlot::handleSentThisConnection`, reset on
each new connection) to avoid duplicating (and leaking) a fresh handle every
single frame. See `frame_protocol.hpp`'s "PROTOCOL HISTORY" comment for the
full writeup; touched `frame_protocol.hpp`, `frame_sender.*`,
`imported_frame.*`, `gl_dx_interop_capture.*`, `synthetic_producer.cpp`, and
`obs_plugin/gmix_source.cpp`'s `receiverThreadFn` (which now must call
`pool.acquire()` even on a dropped/stale frame, since dropping the ONE frame
carrying a slot's only handle would otherwise both leak it and permanently
lose the ability to import that slot for the rest of the connection).

**Bug B -- a real shutdown deadlock in `GmixPipeline`'s singleton lifecycle**
(ironic, since that singleton was itself the fix for the Linux "only renders
in the first scene" bug -- see below). Removing the last "GMix Motion Blur"
source (dropping the `GmixPipeline` shared_ptr's last reference) calls
`~GmixPipeline()`, which set `stop_ = true` and called `worker_.join()` -- but
the worker thread is very likely blocked inside a synchronous
`ConnectNamedPipe`/`ReadFile` call (e.g. no producer connected yet), and a
plain bool flag can't wake a thread out of a blocking OS call. The first fix
attempt (`receiver_.close()` from the destructor thread before `join()`)
did **not** work either -- confirmed by hitting the bug again -- because
closing a handle out from under a DIFFERENT thread's synchronous
(non-overlapped) blocking I/O on it is explicitly unsupported/undefined per
Microsoft's own documentation. The actual fix: `CancelSynchronousIo`, which
targets a **thread handle** (not a file handle) and forces whatever
synchronous I/O call that thread is currently blocked in to return with
`ERROR_OPERATION_ABORTED` -- `CancelSynchronousIo(worker_.native_handle())`
before `join()`. Confirmed fixed: the OBS log's "failed to create named
pipe" error (which fired every time a source was removed-then-re-added
while idle, before this fix) no longer appears. Note this was invisible in
testing as a hang/freeze -- OBS's own UI stayed fully responsive throughout,
because OBS defers the actual source-destructor call to a background
thread, not the UI thread, so the permanently-stuck worker thread was a
silent, easy-to-miss leak rather than an obvious freeze.

Also worth recording: `WIN32/data/locale/en-US.ini` didn't exist before this
port was actually installed and tested, and the OBS plugin's `data/`
directory conventions (`<install>/data/obs-plugins/<module>/locale/...`)
weren't exercised until `setup.bat` needed to stage them for real.

### The DLL-shadowing design abandoned; runtime injection + inline hook adopted, and confirmed working against real osu!stable

Everything in the previous section was verified using `synthetic_producer.cpp`
standing in for the real game. Testing the actual proxy `opengl32.dll`
against real, running osu!stable revealed the entire DLL-shadowing capture
mechanism was non-functional, for two independent, compounding reasons:

1. **osu!stable's .NET/OpenTK renderer calls `gdi32!SwapBuffers`, never
   `wglSwapBuffers`.** This is the documented, standard way OpenGL
   double-buffering works on Windows (`SwapBuffers` is a `gdi32.dll` export
   that internally does the real buffer swap) -- not an osu!-specific quirk.
   A proxy that only shadows `opengl32.dll` and hooks its `wglSwapBuffers`
   export can never see the real present call for an app built this way, no
   matter how correct the shadowing/forwarding logic is.
2. **Even if the right function had been targeted, .NET P/Invoke resolves
   native calls dynamically** (looked up at call time via the CLR's marshaling
   layer, not through a static PE import-table entry), so classic IAT-patch-
   style interception -- which is what "shadow a DLL via search order" really
   relies on -- has nothing to patch regardless of which function is involved.

Confirmed via the 32-bit-PowerShell-host module enumeration technique (64-bit
`tasklist`/PowerShell cannot see inside a WOW64 process's module list) that
osu! loads the real system `opengl32.dll` directly, never the shadowed proxy.

**Fix, per explicit direction ("no touching osu file. hook into osu's render
pipeline live with openGL. unload cleanly when the game close"):** replaced
the entire capture-DLL delivery and interception mechanism:

- `gmix-inject.exe` (`proxy_dll/inject.cpp`) performs genuine runtime
  injection into the already-running target process: `CreateToolhelp32Snapshot`
  to find the pid by name, `VirtualAllocEx`+`WriteProcessMemory` to place the
  DLL path string remotely, `CreateRemoteThread` calling `LoadLibraryW`
  directly in the target process. No game file is ever read, written, or
  renamed.
- `GmixCapture.dll` (renamed from the old proxy; `capture_main.cpp` is its new
  `DllMain`) installs an **inline hook** (`inline_hook.cpp`): patches the
  first ≥5 bytes of the REAL exported function's machine code with a `jmp
  rel32` to a detour, after relocating the displaced whole instructions (via a
  fail-safe x86 instruction-length decoder -- refuses to hook if it can't
  steal only whole, non-branch instructions) into a `VirtualAlloc`'d
  trampoline. This works regardless of how a caller resolved the function
  (P/Invoke, static import, manual `GetProcAddress`) because it rewrites the
  function's actual code, not any table. Hooks BOTH `gdi32!SwapBuffers`
  (primary -- what osu!stable actually calls) and `opengl32!wglSwapBuffers`
  (secondary/best-effort, for any app that does call it directly).
- **Unload**: `DLL_PROCESS_DETACH` deliberately does NOT restore the patched
  bytes or free the trampoline. This is safe, not sloppy: `DLL_PROCESS_DETACH`
  for an injected DLL that's never `FreeLibrary`'d only fires at process exit,
  at which point the whole address space is torn down immediately after --
  attempting to unhook at that moment would only risk racing some other
  thread still mid-call through the patched code, for zero benefit. Closing
  the game is sufficient for a fully clean system state; nothing persists.
- Adapted the injector and inline-hook implementation from a more mature
  sibling project (`C:\Users\man-win\dev\GMIX-Project-WIN32`, `producer/
  inject.cpp` and `producer/gl_hook.cpp`) rather than reinventing it, per
  explicit direction to research before continuing to write code.

**First attempted a GL interop rework** in the same pass, from
`WGL_NV_DX_interop2` (device-registration model, believed NVIDIA-only in
practice per general web research) to `GL_EXT_memory_object_win32` +
`GL_EXT_win32_keyed_mutex` (handle-based import, advertised as cross-vendor
per the Khronos registry). **This assumption turned out to be wrong in both
directions, corrected the same day (2026-07-05):**

- Injecting the `GL_EXT_memory_object_win32` version into real osu!stable
  showed those extension functions failing to fully resolve on this AMD RX
  480 driver (specifically `GL_EXT_win32_keyed_mutex`'s entry points come
  back NULL — confirmed AMD has never shipped that particular extension, on
  any generation, per follow-up research), so capture silently fell back to
  `captureViaReadback` (CPU readback) instead of achieving zero-copy at all.
- A user report of a reference project (`C:\Users\man-win\dev\
  GMIX-Project-WIN32`) claiming working zero-copy on this exact same GPU
  prompted a closer look — that project uses `GL_EXT_semaphore_win32`
  (binary semaphores), NOT keyed-mutex sync, which pointed at keyed-mutex
  specifically being the unsupported piece, not memory-object import itself.
- Rather than reworking to a fence/semaphore-based sync scheme, a direct
  standalone probe (create a real D3D11 device + shared texture, then
  actually CALL `wglDXOpenDeviceNV`/`wglDXRegisterObjectNV`/
  `wglDXLockObjectsNV`, not just check whether the function pointers
  resolve) showed **`WGL_NV_DX_interop2` genuinely works end-to-end on this
  AMD driver** (26.5.2) — Open, Register, and Lock all succeeded. The
  "NVIDIA-only" belief was based on general web-research consensus, never
  actually tested against this project's own hardware; it was simply wrong
  for this (recent) driver.
- **Reverted to `WGL_NV_DX_interop2`** as the interop mechanism (simpler
  than a semaphore/fence rework, and already proven) — see
  `gl_dx_interop_capture.hpp`'s header comment for the full account.
  `GL_EXT_memory_object_win32`/`GL_EXT_semaphore_win32` DO also resolve on
  this driver (confirmed by the same probe) and remain a viable fallback
  avenue if a future driver regresses `WGL_NV_DX_interop2` support, but
  weren't wired up since the simpler, already-working mechanism was
  available.

**Lesson recorded for next time:** verify extension/API support with an
actual functional call on the target hardware, not vendor reputation, an
extension string's mere presence, or whether `GetProcAddress`/
`wglGetProcAddress` returns non-null (a driver can resolve a stub that fails
at call time, and conversely an extension believed unsupported by a vendor
can turn out to work fine on a newer driver).

**Confirmed working end-to-end against real, running osu!stable** (2026-07-05,
final `WGL_NV_DX_interop2` version): injecting `GmixCapture.dll` into a live
`osu!.exe` process while OBS (with a "GMix Motion Blur" source already added)
was running produced, in order: both inline hooks installed successfully, a
D3D11 device created inside osu!'s process, `wglDXOpenDeviceNV` succeeding,
a named-pipe connection to OBS's already-listening `obs-gmix-source`, and —
in the OBS log — `producer connected: 1280x1025 pid=<osu!'s pid>`, both ring
textures imported as `gs_texture_t`, and `first blend retired, front=0 --
video_render should now draw`. Capture continued steadily via the real
interop path (not the CPU fallback) with osu! remaining fully responsive.

**A second, separate bug surfaced immediately after this**: the export
throttle (`kExportInterval`, copied from the Linux Vulkan layer's 200µs /
~5000fps value) allowed an export attempt on nearly every present at osu!'s
uncapped framerate. Unlike Linux, `sendFrame()`'s named-pipe `WriteFile` is
synchronous/blocking and runs *inline* with osu!'s own render thread (this
function executes inside the hooked `gdi32!SwapBuffers` call) — at that
throttle, real gameplay dropped to ~10fps because a consumer that can't
drain the pipe that fast stalls the game itself, not just the capture path.
Fixed by lowering the throttle to ~250fps (4ms) and the keyed-mutex acquire
timeout from 50ms to 4ms (fail fast and skip a frame rather than stall the
game for tens of milliseconds on mutex contention) — see the constants'
comments in `gl_dx_interop_capture.cpp`. Confirmed via the new diag logging
(see below) that this doesn't regress capture: producer/receiver/blend fps
track together with sub-millisecond capture→receive latency.

**Diagnostic logging added** (per request, to make future stalls/regressions
diagnosable without re-deriving them from scratch): a shared, cross-process-
comparable `gmix::ipc::nowNs()` (`QueryPerformanceCounter`-based) in
`frame_protocol.hpp`, used for a `producer fps=` figure on the producer's
existing throttled status log, and a once/second `gmix: diag: ...` line in
the OBS log reporting producer/receiver/blend/draw fps, backlog-dropped fps,
and three latency numbers (capture→receive, capture→blend-dispatch, and how
stale the currently-displayed blended frame is). See `FrameQueue`'s and
`GmixPipeline`'s new atomic counters in `gmix_source.cpp`.

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
5. **A resource created with `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` silently
   drops any write issued before the keyed mutex is ever acquired.** Found
   while writing `test_blend_engine.cpp`'s synthetic source-frame helper: it
   called `UpdateSubresource()` to fill a solid color, then acquired the
   mutex afterwards -- the compute shader's `srcImages[k].Load()` came back
   as `(0,0,0,0)` for every pixel, which looked exactly like a shader-side
   read bug (and cost real time to isolate -- confirmed via a throwaway
   diagnostic shader that read the SRV directly, after first ruling out the
   UAV-write path and the cbuffer read path independently). The fix is
   ordering: `AcquireSync(0, ...)` **before** the write, `ReleaseSync(1)`
   after -- exactly the producer-side protocol `frame_protocol.hpp` already
   documents, and exactly what `gl_dx_interop_capture.cpp`'s real capture
   paths already do correctly (this bug was confined to the test's synthetic
   frame builder, not the production code). Worth remembering for any future
   code that touches a keyed-mutex resource directly: acquire first, always.
6. **`obs-config.h` (from `obs.h`) `#include`s `"obsconfig.h"`, a file CMake
   `configure_file()`s into libobs's *build* directory, not anywhere in its
   source tree.** Building `obs-gmix-source` against a from-source OBS
   checkout (rather than a packaged SDK that already bundles this) failed
   with `Cannot open include file: 'obsconfig.h'` until the CMakeLists
   gained an `OBS_CONFIG_INCLUDE_DIR` option pointing at `<obs build
   dir>/config`.
7. **`gmix_source.cpp` failed with cryptic `error C2589`/`C2059` syntax
   errors around every `std::min(...)` call** once actually compiled with
   `<windows.h>` in the include graph (via the real `obs.h`, which pulls it
   in) -- `windows.h`'s `min`/`max` function-like macros textually replace
   any bare `min(`/`max(` token sequence, including `std::min(`, turning it
   into nonsense. Fixed with `#define NOMINMAX` before `#include <windows.h>`
   in that file specifically (the only WIN32 source that calls `std::min`
   literally in a translation unit that also includes `windows.h`; others
   use `std::clamp`, which isn't textually affected, or don't include
   `windows.h` at all).
8. **`obs-gmix-source`'s `.dll` silently landed in the build root instead of
   `obs_plugin/bin/64bit/`** despite `RUNTIME_OUTPUT_DIRECTORY` being set.
   Root cause: CMake `MODULE` library targets (unlike `SHARED`) use
   `LIBRARY_OUTPUT_DIRECTORY` for their `.dll` on Windows too, not the
   `RUNTIME_OUTPUT_DIRECTORY`/`ARCHIVE_OUTPUT_DIRECTORY` split `SHARED`
   targets get. Fixed by setting both properties to the same path.

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
- `tests/test_ipc.cpp`, `tests/test_blend_engine.cpp` — named-pipe round-trip
  + backlog-drop test, and a real-GPU compute-blend correctness test (both
  passing -- see "Status" above).
- `data/locale/en-US.ini`, `setup.bat` — the OBS plugin's locale data file
  (mirrors `linux-x86_64/data/locale/en-US.ini`), and a self-elevating
  installer that removes any old GMix plugin(s) and installs a fresh build
  into a real OBS install (see "obs-gmix-source runs correctly inside a real
  OBS process" above for why this exists).

## What's NOT built

- **osu!lazer / native D3D11 producer (phase 2).** Not designed in detail;
  see the top-level plan this port followed. Would add a second proxy DLL
  (`dxgi.dll` or `d3d11.dll`) hooking `IDXGISwapChain::Present`, feeding the
  SAME ring/blend/OBS pipeline with no interop hop.
- **The debug `--attach` IPC client** the Linux `gmix` CLI has (`ipc_client.cpp`)
  -- out of scope for this pass; `gmix.exe` here only does `--install-proxy`/
  `--list-gpus`.
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

5. **`WGL_NV_DX_interop2` is NOT actually NVIDIA-exclusive, despite its name
   and general web-research consensus** -- confirmed genuinely functional on
   a current AMD RX 480 driver via a direct probe (see the interop-mechanism
   history section above). Don't take vendor-reputation or an extension
   string's presence as gospel; test the real function calls. The
   CPU-readback fallback (`captureViaReadback`) still exists for whatever
   driver genuinely lacks it (`wglDXOpenDeviceNV` returning null is the
   actual, tested gate -- see `../README.md`'s platform-compatibility note).

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
first-scene-only limitation. Verified the single-instance-per-process
lifecycle (create/destroy/recreate cycle via repeatedly removing and
re-adding the source) works correctly, including the shutdown-deadlock fix
above -- **not yet specifically verified with two DIFFERENT scenes each
holding their own instance of the source simultaneously**, only repeated
add/remove of one at a time.

## Likely next directions (not yet built)

- osu!lazer / D3D11-native producer (phase 2 -- see `../README.md`).
- Multi-adapter retry in the proxy's device creation + a way for the OBS
  plugin to learn and pin to the game's adapter LUID (see note 4 above).
- `test_blend_engine`/`test_ipc`-equivalent automated tests once a build
  environment is available to iterate against.
- The debug `--attach` IPC client (`gmix.exe`), if it turns out to be useful
  for diagnosing a stuck producer/consumer connection in the field.
