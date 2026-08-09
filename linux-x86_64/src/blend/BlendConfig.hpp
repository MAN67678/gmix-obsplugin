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
    // estimated motion direction. 4..64 -- higher packs the directional
    // streak denser, but the shader (resample_blur.comp) only ever spends
    // taps proportional to actual per-pixel motion (capped at
    // min(blurDensity, ceil(motionPx)+1)), so raising the ceiling only costs
    // more where there's genuinely fast motion to resolve, not on the
    // static majority of the frame. Matches BlendEngine::ResampleParams::subSamples.
    uint32_t blurDensity     = 4;
    // "Blur brightness" in the OBS properties UI: a motion-gated exposure
    // boost on the trail (resample_blur.comp's `shutterStrength` -- see that
    // file for the current mechanism, reworked multiple times on
    // 2026-07-03). 1.0 = pure energy-conserving average, no boost -- an
    // earlier version of the shader visibly under-filled the trail (see
    // etc/DEV_NOTES.md's 2026-07-05 entry), so a >1.0 default (1.3, then
    // briefly tuned higher live) was used to compensate. Now that the
    // per-frame-pair motion estimation actually fills the trail properly,
    // 1.0 is genuinely neutral and looks correct on its own -- confirmed
    // live 2026-07-05. Matches gmixGetDefaults()'s kSettingBrightness
    // default in gmix_source.cpp, which is what real sources actually use --
    // this struct default only matters before any saved obs_data/scene-
    // collection value is applied.
    float    shutterStrength = 1.0f;

    // ── Cursor-path directional blur (second, post-blend pass) ──────────────
    // A streak applied to the blended frame along the cursor's TRUE trajectory
    // through the shutter window (see shaders/cursor_path_blur.comp). Distinct
    // from the optical-flow blur above: driven by the real per-frame cursor
    // positions carried in FrameHeader.cursorX/Y, not inferred from pixels.
    // cursorBlurWidth: the streak's half-thickness in blend-buffer px -- the
    //   cursor's on-screen radius. 0 = feature OFF (pass 2 is a pure
    //   passthrough copy, output identical to the single-pass result). A
    //   manual slider for now; to be driven by the real game cursor size later.
    // cursorBlurStrength: 0..1 mix of the streak over the original.
    float    cursorBlurWidth    = 0.0f;
    float    cursorBlurStrength = 1.0f;
    // Blur mode: 0 = optical-flow only (pass 2 off), 1 = cursor-path only (pass
    // 1 falls back to a plain temporal average so the streak stands alone),
    // 2 = both (optical-flow blend + cursor-path streak). Default 2 preserves
    // the pre-picker behavior (with cursorBlurWidth 0, that's optical-only).
    int      blurMode           = 2;

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

    // ── Bloom/glow post-process for cursor trail ─────────────────────────────
    bool   bloomEnabled   = true;
    float  bloomThreshold = 0.8f;
    float  bloomStrength  = 1.0f;
    float  bloomRadius    = 8.0f;
    float  bloomSigma     = 4.0f;

    // ── Cursor sprite (sharp cursor on top of trail) ──────────────────────────
    int      cursorSpriteStyle       = 0;    // 0=circle, 1=crosshair, 2=dot
    float    cursorSpriteAlpha       = 1.0f; // opacity 0..1
    float    cursorSpriteReleaseTime = 0.3f; // fade-out seconds
    bool     cursorSpriteEnabled     = true; // enable/disable sharp cursor
    float    cursorTrailFade         = 0.0f; // extra fade px beyond blurWidth
    float    cursorTrailFadeInTime   = 0.0f; // fade-in seconds (0 = instant)
};

} // namespace gmix
