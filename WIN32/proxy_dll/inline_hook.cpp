#include "inline_hook.hpp"
#include "proxy_common.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace gmix::hook {

namespace {

// Length of the x86 ModR/M operand (ModR/M byte + optional SIB + displacement).
int modrmLen(const uint8_t* p) {
    uint8_t modrm = p[0];
    int len = 1;
    uint8_t mod = modrm >> 6, rm = modrm & 7;
    if (mod != 3 && rm == 4) {                       // SIB byte present
        uint8_t sib = p[1];
        len += 1;
        if (mod == 0 && (sib & 7) == 5) len += 4;    // disp32 with no base
    }
    if (mod == 0) { if (rm == 5) len += 4; }         // [disp32]
    else if (mod == 1) len += 1;                     // disp8
    else if (mod == 2) len += 4;                     // disp32
    return len;
}

// Minimal, FAIL-SAFE x86 instruction-length decoder covering the instruction
// set typically seen in Windows API prologues. Returns 0 for anything it
// doesn't recognize -- the caller then refuses to hook rather than guess.
int x86InsnLen(const uint8_t* p) {
    int n = 0;
    for (;;) {   // legacy/segment/operand-size prefixes
        uint8_t b = p[n];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            ++n;
            continue;
        }
        break;
    }
    uint8_t op = p[n++];
    if (op >= 0x50 && op <= 0x5F) return n;                 // push/pop r32
    if (op >= 0xB8 && op <= 0xBF) return n + 4;             // mov r32, imm32
    switch (op) {
        case 0x90: case 0xC3: case 0xCC: case 0xC9: return n;  // nop/ret/int3/leave
        case 0x6A: return n + 1;                                // push imm8
        case 0x68: return n + 4;                                // push imm32
        case 0xEB: return n + 1;                                // jmp rel8
        case 0xE9: case 0xE8: return n + 4;                     // jmp/call rel32
        case 0xC2: return n + 2;                                // ret imm16
        case 0x88: case 0x8A: case 0x89: case 0x8B:             // mov r/m<->r
        case 0x84: case 0x85: case 0x8D:                        // test, lea
        case 0x01: case 0x03: case 0x29: case 0x2B:
        case 0x31: case 0x33: case 0x39: case 0x3B:
            return n + modrmLen(p + n);
        case 0x83: return n + modrmLen(p + n) + 1;              // grp1 r/m, imm8
        case 0x81: return n + modrmLen(p + n) + 4;              // grp1 r/m, imm32
        case 0xFF: return n + modrmLen(p + n);                  // grp5
        case 0x0F: {                                            // two-byte opcode
            uint8_t op2 = p[n++];
            if (op2 == 0x1F) return n + modrmLen(p + n);        // multi-byte nop
            if ((op2 & 0xF0) == 0x80) return n + 4;             // jcc rel32
            if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF || op2 == 0xAF)
                return n + modrmLen(p + n);                     // movzx/movsx/imul
            return 0;                                           // unknown 2-byte
        }
        default: return 0;                                      // unknown -> fail safe
    }
}

} // namespace

bool installInlineHook(const char* debugName, void* target, void* detour, void** trampolineOut) {
    if (!target) {
        gmix::proxy::debugLog("inline hook %s: target is null", debugName);
        return false;
    }
    uint8_t* t = reinterpret_cast<uint8_t*>(target);
    if (t[0] == 0xE9 || t[0] == 0xEB) {
        gmix::proxy::debugLog("inline hook %s: prologue is already a jmp (hooked by someone else?) -- skipping",
                              debugName);
        return false;
    }

    int stolen = 0;
    while (stolen < 5) {
        uint8_t op = t[stolen];
        if (op == 0xE8 || op == 0xE9 || op == 0xEB || (op == 0x0F && (t[stolen + 1] & 0xF0) == 0x80)) {
            gmix::proxy::debugLog("inline hook %s: relative branch in prologue (+%d) -- aborting",
                                  debugName, stolen);
            return false;
        }
        int len = x86InsnLen(t + stolen);
        if (len == 0) {
            gmix::proxy::debugLog("inline hook %s: undecodable byte 0x%02X at +%d -- aborting",
                                  debugName, t[stolen], stolen);
            return false;
        }
        stolen += len;
    }

    uint8_t* tramp = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        gmix::proxy::debugLog("inline hook %s: VirtualAlloc failed", debugName);
        return false;
    }
    std::memcpy(tramp, t, static_cast<size_t>(stolen));   // displaced original bytes
    tramp[stolen] = 0xE9;                                  // jmp back to target+stolen
    *reinterpret_cast<int32_t*>(tramp + stolen + 1) =
        static_cast<int32_t>((t + stolen) - (tramp + stolen + 5));

    DWORD oldProt = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        gmix::proxy::debugLog("inline hook %s: VirtualProtect failed (%lu)", debugName, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    t[0] = 0xE9;   // jmp detour
    *reinterpret_cast<int32_t*>(t + 1) =
        static_cast<int32_t>(reinterpret_cast<uint8_t*>(detour) - (t + 5));
    VirtualProtect(t, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), t, 5);

    *trampolineOut = tramp;
    gmix::proxy::debugLog("inline hook %s: hooked @%p (%d bytes displaced), trampoline=%p",
                          debugName, target, stolen, static_cast<void*>(tramp));
    return true;
}

} // namespace gmix::hook
