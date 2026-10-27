// WAV files: float 32 and PCM 24 written and read back, RF64 (the form over 4 GB, forced here: a real 4 GB file is too
// large for a test; the size rule is checked at its edge), the header's sizes byte for byte, non-finite samples, the
// pad byte of an odd data size, no half file after a failed write, and broken or hostile RF64 sizes (outside review:
// a ds64 size near 2^64 sent the reader round in circles).
#include "check.h"
#include "wavfile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace ap;
namespace fs = std::filesystem;

namespace {

uint32_t u32at(const std::string& b, size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[i]) | (static_cast<unsigned char>(b[i + 1]) << 8) |
                                 (static_cast<unsigned char>(b[i + 2]) << 16) |
                                 (static_cast<uint32_t>(static_cast<unsigned char>(b[i + 3])) << 24));
}
uint64_t u64at(const std::string& b, size_t i) { return u32at(b, i) | (static_cast<uint64_t>(u32at(b, i + 4)) << 32); }

std::string whole(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void put(std::string& b, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) b.push_back(static_cast<char>(v >> (8 * i)));
}

std::string head(const fs::path& file, size_t n) {
    std::ifstream in(file, std::ios::binary);
    std::string s(n, '\0');
    in.read(s.data(), static_cast<std::streamsize>(n));
    return s;
}

WavData tone(int channels, size_t frames) {
    WavData w;
    w.channels = channels;
    w.sampleRate = 48000;
    w.samples.resize(frames * static_cast<size_t>(channels));
    for (size_t i = 0; i < w.samples.size(); ++i) w.samples[i] = static_cast<float>(0.5 * std::sin(0.01 * static_cast<double>(i)));
    return w;
}

void roundTrips(const fs::path& dir) {
    std::printf("-- write and read back: float 32, PCM 24, RF64\n");
    std::string err;
    const WavData w = tone(2, 4801);
    for (const bool pcm24 : {false, true})
        for (const bool rf64 : {false, true}) {
            const fs::path f = dir / (std::string(pcm24 ? "p24" : "f32") + (rf64 ? "-rf64" : "") + ".wav");
            CHECK(writeWav(f, w, pcm24, err, rf64));
            WavData r;
            CHECK(readWav(f, r, err));
            double worst = 0.0;
            for (size_t i = 0; i < w.samples.size() && i < r.samples.size(); ++i)
                worst = std::max(worst, static_cast<double>(std::fabs(w.samples[i] - r.samples[i])));
            const std::string tag = head(f, 4);
            std::printf("   %s%s: %s, %zu samples back, largest difference %.2g\n", pcm24 ? "PCM 24" : "float 32",
                        rf64 ? " RF64" : "", tag.c_str(), r.samples.size(), worst);
            CHECK(tag == (rf64 ? "RF64" : "RIFF"));
            CHECK(r.channels == 2 && r.sampleRate == 48000 && r.samples.size() == w.samples.size());
            CHECK(worst <= (pcm24 ? 1.0 / 8388608.0 : 0.0));
            CHECK(!fs::exists(fs::path(f.string() + ".part")));
        }
}

void oddAndBad(const fs::path& dir) {
    std::printf("-- an odd data size, non-finite samples, a write that fails\n");
    std::string err;
    // 1 channel, 3 PCM 24 samples: 9 bytes of data and a pad byte; the file's size says so
    WavData w = tone(1, 3);
    const fs::path f = dir / "odd.wav";
    CHECK(writeWav(f, w, true, err));
    std::error_code ec;
    CHECK(fs::file_size(f, ec) == 44 + 9 + 1);
    WavData r;
    CHECK(readWav(f, r, err) && r.samples.size() == 3);
    // NaN and infinity are written as 0, and read as 0 from a file that holds them
    w = tone(2, 10);
    w.samples[3] = std::numeric_limits<float>::quiet_NaN();
    w.samples[4] = std::numeric_limits<float>::infinity();
    CHECK(writeWav(f, w, false, err));
    CHECK(readWav(f, r, err) && r.samples[3] == 0.0f && r.samples[4] == 0.0f);
    {
        std::fstream patch(f, std::ios::binary | std::ios::in | std::ios::out); // a NaN put into the file itself
        const float nan = std::numeric_limits<float>::quiet_NaN();
        patch.seekp(44 + 5 * 4);
        patch.write(reinterpret_cast<const char*>(&nan), 4);
    }
    CHECK(readWav(f, r, err) && r.samples[5] == 0.0f);
    // a folder that is not there: false, and nothing left behind
    const fs::path none = dir / "no such folder" / "x.wav";
    CHECK(!writeWav(none, w, false, err));
    CHECK(!fs::exists(none) && !fs::exists(fs::path(none.string() + ".part")));
    std::printf("   pad byte, non-finite samples as 0, a failed write leaves nothing (%s)\n", err.c_str());
}

