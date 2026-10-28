// SPDX-License-Identifier: GPL-3.0-or-later
#include "winvolume.h"

#include "appstate.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <cmath>

namespace ap {

namespace {

const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr int kLookupTicks = 20; // polls between two look-ups of the default device (1 s at the GUI's 20 Hz)

template <class T> void releaseCom(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

} // namespace

WindowsVolume::~WindowsVolume() { close(); }

void WindowsVolume::close() {
    release();
    releaseCom(enumerator_);
}

void WindowsVolume::release() {
    releaseCom(volume_);
    id_.clear();
    name_.clear();
}

// (Re)opens the default playback device when it is another one than before.
bool WindowsVolume::open() {
    if (!enumerator_ && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_))))
        return false;
    IMMDevice* dev = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        release();
        return false;
    }
    LPWSTR id = nullptr;
    bool ok = SUCCEEDED(dev->GetId(&id));
    if (ok && volume_ && id_ == id) { // still the same device
        CoTaskMemFree(id);
        dev->Release();
        return true;
    }
    release();
    if (ok) {
        id_ = id;
        CoTaskMemFree(id);
        ok = SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                     reinterpret_cast<void**>(&volume_)));
    }
    IPropertyStore* props = nullptr;
    if (ok && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name_ = toUtf8(v.pwszVal);
        PropVariantClear(&v);
        props->Release();
    }
    DWORD hw = 0;
    software_ = ok && SUCCEEDED(volume_->QueryHardwareSupport(&hw)) && !(hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME);
    dev->Release();
    if (!ok) release();
    return ok;
}

bool WindowsVolume::poll() {
    if (ticks_++ % kLookupTicks == 0 || !volume_) open();
    float db = 0.0f, scalar = 1.0f;
    BOOL mute = FALSE;
    if (!volume_ || FAILED(volume_->GetMasterVolumeLevel(&db)) || FAILED(volume_->GetMasterVolumeLevelScalar(&scalar)) ||
        FAILED(volume_->GetMute(&mute))) {
        release(); // the device went away: looked up again at the next poll
        gain_ = scalar_ = 1.0;
        muted_ = false;
        return false;
    }
    scalar_ = scalar;
    muted_ = mute != FALSE;
    // the level in dB is what Windows (or the driver) applies; a device whose range goes above 0 dB is not raised
    gain_ = muted_ || scalar <= 0.0f ? 0.0 : std::fmin(1.0, std::pow(10.0, db / 20.0));
    return true;
}

} // namespace ap
