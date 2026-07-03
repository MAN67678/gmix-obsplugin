// ─────────────────────────────────────────────────────────────────────────────
// GMix — blend configuration: velocity-aware ("optical awareness") motion
// blur params, plus output dimensions/pacing.
//
// Used to also carry a preset Mode selector (Flat/Linear/Cinematic/Heavy/
// Advanced/Raw) with several weight-shape curves (weight_generator.cpp) for
// a plain weighted-average blend path. Removed 2026-07-03 when the plugin
// was stripped down to only the Advanced/optical-flow mode -- see
// DEV_NOTES.md for why (in short: it's the only mode anyone was actually
// using live, and the plain-average path added real code weight for zero
// live value).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "../gmix.hpp"   // kMaxBlendFrames
#include <cstdint>

namespace gmix {

struct BlendConfig {
    // "Blur density" in the OBS properties UI: taps per real frame along the
    // estimated motion direction. 4..32 -- higher packs the directional
    // streak denser at proportionally higher GPU cost. Matches
    // BlendEngine::ResampleParams::subSamples.
    uint32_t blurDensity     = 4;
    // "Blur brightness" in the OBS properties UI: a motion-gated exposure
    // boost on the trail (resample_blur.comp's `shutterStrength` -- see that
    // file for the current mechanism, reworked multiple times on
    // 2026-07-03). 1.0 = pure energy-conserving average, no boost;
    // user-tested live as visibly under-bright for the Advanced trail. 1.3
    // is the live-confirmed default (1.5 gave the best in-game blur but was
    // too bright on menu screens, no single value suits both); matches
    // gmixGetDefaults()'s kSettingBrightness default in gmix_source.cpp,
    // which is what real sources actually use -- this struct default only
    // matters before any saved obs_data/scene-collection value is applied.
    float    shutterStrength = 1.3f;

    // Output window size.
    uint32_t outW = 1920;
    uint32_t outH = 1080;

    // Target output framerate. 60 is the only sane value per the design doc
    // (platform ingest constraint), but kept configurable for testing. The
    // number of source frames blended per output frame is derived from the live
    // capture rate (n = round(captureFps / outFps)) in the present loop, so the
    // blur window is always ~1/outFps of real motion -- a 360-degree shutter.
    uint32_t outFps = 60;

    // GPU override (index into enumerated physical devices). -1 = auto.
    int32_t gpuIndex = -1;

    // Max frames the engine will blend in one dispatch (the shader's MAX_FRAMES).
    int maxBlendFrames() const { return kMaxBlendFrames; }
};

} // namespace gmix
