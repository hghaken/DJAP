#include "wavfile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>

namespace ap {

namespace {
uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint64_t u64(const unsigned char* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

void put32(std::vector<unsigned char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void put16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>(v));
    b.push_back(static_cast<unsigned char>(v >> 8));
}
void put64(std::vector<unsigned char>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
} // namespace

bool readWav(const std::filesystem::path& file, WavData& out, std::string& error) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open the file";
        return false;
    }
    const std::streamoff length = in.tellg();
    in.seekg(0);
    std::vector<unsigned char> buf(length > 0 ? static_cast<size_t>(length) : 0);
    if (!buf.empty() && !in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
        error = "cannot read the file";
        return false;
    }
    const bool rf64 = buf.size() >= 12 && std::memcmp(buf.data(), "RF64", 4) == 0;
    if (buf.size() < 12 || (!rf64 && std::memcmp(buf.data(), "RIFF", 4) != 0) || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF / WAVE file";
        return false;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    uint64_t ds64Data = 0; // RF64: the data chunk's size from the ds64 chunk
    bool haveDs64 = false;
    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const unsigned char* ck = buf.data() + pos;
        uint64_t size = u32(ck + 4);
        const size_t body = pos + 8;
        // RF64's data chunk says 0xFFFFFFFF: its size is in the ds64 chunk (without one: the rest of the file)
        if (rf64 && std::memcmp(ck, "data", 4) == 0 && size == 0xFFFFFFFFu) size = haveDs64 ? ds64Data : buf.size() - body;
        // a truncated last chunk, or a size larger than the file (a broken or hostile ds64): what is there - compared
        // without adding, which could overflow and send the position backwards
        if (size > buf.size() - body) size = buf.size() - body;
        if (std::memcmp(ck, "ds64", 4) == 0 && size >= 16) {
            ds64Data = u64(ck + 16);
            haveDs64 = true;
        } else if (std::memcmp(ck, "fmt ", 4) == 0 && size >= 16) {
            format = u16(ck + 8);
            channels = u16(ck + 10);
            rate = static_cast<int>(u32(ck + 12));
            bits = u16(ck + 22);
            if (format == 0xFFFE && size >= 40) format = u16(ck + 8 + 24); // extensible: sub-format
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = buf.data() + body;
            dataSize = static_cast<size_t>(size);
        }
        pos = body + static_cast<size_t>(size) + static_cast<size_t>(size & 1); // always forward: size <= the rest
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
        out.samples[i] = std::isfinite(v) ? v : 0.0f; // a NaN in a file would poison every filter after it
    }
    return true;
}

bool wavNeedsRf64(uint64_t dataBytes) {
    // a WAV's sizes are 32 bits; the RIFF size counts the header after it too (36 bytes, plus a pad byte)
    return dataBytes + 37 > 0xFFFFFFFFull;
}

bool writeWav(const std::filesystem::path& file, const WavData& d, bool pcm24, std::string& error, bool rf64) {
    const int bytes = pcm24 ? 3 : 4;
    const uint64_t dataSize = static_cast<uint64_t>(d.samples.size()) * static_cast<uint64_t>(bytes);
    if (wavNeedsRf64(dataSize)) rf64 = true;
    std::vector<unsigned char> b;
    const char* tag = rf64 ? "RF64" : "RIFF";
    b.insert(b.end(), tag, tag + 4);
    const uint64_t pad = dataSize & 1;
    const uint64_t riffSize = (rf64 ? 4 + 36 + 24 + 8 : 4 + 24 + 8) + dataSize + pad; // WAVE, (ds64), fmt, data
    put32(b, rf64 ? 0xFFFFFFFFu : static_cast<uint32_t>(riffSize));
    b.insert(b.end(), {'W', 'A', 'V', 'E'});
    if (rf64) { // ds64: the true sizes (EBU Tech 3306)
        b.insert(b.end(), {'d', 's', '6', '4'});
        put32(b, 28);
        put64(b, riffSize);
        put64(b, dataSize);
        put64(b, d.channels ? static_cast<uint64_t>(d.samples.size()) / static_cast<uint64_t>(d.channels) : 0);
        put32(b, 0); // no table
    }
    b.insert(b.end(), {'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, pcm24 ? 1 : 3);
    put16(b, static_cast<uint16_t>(d.channels));
    put32(b, static_cast<uint32_t>(d.sampleRate));
    put32(b, static_cast<uint32_t>(d.sampleRate * d.channels * bytes));
    put16(b, static_cast<uint16_t>(d.channels * bytes));
    put16(b, static_cast<uint16_t>(bytes * 8));
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, rf64 ? 0xFFFFFFFFu : static_cast<uint32_t>(dataSize));

    // the samples in pieces (no second copy of a long file in memory), into "<file>.part"; renamed when complete
    std::filesystem::path part = file;
    part += ".part";
    std::error_code ec;
    try {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create the file";
            return false;
        }
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        const size_t piece = 1 << 16;
        for (size_t start = 0; out && start < d.samples.size(); start += piece) {
            b.clear();
            const size_t end = std::min(d.samples.size(), start + piece);
            for (size_t i = start; i < end; ++i) {
                const float v = std::isfinite(d.samples[i]) ? d.samples[i] : 0.0f;
                if (pcm24) {
                    double x = v;
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
            out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        }
        if (pad) out.put('\0');
        out.close();
        if (!out) {
            std::filesystem::remove(part, ec);
            error = "write failed (disk full?)";
            return false;
        }
    } catch (const std::exception& e) { // out of memory for a piece: no half file left behind
        std::filesystem::remove(part, ec);
        error = std::string("write failed: ") + e.what();
        return false;
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        error = "cannot replace the file (open in another program?)";
        return false;
    }
    return true;
}

} // namespace ap