void headers(const fs::path& dir) {
    std::printf("-- the header's sizes: RIFF, RF64's ds64, the 4 GB rule\n");
    std::string err;
    for (const bool rf64 : {false, true})
        for (const bool pcm24 : {false, true}) {
            const WavData w = tone(1, pcm24 ? 5 : 4); // PCM 24: 15 bytes of data, a pad byte
            const fs::path f = dir / "sizes.wav";
            CHECK(writeWav(f, w, pcm24, err, rf64));
            const std::string b = whole(f);
            const uint64_t dataBytes = w.samples.size() * (pcm24 ? 3u : 4u);
            if (!rf64) {
                CHECK(u32at(b, 4) == b.size() - 8); // RIFF size, the pad byte included
                CHECK(b.compare(36, 4, "data") == 0 && u32at(b, 40) == dataBytes);
            } else {
                CHECK(u32at(b, 4) == 0xFFFFFFFFu && b.compare(12, 4, "ds64") == 0);
                CHECK(u64at(b, 20) == b.size() - 8); // the true RIFF size
                CHECK(u64at(b, 28) == dataBytes);    // the true data size
                CHECK(u64at(b, 36) == w.frames());   // the sample count
                CHECK(b.compare(72, 4, "data") == 0 && u32at(b, 76) == 0xFFFFFFFFu);
            }
        }
    CHECK(!wavNeedsRf64(0xFFFFFFFFull - 37));
    CHECK(wavNeedsRf64(0xFFFFFFFFull - 36));
    CHECK(wavNeedsRf64(5ull << 30));
    std::printf("   sizes as written match the file; RF64 from %llu data bytes on\n", 0xFFFFFFFFull - 36);
}

void brokenRf64(const fs::path& dir) {
    std::printf("-- RF64 with a chunk after the data, without ds64, with a hostile ds64 size\n");
    std::string err;
    const WavData w = tone(2, 100);
    const fs::path f = dir / "rf64.wav";
    CHECK(writeWav(f, w, false, err, true));
    const std::string good = whole(f);
    auto writeBytes = [&](const std::string& b) {
        std::ofstream out(f, std::ios::binary | std::ios::trunc);
        out.write(b.data(), static_cast<std::streamsize>(b.size()));
    };
    WavData r;
    // a chunk after the data: the data's size comes from ds64, not "the rest of the file"
    writeBytes(good + std::string("LIST\x08\0\0\0abcdefgh", 16));
    CHECK(readWav(f, r, err) && r.samples.size() == w.samples.size());
    // no ds64 chunk (the fmt and data chunks only): the data is the rest of the file
    std::string noDs64 = good.substr(0, 12) + good.substr(48);
    writeBytes(noDs64);
    CHECK(readWav(f, r, err) && r.samples.size() == w.samples.size());
    // ds64 sizes larger than the file, up to 2^64 - 1: what is there, at once (no endless loop, no huge allocation)
    for (const uint64_t hostile : {~0ull, ~0ull - 7, 1ull << 63, static_cast<uint64_t>(good.size()) + 1}) {
        std::string b = good;
        for (int i = 0; i < 8; ++i) b[28 + static_cast<size_t>(i)] = static_cast<char>(hostile >> (8 * i));
        writeBytes(b);
        const bool ok = readWav(f, r, err);
        CHECK(ok && r.samples.size() == w.samples.size());
    }
    // a plain RIFF whose data size runs past the end of the file: what is there
    std::string riff;
    riff += "RIFF";
    put(riff, 0xFFFFFFF0u, 4);
    riff += "WAVEfmt ";
    put(riff, 16, 4);
    put(riff, 3, 2);
    put(riff, 2, 2);
    put(riff, 48000, 4);
    put(riff, 48000 * 8, 4);
    put(riff, 8, 2);
    put(riff, 32, 2);
    riff += "data";
    put(riff, 0xFFFFFFF0u, 4);
    riff += good.substr(80, 40 * 8); // 40 frames
    writeBytes(riff);
    CHECK(readWav(f, r, err) && r.samples.size() == 80);
    std::printf("   all read to the end of what is there\n");
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "DJAP_wavfile_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    roundTrips(dir);
    oddAndBad(dir);
    headers(dir);
    brokenRf64(dir);
    fs::remove_all(dir, ec);
    return test::finish("wavfile_test");
}
