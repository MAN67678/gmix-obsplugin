// ─────────────────────────────────────────────────────────────────────────────
// GMix — weight handling for the blend.
//
// The default blend is a FLAT average (uniform 1/N) of the N real frames that
// fall in the 1/60s window -- exactly what ffmpeg's tblend / a box shutter does
// with real captured frames. The only customization is a raw power-user curve
// ("-weight=1 2 1"), which is tiled to the live frame count N and normalized,
// following ffmpeg tmix's semantics:
//   output_pixel = Σ (wᵢ · frameᵢ) / Σ (wᵢ)
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
