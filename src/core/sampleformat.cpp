#include "sampleformat.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ap {

namespace {

// Bits of the value in an integer format (0 for floats).
int valueBits(SampleFormat f) {
    switch (f) {
    case SampleFormat::Int16:
    case SampleFormat::Int32In16:
        return 16;
    case SampleFormat::Int32In18:
        return 18;
    case SampleFormat::Int32In20:
        return 20;
    case SampleFormat::Int24:
    case SampleFormat::Int32In24:
        return 24;
    case SampleFormat::Int32:
        return 32;
    default:
        return 0;
    }
}

// float -> integer with full scale `scale` (2^(bits-1)): clamped, rounded to nearest; NaN -> 0.
inline int32_t quantise(float x, double scale) {
    double v = std::isfinite(x) ? static_cast<double>(x) * scale : 0.0;
    if (v > scale - 1.0) v = scale - 1.0;
    if (v < -scale) v = -scale;
    return static_cast<int32_t>(std::llround(v));
}

} // namespace

int bytesPerSample(SampleFormat f) {
    switch (f) {
    case SampleFormat::Int16:
        return 2;
    case SampleFormat::Int24:
        return 3;
    case SampleFormat::Float64:
        return 8;
    case SampleFormat::Unsupported:
        return 0;
    default:
        return 4;
    }
}

const char* formatName(SampleFormat f) {
    switch (f) {
    case SampleFormat::Int16:
        return "16-bit integer";
    case SampleFormat::Int24:
        return "24-bit integer";
    case SampleFormat::Int32:
        return "32-bit integer";
    case SampleFormat::Int32In16:
        return "16 bits in 32";
    case SampleFormat::Int32In18:
        return "18 bits in 32";
    case SampleFormat::Int32In20:
        return "20 bits in 32";
    case SampleFormat::Int32In24:
        return "24 bits in 32";
    case SampleFormat::Float32:
        return "32-bit float";
    case SampleFormat::Float64:
        return "64-bit float";
    default:
        return "unsupported";
    }
}

void toFloat(const void* buffer, SampleFormat f, std::size_t offset, float* out, int frames) {
    const unsigned char* p = static_cast<const unsigned char*>(buffer) + offset * static_cast<std::size_t>(bytesPerSample(f));
    switch (f) {
    case SampleFormat::Int16:
        for (int i = 0; i < frames; ++i) {
            int16_t v;
            std::memcpy(&v, p + 2 * i, 2);
            out[i] = static_cast<float>(v * (1.0 / 32768.0));
        }
        return;
    case SampleFormat::Int24:
        for (int i = 0; i < frames; ++i) {
            const unsigned char* s = p + 3 * i;
            int32_t v = s[0] | (s[1] << 8) | (s[2] << 16);
            if (v & 0x800000) v -= 0x1000000;
            out[i] = static_cast<float>(v * (1.0 / 8388608.0));
        }
        return;
    case SampleFormat::Int32:
    case SampleFormat::Int32In16:
    case SampleFormat::Int32In18:
    case SampleFormat::Int32In20:
    case SampleFormat::Int32In24: {
        const int bits = valueBits(f);
        const double scale = 1.0 / std::ldexp(1.0, bits - 1);
        const int shift = 32 - bits; // sign-extend the low `bits` bits
        for (int i = 0; i < frames; ++i) {
            int32_t v;
            std::memcpy(&v, p + 4 * i, 4);
            if (shift) v = static_cast<int32_t>(static_cast<uint32_t>(v) << shift) >> shift;
            out[i] = static_cast<float>(v * scale);
        }
        return;
    }
    case SampleFormat::Float32:
        std::memcpy(out, p, sizeof(float) * static_cast<std::size_t>(frames));
        return;
    case SampleFormat::Float64:
        for (int i = 0; i < frames; ++i) {
            double v;
            std::memcpy(&v, p + 8 * i, 8);
            out[i] = static_cast<float>(v);
        }
        return;
    default:
        for (int i = 0; i < frames; ++i) out[i] = 0.0f;
        return;
    }
}

void fromFloat(const float* in, SampleFormat f, void* buffer, std::size_t offset, int frames) {
    unsigned char* p = static_cast<unsigned char*>(buffer) + offset * static_cast<std::size_t>(bytesPerSample(f));
    switch (f) {
    case SampleFormat::Int16:
        for (int i = 0; i < frames; ++i) {
            const int16_t v = static_cast<int16_t>(quantise(in[i], 32768.0));
            std::memcpy(p + 2 * i, &v, 2);
        }
        return;
    case SampleFormat::Int24:
        for (int i = 0; i < frames; ++i) {
            const int32_t v = quantise(in[i], 8388608.0);
            p[3 * i] = static_cast<unsigned char>(v);
            p[3 * i + 1] = static_cast<unsigned char>(v >> 8);
            p[3 * i + 2] = static_cast<unsigned char>(v >> 16);
        }
        return;
    case SampleFormat::Int32:
    case SampleFormat::Int32In16:
    case SampleFormat::Int32In18:
    case SampleFormat::Int32In20:
    case SampleFormat::Int32In24: {
        const double scale = std::ldexp(1.0, valueBits(f) - 1);
        for (int i = 0; i < frames; ++i) {
            const int32_t v = quantise(in[i], scale);
            std::memcpy(p + 4 * i, &v, 4);
        }
        return;
    }
    case SampleFormat::Float32:
        for (int i = 0; i < frames; ++i) {
            const float v = std::isfinite(in[i]) ? in[i] : 0.0f;
            std::memcpy(p + 4 * i, &v, 4);
        }
        return;
    case SampleFormat::Float64:
        for (int i = 0; i < frames; ++i) {
            const double v = std::isfinite(in[i]) ? static_cast<double>(in[i]) : 0.0;
            std::memcpy(p + 8 * i, &v, 8);
        }
        return;
    default:
        return;
    }
}

void silence(SampleFormat f, void* buffer, std::size_t offset, int frames) {
    const int b = bytesPerSample(f);
    if (b > 0)
        std::memset(static_cast<unsigned char*>(buffer) + offset * static_cast<std::size_t>(b), 0,
                    static_cast<std::size_t>(b) * static_cast<std::size_t>(frames));
}

} // namespace ap
