// ─────────────────────────────────────────────────────────────────────────────
// GMix (Windows) — minimal x86 inline (trampoline) hooking, no third-party
// hook library. Needed because osu!stable is .NET/OpenTK and P/Invokes
// `gdi32!SwapBuffers` directly against the exported function address (resolved
// once by the CLR's own internal interop marshaling, not through a classic PE
// import table) -- confirmed empirically: our first approach (shadowing
// opengl32.dll via DLL search order + hooking its wglSwapBuffers export) never
// fired at all, because osu! never calls wglSwapBuffers in the first place,
// and IAT patching has no static import-table slot to patch for a P/Invoke
// call either. The only technique that intercepts a call resolved this way is
// patching the REAL exported function's own machine code with a jump to our
// detour, wherever the caller (managed or native) ends up jumping.
//
// x86-only (osu!stable is a 32-bit/WOW64 process). Adapted from a working
// prior prototype (see WIN32/etc/DEV_NOTES.md).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

namespace gmix::hook {

// Installs a 5-byte jmp-rel32 inline hook on `target`, redirecting it to
// `detour`. Builds a small trampoline (the displaced original instructions +
// a jmp back into `target` past the patched bytes) and returns it via
// `trampolineOut` -- call through the trampoline (cast to the original
// function's type) to invoke the real, un-hooked behavior.
//
// Fail-safe: returns false and modifies nothing if it can't cleanly decode
// and displace >= 5 whole bytes of instructions, if the prologue is already a
// jmp (something else got there first), or if a displaced instruction is
// position-relative (a call/jmp/jcc whose target is computed relative to its
// own address -- relocating it naively into the trampoline would corrupt the
// target). Never risk corrupting the target function's code.
bool installInlineHook(const char* debugName, void* target, void* detour, void** trampolineOut);

} // namespace gmix::hook
