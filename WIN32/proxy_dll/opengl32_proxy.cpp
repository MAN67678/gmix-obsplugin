// ─────────────────────────────────────────────────────────────────────────────
// GMix proxy opengl32.dll — the injection-free capture entry point.
//
// Dropped next to osu!.exe (alongside a renamed copy of the real driver DLL,
// "opengl32_orig.dll" -- see generate_def.ps1's header comment for why), this
// DLL shadows the system opengl32.dll via ordinary Windows DLL search order:
// osu! links against "opengl32.dll" by name, the loader finds this one first
// (same directory as the exe), and every export the game actually calls
// either forwards straight through to opengl32_orig.dll (the vast majority --
// see the generated .def) or is one of the small set we implement ourselves
// below to hook capture. This mirrors the "drop a file, implicit activation,
// zero injector/CreateRemoteThread" philosophy of the Linux side's
// VkLayer_GMIX.json implicit Vulkan layer.
//
// Only wglSwapBuffers is intercepted -- everything else, including
// wglCreateContext/wglMakeCurrent, passes straight through via the .def
// forward so the game's own GL setup is completely unaffected.
// ─────────────────────────────────────────────────────────────────────────────
#include "proxy_common.hpp"
#include "gl_dx_interop_capture.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

HMODULE g_realOpenGL32 = nullptr;
using PFN_wglGetProcAddress = void* (__stdcall*)(const char*);
PFN_wglGetProcAddress g_realWglGetProcAddress = nullptr;
gmix::proxy::PFN_wglSwapBuffers g_realWglSwapBuffers = nullptr;
unsigned long g_pid = 0;

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

PFN_wglSwapBuffers realSwapBuffers() { return g_realWglSwapBuffers; }

unsigned long currentProducerPid() { return g_pid; }

} // namespace gmix::proxy

// ── Our one hooked export ───────────────────────────────────────────────────
extern "C" int __stdcall wglSwapBuffers(HDC hdc) {
    gmix::capture::GlDxInteropCapture::instance().onSwapBuffers(hdc);
    return g_realWglSwapBuffers ? g_realWglSwapBuffers(hdc) : 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        g_pid = GetCurrentProcessId();

        // Explicit full path, NEVER a bare "opengl32_orig.dll" or
        // "opengl32.dll" -- a bare name search from inside this DLL would
        // check the game's own directory first (same rule that let THIS
        // DLL shadow the system one) and could resolve right back to
        // ourselves. opengl32_orig.dll is expected to sit next to us (same
        // directory as this proxy DLL), placed there by
        // `gmix.exe --install-proxy` -- resolve it via our OWN module path,
        // not the process's directory, in case osu! is ever launched with a
        // different working directory.
        wchar_t selfPath[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &self);
        GetModuleFileNameW(self, selfPath, MAX_PATH);
        std::wstring dir(selfPath);
        auto slash = dir.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
        std::wstring origPath = dir + L"\\opengl32_orig.dll";

        g_realOpenGL32 = LoadLibraryW(origPath.c_str());
        if (!g_realOpenGL32) {
            // Not installed correctly -- fall back to the real system copy
            // directly so the game still renders (capture simply won't
            // activate; ensureInit() in GlDxInteropCapture also no-ops
            // safely if wglGetProcAddress/interop setup can't proceed).
            wchar_t sysDir[MAX_PATH]{};
            GetSystemDirectoryW(sysDir, MAX_PATH);
            std::wstring sysPath = std::wstring(sysDir) + L"\\opengl32.dll";
            g_realOpenGL32 = LoadLibraryW(sysPath.c_str());
        }
        if (g_realOpenGL32) {
            g_realWglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
                GetProcAddress(g_realOpenGL32, "wglGetProcAddress"));
            g_realWglSwapBuffers = reinterpret_cast<gmix::proxy::PFN_wglSwapBuffers>(
                GetProcAddress(g_realOpenGL32, "wglSwapBuffers"));
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        gmix::capture::GlDxInteropCapture::instance().shutdown();
        break;
    default:
        break;
    }
    return TRUE;
}
