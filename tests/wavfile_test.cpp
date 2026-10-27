// WAV files: float 32 and PCM 24 written and read back, RF64 (the form over 4 GB, forced here: a real 4 GB file is too
// large for a test), non-finite samples, the pad byte of an odd data size, and no half file after a failed write.
#include "check.h"
#include "wavfile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace ap;
namespace fs = std::filesystem;

namespace {

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

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "DJAP_wavfile_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    roundTrips(dir);
    oddAndBad(dir);
    fs::remove_all(dir, ec);
    return test::finish("wavfile_test");
}
