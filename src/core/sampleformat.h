// Sample formats of a device buffer (the little-endian ASIO sample types) and conversion to and from float.
// Platform-neutral: the ASIO host maps the driver's types onto these.
#pragma once

#include <cstddef>

namespace ap {

enum class SampleFormat {
    Int16,      // 16-bit
    Int24,      // 24-bit packed in 3 bytes
    Int32,      // 32-bit
    Int32In16,  // 32-bit container, value in the low 16 bits (sign-extended)
    Int32In18,  // ... low 18 bits
    Int32In20,  // ... low 20 bits
    Int32In24,  // ... low 24 bits
    Float32,
    Float64,
    Unsupported // big-endian or DSD: the host refuses to start
};

int bytesPerSample(SampleFormat f);
const char* formatName(SampleFormat f);

// Converts `frames` samples starting at sample `offset` of a device buffer into floats (full scale = 1.0).
// No allocation; real-time safe.
void toFloat(const void* buffer, SampleFormat f, std::size_t offset, float* out, int frames);
// The other way: clamps to full scale, rounds to the nearest step (no dither: the interfaces in use take 24 bits
// or more); non-finite values give 0.
void fromFloat(const float* in, SampleFormat f, void* buffer, std::size_t offset, int frames);
// Writes silence (0) into `frames` samples from `offset`.
void silence(SampleFormat f, void* buffer, std::size_t offset, int frames);

} // namespace ap
