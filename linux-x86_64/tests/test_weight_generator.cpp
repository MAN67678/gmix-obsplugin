// Unit tests for the weight helpers + BlendConfig — pure math, no GPU required.
// Run: ./test_weight_generator
#include "test_macros.hpp"
#include "blend/weight_generator.hpp"
#include "blend/BlendConfig.hpp"

#include <numeric>
#include <stdexcept>

using namespace gmix;

// ─── weight-string parsing ───────────────────────────────────────────────────
TEST_CASE(parse_weight_basic) {
    auto w = parseWeightString("1 1 1 1");
    CHECK_EQ(w.size(), 4u);
    std::vector<float> ref = {1.f, 1.f, 1.f, 1.f};
    CHECK_VEC_CLOSE(w, ref, 1e-9);
}

TEST_CASE(parse_weight_floats) {
    auto w = parseWeightString("0.05 0.1 0.2 0.4 0.2 0.1 0.05");
    CHECK_EQ(w.size(), 7u);
    CHECK_CLOSE(w[3], 0.4f, 1e-9);
}

TEST_CASE(parse_weight_garbage_throws) {
    bool threw = false;
    try { parseWeightString("1 abc 2"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST_CASE(parse_weight_empty_throws) {
    bool threw = false;
    try { parseWeightString("   "); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

// ─── normalization (the tmix scale=auto semantics) ───────────────────────────
TEST_CASE(normalize_sums_to_one) {
    std::vector<float> w = {1.f, 2.f, 3.f, 4.f};
    auto n = normalizeWeights(w);
    double sum = std::accumulate(n.begin(), n.end(), 0.0);
    CHECK_CLOSE(sum, 1.0, 1e-5);
    CHECK_CLOSE(n[1], 0.2f, 1e-6);  // 2/10
}

TEST_CASE(normalize_equal_weights_gives_average) {
    std::vector<float> w = {1.f, 1.f, 1.f, 1.f};
    auto n = normalizeWeights(w);
    std::vector<float> ref = {0.25f, 0.25f, 0.25f, 0.25f};
    CHECK_VEC_CLOSE(n, ref, 1e-9);
}

TEST_CASE(normalize_degenerate_all_zero_falls_back_to_uniform) {
    std::vector<float> w = {0.f, 0.f, 0.f};
    auto n = normalizeWeights(w);
    std::vector<float> ref = {1.f/3.f, 1.f/3.f, 1.f/3.f};
    CHECK_VEC_CLOSE(n, ref, 1e-9);
}

// ─── tiling (ffmpeg tmix: hold the last weight) ──────────────────────────────
TEST_CASE(tile_holds_last_weight) {
    std::vector<float> c = {1.f, 2.f, 3.f, 4.f};
    auto t = tileCurve(c, 8);
    std::vector<float> ref = {1.f, 2.f, 3.f, 4.f, 4.f, 4.f, 4.f, 4.f};
    CHECK_VEC_CLOSE(t, ref, 1e-9);
}

TEST_CASE(tile_and_normalize_sums_to_one) {
    std::vector<float> c = {1.f, 2.f, 3.f, 4.f};
    auto n = tileAndNormalize(c, 10);
    CHECK_EQ(n.size(), 10u);
    double sum = std::accumulate(n.begin(), n.end(), 0.0);
    CHECK_CLOSE(sum, 1.0, 1e-5);
}

// ─── BlendConfig facade ──────────────────────────────────────────────────────
TEST_CASE(config_flat_is_uniform_average) {
    BlendConfig cfg;                 // default Mode::Flat
    auto w = cfg.weightsFor(8);
    CHECK_EQ(w.size(), 8u);
    for (float x : w) CHECK_CLOSE(x, 1.f/8.f, 1e-9);
    double sum = std::accumulate(w.begin(), w.end(), 0.0);
    CHECK_CLOSE(sum, 1.0, 1e-5);
}

TEST_CASE(config_raw_tiles_per_N) {
    BlendConfig cfg;
    cfg.mode = BlendConfig::Mode::Raw;
    cfg.rawWeights = {1.f, 1.f, 1.f, 1.f};
    auto w4  = cfg.weightsFor(4);
    auto w16 = cfg.weightsFor(16);
    CHECK_EQ(w4.size(), 4u);
    CHECK_EQ(w16.size(), 16u);
    for (float x : w16) CHECK_CLOSE(x, 1.f/16.f, 1e-9);   // flat curve, any N → uniform
}

TEST_CASE(config_N1_is_passthrough) {
    BlendConfig cfg;
    auto w = cfg.weightsFor(1);
    CHECK_EQ(w.size(), 1u);
    CHECK_CLOSE(w[0], 1.0f, 1e-9);
}

// ─── shaped presets (Linear/Cinematic/Heavy) ─────────────────────────────────
TEST_CASE(preset_curves_sum_to_one_and_right_length) {
    for (auto shape : {PresetShape::Linear, PresetShape::Cinematic, PresetShape::Heavy}) {
        auto w = generateFromPreset(shape, 10);
        CHECK_EQ(w.size(), 10u);
        double sum = std::accumulate(w.begin(), w.end(), 0.0);
        CHECK_CLOSE(sum, 1.0, 1e-5);
    }
}

TEST_CASE(preset_linear_peaks_at_center) {
    auto w = generateFromPreset(PresetShape::Linear, 9);
    // symmetric triangle: the center sample should be the largest.
    float center = w[4];
    for (size_t i = 0; i < w.size(); ++i) CHECK(center >= w[i]);
}

TEST_CASE(preset_heavy_decays_from_newest) {
    auto w = generateFromPreset(PresetShape::Heavy, 8);
    // one-sided decay: index 0 (newest) should be the largest weight.
    for (size_t i = 1; i < w.size(); ++i) CHECK(w[0] >= w[i]);
}

TEST_CASE(config_advanced_uses_resample_path_and_uniform_filler) {
    BlendConfig cfg;
    cfg.mode = BlendConfig::Mode::Advanced;
    CHECK(cfg.usesResamplePath());
    auto w = cfg.weightsFor(8);
    CHECK_EQ(w.size(), 8u);
    for (float x : w) CHECK_CLOSE(x, 1.f/8.f, 1e-9);   // ignored by the shader; just a valid filler
}

TEST_CASE(config_flat_does_not_use_resample_path) {
    BlendConfig cfg;   // default Mode::Flat
    CHECK(!cfg.usesResamplePath());
}

int main() {
    std::printf("==== weight_generator tests ====\n");
    std::printf("==== %d checks, %d failures ====\n", g_test_checks, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
