// Minimal WAV reader / writer: PCM 16 / 24 / 32-bit and IEEE float 32 / 64-bit in, float 32 or PCM 24 out.
#pragma once

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

bool readWav(const std::filesystem::path& file, WavData& out, std::string& error);
// pcm24 = false writes 32-bit float (lossless for the processor's output).
bool writeWav(const std::filesystem::path& file, const WavData& data, bool pcm24, std::string& error);

} // namespace ap
