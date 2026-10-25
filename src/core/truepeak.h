// True-peak detection after ITU-R BS.1770-5 Annex 2: polyphase windowed-sinc interpolation.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>
#include <emmintrin.h>

namespace ap {

// Streaming true-peak detector for one channel. The prototype is centred on a tap, so phase 0 is the sample
// itself and phases 1..F-1 lie between samples (handbook 4.2). Each push of sample n reports the largest
// magnitude on the interval [c, c + 1] with c = n - delay(): the two samples and the F-1 points between them.
// configure() allocates; push() and reset() do not.
class TruePeakDetector {
public:
    // factor F (oversampling, >= 1) and taps per phase T (a multiple of 4, >= 4): delay T / 2 samples.
    void configure(int factor, int tapsPerPhase, double kaiserBeta = 7.0);
    int delay() const { return taps_ / 2; }
    int factor() const { return factor_; }
    void reset();

    inline float push(float x) {
        hist_[pos_] = x;
        hist_[pos_ + taps_] = x;
        pos_ = pos_ + 1 == taps_ ? 0 : pos_ + 1;
        // hist_[pos_ .. pos_ + taps_ - 1] now holds samples n - T + 1 .. n, oldest first.
        const float* h = &hist_[pos_];
        const int d = taps_ / 2;
        float peak = std::fabs(h[d - 1]) > std::fabs(h[d]) ? std::fabs(h[d - 1]) : std::fabs(h[d]); // c and c + 1
        for (int p = 1; p < factor_; ++p) {
            const float a = std::fabs(dot(&coeffs_[static_cast<size_t>(p) * taps_], h, taps_));
            if (a > peak) peak = a;
        }
        return peak;
    }

    // Both channels of a pair with the same configuration at once: each coefficient block is loaded once for both
    // channels and four phases share one horizontal sum (the limiter's 8x / 96-tap detector was the largest single
    // cost of a deck). Reports of a and b in pa and pb.
    static inline void pushPair(TruePeakDetector& a, TruePeakDetector& b, float xa, float xb, float& pa, float& pb) {
        const int t = a.taps_, d = t / 2;
        a.hist_[a.pos_] = xa;
        a.hist_[a.pos_ + t] = xa;
        b.hist_[b.pos_] = xb;
        b.hist_[b.pos_ + t] = xb;
        a.pos_ = a.pos_ + 1 == t ? 0 : a.pos_ + 1;
        b.pos_ = b.pos_ + 1 == t ? 0 : b.pos_ + 1;
        const float* ha = &a.hist_[a.pos_];
        const float* hb = &b.hist_[b.pos_];
        const float sa0 = std::fabs(ha[d - 1]) > std::fabs(ha[d]) ? std::fabs(ha[d - 1]) : std::fabs(ha[d]);
        const float sb0 = std::fabs(hb[d - 1]) > std::fabs(hb[d]) ? std::fabs(hb[d - 1]) : std::fabs(hb[d]);
        const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        __m128 bestA = _mm_setzero_ps(), bestB = _mm_setzero_ps();
        for (int p0 = 1; p0 < a.factor_; p0 += 4) {
            const float* c0 = &a.coeffs_[static_cast<size_t>(p0) * t];
            const float *c1 = c0 + t, *c2 = c1 + t, *c3 = c2 + t;
            __m128 a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0, b0 = a0, b1 = a0, b2 = a0, b3 = a0;
            for (int j = 0; j < t; j += 4) {
                const __m128 va = _mm_loadu_ps(ha + j), vb = _mm_loadu_ps(hb + j);
                __m128 k = _mm_loadu_ps(c0 + j);
                a0 = _mm_add_ps(a0, _mm_mul_ps(k, va));
                b0 = _mm_add_ps(b0, _mm_mul_ps(k, vb));
                k = _mm_loadu_ps(c1 + j);
                a1 = _mm_add_ps(a1, _mm_mul_ps(k, va));
                b1 = _mm_add_ps(b1, _mm_mul_ps(k, vb));
                k = _mm_loadu_ps(c2 + j);
                a2 = _mm_add_ps(a2, _mm_mul_ps(k, va));
                b2 = _mm_add_ps(b2, _mm_mul_ps(k, vb));
                k = _mm_loadu_ps(c3 + j);
                a3 = _mm_add_ps(a3, _mm_mul_ps(k, va));
                b3 = _mm_add_ps(b3, _mm_mul_ps(k, vb));
            }
            _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
            _MM_TRANSPOSE4_PS(b0, b1, b2, b3);
            const __m128 sa = _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3)); // lane i = phase p0 + i
            const __m128 sb = _mm_add_ps(_mm_add_ps(b0, b1), _mm_add_ps(b2, b3));
            bestA = _mm_max_ps(bestA, _mm_and_ps(sa, absMask));
            bestB = _mm_max_ps(bestB, _mm_and_ps(sb, absMask));
        }
        const float ma = hmax(bestA), mb = hmax(bestB);
        pa = ma > sa0 ? ma : sa0;
        pb = mb > sb0 ? mb : sb0;
    }
    // The larger report of the two channels.
    static inline float pushPair(TruePeakDetector& a, TruePeakDetector& b, float xa, float xb) {
        float pa, pb;
        pushPair(a, b, xa, xb, pa, pb);
        return pa > pb ? pa : pb;
    }

private:
    static inline float hmax(__m128 v) {
        v = _mm_max_ps(v, _mm_movehl_ps(v, v));
        v = _mm_max_ss(v, _mm_shuffle_ps(v, v, 1));
        return _mm_cvtss_f32(v);
    }
    // Dot product with SSE (baseline x64); n is a multiple of 4.
    static inline float dot(const float* a, const float* b, int n) {
        __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
        int j = 0;
        for (; j + 8 <= n; j += 8) {
            s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + j), _mm_loadu_ps(b + j)));
            s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + j + 4), _mm_loadu_ps(b + j + 4)));
        }
        for (; j + 4 <= n; j += 4) s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + j), _mm_loadu_ps(b + j)));
        s0 = _mm_add_ps(s0, s1);
        s0 = _mm_add_ps(s0, _mm_movehl_ps(s0, s0));
        s0 = _mm_add_ss(s0, _mm_shuffle_ps(s0, s0, 1));
        return _mm_cvtss_f32(s0);
    }

    int factor_ = 1, taps_ = 2, pos_ = 0;
    std::vector<float> coeffs_; // [phase][tap], taps apply to samples n - T + 1 .. n
    std::vector<float> hist_;   // 2 x taps, mirrored for contiguous reads
};

} // namespace ap
