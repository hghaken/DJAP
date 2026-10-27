// Minimal WAV reader / writer: PCM 16 / 24 / 32-bit and IEEE float 32 / 64-bit in, float 32 or PCM 24 out. Files of
// more than 4 GB are RF64 (EBU Tech 3306, the 64-bit form of WAV), in and out.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ap {

struct WavData {
    int channels = 0;
    int sampleRate = 0;
    std::vector<float> samples; // interleaved
    size_t frames() const { return channels ? samples.size() / static_cast<size_t>(channels) : 0; }
};

// Samples that are not finite (NaN, infinity) are read as 0.
bool readWav(const std::filesystem::path& file, WavData& out, std::string& error);
// pcm24 = false writes 32-bit float (lossless for the processor's output). RF64 when the data would not fit a WAV's
// 32-bit sizes (about 4 GB: three hours of 4-channel 24-bit audio at 48 kHz), or always with `rf64` (tests). Written
// to "<file>.part" first and renamed when complete: a failed write leaves no half file under the name asked for.
bool writeWav(const std::filesystem::path& file, const WavData& data, bool pcm24, std::string& error, bool rf64 = false);
// The rule writeWav uses: data of this many bytes does not fit a WAV's 32-bit sizes.
bool wavNeedsRf64(uint64_t dataBytes);
// The header writeWav puts before `dataBytes` of samples (`frames` frames): RIFF, or RF64 when asked or when the sizes
// need it (wavNeedsRf64). Separate so a test can check the 4 GB switch without writing 4 GB.
std::vector<unsigned char> wavHeader(uint64_t dataBytes, uint64_t frames, int channels, int sampleRate, bool pcm24,
                                     bool rf64);

} // namespace ap
