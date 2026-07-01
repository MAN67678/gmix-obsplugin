// ─────────────────────────────────────────────────────────────────────────────
// GMix — blend configuration: the blend is a FLAT average of the real frames in
// the 1/60s window, or an optional raw power-user weight curve. Plus the output
// dimensions / pacing. The CLI populates this; everything else consumes it.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "weight_generator.hpp"
#include "../gmix.hpp"   // kMaxBlendFrames
#include <vector>
#include <cstdint>

namespace gmix {

struct BlendConfig {
    // Flat = uniform 1/N average of the N source frames (the default; what
    // ffmpeg tblend / a box shutter does with real frames). Raw = a custom
    // power-user weight curve from -weight, tiled to N and normalized.
    enum class Mode { Flat, Raw } mode = Mode::Flat;

    // Used only when mode == Raw. Tiled to the actual N per dispatch, normalized.
    std::vector<float> rawWeights;

    // Output window size.
    uint32_t outW = 1920;
    uint32_t outH = 1080;

    // Target output framerate. 60 is the only sane value per the design doc
    // (platform ingest constraint), but kept configurable for testing. The
    // number of source frames blended per output frame is derived from the live
    // capture rate (n = round(captureFps / outFps)) in the present loop, so the
    // blur window is always ~1/outFps of real motion -- a 360-degree shutter.
    uint32_t outFps = 60;

    // Present-pacing trade-off (numeric so the .ini can't be misspelled):
    //   1 = low latency, 2 = steady output. Both pace to vblank via present_wait.
    int latencyMode = 1;

    // GPU override (index into enumerated physical devices). -1 = auto.
    int32_t gpuIndex = -1;

    // Max frames the engine will blend in one dispatch (the shader's MAX_FRAMES).
    int maxBlendFrames() const { return kMaxBlendFrames; }

    // Normalized weight vector for N actual source frames.
    //   N <= 1   -> {1.0} (passthrough; nothing to blend)
    //   Flat     -> uniform 1/N
    //   Raw      -> tileAndNormalize(rawWeights, N)  (ffmpeg tmix tiling)
    std::vector<float> weightsFor(int N) const {
        if (N <= 1) return {1.0f};
        if (mode == Mode::Raw && !rawWeights.empty())
            return tileAndNormalize(rawWeights, N);
        return std::vector<float>(static_cast<size_t>(N), 1.0f / static_cast<float>(N));
    }
};

} // namespace gmix
