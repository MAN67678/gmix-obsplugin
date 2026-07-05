// ─────────────────────────────────────────────────────────────────────────────
// GMix (Windows) — gmix-inject.exe: loads the capture DLL into a running
// target process via classic CreateRemoteThread(LoadLibraryW) injection.
//
// Why runtime injection instead of the original "shadow opengl32.dll next to
// the game's exe" design: confirmed against real osu!stable that (a) its
// .NET/OpenTK renderer calls `gdi32!SwapBuffers`, not `wglSwapBuffers` (the
// documented, standard way OpenGL double-buffering is done on Windows --
// not an osu!-specific quirk), so shadowing opengl32.dll never intercepted
// anything, and (b) even if it did, .NET P/Invoke calls are resolved
// dynamically (no static import-table slot), so injection + inline-hooking
// the real exported function is the only mechanism that actually works
// regardless of caller. See gl_dx_interop_capture.cpp's header comment and
// WIN32/etc/DEV_NOTES.md for the full writeup.
//
// Usage: gmix-inject.exe [process-name] [dll-path]
//   process-name defaults to "osu!.exe". dll-path defaults to
//   "GmixCapture.dll" next to this exe. LoadLibraryW is idempotent for an
//   already-loaded path (returns the existing module), so re-running this
//   against an already-injected process is harmless.
// ─────────────────────────────────────────────────────────────────────────────
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

DWORD findPid(const std::wstring& exeName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool injectInto(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) {
        std::fprintf(stderr, "gmix-inject: OpenProcess(%lu) failed (%lu) -- run as the same user, "
                              "or elevated if the target is elevated\n", pid, GetLastError());
        return false;
    }

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::fprintf(stderr, "gmix-inject: VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        std::fprintf(stderr, "gmix-inject: WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    auto loadLibraryW = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryW), remote, 0, nullptr);
    if (!thread) {
        std::fprintf(stderr, "gmix-inject: CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, INFINITE);
    DWORD remoteModule = 0;
    GetExitCodeThread(thread, &remoteModule);   // LoadLibraryW's return value (low 32 bits of the HMODULE)
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (remoteModule == 0) {
        std::fprintf(stderr, "gmix-inject: LoadLibraryW returned NULL in the target process -- "
                              "check the target's architecture matches this DLL (32-bit vs 64-bit)\n");
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring target = (argc > 1) ? argv[1] : L"osu!.exe";

    std::filesystem::path dll;
    if (argc > 2) {
        dll = argv[2];
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dll = std::filesystem::path(self).parent_path() / L"GmixCapture.dll";
    }
    if (!std::filesystem::exists(dll)) {
        std::fwprintf(stderr, L"gmix-inject: DLL not found: %ls\n", dll.c_str());
        return 1;
    }

    DWORD pid = findPid(target);
    if (!pid) {
        std::fwprintf(stderr, L"gmix-inject: process '%ls' not running\n", target.c_str());
        return 1;
    }

    std::wprintf(L"gmix-inject: injecting %ls into pid %lu (%ls)\n", dll.c_str(), pid, target.c_str());
    if (!injectInto(pid, dll.wstring())) return 1;
    std::wprintf(L"gmix-inject: ok. Add a \"GMix Motion Blur\" source in OBS if you haven't already.\n");
    return 0;
}
