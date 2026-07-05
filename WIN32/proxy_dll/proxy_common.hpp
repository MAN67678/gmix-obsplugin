// ─────────────────────────────────────────────────────────────────────────────
// GMix capture DLL (Windows) — shared bits between the DllMain/hook-install
// entry point and gl_dx_interop_capture.cpp (the actual capture logic).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace gmix::proxy {

// Resolves a GL/WGL function pointer against the real driver's ALREADY-
// LOADED opengl32.dll (found via GetModuleHandleW -- this DLL is injected at
// runtime, not shadowing anything, so there is only ever one real
// opengl32.dll in the target process). Tries wglGetProcAddress first
// (required for anything past GL1.1/core WGL -- FBOs, GL_EXT_memory_object_
// win32, etc.), then falls back to a plain GetProcAddress on that module for
// core exports wglGetProcAddress isn't obligated to return.
void* resolveGlProc(const char* name);

// The current process's id, cached once.
unsigned long currentProducerPid();

// Appends one timestamped line to %TEMP%\gmix_proxy_debug.log. This DLL runs
// inside an arbitrary GUI process (no console), so this is the only way to
// see what it's doing.
void debugLog(const char* fmt, ...);

} // namespace gmix::proxy
