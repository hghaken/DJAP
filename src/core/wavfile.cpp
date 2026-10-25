#include "wavfile.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace ap {

namespace {
uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void put32(std::vector<unsigned char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void put16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>(v));
    b.push_back(static_cast<unsigned char>(v >> 8));
}
} // namespace

bool readWav(const std::filesystem::path& file, WavData& out, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open the file";
        return false;
    }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF / WAVE file";
        return false;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const unsigned char* ck = buf.data() + pos;
        size_t size = u32(ck + 4);
        const size_t body = pos + 8;
        if (body + size > buf.size()) size = buf.size() - body; // tolerate a truncated last chunk
        if (std::memcmp(ck, "fmt ", 4) == 0 && size >= 16) {
            format = u16(ck + 8);
            channels = u16(ck + 10);
            rate = static_cast<int>(u32(ck + 12));
            bits = u16(ck + 22);
            if (format == 0xFFFE && size >= 40) format = u16(ck + 8 + 24); // extensible: sub-format
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = buf.data() + body;
            dataSize = size;
        }
        pos = body + size + (size & 1);
    }
    if (!data || channels <= 0 || rate <= 0) {
        error = "no fmt or data chunk";
        return false;
    }
    const int bytes = bits / 8;
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && (bits == 32 || bits == 64)))) {
        error = "unsupported sample format (PCM 16/24/32 or float 32/64 only)";
        return false;
    }
    const size_t count = dataSize / static_cast<size_t>(bytes) / static_cast<size_t>(channels) * channels;
    out.channels = channels;
    out.sampleRate = rate;
    out.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* p = data + i * static_cast<size_t>(bytes);
        float v = 0.0f;
        if (format == 1 && bits == 16)
            v = static_cast<float>(static_cast<int16_t>(u16(p)) / 32768.0);
        else if (format == 1 && bits == 24)
            v = static_cast<float>(
                static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                     (static_cast<uint32_t>(p[2]) << 24)) /
                2147483648.0);
        else if (format == 1 && bits == 32)
            v = static_cast<float>(static_cast<int32_t>(u32(p)) / 2147483648.0);
        else if (bits == 32)
            std::memcpy(&v, p, 4);
        else {
            double d;
            std::memcpy(&d, p, 8);
            v = static_cast<float>(d);
        }
        out.samples[i] = v;
    }
    return true;
}

bool writeWav(const std::filesystem::path& file, const WavData& d, bool pcm24, std::string& error) {
    const int bytes = pcm24 ? 3 : 4;
    const uint32_t dataSize = static_cast<uint32_t>(d.samples.size() * static_cast<size_t>(bytes));
    std::vector<unsigned char> b;
    b.reserve(44 + dataSize);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + dataSize);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, pcm24 ? 1 : 3);
    put16(b, static_cast<uint16_t>(d.channels));
    put32(b, static_cast<uint32_t>(d.sampleRate));
    put32(b, static_cast<uint32_t>(d.sampleRate * d.channels * bytes));
    put16(b, static_cast<uint16_t>(d.channels * bytes));
    put16(b, static_cast<uint16_t>(bytes * 8));
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, dataSize);
    for (float v : d.samples) {
        if (pcm24) {
            double x = std::isfinite(v) ? v : 0.0;
            x = x > 1.0 ? 1.0 : (x < -1.0 ? -1.0 : x);
            long q = std::lround(x * 8388608.0);
            if (q > 8388607) q = 8388607;
            const uint32_t u = static_cast<uint32_t>(q);
            b.push_back(static_cast<unsigned char>(u));
            b.push_back(static_cast<unsigned char>(u >> 8));
            b.push_back(static_cast<unsigned char>(u >> 16));
        } else {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            put32(b, u);
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot create the file";
        return false;
    }
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!out) {
        error = "write failed";
        return false;
    }
    return true;
}

} // namespace ap
