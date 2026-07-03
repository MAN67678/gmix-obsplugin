// ─────────────────────────────────────────────────────────────────────────────
// GMix — shared forward declarations & common types.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>

namespace gmix {

// App-wide constants.
inline constexpr uint32_t kDefaultOutputFps = 60;
inline constexpr int      kMaxBlendFrames   = 64;   // must match MAX_FRAMES in
                                                    // shaders/resample_blur.comp
                                                    // (64 frames in a 16.6ms shutter = up to ~3800fps)
inline constexpr int      kRingCapacity     = 8;    // timestamped frame ring depth

} // namespace gmix
