// ─────────────────────────────────────────────────────────────────────────────
// Tiny test macros — no external test framework dependency for v1. Portable,
// kept in sync verbatim with linux-x86_64/tests/test_macros.hpp.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_test_failures = 0;
static int g_test_checks   = 0;
static std::string g_current_test;

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    struct name##_runner {                                                     \
        name##_runner() {                                                      \
            g_current_test = #name;                                            \
            std::printf("[ RUN      ] %s\n", #name);                           \
            name();                                                            \
            std::printf("[       OK ] %s\n", #name);                           \
        }                                                                      \
    } name##_instance;                                                         \
    static void name()

#define CHECK(cond) do {                                                       \
    ++g_test_checks;                                                           \
    if (!(cond)) {                                                             \
        ++g_test_failures;                                                     \
        std::printf("  FAIL  %s:%d  CHECK(%s)\n",                              \
                    __FILE__, __LINE__, #cond);                                \
    }                                                                          \
} while (0)

#define CHECK_CLOSE(a, b, eps) do {                                            \
    ++g_test_checks;                                                           \
    double _da = (a), _db = (b);                                               \
    if (std::fabs(_da - _db) > (eps)) {                                        \
        ++g_test_failures;                                                     \
        std::printf("  FAIL  %s:%d  CHECK_CLOSE(%g ~= %g, eps=%g)\n",          \
                    __FILE__, __LINE__, _da, _db, double(eps));                \
    }                                                                          \
} while (0)

#define CHECK_EQ(a, b) do {                                                    \
    ++g_test_checks;                                                           \
    if (!((a) == (b))) {                                                       \
        ++g_test_failures;                                                     \
        std::printf("  FAIL  %s:%d  CHECK_EQ(%s, %s)\n",                       \
                    __FILE__, __LINE__, #a, #b);                               \
    }                                                                          \
} while (0)

inline std::vector<float> approxEqual_vec(const std::vector<float>& v) { return v; }

template <typename T>
inline bool vec_allclose(const std::vector<T>& a, const std::vector<T>& b, double eps) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])) > eps)
            return false;
    }
    return true;
}

#define CHECK_VEC_CLOSE(a, b, eps) do {                                        \
    ++g_test_checks;                                                           \
    if (!vec_allclose((a), (b), (eps))) {                                      \
        ++g_test_failures;                                                     \
        std::printf("  FAIL  %s:%d  CHECK_VEC_CLOSE size/contents\n",         \
                    __FILE__, __LINE__);                                       \
    }                                                                          \
} while (0)
