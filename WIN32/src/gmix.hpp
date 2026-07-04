// ─────────────────────────────────────────────────────────────────────────────
// GMix (Windows) — shared forward declarations & common types.
//
// Kept identical to linux-x86_64/src/gmix.hpp -- these constants are pure and
// platform-independent (the shutter/blend design doesn't change per-OS), so
// this is a straight copy, not a divergent port. If you change kMaxBlendFrames
// here, change shaders/blend.hlsl's MAX_FRAMES to match, same as the Linux
// side keeps blend.comp in sync.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>

namespace gmix {

// App-wide constants.
inline constexpr uint32_t kDefaultOutputFps = 60;
inline constexpr int      kMaxBlendFrames   = 64;   // must match MAX_FRAMES in
                                                    // shaders/blend.hlsl
                                                    // (64 frames in a 16.6ms shutter = up to ~3800fps)
inline constexpr int      kRingCapacity     = 8;    // timestamped frame ring depth

} // namespace gmix
