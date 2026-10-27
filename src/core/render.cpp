#include "render.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace ap {

namespace {
void drain(DeckProcessor& deck, int d, RenderStats& s, double& limSum, double& levSum, const RecordSink& onRecord) {
    MeterRecord recs[64];
    int n;
    while ((n = deck.meters().pop(recs, 64)) > 0) {
        for (int i = 0; i < n; ++i) {
            const MeterRecord& r = recs[i];
            if (onRecord) onRecord(d, r);
            s.in.addRecord(r.energyIn, r.frames);
            s.out.addRecord(r.energyOut, r.frames);
            s.maxTruePeakDb = std::max(s.maxTruePeakDb, linToDb(std::max(r.tpOut[0], r.tpOut[1])));
            s.maxLimiterDb = std::max(s.maxLimiterDb, static_cast<double>(r.limiterDb));
            s.maxCompressorDb = std::max(s.maxCompressorDb, static_cast<double>(r.compressorDb));
            limSum += r.limiterDb;
            levSum += r.levellerDb;
            ++s.records;
        }
    }
}
} // namespace

bool renderOffline(const WavData& in, const RenderDeck decks[2], bool align, int chunkFrames, WavData& out,
                   RenderResult& result, std::string& error, const RecordSink& onRecord) {
    if (in.channels != 2 && in.channels != 4) {
        error = "the input needs 2 channels (deck A) or 4 channels (deck A + deck B)";
        return false;
    }
    if (in.sampleRate < 22050 || in.sampleRate > 192000) {
        error = "unsupported sample rate";
        return false;
    }
    chunkFrames = std::max(1, std::min(chunkFrames, 8192));
    const int deckCount = in.channels / 2;
    const double fs = in.sampleRate;
    std::unique_ptr<DeckProcessor> proc[2];
    int latency = 0;
    for (int d = 0; d < deckCount; ++d) {
        proc[d].reset(new DeckProcessor());
        proc[d]->prepare(fs);
        DeckControl ctl = decks[d].control;
        PresetValues values = decks[d].used ? decks[d].values : defaultValues();
        if (!decks[d].used) ctl.processing = false;
        DeckConfig cfg = makeDeckConfig(values, ctl, fs);
        proc[d]->publish(cfg);
        // take the configuration before the first real block (a 0-frame call applies it)
        proc[d]->process(nullptr, nullptr, 0);
        latency = proc[d]->latency();
        proc[d]->meters().skipAll();
    }
    result.latency = latency;

    const size_t frames = in.frames();
    // the input and then silence for the look-ahead, so the music's last frames come out too; aligned, the output
    // is as long as the input, else the delay longer (outside review: the end was cut off by the delay)
    const size_t total = frames + static_cast<size_t>(latency);
    const size_t outFrames = align ? frames : total;
    out.channels = in.channels;
    out.sampleRate = in.sampleRate;
    out.samples.assign(outFrames * static_cast<size_t>(in.channels), 0.0f);
    std::vector<float> l(static_cast<size_t>(chunkFrames)), r(static_cast<size_t>(chunkFrames));
    double limSum[2] = {0, 0}, levSum[2] = {0, 0};
    double seconds = 0.0;
    for (size_t pos = 0; pos < total; pos += static_cast<size_t>(chunkFrames)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunkFrames), total - pos));
        for (int d = 0; d < deckCount; ++d) {
            for (int i = 0; i < n; ++i) {
                const size_t f = pos + static_cast<size_t>(i);
                const bool inside = f < frames;
                l[static_cast<size_t>(i)] = inside ? in.samples[f * in.channels + 2 * d] : 0.0f;
                r[static_cast<size_t>(i)] = inside ? in.samples[f * in.channels + 2 * d + 1] : 0.0f;
            }
            const auto t0 = std::chrono::steady_clock::now();
            proc[d]->process(l.data(), r.data(), n);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            for (int i = 0; i < n; ++i) {
                const long long f = static_cast<long long>(pos) + i - (align ? latency : 0);
                if (f < 0 || f >= static_cast<long long>(outFrames)) continue;
                out.samples[static_cast<size_t>(f) * in.channels + 2 * d] = l[static_cast<size_t>(i)];
                out.samples[static_cast<size_t>(f) * in.channels + 2 * d + 1] = r[static_cast<size_t>(i)];
            }
            drain(*proc[d], d, result.deck[d], limSum[d], levSum[d], onRecord);
        }
    }
    for (int d = 0; d < deckCount; ++d) {
        RenderStats& s = result.deck[d];
        if (s.records) {
            s.meanLimiterDb = limSum[d] / static_cast<double>(s.records);
            s.meanLevellerDb = levSum[d] / static_cast<double>(s.records);
        }
    }
    result.processSeconds = seconds;
    result.audioSeconds = static_cast<double>(frames) / fs;
    return true;
}

} // namespace ap
