#include "mediafile.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>
#include <vector>
#include <cwctype>

namespace ap {

namespace {

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string hr(const char* what, HRESULT h) {
    char b[96];
    std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, static_cast<unsigned long>(h));
    return b;
}

bool decode(const std::filesystem::path& file, WavData& out, std::string& error) {
    IMFSourceReader* reader = nullptr;
    IMFMediaType* want = nullptr;
    IMFMediaType* got = nullptr;
    bool ok = false;
    HRESULT h = MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader);
    if (FAILED(h)) {
        error = "Windows cannot decode this file (" + hr("MFCreateSourceReaderFromURL", h) + ")";
        return false;
    }
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(audio, TRUE);
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    h = reader->SetCurrentMediaType(audio, nullptr, want);
    if (SUCCEEDED(h)) h = reader->GetCurrentMediaType(audio, &got);
    if (FAILED(h)) {
        error = "no audio stream that decodes to float (" + hr("SetCurrentMediaType", h) + ")";
    } else {
        const UINT32 channels = MFGetAttributeUINT32(got, MF_MT_AUDIO_NUM_CHANNELS, 0);
        const UINT32 rate = MFGetAttributeUINT32(got, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        const UINT32 bits = MFGetAttributeUINT32(got, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
        if (channels == 0 || rate == 0 || bits != 32) {
            error = "unexpected decoded format";
        } else {
            std::vector<float> data;
            for (;;) {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                h = reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample);
                if (FAILED(h)) {
                    error = hr("ReadSample", h);
                    release(sample);
                    break;
                }
                if (sample) {
                    IMFMediaBuffer* buf = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                        BYTE* p = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                            const float* f = reinterpret_cast<const float*>(p);
                            data.insert(data.end(), f, f + len / sizeof(float));
                            buf->Unlock();
                        }
                        release(buf);
                    }
                    release(sample);
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                    ok = true;
                    break;
                }
            }
            if (ok) {
                out.sampleRate = static_cast<int>(rate);
                if (channels == 1) { // mono: both channels, as it is played
                    out.channels = 2;
                    out.samples.resize(data.size() * 2);
                    for (size_t i = 0; i < data.size(); ++i) out.samples[2 * i] = out.samples[2 * i + 1] = data[i];
                } else {
                    out.channels = static_cast<int>(channels);
                    out.samples = std::move(data);
                }
            }
        }
    }
    release(got);
    release(want);
    release(reader);
    return ok;
}

} // namespace

bool readAudioFile(const std::filesystem::path& file, WavData& out, std::string& error) {
    std::wstring ext = file.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (ext == L".wav" || ext == L".wave") return readWav(file, out, error);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        error = "cannot open the file";
        return false;
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
        error = "Media Foundation is not available";
        if (SUCCEEDED(co)) CoUninitialize();
        return false;
    }
    const bool ok = decode(file, out, error);
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

} // namespace ap
