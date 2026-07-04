#include "weight_generator.hpp"
#include "../gmix.hpp"   // kMaxBlendFrames

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace gmix {

// Maximum frames the engine will ever blend in one dispatch. Must match
// MAX_FRAMES in shaders/blend.hlsl (== kMaxBlendFrames).
static constexpr int kMaxFrames = kMaxBlendFrames;

// ─── weight-string parsing ───────────────────────────────────────────────────
std::vector<float> parseWeightString(std::string_view s) {
    std::vector<float> out;
    std::istringstream iss{std::string(s)};
    float v;
    while (iss >> v) {
        out.push_back(v);
        if (out.size() > static_cast<size_t>(kMaxFrames)) {
            throw std::invalid_argument(
                "weight array exceeds MAX_FRAMES (" + std::to_string(kMaxFrames) + ")");
        }
    }
    // partial-parse failure: e.g. "1 abc" leaves iss in fail state with a stray token
    if (!iss.eof() && iss.fail()) {
        throw std::invalid_argument("malformed weight string: " + std::string(s));
    }
    if (out.empty()) {
        throw std::invalid_argument("empty weight string");
    }
    return out;
}

// ─── tmix normalization ──────────────────────────────────────────────────────
std::vector<float> normalizeWeights(const std::vector<float>& w) {
    if (w.empty()) return {1.0f};

    double sum = 0.0;
    for (float x : w) sum += x;

    std::vector<float> out = w;
    if (std::abs(sum) < 1e-12) {
        // Degenerate: all-zero weights. Fall back to uniform to avoid div-by-zero
        // and a black output.
        const float u = 1.0f / static_cast<float>(w.size());
        std::fill(out.begin(), out.end(), u);
        return out;
    }

    const float inv = static_cast<float>(1.0 / sum);
    for (auto& x : out) x *= inv;
    return out;
}

// ─── tiling (hold last weight, not stretch) ─────────────────────────────────
// ffmpeg tmix semantics: "if the number of weights is smaller than the number
// of frames, the last specified weight is used for all remaining unset weights"
// -- "-weight=1 2 3 4" with N=10 frames gives [1,2,3,4,4,4,4,4,4,4].
std::vector<float> tileCurve(const std::vector<float>& curve, int N) {
    if (curve.empty()) throw std::invalid_argument("cannot tile empty curve");
    if (N <= 0)        throw std::invalid_argument("N must be >= 1");

    const int W = static_cast<int>(curve.size());
    std::vector<float> out;
    out.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) out.push_back(curve[std::min(i, W - 1)]);
    return out;
}

std::vector<float> tileAndNormalize(const std::vector<float>& curve, int N) {
    auto r = tileCurve(curve, N);
    return normalizeWeights(r);
}

} // namespace gmix
