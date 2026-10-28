// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny test harness: CHECK / CHECK_NEAR count failures; the program's exit code is the number of failures.
#pragma once

#include <cmath>
#include <cstdio>

namespace test {
inline int& failures() {
    static int f = 0;
    return f;
}
inline int finish(const char* name) {
    std::printf("%s: %s (%d failure%s)\n", name, failures() ? "FAILED" : "passed", failures(), failures() == 1 ? "" : "s");
    return failures() ? 1 : 0;
}
} // namespace test

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++test::failures();                                                                                        \
        }                                                                                                              \
    } while (0)

#define CHECK_NEAR(value, expected, tol)                                                                               \
    do {                                                                                                               \
        const double v_ = (value), e_ = (expected), t_ = (tol);                                                        \
        if (!(std::fabs(v_ - e_) <= t_)) {                                                                             \
            std::printf("FAIL %s:%d: %s = %.4f, expected %.4f +- %.4f\n", __FILE__, __LINE__, #value, v_, e_, t_);     \
            ++test::failures();                                                                                        \
        }                                                                                                              \
    } while (0)

// value must lie in [lo, hi]
#define CHECK_RANGE(value, lo, hi)                                                                                     \
    do {                                                                                                               \
        const double v_ = (value), l_ = (lo), h_ = (hi);                                                               \
        if (!(v_ >= l_ && v_ <= h_)) {                                                                                 \
            std::printf("FAIL %s:%d: %s = %.4f, expected %.4f .. %.4f\n", __FILE__, __LINE__, #value, v_, l_, h_);     \
            ++test::failures();                                                                                        \
        }                                                                                                              \
    } while (0)
