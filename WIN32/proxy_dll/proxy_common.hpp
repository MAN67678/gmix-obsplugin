// ─────────────────────────────────────────────────────────────────────────────
// GMix proxy opengl32.dll — shared bits between opengl32_proxy.cpp (the
// DllMain / export surface) and gl_dx_interop_capture.cpp (the actual
// capture logic), so the capture code never has to know how the real driver
// DLL was located.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace gmix::proxy {

// Resolves a GL/WGL function pointer against the REAL driver, never our own
// proxy. Tries the real wglGetProcAddress first (required for anything past
// GL1.1/core WGL -- FBOs, WGL_NV_DX_interop2, etc.), then falls back to a
// plain GetProcAddress on the real opengl32.dll module for core exports
// wglGetProcAddress isn't obligated to return (per the WGL spec, it only
// covers extension entry points once a context is current).
void* resolveGlProc(const char* name);

// Real wglSwapBuffers, resolved once at load time -- what our own exported
// wglSwapBuffers calls through to after the capture hook runs.
using PFN_wglSwapBuffers = int(__stdcall*)(void* hdc);
PFN_wglSwapBuffers realSwapBuffers();

// The current process's id, cached once -- used to name this producer's
// shared ring textures (see ipc/frame_protocol.hpp's sharedTextureName()).
unsigned long currentProducerPid();

} // namespace gmix::proxy
