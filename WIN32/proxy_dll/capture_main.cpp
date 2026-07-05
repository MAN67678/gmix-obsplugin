// ─────────────────────────────────────────────────────────────────────────────
// GMix capture DLL (Windows) — GmixCapture.dll: the injection-free-DESIGN
// approach didn't work (see below), so this is now loaded via genuine
// runtime injection (gmix-inject.exe, CreateRemoteThread+LoadLibraryW).
//
// HISTORY: the original design shadowed opengl32.dll (dropped next to the
// game's exe, relying on DLL search order) and hooked its wglSwapBuffers
// export. Confirmed against real osu!stable that this never fires at all:
// its .NET/OpenTK renderer calls `gdi32!SwapBuffers`, not `wglSwapBuffers`
// -- this is the documented, standard way OpenGL double-buffering is done
// on Windows (not an osu!-specific quirk), so most OpenGL apps' hot path
// never touches wglSwapBuffers regardless of shadowing. Even if it had
// called the right function, .NET P/Invoke resolves native calls dynamically
// (no static PE import-table slot), so IAT-style patching would also have
// caught nothing. The only mechanism that works regardless of caller is
// injecting into the already-running process and inline-hooking the REAL
// exported function's machine code (see inline_hook.hpp) -- this is what
// this file does, on both `gdi32!SwapBuffers` (osu!stable's actual call)
// and `opengl32!wglSwapBuffers` (kept as a secondary hook for any app that
// does call it directly). See WIN32/etc/DEV_NOTES.md for the full writeup.
// ─────────────────────────────────────────────────────────────────────────────
#include "proxy_common.hpp"
#include "inline_hook.hpp"
#include "gl_dx_interop_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <thread>

namespace {

unsigned long g_pid = 0;

using PFN_wglGetProcAddress = void* (__stdcall*)(const char*);
PFN_wglGetProcAddress g_realWglGetProcAddress = nullptr;
HMODULE g_realOpenGL32 = nullptr;

using PFN_SwapBuffers = BOOL(__stdcall*)(HDC);
void* g_trampolineGdiSwapBuffers = nullptr;
void* g_trampolineWglSwapBuffers = nullptr;

} // namespace

namespace gmix::proxy {

void* resolveGlProc(const char* name) {
    if (g_realWglGetProcAddress) {
        if (void* p = g_realWglGetProcAddress(name)) return p;
    }
    if (g_realOpenGL32) {
        return reinterpret_cast<void*>(GetProcAddress(g_realOpenGL32, name));
    }
    return nullptr;
}

unsigned long currentProducerPid() { return g_pid; }

void debugLog(const char* fmt, ...) {
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring path = std::wstring(tempDir) + L"gmix_proxy_debug.log";
    FILE* f = _wfopen(path.c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "%02u:%02u:%02u.%03u [pid %lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_pid);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fprintf(f, "\n");
    std::fclose(f);
}

} // namespace gmix::proxy

namespace {

// ── hook detours ────────────────────────────────────────────────────────────
BOOL __stdcall hookGdiSwapBuffers(HDC hdc) {
    gmix::capture::GlDxInteropCapture::instance().onSwapBuffers(hdc);
    auto real = reinterpret_cast<PFN_SwapBuffers>(g_trampolineGdiSwapBuffers);
    return real ? real(hdc) : TRUE;
}

BOOL __stdcall hookWglSwapBuffers(HDC hdc) {
    gmix::capture::GlDxInteropCapture::instance().onSwapBuffers(hdc);
    auto real = reinterpret_cast<PFN_SwapBuffers>(g_trampolineWglSwapBuffers);
    return real ? real(hdc) : TRUE;
}

// Installs both hooks once their target modules are loaded. Returns true once
// the primary target (gdi32!SwapBuffers -- what osu!stable actually calls)
// is hooked; opengl32!wglSwapBuffers is best-effort/secondary.
bool tryInstallHooks() {
    bool any = false;
    if (!g_trampolineGdiSwapBuffers) {
        if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll")) {
            void* target = reinterpret_cast<void*>(GetProcAddress(gdi, "SwapBuffers"));
            if (gmix::hook::installInlineHook("gdi32!SwapBuffers", target,
                                              reinterpret_cast<void*>(hookGdiSwapBuffers),
                                              &g_trampolineGdiSwapBuffers)) {
                any = true;
            }
        }
    } else {
        any = true;
    }
    if (!g_trampolineWglSwapBuffers) {
        if (HMODULE gl = GetModuleHandleW(L"opengl32.dll")) {
            g_realOpenGL32 = gl;
            g_realWglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
                GetProcAddress(gl, "wglGetProcAddress"));
            void* target = reinterpret_cast<void*>(GetProcAddress(gl, "wglSwapBuffers"));
            gmix::hook::installInlineHook("opengl32!wglSwapBuffers", target,
                                          reinterpret_cast<void*>(hookWglSwapBuffers),
                                          &g_trampolineWglSwapBuffers);
        }
    }
    return any;
}

void initThread() {
    gmix::proxy::debugLog("capture_main: init thread started, waiting for gdi32/opengl32 to load...");
    bool ok = false;
    for (int i = 0; i < 200 && !ok; ++i) {   // up to ~20s for the game to reach its render setup
        ok = tryInstallHooks();
        if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ok) {
        gmix::proxy::debugLog("capture_main: gdi32!SwapBuffers hook install FAILED after retrying -- "
                              "capture will not activate");
    } else {
        gmix::proxy::debugLog("capture_main: hook(s) installed successfully");
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hinst, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_pid = GetCurrentProcessId();
        DisableThreadLibraryCalls(hinst);
        gmix::proxy::debugLog("capture_main: DLL_PROCESS_ATTACH");
        std::thread(initThread).detach();
        break;
    case DLL_PROCESS_DETACH:
        // No live unhook: the inline-patched machine code and trampolines
        // are left in place. Safe because DLL_PROCESS_DETACH for this DLL
        // only happens at process exit (nothing ever calls FreeLibrary on
        // it) -- the whole process address space is torn down immediately
        // after, taking the patched bytes with it. Restoring original bytes
        // here would race whatever thread might still be mid-call through
        // them during shutdown, for no benefit.
        gmix::capture::GlDxInteropCapture::instance().shutdown();
        break;
    default:
        break;
    }
    return TRUE;
}
