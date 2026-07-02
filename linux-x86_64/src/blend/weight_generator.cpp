#include "weight_generator.hpp"
#include "../gmix.hpp"   // kMaxBlendFrames

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>

namespace gmix {

// Maximum frames the engine will ever blend in one dispatch. Must match
// MAX_FRAMES in shaders/blend.comp (== kMaxBlendFrames).
static constexpr int kMaxFrames = kMaxBlendFrames;

// ─── preset shape curves ─────────────────────────────────────────────────────
// Each returns N samples in [0,1]-ish (shape only, not normalized).
std::vector<float> generatePresetCurve(PresetShape shape, int N) {
    if (N <= 0) throw std::invalid_argument("N must be >= 1");

    std::vector<float> w;
    w.reserve(static_cast<size_t>(N));

    const auto mid = [](int n) { return (n - 1) * 0.5; };

    // danser-go computes its weights as `1.0 + shape(t)*100`, NOT a bare shape
    // function -- the +1.0 floor means every blended frame keeps a meaningful
    // relative contribution (edge:center ratio ~1:9 at its default gauss
    // multiplier of 1.5) instead of decaying toward zero at the window edges.
    // A bare gaussian/triangle/exponential effectively only blends the few
    // frames nearest the peak once N is more than ~6-8, which reads as almost
    // no blur at all even though weightsFor() is being fed 10+ frames.
    constexpr double kFloor = 1.0;
    constexpr double kScale = 100.0;

    switch (shape) {
    case PresetShape::Linear: {
        // symmetric triangle peaking at the center frame.
        const double m = mid(N);
        for (int i = 0; i < N; ++i) {
            const double s = 1.0 - std::abs(i - m) / (m + 0.5);
            w.push_back(static_cast<float>(kFloor + s * kScale));
        }
        break;
    }

    case PresetShape::Cinematic: {
        // symmetric gaussian (danser's gaussSymmetric default: mult=1.5).
        const double mult = 1.5;
        for (int i = 0; i < N; ++i) {
            const double t = (N > 1) ? static_cast<double>(i) / (N - 1) : 0.5;
            const double s = std::exp(-std::pow(mult * (t * 2.0 - 1.0), 2));
            w.push_back(static_cast<float>(kFloor + s * kScale));
        }
        break;
    }

    case PresetShape::Heavy: {
        // one-sided exponential decay from frame 0 (newest) toward older
        // frames, producing a long visible trailing ghost.
        const double tau = std::max(1.0, N / 4.0);
        for (int i = 0; i < N; ++i) {
            const double s = std::exp(-i / tau);
            w.push_back(static_cast<float>(kFloor + s * kScale));
        }
        break;
    }
    }

    // Clamp tiny negatives from float error to zero (Linear edges can go -ε).
    for (auto& x : w) x = std::max(0.0f, x);
    return w;
}

std::vector<float> generateFromPreset(PresetShape shape, int N) {
    auto c = generatePresetCurve(shape, N);
    return normalizeWeights(c);
}

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
