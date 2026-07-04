// ─────────────────────────────────────────────────────────────────────────────
// GMix (Windows) — weight handling for the blend.
//
// Pure math, no platform dependency -- kept in sync verbatim with
// linux-x86_64/src/blend/weight_generator.hpp. See that file's header comment
// for the full rationale (ffmpeg tmix semantics).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gmix {

// Parse "-weight=1 2 1" into a raw float vector (length W = whatever was given).
// Throws std::invalid_argument on malformed/empty input.
std::vector<float> parseWeightString(std::string_view s);

// tmix normalize: divide every weight by Σw. Empty input → {1.0}.
// Σw == 0 (degenerate, all-zero) → returns uniform 1/N to avoid div-by-zero.
std::vector<float> normalizeWeights(const std::vector<float>& w);

// Extend `curve` to length N by HOLDING the last weight for the remaining slots
// (ffmpeg tmix semantics): "-weight=1 2 3 4" with N=10 → [1,2,3,4,4,4,4,4,4,4].
// Empty input throws std::invalid_argument.
std::vector<float> tileCurve(const std::vector<float>& curve, int N);

// Raw-weight pipeline: tileCurve(curve, N) → normalize. Length N, Σ ≈ 1.0.
std::vector<float> tileAndNormalize(const std::vector<float>& curve, int N);

} // namespace gmix
