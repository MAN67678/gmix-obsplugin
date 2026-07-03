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
    // Flat      = uniform 1/N average of the N source frames (the default;
    //             what ffmpeg tblend / a box shutter does with real frames).
    // Linear/Cinematic/Heavy = shaped curves (see weight_generator's
    //             PresetShape) -- same plain-blend shader (blend.comp), just
    //             a different weight distribution across the window.
    // Advanced  = velocity-aware ("optical awareness") motion blur: routes
    //             the dispatch to resample_blur.comp instead of a weighted
    //             average of blend.comp. See blurDensity/shutterStrength/
    //             falloff below and BlendEngine::ResampleParams.
    // Raw       = a custom power-user weight curve from -weight, tiled to N
    //             and normalized.
    enum class Mode { Flat, Linear, Cinematic, Heavy, Advanced, Raw } mode = Mode::Flat;

    // Used only when mode == Raw. Tiled to the actual N per dispatch, normalized.
    std::vector<float> rawWeights;

    // ── Advanced (optical-flow) params, used only when mode == Advanced ─────
    // "Blur density" in the OBS properties UI: taps per real frame along the
    // estimated motion direction. 4..32 -- higher packs the directional
    // streak denser at proportionally higher GPU cost. Matches
    // BlendEngine::ResampleParams::subSamples.
    uint32_t blurDensity     = 4;
    // "Blur brightness" in the OBS properties UI: a motion-gated exposure
    // boost on the trail (resample_blur.comp's `shutterStrength` -- see that
    // file for the current mechanism, reworked multiple times on
    // 2026-07-03; this comment described an earlier exponential-dominance
    // scheme that no longer exists). 1.0 = pure energy-conserving average,
    // no boost; user-tested live as visibly under-bright for the Advanced
    // trail. 1.3 is the live-confirmed default (1.5 gave the best in-game
    // blur but was too bright on menu screens, no single value suits both);
    // matches gmixGetDefaults()'s kSettingBrightness default in
    // gmix_source.cpp, which is what real sources actually use -- this
    // struct default only matters as the engine's fallback before any
    // persisted ~/.config/gmix/blend_config exists.
    float    shutterStrength = 1.3f;
    float    falloff         = 1.0f;   // recency falloff exponent

    bool usesResamplePath() const { return mode == Mode::Advanced; }

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
    //   N <= 1                  -> {1.0} (passthrough; nothing to blend)
    //   Flat                    -> uniform 1/N
    //   Linear/Cinematic/Heavy  -> generateFromPreset(shape, N)
    //   Advanced                -> uniform filler; resample_blur.comp computes
    //                              its own in-shader recency weighting and
    //                              ignores the weights SSBO entirely.
    //   Raw                     -> tileAndNormalize(rawWeights, N) (ffmpeg tmix tiling)
    std::vector<float> weightsFor(int N) const {
        if (N <= 1) return {1.0f};
        switch (mode) {
        case Mode::Linear:    return generateFromPreset(PresetShape::Linear, N);
        case Mode::Cinematic: return generateFromPreset(PresetShape::Cinematic, N);
        case Mode::Heavy:     return generateFromPreset(PresetShape::Heavy, N);
        case Mode::Raw:
            if (!rawWeights.empty()) return tileAndNormalize(rawWeights, N);
            [[fallthrough]];
        case Mode::Flat:
        case Mode::Advanced:
        default:
            return std::vector<float>(static_cast<size_t>(N), 1.0f / static_cast<float>(N));
        }
    }
};

} // namespace gmix
